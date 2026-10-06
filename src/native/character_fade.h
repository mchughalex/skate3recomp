#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace skate3::native_scene {

inline bool CharacterFadeOpacityValid(float opacity) {
  return std::isfinite(opacity) && opacity >= 0.0f && opacity <= 1.0f;
}

// These retail body shaders export the entity opacity directly. Hair,
// lenses and vehicle glass compose opacity with material coverage instead.
inline bool CharacterFadeBodyEligible(uint32_t family, bool alpha_accessory) {
  return !alpha_accessory &&
         (family == 1 || family == 2 || family == 3 || family == 6);
}

struct CharacterFadePolicy {
  bool eligible = false;
  float opacity = 1.0f;
};

inline CharacterFadePolicy ResolveCharacterBodyFade(
    uint32_t family, bool alpha_accessory, float authoritative_opacity,
    float captured_family, float captured_opacity, bool enabled) {
  if (!enabled || !CharacterFadeBodyEligible(family, alpha_accessory)) return {};
  // The entity field remains authoritative even when lighting rows failed
  // validation. Requiring those rows here leaves individual body pieces solid.
  if (CharacterFadeOpacityValid(authoritative_opacity)) {
    return {true, authoritative_opacity};
  }
  if (captured_family == float(family) &&
      CharacterFadeOpacityValid(captured_opacity)) {
    return {true, captured_opacity};
  }
  return {};
}

// zw carry the independent body fade; xy are reserved.
// Clearing the flag is essential when the next draw is a world material.
inline void WriteCharacterBodyFadeOverride(const CharacterFadePolicy& fade,
                                           float parameters[4]) {
  parameters[2] = fade.eligible ? fade.opacity : 0.0f;
  parameters[3] = fade.eligible ? 1.0f : 0.0f;
}

struct CharacterFadeUniformReservation {
  bool valid = false;
  uint32_t bone_offset = 0;
  uint32_t char_offset = 0;
  uint32_t next_offset = 0;
};

// Reserve both passes' palette and character CB together. No cursor change
// is published when either allocation cannot fit; uint64_t arithmetic also
// rejects oversized palettes before alignment or addition can overflow.
inline CharacterFadeUniformReservation ReserveCharacterFadeUniforms(
    uint32_t cursor, uint64_t palette_bytes, uint32_t capacity) {
  if (cursor > capacity || palette_bytes > capacity) return {};
  constexpr uint64_t alignment = 256;
  constexpr uint64_t char_bytes = 512;
  const uint64_t bone_offset = (uint64_t(cursor) + alignment - 1) & ~(alignment - 1);
  const uint64_t char_offset =
      (bone_offset + palette_bytes + alignment - 1) & ~(alignment - 1);
  const uint64_t next_offset = char_offset + char_bytes;
  if (next_offset > capacity) return {};
  return {true, palette_bytes != 0 ? uint32_t(bone_offset) : 0u,
          uint32_t(char_offset), uint32_t(next_offset)};
}

// One cache per rendered frame. Stable item identity keeps depth and color
// on the same reservation, including a remembered failure to reserve both.
class CharacterFadeUniformCache {
 public:
  const CharacterFadeUniformReservation& Reserve(
      const void* item, uint32_t& cursor, uint64_t palette_bytes,
      uint32_t capacity, bool* newly_reserved = nullptr) {
    auto [it, inserted] = reservations_.try_emplace(item);
    if (inserted) {
      it->second = ReserveCharacterFadeUniforms(cursor, palette_bytes, capacity);
      if (it->second.valid) cursor = it->second.next_offset;
    }
    if (newly_reserved != nullptr) *newly_reserved = inserted && it->second.valid;
    return it->second;
  }

 private:
  std::unordered_map<const void*, CharacterFadeUniformReservation> reservations_;
};

}  // namespace skate3::native_scene
