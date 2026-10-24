// Extended draw distance and LOD switch ranges.
//
// Three guest systems decide how far away geometry stays visible/detailed:
//
//  1. Screen-contribution cull (Sk8::VectorizedCullingJob -> DoFrustumCulling):
//     beyond an engage distance a mesh survives only while
//     size * (engage / dist^2) >= threshold. The threshold lives at
//     [cullObject+6064] and is programmed from config data by sub_8288DC58.
//     Small world meshes (foliage, street furniture, props) hard-pop when the
//     verdict flips; scaling the threshold by 1/k^2 moves the pop distance
//     out by k. The engage distance at +6068 is left untouched.
//
//  2. Per-zone optimesh mesh cull: WorldPresEntityOptimesh rebuilds a
//     {camX, drawDistance, camZ, 1} reference vector at entity+512 every
//     frame (sub_82792900, attribute 0x2E9AAECD1C29F81F); the cull job
//     compares world-mesh pieces against lane 1. Lane 1 is NOT scaled:
//     the vector doubles as the entity's world anchor (the entity parks
//     drawDistance meters above the camera so its own cull distance is a
//     constant), and backdrop meshes anchored to the entity - the fake
//     skyline dome - render at that height, so scaling it visibly raised
//     the skyline off the horizon. Zone pieces still reach x k through
//     the shared contribution-cull threshold in (1).
//
//  3. Character LOD bands: SceneRenderView::GetLODDistancesFromAttrib
//     (sub_827E1AD8) rewrites six squared switch distances every frame,
//     view+23760/23764/23768 for skaters/pedestrians and +23772/23776/23780
//     for vehicles, consumed by UpdateEntityLOD against the squared
//     camera distance. Static world geometry is not routed through these
//     bands (it fails the entity-type gate), hence the separate cull hooks.
//
// The game refreshes (2) and (3) every frame and (1) on config apply, so each
// hook rescales the freshly written values in place and nothing compounds.
// The cull threshold is additionally reapplied once per frame from the
// GetLODDistancesFromAttrib hook (the view's cull object is at view+8) so
// cvar changes take effect without a config reload.

#include "skate3_init.h"
#include "native/skate3_native_guest_read.h"
#include "skate3_demo_path.h"
#include "skate3_native_scene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_DOUBLE(skate3_draw_distance_scale, 2.0, "Skate 3",
                      "Scale the distance at which small world meshes "
                      "(foliage, props, street furniture) stop being drawn. "
                      "1 = original console behavior. Larger values draw more "
                      "of the world and cost proportionally more GPU/CPU.")
    .range(0.25, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_lod_distance_scale, 2.0, "Skate 3",
                      "Scale the distances at which skaters, pedestrians and "
                      "vehicles switch to lower-detail models. 1 = original "
                      "console behavior.")
    .range(0.25, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_draw_distance_stream_probe, 0.0, "Skate 3",
                      "Pre-stream world detail cells this many metres ahead "
                      "of the camera focus. The game's detail collections "
                      "(foliage clusters, street furniture sets) are streamed "
                      "per grid cell only once the focus enters the cell, so "
                      "their content pops in at the cell boundary; probing "
                      "neighbouring cells at this radius loads them before "
                      "they are visible. 0 = original console behavior.")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_draw_distance_debug, false, "Skate 3",
                    "Log draw-distance diagnostics: programmed cull thresholds, "
                    "world-mesh cull reference vectors, and asset-collection "
                    "stream load/unload requests. Used to attribute a specific "
                    "pop-in to the culling vs. streaming system.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr double kScaleEpsilon = 1e-3;

inline uint32_t LoadGuestU32(uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

inline uint64_t LoadGuestU64(uint8_t* base, uint32_t addr) {
  uint64_t raw;
  std::memcpy(&raw, base + addr, 8);
  return __builtin_bswap64(raw);
}

inline void StoreGuestU32(uint8_t* base, uint32_t addr, uint32_t value) {
  const uint32_t raw = __builtin_bswap32(value);
  std::memcpy(base + addr, &raw, 4);
}

inline float LoadGuestF32(uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  raw = __builtin_bswap32(raw);
  float value;
  std::memcpy(&value, &raw, 4);
  return value;
}

inline void StoreGuestF32(uint8_t* base, uint32_t addr, float value) {
  uint32_t raw;
  std::memcpy(&raw, &value, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(base + addr, &raw, 4);
}

inline bool PlausibleGuestAddr(uint32_t addr) { return addr >= 0x10000; }

// Contribution-cull threshold slots we manage. The threshold is programmed
// by the game through more than one path (boot-time defaults as well as the
// config-apply setter), so instead of depending on catching every writer,
// each slot remembers the game's unscaled value and the exact bits we last
// wrote: a current value that differs from our last write is a fresh game
// value and becomes the new base. The scale is always derived from the base,
// never from the current value, so nothing compounds.
struct CullThresholdSlot {
  uint32_t addr = 0;
  uint32_t base_bits = 0;
  uint32_t written_bits = 0;
};

std::mutex g_cull_slots_mutex;
CullThresholdSlot g_cull_slots[16];
size_t g_cull_slot_count = 0;

uint32_t ScaledThresholdBits(uint32_t base_bits) {
  const double scale = REXCVAR_GET(skate3_draw_distance_scale);
  if (std::abs(scale - 1.0) <= kScaleEpsilon || scale <= 0.0) {
    return base_bits;
  }
  float value;
  std::memcpy(&value, &base_bits, 4);
  value = float(double(value) / (scale * scale));
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

void EnsureCullThresholdScaled(uint8_t* base, uint32_t cull_object) {
  if (!PlausibleGuestAddr(cull_object)) {
    return;
  }
  const uint32_t addr = cull_object + 6064;
  uint32_t current;
  std::memcpy(&current, base + addr, 4);
  current = __builtin_bswap32(current);
  std::lock_guard<std::mutex> lock(g_cull_slots_mutex);
  CullThresholdSlot* slot = nullptr;
  for (size_t i = 0; i < g_cull_slot_count; ++i) {
    if (g_cull_slots[i].addr == addr) {
      slot = &g_cull_slots[i];
      break;
    }
  }
  if (slot == nullptr) {
    if (g_cull_slot_count >= sizeof(g_cull_slots) / sizeof(g_cull_slots[0])) {
      return;
    }
    slot = &g_cull_slots[g_cull_slot_count++];
    slot->addr = addr;
    slot->base_bits = current;
    slot->written_bits = current;
  } else if (current != slot->written_bits) {
    slot->base_bits = current;
  }
  const uint32_t scaled = ScaledThresholdBits(slot->base_bits);
  if (scaled != current) {
    const uint32_t raw = __builtin_bswap32(scaled);
    std::memcpy(base + addr, &raw, 4);
  }
  slot->written_bits = scaled;
}

void RecordCullThreshold(uint8_t* base, uint32_t cull_object) {
  if (!PlausibleGuestAddr(cull_object)) {
    return;
  }
  if (REXCVAR_GET(skate3_draw_distance_debug)) {
    REXLOG_INFO(
        "draw_distance: cull params obj=0x{:08X} flags=0x{:08X} "
        "threshold={} engage={} axis=({}, {}, {})",
        cull_object, LoadGuestU32(base, cull_object + 6060),
        LoadGuestF32(base, cull_object + 6064),
        LoadGuestF32(base, cull_object + 6068),
        LoadGuestF32(base, cull_object + 5936),
        LoadGuestF32(base, cull_object + 5940),
        LoadGuestF32(base, cull_object + 5944));
  }
  EnsureCullThresholdScaled(base, cull_object);
}

struct ProbeRecord {
  uint64_t id;
  float dist;
  bool vanilla;
  uint8_t blob[16];
};

// A probe-discovered record may only enter the stream set if its embedded
// collection pointer is live: the cell member tables can reference
// collections that are not registered (mid-load, or cells outside the built
// world), and the differ dereferences the pointer for the request's stream
// type; a junk pointer there indexes the per-type request queues with a
// wild value. Live means: plausible address, the collection's own id at +64
// round-trips, and the stream type at +112 is a sane small index.
bool ProbeRecordIsLive(uint8_t* base, uint64_t id, const uint8_t* blob) {
  uint32_t ptr_be;
  std::memcpy(&ptr_be, blob + 8, 4);
  const uint32_t ptr = __builtin_bswap32(ptr_be);
  if (!PlausibleGuestAddr(ptr)) {
    return false;
  }
  uint64_t live_id_raw;
  if (!skate3::native_scene::GuestTryCopy(&live_id_raw, base + ptr + 64, 8) ||
      __builtin_bswap64(live_id_raw) != id) {
    return false;
  }
  uint32_t type_raw;
  if (!skate3::native_scene::GuestTryCopy(&type_raw, base + ptr + 112, 4) ||
      __builtin_bswap32(type_raw) > 7) {
    return false;
  }
  return true;
}

// Cached probe-fan results per probed focus (slot 0 = world stream, slot 1 =
// detail stream). Valid while the focus stays within half a grid cell of the
// cached position; cell membership cannot change inside that window, so the
// expensive gather fan only re-runs after real movement.
struct ProbeCache {
  uint32_t focus = 0;
  float x = 0.0f;
  float z = 0.0f;
  double radius = 0.0;
  size_t count = 0;
  ProbeRecord records[160];
};
ProbeCache g_probe_cache[2];

// F6 or gamepad LB+B (while skate3_draw_distance_debug is on): drop a
// numbered marker line into the log so a visual event ("the trees just
// popped in") can be lined up with the surrounding stream/cull entries.
// Edge-detected on the combined state; polled from the per-frame LOD hook
// below.
void MaybePollDebugMarkerKey() {
  if (!REXCVAR_GET(skate3_draw_distance_debug)) {
    return;
  }
  static bool was_down = false;
  static int marker = 0;
  bool down = false;
#if defined(_WIN32)
  down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
#endif
  if (!down) {
    if (rex::input::InputSystem* input = skate3::demo_path::GetUiInputSystem()) {
      constexpr uint16_t kChord = rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER |
                                  rex::input::X_INPUT_GAMEPAD_B;
      rex::input::X_INPUT_GAMEPAD pad;
      down = input->GetUiGamepadState(&pad) && (pad.buttons & kChord) == kChord;
    }
  }
  if (down && !was_down) {
    ++marker;
    REXLOG_WARN(
        "draw_distance: ======== USER MARKER {} (F6 / LB+B): pop-in observed "
        "========",
        marker);
  }
  was_down = down;
}

}  // namespace

// Sk8 cull-parameter setter: [cullObject+6060] = flags, [cullObject+6064] =
// contribution-cull threshold (float). Record the game's value and apply the
// configured scale.
extern "C" REX_FUNC(sub_8288DC58) {
  const uint32_t cull_object = ctx.r3.u32;
  __imp__sub_8288DC58(ctx, base);
  RecordCullThreshold(base, cull_object);
}

// WorldPresEntityOptimesh per-frame update: rebuilds the size-cull reference
// vector at entity+512 from a global camera vector, with lane 1 replaced by
// the zone attribute. Lane 1 doubles as the entity's world-space height
// (the entity is parked that many meters above the camera so its cull
// distance stays constant), and the fake-skyline backdrop renders anchored
// to it - scaling lane 1 visibly raised the skyline off the horizon by
// (k - 1) x drawDistance meters. Zone reach therefore rides the shared
// contribution-cull threshold scaling only; lane 1 stays untouched.
extern "C" REX_FUNC(sub_82792900) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82792900(ctx, base);
  if (!PlausibleGuestAddr(entity)) {
    return;
  }
  if (REXCVAR_GET(skate3_draw_distance_debug)) {
    static std::atomic<int> log_budget{24};
    if (log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO("draw_distance: optimesh ref entity=0x{:08X} v=({}, {}, {})",
                  entity, LoadGuestF32(base, entity + 512),
                  LoadGuestF32(base, entity + 516),
                  LoadGuestF32(base, entity + 520));
    }
  }
  // Drone camera engaged: recenter the size-cull reference on the flown
  // position (lanes 0/2 are the camera X/Z the cull job measures distance
  // from) so world pieces near the drone don't get distance-culled against
  // the far-away gameplay camera.
  float drone[3];
  if (skate3::native_scene::FreecamGuestPose(drone)) {
    StoreGuestF32(base, entity + 512, drone[0]);
    StoreGuestF32(base, entity + 520, drone[2]);
  }
}

// Asset-stream request logging (diagnostics only): a pop-in that coincides
// with a load request here is world-collection streaming (ProxyWorld swap),
// which is a separate system from the distance culls above.
extern "C" REX_FUNC(sub_8247AA88) {
  if (REXCVAR_GET(skate3_draw_distance_debug)) {
    REXLOG_INFO("draw_distance: stream LOAD id=0x{:016X} type={} prio={}",
                ctx.r5.u64, ctx.r6.u32, float(ctx.f1.f64));
  }
  __imp__sub_8247AA88(ctx, base);
}

extern "C" REX_FUNC(sub_8247AC48) {
  if (REXCVAR_GET(skate3_draw_distance_debug)) {
    REXLOG_INFO("draw_distance: stream UNLOAD id=0x{:016X} type={}",
                ctx.r5.u64, ctx.r6.u32);
  }
  __imp__sub_8247AC48(ctx, base);
}

// tStreamFocus entry gather (candidate collections for the load/unload
// differ). World detail collections (stream type 2) are grid-cell quantized
// by an XZ position hash: vanilla gathers only the cell containing the focus
// point, so a cell's content (foliage clusters, street detail) pops exactly
// when the player reaches its boundary. After the vanilla gather, re-run it
// for eight displaced probe positions (the 3x3 cell neighbourhood at the
// probe radius) and union the results by asset id, so neighbouring cells are
// resident before they are visible; the differ unloads them again once every
// probe has left. The union is written back distance-sorted (record 0 must
// stay the nearest; FindCollections compares it against the focus's current
// id) and clamped to the 64-record capacity shared by the entry buffer and
// the focus descriptor array. The buffer's u64 header slot at +0 belongs to
// the caller and is left untouched. The caller holds the focus critical
// section for the whole call, so the temporary position writes cannot race
// MoveStreamFocus.
extern "C" REX_FUNC(sub_8247B3A0) {
  const uint32_t sys = ctx.r3.u32;
  const uint32_t focus = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  __imp__sub_8247B3A0(ctx, base);
  const double probe = REXCVAR_GET(skate3_draw_distance_stream_probe);
  if (probe < 1.0 || !PlausibleGuestAddr(focus) || !PlausibleGuestAddr(out)) {
    return;
  }
  const uint32_t focus_type = LoadGuestU32(base, focus + 32);
  const uint32_t list_mode = LoadGuestU32(base, focus + 20) >> 24;
  const uint32_t list_count = LoadGuestU32(base, focus + 40);
  const bool debug = REXCVAR_GET(skate3_draw_distance_debug);
  // Grid cell size of the first collection group this focus gathers from;
  // the probe pattern must step in cell multiples or it skips over the
  // immediate neighbour cells entirely.
  uint32_t cell_size = 0;
  const uint32_t group_begin = LoadGuestU32(base, sys + focus_type * 16 + 36);
  const uint32_t group_end = LoadGuestU32(base, sys + focus_type * 16 + 40);
  if (PlausibleGuestAddr(group_begin) && group_end > group_begin) {
    const uint32_t group = LoadGuestU32(base, group_begin);
    if (PlausibleGuestAddr(group)) {
      cell_size = LoadGuestU32(base, group + 600);
    }
  }
  if (debug) {
    REXLOG_INFO(
        "draw_distance: stream gather focus=0x{:08X} type={} list_mode={} "
        "list_count={} entries={} cell_size={}",
        focus, focus_type, list_mode, list_count,
        LoadGuestU32(base, out + 1032), cell_size);
  }
  // Probe the world stream (type 0) and the detail stream (type 2). Type 1
  // has extra bookkeeping in FindCollections and stays vanilla. The
  // explicit-list path (mode byte set AND a non-empty list, mirroring the
  // game's own condition) is position-independent; nothing to probe. A
  // zero cell size means the group table is not populated yet (boot).
  if (cell_size == 0) {
    // Group table not populated (boot / level transition); drop the caches
    // so nothing from a previous collection registry survives the rebuild.
    g_probe_cache[0] = {};
    g_probe_cache[1] = {};
    return;
  }
  if ((focus_type != 0 && focus_type != 2) ||
      (list_mode != 0 && list_count != 0) || cell_size > 4096) {
    return;
  }
  // Loading screens and the frontend stream one world set out and another
  // in, then destroy the departing collection objects. Any probe record
  // kept in the gather output here stays loaded past that teardown and its
  // content leaks into the arriving map (the differ cannot unload a record
  // whose collection object is already gone - the ProcessEntries sanitizer
  // below drops such descriptors without an unload). Leave the output
  // untouched so probe cells unload through the same differ passes as the
  // departing vanilla set while their collections are still live, and drop
  // the caches: their neighbourhood is the departing map's. The in-game
  // pause menu keeps the world resident and reports false; it merges the
  // cache below instead.
  if (skate3::native_scene::LoadingOrFrontendActive()) {
    if (debug && (g_probe_cache[0].count > 0 || g_probe_cache[1].count > 0)) {
      REXLOG_INFO(
          "draw_distance: stream probe caches dropped (loading/frontend)");
    }
    g_probe_cache[0] = {};
    g_probe_cache[1] = {};
    return;
  }

  ProbeRecord records[192];
  size_t record_count = 0;
  bool is_vanilla_pass = true;
  const auto absorb = [&]() {
    const uint32_t count =
        std::min<uint32_t>(LoadGuestU32(base, out + 1032), 64);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t rec = out + 8 + i * 16;
      const uint64_t id = LoadGuestU64(base, rec);
      const float dist = LoadGuestF32(base, rec + 12);
      size_t j = 0;
      while (j < record_count && records[j].id != id) {
        ++j;
      }
      if (j < record_count) {
        records[j].vanilla = records[j].vanilla || is_vanilla_pass;
        if (dist < records[j].dist) {
          records[j].dist = dist;
          std::memcpy(records[j].blob, base + rec, 16);
        }
        continue;
      }
      if (record_count < sizeof(records) / sizeof(records[0])) {
        records[record_count].id = id;
        records[record_count].dist = dist;
        records[record_count].vanilla = is_vanilla_pass;
        std::memcpy(records[record_count].blob, base + rec, 16);
        if (!is_vanilla_pass &&
            !ProbeRecordIsLive(base, id, records[record_count].blob)) {
          continue;
        }
        ++record_count;
      }
    }
  };
  absorb();
  is_vanilla_pass = false;
  const size_t vanilla_count = record_count;

  const float fx = LoadGuestF32(base, focus + 0);
  const float fz = LoadGuestF32(base, focus + 8);
  ProbeCache& cache = g_probe_cache[focus_type == 2 ? 1 : 0];
  // A focus more than two cells from where the fan last ran was teleported
  // (map switch, fast travel): the focus object survives the transition, but
  // its cached neighbourhood belongs to the departed position, and merging
  // it below would make the arrival load wait on the old area's collections.
  // Gameplay movement cannot cover two cells between gather cycles, so a
  // jump this large is never ordinary mid-stream drift.
  if (cache.focus == focus && cache.count > 0 &&
      (std::abs(fx - cache.x) > float(cell_size) * 2.0f ||
       std::abs(fz - cache.z) > float(cell_size) * 2.0f)) {
    if (debug) {
      REXLOG_INFO(
          "draw_distance: stream probe type={} focus teleported "
          "({}, {}) -> ({}, {}); cache dropped",
          focus_type, cache.x, cache.z, fx, fz);
    }
    cache = {};
  }
  const bool cache_usable = cache.focus == focus && cache.radius == probe;
  const bool cache_valid = cache_usable &&
                           std::abs(fx - cache.x) < float(cell_size) * 0.5f &&
                           std::abs(fz - cache.z) < float(cell_size) * 0.5f;
  // Never fan while the focus's current descriptor set is still streaming:
  // at map load the game's loading flow waits on its stream set, and a
  // probe burst there multiplies the load time for content that is not
  // visible yet. The focus tracks loaded-vs-total descriptor counts from
  // its per-cycle rescan; once the vanilla set is resident the fan runs
  // and the neighbourhood trickles in during play. The counts alone are
  // not a sufficient gate: right after a teleport they still describe the
  // previous position's fully-loaded set, so the fan additionally requires
  // the gameplay presence context (0 = frontend/pause/loading) - loading
  // flows always stream the pure vanilla set. A stale cache (moved more
  // than half a cell while additions are still streaming, or paused with
  // the context out of gameplay) keeps being merged rather than refanned,
  // so in-flight probe records are not dropped and re-requested by the
  // differ.
  const uint32_t desc_total = LoadGuestU32(base, focus + 336);
  const uint32_t desc_loaded = LoadGuestU32(base, focus + 340);
  const bool set_resident = desc_total > 0 && desc_loaded >= desc_total;
  const bool in_gameplay =
      rex::kernel::guest_presence::GameplayContextValue() == 1;
  // An empty vanilla gather is the game clearing this focus (teardown or a
  // transient); never inject on top of it - the differ is about to unload
  // everything the focus held.
  if (vanilla_count > 0 &&
      (cache_valid || (cache_usable && cache.count > 0 &&
                       (!set_resident || !in_gameplay)))) {
    // The neighbourhood cannot have changed (cell membership is quantized
    // to the grid): merge the cached probe results instead of re-running
    // the gather fan. Cached distances are slightly stale; they are only
    // load priorities. Each cached record embeds a collection object
    // pointer, and a level transition rebuilds the collection registry
    // while the focus object (and often its position) survives; so every
    // cached pointer must round-trip through the live object before it is
    // reinjected: the collection stores its own asset id at +64, and a
    // stale pointer will not reproduce the cached 64-bit id.
    for (size_t c = 0; c < cache.count && record_count < 192; ++c) {
      const ProbeRecord& cached = cache.records[c];
      if (!ProbeRecordIsLive(base, cached.id, cached.blob)) {
        continue;
      }
      size_t j = 0;
      while (j < record_count && records[j].id != cached.id) {
        ++j;
      }
      if (j == record_count) {
        records[record_count] = cached;
        records[record_count].vanilla = false;
        ++record_count;
      }
    }
  } else if (vanilla_count == 0 || !set_resident || !in_gameplay) {
    // Initial load, a fresh focus, an emptied focus, or outside gameplay:
    // let the vanilla set finish streaming first; the fan starts on a
    // later cycle.
  } else {
    // Full probe fan: every grid cell within the probe radius, stepped in
    // cell multiples. The 7x7 cap bounds the fan at 48 gather calls; the
    // fan only re-runs after half a cell of movement, so the cost is a
    // once-per-~50m spike, not a per-frame load.
    uint8_t saved_pos[16];
    std::memcpy(saved_pos, base + focus, 16);
    const int k = std::min(
        3, int(std::ceil(probe / double(cell_size) - 1e-6)));
    for (int dx = -k; dx <= k; ++dx) {
      for (int dz = -k; dz <= k; ++dz) {
        if (dx == 0 && dz == 0) {
          continue;
        }
        StoreGuestF32(base, focus + 0, fx + float(cell_size) * float(dx));
        StoreGuestF32(base, focus + 8, fz + float(cell_size) * float(dz));
        ctx.r3.u64 = sys;
        ctx.r4.u64 = focus;
        ctx.r5.u64 = out;
        __imp__sub_8247B3A0(ctx, base);
        absorb();
      }
    }
    std::memcpy(base + focus, saved_pos, 16);
    // Refresh the cache with everything the probes found beyond the
    // vanilla set.
    cache.focus = focus;
    cache.radius = probe;
    cache.x = fx;
    cache.z = fz;
    cache.count = 0;
    for (size_t i = 0;
         i < record_count && cache.count < sizeof(cache.records) /
                                               sizeof(cache.records[0]);
         ++i) {
      if (!records[i].vanilla) {
        cache.records[cache.count++] = records[i];
      }
    }
  }

  if (record_count == vanilla_count) {
    // Nothing beyond the vanilla set this cycle (the fan held off, or it
    // found no new cells): leave the game's own gather output untouched.
    return;
  }
  std::sort(records, records + record_count,
            [](const ProbeRecord& a, const ProbeRecord& b) {
              return a.dist < b.dist;
            });
  // Cap the union. The entry buffer, the focus descriptor array, and
  // ProcessEntries' staging all hold at most 64 records; that limit is
  // hard. Within it, a probe record is admitted only when its distance is
  // inside the probe radius (plus a cell of margin for the coarse
  // quantization): that is exactly the content that would otherwise pop
  // into view at the cell boundary. Anything farther is the neighbour
  // cells' long tail and streaming it is cost without visible benefit.
  // Vanilla records are never dropped, or the differ would unload world
  // content the game intends to keep. Records are distance-sorted, so a
  // single ascending pass keeps the nearest qualifying additions.
  // Type 0 swaps a cheap ProxyWorld stand-in for the full-detail chunk the
  // moment a collection loads, so every admitted world chunk is rendered at
  // full detail from then on; admit only within the probe radius itself.
  // Type 2 detail cells are small payloads; give them a cell of margin for
  // the grid quantization.
  const double admit_radius =
      focus_type == 2 ? probe + double(cell_size) : probe;
  const float admit_dist_sq = float(admit_radius * admit_radius);
  size_t kept = 0;
  size_t dropped = 0;
  size_t vanilla_seen = 0;
  for (size_t i = 0; i < record_count; ++i) {
    bool admit;
    if (records[i].vanilla) {
      admit = true;
      ++vanilla_seen;
    } else {
      // Reserve room for every vanilla record still ahead in the sorted
      // order; they are admitted unconditionally and the 64 cap is hard.
      const size_t vanilla_ahead = vanilla_count - vanilla_seen;
      admit = kept + vanilla_ahead < 64 && records[i].dist <= admit_dist_sq;
    }
    if (!admit) {
      ++dropped;
      continue;
    }
    if (kept != i) {
      records[kept] = records[i];
    }
    ++kept;
  }
  record_count = kept;
  for (size_t i = 0; i < record_count; ++i) {
    std::memcpy(base + out + 8 + i * 16, records[i].blob, 16);
  }
  StoreGuestU32(base, out + 1032, uint32_t(record_count));
  if (debug) {
    REXLOG_INFO("draw_distance: stream probe type={} cells {} -> {} dropped={}",
                focus_type, vanilla_count, record_count, dropped);
  }
}

// tStreamFocus::ProcessEntries: before the old/new merge-diff walks the
// focus's previous descriptor set, drop any old record whose collection
// pointer no longer resolves. A level transition destroys collection objects
// that can stay referenced by descriptors staged before it (the probe union
// holds more cells than the game's own set, including cells whose
// collections are torn down), and the differ dereferences each old-only
// record's pointer for the unload request's stream type; a dead pointer
// there indexes the per-type request queues with a wild value. Runs under
// the focus critical section held by the caller.
extern "C" REX_FUNC(sub_8247DCF0) {
  const uint32_t focus = ctx.r3.u32;
  if (PlausibleGuestAddr(focus)) {
    const uint32_t count = LoadGuestU32(base, focus + 1400);
    if (count <= 64) {
      uint32_t kept = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint8_t blob[16];
        std::memcpy(blob, base + focus + 376 + i * 16, 16);
        uint64_t id_raw;
        std::memcpy(&id_raw, blob, 8);
        if (!ProbeRecordIsLive(base, __builtin_bswap64(id_raw), blob)) {
          continue;
        }
        if (kept != i) {
          std::memcpy(base + focus + 376 + kept * 16, blob, 16);
        }
        ++kept;
      }
      if (kept != count) {
        StoreGuestU32(base, focus + 1400, kept);
        if (REXCVAR_GET(skate3_draw_distance_debug)) {
          REXLOG_WARN(
              "draw_distance: dropped {} dead stream descriptors "
              "(focus=0x{:08X})",
              count - kept, focus);
        }
      }
    }
  }
  __imp__sub_8247DCF0(ctx, base);
}

// Load/unload COMPLETION (the moment the streamed collection is swapped in
// or out of the world, including the ProxyWorld high/proxy exchange): this,
// not the request above, is when a streaming pop becomes visible.
extern "C" REX_FUNC(sub_8247BB50) {
  if (REXCVAR_GET(skate3_draw_distance_debug)) {
    REXLOG_WARN("draw_distance: stream LOAD-COMPLETE id=0x{:016X}",
                ctx.r4.u64);
  }
  __imp__sub_8247BB50(ctx, base);
}

extern "C" REX_FUNC(sub_8247BD28) {
  if (REXCVAR_GET(skate3_draw_distance_debug)) {
    REXLOG_WARN("draw_distance: stream UNLOAD-COMPLETE id=0x{:016X}",
                ctx.r4.u64);
  }
  __imp__sub_8247BD28(ctx, base);
}

// SceneRenderView::GetLODDistancesFromAttrib: rewrites the six squared LOD
// switch distances on the view every frame. Scale them by k^2 (squared
// domain), and take the per-frame opportunity to reapply the cull threshold
// on this view's cull object.
extern "C" REX_FUNC(sub_827E1AD8) {
  const uint32_t view = ctx.r3.u32;
  __imp__sub_827E1AD8(ctx, base);
  MaybePollDebugMarkerKey();
  if (!PlausibleGuestAddr(view)) {
    return;
  }
  const double lod_scale = REXCVAR_GET(skate3_lod_distance_scale);
  if (std::abs(lod_scale - 1.0) > kScaleEpsilon) {
    const double squared = lod_scale * lod_scale;
    for (uint32_t offset = 23760; offset <= 23780; offset += 4) {
      const uint32_t addr = view + offset;
      StoreGuestF32(base, addr,
                    float(double(LoadGuestF32(base, addr)) * squared));
    }
  }
  EnsureCullThresholdScaled(base, LoadGuestU32(base, view + 8));
}
