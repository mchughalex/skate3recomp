#include "skate3_fov.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>

#include <rex/cvar.h>

namespace {

constexpr double kDefaultFieldOfViewDegrees = 60.0;
constexpr double kFieldOfViewEpsilon = 0.001;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

std::atomic<uint32_t> g_projection_fov_override_bits{0};

float FloatFromBits(uint32_t value) {
  union {
    uint32_t u;
    float f;
  } bits{value};
  return bits.f;
}

uint32_t BitsFromFloat(float value) {
  union {
    float f;
    uint32_t u;
  } bits{value};
  return bits.u;
}

}  // namespace

void Skate3UpdateFieldOfViewOverride(double degrees) {
  if (!std::isfinite(degrees) ||
      std::abs(degrees - kDefaultFieldOfViewDegrees) <= kFieldOfViewEpsilon) {
    g_projection_fov_override_bits.store(0, std::memory_order_relaxed);
    return;
  }

  const double clamped_degrees = std::clamp(degrees, 40.0, 120.0);
  const float radians = float(clamped_degrees * kDegreesToRadians);
  if (!std::isfinite(radians) || radians <= 0.0f) {
    g_projection_fov_override_bits.store(0, std::memory_order_relaxed);
    return;
  }

  g_projection_fov_override_bits.store(BitsFromFloat(radians), std::memory_order_relaxed);
}

void Skate3InitializeFieldOfViewOverride() {
  Skate3UpdateFieldOfViewOverride(rex::cvar::Query<double>("skate3_field_of_view"));
  rex::cvar::RegisterChangeCallback(
      "skate3_field_of_view", [](std::string_view, std::string_view value) {
        double degrees = 0.0;
        if (rex::cvar::ParseDouble(value, degrees)) {
          Skate3UpdateFieldOfViewOverride(degrees);
        }
      });
}

float Skate3MaybeOverrideProjectionFovRadians(float native_radians) {
  const uint32_t override_bits = g_projection_fov_override_bits.load(std::memory_order_relaxed);
  if (!override_bits || !std::isfinite(native_radians) || native_radians < 0.5f ||
      native_radians > 2.4f) {
    return native_radians;
  }
  return FloatFromBits(override_bits);
}
