#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

namespace skate3::native_scene {

enum class ScreenFxVariant : uint8_t { kNone, kOpaque, kAlpha };
enum class ScreenFxVertex : uint8_t { kNone, kRaw, kScaled };

// Retail D3D constructors copy the compiled metadata after different
// object headers: VS type 6 has 872 bytes; PS type 7 has 40 bytes. Both
// compiler debug names begin 44 bytes into that metadata.
inline constexpr uint32_t kScreenFxVsHeaderOffset = 0x368;
inline constexpr uint32_t kScreenFxPsHeaderOffset = 0x28;
inline constexpr uint32_t kScreenFxVsPathOffset = 0x394;
inline constexpr uint32_t kScreenFxPsPathOffset = 0x54;

inline bool ScreenFxShaderObjectHeadersValid(uint32_t vertex_type,
                                             uint32_t vertex_header,
                                             uint32_t pixel_type,
                                             uint32_t pixel_header) {
  // sub_82B85770 patches the VS fetch instructions for the active vertex
  // declaration, then ORs bit 0x40 into metadata[0]. This bit changes no
  // shader identity; preserve all other stage/version checks.
  return vertex_type == 6 && (vertex_header & ~0x40u) == 0x102a1101 &&
         pixel_type == 7 && pixel_header == 0x102a1100;
}

inline std::string_view ScreenFxShaderLeaf(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  return path.substr(slash == path.npos ? 0 : slash + 1);
}

// Shader objects are recycled while streaming. Callers re-read the paths
// at the draw; object addresses alone must never cache these identities.
inline ScreenFxVariant ClassifyScreenFxPixelShader(std::string_view path) {
  const auto leaf = ScreenFxShaderLeaf(path);
  if (leaf == "postfx_basictex_fisheye_opaquePS.updb") {
    return ScreenFxVariant::kOpaque;
  }
  if (leaf == "postfx_basictex_fisheyePS.updb") {
    return ScreenFxVariant::kAlpha;
  }
  return ScreenFxVariant::kNone;
}

inline ScreenFxVertex ClassifyScreenFxVertexShader(std::string_view path) {
  const auto leaf = ScreenFxShaderLeaf(path);
  if (leaf == "postfx_quadTransformVS.updb") return ScreenFxVertex::kScaled;
  if (leaf == "postfx_defaultVS.updb") return ScreenFxVertex::kRaw;
  return ScreenFxVertex::kNone;
}

struct ScreenFxState {
  ScreenFxVariant variant = ScreenFxVariant::kNone;
  bool scaled_uv = false;
  uint64_t generation = 0;
  float ps[3][4] = {};
  float vs[4][4] = {};
  uint32_t fetch[3][6] = {};
  // First three words of Xenos bank 0x2200 (device+0x2934), including blend control,
  // plus RB_COLOR_MASK (device+0x28DC). Preserve the draw's own state.
  uint32_t render_states[3] = {};
  uint32_t color_mask = 0;
};

inline bool ScreenFxTextureFetchValid(const uint32_t fetch[6]) {
  return (fetch[0] & 3u) == 2u && (fetch[1] >> 12) != 0;
}

// Xenos BaseMap mip filtering samples the declared minimum level, even
// when the native texture has generated mip levels. Other mip filters
// retain implicit derivatives/LOD; -1 tells the shader to use Sample.
inline float ScreenFxBaseMapLod(const uint32_t fetch[6]) {
  if (((fetch[3] >> 23) & 3u) != 2u) return -1.0f;
  // Match the SDK's subresource normalization: without a physical mip
  // page the guest has only level zero, regardless of raw min/max fields.
  if (((fetch[5] >> 12) & 0x1ffffu) == 0) return 0.0f;
  return float((fetch[4] >> 2) & 15u);
}

inline bool ScreenFxCaptureValid(const ScreenFxState& state) {
  if (state.variant != ScreenFxVariant::kOpaque &&
      state.variant != ScreenFxVariant::kAlpha) {
    return false;
  }
  // Validate only lanes the retail shaders consume. Unwritten lanes in
  // the guest staging banks can contain arbitrary non-finite values.
  constexpr unsigned lens_lanes[] = {0, 1, 3, 4, 5, 6, 9};
  for (unsigned lane : lens_lanes) {
    if (!std::isfinite(state.ps[lane / 4][lane % 4])) return false;
  }
  if (state.scaled_uv) {
    if (!std::isfinite(state.vs[2][0]) || !std::isfinite(state.vs[2][1]) ||
        !std::isfinite(state.vs[3][0]) || !std::isfinite(state.vs[3][1]) ||
        state.vs[3][0] == 0.0f || state.vs[3][1] == 0.0f) {
      return false;
    }
  }
  if (!ScreenFxTextureFetchValid(state.fetch[0]) ||
      !ScreenFxTextureFetchValid(state.fetch[2])) {
    return false;
  }
  return true;
}

// Every guest frame consumes and clears its own capture, including frames
// without a world submission. The next frame cannot inherit an old lens
// after the lens effect has ended.
inline ScreenFxState TakeScreenFxCapture(ScreenFxState& pending,
                                         uint64_t generation) {
  ScreenFxState current;
  if (ScreenFxCaptureValid(pending)) current = pending;
  current.generation = generation;
  pending = {};
  return current;
}

}  // namespace skate3::native_scene
