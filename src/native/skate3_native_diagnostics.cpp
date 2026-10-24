// Native renderer diagnostics: offline-analysis recording, camera/bone
// signal recorders, synthetic-pan probe. Extracted verbatim from
// skate3_native_scene.cpp; no rendering logic lives here.

#include "native/skate3_native_diag.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#include <rex/logging.h>

#include <cinttypes>
#include <string>

#include <rex/cvar.h>

// Guest-thread shared scene state (the post-sweep's shadow-readiness gate
// reads g_shadow_have; OnCaptureFrameEnd runs on the same thread).
#include "skate3_native_scene_state.h"

#include "skate3_init.h"
#include "skate3_screenshot.h"

#if defined(_WIN32)
#include <windows.h>
// Xbox-controller capture combos (see the hotkey block): the artifacts under
// investigation are too brief to reach the keyboard from the pad.
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")
#endif

REXCVAR_DEFINE_INT32(skate3_native_render_snapshot_min_meshes, 0, "Skate 3",
                     "One-shot guest memory snapshot: trigger on the first frame with at "
                     "least this many RenderMesh submissions (0 = disabled)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_snapshot_frames, 4, "Skate 3",
                     "Number of frames of RenderMesh records to collect before writing "
                     "the snapshot")
    .range(1, 600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Defined in skate3_native_scene.cpp (the recording filter lives there).
REXCVAR_DECLARE(bool, skate3_native_render_snapshot_all_draws);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_photo_compose_trace, false, "Skate 3",
    "Auto-capture an F10-style diagnostic recording (all draws, ~360 "
    "frames + memory snapshot) the first time a photo display card comes "
    "up in a session. Captures the game's one-shot framed-card compose "
    "pass: its draws, source textures (frame art / logo / caption) and "
    "geometry, for offline analysis. Writes ~100 MB into "
    "native_render_snapshots once per session.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_snapshot_stride, 1, "Skate 3",
                     "Record every Nth frame while collecting (long viewer recordings: "
                     "e.g. 12 = ~12 recorded frames/sec at 144fps)")
    .range(1, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Defined in skate3_native_scene.cpp (the HDR post-effect cvars the sweep
// below drives).
REXCVAR_DECLARE(bool, skate3_native_render_scene_bloom);
REXCVAR_DECLARE(bool, skate3_native_render_scene_haze);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_hdr_debug);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shafts);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_post_sweep, false, "Skate 3",
    "One-shot HDR post-effect comparison sweep: once gameplay is on screen, "
    "steps through baseline (bloom/shafts/haze off), each effect alone, "
    "everything on, and the hdr_debug views (shaft plane / haze term / sky "
    "gate), settling between steps and writing one tagged screenshot per "
    "step (shot_<ts>_sweep_<step>.png), then restores the previous cvar "
    "values and turns itself off. Arm via config for a hands-off capture "
    "run, or from the debug dialog mid-session.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_snapshot_dir, "native_render_snapshots", "Skate 3",
                      "Directory for native-render guest memory snapshots and metadata")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_capture_hotkeys, false, "Skate 3",
                    "Enable the diagnostic capture triggers: F7 scene-ring dump, F8 "
                    "cache flush, F9/F10 snapshot recording, F11 A/B parity capture, "
                    "controller RB+X / RB+A combos, and the snapshot-dir trigger "
                    "file. Off = no key/pad polling and no capture output.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_scene {

// ---- Offline-analysis recording ---------------------------------------------

std::mutex g_record_mutex;
std::atomic<bool> g_recording{false};
uint32_t g_record_frame = 0;   // index of the NEXT recorded frame
uint32_t g_record_stride = 1;  // record every Nth frame
uint32_t g_frames_seen = 0;    // frames completed since StartRecording
std::vector<std::unique_ptr<RecordedDraw>> g_recorded_draws;
std::vector<RecordedFrame> g_recorded_frames;
std::vector<RecordedBuffer> g_recorded_buffers;
std::unordered_set<uint64_t> g_recorded_buffer_keys;
size_t g_recorded_buffer_bytes = 0;
std::vector<PendingInlineDump> g_pending_inline_dumps;
std::unordered_map<uint64_t, uint32_t> g_frame_dump_ids;

void StartRecording(uint32_t stride) {
  std::lock_guard<std::mutex> lock(g_record_mutex);
  g_recorded_draws.clear();
  g_recorded_frames.clear();
  g_recorded_buffers.clear();
  g_recorded_buffer_keys.clear();
  g_recorded_buffer_bytes = 0;
  g_pending_inline_dumps.clear();
  g_frame_dump_ids.clear();
  g_record_frame = 0;
  g_frames_seen = 0;
  g_record_stride = stride == 0 ? 1 : stride;
  g_recording.store(true, std::memory_order_relaxed);
}

void WriteRecording(const char* dir, const char* stem) {
  g_recording.store(false, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_record_mutex);
  const std::filesystem::path base_path = std::filesystem::path(dir) / stem;

  // Binary draw stream: header "SK3DRAW7", fixed records {u32 frame, u32
  // func, u32 ib, u32 vb, u32 vb_offset, u32 vb_stride, u32 streams[4][3],
  // u32 vfetch[12], u32 args[4], f32 vs_bank[1024], f32 ps_bank[256],
  // u32 flags2d, u32 ps_obj, u32 vs_obj, u32 viewport[6], u32 scissor[4],
  // u32 rstates[256], u32 vfetch_all[192], u32 vb_dump, u32 ib_dump}
  // (little-endian). func: 0 DrawIndexedVertices, 1 DrawVertices,
  // 2 BeginVertices. flags2d: bit0 FrontEndManager::Render2D, bit1
  // AptMovieIntegration::Render, bit2 DrawRenderingUnit. vb_dump/ib_dump
  // index into buffers.bin records (~0u = none).
  {
    std::ofstream out(base_path.string() + ".draws.bin", std::ios::binary);
    out.write("SK3DRAW7", 8);
    for (const auto& d : g_recorded_draws) {
      out.write(reinterpret_cast<const char*>(&d->frame), 4);
      out.write(reinterpret_cast<const char*>(&d->func), 4);
      out.write(reinterpret_cast<const char*>(&d->ib), 4);
      out.write(reinterpret_cast<const char*>(&d->vb), 4);
      out.write(reinterpret_cast<const char*>(&d->vb_offset), 4);
      out.write(reinterpret_cast<const char*>(&d->vb_stride), 4);
      out.write(reinterpret_cast<const char*>(d->streams), 48);
      out.write(reinterpret_cast<const char*>(d->vfetch), 48);
      out.write(reinterpret_cast<const char*>(d->args), 16);
      out.write(reinterpret_cast<const char*>(d->bank), 4096);
      out.write(reinterpret_cast<const char*>(d->ps), 1024);
      out.write(reinterpret_cast<const char*>(&d->flags2d), 4);
      out.write(reinterpret_cast<const char*>(&d->ps_obj), 4);
      out.write(reinterpret_cast<const char*>(&d->vs_obj), 4);
      out.write(reinterpret_cast<const char*>(d->viewport), 24);
      out.write(reinterpret_cast<const char*>(d->scissor), 16);
      out.write(reinterpret_cast<const char*>(d->rstates), 1024);
      out.write(reinterpret_cast<const char*>(d->vfetch_all), 768);
      out.write(reinterpret_cast<const char*>(&d->vb_dump), 4);
      out.write(reinterpret_cast<const char*>(&d->ib_dump), 4);
    }
  }

  // Captured buffer payloads: header "SK3BUFS1", then records {u32 vb_addr,
  // u32 ib_addr, u64 fingerprint, u32 vb_len, u32 ib_len, raw vb, raw ib}.
  {
    std::ofstream out(base_path.string() + ".buffers.bin", std::ios::binary);
    out.write("SK3BUFS1", 8);
    for (const RecordedBuffer& b : g_recorded_buffers) {
      const uint32_t vb_len = uint32_t(b.vb.size());
      const uint32_t ib_len = uint32_t(b.ib.size());
      out.write(reinterpret_cast<const char*>(&b.vb_addr), 4);
      out.write(reinterpret_cast<const char*>(&b.ib_addr), 4);
      out.write(reinterpret_cast<const char*>(&b.fingerprint), 8);
      out.write(reinterpret_cast<const char*>(&vb_len), 4);
      out.write(reinterpret_cast<const char*>(&ib_len), 4);
      out.write(reinterpret_cast<const char*>(b.vb.data()), vb_len);
      out.write(reinterpret_cast<const char*>(b.ib.data()), ib_len);
    }
  }

  // Per-frame item dump.
  {
    std::ofstream out(base_path.string() + ".scene.jsonl");
    const auto write_item = [&out](const DrawItem& d) {
      out << "{\"mesh\":\"" << std::hex << d.mesh << "\",\"ib_obj\":\"" << d.ib_obj
          << "\",\"vb_obj\":\"" << d.vb_obj << "\",\"vb_addr\":\"" << d.vb_addr
          << "\",\"ib_addr\":\"" << d.ib_addr << "\",\"diffuse\":\"" << d.diffuse_tex
          << "\",\"diffuse_fetch\":\"" << d.diffuse_fetch[1]
          << "\",\"lightmap\":\"" << d.lightmap_tex << "\",\"macro\":\"" << d.macro_tex
          << "\",\"decal_art\":\"" << d.decal_art
          << "\",\"decal_fetch\":\"" << d.decal_fetch[1]
          << "\",\"fp\":\"" << d.fingerprint
          << "\"" << std::dec << ",\"vb_bytes\":" << d.vb_bytes << ",\"ib_count\":"
          << d.ib_count << ",\"stride\":" << int(d.stride) << ",\"pos_fmt\":"
          << int(d.pos_fmt) << ",\"pos_off\":" << d.pos_offset << ",\"bw_off\":"
          << d.bw_offset << ",\"bi_off\":" << d.bi_offset << ",\"skinned\":"
          << (d.skinned ? 1 : 0) << ",\"pending\":" << (d.pending ? 1 : 0)
          << ",\"decal\":" << (d.decal ? 1 : 0)
          << ",\"transparent\":" << (d.transparent ? 1 : 0)
          << ",\"selected\":" << (d.selected ? 1 : 0)
          // Provenance: which pipeline path produced this item (dbg_src
          // codes in skate3_native_scene.h) and the identity keys the
          // offline analyzers join on.
          << ",\"ctx\":\"" << std::hex << d.ctx << std::dec
          << "\",\"dbg_src\":" << int(d.dbg_src)
          << ",\"ropa\":" << (d.ropa ? 1 : 0)
          << ",\"fam\":" << int(d.char_family)
          << ",\"caster\":" << (d.caster_bank ? 1 : 0)
          << ",\"retained\":" << (d.retained ? 1 : 0) << ",\"world\":[";
      for (int i = 0; i < 16; ++i) out << (i ? "," : "") << d.world[i];
      out << "],\"draws\":[";
      for (size_t i = 0; i < d.draws.size(); ++i) {
        const DrawEntry& e = d.draws[i];
        out << (i ? "," : "") << "[" << e.prim << "," << e.base_vertex << ","
            << e.start_index << "," << e.index_count << "]";
      }
      out << "],\"bones\":[";
      for (size_t i = 0; i < d.bones.size(); ++i) out << (i ? "," : "") << d.bones[i];
      out << "]}";
    };
    for (const RecordedFrame& rf : g_recorded_frames) {
      out << "{\"generation\":" << rf.generation << ",\"cam\":[" << rf.cam_pos[0] << ","
          << rf.cam_pos[1] << "," << rf.cam_pos[2] << "],\"view_proj\":[";
      for (int i = 0; i < 16; ++i) out << (i ? "," : "") << rf.view_proj[i];
      out << "],\"dynitems\":[";
      for (size_t i = 0; i < rf.dynitems.size(); ++i) {
        if (i) out << ",";
        write_item(rf.dynitems[i]);
      }
      out << "],\"items\":[";
      for (size_t i = 0; i < rf.items.size(); ++i) {
        if (i) out << ",";
        write_item(rf.items[i]);
      }
      out << "]}\n";
    }
  }
  REXLOG_INFO(
      "native-scene: recording written ({} draws, {} frames, {} buffers {} MiB) -> "
      "{}.draws.bin/.scene.jsonl/.buffers.bin",
      g_recorded_draws.size(), g_recorded_frames.size(), g_recorded_buffers.size(),
      g_recorded_buffer_bytes >> 20, base_path.string());
  g_recorded_draws.clear();
  g_recorded_frames.clear();
  g_recorded_buffers.clear();
  g_recorded_buffer_keys.clear();
  g_recorded_buffer_bytes = 0;
  g_pending_inline_dumps.clear();
  g_frame_dump_ids.clear();
}

// ---- Camera-signal recorder ---------------------------------------------------

namespace {
struct CamSigEntry {
  double t;       // host time (sampler push / frame build)
  double play_t;  // kind 1 only: the smoother's playback time
  float yaw;      // heading, degrees
  uint8_t kind;   // 0 = raw sampler pose, 1 = smoothed frame, 2 = raw frame
};
std::mutex g_camsig_mutex;
std::vector<CamSigEntry> g_camsig;
}  // namespace
std::atomic<double> g_camsig_deadline{0.0};

float YawFromViewRows(const float view[16]) {
  return std::atan2(view[2], view[10]) * float(180.0 / 3.14159265358979323846);
}

void CamSigPush(double t, double play_t, float yaw, uint8_t kind) {
  std::lock_guard<std::mutex> lock(g_camsig_mutex);
  if (g_camsig.size() < 200000) {
    g_camsig.push_back({t, play_t, yaw, kind});
  }
}

void CamSigFrameTick(double rec_now, const float cam_view[16],
                     const float* smoothed_rot, double play_t) {
  const double dl = g_camsig_deadline.load(std::memory_order_relaxed);
  if (dl <= 0.0) {
    return;
  }
  if (rec_now < dl) {
    std::lock_guard<std::mutex> lock(g_camsig_mutex);
    if (g_camsig.size() < 200000) {
      g_camsig.push_back({rec_now, 0.0, YawFromViewRows(cam_view), 2});
      if (smoothed_rot) {
        g_camsig.push_back({rec_now, play_t, YawFromViewRows(smoothed_rot), 1});
      }
    }
  } else {
    std::vector<CamSigEntry> entries;
    {
      std::lock_guard<std::mutex> lock(g_camsig_mutex);
      entries.swap(g_camsig);
    }
    g_camsig_deadline.store(0.0, std::memory_order_release);
    std::thread([entries = std::move(entries)]() {
      std::error_code ec;
      std::filesystem::create_directories("logs", ec);
      char path[128];
      std::snprintf(path, sizeof(path), "logs/cam_signal_%lld.csv",
                    static_cast<long long>(std::time(nullptr)));
      std::ofstream f(path);
      f << "kind,t,play_t,yaw_deg\n";
      char line[128];
      for (const CamSigEntry& e : entries) {
        std::snprintf(line, sizeof(line), "%d,%.6f,%.6f,%.5f\n", int(e.kind), e.t,
                      e.play_t, double(e.yaw));
        f << line;
      }
      REXLOG_INFO("native-scene cam-signal: wrote {} entries -> {}", entries.size(),
                  path);
    }).detach();
  }
}

void RecordCameraSignal(double seconds) {
  {
    std::lock_guard<std::mutex> lock(g_camsig_mutex);
    g_camsig.clear();
    g_camsig.reserve(size_t(seconds * 1400.0));
  }
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  g_camsig_deadline.store(now + seconds, std::memory_order_release);
  REXLOG_INFO(
      "native-scene cam-signal: recording {} s; pan the camera with the stick at a "
      "steady rate NOW",
      seconds);
}

// ---- Bone-signal recorder -----------------------------------------------------

namespace {
std::vector<uint8_t> g_bonesig;
std::atomic<double> g_bonesig_deadline{0.0};
// Armed from the UI thread (F12 button), consumed on the guest thread; the
// blob itself is guest-thread-only.
std::atomic<double> g_bonesig_request{0.0};
}  // namespace

void BoneSigAppend(uint8_t kind, uint64_t key, double t, double play, const float* v,
                   uint32_t n) {
  if (g_bonesig.size() > (200u << 20)) {
    return;  // runaway cap
  }
  const size_t off = g_bonesig.size();
  g_bonesig.resize(off + 29 + size_t(n) * 4);
  uint8_t* p = g_bonesig.data() + off;
  *p = kind;
  std::memcpy(p + 1, &key, 8);
  std::memcpy(p + 9, &t, 8);
  std::memcpy(p + 17, &play, 8);
  std::memcpy(p + 25, &n, 4);
  std::memcpy(p + 29, v, size_t(n) * 4);
}

bool BoneSigTick(double now) {
  const double req = g_bonesig_request.exchange(0.0, std::memory_order_acq_rel);
  if (req > 0.0) {
    g_bonesig.clear();
    g_bonesig.reserve(16u << 20);
    g_bonesig_deadline.store(now + req, std::memory_order_relaxed);
  }
  const double dl = g_bonesig_deadline.load(std::memory_order_relaxed);
  if (dl <= 0.0) {
    return false;
  }
  if (now < dl) {
    return true;
  }
  std::vector<uint8_t> blob;
  blob.swap(g_bonesig);
  g_bonesig_deadline.store(0.0, std::memory_order_release);
  std::thread([blob = std::move(blob)]() {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    char path[128];
    std::snprintf(path, sizeof(path), "logs/bone_signal_%lld.bin",
                  static_cast<long long>(std::time(nullptr)));
    std::ofstream f(path, std::ios::binary);
    f.write("BSIG1\n", 6);
    f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    REXLOG_INFO("native-scene bone-signal: wrote {} bytes -> {}", blob.size() + 6,
                path);
  }).detach();
  return false;
}

void RecordBoneSignal(double seconds) {
  g_bonesig_request.store(seconds, std::memory_order_release);
  REXLOG_INFO(
      "native-scene bone-signal: recording {} s; skate past the camera at a steady "
      "speed NOW",
      seconds);
}

// ---- Synthetic camera pan -----------------------------------------------------

std::atomic<int> g_synpan_active{0};  // engaged mode (0 = off)
std::mutex g_synpan_mutex;
float g_synpan_view0[16];  // raw guest view at engage
float g_synpan_proj0[16];
float g_synpan_c0[3];   // camera world position at engage (held fixed)
double g_synpan_t0 = 0.0;
double g_synpan_step_phase = 0.0;  // mode 2: accumulated phase (degrees)
double g_synpan_ema_dt = 0.0;      // mode 2: slow EMA of the publish dt
uint64_t g_synpan_frames = 0;
double g_synpan_last_build = 0.0;
double g_synpan_dt_sum = 0.0, g_synpan_dt_sum2 = 0.0;
double g_synpan_dt_min = 0.0, g_synpan_dt_max = 0.0;
double g_synpan_err_sum2 = 0.0, g_synpan_err_max = 0.0;
uint64_t g_synpan_err_n = 0;
std::unordered_map<uint64_t, DrawItem> g_synpan_union;

uint64_t SynPanItemKey(const DrawItem& it) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(it.mesh);
  mix(it.vb_obj);
  mix(it.ib_obj);
  mix(std::bit_cast<uint32_t>(it.world[12]));
  mix(std::bit_cast<uint32_t>(it.world[13]));
  mix(std::bit_cast<uint32_t>(it.world[14]));
  return h;
}

double SynPanAngleDeg(double phase_deg, double amp) {
  if (amp <= 0.0) {
    return phase_deg;
  }
  const double period = 4.0 * amp;
  double m = std::fmod(phase_deg, period);
  if (m < 0.0) {
    m += period;
  }
  return m < amp ? m : (m < 3.0 * amp ? 2.0 * amp - m : m - period);
}

void SynPanView(double angle_deg, float view_out[16]) {
  const double a = angle_deg * (3.14159265358979323846 / 180.0);
  const float c = float(std::cos(a)), s = float(std::sin(a));
  const float ry[3][3] = {{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}};
  std::memset(view_out, 0, 16 * sizeof(float));
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      float sum = 0.0f;
      for (int k = 0; k < 3; ++k) {
        sum += ry[i][k] * g_synpan_view0[k * 4 + j];
      }
      view_out[i * 4 + j] = sum;
    }
  }
  for (int k = 0; k < 3; ++k) {
    view_out[12 + k] = -(g_synpan_c0[0] * view_out[0 * 4 + k] +
                         g_synpan_c0[1] * view_out[1 * 4 + k] +
                         g_synpan_c0[2] * view_out[2 * 4 + k]);
  }
  view_out[15] = 1.0f;
}


// ---- Frame-end capture diagnostics (hotkeys + guest-memory snapshots) ------
// Moved verbatim from the frame-boundary hook: arming (cvar/hotkey/pad/
// trigger-file/photo-trace), per-frame record collection, and the .gsnap +
// metadata writers. Runs under the hook layer's record lock.

namespace {

struct FrameRecords {
  uint64_t frame_index;
  std::vector<SubmitRecord> records;
};
std::vector<FrameRecords> g_collected_frames;
uint64_t g_collect_counter = 0;
bool g_collecting = false;
// F10 immediate mode: capture exactly one frame regardless of the snapshot
// frames/stride cvars.
bool g_immediate = false;
bool g_snapshot_written = false;

std::filesystem::path SnapshotDir() {
  std::filesystem::path dir{std::string(REXCVAR_GET(skate3_native_render_snapshot_dir))};
  if (dir.empty()) {
    dir = "native_render_snapshots";
  }
  return dir;
}

std::string SnapshotStem() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  char stem[64];
  std::snprintf(stem, sizeof(stem), "snapshot_%" PRId64, static_cast<int64_t>(seconds));
  return stem;
}

// Guest memory snapshot format (.gsnap):
//   char magic[8] = "SK3GSNP1"
//   repeated regions: u64 guest_offset (little-endian), u64 size, raw bytes
//   terminator region: guest_offset == 0xFFFFFFFFFFFFFFFF, size == 0
// guest_offset is the offset from the guest base mapping. Guest virtual
// address A maps to file region offset A for A < 0xE0000000 and A + 0x1000
// above that (REX_PHYS_HOST_OFFSET physical mirror shift).
bool WriteMemorySnapshot(uint8_t* base, const std::filesystem::path& path) {
#if defined(_WIN32)
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    REXLOG_ERROR("native-render snapshot: cannot open {}", path.string());
    return false;
  }
  out.write("SK3GSNP1", 8);

  uint64_t total_bytes = 0;
  uint32_t region_count = 0;
  uint64_t offset = 0;
  while (offset < REX_MEMORY_SIZE) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(base + offset, &info, sizeof(info)) == 0) {
      break;
    }
    const uint64_t region_size = static_cast<uint64_t>(info.RegionSize);
    const bool readable =
        info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
        (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (readable) {
      const uint64_t clamped =
          region_size > REX_MEMORY_SIZE - offset ? REX_MEMORY_SIZE - offset : region_size;
      out.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
      out.write(reinterpret_cast<const char*>(&clamped), sizeof(clamped));
      out.write(reinterpret_cast<const char*>(base + offset),
                static_cast<std::streamsize>(clamped));
      total_bytes += clamped;
      ++region_count;
    }
    offset += region_size;
  }

  const uint64_t terminator_offset = ~0ull;
  const uint64_t terminator_size = 0;
  out.write(reinterpret_cast<const char*>(&terminator_offset), sizeof(terminator_offset));
  out.write(reinterpret_cast<const char*>(&terminator_size), sizeof(terminator_size));
  out.close();
  if (!out) {
    REXLOG_ERROR("native-render snapshot: write failed for {}", path.string());
    return false;
  }
  REXLOG_INFO("native-render snapshot: {} regions, {} MiB -> {}", region_count,
              total_bytes >> 20, path.string());
  return true;
#else
  (void)base;
  REXLOG_WARN("native-render snapshot: only implemented on Windows, skipping {}",
              path.string());
  return false;
#endif
}

bool WriteMetadata(const std::filesystem::path& path,
                   const std::vector<FrameRecords>& frames) {
  std::ofstream out(path);
  if (!out) {
    REXLOG_ERROR("native-render snapshot: cannot open {}", path.string());
    return false;
  }
  out << "{\"type\":\"header\",\"image_base\":\"0x82000000\","
      << "\"phys_mirror_note\":\"guest addr A -> file offset A, plus 0x1000 for A >= "
         "0xE0000000\","
      << "\"record_fields\":[\"kind\",\"a\",\"b\",\"c\"],"
      << "\"kinds\":{\"0\":\"RenderMesh a=ctx b=vps c=dyn\",\"1\":\"SceneDrawList a=ctx "
         "b=list_offset c=view\",\"2\":\"WorldPathCapture a=ctx b=view c=dyn\","
         "\"3\":\"QuadListDraw a=key c=dyn\"}}\n";
  for (const FrameRecords& frame : frames) {
    out << "{\"type\":\"frame\",\"index\":" << frame.frame_index << ",\"records\":[";
    for (size_t i = 0; i < frame.records.size(); ++i) {
      const SubmitRecord& r = frame.records[i];
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s[%u,\"%08X\",\"%08X\",\"%08X\"]", i ? "," : "",
                    r.kind, r.a, r.b, r.c);
      out << buf;
    }
    out << "]}\n";
  }
  out.close();
  return static_cast<bool>(out);
}

void WriteSnapshotLocked(uint8_t* base) {
  const std::filesystem::path dir = SnapshotDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string stem = SnapshotStem();
  const bool meta_ok = WriteMetadata(dir / (stem + ".meta.jsonl"), g_collected_frames);
  const bool snap_ok = WriteMemorySnapshot(base, dir / (stem + ".gsnap"));
  WriteRecording(dir.string().c_str(), stem.c_str());
  REXLOG_INFO("native-render snapshot: {} ({} frames of records, meta_ok={} snap_ok={})",
              stem, g_collected_frames.size(), meta_ok, snap_ok);
  g_collected_frames.clear();
  g_snapshot_written = true;
}

}  // namespace

bool SnapshotWritten() { return g_snapshot_written; }

void OnCaptureFrameEnd(uint8_t* base, uint64_t frame_index,
                       std::vector<SubmitRecord>& current_frame) {
  const size_t mesh_count = current_frame.size();
  if (!g_snapshot_written) {
    const int32_t min_meshes = REXCVAR_GET(skate3_native_render_snapshot_min_meshes);
    if (!g_collecting && min_meshes > 0 &&
        mesh_count >= static_cast<size_t>(min_meshes)) {
      g_collecting = true;
      g_collect_counter = 0;
      StartRecording(
          uint32_t(REXCVAR_GET(skate3_native_render_snapshot_stride)));
      REXLOG_INFO("native-render snapshot: armed at frame {} ({} meshes)", frame_index,
                  mesh_count);
    }
  }
  // Manual triggers (work repeatedly): press F9 (window recording per the
  // snapshot cvars), F10 (IMMEDIATE single-frame capture: full memory
  // snapshot + this frame's records/draws, for catching a broken object the
  // moment it is on screen), or create <snapshot_dir>\trigger. All of them
  // (and their key/pad polling) sit behind skate3_native_render_capture_hotkeys.
  const bool capture_hotkeys = REXCVAR_GET(skate3_native_render_capture_hotkeys);
#if defined(_WIN32)
  if (capture_hotkeys && !g_collecting) {
    static bool f9_was_down = false;
    const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_down && !f9_was_down) {
      g_collecting = true;
      g_collect_counter = 0;
      StartRecording(
          uint32_t(REXCVAR_GET(skate3_native_render_snapshot_stride)));
      REXLOG_INFO("native-render snapshot: F9, armed at frame {} ({} meshes)",
                  frame_index, mesh_count);
    }
    f9_was_down = f9_down;
    // F8: flush the native texture + mesh caches. Debug/bisect aid, and the
    // reproducible worst-case decode burst for perf work (everything visible
    // re-decodes at once; the decode workers should absorb it with a brief
    // white/pop-in instead of a render-thread freeze).
    static bool f8_was_down = false;
    const bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8_down && !f8_was_down) {
      FlushTextureCache();
      FlushMeshCache();
      REXLOG_INFO("native-render: F8, texture + mesh caches flushed");
    }
    f8_was_down = f8_down;
    // F7: dump the rolling scene-composition ring (last ~900 frames of
    // per-item signatures); press within a few seconds of SEEING a 1-2
    // frame artifact; diffing the artifact frame against neighbors in the
    // CSV names the item that flashed.
    static bool f7_was_down = false;
    const bool f7_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7_down && !f7_was_down) {
      RequestSceneRingDump();
      REXLOG_INFO("native-render: F7, scene ring dump requested");
    }
    f7_was_down = f7_down;
    static bool f10_was_down = false;
    const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !f10_was_down) {
      g_collecting = true;
      g_immediate = true;
      g_collect_counter = 0;
      StartRecording(1);
      // Simultaneous screenshot: the recorded scene/draw data alone cannot
      // prove what was on screen (render-stage state is not captured);
      // the paired PNG anchors every diagnostic to the visible frame.
      skate3::screenshot::CaptureWindow(skate3::screenshot::RememberedWindow(),
                                        "f10");
      REXLOG_INFO("native-render snapshot: F10, immediate single-frame capture at frame {}",
                  frame_index);
    }
    f10_was_down = f10_down;
    // Xbox controller capture combos (the artifacts are too brief to reach
    // the keyboard from the pad): RB+X = F10-style immediate capture,
    // RB+A = F7 scene-ring dump (retroactive ~17 s, so a slightly late
    // press still contains the flash frame). Pad 0; disconnected pads
    // re-probe on a backoff; XInputGetState is slow for absent devices.
    {
      static bool combo_x_was = false;
      static bool combo_a_was = false;
      static uint32_t pad_retry = 0;
      static bool pad_seen = true;
      if (pad_seen || ++pad_retry >= 240) {
        pad_retry = 0;
        XINPUT_STATE xs{};
        if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
          pad_seen = true;
          const WORD b = xs.Gamepad.wButtons;
          const bool rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
          const bool cx = rb && (b & XINPUT_GAMEPAD_X) != 0;
          const bool ca = rb && (b & XINPUT_GAMEPAD_A) != 0;
          if (cx && !combo_x_was) {
            g_collecting = true;
            g_immediate = true;
            g_collect_counter = 0;
            StartRecording(1);
            skate3::screenshot::CaptureWindow(
                skate3::screenshot::RememberedWindow(), "rbx");
            REXLOG_INFO(
                "native-render snapshot: RB+X, immediate single-frame capture "
                "at frame {}",
                frame_index);
          }
          combo_x_was = cx;
          if (ca && !combo_a_was) {
            RequestSceneRingDump();
            REXLOG_INFO("native-render: RB+A, scene ring dump requested");
          }
          combo_a_was = ca;
        } else {
          pad_seen = false;
          combo_x_was = combo_a_was = false;
        }
      }
    }
  }
  // F11: paired A/B parity capture; one keypress produces
  // shot_<ts>_native.png + shot_<ts>_emulated.png (same viewpoint, ~half a
  // second apart while the renderer toggles) + an immediate F10-style
  // capture taken back in native mode. Sequenced across guest frames so
  // each renderer has settled before its screenshot (the emulated pipeline
  // needs to recompose after suppression lifts).
  {
    enum class AbState { kIdle, kNativeSettle, kEmulatedSettle, kBackToNative };
    static AbState ab_state = AbState::kIdle;
    static uint64_t ab_resume_frame = 0;
    static char ab_tag[24] = {};
    static bool f11_was_down = false;
    // Gate the poll itself; a sequence already in flight still runs to
    // completion so the renderer is never left toggled to emulated.
    const bool f11_down =
        capture_hotkeys && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    constexpr uint64_t kSettleFrames = 60;
    switch (ab_state) {
      case AbState::kIdle:
        if (f11_down && !f11_was_down && !g_collecting) {
          const std::time_t t = std::time(nullptr);
          std::tm tm{};
          localtime_s(&tm, &t);
          std::snprintf(ab_tag, sizeof(ab_tag), "%02d%02d%02d", tm.tm_hour, tm.tm_min,
                        tm.tm_sec);
          // Ensure the sequence starts in NATIVE mode (the gsnap capture at
          // the end must record native-path scene data).
          if (!Enabled()) {
            ToggleSceneEnabled();
          }
          ab_state = AbState::kNativeSettle;
          ab_resume_frame = frame_index + kSettleFrames;
          REXLOG_INFO("native-render A/B capture: F11, tag {}", ab_tag);
        }
        break;
      case AbState::kNativeSettle:
        if (frame_index >= ab_resume_frame) {
          char tag[40];
          std::snprintf(tag, sizeof(tag), "%s_native", ab_tag);
          skate3::screenshot::CaptureWindow(skate3::screenshot::RememberedWindow(), tag);
          ToggleSceneEnabled();  // -> emulated
          ab_state = AbState::kEmulatedSettle;
          ab_resume_frame = frame_index + kSettleFrames;
        }
        break;
      case AbState::kEmulatedSettle:
        if (frame_index >= ab_resume_frame) {
          char tag[40];
          std::snprintf(tag, sizeof(tag), "%s_emulated", ab_tag);
          skate3::screenshot::CaptureWindow(skate3::screenshot::RememberedWindow(), tag);
          ToggleSceneEnabled();  // -> native
          ab_state = AbState::kBackToNative;
          ab_resume_frame = frame_index + kSettleFrames;
        }
        break;
      case AbState::kBackToNative:
        if (frame_index >= ab_resume_frame && !g_collecting) {
          g_collecting = true;
          g_immediate = true;
          g_collect_counter = 0;
          StartRecording(1);
          REXLOG_INFO(
              "native-render A/B capture: screenshots tagged {} done, immediate "
              "capture armed",
              ab_tag);
          ab_state = AbState::kIdle;
        }
        break;
    }
    f11_was_down = f11_down;
  }
  // Post-effect comparison sweep (skate3_native_render_post_sweep): one
  // session, one screenshot per render state: baseline, each HDR post
  // effect alone, everything on, then the hdr_debug diagnostic views.
  // Gameplay detection reuses the auto-snapshot heuristic (a sustained
  // full-scene mesh count); each step settles before its capture so
  // hot-reload toggles and exposure have taken effect.
  {
    struct SweepStep {
      const char* tag;
      bool bloom;
      bool shafts;
      bool haze;
      int32_t debug;
    };
    static constexpr SweepStep kSweepSteps[] = {
        {"base", false, false, false, 0},
        {"bloom", true, false, false, 0},
        {"shafts", false, true, false, 0},
        {"haze", false, false, true, 0},
        {"all", true, true, true, 0},
        {"dbg_shaftplane", true, true, true, 4},
        {"dbg_hazeterm", true, true, true, 5},
    };
    enum class SweepState { kIdle, kWaitGameplay, kStep };
    static SweepState sweep_state = SweepState::kIdle;
    static uint32_t sweep_gameplay_frames = 0;
    static size_t sweep_index = 0;
    static uint64_t sweep_capture_frame = 0;
    static bool sweep_captured = false;
    static bool saved_bloom = false, saved_shafts = false, saved_haze = false;
    static int32_t saved_debug = 0;
    static char sweep_ts[24] = {};
    // Every swept cvar is a per-frame hot read (no pipeline rebuilds), so
    // settling is short. Kept to a fraction of a second per step: quick
    // enough that the captures stay frame-comparable for offline A/B
    // diffing, slow enough to follow by eye.
    constexpr uint64_t kSweepSettleFrames = 48;
    const bool sweep_armed = REXCVAR_GET(skate3_native_render_post_sweep);
    const auto sweep_restore = [&] {
      REXCVAR_SET(skate3_native_render_scene_bloom, saved_bloom);
      REXCVAR_SET(skate3_native_render_scene_shafts, saved_shafts);
      REXCVAR_SET(skate3_native_render_scene_haze, saved_haze);
      REXCVAR_SET(skate3_native_render_scene_hdr_debug, saved_debug);
      REXCVAR_SET(skate3_native_render_post_sweep, false);
      sweep_state = SweepState::kIdle;
    };
    switch (sweep_state) {
      case SweepState::kIdle:
        if (sweep_armed) {
          sweep_state = SweepState::kWaitGameplay;
          sweep_gameplay_frames = 0;
          REXLOG_INFO("native-render post sweep: armed, waiting for gameplay");
        }
        break;
      case SweepState::kWaitGameplay:
        if (!sweep_armed) {
          sweep_state = SweepState::kIdle;
          break;
        }
        // Gameplay AND the frame-global shadow rows captured: the sweep's
        // shaft states are meaningless before the shadow pass is live, and
        // right after takeover the sweep otherwise outruns its bring-up.
        sweep_gameplay_frames =
            (mesh_count >= 300 && g_shadow_have) ? sweep_gameplay_frames + 1
                                                 : 0;
        if (sweep_gameplay_frames >= 240) {
          saved_bloom = REXCVAR_GET(skate3_native_render_scene_bloom);
          saved_shafts = REXCVAR_GET(skate3_native_render_scene_shafts);
          saved_haze = REXCVAR_GET(skate3_native_render_scene_haze);
          saved_debug = REXCVAR_GET(skate3_native_render_scene_hdr_debug);
          const std::time_t t = std::time(nullptr);
          std::tm tm{};
          localtime_s(&tm, &t);
          std::snprintf(sweep_ts, sizeof(sweep_ts), "%02d%02d%02d", tm.tm_hour,
                        tm.tm_min, tm.tm_sec);
          sweep_index = 0;
          sweep_captured = false;
          sweep_capture_frame = frame_index + kSweepSettleFrames;
          REXCVAR_SET(skate3_native_render_scene_bloom, kSweepSteps[0].bloom);
          REXCVAR_SET(skate3_native_render_scene_shafts, kSweepSteps[0].shafts);
          REXCVAR_SET(skate3_native_render_scene_haze, kSweepSteps[0].haze);
          REXCVAR_SET(skate3_native_render_scene_hdr_debug, kSweepSteps[0].debug);
          sweep_state = SweepState::kStep;
          REXLOG_INFO("native-render post sweep: started, tag {} ({} steps)",
                      sweep_ts, std::size(kSweepSteps));
        }
        break;
      case SweepState::kStep:
        if (!sweep_armed) {
          sweep_restore();
          REXLOG_INFO("native-render post sweep: cancelled, cvars restored");
          break;
        }
        if (!sweep_captured && frame_index >= sweep_capture_frame) {
          char tag[48];
          std::snprintf(tag, sizeof(tag), "%s_sweep_%s", sweep_ts,
                        kSweepSteps[sweep_index].tag);
          skate3::screenshot::CaptureWindow(
              skate3::screenshot::RememberedWindow(), tag);
          REXLOG_INFO("native-render post sweep: captured {} ({}/{})", tag,
                      sweep_index + 1, std::size(kSweepSteps));
          sweep_captured = true;
          // A short hold so the (asynchronous) window grab reads this
          // state's frame before the next step's cvars apply.
          sweep_capture_frame = frame_index + 12;
          break;
        }
        if (sweep_captured && frame_index >= sweep_capture_frame) {
          if (++sweep_index >= std::size(kSweepSteps)) {
            sweep_restore();
            REXLOG_INFO(
                "native-render post sweep: done, cvars restored (tag {})",
                sweep_ts);
            break;
          }
          REXCVAR_SET(skate3_native_render_scene_bloom,
                      kSweepSteps[sweep_index].bloom);
          REXCVAR_SET(skate3_native_render_scene_shafts,
                      kSweepSteps[sweep_index].shafts);
          REXCVAR_SET(skate3_native_render_scene_haze,
                      kSweepSteps[sweep_index].haze);
          REXCVAR_SET(skate3_native_render_scene_hdr_debug,
                      kSweepSteps[sweep_index].debug);
          sweep_captured = false;
          sweep_capture_frame = frame_index + kSweepSettleFrames;
        }
        break;
    }
  }
#endif
  if (capture_hotkeys && !g_collecting && frame_index % 32 == 0) {
    const std::filesystem::path trigger = SnapshotDir() / "trigger";
    std::error_code ec;
    if (std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      g_collecting = true;
      g_collect_counter = 0;
      StartRecording(
          uint32_t(REXCVAR_GET(skate3_native_render_snapshot_stride)));
      REXLOG_INFO("native-render snapshot: trigger file, armed at frame {} ({} meshes)",
                  frame_index, mesh_count);
    }
  }
  // Photo display-card compose auto-trace: the first time the framed card
  // comes up in a session, record EVERY draw for ~360 frames + the memory
  // snapshot; the game's one-shot card compose (frame art / logo / caption
  // stamped over the photo) fires at an unpredictable moment within this
  // window, and the capture pins its draws, source textures and geometry.
  if (!g_collecting && REXCVAR_GET(skate3_native_render_photo_compose_trace)) {
    static bool s_compose_trace_done = false;
    if (!s_compose_trace_done && PhotoCardVisible()) {
      s_compose_trace_done = true;
      REXCVAR_SET(skate3_native_render_snapshot_frames, 360);
      REXCVAR_SET(skate3_native_render_snapshot_stride, 1);
      REXCVAR_SET(skate3_native_render_snapshot_all_draws, true);
      g_collecting = true;
      g_collect_counter = 0;
      StartRecording(1);
      REXLOG_INFO(
          "native-render snapshot: photo display card up - compose trace "
          "armed at frame {} (360 frames, all draws; snapshot cvars stay "
          "changed for this session)",
          frame_index);
    }
  }
  if (g_collecting) {
    // Immediate (F10) captures skip the arming frame: the scene-side
    // recording was only armed after this frame's BuildFrameScene ran, so
    // the first fully-recorded frame is the next one.
    const auto stride = g_immediate
        ? uint64_t(2)
        : static_cast<uint64_t>(REXCVAR_GET(skate3_native_render_snapshot_stride));
    if (++g_collect_counter % stride == 0) {
      g_collected_frames.push_back({frame_index, std::move(current_frame)});
      const auto wanted = g_immediate
          ? size_t(1)
          : static_cast<size_t>(REXCVAR_GET(skate3_native_render_snapshot_frames));
      if (g_collected_frames.size() >= wanted) {
        g_collecting = false;
        g_immediate = false;
        WriteSnapshotLocked(base);
      }
    }
  }
}

}  // namespace skate3::native_scene

