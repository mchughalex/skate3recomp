// In-world neon spline shader (waypoint arrows / marker beams). The guest
// B-spline VS is evaluated on the CPU at publish time into WORLD-space
// vertices; the VS here projects them with the scene's (smoothed) view_proj
// rows so the signs stay glued to the world under camera re-timing. The two
// PS variants transcribe the game's own spline.fx (Skate-Shaders repo):
// "default" = additive glow with the squared-gamma trick, "darken" =
// straight-alpha backdrop dimming. The gradient texture uses the wrap/aniso
// sampler like the original i_diffuse (U runs 0..N along the band).
cbuffer C : register(b0) {
  float4 vp0;  // scene view_proj rows (row-vector: clip = x*vp0+y*vp1+z*vp2+w*vp3)
  float4 vp1;
  float4 vp2;
  float4 vp3;
  float4 intensity;  // x/y = i_intensity as staged (default/darken gain).
                     // zw = showcase blackout gate, staged CPU-side over the
                     // guest row's unused lanes: z = split x (output pixels),
                     // w = left|right visibility bits (3 = fully visible).
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float fade : TEXCOORD1;
};
VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0, float fade : TEXCOORD1) {
  VSOut o;
  o.pos = p.x * vp0 + p.y * vp1 + p.z * vp2 + p.w * vp3;
  o.uv = uv;
  o.fade = fade;
  return o;
}
// Exact transcription of the Skate 3 spline pixel shaders (disassembled
// ucode, NOT the older Skate 2 spline.fx source: EA added in-shader sqrt
// gamma compensation). default: oC0.rgb = sqrt(rgb^2 * i.x * fade / a);
// darken: oC0.rgb = sqrt(rgb^2 * i.y), oC0.a = sqrt(a * i.y * fade).
// Getting this wrong is visible: a linear 1/a over-brightens the low-alpha
// glow fringe (fuzzy, blown-out edges) and a linear darken alpha weakens the
// backdrop dimming that makes the neon read as solid.
// The guest blends these gamma values after material tone mapping, then
// postfxuber multiplies the scene by 1.41. HDR draws into a separate layer
// so its darken/additive passes never blend with linear scene lighting.
static const float kSceneMultiplier = 1.41;
float3 SplineOut(float3 c) {
#ifdef HDR
  return c;
#else
  return c * kSceneMultiplier;
#endif
}
// Showcase blackout gate: the spline pass draws outside the scene shaders,
// so ps_main's blackout stage (scene.hlsl, mask bit 1024) cannot cover it;
// without this the neon lines leak over the black recording bookends.
// Compiled in only for the SHOWCASE=1 variant swapped in during a run; the
// default build folds to visible and the gate dead-strips.
#ifndef SHOWCASE
#define SHOWCASE 0
#endif
bool SplineVisible(float px_x) {
#if SHOWCASE
  int bits = (int)(intensity.w + 0.5);
  return ((px_x < intensity.z ? 1 : 2) & bits) != 0;
#else
  return true;
#endif
}
float4 ps_default(VSOut i) : SV_Target {
  if (!SplineVisible(i.pos.x)) {
    clip(-1.0);
  }
  float4 c = tex.Sample(smp, i.uv);
  float3 sq = c.rgb * c.rgb * (intensity.x * i.fade / max(c.a, 1.0 / 255.0));
  return float4(SplineOut(sqrt(abs(sq))), 1.0);
}
float4 ps_darken(VSOut i) : SV_Target {
  if (!SplineVisible(i.pos.x)) {
    clip(-1.0);
  }
  float4 c = tex.Sample(smp, i.uv);
  float3 rgb = sqrt(abs(c.rgb * c.rgb * intensity.y));
  float a = sqrt(abs(c.a * intensity.y * i.fade));
  return float4(SplineOut(rgb), a);
}

#ifndef SAMPLES
#define SAMPLES 1
#endif
#if SAMPLES > 1
Texture2DMS<float4> layer : register(t0);
#endif
float4 ps_composite(float4 pos : SV_Position) : SV_Target {
#if SAMPLES > 1
  float4 c = 0.0;
  [unroll] for (int k = 0; k < SAMPLES; ++k) {
    c += layer.Load(int2(pos.xy), k);
  }
  c /= SAMPLES;
#else
  float4 c = tex.Load(int3(pos.xy, 0));
#endif
  // RGB stores the accumulated premultiplied glow; alpha stores only the
  // darken coverage. Resolve both before blending over the gamma scene.
  return float4(c.rgb * kSceneMultiplier, c.a);
}
