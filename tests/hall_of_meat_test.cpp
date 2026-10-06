#include "native/hall_of_meat.h"

#include <cassert>
#include <limits>
#include <iostream>
#include <vector>

using skate3::native_scene::HallOfMeatRowsValid;
using skate3::native_scene::HallOfMeatFetchValid;
using skate3::native_scene::IsHallOfMeatPixelShader;
using skate3::native_scene::GraftHallOfMeatState;

// The capture merge chooses geometry independently of shading state.
// Test the real graft helper with a small draw-state fixture.
struct Item {
  bool hall_of_meat = false, caster_bank = false, skinned = true, ropa = false;
  unsigned ctx = 1, mesh = 2, char_family = 2;
  unsigned vb_obj = 3, ib_obj = 4, stride = 56;
  unsigned diffuse_fetch[6] = {};
  float hom_rows[24] = {}, world[16] = {};
  std::vector<float> bones;
  unsigned geometry_indices = 0;
};

void TestMerge() {
  Item hom;
  hom.hall_of_meat = true;
  hom.hom_rows[8] = 1.0f;
  hom.diffuse_fetch[1] = 123;
  hom.bones = {4.0f, 5.0f};
  hom.world[12] = 6.0f;
  Item generic;
  generic.geometry_indices = 99;
  assert(GraftHallOfMeatState(generic, hom));  // generic first, HoM later
  assert(generic.hall_of_meat && generic.char_family == 0);
  assert(generic.geometry_indices == 99);  // fuller geometry is preserved
  assert(generic.hom_rows[8] == 1.0f && generic.diffuse_fetch[1] == 123);
  assert(generic.bones == hom.bones && generic.world[12] == 6.0f);
  Item later_generic;
  later_generic.geometry_indices = 111;
  assert(GraftHallOfMeatState(later_generic, generic));  // HoM first, fuller generic later
  assert(later_generic.hall_of_meat && later_generic.geometry_indices == 111);
  assert(later_generic.diffuse_fetch[1] == 123 && later_generic.bones == hom.bones);
  const auto refused = [&](Item source) {
    Item target;
    assert(!GraftHallOfMeatState(target, source));
    assert(!target.hall_of_meat && target.char_family == 2);
  };
  Item wrong = hom;
  wrong.hall_of_meat = false;
  refused(wrong);
  wrong = hom; wrong.caster_bank = true; refused(wrong);
  wrong = hom; wrong.ctx = 0; refused(wrong);
  wrong = hom; wrong.ctx = 3; refused(wrong);
  wrong = hom; wrong.mesh = 3; refused(wrong);
  wrong = hom; wrong.vb_obj = 5; refused(wrong);
  wrong = hom; wrong.ib_obj = 5; refused(wrong);
  wrong = hom; wrong.stride = 32; refused(wrong);
  wrong = hom; wrong.skinned = false; refused(wrong);
  wrong = hom; wrong.ropa = true; refused(wrong);
}

int main() {
  TestMerge();
  uint32_t fetch[6] = {0x84000002, 0x1D840054, 0, 0, 0, 0};
  assert(HallOfMeatFetchValid(fetch));
  fetch[1] = 0;
  assert(!HallOfMeatFetchValid(fetch));
  fetch[1] = 0x54;  // format/flags alone do not supply a texture address
  assert(!HallOfMeatFetchValid(fetch));
  fetch[1] = 0x1D840054;
  for (uint32_t type : {0u, 1u, 3u}) {
    fetch[0] = 0x84000000 | type;
    assert(!HallOfMeatFetchValid(fetch));
  }
  assert(IsHallOfMeatPixelShader("D:\\build\\shaders\\defaulthom_defaultPS.updb"));
  assert(IsHallOfMeatPixelShader("/build/shaders/defaulthom_defaultPS.updb"));
  assert(IsHallOfMeatPixelShader("defaulthom_defaultPS.updb"));
  assert(!IsHallOfMeatPixelShader("defaulthom_defaultVS.updb"));
  assert(!IsHallOfMeatPixelShader("cacstamp_skin_defaultPS.updb"));
  assert(!IsHallOfMeatPixelShader("/defaulthom_defaultPS.updb/cacstamp_defaultPS.updb"));
  assert(!IsHallOfMeatPixelShader("prefix_defaulthom_defaultPS.updb"));
  assert(!IsHallOfMeatPixelShader("defaulthom_defaultPS.updb.stale"));
  assert(!IsHallOfMeatPixelShader(""));
  float rows[24] = {};
  rows[1] = 1.0f;  // material multiplier
  rows[7] = 2.0f;  // fresnel exponent
  rows[8] = rows[9] = rows[10] = 1.0f;  // dislocated white
  rows[12] = rows[13] = rows[18] = 1.0f;
  assert(HallOfMeatRowsValid(rows));
  rows[9] = rows[10] = 0.0f;  // broken red: preserve the guest color
  assert(HallOfMeatRowsValid(rows));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  constexpr unsigned unused[] = {0, 2, 3, 4, 5, 6, 11, 14, 15, 19};
  for (unsigned i : unused) rows[i] = nan;
  assert(HallOfMeatRowsValid(rows));  // unwritten guest lanes are harmless
  constexpr unsigned used[] = {1, 7, 8, 9, 10, 12, 13, 16, 17, 18, 20, 21, 22, 23};
  for (unsigned i : used) {
    const float saved = rows[i];
    rows[i] = nan;
    assert(!HallOfMeatRowsValid(rows));
    rows[i] = std::numeric_limits<float>::infinity();
    assert(!HallOfMeatRowsValid(rows));
    rows[i] = saved;
  }
  rows[7] = -1.0f;
  assert(!HallOfMeatRowsValid(rows));
  std::cout << "PASS: exact HoM shader, capture validation, and same-instance merge scope\n";
}
