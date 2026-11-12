float HairWorldShadow(float3 wp) {
  if (overlay.w < 0.5) return 1.0;

  const int kHairShadowSize = 512;
  float4 p = float4(wp, 1.0);
  float2 uv = float2(dot(p, ch_sh[0]) * 0.5 + 0.5,
                     dot(p, ch_sh[1]) * -0.5 + 0.5);
  float depth = dot(p, ch_sh[2]);
  float2 st = uv * kHairShadowSize - 0.5;
  int2 base = int2(floor(st));
  float4 taps;
  taps.x = normal_map.Load(int3(clamp(base, 0, kHairShadowSize - 1), 0)).x >= depth;
  taps.y = normal_map.Load(int3(clamp(base + int2(1, 0), 0, kHairShadowSize - 1), 0)).x >= depth;
  taps.z = normal_map.Load(int3(clamp(base + int2(0, 1), 0, kHairShadowSize - 1), 0)).x >= depth;
  taps.w = normal_map.Load(int3(clamp(base + int2(1, 1), 0, kHairShadowSize - 1), 0)).x >= depth;
  float2 f = frac(st);
  return lerp(lerp(taps.x, taps.y, f.x), lerp(taps.z, taps.w, f.x), f.y);
}

// Character families: the game's own lighting in LINEAR space (diffuse is
// gamma -> square it), then the exact tone chain from the disassembly and
// the postfx uber's 1.41 scene multiplier (which the empirical world
// shading already folds into its constants; without it characters sit
// ~30% darker than their surroundings, measured on an F11 A/B pair).
float4 ShadeCharacter(VSOut i, float4 albedo) {
  float fam = cam_pos.w;
  float3 dlin = albedo.rgb * albedo.rgb;
  float3 cn = dot(i.nrm, i.nrm) > 0.01
                  ? normalize(i.nrm)
                  : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  float ndl = saturate(dot(cn, ch_light.xyz));
  float3 vd = -normalize(i.rpos);
  float3 lin;
  float out_a = 1.0;
  if (fam > 5.5) {
    // Traffic vehicles (vehicle.fx fam 6 body / vehicle_glass.fx fam 7
    // windows, disassembled from vehicle_defaultPS): paint recolor where
    // the diffuse green channel is below the mask threshold (red-channel
    // mask * colorize_red + blue-channel mask * colorize_blue, the taxi
    // yellow), key light + the livingworld flat ambient, phong specular
    // along the reflected sun, and an environment-cube reflection scaled
    // by fresnel and the gloss packed in the diffuse alpha. Glass keeps
    // only the reflection terms (its tint rows are zero) and blends at
    // the captured alpha. overlay.y > 0 = the material's cube resolved
    // at t6 (same convention as water).
    float3 sel;
    if (fam < 6.5) {
      sel = dlin.g > 0.001225
                ? dlin
                : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    } else {
      sel = ch_tintA.rgb;
    }
    // DXN panel normal map (the material's `normal` channel, riding the
    // macro slot; overlay.z > 0 = resolved). The vertex layout carries no
    // tangent frame, so build a screen-space cotangent frame from the
    // position/uv derivatives; it reproduces the authored panel shading
    // including mirrored UV islands. Skipping the map entirely shades the
    // hinged panels by their vertex normals, which face away from the sun
    // - the dark ambient-blue "misdrawn shadow" that stopped at the door
    // seam (verified against the ucode: flat map = the artifact, real
    // map = the emulated car).
    float3 vn = cn;
    if (overlay.z > 0.5) {
      float2 nm = macro.Sample(smp, i.uv).rg * 2.0 - 1.0;
      float3 dp1 = ddx(i.rpos), dp2 = ddy(i.rpos);
      float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
      float3 dp2p = cross(dp2, cn), dp1p = cross(cn, dp1);
      float3 tt = dp2p * du1.x + dp1p * du2.x;
      float3 bb = dp2p * du1.y + dp1p * du2.y;
      float im = rsqrt(max(max(dot(tt, tt), dot(bb, bb)), 1e-12));
      vn = normalize(nm.x * tt * im + nm.y * bb * im +
                     cn * sqrt(saturate(1.0 - dot(nm, nm))));
    }
    float vndl = saturate(dot(vn, ch_light.xyz));
    float3 rfl = ch_light.xyz - 2.0 * dot(vn, ch_light.xyz) * vn;
    // The sun spec is gated on N.L >= 0 (the ucode multiplies the spec
    // term by an sge result); the cube reflection is not.
    float spec = pow(saturate(dot(vd, -rfl)), max(ch_sh[0].w, 1.0)) *
                 (dot(vn, ch_light.xyz) >= 0.0 ? 1.0 : 0.0);
    float fres = pow(1.0 - saturate(dot(vn, vd)), max(ch_light.w, 1.0));
    float3 cube = float3(0.0, 0.0, 0.0);
    if (overlay.y > 0.5) {
      cube = env_cube.Sample(smp, reflect(-vd, vn)).rgb;
      cube *= cube;  // the PS consumes the cube squared (linear space)
    }
    // Gloss = the diffuse alpha SQUARED: the ucode squares the whole
    // diffuse fetch (linear-space decode), alpha included; raw alpha
    // over-specs ~5x and mottles the body panels.
    float gloss = fam < 6.5 ? albedo.a * albedo.a : 1.0;
    lin = sel * (ch_key.rgb * vndl + ch_amb.rgb) +
          (spec * ch_sh[0].rgb + cube) * fres * gloss;
    out_a = ch_misc.x;
  } else if (fam > 3.5) {
    float3 ht = i.hair_tangent, hb = i.tanb.xyz;
    float3 hn = i.nrm;
    if (i.tanb.w < 0.5) {
      ScreenTangentFrame(cn, i.rpos, i.uv, ht, hb);
      hn = cn;
    }
    float2 nm = overlay.z > 0.5
                    ? macro.SampleBias(smp, i.uv, misc.y).ag * 2.0 - 1.0
                    : float2(0.0, 0.0);
    float3 mapped = normalize(nm.x * ht + nm.y * hb +
                              sqrt(abs(1.0 - dot(nm, nm))) * hn);
    // Hair uses the baked world map, with the material's own shadow floor.
    float shadow = max(HairWorldShadow(i.rpos + cam_pos.xyz), ch_tintA.w);
    if (fam < 4.5) {
      lin = CacHairLight(dlin, mapped, ht, vd, ch_light.xyz, shadow,
                        ch_amb.w, ch_key.rgb, ch_tintB.rgb, ch_light.w);
    } else {
      lin = DefaultHairLight(albedo * albedo, mapped, vd, ch_light.xyz,
                            shadow, ch_amb.w, float4(ch_key.rgb, ch_rim.w),
                            ch_ks, ch_rs, ch_rim.rgb, ch_misc.w, ch_tintA.x);
    }
    out_a = saturate(decal_art.Sample(smp, i.uv2).r * ch_tintB.w);
  } else if (fam > 2.5) {
    // livingworld pedestrians: the diffuse is a stamp-mask atlas; red
    // regions recolor with tintA, blue with tintB (judged in linear
    // space; real-color regions have green above the threshold).
    // The game's character PSes multiply the key light by the CSM shadow
    // (tap >= ray = lit); characters are casters themselves, so an extra
    // receiver bias suppresses self-shadow acne while the body-onto-board
    // / body-onto-NPC shading survives (the sun-axis depth gap there is
    // tens of cm). Without this the held skateboard, a big flat surface
    // that is almost always inside the skater's own shadow, renders
    // fully sunlit (near-white) against the emulated dark deck.
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, cn, i.pos.xy),
                    SampleStaticSun(i.rpos + cam_pos.xyz, cn, i.pos.xy));
    float3 sel = dlin.g > 0.001225
                     ? dlin
                     : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    lin = sel * (ch_key.rgb * ndl * csm + ch_amb.rgb);
    // livingworld_stamp_defaultPS ends `max oC0.w, c21.x`: the entity's
    // spawn/distance fade. Only visible when the item is routed to the
    // blended sub-pass (alpha < 1); the opaque pass ignores it.
    out_a = ch_misc.x;
  } else {
    // defaultcharacter / CAC pieces: key light + SH irradiance ambient,
    // key gated by the CSM shadow (see the livingworld comment above).
    // The material's DXT5nm normal map (x in ALPHA, y in GREEN; the PSes
    // read tf4.wy) rides the macro slot; overlay.z > 0 = resolved. The
    // skinned vertex layout has no free bytes for the authored tangent
    // frame (blend weights/indices own them), so use the screen-space
    // cotangent frame. ScreenTangentFrame's env calibration (negated U
    // axis, raw V axis) holds for characters too: projecting the skinned
    // meshes through the frame's real view_proj and taking the D3D y-down
    // screen-basis derivatives on the CAMERA-FACING triangles dots the
    // frame against the skinned usage-6/7 tangent frame at -0.95 (tt.T) /
    // +0.95 (bb.B); an edge-basis check without the projection lands on
    // the back-face orientation and reads inverted. Map x rides T, map y
    // rides B (the VS skins the usage-6 tangent into the interpolator the
    // PS pairs with tf4.w, the usage-7 binormal into the tf4.y one).
    float3 vn = cn;
    if (overlay.z > 0.5) {
      // misc.y = LOD bias to the console's 640p-gradient mip (same
      // rationale as the reflective families' cube bias): mip 0 at 4K
      // keeps fine wrinkle noise the console filters away, which reads
      // as weaker authored folds than the emulated reference.
      float2 nm = macro.SampleBias(smp, i.uv, misc.y).ag * 2.0 - 1.0;
      float3 tt, bb;
      ScreenTangentFrame(cn, i.rpos, i.uv, tt, bb);
      vn = normalize(nm.x * tt + nm.y * bb +
                     cn * sqrt(saturate(1.0 - dot(nm, nm))));
    }
    float vndl_u = dot(vn, ch_light.xyz);
    float vndl = saturate(vndl_u);
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, vn, i.pos.xy),
                    SampleStaticSun(i.rpos + cam_pos.xyz, vn, i.pos.xy));
    if (ch_tintA.w > 0.0) {
      dlin *= ch_tintA.rgb;
    }
    float3 irr = saturate(
        ch_sh[0].rgb + vn.x * ch_sh[1].rgb + vn.y * ch_sh[2].rgb +
        vn.z * ch_sh[3].rgb + (vn.x * vn.z) * ch_sh[4].rgb +
        (vn.z * vn.y) * ch_sh[5].rgb + (vn.y * vn.x) * ch_sh[6].rgb +
        (vn.z * vn.z) * ch_sh[7].rgb +
        (vn.x * vn.x - vn.y * vn.y) * ch_sh[8].rgb);
    float3 lit = ch_key.rgb * vndl * csm + irr * ch_amb.w;
    float3 spec = float3(0.0, 0.0, 0.0);
    // Rim light + key/rim phong specular, exact from the character PSes
    // (ch_ks.w == 0 = rows not captured -> the terms vanish). The key spec
    // reflects the sun about the mapped normal, gated on N.L >= 0 and the
    // shadow; the rim terms share the game's fixed rim direction built
    // from the sun and view. Spec mask = the diffuse alpha SQUARED
    // (linear-space decode, like the vehicle gloss); skin/face carry a
    // dedicated mask map in the free decal slot instead (overlay.w = 3;
    // 2 = map present but not yet decoded -> mask 0, because the DXT1
    // skin diffuse's opaque alpha would read as a full-white mask).
    if (ch_ks.w > 0.0) {
      float fb = 1.0 - saturate(dot(vn, vd));
      float kfres = pow(max(fb, 1e-6), ch_rim.w);
      float rfres = pow(max(fb, 1e-6), ch_misc.w);
      float3 rd = normalize(ch_light.xyz * float3(-1.0, 0.2, -1.0) - vd);
      lit += saturate(rfres * saturate(dot(vn, rd))) * ch_rim.rgb;
      float3 kr = ch_light.xyz - 2.0 * vndl_u * vn;
      float ks = pow(saturate(dot(vd, -kr)), ch_ks.w) * csm *
                 (vndl_u >= 0.0 ? 1.0 : 0.0);
      float3 rr = rd - 2.0 * dot(vn, rd) * vn;
      float rs = pow(saturate(dot(vd, -rr)), ch_rs.w) * rfres;
      float smask = overlay.w > 2.5 ? decal_art.SampleBias(smp, i.uv, misc.y).r
                                    : (overlay.w > 1.5 ? 0.0 : albedo.a);
      smask *= smask;
      spec = saturate((ks * ch_ks.rgb * kfres + rs * ch_rs.rgb) * smask);
    }
    lin = dlin * lit + spec;
    // The PS multiplies the lit color by the material multiplier
    // m_params[0].y before the tone chain (1.2 on the gameplay banks;
    // without it the jeans sit ~14% darker than the emulated reference).
    // Captured into the otherwise-unused tintB.w on fams 1/2; 0 = an
    // older capture without it.
    if (ch_tintB.w > 0.25) {
      lin *= ch_tintB.w;
    }
    // defaultcharacter/cacstamp PSes end `max oC0.w, c13.x / c22.x`:
    // the entity's spawn fade (see the livingworld comment above).
    out_a = ch_misc.x;
    // character.alpha accessory (sunglass lens): coverage from the mesh's
    // "alpha" channel at the raw second texcoord, like hair (cac_alphaPS:
    // oC0.w = tf5(uv2).r * c22.x), routed to the blended sub-pass.
    if (ch_misc.z > 0.5) {
      out_a *= saturate(decal_art.Sample(smp, i.uv2).r);
    }
  }
  // Exact tone chain: sqrt(0.5 * (max(x*E/4 + 0.75, 1) - sat(1 - x*E)^2)).
  float E = max(ch_key.w, 0.01);
  return ToneOut(lin * E, out_a, false);

}
