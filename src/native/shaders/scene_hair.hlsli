// The CAC and default hair shaders use different highlight models.
float3 CacHairLight(float3 diffuse_linear, float3 n, float3 tangent,
                    float3 view_dir, float3 light_dir, float shadow,
                    float ambient, float3 base_color, float3 highlight_color,
                    float highlight_power) {
  float3 half_dir = normalize(view_dir + light_dir);
  float th = dot(tangent, half_dir);
  float highlight = pow(sqrt(abs(1.0 - th * th)), highlight_power);
  float nl = dot(n, light_dir);
  float wrap = saturate(nl * 0.75 + 0.25);
  float steep_wrap = saturate(nl * 1.75 + 0.25);
  return diffuse_linear * (base_color * (wrap * shadow + ambient) +
                            highlight_color * highlight * steep_wrap * shadow);
}

float3 DefaultHairLight(float4 diffuse_linear, float3 n, float3 view_dir,
                        float3 light_dir, float shadow, float ambient,
                        float4 key_color, float4 key_spec, float4 rim_spec,
                        float3 rim_color, float rim_power, float multiplier) {
  float3 rim_dir = normalize(light_dir * float3(-1.0, 0.2, -1.0) - view_dir);
  float fresnel = 1.0 - saturate(dot(n, view_dir));
  float rim_fresnel = pow(fresnel, rim_power);
  float3 lit = key_color.rgb * saturate(dot(n, light_dir)) * shadow + ambient;
  lit += rim_color * saturate(saturate(dot(n, rim_dir)) * rim_fresnel);
  float ks = pow(saturate(dot(view_dir, reflect(-light_dir, n))), key_spec.w);
  float rs = pow(saturate(dot(view_dir, reflect(-rim_dir, n))), rim_spec.w);
  float3 spec = key_spec.rgb * ks * shadow * pow(fresnel, key_color.w) +
                rim_spec.rgb * rs * rim_fresnel;
  return (diffuse_linear.rgb * lit + saturate(diffuse_linear.a * spec)) * multiplier;
}
