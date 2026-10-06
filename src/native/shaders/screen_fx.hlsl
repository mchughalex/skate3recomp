// The game's final screen pass, independent of the photo-editor chain.
// Live PS rows c0..c2 and VS rows c240..c243 are captured at the draw.
// t0 is the finished native frame; t2 is the game's vignette gradient;
// Host row c248.xy selects explicit base-map LOD for t0/t2, respectively;
// a negative value keeps the shader instruction's computed LOD behavior.
cbuffer Consts : register(b0) { float4 c[256]; };

Texture2D t0 : register(t0);
Texture2D t2 : register(t2);
SamplerState s_lin : register(s0);

float4 SampleScene(float2 uv) {
  float4 result = 0.0;
  if (c[248].x >= 0.0) result = t0.SampleLevel(s_lin, uv, c[248].x);
  else result = t0.Sample(s_lin, uv);
  return result;
}

float4 SampleVignette(float2 uv) {
  float4 result = 0.0;
  if (c[248].y >= 0.0) result = t2.SampleLevel(s_lin, uv, c[248].y);
  else result = t2.Sample(s_lin, uv);
  return result;
}

struct VsOut {
  float4 pos : SV_Position;
  float4 r0 : TEXCOORD0;
};

VsOut vs_raw(uint vid : SV_VertexID) {
  VsOut o;
  float2 uv = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.r0 = float4(uv, uv);
  return o;
}

// postfx_quadTransformVS: sampling UV scale/translation are independent
// of the position transform. The host supplies a fullscreen triangle.
VsOut vs_scaled(uint vid : SV_VertexID) {
  VsOut o = vs_raw(vid);
  o.r0.xy = o.r0.xy * c[243].xy + c[242].xy;
  return o;
}

// postfx_basictex_fisheyePS. The opaque variant has the same RGB math.
float4 LensColor(float4 input_uv) {
  const float4 l254 = float4(-1.0, 0.5, 0.0, 0.0);
  const float4 l255 = float4(-0.5, -0.888888896, 1.0, 1.77777779);
  float4 r0 = input_uv;
  float4 r1 = 0;
  r1.y = c[0].y;
  {
    float old_x = r0.x;
    r0.xyz = float3(old_x * l255.w + l255.y,
                   r0.y * l255.z + l255.x,
                   old_x * l255.z + l255.x);
  }
  r1.x = r0.x * r0.x;
  r1.z = r0.y * r0.y;
  r1.w = r0.z * r0.z;
  {
    float radius_x = r1.x + r1.z;
    float radius_w = r1.w + r1.z;
    r0.x = radius_x;
    r0.w = radius_w;
  }
  r1.x = c[0].x + l255.z;
  r1.z = r0.w * c[1].x;
  r1.w = sqrt(abs(r0.x));
  r1.x = r1.w * (-c[1].z) + r1.x;
  r0.x = r1.z * r0.w + c[1].y;
  {
    float radius = r0.x;
    r0.xy = r0.zy * radius + l254.yy;
  }
  r0 = SampleScene(r0.xy);
  r1 = SampleVignette(r1.xy);
  r1 = saturate(r1 + c[0].w);
  r1 = r1 + l254.x;
  r1 = r1 * c[2].y + l255.z;
  return r1 * r0;
}

float4 ps_opaque(VsOut i) : SV_Target {
  float4 result = LensColor(i.r0);
  result.a = 1.0;
  return result;
}

float4 ps_alpha(VsOut i) : SV_Target {
  return LensColor(i.r0);
}
