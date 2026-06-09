#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <string>
#include <string_view>
#include <mutex>

#include <rex/cvar.h>
#include <rex/graphics/ultrawide_debug.h>
#include <rex/logging.h>

inline bool Skate3UltrawideHasExpandedTargetAspect() {
  if (!rex::cvar::Query<bool>("skate3_ultrawide") ||
      !rex::cvar::Query<bool>("skate3_ultrawide_hor_plus") ||
      !rex::graphics::ultrawide_debug::IsSkate3GameplayUltrawideActive()) {
    return false;
  }

  return rex::cvar::Query<double>("skate3_ultrawide_target_aspect") >
         (16.0 / 9.0) + 0.01;
}

inline float Skate3UltrawideFloatFromU32(uint32_t value) {
  union {
    uint32_t u;
    float f;
  } bits{value};
  return bits.f;
}

inline uint32_t Skate3UltrawideU32FromFloat(float value) {
  union {
    float f;
    uint32_t u;
  } bits{value};
  return bits.u;
}

inline double Skate3UltrawideMaybeOverrideGameplayFrustumAspect(uint32_t site_id,
                                                                double native_aspect) {
  if (!Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags") ||
      !std::isfinite(native_aspect) || native_aspect <= 0.0) {
    return native_aspect;
  }

  const double forced_aspect = std::clamp(
      rex::cvar::Query<double>("skate3_ultrawide_target_aspect"), 16.0 / 9.0, 8.0);
  if (forced_aspect <= native_aspect + 0.001) {
    return native_aspect;
  }

  const uint32_t native_milli = uint32_t(std::clamp(native_aspect * 1000.0, 0.0, 100000.0));
  const uint32_t forced_milli = uint32_t(std::clamp(forced_aspect * 1000.0, 0.0, 100000.0));
  rex::graphics::ultrawide_debug::RecordScreenCallback(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kWidth, site_id, native_milli);
  rex::graphics::ultrawide_debug::RecordScreenCallbackResult(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kWidth, site_id, native_milli,
      forced_milli);
  return forced_aspect;
}

inline void Skate3UltrawideMaybeExpandMainCullRange(PPCContext& ctx, u8* base,
                                                    uint32_t min_addr,
                                                    uint32_t max_addr) {
  if (!Skate3UltrawideHasExpandedTargetAspect()) {
    return;
  }

  const double target_aspect = std::clamp(
      rex::cvar::Query<double>("skate3_ultrawide_target_aspect"), 16.0 / 9.0, 8.0);
  const float scale = float(target_aspect / (16.0 / 9.0));
  if (scale <= 1.0f) {
    return;
  }

  const float native_min = Skate3UltrawideFloatFromU32(REX_LOAD_U32(min_addr));
  const float native_max = Skate3UltrawideFloatFromU32(REX_LOAD_U32(max_addr));
  if (!std::isfinite(native_min) || !std::isfinite(native_max) ||
      native_max <= native_min) {
    return;
  }

  const float center = (native_min + native_max) * 0.5f;
  const float half_width = (native_max - native_min) * 0.5f * scale;
  const float widened_min = center - half_width;
  const float widened_max = center + half_width;
  if (!std::isfinite(widened_min) || !std::isfinite(widened_max)) {
    return;
  }

  REX_STORE_U32(min_addr, Skate3UltrawideU32FromFloat(widened_min));
  REX_STORE_U32(max_addr, Skate3UltrawideU32FromFloat(widened_max));
}

inline uint32_t Skate3UltrawideRecordScreenHeight(uint32_t site_id, uint32_t native_height) {
  rex::graphics::ultrawide_debug::RecordScreenCallback(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kHeight, site_id, native_height);
  return native_height;
}

inline uint32_t Skate3UltrawideMaybeOverrideScreenFlag(
    rex::graphics::ultrawide_debug::ScreenCallbackKind kind, uint32_t site_id,
    uint32_t native_value) {
  const bool selected =
      rex::graphics::ultrawide_debug::RecordScreenCallback(kind, site_id, native_value);
  return selected ? 1 : native_value;
}

inline uint32_t Skate3UltrawideMaybeForceMainVisibilityFlag(uint32_t site_id,
                                                            uint32_t native_value) {
  rex::graphics::ultrawide_debug::RecordScreenCallback(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kScreenFlag, site_id, native_value);
  if (!Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags")) {
    return native_value;
  }
  rex::graphics::ultrawide_debug::RecordScreenCallbackResult(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kScreenFlag, site_id, native_value, 1);
  return 1;
}

inline uint32_t Skate3UltrawideMaybeForceHighestLodState(uint32_t site_id, uint32_t object,
                                                         uint32_t native_state) {
  if (!Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags") ||
      native_state == 0) {
    return native_state;
  }

  return 0;
}

inline thread_local bool g_skate3_ultrawide_force_main_cull_classifier = false;

struct Skate3UltrawideMainCullClassifierScope {
  explicit Skate3UltrawideMainCullClassifierScope(bool enabled)
      : previous(g_skate3_ultrawide_force_main_cull_classifier) {
    g_skate3_ultrawide_force_main_cull_classifier =
        previous || (enabled && Skate3UltrawideHasExpandedTargetAspect());
  }

  ~Skate3UltrawideMainCullClassifierScope() {
    g_skate3_ultrawide_force_main_cull_classifier = previous;
  }

  bool previous;
};

inline uint32_t Skate3UltrawideMaybeForceMainCullClassifierHalfword(uint32_t native_value) {
  if (!g_skate3_ultrawide_force_main_cull_classifier ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags")) {
    return native_value;
  }
  return 1;
}

inline bool Skate3UltrawideShouldForceWorldMainCullDescriptor(u8* base, uint32_t descriptor) {
  if (!Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags") ||
      descriptor < 512) {
    return false;
  }

  constexpr uint32_t kWorldVtable = 0x8230B278;
  const uint32_t object = descriptor - 512;
  if (REX_LOAD_U32(object) != kWorldVtable) {
    return false;
  }

  const uint32_t output_flags = REX_LOAD_U32(descriptor + 12);
  return output_flags != 0 && output_flags == REX_LOAD_U32(object + 496);
}

inline bool Skate3UltrawideShouldWidenGameCullFrustum(u8* base, uint32_t cull_block) {
  if (!Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_widen_game_frustum") || !cull_block) {
    return false;
  }

  return REX_LOAD_U32(cull_block + 5260) != 0;
}

struct Skate3UltrawideGameFrustumPatchScope {
  Skate3UltrawideGameFrustumPatchScope(PPCContext& ctx_, u8* base_, uint32_t cull_block)
      : ctx(ctx_), base(base_) {
    if (!Skate3UltrawideShouldWidenGameCullFrustum(base, cull_block)) {
      return;
    }

    const double target_aspect = std::clamp(
        rex::cvar::Query<double>("skate3_ultrawide_target_aspect"), 16.0 / 9.0, 8.0);
    side_scale = float((16.0 / 9.0) / target_aspect);
    if (!(side_scale > 0.0f && side_scale < 0.999f)) {
      return;
    }

    block = cull_block;
    PlaneBits current_left_bits = LoadPlaneBits(kLeftPlaneOffset);
    PlaneBits current_right_bits = LoadPlaneBits(kRightPlaneOffset);

    WidenedPlaneCache& cache = CacheForBlock();
    if (cache.valid && cache.side_scale == side_scale &&
        current_left_bits == cache.widened[0] && current_right_bits == cache.widened[1]) {
      return;
    }

    cache.original[0] = current_left_bits;
    cache.original[1] = current_right_bits;

    Plane left = LoadPlane(kLeftPlaneOffset);
    Plane right = LoadPlane(kRightPlaneOffset);
    Plane widened_left{};
    Plane widened_right{};
    for (size_t i = 0; i < left.size(); ++i) {
      const float center = (left[i] + right[i]) * 0.5f;
      const float side = (left[i] - right[i]) * 0.5f * side_scale;
      widened_left[i] = center + side;
      widened_right[i] = center - side;
    }

    if (!NormalizePlane(widened_left) || !NormalizePlane(widened_right)) {
      return;
    }

    StorePlane(kLeftPlaneOffset, widened_left);
    StorePlane(kRightPlaneOffset, widened_right);
    cache.widened[0] = LoadPlaneBits(kLeftPlaneOffset);
    cache.widened[1] = LoadPlaneBits(kRightPlaneOffset);
    cache.side_scale = side_scale;
    cache.valid = true;
    active = true;
  }

  ~Skate3UltrawideGameFrustumPatchScope() = default;

  Skate3UltrawideGameFrustumPatchScope(const Skate3UltrawideGameFrustumPatchScope&) = delete;
  Skate3UltrawideGameFrustumPatchScope& operator=(const Skate3UltrawideGameFrustumPatchScope&) =
      delete;

 private:
  using Plane = std::array<float, 4>;
  using PlaneBits = std::array<uint32_t, 4>;

  struct WidenedPlaneCache {
    uint32_t block = 0;
    std::array<PlaneBits, 2> original{};
    std::array<PlaneBits, 2> widened{};
    float side_scale = 1.0f;
    bool valid = false;
  };

  static constexpr uint32_t kLeftPlaneOffset = 5152;
  static constexpr uint32_t kRightPlaneOffset = 5168;

  float LoadFloat(uint32_t address) const {
    PPCRegister value{};
    value.u32 = REX_LOAD_U32(address);
    return value.f32;
  }

  void StoreFloat(uint32_t address, float number) const {
    PPCRegister value{};
    value.f32 = number;
    REX_STORE_U32(address, value.u32);
  }

  Plane LoadPlane(uint32_t offset) const {
    Plane plane{};
    for (uint32_t i = 0; i < plane.size(); ++i) {
      plane[i] = LoadFloat(block + offset + i * 4);
    }
    return plane;
  }

  void StorePlane(uint32_t offset, const Plane& plane) const {
    for (uint32_t i = 0; i < plane.size(); ++i) {
      StoreFloat(block + offset + i * 4, plane[i]);
    }
  }

  PlaneBits LoadPlaneBits(uint32_t offset) const {
    PlaneBits plane{};
    for (uint32_t i = 0; i < plane.size(); ++i) {
      plane[i] = REX_LOAD_U32(block + offset + i * 4);
    }
    return plane;
  }

  WidenedPlaneCache& CacheForBlock() {
    static thread_local std::array<WidenedPlaneCache, 16> cache_entries{};
    WidenedPlaneCache* empty = nullptr;
    for (WidenedPlaneCache& entry : cache_entries) {
      if (entry.valid && entry.block == block) {
        return entry;
      }
      if (!entry.valid && !empty) {
        empty = &entry;
      }
    }

    WidenedPlaneCache& entry = empty ? *empty : cache_entries[block % cache_entries.size()];
    entry = {};
    entry.block = block;
    return entry;
  }

  static bool NormalizePlane(Plane& plane) {
    const float length =
        std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (!(length > 0.00001f) || !std::isfinite(length)) {
      return false;
    }

    const float inv_length = 1.0f / length;
    for (float& value : plane) {
      value *= inv_length;
      if (!std::isfinite(value)) {
        return false;
      }
    }
    return true;
  }

  uint32_t block = 0;
  PPCContext& ctx;
  u8* base = nullptr;
  float side_scale = 1.0f;
  bool active = false;
};

inline std::filesystem::path Skate3UltrawideCacheLogPath(const char* filename);

enum Skate3UltrawideObjectStageBits : uint32_t {
  kSkate3ObjectStageActive = 1u << 0,
  kSkate3ObjectStagePrep = 1u << 1,
  kSkate3ObjectStageList = 1u << 2,
  kSkate3ObjectStageMain = 1u << 3,
};

struct Skate3UltrawideObjectStageRecord {
  uint32_t bits = 0;
  uint32_t vtable = 0;
  uint32_t target = 0;
  uint32_t first_site = 0;
  uint32_t last_site = 0;
  uint32_t aux0 = 0;
  uint32_t aux1 = 0;
};

inline uint64_t& Skate3UltrawideObjectStageFrameId() {
  static uint64_t frame = 0;
  return frame;
}

inline uint32_t& Skate3UltrawideObjectStageRenderer() {
  static uint32_t renderer = 0;
  return renderer;
}

inline std::unordered_map<uint32_t, Skate3UltrawideObjectStageRecord>&
Skate3UltrawideObjectStageRecords() {
  static std::unordered_map<uint32_t, Skate3UltrawideObjectStageRecord> records;
  return records;
}

inline std::mutex& Skate3UltrawideObjectStageMutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::ofstream& Skate3UltrawideObjectStageLogFile() {
  static std::ofstream log_file;
  if (!log_file.is_open()) {
    std::filesystem::path log_path =
        Skate3UltrawideCacheLogPath("skate3_ultrawide_object_stage_trace.log");
    std::error_code ec;
    if (log_path.has_parent_path()) {
      std::filesystem::create_directories(log_path.parent_path(), ec);
    }
    log_file.open(log_path, std::ios::out | std::ios::trunc);
    if (log_file.is_open()) {
      log_file << "seq,frame,kind,marker,renderer,object,bits,site,vtable,target,aux0,aux1,"
                  "active,prep,list,main,missing_main,total\n";
    }
  }
  return log_file;
}

inline bool Skate3UltrawideObjectStageTraceEnabled() {
  return rex::cvar::Query<bool>("skate3_ultrawide_object_stage_trace_continuous") ||
         rex::cvar::Query<int32_t>("skate3_ultrawide_object_stage_trace_frames_remaining") > 0 ||
         rex::cvar::Query<int32_t>("skate3_ultrawide_object_stage_marker") != 0;
}

inline void Skate3UltrawideWriteObjectStageLog(
    uint64_t frame, const char* kind, uint32_t marker, uint32_t renderer, uint32_t object,
    uint32_t bits, uint32_t site, uint32_t vtable, uint32_t target, uint32_t aux0,
    uint32_t aux1, uint32_t active_count, uint32_t prep_count, uint32_t list_count,
    uint32_t main_count, uint32_t missing_main_count, uint32_t total_count) {
  if (!rex::cvar::Query<bool>("skate3_ultrawide_object_stage_trace_to_disk")) {
    return;
  }

  static std::atomic<uint64_t> sequence{0};
  std::ofstream& log_file = Skate3UltrawideObjectStageLogFile();
  if (!log_file.is_open()) {
    return;
  }

  log_file << std::dec << sequence.fetch_add(1, std::memory_order_relaxed) << ',' << frame
           << ',' << kind << ',' << marker << ",0x" << std::hex << renderer << ",0x"
           << object << ",0x" << bits << ",0x" << site << ",0x" << vtable << ",0x"
           << target << ",0x" << aux0 << ",0x" << aux1 << ',' << std::dec << active_count
           << ',' << prep_count << ',' << list_count << ',' << main_count << ','
           << missing_main_count << ',' << total_count << '\n';
  log_file.flush();
}

inline void Skate3UltrawideFinalizeObjectStageFrameLocked(const char* reason) {
  auto& records = Skate3UltrawideObjectStageRecords();
  if (records.empty()) {
    return;
  }

  const uint32_t marker =
      static_cast<uint32_t>(rex::cvar::Query<int32_t>("skate3_ultrawide_object_stage_marker"));
  const bool enabled = Skate3UltrawideObjectStageTraceEnabled();
  uint32_t remaining =
      static_cast<uint32_t>(std::max(0, rex::cvar::Query<int32_t>(
                                            "skate3_ultrawide_object_stage_trace_frames_remaining")));
  if (!enabled) {
    records.clear();
    return;
  }

  uint32_t active_count = 0;
  uint32_t prep_count = 0;
  uint32_t list_count = 0;
  uint32_t main_count = 0;
  uint32_t missing_main_count = 0;
  for (const auto& [object, record] : records) {
    active_count += (record.bits & kSkate3ObjectStageActive) ? 1 : 0;
    prep_count += (record.bits & kSkate3ObjectStagePrep) ? 1 : 0;
    list_count += (record.bits & kSkate3ObjectStageList) ? 1 : 0;
    main_count += (record.bits & kSkate3ObjectStageMain) ? 1 : 0;
    if ((record.bits & (kSkate3ObjectStagePrep | kSkate3ObjectStageList)) &&
        !(record.bits & kSkate3ObjectStageMain)) {
      ++missing_main_count;
    }
  }

  const uint64_t frame = Skate3UltrawideObjectStageFrameId();
  const uint32_t renderer = Skate3UltrawideObjectStageRenderer();
  Skate3UltrawideWriteObjectStageLog(frame, reason, marker, renderer, 0, 0, 0, 0, 0, 0, 0,
                                     active_count, prep_count, list_count, main_count,
                                     missing_main_count, static_cast<uint32_t>(records.size()));

  uint32_t emitted_missing = 0;
  const uint32_t missing_limit = static_cast<uint32_t>(std::clamp(
      rex::cvar::Query<int32_t>("skate3_ultrawide_object_stage_missing_limit"), 0, 4096));
  for (const auto& [object, record] : records) {
    if (!((record.bits & (kSkate3ObjectStagePrep | kSkate3ObjectStageList)) &&
          !(record.bits & kSkate3ObjectStageMain))) {
      continue;
    }
    if (emitted_missing++ >= missing_limit) {
      break;
    }
    Skate3UltrawideWriteObjectStageLog(frame, "missing-main", marker, renderer, object,
                                       record.bits, record.last_site, record.vtable,
                                       record.target, record.aux0, record.aux1, active_count,
                                       prep_count, list_count, main_count, missing_main_count,
                                       static_cast<uint32_t>(records.size()));
  }

  if (remaining > 0) {
    rex::cvar::SetFlagByName("skate3_ultrawide_object_stage_trace_frames_remaining",
                             std::to_string(remaining - 1));
  }
  if (marker != 0) {
    rex::cvar::SetFlagByName("skate3_ultrawide_object_stage_marker", "0");
  }
  records.clear();
}

inline void Skate3UltrawideBeginObjectStageFrame(uint32_t site_id, uint32_t renderer) {
  if (!Skate3UltrawideHasExpandedTargetAspect()) {
    return;
  }

  if (renderer < 0x10000) {
    return;
  }

  std::lock_guard lock(Skate3UltrawideObjectStageMutex());
  Skate3UltrawideFinalizeObjectStageFrameLocked("frame");
  ++Skate3UltrawideObjectStageFrameId();
  Skate3UltrawideObjectStageRenderer() = renderer;
  if (Skate3UltrawideObjectStageTraceEnabled()) {
    Skate3UltrawideWriteObjectStageLog(Skate3UltrawideObjectStageFrameId(), "begin", 0,
                                       renderer, 0, 0, site_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  }
}

inline uint32_t Skate3UltrawideObjectStageBitForAction(std::string_view action) {
  if (action == "render-active-v68") {
    return kSkate3ObjectStageActive;
  }
  if (action == "render-prep-v48") {
    return kSkate3ObjectStagePrep;
  }
  if (action == "render-list-v24") {
    return kSkate3ObjectStageList;
  }
  if (action == "render-main-v52") {
    return kSkate3ObjectStageMain;
  }
  return 0;
}

inline void Skate3UltrawideRecordObjectStage(const char* action, uint32_t site_id,
                                             uint32_t object, uint32_t vtable,
                                             uint32_t target, uint32_t aux0,
                                             uint32_t aux1, uint32_t renderer) {
  const uint32_t bit = Skate3UltrawideObjectStageBitForAction(action ? action : "");
  if (!bit || !object || !Skate3UltrawideHasExpandedTargetAspect()) {
    return;
  }

  std::lock_guard lock(Skate3UltrawideObjectStageMutex());
  auto& record = Skate3UltrawideObjectStageRecords()[object];
  if (!record.bits) {
    record.first_site = site_id;
    record.vtable = vtable;
    record.target = target;
  }
  record.bits |= bit;
  record.last_site = site_id;
  record.aux0 = aux0;
  record.aux1 = aux1;
  if (vtable) {
    record.vtable = vtable;
  }
  if (target) {
    record.target = target;
  }
  if (renderer >= 0x10000) {
    Skate3UltrawideObjectStageRenderer() = renderer;
  }
}

inline uint32_t Skate3UltrawideMaybeForceMainObjectActive(uint32_t site_id,
                                                          uint32_t native_value) {
  rex::graphics::ultrawide_debug::RecordScreenCallback(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kScreenFlag, site_id, native_value);
  if (!Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags")) {
    return native_value;
  }
  rex::graphics::ultrawide_debug::RecordScreenCallbackResult(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kScreenFlag, site_id, native_value, 1);
  return 1;
}

inline uint32_t Skate3UltrawideMaybeForceWorldPartVisible(uint32_t site_id,
                                                          uint32_t native_value,
                                                          uint32_t sibling_value) {
  rex::graphics::ultrawide_debug::RecordScreenCallback(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kScreenFlag, site_id, native_value);
  if (native_value != 0 || sibling_value == 0 || !Skate3UltrawideHasExpandedTargetAspect() ||
      !rex::cvar::Query<bool>("skate3_ultrawide_force_main_visibility_flags")) {
    return native_value;
  }

  rex::graphics::ultrawide_debug::RecordScreenCallbackResult(
      rex::graphics::ultrawide_debug::ScreenCallbackKind::kScreenFlag, site_id, native_value, 1);
  return 1;
}

inline std::filesystem::path Skate3UltrawideCacheLogPath(const char* filename) {
#if defined(_WIN32)
  char* appdata_raw = nullptr;
  size_t appdata_length = 0;
  if (_dupenv_s(&appdata_raw, &appdata_length, "APPDATA") == 0 && appdata_raw &&
      appdata_length > 0) {
    std::filesystem::path appdata_path(appdata_raw);
    std::free(appdata_raw);
    return appdata_path / "skate3" / "cache" / filename;
  }
  std::free(appdata_raw);
#else
  if (const char* appdata = std::getenv("APPDATA")) {
    return std::filesystem::path(appdata) / "skate3" / "cache" / filename;
  }
#endif
  return filename;
}

inline void Skate3UltrawideTraceRenderVirtual(const char* action, uint32_t site_id,
                                              uint32_t object_ptr, uint32_t vtable_ptr,
                                              uint32_t target_ptr, uint32_t aux0 = 0,
                                              uint32_t aux1 = 0, uint32_t renderer = 0) {
  Skate3UltrawideRecordObjectStage(action, site_id, object_ptr, vtable_ptr, target_ptr, aux0,
                                   aux1, renderer);
}
