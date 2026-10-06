#include "native/character_fade.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace skate3::native_scene;

void TestPolicy() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (float alpha : {0.0f, 0.004f, 0.5f, 0.999f, 1.0f}) {
    assert(CharacterFadeOpacityValid(alpha));
    for (uint32_t family : {1u, 2u, 3u, 6u}) {
      assert(CharacterFadeBodyEligible(family, false));
      const auto missing_rows = ResolveCharacterBodyFade(
          family, false, alpha, 0.0f, nan, true);
      assert(missing_rows.eligible && missing_rows.opacity == alpha);
      const auto captured = ResolveCharacterBodyFade(
          family, false, -1.0f, float(family), alpha, true);
      assert(captured.eligible && captured.opacity == alpha);
      const auto authoritative = ResolveCharacterBodyFade(
          family, false, alpha, float(family), 1.0f - alpha, true);
      assert(authoritative.eligible && authoritative.opacity == alpha);
      assert(!ResolveCharacterBodyFade(
          family, false, alpha, float(family), alpha, false).eligible);
      assert(!ResolveCharacterBodyFade(
          family, true, alpha, float(family), alpha, true).eligible);
    }
  }
  for (float bad : {-1.0f, -0.001f, 1.001f, nan, inf, -inf}) {
    assert(!CharacterFadeOpacityValid(bad));
    assert(!ResolveCharacterBodyFade(2, false, bad, 2.0f, bad, true).eligible);
    const auto captured = ResolveCharacterBodyFade(2, false, bad, 2.0f, 0.5f, true);
    assert(captured.eligible && captured.opacity == 0.5f);
  }
  for (uint32_t family : {0u, 4u, 5u, 7u, 8u, 255u}) {
    assert(!CharacterFadeBodyEligible(family, false));
    assert(!ResolveCharacterBodyFade(
        family, false, 0.5f, float(family), 0.5f, true).eligible);
  }
  for (float foreign : {0.0f, 1.0f, 2.001f, 3.0f, nan, inf}) {
    assert(!ResolveCharacterBodyFade(2, false, -1.0f, foreign, 0.5f, true).eligible);
  }
  float parameters[4]{3.0f, 0.7f, 0.0f, 0.0f};
  WriteCharacterBodyFadeOverride(
      ResolveCharacterBodyFade(2, false, 0.5f, 0.0f, nan, true), parameters);
  assert(parameters[0] == 3.0f && parameters[1] == 0.7f);
  assert(parameters[2] == 0.5f && parameters[3] == 1.0f);
  WriteCharacterBodyFadeOverride(
      ResolveCharacterBodyFade(0, false, 0.5f, 0.0f, nan, true), parameters);
  assert(parameters[0] == 3.0f && parameters[1] == 0.7f);
  assert(parameters[2] == 0.0f && parameters[3] == 0.0f);  // Neutral world draw.
}

struct Surface {
  float depth;
  float color;
  CharacterFadePolicy fade;
};

// Model the renderer's visibility pass and subsequent depth-equal color
// pass. Different pieces at the same pixel must expose the nearest body
// surface once, independently of submission order and missing lighting.
float CompositeVisibleBody(const std::array<Surface, 3>& surfaces,
                           const std::array<unsigned, 3>& order,
                           float world_depth, float background) {
  float depth = world_depth;
  for (unsigned index : order) {
    const Surface& s = surfaces[index];
    if (s.fade.eligible && s.fade.opacity > 0.004f) {
      depth = std::min(depth, s.depth);
    }
  }
  float color = background;
  for (unsigned index : order) {
    const Surface& s = surfaces[index];
    if (s.fade.eligible && s.fade.opacity > 0.004f && s.depth == depth) {
      color = s.fade.opacity * s.color + (1.0f - s.fade.opacity) * color;
    }
  }
  return color;
}

void TestOverlapContract() {
  std::array<unsigned, 3> order{0, 1, 2};
  for (float alpha : {0.0f, 0.5f, 1.0f}) {
    // The near piece lacks captured lighting, but has the same owner fade.
    const std::array<Surface, 3> surfaces{{
        {0.2f, 1.0f, ResolveCharacterBodyFade(2, false, alpha, 0.0f, 0.0f, true)},
        {0.5f, 1.0f, ResolveCharacterBodyFade(2, false, alpha, 2.0f, alpha, true)},
        {0.7f, 1.0f, ResolveCharacterBodyFade(1, false, alpha, 1.0f, alpha, true)}}};
    order = {0, 1, 2};
    do {
      assert(std::fabs(CompositeVisibleBody(surfaces, order, 1.0f, 0.0f) - alpha) < 1e-6f);
      // An opaque world surface in front still fully occludes the fade.
      assert(CompositeVisibleBody(surfaces, order, 0.1f, 0.25f) == 0.25f);
    } while (std::next_permutation(order.begin(), order.end()));
  }
  // The former far-to-near color/depth pass blended the same 50% body
  // twice into 75% coverage; the expected single-surface fade is 50%.
  const float repeated_blend = 0.5f + 0.5f * 0.5f;
  assert(repeated_blend == 0.75f && repeated_blend != 0.5f);
}

void TestUniformReservation() {
  const auto empty_palette = ReserveCharacterFadeUniforms(512, 0, 1024);
  assert(empty_palette.valid && empty_palette.bone_offset == 0);
  assert(empty_palette.char_offset == 512 && empty_palette.next_offset == 1024);
  const auto aligned = ReserveCharacterFadeUniforms(513, 96, 1536);
  assert(aligned.valid && aligned.bone_offset == 768);
  assert(aligned.char_offset == 1024 && aligned.next_offset == 1536);
  const auto full_palette = ReserveCharacterFadeUniforms(512, 96 * 48, 5632);
  assert(full_palette.valid && full_palette.bone_offset == 512);
  assert(full_palette.char_offset == 5120 && full_palette.next_offset == 5632);
  for (const auto plan : {ReserveCharacterFadeUniforms(512, 0, 1023),
                          ReserveCharacterFadeUniforms(513, 96, 1535),
                          ReserveCharacterFadeUniforms(512, 96 * 48, 5631),
                          ReserveCharacterFadeUniforms(1025, 0, 1024),
                          ReserveCharacterFadeUniforms(0, 0, 511),
                          ReserveCharacterFadeUniforms(0, 0, 0),
                          ReserveCharacterFadeUniforms(0, 1025, 1024),
                          ReserveCharacterFadeUniforms(0, std::numeric_limits<uint64_t>::max(), 1024),
                          ReserveCharacterFadeUniforms(std::numeric_limits<uint32_t>::max(), 0,
                                                       std::numeric_limits<uint32_t>::max())}) {
    assert(!plan.valid && plan.bone_offset == 0 && plan.char_offset == 0 && plan.next_offset == 0);
  }
  const auto large = ReserveCharacterFadeUniforms(0xfffffc00u, 256, 0xfffffff0u);
  assert(large.valid && large.bone_offset == 0xfffffc00u);
  assert(large.char_offset == 0xfffffd00u && large.next_offset == 0xffffff00u);
  int near_piece = 0, rear_piece = 0, failed_piece = 0;
  CharacterFadeUniformCache cache;
  uint32_t cursor = 512;
  bool created = false;
  const auto& depth = cache.Reserve(&near_piece, cursor, 96, 4096, &created);
  assert(created && depth.valid && cursor == 1280);
  const auto& rear = cache.Reserve(&rear_piece, cursor, 96, 4096, &created);
  assert(created && rear.valid && cursor == 2048);
  const auto& color = cache.Reserve(&near_piece, cursor, 96, 4096, &created);
  assert(!created && &color == &depth && cursor == 2048);
  const auto& failure = cache.Reserve(&failed_piece, cursor, 4096, 4096, &created);
  assert(!created && !failure.valid && cursor == 2048);
  // A later call must preserve the same failed reservation, so it cannot
  // issue color after an earlier depth pass skipped this piece.
  const auto& repeated_failure = cache.Reserve(&failed_piece, cursor, 0, 4096, &created);
  assert(!created && &repeated_failure == &failure && !repeated_failure.valid);
  assert(cursor == 2048);
}

int main() {
  TestPolicy();
  TestOverlapContract();
  TestUniformReservation();
  std::cout << "PASS: character fade policy and overlap visibility contract\n";
}
