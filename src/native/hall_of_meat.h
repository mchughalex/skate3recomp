#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace skate3::native_scene {

// Identify the actual pixel shader, not the material's generic character
// family or a substring in a debug-directory name. The shader-set hook's
// vertex/pixel labels are swapped, so callers check both tracked objects.
inline bool IsHallOfMeatPixelShader(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  const auto leaf = path.substr(slash == path.npos ? 0 : slash + 1);
  return leaf == "defaulthom_defaultPS.updb";
}

// PS c1..c4, followed by VS c5/c6. Unused material lanes may contain
// uninitialized guest values; validate only lanes consumed by the shader.
inline bool HallOfMeatRowsValid(const float rows[24]) {
  constexpr unsigned used[] = {1, 7, 8, 9, 10, 12, 13,
                               16, 17, 18, 20, 21, 22, 23};
  for (unsigned i : used) {
    if (!std::isfinite(rows[i])) return false;
  }
  return rows[7] >= 0.0f && rows[18] >= 0.0f;
}

// The exact HoM shader samples tf3. A missing or non-texture fetch must not
// let a previously captured generic character texture become its diffuse.
inline bool HallOfMeatFetchValid(const uint32_t fetch[6]) {
  return (fetch[0] & 3u) == 2u && (fetch[1] >> 12) != 0;
}

// Keep the overlay's own shader state when pass arbitration chooses a
// fuller geometry list. A different instance, mesh, or resolved skinning
// mode must never inherit this state, and shadow-bank copies cannot supply it.
template <typename Item>
inline bool GraftHallOfMeatState(Item& geometry, const Item& state) {
  if (!state.hall_of_meat || state.caster_bank || state.ctx == 0 ||
      geometry.ctx != state.ctx || geometry.mesh != state.mesh ||
      geometry.vb_obj != state.vb_obj || geometry.ib_obj != state.ib_obj ||
      geometry.stride != state.stride ||
      geometry.skinned != state.skinned || geometry.ropa != state.ropa) {
    return false;
  }
  geometry.hall_of_meat = true;
  geometry.char_family = 0;
  std::memcpy(geometry.hom_rows, state.hom_rows, sizeof(geometry.hom_rows));
  std::memcpy(geometry.diffuse_fetch, state.diffuse_fetch,
              sizeof(geometry.diffuse_fetch));
  geometry.bones = state.bones;
  std::memcpy(geometry.world, state.world, sizeof(geometry.world));
  geometry.caster_bank = false;
  return true;
}

}  // namespace skate3::native_scene
