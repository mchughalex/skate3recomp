#include "skate3_native_render.h"

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
#include "skate3_native_scene.h"

#include "skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_BOOL(skate3_native_render, true, "Skate 3",
                    "Enable the Skate 3 data-driven native renderer hook layer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(skate3_native_render_log_interval, 0, "Skate 3",
                     "Frames between native-render hook liveness log lines (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_log);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull_guest);
REXCVAR_DEFINE_DOUBLE(skate3_guest_fps_cap, 0.0, "Skate 3",
                      "Pace the guest render loop to this frame rate (0 = uncapped). The "
                      "guest produces frames at irregular 2-9 ms intervals; the display "
                      "(especially with G-Sync/VRR, which follows present times directly) "
                      "turns that variance into visible irregular judder that no content "
                      "smoothing can fix. An even cap a few fps below the display refresh "
                      "(e.g. 140 on a 144 Hz panel) is the standard VRR recipe: every "
                      "frame arrives on a steady beat. Precise pacing: coarse sleep to "
                      "~1.5 ms before the target, then spin.")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_guest_fps_cap_auto, true, "Skate 3",
                    "Derive the guest frame cap from the display the window is on: "
                    "cap a safety margin below the refresh rate (4 fps or 5%, "
                    "whichever is larger; see rex::ui::Window::AutoFrameCapHz), "
                    "the VRR recipe above, without hand-tuning per monitor. Above "
                    "the display refresh the extra frames cannot be shown anyway; "
                    "refreshes beat-sample the frame stream and steady motion "
                    "judders (measured: a mathematically perfect synthetic pan "
                    "judders at 330 fps on a 144 Hz panel and is smooth capped "
                    "below it). The margin must also absorb swap-to-present "
                    "jitter: presents run with tearing allowed, so a present "
                    "landing inside the panel's minimum refresh period tears even "
                    "under VRR. Overrides skate3_guest_fps_cap while the display "
                    "refresh is known.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_render {
namespace {

// One per-mesh submission. kind 0 = RenderMesh (dynamic entities: a = the
// MeshContext, b = VertexProgramState, c = dynitem index+1). kind 1 =
// SceneRenderView sort-list entry (a = MeshContext, b = list offset from the
// view, c = view). kind 2 = world-path capture (skinned / model-space prop:
// a = MeshContext, b = submitting view, c = dynitem index+1). kind 3 =
// quad-list DrawVertices capture (a = synthetic key, c = dynitem index+1).
using RenderMeshRecord = skate3::native_scene::SubmitRecord;


std::mutex g_mutex;
std::vector<RenderMeshRecord> g_current_frame;
uint64_t g_frame_index = 0;
std::atomic<bool> g_announced{false};

bool Enabled() { return REXCVAR_GET(skate3_native_render); }


void OnRenderMesh(uint8_t* base, uint32_t mesh_context, uint32_t vertex_program_state,
                  bool drew_inside) {
  const uint32_t dyn = skate3::native_scene::CaptureDynamicState(
      base, mesh_context, /*world_path=*/false, drew_inside);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({0, mesh_context, vertex_program_state, dyn});
}


// SceneRenderView draw-list renderer sub_827FAF50(view, sort_vec, first, count):
// sort_vec points at an eastl vector whose [0] is the entry array; entries are
// 8 bytes {u32 sort_key, MeshContext*}.
//
// Skinned entries and rigid MODEL-SPACE props (vending machines and other
// movables that never reach RenderMesh) are captured HERE, before the
// dispatcher draws the list. The captures are transform-pending; the
// post-draw fixup attaches the palette / world matrix at whichever draw
// eventually consumes the mesh's buffers.
void OnSceneDrawList(uint8_t* base, uint32_t view, uint32_t sort_vec, uint32_t first,
                     uint32_t count) {
  if (count == 0 || count > 100000) {
    return;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return;
  }
  // b = which of the view's sort lists this came from (sort_vec - view), so
  // the scene builder can select the primary opaque list (+20160).
  const uint32_t list_offset = sort_vec - view;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = entries + (first + i) * 8;
    const uint32_t mesh_context = REX_LOAD_U32(entry + 4);
    if (mesh_context == 0) {
      continue;
    }
    g_current_frame.push_back({1, mesh_context, list_offset, view});
    const uint32_t dyn =
        skate3::native_scene::CaptureDynamicState(base, mesh_context, /*world_path=*/true);
    if (dyn != 0) {
      // b = the submitting view: shadow-cascade views submit their own
      // contexts for the same NPCs; rendering those creates ghost
      // duplicates (torso-less: their deferred skin passes never run).
      g_current_frame.push_back({2, mesh_context, view, dyn});
    }
  }
}

// ---- Guest-side occlusion dispatch filter ---------------------------------
// The native renderer suppresses the guest's emulated draws, so for world
// statics the sorted-list dispatch below (material setup, command-packet
// building) produces nothing anyone consumes - yet it is the guest render
// thread's dominant per-item cost. For MeshContexts the render-side
// occlusion cull proved hidden last frame (published by RenderScene), the
// hook compacts the entry segment before invoking the guest dispatcher and
// restores it afterwards. Capture above has already recorded every entry,
// so scene.items, the shadow caster caches, and all state capture stay
// complete; the only guest work skipped is packet-building for draws that
// were both suppressed and occlusion-culled anyway. Guest render thread
// only.
struct OcclDispatchFilter {
  uint64_t stamp = ~0ull;          // g_frame_index of the ctx snapshot
  std::vector<uint32_t> ctxs;      // sorted culled-ctx snapshot
  std::vector<uint8_t> saved;      // original segment bytes for restore
  uint32_t saved_addr = 0;
  uint32_t saved_bytes = 0;
  bool active = false;             // a filtered dispatch is in flight
};
OcclDispatchFilter g_occl_filter;

// Compacts entries[first..first+count) in place, dropping culled ctxs, and
// returns the kept count. Returns `count` unchanged (nothing saved) when
// filtering is off, stale, empty, or nothing matched.
uint32_t FilterSceneDrawList(uint8_t* base, uint32_t sort_vec, uint32_t first,
                             uint32_t count) {
  OcclDispatchFilter& f = g_occl_filter;
  if (f.active || count == 0 || count > 100000 ||
      !REXCVAR_GET(skate3_native_render_scene_occlusion_cull_guest)) {
    return count;
  }
  if (f.stamp != g_frame_index) {
    f.stamp = g_frame_index;
    skate3::native_scene::CopyOcclusionCulledCtxs(f.ctxs);
  }
  if (f.ctxs.empty()) {
    return count;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return count;
  }
  const uint32_t seg = entries + first * 8;
  f.saved.assign(base + seg, base + seg + size_t(count) * 8);
  uint32_t kept = 0;
  uint32_t skipped = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* src = f.saved.data() + size_t(i) * 8;
    uint32_t ctx_be;
    std::memcpy(&ctx_be, src + 4, 4);
    const uint32_t mesh_context = __builtin_bswap32(ctx_be);
    if (mesh_context != 0 &&
        std::binary_search(f.ctxs.begin(), f.ctxs.end(), mesh_context)) {
      ++skipped;
      continue;
    }
    if (kept != i) {
      std::memcpy(base + seg + size_t(kept) * 8, src, 8);
    }
    ++kept;
  }
  if (skipped == 0) {
    f.saved.clear();
    return count;
  }
  f.active = true;
  f.saved_addr = seg;
  f.saved_bytes = count * 8;
  skate3::native_scene::AddGuestOcclSkipped(skipped);
  return kept;
}

void RestoreSceneDrawList(uint8_t* base) {
  OcclDispatchFilter& f = g_occl_filter;
  if (!f.active) {
    return;
  }
  std::memcpy(base + f.saved_addr, f.saved.data(), f.saved_bytes);
  f.active = false;
}

// Non-indexed cloth patch draws (see native_scene::CaptureClothDraw). The
// synthetic "context" key is the dynamic buffer object, stable per garment.
void OnClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
  uint32_t key = 0;
  const uint32_t dyn = skate3::native_scene::CaptureClothDraw(base, r4, r5, r6, r7, &key);
  if (dyn == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({3, key, 0, dyn});
}

// Precise guest frame pacing (see skate3_guest_fps_cap): called on the guest
// render thread at the swap boundary. Absolute-schedule pacing (target +=
// interval) so sleep jitter never accumulates; resyncs when the guest falls
// more than one interval behind (loads, hitches).
void PaceGuestFrame() {
  double cap = REXCVAR_GET(skate3_guest_fps_cap);
  if (REXCVAR_GET(skate3_guest_fps_cap_auto)) {
    // Refresh-derived cap (see the cvar). Falls through to the explicit cap
    // while the platform hasn't reported a refresh rate.
    const double auto_cap = double(rex::ui::Window::AutoFrameCapHz(
        rex::ui::Window::CachedDisplayRefreshHz()));
    if (auto_cap > 0.0) {
      cap = auto_cap;
    }
  }
  static std::chrono::steady_clock::time_point s_next{};
  if (cap < 1.0) {
    s_next = {};
    return;
  }
  const auto interval =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / cap));
  const auto now = std::chrono::steady_clock::now();
  if (s_next.time_since_epoch().count() == 0 || now > s_next + interval) {
    s_next = now + interval;
    return;
  }
  // Coarse sleep to ~1.5 ms before the target, then spin for precision.
  while (true) {
    const auto remaining = s_next - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      break;
    }
    if (remaining > std::chrono::milliseconds(2)) {
      std::this_thread::sleep_for(remaining - std::chrono::milliseconds(2));
    } else if (remaining > std::chrono::microseconds(50)) {
      std::this_thread::yield();
    }
  }
  s_next += interval;
}

void OnFrameEnd(uint8_t* base) {
  PaceGuestFrame();
  // EMULATED-mode guest frame breakdown (emulated gameplay once regressed
  // from 140 to 66 fps while native stayed at cap; the native-scene perf
  // line only prints while the native renderer is active, so emulated
  // stretches had no perf visibility at all). Logs every
  // ~600 guest frames while the native scene is OFF: whole-frame time (the
  // fps), time spent inside BuildFrameScene, and the per-frame draw/upload
  // hook traffic. If frame_avg is ~15 ms but build/hooks are small, the
  // cost is in the emulated GPU/CP pipeline, not our guest-thread code.
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_bd_prev{};
  static uint64_t s_bd_frames = 0;
  static double s_bd_frame_ns = 0, s_bd_frame_max = 0;
  static double s_bd_build_ns = 0, s_bd_build_max = 0;
  static uint64_t s_bd_draws0 = 0;
  const bool emu_profile = !skate3::native_scene::Enabled() &&
                           REXCVAR_GET(skate3_native_render_scene_perf_log);
  const auto bd_now = Clock::now();
  if (emu_profile && s_bd_prev.time_since_epoch().count() != 0) {
    const double dt =
        std::chrono::duration<double, std::nano>(bd_now - s_bd_prev).count();
    s_bd_frame_ns += dt;
    s_bd_frame_max = std::max(s_bd_frame_max, dt);
  }
  s_bd_prev = bd_now;

  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frame_index;
  const size_t mesh_count = g_current_frame.size();

  const auto bd_build0 = Clock::now();
  skate3::native_scene::BuildFrameScene(base, g_current_frame.data(),
                                        g_current_frame.size());
  if (emu_profile) {
    const double bns = std::chrono::duration<double, std::nano>(Clock::now() -
                                                                bd_build0)
                           .count();
    s_bd_build_ns += bns;
    s_bd_build_max = std::max(s_bd_build_max, bns);
    if (++s_bd_frames >= 600) {
      const uint64_t draws = skate3::native_scene::DrawSequence();
      // Quantum probe: a measured 1 ms sleep + the kernel's current timer
      // resolution. frame avg ~15 ms with build ~0 and the GPU at idle
      // wattage is the signature of every pacing sleep rounding up to the
      // 15.625 ms default Windows quantum; sleep1ms ~15.6 here proves it,
      // ~1-2 ms refutes it.
      const auto sp0 = Clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const double sleep_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - sp0).count();
      double timer_res_ms = -1.0;
#if defined(_WIN32)
      {
        using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
        static const auto query = reinterpret_cast<NtQueryTimerResolutionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
        if (query != nullptr) {
          ULONG min_res = 0, max_res = 0, cur_res = 0;
          if (query(&min_res, &max_res, &cur_res) == 0) {
            timer_res_ms = double(cur_res) / 10000.0;  // 100 ns units
          }
        }
      }
#endif
      REXLOG_INFO(
          "native-render EMULATED breakdown: frame avg={:.2f}/max={:.2f}ms "
          "build avg={:.3f}/max={:.3f}ms draws/frame={} sleep1ms={:.2f}ms "
          "timer_res={:.2f}ms (600-frame window)",
          s_bd_frame_ns / s_bd_frames / 1e6, s_bd_frame_max / 1e6,
          s_bd_build_ns / s_bd_frames / 1e6, s_bd_build_max / 1e6,
          (draws - s_bd_draws0) / s_bd_frames, sleep_ms, timer_res_ms);
      s_bd_frames = 0;
      s_bd_frame_ns = s_bd_frame_max = s_bd_build_ns = s_bd_build_max = 0;
      s_bd_draws0 = draws;
    }
  } else {
    s_bd_frames = 0;
    s_bd_frame_ns = s_bd_frame_max = s_bd_build_ns = s_bd_build_max = 0;
    s_bd_draws0 = skate3::native_scene::DrawSequence();
  }

  const int32_t log_interval = REXCVAR_GET(skate3_native_render_log_interval);
  if (log_interval > 0 && g_frame_index % static_cast<uint64_t>(log_interval) == 0) {
    REXLOG_INFO("native-render frame={} meshes={} snapshot_done={}", g_frame_index,
                mesh_count, skate3::native_scene::SnapshotWritten());
  }

  skate3::native_scene::OnCaptureFrameEnd(base, g_frame_index,
                                           g_current_frame);

  g_current_frame.clear();
}

}  // namespace

void Install() {
  if (!Enabled()) {
    return;
  }
  if (!g_announced.exchange(true)) {
    REXLOG_INFO(
        "native-render hook layer enabled");
  }
  skate3::native_scene::Install();
}

}  // namespace skate3::native_render

// "RenderMesh" per-visible-mesh submission for dynamic entities (characters,
// props). Actual convention (verified via recompiled code + snapshot):
// r3 = MeshContext*, r4 = renderengine::VertexProgramState*.
//
// The dynamic state snapshot must be taken AFTER the original call: the
// mesh's VS constants (instance world matrix, bone palette) are only flushed
// through D3D::SetPending_AluConstants from inside DrawIndexedVertices
// (sub_82B7AD68), i.e. during the RenderMesh body. Capturing on entry reads
// the PREVIOUS draw's constants and renders every dynamic entity with the
// previous entity's transform/palette.
// Deferred (multi-pass) meshes draw nothing inside the call, detected via
// the draw sequence counter so their transforms are left for the post-draw
// fixup instead of being read from a stale constant bank.
extern "C" REX_FUNC(sub_82795AD8) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t mesh_context = ctx.r3.u32;
  const uint32_t vertex_program_state = ctx.r4.u32;
  const uint64_t draws_before = skate3::native_scene::DrawSequence();
  __imp__sub_82795AD8(ctx, base);
  if (enabled) {
    const bool drew_inside = skate3::native_scene::DrawSequence() != draws_before;
    skate3::native_render::OnRenderMesh(base, mesh_context, vertex_program_state,
                                        drew_inside);
  }
}

// SceneRenderView sorted draw-list renderer (world geometry):
// sub_827FAF50(r3 = SceneRenderView*, r4 = eastl vector of 8-byte
// {sort_key, MeshContext*} entries, r5 = first, r6 = count). Called from
// SceneRenderView::Render (82 7FB158) for each of the view's key lists.
extern "C" REX_FUNC(sub_827FAF50) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnSceneDrawList(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                           ctx.r6.u32);
    // Guest-side occlusion dispatch filter: capture above saw every entry;
    // the dispatcher only gets the ones the occlusion cull did not prove
    // hidden. The segment is restored after the call (the list object
    // outlives this dispatch).
    const uint32_t kept = skate3::native_render::FilterSceneDrawList(
        base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (kept != ctx.r6.u32) {
      ctx.r6.u64 = kept;
      __imp__sub_827FAF50(ctx, base);
      skate3::native_render::RestoreSceneDrawList(base);
      return;
    }
  }
  __imp__sub_827FAF50(ctx, base);
}


// Guest D3D Swap: frame boundary.
extern "C" REX_FUNC(sub_82B82E08) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnFrameEnd(base);
  }
  __imp__sub_82B82E08(ctx, base);
}

// cProcessArenaAsset::RegisterTexture(cAssetList*, cAssetID,
// renderengine::Texture*, rw::Resource&): r4 = 64-bit asset guid,
// r5 = texture object.
extern "C" REX_FUNC(sub_82C9A618) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnRegisterTexture(ctx.r4.u64, ctx.r5.u32);
  }
  __imp__sub_82C9A618(ctx, base);
}

// ---- palette snapshot hooks (native/skate3_native_palette.h). Each fires
// POST-call, when m_matrices holds the settled packed upload palette.

// cModelInstance::PackAndMultiplyMatricesForUpload(Matrix44& localToWorld):
// post-call, m_matrices (+0x14) holds exactly the packed 4x3 column-major
// upload palette the VS consumes. r3 = the cModelInstance.
extern "C" REX_FUNC(sub_827E5B30) {
  const uint32_t instance = ctx.r3.u32;
  __imp__sub_827E5B30(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnPackPalette(base, instance);
  }
}

// Sk8::UpdateBoneTransforms(cModelInstance* parts, uint count, Matrix44*
// srcPalette, uint** remaps, Matrix44& out): r3 is an ARRAY of
// cModelInstance part records (stride 0x28): the player skater's per-piece
// palette source (its parts never reach the pack function above).
extern "C" REX_FUNC(sub_827A52C8) {
  const uint32_t parts = ctx.r3.u32;
  const uint32_t count = ctx.r4.u32;
  __imp__sub_827A52C8(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnBoneTransforms(base, parts, count);
  }
}

// Sk8::cLivingWorldPresEntity::Update: post-call, this+528 holds the
// entity's evaluated spawn/distance fade opacity (x = alpha), this+16 the
// current LOD index. Feeds the LW entity store (per-instance ctx ->
// alpha/identity, the serving path).
extern "C" REX_FUNC(sub_827C1188) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1188(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_lw::OnLwEntityTick(base, entity);
  }
}

// Sk8::SkaterPresEntity::StartJobs, bracketed as a pack owner:
// UpdateBoneTransforms calls inside stamp their snapshots with this entity.
extern "C" REX_FUNC(sub_827825B0) {
  const uint32_t prev_owner = skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_827825B0(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Sk8::SkaterPresEntity::DoubleBuffer(garment index): runs once per
// garment per COMPLETED cloth sim tick (the job's output memcpy + buffer
// flip). The identity store's deformed-VB freshness signal: garment-table
// fields persist after the sim stops, so this is the only per-tick proof
// the garment is really CPU-simulated right now.
extern "C" REX_FUNC(sub_82783038) {
  const uint32_t skater = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  __imp__sub_82783038(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnRopaDoubleBuffer(base, skater, index);
  }
}

// Sk8::SkaterPresEntity::EndJobs: calls PackAndMultiplyMatricesForUpload
// per instance with the tick's final locomotion (the body pose of record).
// Bracketed as a pack owner so those snapshots carry the entity.
extern "C" REX_FUNC(sub_82782818) {
  const uint32_t prev_owner =
      skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_82782818(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Skater-family virtual UBT+Pack driver (vtable slot +16): the remaining
// palette-write path (UpdateBoneTransforms + PackAndMultiply outside the
// StartJobs/EndJobs pair). Bracketed as a pack owner.
extern "C" REX_FUNC(sub_82785778) {
  const uint32_t prev_owner =
      skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_82785778(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Sk8::RenderPresentation::AddEntityToRenderViews / RmvEntityFrmRenderViews
// - r4 is the PresentationEntity being (de)registered: the identity store's
// scene-membership/lifetime signal (native/skate3_native_entity.h).
extern "C" REX_FUNC(sub_827A6C50) {
  const uint32_t entity = ctx.r4.u32;
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnEntityViewAdd(entity);
  }
  __imp__sub_827A6C50(ctx, base);
}

extern "C" REX_FUNC(sub_827A6CE8) {
  const uint32_t entity = ctx.r4.u32;
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnEntityViewRemove(entity);
  }
  __imp__sub_827A6CE8(ctx, base);
}

// Sk8::PresentationEntity::BindConstants (base override): post-call, every
// MeshContext of the entity's current LOD has just received its constant
// param POINTERS (world hash 0x7DDF552B -> &entity+416, etc.; the game
// binds by pointer and dereferences at draw). Every subclass bind calls
// this base, so the exit sees every renderable presentation entity with
// its full ctx set: the ctx -> entity identity write point.
extern "C" REX_FUNC(sub_827A6658) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827A6658(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindConstants(base, entity);
  }
}

// Derived BindConstants overrides: class tags for the identity store. The
// most-derived override returns last (each calls its parent first), so the
// last tag to land is the entity's concrete class.
extern "C" REX_FUNC(sub_82783D68) {  // Sk8::SkaterPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82783D68(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kSkater);
  }
}

extern "C" REX_FUNC(sub_82785260) {  // ColorizedSkaterPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82785260(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kColorized);
  }
}

extern "C" REX_FUNC(sub_82793F70) {  // CACPresEntity (untransposed world)
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82793F70(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kCac);
  }
}

extern "C" REX_FUNC(sub_82785528) {  // unnamed skater-layout class
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82785528(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kSkaterAux);
  }
}

extern "C" REX_FUNC(sub_827C1720) {  // cLivingWorldPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1720(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kLivingWorld);
  }
}

// pegasus::tRModelData::Fixup(void* model, rw::core::arena::ArenaIterator*)
// - the rw-arena LOAD-time pointer resolve, fired once per model while its
// arena streams in. (The `Unfix(void*, SizeAndAlignment*)` atoms are the
// SAVE/size path; hooking tROptiMeshData::Unfix never fired during loads.)
// Post-call the model's mesh table is live: queue its meshes for the
// prewarm decode workers. This is the EARLY prewarm source; it fires
// throughout the load's disk-streaming phase, hours of decode headroom
// before the final-seconds AddRenderInstance activation burst. The atoms
// dispatch indirectly through the recomp function table, which resolves to
// this override at link time like any other reference.
extern "C" REX_FUNC(sub_82963510) {
  const uint32_t model = ctx.r3.u32;
  __imp__sub_82963510(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnModelFixup(base, model);
  }
}

// Sk8::Challenge::PhotoReplayController::Update(float): runs once per guest
// frame while a photo-mission's photo editor is up (pick-a-photo +
// depth-of-field / saturation / brightness / contrast controls). Heartbeat
// for the native scene's photo-editor yield: the editor's effects are the
// game's own postfx chain, which only the emulated path executes. Reached
// via the recomp function table (virtual dispatch), so the override fires
// like any direct call.
extern "C" REX_FUNC(sub_825623F0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoReplayUpdate();
  }
  __imp__sub_825623F0(ctx, base);
}

// Sk8::FE::FrontEndState_Replay2::TakePhoto(): fires once when the player
// takes a photo (replay editor / photo mission Select). The game then
// renders the shot into the 1152x640 PostFX screenshot target, resolves it,
// and ScreenshotBackEnd::GrabScreenshot CPU-reads the resolved guest memory
// to JPEG-encode it, which is all zeros unless resolve readback is forced
// (the invisible-final-photo bug: an F11 capture showed the grab texture
// 0x04911000 memory fully zero). Arms the photo-grab readback window.
extern "C" REX_FUNC(sub_826147C8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnTakePhoto();
  }
  __imp__sub_826147C8(ctx, base);
}

// Sk8::BE::ScreenshotBackEnd::GrabScreenshot(bool), the actual grab: the
// game CPU-reads the resolved screenshot target from guest memory and
// JPEG-encodes it. Fires in EVERY grab flow; the photo-mission Select
// confirm does NOT go through FrontEndState_Replay2::TakePhoto (a logged
// full photo-mission run never opened the card compose window), so this is
// the canonical shutter heartbeat. Right after the grab
// the FE composes the framed display card (white border / caption / logo)
// over the JPEG texture in a ONE-SHOT RTT pass; the card compose window
// (UpdatePhotoGrabWindow) keys off this timestamp so that pass executes and
// its resolve lands in CPU guest memory for the native 2D decoder.
extern "C" REX_FUNC(sub_824FD550) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnTakePhoto();
  }
  __imp__sub_824FD550(ctx, base);
  // Post-call: the grab has CPU-read the shot; the card compose that
  // follows reads already-copied memory. Event-closes the shutter burst.
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoGrabDone();
  }
}

// Sk8::BE::ScreenshotBackEnd grab REQUEST (sets the grab params + arms the
// request flag; called by the FE photo flows 1-2 frames BEFORE the game
// renders the shot + card-composite frame sequence whose resolves
// GrabScreenshot/OnScreenShot then CPU-read). This is the only PREDICTIVE
// shutter signal; the GrabScreenshot hook above fires AFTER those frames
// already rendered (suppressed). Arms the shutter burst: suppression lifted
// + full small-resolve readbacks for ~1.5 s so the card-build's inputs are
// real (a logged control run showed the composite CPU copies land exactly
// in this window; every post-hoc window/bound missed them).
extern "C" REX_FUNC(sub_824FD4B0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoGrabRequest();
  }
  __imp__sub_824FD4B0(ctx, base);
}

namespace skate3::native_scene {
// Fault-guarded guest read (defined in skate3_native_scene.cpp).
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out);
}  // namespace skate3::native_scene

REXCVAR_DEFINE_BOOL(
    skate3_autoexposure_pin, true, "Skate 3",
    "Pin the game's auto-exposure at its per-zone maximum. The game "
    "adapts world exposure from a GPU luminance measurement it reads "
    "back from memory; with resolve readbacks disabled (the native "
    "renderer's standard configuration) that measurement reads empty and "
    "exposure settles at the maximum anyway - but windows that "
    "temporarily enable readbacks (the photo shutter) can leak one real "
    "measurement of a bright frame, after which the frozen measurement "
    "drags exposure to the minimum clamp permanently (the stuck world "
    "darkening after photo missions). Pinning removes the dependence on "
    "measurement availability. Turn OFF only when running with full "
    "resolve readbacks enabled, where live per-frame measurements make "
    "the game's own adaptation behave as on console.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// World auto-exposure evaluator (per frame): reads the GPU luminance
// measurement (sum / area of a resolved measurement surface), adapts the
// scene exposure toward the target luminance and clamps it to the
// per-zone [min, max] from the tuning block at [this+2468]+120..132
// (target, min, max, rate). Without resolve readbacks the measurement
// reads empty and exposure settles at the maximum; a readback window
// that briefly lets one bright measurement through (the photo shutter
// burst) leaves a frozen too-bright measurement that drags exposure to
// the minimum clamp permanently. The hook re-pins the adapted exposure
// (the evaluator's internal state and both published outputs, honoring
// the same ownership gates the game checks) to the per-zone maximum.
extern "C" REX_FUNC(sub_827F0D00) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_827F0D00(ctx, base);
  if (!REXCVAR_GET(skate3_autoexposure_pin) || self < 0x10000) {
    return;
  }
  uint32_t aux = 0, inst_a = 0, inst_b = 0, max_bits = 0;
  if (!skate3::native_scene::GuestTryLoadU32(base, self + 2468, &aux) ||
      aux < 0x10000 ||
      !skate3::native_scene::GuestTryLoadU32(base, aux + 128, &max_bits) ||
      !skate3::native_scene::GuestTryLoadU32(base, self + 2484, &inst_a) ||
      !skate3::native_scene::GuestTryLoadU32(base, self + 2500, &inst_b)) {
    return;
  }
  float max_expo;
  std::memcpy(&max_expo, &max_bits, sizeof(max_expo));
  if (!(max_expo > 0.0f) || max_expo > 16.0f) {
    return;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  uint32_t prev_bits = REX_LOAD_U32(self + 3240);
  float prev;
  std::memcpy(&prev, &prev_bits, sizeof(prev));
  static std::atomic<int64_t> s_last_log_ns{-1};
  if (prev > 0.0f && prev < max_expo * 0.98f) {
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const int64_t last = s_last_log_ns.load(std::memory_order_relaxed);
    if (last < 0 || now_ns - last > 60'000'000'000) {
      s_last_log_ns.store(now_ns, std::memory_order_relaxed);
      REXLOG_INFO(
          "native-render: auto-exposure re-pinned at zone max {:.3f} "
          "(adaptation had moved it to {:.3f}; luminance measurement "
          "unavailable without resolve readbacks)",
          max_expo, prev);
    }
  }
  REX_STORE_U32(self + 3240, max_bits);
  REX_STORE_U32(self + 3392, max_bits);
  const uint32_t own_a = REX_LOAD_U8(self + 3397);
  const uint32_t own_b = REX_LOAD_U8(self + 3396);
  if (own_a == 0 && inst_a >= 0x10000) {
    REX_STORE_U32(inst_a + 48, max_bits);
  }
  if (own_b == 0 && inst_b >= 0x10000) {
    REX_STORE_U32(inst_b + 56, max_bits);
  }
}

// rw::movie::MovieDecoder::Decode(int, VideoRenderable**,
// SubtitleRenderable*): fires per decoded FMV frame while any movie plays
// (boot intro logos and all other rw::movie playback). Heartbeat for the
// native scene's FMV yield: the video frame is CPU-decoded into a texture
// and reaches the screen through the game's postfx chain + swap without a
// capturable 2D draw, so only the emulated path can show it.
extern "C" REX_FUNC(sub_82A92DC8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnMovieDecode();
  }
  __imp__sub_82A92DC8(ctx, base);
}

// VideoRenderer_RwTexture::Render(VideoRenderable*, int): fills the three
// YUV plane textures (members this+12/+124/+68) with the decoded FMV frame
// via Texture::Lock/FillTextureData/Unlock. Post-call, the planes hold the
// finished frame: publish their fetch words for the native FMV blit.
extern "C" REX_FUNC(sub_8263C498) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8263C498(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnMovieFrame(base, self);
  }
}

// Sk8::WorldPresentation::AddRenderInstance(pegasus::tInstance*): the world
// registry add, fired per placed instance while a map loads (r4 = tInstance).
// The prewarm's primary mesh source: the instance's tRModelData mesh table
// is walked (validated offsets) and every optimesh queued for the
// loading-screen decode.
extern "C" REX_FUNC(sub_82791290) {
  const uint32_t instance = ctx.r4.u32;
  __imp__sub_82791290(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnAddRenderInstance(base, instance);
  }
}

// D3D::SetPending_AluConstants(device, u64 dirty_group_mask, bank, ptr):
// bank 0x4000 = vertex constants. Called from inside the Draw* functions;
// ptr is the device's positional constant shadow bank.
extern "C" REX_FUNC(sub_82B83FE0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnVsConstantUpload(base, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                             ctx.r3.u32);
  }
  __imp__sub_82B83FE0(ctx, base);
}

// D3DDevice_SetIndices(device, ib) / D3DDevice_SetStreamSource(device,
// stream, vb, offset, stride): track the currently bound guest buffers so
// draws can be matched back to captured skinned items.
extern "C" REX_FUNC(sub_82B79190) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetIndices(ctx.r4.u32);
  }
  __imp__sub_82B79190(ctx, base);
}

extern "C" REX_FUNC(sub_82B78FF0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetStreamSource(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                            ctx.r7.u32);
  }
  __imp__sub_82B78FF0(ctx, base);
}

// D3DDevice_DrawIndexedVertices: post-call, the draw's VS constants are now
// in the shadow bank; refresh any pending skinned item bound to these
// buffers (deferred multi-pass meshes only draw here).
extern "C" REX_FUNC(sub_82B7AD68) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7AD68(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 0, r4, r5, r6, r7);
  }
}

// D3DDevice_DrawVertices, non-indexed draw path: cloth-simulated garments
// (captured live as world-space quad items) and character shadow proxies.
extern "C" REX_FUNC(sub_82B7A970) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7A970(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 1, r4, r5, r6, r7);
    skate3::native_render::OnClothDraw(base, r4, r5, r6, r7);
  }
}

// ---- 2D / APT (Flash-converted HUD) reconnaissance hooks -----------------
// Every HUD/menu 2D element is a Flash SWF converted to EA APT, rendered by
// Sk8::FE::AptRenderingIntegration through the same guest D3D draw functions
// hooked above. These brackets tag draws issued inside the 2D pass so the
// recorder can capture and the future native 2D pass can replay them.

// Sk8::FE::FrontEndManager::Render2D(unsigned int): the game's whole 2D
// pass (FE movies + HUD).
extern "C" REX_FUNC(sub_825D9168) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(0, true);
  __imp__sub_825D9168(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(0, false);
}

// Sk8::FE::AptMovieIntegration::Render(unsigned int): one APT movie.
extern "C" REX_FUNC(sub_825D67D8) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(1, true);
  __imp__sub_825D67D8(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(1, false);
}

// Sk8::FE::AptRenderingIntegration::DrawRenderingUnit(void*, AptRenderInfo
// const*): one APT display-list element (texture quad / vector shape).
extern "C" REX_FUNC(sub_825D4490) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(2, true);
  __imp__sub_825D4490(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(2, false);
}

// Sk8::FE::AptRenderingIntegration::UpdateRenderToTexture(unsigned int):
// in gameplay this renders the whole HUD into a screen-sized overlay
// texture at true screen coordinates (the game composites it later through
// the suppressed emulated pass). Diagnostic bracket only.
extern "C" REX_FUNC(sub_825D4E50) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(3, true);
  __imp__sub_825D4E50(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(3, false);
}

// Sk8::Render::cFont::DrawstringLocal<char> / <unsigned short>: the glyph
// text emitter (trick names, scores). Text can flush OUTSIDE the APT
// brackets, so it gets its own bit.
extern "C" REX_FUNC(sub_82808388) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(4, true);
  __imp__sub_82808388(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(4, false);
}

extern "C" REX_FUNC(sub_82808708) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(4, true);
  __imp__sub_82808708(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(4, false);
}

// Sk8::Render::SimpleDraw::DrawParameters::Draw: the game's immediate-mode
// quad/tri utility (chase arrows, in-world guide markers/beams, debug
// draws). Bottoms out in BeginVertices like the APT path.
extern "C" REX_FUNC(sub_82804168) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(5, true);
  __imp__sub_82804168(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(5, false);
}

// D3DDevice_SetPixelShader / SetVertexShader (r4 = guest shader object):
// recorded per draw to group the 2D stream by shader variant.
extern "C" REX_FUNC(sub_82B7F408) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetShader(true, ctx.r4.u32);
  }
  __imp__sub_82B7F408(ctx, base);
}

extern "C" REX_FUNC(sub_82B7F150) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetShader(false, ctx.r4.u32);
  }
  __imp__sub_82B7F150(ctx, base);
}

// D3D::SetPending_RenderStates(device, u64 dirty mask, bank, ptr): the
// render-state shadow bank (blend/depth state for 2D draws lives here).
extern "C" REX_FUNC(sub_82B83C48) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnRenderStateUpload(ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_82B83C48(ctx, base);
}

// D3DDevice_SetViewport(device, D3DVIEWPORT*) / SetScissorRect(device,
// RECT*): recorded per draw (render-to-texture APT passes and mask rects).
extern "C" REX_FUNC(sub_82B74310) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetViewport(base, ctx.r4.u32);
  }
  __imp__sub_82B74310(ctx, base);
}

extern "C" REX_FUNC(sub_82B769C0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetScissor(base, ctx.r4.u32);
  }
  __imp__sub_82B769C0(ctx, base);
}

// D3DDevice_BeginVertices: inline (write-through-ring) vertex path; the
// CPU writes computed vertices directly. Post-call r3 = guest write pointer.
extern "C" REX_FUNC(sub_82B79FC0) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B79FC0(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 2, r4, r5, r6, ctx.r3.u32 != 0 ? ctx.r3.u32 : r7);
  }
}

// LivingWorld batch pack writer (unnamed; called per entity per sim tick
// from the tail of cLivingWorldPresEntityManager::Update), the LOD-
// pedestrian skinning-palette source: fused UpdateBoneTransforms+Pack
// writing the concatenated per-mesh 4x3 palettes into
// cModelInstance.m_matrices. r3 = the cLivingWorldPresEntity-derived
// entity; post-call m_matrices holds the packed rows.
extern "C" REX_FUNC(sub_827C1D38) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1D38(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnLwPack(base, entity);
  }
}

