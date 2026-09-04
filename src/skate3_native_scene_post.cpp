// Native scene renderer, post-process passes (render thread): SSAO (GTAO),
// screen-space reflections, the HDR bloom/tonemap chain, and the settings-menu
// backdrop blur (including the blur-over-emulated-frames post processor).
// Shared GPU state lives in skate3_native_scene_gpu_internal.h.

#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
// Offline-compiled SPIR-V for the native shaders (compiled from the HLSL
// sources with DXC): the Vulkan RHI backend consumes these blobs; the D3D12
// backend runtime-compiles the embedded HLSL as before.
#include "native/shaders/spirv/skate3_native_shaders_spirv.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif
#include "skate3_native_scene_gpu_internal.h"

// Cvars defined in skate3_native_scene.cpp.
REXCVAR_DECLARE(double, skate3_menu_blur_sigma);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_bloom);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_knee);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_threshold);
REXCVAR_DECLARE(bool, skate3_native_render_scene_haze);
REXCVAR_DECLARE(double, skate3_native_render_scene_haze_density);
REXCVAR_DECLARE(double, skate3_native_render_scene_haze_intensity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_hdr_debug);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr_packed);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shafts);
REXCVAR_DECLARE(double, skate3_native_render_scene_shafts_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_shafts_reach);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shafts_steps);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shafts_res);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_debug);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_full_res);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_luma_protect);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_radius);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ssr_debug);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssr_intensity);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ssr_steps);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssr_thickness);

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)

#include "skate3_native_shaders.h"

namespace skate3::native_scene {

// ---- Screen-space ambient occlusion (ssao.hlsl: GTAO) --------------------
// Layout + PSOs, built lazily on the first enabled frame and rebuilt when
// the MSAA level (linearize shader variant) or the output format (composite
// target) changes. Failure disables the effect only.
bool EnsureSsaoPipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.ao_failed) {
    return false;
  }
  // Composite target: the float HDR plane (pre-tonemap multiply) or the
  // gamma guest output (classic).
  const nrhi::Format ao_out_fmt = g_r.hdr_active
                                      ? g_r.hdr_scene_format
                                      : context.guest_output->format();
  if (g_r.pso_ao_linearize != nullptr && g_r.ao_msaa == g_r.msaa &&
      g_r.ao_rtv_format == ao_out_fmt && g_r.ao_hdr == g_r.hdr_active) {
    return true;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: ssao pipeline setup failed ({})", what);
    g_r.ao_failed = true;
    return false;
  };
  if (g_r.ao_layout == nullptr) {
    nrhi::BindingLayoutDesc ld;
    ld.param_count = 3;
    ld.params[0] = {nrhi::BindingParamKind::kConstants, /*b*/ 0, 16,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 1,
                    nrhi::Visibility::kPixel};
    ld.params[2] = {nrhi::BindingParamKind::kTextureTable, 1, 1,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 2;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kPoint,
                             nrhi::AddressMode::kClamp, 1};
    ld.static_samplers[1] = {1, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.allow_input_layout = false;
    g_r.ao_layout = device->CreateBindingLayout(ld);
    if (g_r.ao_layout == nullptr) {
      return fail("binding layout");
    }
  }
  nrhi::Pipeline** psos[6] = {&g_r.pso_ao_linearize, &g_r.pso_ao_gtao,
                              &g_r.pso_ao_blur, &g_r.pso_ao_luma,
                              &g_r.pso_ao_composite, &g_r.pso_ao_debug};
  for (nrhi::Pipeline** p : psos) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }
  const bool msaa = g_r.msaa > 1;
  const nrhi::ShaderMacro msaa_defs[] = {{"AO_MSAA", "1"}, {nullptr, nullptr}};
  // HDR=1 on the luma pass (tonemap the pre-tonemap scene before the
  // protection luma) and the march (linear-space intensity exponent).
  const nrhi::ShaderMacro hdr_defs[] = {{"HDR", "1"}, {nullptr, nullptr}};
  nrhi::Shader* vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "ssao.hlsl",
                     kSsaoShaderSource, "vs_main", nullptr, ""));
  const char* ps_entries[6] = {"ps_linearize", "ps_gtao", "ps_blur",
                               "ps_luma", "ps_composite", "ps_debug"};
  nrhi::Shader* ps[6] = {};
  bool ok = vs != nullptr;
  for (int i = 0; i < 6; ++i) {
    const bool msaa_entry = i == 0 && msaa;
    const bool hdr_entry = (i == 1 || i == 3) && g_r.hdr_active;
    ps[i] = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "ssao.hlsl", kSsaoShaderSource,
        ps_entries[i],
        msaa_entry ? msaa_defs : (hdr_entry ? hdr_defs : nullptr),
        msaa_entry ? "AO_MSAA=1" : (hdr_entry ? "HDR=1" : "")));
    ok = ok && ps[i] != nullptr;
  }
  if (!ok) {
    device->DestroyDeferred(vs);
    for (nrhi::Shader* s : ps) {
      device->DestroyDeferred(s);
    }
    return fail("shader compile");
  }
  nrhi::GraphicsPipelineDesc pso;
  pso.layout = g_r.ao_layout;
  pso.vs = vs;
  pso.cull = nrhi::CullMode::kNone;
  pso.sample_count = 1;
  pso.ps = ps[0];
  pso.rtv_format = nrhi::Format::kR32_FLOAT;
  g_r.pso_ao_linearize = device->CreateGraphicsPipeline(pso);
  pso.ps = ps[1];
  pso.rtv_format = nrhi::Format::kR8_UNORM;
  g_r.pso_ao_gtao = device->CreateGraphicsPipeline(pso);
  pso.ps = ps[2];
  g_r.pso_ao_blur = device->CreateGraphicsPipeline(pso);
  pso.ps = ps[3];
  g_r.pso_ao_luma = device->CreateGraphicsPipeline(pso);
  // Composite: dst.rgb * src.a; the AO plane carries the finished
  // multiplier, and the RGB-only write mask keeps the output's alpha
  // untouched.
  pso.ps = ps[4];
  pso.rtv_format = ao_out_fmt;
  pso.blend.enable = true;
  pso.blend.src = nrhi::BlendFactor::kZero;
  pso.blend.dst = nrhi::BlendFactor::kSrcAlpha;
  pso.blend.op = nrhi::BlendOp::kAdd;
  pso.blend.src_alpha = nrhi::BlendFactor::kZero;
  pso.blend.dst_alpha = nrhi::BlendFactor::kOne;
  pso.blend.op_alpha = nrhi::BlendOp::kAdd;
  pso.blend.write_mask = 0x7;
  g_r.pso_ao_composite = device->CreateGraphicsPipeline(pso);
  pso.blend = {};
  pso.ps = ps[5];
  g_r.pso_ao_debug = device->CreateGraphicsPipeline(pso);
  device->DestroyDeferred(vs);
  for (nrhi::Shader* s : ps) {
    device->DestroyDeferred(s);
  }
  if (g_r.pso_ao_linearize == nullptr || g_r.pso_ao_gtao == nullptr ||
      g_r.pso_ao_blur == nullptr || g_r.pso_ao_luma == nullptr ||
      g_r.pso_ao_composite == nullptr || g_r.pso_ao_debug == nullptr) {
    return fail("pso");
  }
  g_r.ao_msaa = g_r.msaa;
  g_r.ao_rtv_format = ao_out_fmt;
  g_r.ao_hdr = g_r.hdr_active;
  REXLOG_INFO("native-scene: ssao pipeline created (MSAA x{}, {})", g_r.msaa,
              g_r.hdr_active ? "HDR" : "classic");
  return true;
}

// GTAO chain over the resolved scene (see ssao.hlsl): linearize depth,
// horizon-based AO, depth-aware separable blur, multiply-composite onto the
// guest output (which must be in RENDER_TARGET state). Returns true when it
// drew; binding layout/state was changed and the caller restores the main
// pass's bindings.
bool ApplySsaoPass(const NativeGuestOutputRenderContext& context,
                   nrhi::Cmd* cmd, const FrameScene& scene,
                   const nrhi::Viewport& viewport, const nrhi::Rect& scissor) {
  // The published projection must be the live perspective matrix (row-vector
  // m23 == 1; all zeros until the first publish).
  const float* pr = scene.proj;
  if (pr[11] != 1.0f || pr[0] == 0.0f || pr[5] == 0.0f || pr[14] == 0.0f) {
    return false;
  }
  if (!EnsureSsaoPipeline(context)) {
    return false;
  }
  // The luma pass samples the scene through this view; the composite writes
  // the same plane (HDR: the float scene plane pre-tonemap, classic: the
  // guest output).
  const bool hdr = g_r.hdr_active && g_r.hdr_resolved != nullptr;
  nrhi::Texture* const scene_plane =
      hdr ? g_r.hdr_resolved : context.guest_output;
  nrhi::TextureView* const scene_srv =
      hdr ? g_r.hdr_srv : g_r.output_srv_slot;
  if (scene_srv == nullptr) {
    return false;
  }
  nrhi::Device* device = context.device;
  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;
  // AO raster: half the output resolution by default: for a low-frequency
  // term the depth-aware blur + bilinear upsample hide it, at 1/4 the march
  // cost (full-res GTAO at 4K x 300+ uncapped fps pegged the GPU). The
  // linearized depth stays full res: the march samples it at arbitrary UVs
  // either way, and it keeps silhouette depth exact.
  const bool ao_full =
      REXCVAR_GET(skate3_native_render_scene_ssao_full_res);
  const uint32_t aw = ao_full ? width : std::max(1u, (width + 1) / 2);
  const uint32_t ah = ao_full ? height : std::max(1u, (height + 1) / 2);
  // Intermediates (steady state RENDER_TARGET), rebuilt on either size.
  if (g_r.ao_width != aw || g_r.ao_height != ah ||
      g_r.ao_lin_width != width || g_r.ao_lin_height != height ||
      g_r.ao_lin_depth == nullptr || g_r.ao_luma == nullptr) {
    nrhi::Texture** res[4] = {&g_r.ao_lin_depth, &g_r.ao_tex[0],
                              &g_r.ao_tex[1], &g_r.ao_luma};
    nrhi::TextureView** views[4] = {&g_r.ao_lin_srv, &g_r.ao_srv[0],
                                    &g_r.ao_srv[1], &g_r.ao_luma_srv};
    for (int i = 0; i < 4; ++i) {
      if (*views[i] != nullptr) {
        device->DestroyDeferred(*views[i]);
        *views[i] = nullptr;
      }
      if (*res[i] != nullptr) {
        device->DestroyDeferred(*res[i]);
        *res[i] = nullptr;
      }
      nrhi::TextureDesc desc;
      desc.width = i == 0 ? width : aw;
      desc.height = i == 0 ? height : ah;
      desc.mip_levels = 1;
      desc.format =
          i == 0 ? nrhi::Format::kR32_FLOAT : nrhi::Format::kR8_UNORM;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      *res[i] = device->CreateTexture(desc);
      if (*res[i] == nullptr) {
        g_r.ao_failed = true;
        return false;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      *views[i] = device->CreateTextureView(*res[i], vd);
      if (*views[i] == nullptr) {
        g_r.ao_failed = true;
        return false;
      }
    }
    g_r.ao_width = aw;
    g_r.ao_height = ah;
    g_r.ao_lin_width = width;
    g_r.ao_lin_height = height;
  }
  // Scene-depth SRV, re-pointed when the depth buffer is rebuilt (resize /
  // MSAA change). D32 textures view as R32_FLOAT automatically.
  if (g_r.ao_depth_srv == nullptr || g_r.ao_depth_srv_of != g_r.depth) {
    if (g_r.ao_depth_srv != nullptr) {
      device->DestroyDeferred(g_r.ao_depth_srv);
      g_r.ao_depth_srv = nullptr;
    }
    nrhi::TextureViewDesc sd;
    if (g_r.msaa > 1) {
      sd.dimension = nrhi::ViewDimension::k2DMS;
    } else {
      sd.dimension = nrhi::ViewDimension::k2D;
      sd.mip_levels = 1;
    }
    g_r.ao_depth_srv = device->CreateTextureView(g_r.depth, sd);
    if (g_r.ao_depth_srv == nullptr) {
      g_r.ao_failed = true;
      return false;
    }
    g_r.ao_depth_srv_of = g_r.depth;
  }

  // b0 rows (see ssao.hlsl cbuffer C): size, projection, tuning, blur axis.
  // size = the AO raster the march/blur passes run at; every shader use is
  // UV-relative, so half res needs no shader-side changes.
  float c[16];
  c[0] = float(aw);
  c[1] = float(ah);
  c[2] = 1.0f / float(aw);
  c[3] = 1.0f / float(ah);
  c[4] = std::fabs(pr[0]);  // |m00|: the guest projection's x scale is
                            // negative; AO is mirror-invariant
  c[5] = std::fabs(pr[5]);  // |m11|
  c[6] = pr[10];            // m22
  c[7] = pr[14];            // m32
  c[8] = float(REXCVAR_GET(skate3_native_render_scene_ssao_radius));
  c[9] = float(REXCVAR_GET(skate3_native_render_scene_ssao_intensity));
  // Distance fade 150..250 view units: fog owns the far field and the
  // horizon-search precision degrades with depth anyway.
  c[10] = 150.0f;
  c[11] = 1.0f / 100.0f;
  c[12] = 1.0f;  // blur axis (horizontal first)
  c[13] = 0.0f;
  c[14] = float(REXCVAR_GET(skate3_native_render_scene_ssao_luma_protect));
  c[15] = 0.0f;

  const nrhi::Viewport ao_vp{0.0f, 0.0f, float(aw), float(ah), 0.0f, 1.0f};
  const nrhi::Rect ao_sc{0, 0, int32_t(aw), int32_t(ah)};

  cmd->SetBindingLayout(g_r.ao_layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  cmd->SetRootConstants(0, 16, c, 0);

  // 1) Scene depth -> linear view Z (sample 0 when MSAA). Full res.
  cmd->Barrier(g_r.depth, nrhi::ResourceState::kDepthWrite,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(g_r.ao_lin_depth, nullptr);
  cmd->SetPipeline(g_r.pso_ao_linearize);
  cmd->SetTexture(1, g_r.ao_depth_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.depth, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kDepthWrite);
  cmd->Barrier(g_r.ao_lin_depth, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();

  // 1b) Scene luminance -> ao_luma (AO raster): the sun-lit protection
  //     mask. The scene plane pauses as a sampled source for one draw.
  cmd->Barrier(scene_plane, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  cmd->SetViewport(ao_vp);
  cmd->SetScissor(ao_sc);
  cmd->SetRenderTargets(g_r.ao_luma, nullptr);
  cmd->SetPipeline(g_r.pso_ao_luma);
  cmd->SetTexture(1, scene_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.ao_luma, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->Barrier(scene_plane, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();

  // 2) GTAO -> ao_tex[0], at the AO raster size.
  cmd->SetRenderTargets(g_r.ao_tex[0], nullptr);
  cmd->SetPipeline(g_r.pso_ao_gtao);
  cmd->SetTexture(1, g_r.ao_lin_srv);
  cmd->SetTexture(2, g_r.ao_luma_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.ao_tex[0], nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();

  // 3) Depth-aware blur, horizontal -> ao_tex[1].
  cmd->SetRenderTargets(g_r.ao_tex[1], nullptr);
  cmd->SetPipeline(g_r.pso_ao_blur);
  cmd->SetTexture(1, g_r.ao_srv[0]);
  cmd->SetTexture(2, g_r.ao_lin_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.ao_tex[1], nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->Barrier(g_r.ao_tex[0], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();

  // 4) Vertical -> ao_tex[0].
  const float dir_v[2] = {0.0f, 1.0f};
  cmd->SetRootConstants(0, 2, dir_v, 12);
  cmd->SetRenderTargets(g_r.ao_tex[0], nullptr);
  cmd->SetTexture(1, g_r.ao_srv[1]);
  cmd->SetTexture(2, g_r.ao_lin_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.ao_tex[0], nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();

  // 5) Consume the finished multiplier plane. Classic: a full-res composite
  //    (or debug replace) draw onto the scene plane, s1 bilinearly
  //    upsampling. HDR: no draw at all: ps_tonemap samples the plane at t2
  //    and multiplies pre-tonemap (identical math, one less full-res pass);
  //    the plane stays in PIXEL_SHADER_RESOURCE for ApplyHdrPost, which
  //    restores it.
  if (hdr) {
    g_r.ao_plane_in_psr = true;
  } else {
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetRenderTargets(scene_plane, nullptr);
    cmd->SetPipeline(REXCVAR_GET(skate3_native_render_scene_ssao_debug)
                         ? g_r.pso_ao_debug
                         : g_r.pso_ao_composite);
    cmd->SetTexture(1, g_r.ao_srv[0]);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.ao_tex[0], nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
  }

  // Occlusion grid (perf-items attribution AND the occlusion cull): while
  // the linear depth is still bound as an SRV, tile-MAX reduce it into the
  // small R32F grid and queue a never-waited copy into the readback ring
  // (the photo-grab pattern; consumed 1-2 frames later by the item
  // classifier / cull in RenderScene). Any creation failure disables only
  // these consumers.
  if ((REXCVAR_GET(skate3_native_render_scene_perf_items) ||
       REXCVAR_GET(skate3_native_render_scene_occlusion_cull)) &&
      !g_r.occl_failed) {
    if (g_r.pso_occl_reduce == nullptr) {
      nrhi::Shader* rvs = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kVertex, "ssao.hlsl",
                         kSsaoShaderSource, "vs_main", nullptr, ""));
      nrhi::Shader* rps = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kPixel, "ssao.hlsl",
                         kSsaoShaderSource, "ps_depth_max", nullptr, ""));
      if (rvs != nullptr && rps != nullptr) {
        nrhi::GraphicsPipelineDesc rp;
        rp.layout = g_r.ao_layout;
        rp.vs = rvs;
        rp.ps = rps;
        rp.cull = nrhi::CullMode::kNone;
        rp.sample_count = 1;
        rp.rtv_format = nrhi::Format::kR32_FLOAT;
        g_r.pso_occl_reduce = device->CreateGraphicsPipeline(rp);
      }
      if (rvs != nullptr) {
        device->DestroyDeferred(rvs);
      }
      if (rps != nullptr) {
        device->DestroyDeferred(rps);
      }
      if (g_r.pso_occl_reduce == nullptr) {
        g_r.occl_failed = true;
        REXLOG_WARN(
            "native-scene: occlusion-grid PSO creation failed - occluded-item "
            "attribution disabled");
      }
    }
    if (!g_r.occl_failed && g_r.occl_tex == nullptr) {
      nrhi::TextureDesc td;
      td.width = RendererState::kOcclGridW;
      td.height = RendererState::kOcclGridH;
      td.mip_levels = 1;
      td.format = nrhi::Format::kR32_FLOAT;
      // The grid is read back to the CPU every frame, so it must declare
      // copy-source usage: the Vulkan backend only requests TRANSFER_SRC for
      // textures that ask for it, and both the transition to the copy-source
      // state and the copy itself are invalid without it.
      td.usage = nrhi::kTextureUsageRenderTarget | nrhi::kTextureUsageCopySource;
      td.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.occl_tex = device->CreateTexture(td);
      nrhi::BufferDesc bd;
      bd.size = uint64_t(RendererState::kOcclRowPitch) * RendererState::kOcclGridH;
      bd.heap = nrhi::HeapKind::kReadback;
      for (int i = 0; i < 2 && g_r.occl_tex != nullptr; ++i) {
        g_r.occl_readback[i] = device->CreateBuffer(bd);
        if (g_r.occl_readback[i] == nullptr ||
            (g_r.occl_readback_ptr[i] = static_cast<uint8_t*>(
                 device->Map(g_r.occl_readback[i]))) == nullptr) {
          break;
        }
      }
      if (g_r.occl_tex == nullptr || g_r.occl_readback_ptr[1] == nullptr) {
        g_r.occl_failed = true;
        REXLOG_WARN(
            "native-scene: occlusion-grid readback creation failed - "
            "occluded-item attribution disabled");
      }
    }
    // Skip the frame rather than ever waiting when both ring slots are
    // still in flight (the consumer retires completed slots).
    const int w = g_r.occl_write_index;
    if (!g_r.occl_failed &&
        (!g_r.occl_pending[w] ||
         g_r.occl_submission[w] < device->CompletedSubmission())) {
      float rc[4] = {float(width), float(height),
                     float(RendererState::kOcclGridW),
                     float(RendererState::kOcclGridH)};
      cmd->SetRootConstants(0, 4, rc, 0);
      cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f,
                                      float(RendererState::kOcclGridW),
                                      float(RendererState::kOcclGridH), 0.0f,
                                      1.0f});
      cmd->SetScissor(nrhi::Rect{0, 0, int32_t(RendererState::kOcclGridW),
                                 int32_t(RendererState::kOcclGridH)});
      cmd->SetRenderTargets(g_r.occl_tex, nullptr);
      cmd->SetPipeline(g_r.pso_occl_reduce);
      cmd->SetTexture(1, g_r.ao_lin_srv);
      cmd->Draw(3, 0);
      cmd->Barrier(g_r.occl_tex, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kCopySource);
      cmd->FlushBarriers();
      cmd->CopyTextureToBuffer(g_r.occl_readback[w], 0,
                               RendererState::kOcclRowPitch, g_r.occl_tex, 0,
                               RendererState::kOcclGridW,
                               RendererState::kOcclGridH);
      cmd->Barrier(g_r.occl_tex, nrhi::ResourceState::kCopySource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      std::memcpy(g_r.occl_vp[w], scene.view_proj, sizeof(g_r.occl_vp[w]));
      std::memcpy(g_r.occl_cam[w], scene.cam_pos, sizeof(g_r.occl_cam[w]));
      g_r.occl_submission[w] = device->CurrentSubmission();
      g_r.occl_pending[w] = true;
      g_r.occl_write_index = 1 - w;
    }
  }

  // Restore intermediate steady states.
  cmd->Barrier(g_r.ao_tex[1], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(g_r.ao_lin_depth, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(g_r.ao_luma, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  return true;
}

// ---- Screen-space reflections (ssr.hlsl + scene.hlsl ps_refl_gbuf) --------
// Layout + PSOs, built lazily on the first SSR frame and rebuilt when the
// MSAA level (linearize variant) or the scene float format (composite
// target) changes. Failure disables the effect only, never g_r.failed.
bool EnsureSsrPipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.ssr_failed || !g_r.hdr_active) {
    return false;
  }
  if (g_r.pso_ssr_march != nullptr && g_r.ssr_msaa == g_r.msaa &&
      g_r.ssr_scene_fmt == g_r.hdr_scene_format &&
      g_r.ssr_showcase == g_r.showcase_shaders) {
    return true;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: ssr pipeline setup failed ({})", what);
    g_r.ssr_failed = true;
    return false;
  };
  if (g_r.ssr_layout == nullptr) {
    // The SSAO layout shape with a wider constant block (the march needs
    // the world->view rotation rows): root constants b0 (32 floats) + three
    // single-texture tables + point/linear clamp samplers.
    nrhi::BindingLayoutDesc ld;
    ld.param_count = 4;
    ld.params[0] = {nrhi::BindingParamKind::kConstants, /*b*/ 0, 32,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 1,
                    nrhi::Visibility::kPixel};
    ld.params[2] = {nrhi::BindingParamKind::kTextureTable, 1, 1,
                    nrhi::Visibility::kPixel};
    ld.params[3] = {nrhi::BindingParamKind::kTextureTable, 2, 1,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 2;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kPoint,
                             nrhi::AddressMode::kClamp, 1};
    ld.static_samplers[1] = {1, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.allow_input_layout = false;
    g_r.ssr_layout = device->CreateBindingLayout(ld);
    if (g_r.ssr_layout == nullptr) {
      return fail("binding layout");
    }
  }
  for (nrhi::Pipeline** p : {&g_r.pso_ssr_gbuf, &g_r.pso_ssr_linearize,
                             &g_r.pso_ssr_march, &g_r.pso_ssr_composite}) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }
  const bool msaa = g_r.msaa > 1;
  const nrhi::ShaderMacro msaa_defs[] = {{"SSR_MSAA", "1"},
                                         {nullptr, nullptr}};
  nrhi::Shader* scene_vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource,
                     "vs_main", nullptr, ""));
  nrhi::Shader* ps_gbuf = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                     "ps_refl_gbuf", nullptr, ""));
  nrhi::Shader* vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "ssr.hlsl", kSsrShaderSource,
                     "vs_main", nullptr, ""));
  nrhi::Shader* ps_lin = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "ssr.hlsl", kSsrShaderSource, "ps_linearize",
      msaa ? msaa_defs : nullptr, msaa ? "SSR_MSAA=1" : ""));
  nrhi::Shader* ps_march = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "ssr.hlsl", kSsrShaderSource,
                     "ps_march", nullptr, ""));
  // The composite follows the showcase shader swap (its SHOWCASE=1 variant
  // carries the per-side reveal gate).
  const nrhi::ShaderMacro sc_defs[] = {{"SHOWCASE", "1"}, {nullptr, nullptr}};
  nrhi::Shader* ps_comp = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "ssr.hlsl", kSsrShaderSource, "ps_composite",
      g_r.showcase_shaders ? sc_defs : nullptr,
      g_r.showcase_shaders ? "SHOWCASE=1" : ""));
  const bool shaders_ok = scene_vs != nullptr && ps_gbuf != nullptr &&
                          vs != nullptr && ps_lin != nullptr &&
                          ps_march != nullptr && ps_comp != nullptr;
  if (!shaders_ok) {
    for (nrhi::Shader* s :
         {scene_vs, ps_gbuf, vs, ps_lin, ps_march, ps_comp}) {
      device->DestroyDeferred(s);
    }
    return fail("shader compile");
  }
  // Reflection G-buffer pass: the scene VS + input layout under the MAIN
  // binding layout (the caller re-stages each reflective item's main-pass
  // root constants / textures), half-res single-sample float target with
  // its OWN half-res depth (test+write: the MSAA scene depth cannot pair
  // with a 1x RTV) so the nearest reflective surface owns each texel;
  // occlusion by non-reflective geometry is then resolved in the march by
  // comparing the stored G-buffer depth against the scene linear depth.
  {
    nrhi::GraphicsPipelineDesc pso;
    pso.layout = g_r.layout;
    pso.vs = scene_vs;
    pso.ps = ps_gbuf;
    pso.input_elements = kSceneInputLayout;
    pso.input_element_count = 7;
    pso.vertex_stride = 56;
    pso.cull = nrhi::CullMode::kNone;
    pso.sample_count = 1;
    pso.depth_clip = true;
    pso.depth.test_enable = true;
    pso.depth.write_enable = true;
    pso.depth.func = nrhi::CompareFunc::kLess;
    pso.dsv_format = nrhi::Format::kD32_FLOAT;
    pso.rtv_format = nrhi::Format::kR16G16B16A16_FLOAT;
    g_r.pso_ssr_gbuf = device->CreateGraphicsPipeline(pso);
  }
  nrhi::GraphicsPipelineDesc pso;
  pso.layout = g_r.ssr_layout;
  pso.vs = vs;
  pso.cull = nrhi::CullMode::kNone;
  pso.sample_count = 1;
  pso.ps = ps_lin;
  pso.rtv_format = nrhi::Format::kR32_FLOAT;
  g_r.pso_ssr_linearize = device->CreateGraphicsPipeline(pso);
  pso.ps = ps_march;
  pso.rtv_format = nrhi::Format::kR16G16B16A16_FLOAT;
  g_r.pso_ssr_march = device->CreateGraphicsPipeline(pso);
  // Composite: straight src-alpha blend onto the float scene plane (the
  // shader outputs confidence x reflectivity x intensity in alpha), RGB
  // write mask: the plane's alpha stays untouched.
  pso.ps = ps_comp;
  pso.rtv_format = g_r.hdr_scene_format;
  pso.blend.enable = true;
  pso.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pso.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.op = nrhi::BlendOp::kAdd;
  pso.blend.src_alpha = nrhi::BlendFactor::kZero;
  pso.blend.dst_alpha = nrhi::BlendFactor::kOne;
  pso.blend.op_alpha = nrhi::BlendOp::kAdd;
  pso.blend.write_mask = 0x7;
  g_r.pso_ssr_composite = device->CreateGraphicsPipeline(pso);
  for (nrhi::Shader* s : {scene_vs, ps_gbuf, vs, ps_lin, ps_march, ps_comp}) {
    device->DestroyDeferred(s);
  }
  if (g_r.pso_ssr_gbuf == nullptr || g_r.pso_ssr_linearize == nullptr ||
      g_r.pso_ssr_march == nullptr || g_r.pso_ssr_composite == nullptr) {
    return fail("pso");
  }
  g_r.ssr_msaa = g_r.msaa;
  g_r.ssr_scene_fmt = g_r.hdr_scene_format;
  g_r.ssr_showcase = g_r.showcase_shaders;
  REXLOG_INFO("native-scene: ssr pipeline created (MSAA x{})", g_r.msaa);
  return true;
}

// Half-res SSR intermediates (reflection G-buffer + march output), steady
// state RENDER_TARGET, rebuilt on resize.
bool EnsureSsrTargets(const NativeGuestOutputRenderContext& context) {
  const uint32_t sw = std::max(1u, (context.guest_output_width + 1) / 2);
  const uint32_t sh = std::max(1u, (context.guest_output_height + 1) / 2);
  if (g_r.ssr_width == sw && g_r.ssr_height == sh && g_r.ssr_gbuf != nullptr &&
      g_r.ssr_tex != nullptr) {
    return true;
  }
  nrhi::Device* device = context.device;
  if (g_r.ssr_gbuf_depth != nullptr) {
    device->DestroyDeferred(g_r.ssr_gbuf_depth);
    g_r.ssr_gbuf_depth = nullptr;
  }
  {
    nrhi::TextureDesc dd;
    dd.width = sw;
    dd.height = sh;
    dd.format = nrhi::Format::kD32_FLOAT;
    dd.usage = nrhi::kTextureUsageDepthStencil;
    dd.initial_state = nrhi::ResourceState::kDepthWrite;
    dd.clear_depth = 1.0f;
    g_r.ssr_gbuf_depth = device->CreateTexture(dd);
    if (g_r.ssr_gbuf_depth == nullptr) {
      g_r.ssr_failed = true;
      return false;
    }
  }
  nrhi::Texture** res[2] = {&g_r.ssr_gbuf, &g_r.ssr_tex};
  nrhi::TextureView** views[2] = {&g_r.ssr_gbuf_srv, &g_r.ssr_srv};
  for (int i = 0; i < 2; ++i) {
    if (*views[i] != nullptr) {
      device->DestroyDeferred(*views[i]);
      *views[i] = nullptr;
    }
    if (*res[i] != nullptr) {
      device->DestroyDeferred(*res[i]);
      *res[i] = nullptr;
    }
    nrhi::TextureDesc desc;
    desc.width = sw;
    desc.height = sh;
    desc.mip_levels = 1;
    desc.format = nrhi::Format::kR16G16B16A16_FLOAT;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    *res[i] = device->CreateTexture(desc);
    if (*res[i] == nullptr) {
      g_r.ssr_failed = true;
      return false;
    }
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    *views[i] = device->CreateTextureView(*res[i], vd);
    if (*views[i] == nullptr) {
      g_r.ssr_failed = true;
      return false;
    }
  }
  g_r.ssr_width = sw;
  g_r.ssr_height = sh;
  return true;
}

// SSR march + composite (see ssr.hlsl): consumes the reflection G-buffer
// the in-pass draw left in PIXEL_SHADER_RESOURCE, marches the full-res
// linear depth (reusing the SSAO linearize plane when AO ran this frame)
// while sampling the pre-tonemap HDR plane, and blends the result back onto
// it. Returns true when it drew; the binding layout changed and the
// caller's post_ran block restores the main-pass bindings.
bool ApplySsrPass(const NativeGuestOutputRenderContext& context,
                  nrhi::Cmd* cmd, const FrameScene& scene,
                  const nrhi::Viewport& viewport, const nrhi::Rect& scissor,
                  bool lin_depth_ready) {
  if (!g_r.ssr_gbuf_ready) {
    return false;
  }
  // Consume the handoff up front: every exit below must leave the G-buffer
  // back in its RENDER_TARGET steady state.
  g_r.ssr_gbuf_ready = false;
  nrhi::Device* device = context.device;
  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;
  const auto bail = [&] {
    cmd->Barrier(g_r.ssr_gbuf, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    return false;
  };
  // Full-res linear view-Z: the SSAO plane when AO linearized this frame
  // (it idles back in RENDER_TARGET after the AO pass), else the own
  // linearize below.
  lin_depth_ready = lin_depth_ready && g_r.ao_lin_depth != nullptr &&
                    g_r.ao_lin_srv != nullptr && g_r.ao_lin_width == width &&
                    g_r.ao_lin_height == height;
  if (!lin_depth_ready) {
    if (g_r.ssr_lin_depth == nullptr || g_r.ssr_lin_width != width ||
        g_r.ssr_lin_height != height) {
      if (g_r.ssr_lin_srv != nullptr) {
        device->DestroyDeferred(g_r.ssr_lin_srv);
        g_r.ssr_lin_srv = nullptr;
      }
      if (g_r.ssr_lin_depth != nullptr) {
        device->DestroyDeferred(g_r.ssr_lin_depth);
        g_r.ssr_lin_depth = nullptr;
      }
      nrhi::TextureDesc desc;
      desc.width = width;
      desc.height = height;
      desc.mip_levels = 1;
      desc.format = nrhi::Format::kR32_FLOAT;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.ssr_lin_depth = device->CreateTexture(desc);
      if (g_r.ssr_lin_depth == nullptr) {
        g_r.ssr_failed = true;
        return bail();
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.ssr_lin_srv = device->CreateTextureView(g_r.ssr_lin_depth, vd);
      if (g_r.ssr_lin_srv == nullptr) {
        g_r.ssr_failed = true;
        return bail();
      }
      g_r.ssr_lin_width = width;
      g_r.ssr_lin_height = height;
    }
    // Scene-depth SRV, re-pointed when the depth buffer is rebuilt.
    if (g_r.ssr_depth_srv == nullptr || g_r.ssr_depth_srv_of != g_r.depth) {
      if (g_r.ssr_depth_srv != nullptr) {
        device->DestroyDeferred(g_r.ssr_depth_srv);
        g_r.ssr_depth_srv = nullptr;
      }
      nrhi::TextureViewDesc sd;
      if (g_r.msaa > 1) {
        sd.dimension = nrhi::ViewDimension::k2DMS;
      } else {
        sd.dimension = nrhi::ViewDimension::k2D;
        sd.mip_levels = 1;
      }
      g_r.ssr_depth_srv = device->CreateTextureView(g_r.depth, sd);
      if (g_r.ssr_depth_srv == nullptr) {
        g_r.ssr_failed = true;
        return bail();
      }
      g_r.ssr_depth_srv_of = g_r.depth;
    }
  }
  nrhi::Texture* const lin_tex =
      lin_depth_ready ? g_r.ao_lin_depth : g_r.ssr_lin_depth;
  nrhi::TextureView* const lin_srv =
      lin_depth_ready ? g_r.ao_lin_srv : g_r.ssr_lin_srv;

  // b0 rows (see ssr.hlsl cbuffer C): dest size, projection, the world ->
  // "AO view space" rotation, tuning. view = view_proj * proj^-1 (row-
  // vector); the projection's per-axis signs fold into the rotation so
  // normals land in the same mirrored space ViewPos reconstructs positions
  // in; reflection directions are mirror-SENSITIVE, unlike every AO term.
  const float* pr = scene.proj;
  float c[32] = {};
  c[0] = float(g_r.ssr_width);
  c[1] = float(g_r.ssr_height);
  c[2] = 1.0f / float(g_r.ssr_width);
  c[3] = 1.0f / float(g_r.ssr_height);
  c[4] = std::fabs(pr[0]);
  c[5] = std::fabs(pr[5]);
  c[6] = pr[10];
  c[7] = pr[14];
  const float sxs = pr[0] < 0.0f ? -1.0f : 1.0f;
  const float sys = pr[5] < 0.0f ? -1.0f : 1.0f;
  const float inv00 = 1.0f / pr[0];
  const float inv11 = 1.0f / pr[5];
  for (int r = 0; r < 3; ++r) {
    // proj^-1 columns: x/y scale by 1/m00 / 1/m11, view z = clip w (the
    // view_proj row's w column); the AO-space axis signs multiply in.
    c[8 + r * 4 + 0] = scene.view_proj[r * 4 + 0] * inv00 * sxs;
    c[8 + r * 4 + 1] = scene.view_proj[r * 4 + 1] * inv11 * sys;
    c[8 + r * 4 + 2] = scene.view_proj[r * 4 + 3];
  }
  c[20] = float(REXCVAR_GET(skate3_native_render_scene_ssr_steps));
  c[21] = float(REXCVAR_GET(skate3_native_render_scene_ssr_thickness));
  c[22] = float(REXCVAR_GET(skate3_native_render_scene_ssr_intensity));
  c[23] = float(REXCVAR_GET(skate3_native_render_scene_ssr_debug));
  c[24] = 250.0f;  // max ray distance (view units; the screen clip governs)
  // Build-up showcase gate (p2, see ssr.hlsl ps_composite): the composite
  // side whose layer mask has not revealed reflections outputs alpha 0.
  // Rows carry 256 + mask (0 = showcase off); SSR is mask bit 32.
  const auto ssr_visible = [](float row) {
    return (row < 255.5f || ((uint32_t(row + 0.5f) - 256u) & 32u) != 0u)
               ? 1.0f
               : 0.0f;
  };
  c[28] = g_r.showcase_rows[2];
  c[29] = ssr_visible(g_r.showcase_rows[0]);
  c[30] = ssr_visible(g_r.showcase_rows[1]);

  cmd->SetBindingLayout(g_r.ssr_layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->SetRootConstants(0, 32, c, 0);

  // 1) Own linearize when AO did not produce the plane this frame.
  if (!lin_depth_ready) {
    cmd->Barrier(g_r.depth, nrhi::ResourceState::kDepthWrite,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetRenderTargets(g_r.ssr_lin_depth, nullptr);
    cmd->SetPipeline(g_r.pso_ssr_linearize);
    cmd->SetTexture(1, g_r.ssr_depth_srv);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.depth, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kDepthWrite);
  }
  cmd->Barrier(lin_tex, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);

  // 2) March (half res): the HDR plane pauses as a sampled source.
  cmd->Barrier(g_r.hdr_resolved, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  const nrhi::Viewport ssr_vp{0.0f, 0.0f, float(g_r.ssr_width),
                              float(g_r.ssr_height), 0.0f, 1.0f};
  const nrhi::Rect ssr_sc{0, 0, int32_t(g_r.ssr_width),
                          int32_t(g_r.ssr_height)};
  cmd->SetViewport(ssr_vp);
  cmd->SetScissor(ssr_sc);
  cmd->SetRenderTargets(g_r.ssr_tex, nullptr);
  cmd->SetPipeline(g_r.pso_ssr_march);
  cmd->SetTexture(1, g_r.ssr_gbuf_srv);
  cmd->SetTexture(2, lin_srv);
  cmd->SetTexture(3, g_r.hdr_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.ssr_tex, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->Barrier(g_r.hdr_resolved, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();

  // 3) Composite (full res, src-alpha blend onto the HDR plane). Dest-size
  // constants switch to the full raster; the rest carries over.
  c[0] = float(width);
  c[1] = float(height);
  c[2] = 1.0f / float(width);
  c[3] = 1.0f / float(height);
  cmd->SetRootConstants(0, 4, c, 0);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  cmd->SetRenderTargets(g_r.hdr_resolved, nullptr);
  cmd->SetPipeline(g_r.pso_ssr_composite);
  cmd->SetTexture(1, g_r.ssr_srv);
  cmd->SetTexture(2, g_r.ssr_gbuf_srv);
  cmd->SetTexture(3, lin_srv);
  cmd->Draw(3, 0);

  // Restore intermediate steady states.
  cmd->Barrier(g_r.ssr_tex, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(lin_tex, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(g_r.ssr_gbuf, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  return true;
}

// ---- Volumetric lighting (hdr.hlsl ps_vol_*: sun shafts + haze) ---------
// PSOs under the HDR binding layout, built lazily on the first enabled frame
// and rebuilt when the MSAA level (linearize variant) changes. Failure
// disables the effect only.
bool EnsureVolumetricPipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.vol_failed || !g_r.hdr_active || g_r.hdr_layout == nullptr) {
    return false;
  }
  if (g_r.pso_vol_shafts != nullptr && g_r.vol_msaa == g_r.msaa) {
    return true;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: volumetric pipeline setup failed ({})", what);
    g_r.vol_failed = true;
    return false;
  };
  for (nrhi::Pipeline** p : {&g_r.pso_vol_linearize, &g_r.pso_vol_shafts,
                             &g_r.pso_vol_blur}) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }
  const bool msaa = g_r.msaa > 1;
  const nrhi::ShaderMacro msaa_defs[] = {{"VOL_MSAA", "1"},
                                         {nullptr, nullptr}};
  nrhi::Shader* vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "hdr.hlsl", kHdrShaderSource,
                     "vs_main", nullptr, ""));
  nrhi::Shader* ps_lin = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource,
      "ps_vol_linearize", msaa ? msaa_defs : nullptr,
      msaa ? "VOL_MSAA=1" : ""));
  nrhi::Shader* ps_shafts = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource,
                     "ps_vol_shafts", nullptr, ""));
  // The 3x3 tent blur reuses ps_bloom_up (in a non-blended PSO): it
  // integrates the march jitter and softens shadow-volume edges.
  nrhi::Shader* ps_blur = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource,
                     "ps_bloom_up", nullptr, ""));
  const bool shaders_ok = vs != nullptr && ps_lin != nullptr &&
                          ps_shafts != nullptr && ps_blur != nullptr;
  if (!shaders_ok) {
    for (nrhi::Shader* s : {vs, ps_lin, ps_shafts, ps_blur}) {
      device->DestroyDeferred(s);
    }
    return fail("shader compile");
  }
  nrhi::GraphicsPipelineDesc pso;
  pso.layout = g_r.hdr_layout;
  pso.vs = vs;
  pso.cull = nrhi::CullMode::kNone;
  pso.sample_count = 1;
  pso.ps = ps_lin;
  pso.rtv_format = nrhi::Format::kR32_FLOAT;
  g_r.pso_vol_linearize = device->CreateGraphicsPipeline(pso);
  pso.rtv_format = nrhi::Format::kR16G16B16A16_FLOAT;
  pso.ps = ps_shafts;
  g_r.pso_vol_shafts = device->CreateGraphicsPipeline(pso);
  pso.ps = ps_blur;
  g_r.pso_vol_blur = device->CreateGraphicsPipeline(pso);
  for (nrhi::Shader* s : {vs, ps_lin, ps_shafts, ps_blur}) {
    device->DestroyDeferred(s);
  }
  if (g_r.pso_vol_linearize == nullptr || g_r.pso_vol_shafts == nullptr ||
      g_r.pso_vol_blur == nullptr) {
    return fail("pso");
  }
  g_r.vol_msaa = g_r.msaa;
  REXLOG_INFO("native-scene: volumetric pipeline created (MSAA x{})",
              g_r.msaa);
  return true;
}

// General 4x4 inverse (Gauss-Jordan). Returns false on a singular matrix.
static bool Invert4x4(const float m[16], float out[16]) {
  float a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0f : 0.0f;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
        pivot = r;
      }
    }
    if (std::fabs(a[pivot][col]) < 1e-12f) {
      return false;
    }
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) {
        std::swap(a[col][c], a[pivot][c]);
      }
    }
    const float inv = 1.0f / a[col][col];
    for (int c = 0; c < 8; ++c) {
      a[col][c] *= inv;
    }
    for (int r = 0; r < 4; ++r) {
      if (r == col) {
        continue;
      }
      const float f = a[r][col];
      for (int c = 0; c < 8; ++c) {
        a[r][c] -= f * a[col][c];
      }
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = a[r][c + 4];
    }
  }
  return true;
}

// Shadow-marched sun shafts + the constant staging for the haze term
// ps_tonemap evaluates. Runs between the SSR composite and the HDR post.
// Leaves the shaft plane and the linear-depth plane in
// PIXEL_SHADER_RESOURCE for ps_tonemap (ApplyHdrPost restores both).
// Returns true when it staged anything; the binding layout changed and
// the caller's post_ran block restores the main-pass bindings.
bool ApplyVolumetricPass(const NativeGuestOutputRenderContext& context,
                         nrhi::Cmd* cmd, const FrameScene& scene,
                         const nrhi::Viewport& viewport,
                         const nrhi::Rect& scissor, bool ssao_ran,
                         uint64_t frame_number) {
  // Shafts need this frame's shadow state: the receiver constants (b1
  // slice), the blurred CSM atlas left in PIXEL_SHADER_RESOURCE by the
  // shadow pass, and a valid capture. The haze needs only the sun capture.
  const bool shafts = REXCVAR_GET(skate3_native_render_scene_shafts) &&
                      scene.shadow_valid && g_r.shadow_cb != nullptr &&
                      g_r.shadow_srv_final != nullptr &&
                      g_r.shadow_in_srv_state;
  const bool haze_want =
      REXCVAR_GET(skate3_native_render_scene_haze) && scene.sky_sun_valid;
  if ((!shafts && !haze_want) || g_r.hdr_resolved == nullptr ||
      g_r.hdr_srv == nullptr) {
    return false;
  }
  // The published projection must be the live perspective matrix.
  const float* pr = scene.proj;
  if (pr[11] != 1.0f || pr[0] == 0.0f || pr[5] == 0.0f || pr[14] == 0.0f) {
    return false;
  }
  if (!EnsureVolumetricPipeline(context)) {
    return false;
  }
  nrhi::Device* device = context.device;
  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;

  // ---- Full-res linear view-Z: the SSAO plane when AO linearized this
  // frame (it idles back in RENDER_TARGET after the AO pass), else the own
  // linearize below.
  bool lin_ready = ssao_ran && g_r.ao_lin_depth != nullptr &&
                   g_r.ao_lin_srv != nullptr && g_r.ao_lin_width == width &&
                   g_r.ao_lin_height == height;
  if (!lin_ready) {
    if (g_r.vol_lin_depth == nullptr || g_r.vol_lin_width != width ||
        g_r.vol_lin_height != height) {
      if (g_r.vol_lin_srv != nullptr) {
        device->DestroyDeferred(g_r.vol_lin_srv);
        g_r.vol_lin_srv = nullptr;
      }
      if (g_r.vol_lin_depth != nullptr) {
        device->DestroyDeferred(g_r.vol_lin_depth);
        g_r.vol_lin_depth = nullptr;
      }
      nrhi::TextureDesc desc;
      desc.width = width;
      desc.height = height;
      desc.mip_levels = 1;
      desc.format = nrhi::Format::kR32_FLOAT;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.vol_lin_depth = device->CreateTexture(desc);
      if (g_r.vol_lin_depth == nullptr) {
        g_r.vol_failed = true;
        return false;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.vol_lin_srv = device->CreateTextureView(g_r.vol_lin_depth, vd);
      if (g_r.vol_lin_srv == nullptr) {
        g_r.vol_failed = true;
        return false;
      }
      g_r.vol_lin_width = width;
      g_r.vol_lin_height = height;
    }
    // Scene-depth SRV, re-pointed when the depth buffer is rebuilt.
    if (g_r.vol_depth_srv == nullptr || g_r.vol_depth_srv_of != g_r.depth) {
      if (g_r.vol_depth_srv != nullptr) {
        device->DestroyDeferred(g_r.vol_depth_srv);
        g_r.vol_depth_srv = nullptr;
      }
      nrhi::TextureViewDesc sd_desc;
      if (g_r.msaa > 1) {
        sd_desc.dimension = nrhi::ViewDimension::k2DMS;
      } else {
        sd_desc.dimension = nrhi::ViewDimension::k2D;
        sd_desc.mip_levels = 1;
      }
      g_r.vol_depth_srv = device->CreateTextureView(g_r.depth, sd_desc);
      if (g_r.vol_depth_srv == nullptr) {
        g_r.vol_failed = true;
        return false;
      }
      g_r.vol_depth_srv_of = g_r.depth;
    }
  }

  // ---- Shaft planes (march + blurred), rebuilt on resize or divisor
  // change; the tonemap upsample reads the divisor from vs0.x.
  const uint32_t vol_div = uint32_t(
      std::clamp<int32_t>(REXCVAR_GET(skate3_native_render_scene_shafts_res),
                          2, 8));
  const uint32_t vw = std::max(1u, (width + vol_div - 1) / vol_div);
  const uint32_t vh = std::max(1u, (height + vol_div - 1) / vol_div);
  if (shafts &&
      (g_r.vol_width != vw || g_r.vol_height != vh || g_r.vol_tex == nullptr ||
       g_r.vol_tex_b == nullptr)) {
    nrhi::Texture** texs[2] = {&g_r.vol_tex, &g_r.vol_tex_b};
    nrhi::TextureView** views[2] = {&g_r.vol_srv, &g_r.vol_srv_b};
    for (int i = 0; i < 2; ++i) {
      if (*views[i] != nullptr) {
        device->DestroyDeferred(*views[i]);
        *views[i] = nullptr;
      }
      if (*texs[i] != nullptr) {
        device->DestroyDeferred(*texs[i]);
        *texs[i] = nullptr;
      }
      nrhi::TextureDesc desc;
      desc.width = vw;
      desc.height = vh;
      desc.mip_levels = 1;
      desc.format = nrhi::Format::kR16G16B16A16_FLOAT;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      *texs[i] = device->CreateTexture(desc);
      if (*texs[i] == nullptr) {
        g_r.vol_failed = true;
        return false;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      *views[i] = device->CreateTextureView(*texs[i], vd);
      if (*views[i] == nullptr) {
        g_r.vol_failed = true;
        return false;
      }
    }
    g_r.vol_width = vw;
    g_r.vol_height = vh;
  }

  // Sun direction in the AO view space (the projection's per-axis signs
  // folded into the world->view rotation, exactly as the SSR pass builds
  // it): the space ps_tonemap reconstructs view rays in for the haze.
  const float* sd = scene.sky_sun;  // [0..2] = unit vector toward the sun
  const float* vpm = scene.view_proj;
  const float sxs = pr[0] < 0.0f ? -1.0f : 1.0f;
  const float sys = pr[5] < 0.0f ? -1.0f : 1.0f;
  const float inv00 = 1.0f / pr[0];
  const float inv11 = 1.0f / pr[5];
  float sun_view[3] = {
      sd[0] * vpm[0] * inv00 * sxs + sd[1] * vpm[4] * inv00 * sxs +
          sd[2] * vpm[8] * inv00 * sxs,
      sd[0] * vpm[1] * inv11 * sys + sd[1] * vpm[5] * inv11 * sys +
          sd[2] * vpm[9] * inv11 * sys,
      sd[0] * vpm[3] + sd[1] * vpm[7] + sd[2] * vpm[11]};
  const float svl = std::sqrt(sun_view[0] * sun_view[0] +
                              sun_view[1] * sun_view[1] +
                              sun_view[2] * sun_view[2]);
  if (svl > 1e-6f) {
    sun_view[0] /= svl;
    sun_view[1] /= svl;
    sun_view[2] /= svl;
  }
  // Haze tint: the frame's captured linear fog color folded through the
  // scene exposure: the fully-fogged xe value, so the haze always matches
  // the game's own atmosphere (and fades out at night with it).
  const float expo = scene.sky_sun[5] > 0.0f ? scene.sky_sun[5] : 2.5f;
  const float haze_int =
      haze_want ? float(REXCVAR_GET(skate3_native_render_scene_haze_intensity))
                : 0.0f;
  const float shaft_int =
      shafts ? float(REXCVAR_GET(skate3_native_render_scene_shafts_intensity))
             : 0.0f;

  // The march reconstructs world positions through the inverse
  // view-projection.
  float inv_vp[16];
  const bool inv_ok = Invert4x4(scene.view_proj, inv_vp);

  const uint32_t cb_offset =
      uint32_t(frame_number % RendererState::kShadowCbRegions) *
      RendererState::kShadowCbSlice;
  cmd->SetBindingLayout(g_r.hdr_layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);

  float c[32] = {};
  // 1) Own linearize when AO did not produce the plane this frame (needs
  // the m22/m32 rows in the vp slot).
  nrhi::Texture* lin_tex = lin_ready ? g_r.ao_lin_depth : g_r.vol_lin_depth;
  nrhi::TextureView* lin_srv = lin_ready ? g_r.ao_lin_srv : g_r.vol_lin_srv;
  if (!lin_ready) {
    c[16] = std::fabs(pr[0]);
    c[17] = std::fabs(pr[5]);
    c[18] = pr[10];
    c[19] = pr[14];
    cmd->SetRootConstants(0, 32, c, 0);
    cmd->Barrier(g_r.depth, nrhi::ResourceState::kDepthWrite,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetRenderTargets(g_r.vol_lin_depth, nullptr);
    cmd->SetPipeline(g_r.pso_vol_linearize);
    cmd->SetTexture(1, g_r.vol_depth_srv);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.depth, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kDepthWrite);
  }
  cmd->Barrier(lin_tex, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();

  // 2) Shadow-marched shafts (half res): world-space camera->pixel march
  // testing per-step sun visibility against the CSM atlas + the native
  // static sun-shadow map, with the baked world-shadow map as the fallback
  // outside its coverage (see ps_vol_shafts).
  bool shaft_plane = false;
  if (shafts && inv_ok && g_r.vol_tex != nullptr && g_r.vol_tex_b != nullptr) {
    const nrhi::Viewport vol_vp{0.0f, 0.0f, float(vw), float(vh), 0.0f, 1.0f};
    const nrhi::Rect vol_sc{0, 0, int32_t(vw), int32_t(vh)};
    c[0] = float(vw);
    c[1] = float(vh);
    c[2] = 1.0f / float(vw);
    c[3] = 1.0f / float(vh);
    c[4] = scene.cam_pos[0];
    c[5] = scene.cam_pos[1];
    c[6] = scene.cam_pos[2];
    c[7] = float(REXCVAR_GET(skate3_native_render_scene_shafts_reach));
    c[8] = float(REXCVAR_GET(skate3_native_render_scene_shafts_steps));
    c[10] = 0.6f;  // Henyey-Greenstein forward-scatter anisotropy
    c[12] = pr[10];
    c[13] = pr[14];
    std::memcpy(c + 16, inv_vp, sizeof(inv_vp));
    cmd->SetRootConstants(0, 32, c, 0);
    cmd->SetViewport(vol_vp);
    cmd->SetScissor(vol_sc);
    cmd->SetRenderTargets(g_r.vol_tex, nullptr);
    cmd->SetPipeline(g_r.pso_vol_shafts);
    cmd->SetTexture(2, lin_srv);                 // t1 = linear depth
    cmd->SetTexture(3, g_r.shadow_srv_final);    // t2 = CSM atlas
    // t3 = the static world-shadow map when primed (white is neutral: the
    // stored depth 1 compares lit everywhere).
    cmd->SetTexture(4, (g_r.world_shadow_srv != nullptr &&
                        g_r.world_shadow_in_srv)
                           ? g_r.world_shadow_srv
                           : g_r.white.srv);
    // t4 = the native static sun-shadow atlas (the scene pass's t10) when
    // rendered this frame; the march prefers it over the ws map inside its
    // coverage so static shafts follow the true material-sun axis. The b1
    // nsm rows are zeroed when the map is invalid, so the white fallback is
    // never actually compared.
    cmd->SetTexture(5, (g_r.static_sun_valid && g_r.static_sun_in_srv &&
                        g_r.static_sun_srv != nullptr)
                           ? g_r.static_sun_srv
                           : g_r.white.srv);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.vol_tex, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    // 3x3 tent blur (integrates the march jitter, softens shadow-volume
    // edges); the blurred plane is what ps_tonemap samples.
    c[4] = float(vw);
    c[5] = float(vh);
    c[6] = 1.0f / float(vw);
    c[7] = 1.0f / float(vh);
    cmd->SetRootConstants(0, 8, c, 0);
    cmd->SetRenderTargets(g_r.vol_tex_b, nullptr);
    cmd->SetPipeline(g_r.pso_vol_blur);
    cmd->SetTexture(1, g_r.vol_srv);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.vol_tex, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->Barrier(g_r.vol_tex_b, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    shaft_plane = true;
  }

  // 3) Stage ps_tonemap's volumetric rows + the plane handoffs.
  g_r.vol_rows[0] = std::fabs(pr[0]);
  g_r.vol_rows[1] = std::fabs(pr[5]);
  g_r.vol_rows[2] = pr[10];
  g_r.vol_rows[3] = pr[14];
  g_r.vol_rows[4] = float(vol_div);
  g_r.vol_rows[5] = 0.0f;
  g_r.vol_rows[6] = 0.0f;
  g_r.vol_rows[7] = shaft_plane ? shaft_int : 0.0f;
  g_r.vol_rows[8] = scene.fog_color[0] * expo;
  g_r.vol_rows[9] = scene.fog_color[1] * expo;
  g_r.vol_rows[10] = scene.fog_color[2] * expo;
  g_r.vol_rows[11] = haze_int;
  g_r.vol_rows[12] = sun_view[0];
  g_r.vol_rows[13] = sun_view[1];
  g_r.vol_rows[14] = sun_view[2];
  g_r.vol_rows[15] =
      float(REXCVAR_GET(skate3_native_render_scene_haze_density));
  g_r.vol_tonemap_valid = true;
  g_r.vol_plane_in_psr = shaft_plane;
  g_r.vol_lin_plane = lin_tex;
  g_r.vol_lin_plane_srv = lin_srv;
  g_r.vol_lin_in_psr = true;
  // Throttled diagnostics: the terms degrade to zero silently when the
  // shadow state or the fog capture is missing; log what decides them.
  static uint32_t s_vol_log = 0;
  if (s_vol_log < 4 || (s_vol_log % 36000) == 0) {
    REXLOG_INFO(
        "native-scene: vol2 shafts={} gates[cvar={} shvalid={} atlas={} "
        "insrv={}] inv={} ws={} nsm={} tex={} tint=({:.3f},{:.3f},{:.3f}) "
        "expo={:.2f} sunv=({:.2f},{:.2f},{:.2f}) lin={}",
        shaft_plane ? 1 : 0,
        REXCVAR_GET(skate3_native_render_scene_shafts) ? 1 : 0,
        scene.shadow_valid ? 1 : 0, g_r.shadow_srv_final != nullptr ? 1 : 0,
        g_r.shadow_in_srv_state ? 1 : 0, inv_ok ? 1 : 0,
        g_r.world_shadow_in_srv ? 1 : 0,
        (g_r.static_sun_valid && g_r.static_sun_in_srv) ? 1 : 0,
        g_r.vol_tex != nullptr ? 1 : 0,
        g_r.vol_rows[8], g_r.vol_rows[9], g_r.vol_rows[10], expo, sun_view[0],
        sun_view[1], sun_view[2], lin_ready ? "ao" : "own");
  }
  ++s_vol_log;
  return true;
}

// ---- HDR post chain (hdr.hlsl: bloom pyramid + the extracted tonemap) ----
// Layout + PSOs, built when the HDR path activates and rebuilt when the
// guest output format (the tonemap target) changes. Failure falls back to
// the classic in-material tonemap instead of killing the native renderer.
bool EnsureHdrPipeline(const NativeGuestOutputRenderContext& context) {
  if (!g_r.hdr_active || g_r.hdr_failed) {
    return false;
  }
  if (g_r.pso_tonemap != nullptr &&
      g_r.hdr_pso_out_format == context.guest_output->format() &&
      g_r.hdr_showcase == g_r.showcase_shaders) {
    return true;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_WARN(
        "native-scene: HDR pipeline setup failed ({}), falling back to the "
        "classic tonemap path",
        what);
    g_r.hdr_failed = true;
    return false;
  };
  if (g_r.hdr_layout == nullptr) {
    // The SSAO layout shape widened for the fused tonemap consumers: root
    // constants b0 (32 floats: the tail rows carry the volumetric params)
    // + five single-texture tables (t2 = the fused AO multiplier plane,
    // t3 = the shaft plane, t4 = linear depth) + point/linear clamp
    // samplers + the per-frame shadow constant slice at b1 (the volumetric
    // march samples sun visibility with the material receiver math). The
    // CBV is APPENDED so the table param indices stay 1..5 (Vulkan's set-0
    // derivation orders buffer params by declaration order regardless).
    nrhi::BindingLayoutDesc ld;
    ld.param_count = 7;
    ld.params[0] = {nrhi::BindingParamKind::kConstants, /*b*/ 0, 32,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 1,
                    nrhi::Visibility::kPixel};
    ld.params[2] = {nrhi::BindingParamKind::kTextureTable, 1, 1,
                    nrhi::Visibility::kPixel};
    ld.params[3] = {nrhi::BindingParamKind::kTextureTable, 2, 1,
                    nrhi::Visibility::kPixel};
    ld.params[4] = {nrhi::BindingParamKind::kTextureTable, 3, 1,
                    nrhi::Visibility::kPixel};
    ld.params[5] = {nrhi::BindingParamKind::kTextureTable, 4, 1,
                    nrhi::Visibility::kPixel};
    ld.params[6] = {nrhi::BindingParamKind::kConstantBuffer, /*b*/ 1, 1,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 2;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kPoint,
                             nrhi::AddressMode::kClamp, 1};
    ld.static_samplers[1] = {1, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.allow_input_layout = false;
    g_r.hdr_layout = device->CreateBindingLayout(ld);
    if (g_r.hdr_layout == nullptr) {
      return fail("binding layout");
    }
  }
  for (nrhi::Pipeline** p : {&g_r.pso_bloom_first, &g_r.pso_bloom_down,
                             &g_r.pso_bloom_up, &g_r.pso_tonemap}) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }
  const nrhi::ShaderMacro first_defs[] = {{"BLOOM_FIRST", "1"},
                                          {nullptr, nullptr}};
  nrhi::Shader* vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "hdr.hlsl", kHdrShaderSource,
                     "vs_main", nullptr, ""));
  nrhi::Shader* ps_first = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource,
      "ps_bloom_down", first_defs, "BLOOM_FIRST=1"));
  nrhi::Shader* ps_down = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource,
                     "ps_bloom_down", nullptr, ""));
  nrhi::Shader* ps_up = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource,
                     "ps_bloom_up", nullptr, ""));
  // The tonemap follows the showcase shader swap (its SHOWCASE=1 variant
  // carries the post-layer gates and the split divider).
  const nrhi::ShaderMacro sc_defs[] = {{"SHOWCASE", "1"}, {nullptr, nullptr}};
  nrhi::Shader* ps_tone = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "hdr.hlsl", kHdrShaderSource, "ps_tonemap",
      g_r.showcase_shaders ? sc_defs : nullptr,
      g_r.showcase_shaders ? "SHOWCASE=1" : ""));
  const bool shaders_ok = vs != nullptr && ps_first != nullptr &&
                          ps_down != nullptr && ps_up != nullptr &&
                          ps_tone != nullptr;
  if (!shaders_ok) {
    for (nrhi::Shader* s : {vs, ps_first, ps_down, ps_up, ps_tone}) {
      device->DestroyDeferred(s);
    }
    return fail("shader compile");
  }
  nrhi::GraphicsPipelineDesc pso;
  pso.layout = g_r.hdr_layout;
  pso.vs = vs;
  pso.cull = nrhi::CullMode::kNone;
  pso.sample_count = 1;
  pso.rtv_format = nrhi::Format::kR16G16B16A16_FLOAT;
  pso.ps = ps_first;
  g_r.pso_bloom_first = device->CreateGraphicsPipeline(pso);
  pso.ps = ps_down;
  g_r.pso_bloom_down = device->CreateGraphicsPipeline(pso);
  // Progressive upsample: additive ONE/ONE onto the next-larger level.
  pso.ps = ps_up;
  pso.blend.enable = true;
  pso.blend.src = nrhi::BlendFactor::kOne;
  pso.blend.dst = nrhi::BlendFactor::kOne;
  pso.blend.op = nrhi::BlendOp::kAdd;
  pso.blend.src_alpha = nrhi::BlendFactor::kOne;
  pso.blend.dst_alpha = nrhi::BlendFactor::kOne;
  pso.blend.op_alpha = nrhi::BlendOp::kAdd;
  g_r.pso_bloom_up = device->CreateGraphicsPipeline(pso);
  pso.blend = {};
  pso.ps = ps_tone;
  pso.rtv_format = context.guest_output->format();
  g_r.pso_tonemap = device->CreateGraphicsPipeline(pso);
  for (nrhi::Shader* s : {vs, ps_first, ps_down, ps_up, ps_tone}) {
    device->DestroyDeferred(s);
  }
  if (g_r.pso_bloom_first == nullptr || g_r.pso_bloom_down == nullptr ||
      g_r.pso_bloom_up == nullptr || g_r.pso_tonemap == nullptr) {
    return fail("pso");
  }
  g_r.hdr_pso_out_format = context.guest_output->format();
  g_r.hdr_showcase = g_r.showcase_shaders;
  REXLOG_INFO("native-scene: HDR post pipeline created");
  return true;
}

// Bloom pyramid + tonemap over the float scene plane into the guest output.
// Runs after the AO composite (bloom sees the occluded scene). Transitions
// the guest output kGuestOutput -> kRenderTarget (the state every later
// consumer expects); the caller restores the main binding layout after.
void ApplyHdrPost(const NativeGuestOutputRenderContext& context,
                  nrhi::Cmd* cmd, const nrhi::Viewport& viewport,
                  const nrhi::Rect& scissor, bool loading_native,
                  uint64_t frame_number) {
  nrhi::Device* device = context.device;
  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;
  // Bloom chain intermediates: QUARTER res halving down to the level cap or
  // an 8-px floor, RGBA16F, steady state RENDER_TARGET. Quarter start =
  // 1/4 the pyramid cost of a half-res chain; the extraction pass widens
  // its 13-tap spread (p1.x = 2) so the 4x reduction still integrates the
  // whole source block. Creation failure only loses bloom (the tonemap
  // still runs).
  if (g_r.bloom_base_w != width || g_r.bloom_base_h != height ||
      (g_r.bloom_levels > 0 && g_r.bloom_tex[0] == nullptr)) {
    for (uint32_t i = 0; i < RendererState::kBloomMaxLevels; ++i) {
      if (g_r.bloom_srv[i] != nullptr) {
        device->DestroyDeferred(g_r.bloom_srv[i]);
        g_r.bloom_srv[i] = nullptr;
      }
      if (g_r.bloom_tex[i] != nullptr) {
        device->DestroyDeferred(g_r.bloom_tex[i]);
        g_r.bloom_tex[i] = nullptr;
      }
    }
    g_r.bloom_levels = 0;
    uint32_t w = std::max(1u, (width + 1) / 2);
    uint32_t h = std::max(1u, (height + 1) / 2);
    for (uint32_t i = 0; i < RendererState::kBloomMaxLevels; ++i) {
      w = std::max(1u, (w + 1) / 2);
      h = std::max(1u, (h + 1) / 2);
      if (w < 8 || h < 8) {
        break;
      }
      nrhi::TextureDesc desc;
      desc.width = w;
      desc.height = h;
      desc.mip_levels = 1;
      desc.format = nrhi::Format::kR16G16B16A16_FLOAT;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.bloom_tex[i] = device->CreateTexture(desc);
      if (g_r.bloom_tex[i] == nullptr) {
        break;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.bloom_srv[i] = device->CreateTextureView(g_r.bloom_tex[i], vd);
      if (g_r.bloom_srv[i] == nullptr) {
        device->DestroyDeferred(g_r.bloom_tex[i]);
        g_r.bloom_tex[i] = nullptr;
        break;
      }
      g_r.bloom_w[i] = w;
      g_r.bloom_h[i] = h;
      ++g_r.bloom_levels;
    }
    g_r.bloom_base_w = width;
    g_r.bloom_base_h = height;
    if (g_r.bloom_levels == 0) {
      REXLOG_WARN("native-scene: bloom chain allocation failed (bloom off)");
    }
  }
  const bool bloom = REXCVAR_GET(skate3_native_render_scene_bloom) &&
                     !loading_native && g_r.bloom_levels > 0 &&
                     g_r.pso_bloom_first != nullptr &&
                     g_r.pso_bloom_down != nullptr &&
                     g_r.pso_bloom_up != nullptr;

  cmd->SetBindingLayout(g_r.hdr_layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  // The layout carries the shadow constant slice at b1 (the volumetric
  // march); bind it for every draw on this layout; the bloom/tonemap
  // shaders never read it, but the descriptor must hold a valid buffer.
  if (g_r.shadow_cb != nullptr) {
    const uint32_t cb_offset =
        uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
    cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
  }
  // The scene plane is the sampled source for the whole chain.
  cmd->Barrier(g_r.hdr_resolved, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();

  float c[32] = {};
  // Rows 16-31 = the volumetric params ApplyVolumetricPass staged this
  // frame (zeros otherwise; every volumetric term drops out).
  if (g_r.vol_tonemap_valid) {
    std::memcpy(c + 16, g_r.vol_rows, sizeof(g_r.vol_rows));
  }
  const auto set_consts = [&](uint32_t dw, uint32_t dh, uint32_t sw,
                              uint32_t sh, float intensity, float tap_scale) {
    c[0] = float(dw);
    c[1] = float(dh);
    c[2] = 1.0f / float(dw);
    c[3] = 1.0f / float(dh);
    c[4] = float(sw);
    c[5] = float(sh);
    c[6] = 1.0f / float(sw);
    c[7] = 1.0f / float(sh);
    c[8] = float(REXCVAR_GET(skate3_native_render_scene_bloom_threshold));
    c[9] = float(REXCVAR_GET(skate3_native_render_scene_bloom_knee));
    c[10] = intensity;
    // Debug view (ps_tonemap p0.w); the classic ssao_debug cvar maps onto
    // the fused AO view when the plane rode along this frame.
    c[11] = float(REXCVAR_GET(skate3_native_render_scene_hdr_debug));
    if (c[11] == 0.0f && g_r.ao_plane_in_psr &&
        REXCVAR_GET(skate3_native_render_scene_ssao_debug)) {
      c[11] = 3.0f;
    }
    c[12] = tap_scale;
    c[13] = c[14] = c[15] = 0.0f;
    cmd->SetRootConstants(0, 32, c, 0);
  };

  if (bloom) {
    // Downsample chain (level 0 extracts with threshold + Karis weighting;
    // its 4x reduction doubles the tap spread to cover the source block).
    for (uint32_t i = 0; i < g_r.bloom_levels; ++i) {
      const uint32_t sw = i == 0 ? width : g_r.bloom_w[i - 1];
      const uint32_t sh = i == 0 ? height : g_r.bloom_h[i - 1];
      set_consts(g_r.bloom_w[i], g_r.bloom_h[i], sw, sh, 0.0f,
                 i == 0 ? 2.0f : 1.0f);
      const nrhi::Viewport vp{0.0f, 0.0f, float(g_r.bloom_w[i]),
                              float(g_r.bloom_h[i]), 0.0f, 1.0f};
      const nrhi::Rect sc{0, 0, int32_t(g_r.bloom_w[i]),
                          int32_t(g_r.bloom_h[i])};
      cmd->SetViewport(vp);
      cmd->SetScissor(sc);
      cmd->SetRenderTargets(g_r.bloom_tex[i], nullptr);
      cmd->SetPipeline(i == 0 ? g_r.pso_bloom_first : g_r.pso_bloom_down);
      cmd->SetTexture(1, i == 0 ? g_r.hdr_srv : g_r.bloom_srv[i - 1]);
      cmd->Draw(3, 0);
      cmd->Barrier(g_r.bloom_tex[i], nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
    }
    // Progressive tent upsample, additive onto each larger level.
    for (int32_t i = int32_t(g_r.bloom_levels) - 2; i >= 0; --i) {
      cmd->Barrier(g_r.bloom_tex[i], nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      set_consts(g_r.bloom_w[i], g_r.bloom_h[i], g_r.bloom_w[i + 1],
                 g_r.bloom_h[i + 1], 0.0f, 1.0f);
      const nrhi::Viewport vp{0.0f, 0.0f, float(g_r.bloom_w[i]),
                              float(g_r.bloom_h[i]), 0.0f, 1.0f};
      const nrhi::Rect sc{0, 0, int32_t(g_r.bloom_w[i]),
                          int32_t(g_r.bloom_h[i])};
      cmd->SetViewport(vp);
      cmd->SetScissor(sc);
      cmd->SetRenderTargets(g_r.bloom_tex[i], nullptr);
      cmd->SetPipeline(g_r.pso_bloom_up);
      cmd->SetTexture(1, g_r.bloom_srv[i + 1]);
      cmd->Draw(3, 0);
      cmd->Barrier(g_r.bloom_tex[i], nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
    }
  }

  // Tonemap into the guest output (the single application of the game's
  // shared tone chain; bloom energy joins pre-tonemap).
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  set_consts(width, height, width, height,
             bloom ? float(REXCVAR_GET(
                         skate3_native_render_scene_bloom_intensity))
                   : 0.0f,
             1.0f);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  cmd->SetRenderTargets(context.guest_output, nullptr);
  cmd->SetPipeline(g_r.pso_tonemap);
  cmd->SetTexture(1, g_r.hdr_srv);
  cmd->SetTexture(2, bloom ? g_r.bloom_srv[0] : g_r.white.srv);
  // t2 = the fused AO multiplier plane (white = no AO this frame).
  cmd->SetTexture(3, g_r.ao_plane_in_psr ? g_r.ao_srv[0] : g_r.white.srv);
  // t3/t4 = the volumetric shaft plane + linear depth (white with
  // zero-weight constant rows when the pass did not run).
  cmd->SetTexture(4, g_r.vol_plane_in_psr ? g_r.vol_srv_b : g_r.white.srv);
  cmd->SetTexture(5, g_r.vol_lin_in_psr ? g_r.vol_lin_plane_srv
                                        : g_r.white.srv);
  cmd->Draw(3, 0);

  // Restore steady states.
  cmd->Barrier(g_r.hdr_resolved, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  if (g_r.ao_plane_in_psr) {
    cmd->Barrier(g_r.ao_tex[0], nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    g_r.ao_plane_in_psr = false;
  }
  if (g_r.vol_plane_in_psr) {
    cmd->Barrier(g_r.vol_tex_b, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    g_r.vol_plane_in_psr = false;
  }
  if (g_r.vol_lin_in_psr) {
    cmd->Barrier(g_r.vol_lin_plane, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    g_r.vol_lin_in_psr = false;
    g_r.vol_lin_plane = nullptr;
    g_r.vol_lin_plane_srv = nullptr;
  }
  g_r.vol_tonemap_valid = false;
  if (bloom) {
    for (uint32_t i = 0; i < g_r.bloom_levels; ++i) {
      cmd->Barrier(g_r.bloom_tex[i],
                   nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
    }
  }
  cmd->FlushBarriers();
}


// ---- Settings-menu backdrop blur (shared native + emulated-post paths) ----
// Clean separable gaussian over the finished guest output; deliberately
// shares nothing with the game's
// popup chain (that one reproduces the console's 1152x640 lattice on
// purpose). Downsample to half res (plain bilinear = 2x2 box), gaussian H,
// gaussian V, bilinear stretch back. Eased in/out (~60 ms) so open/close
// doesn't pop; the smoothing state is shared across the native and
// emulated-post paths (only one runs per frame). Returns true while the
// effect is active (target > 0 or still easing out).
bool ApplyMenuBlurPass(const NativeGuestOutputRenderContext& context, nrhi::Cmd* cmd,
                       float target_sigma, bool output_in_guest_output_state) {
  static float s_menu_sigma = 0.0f;
  static int64_t s_menu_ns = 0;
  const int64_t menu_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  PerfClock::now().time_since_epoch())
                                  .count();
  float menu_dt = s_menu_ns != 0 ? float(menu_now_ns - s_menu_ns) * 1e-9f : 1.0f;
  if (menu_dt > 0.25f) {
    s_menu_sigma = 0.0f;
    menu_dt = 0.0f;
  }
  s_menu_ns = menu_now_ns;
  const float menu_a = 1.0f - std::exp(-menu_dt / 0.06f);
  s_menu_sigma += (target_sigma - s_menu_sigma) * menu_a;
  const bool active = target_sigma > 0.0f || s_menu_sigma > 0.05f;
  if (s_menu_sigma <= 0.05f || g_r.pso_menu_gauss == nullptr ||
      g_r.pso_blur_blit == nullptr || g_r.menu_blur_tex[0] == nullptr ||
      g_r.output_srv_slot == nullptr) {
    return active;
  }
  const float w = float(g_r.menu_blur_w);
  const float h = float(g_r.menu_blur_h);
  // Sigma is authored in 1080p pixels; halve again for the half-res space.
  const float sigma_half = std::max(
      0.3f, s_menu_sigma * (float(context.guest_output_height) / 1080.0f) * 0.5f);
  const float radius = std::min(48.0f, std::ceil(sigma_half * 3.0f));
  const nrhi::Viewport half_vp{0.0f, 0.0f, w, h, 0.0f, 1.0f};
  const nrhi::Rect half_sc{0, 0, int32_t(g_r.menu_blur_w), int32_t(g_r.menu_blur_h)};
  const nrhi::Viewport full_vp{0.0f,
                               0.0f,
                               float(context.guest_output_width),
                               float(context.guest_output_height),
                               0.0f,
                               1.0f};
  const nrhi::Rect full_sc{0, 0, int32_t(context.guest_output_width),
                           int32_t(context.guest_output_height)};
  const nrhi::ResourceState output_entry_state = output_in_guest_output_state
                                                     ? nrhi::ResourceState::kGuestOutput
                                                     : nrhi::ResourceState::kRenderTarget;
  // The binding layout must be latched on this cmd before any draw; on the
  // emulated-post path this is a FRESH frame state where no layout has been
  // set, and nrhi draws silently no-op without one (EnsureDrawState).
  // Harmless on the native path (same layout, re-latch).
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  // Downsample: guest output -> menu[0].
  cmd->Barrier(context.guest_output, output_entry_state,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(g_r.menu_blur_tex[0], nullptr);
  cmd->SetViewport(half_vp);
  cmd->SetScissor(half_sc);
  cmd->SetPipeline(g_r.pso_blur_blit);
  cmd->SetTexture(1, g_r.output_srv_slot);
  cmd->Draw(3, 0);
  // H: menu[0] -> menu[1].
  cmd->Barrier(g_r.menu_blur_tex[0], nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(g_r.menu_blur_tex[1], nullptr);
  cmd->SetPipeline(g_r.pso_menu_gauss);
  const float h_consts[8] = {1.0f / w, 0.0f, sigma_half, radius, 1.0f, 1.0f, 1.0f, 1.0f};
  cmd->SetRootConstants(0, 8, h_consts, 0);
  cmd->SetTexture(1, g_r.menu_blur_srv[0]);
  cmd->Draw(3, 0);
  // V: menu[1] -> menu[0].
  cmd->Barrier(g_r.menu_blur_tex[1], nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->Barrier(g_r.menu_blur_tex[0], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(g_r.menu_blur_tex[0], nullptr);
  const float v_consts[8] = {0.0f, 1.0f / h, sigma_half, radius, 1.0f, 1.0f, 1.0f, 1.0f};
  cmd->SetRootConstants(0, 8, v_consts, 0);
  cmd->SetTexture(1, g_r.menu_blur_srv[1]);
  cmd->Draw(3, 0);
  // Stretch back over the full output.
  cmd->Barrier(g_r.menu_blur_tex[0], nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(context.guest_output, nullptr);
  cmd->SetViewport(full_vp);
  cmd->SetScissor(full_sc);
  cmd->SetPipeline(g_r.pso_blur_blit);
  cmd->SetTexture(1, g_r.menu_blur_srv[0]);
  cmd->Draw(3, 0);
  // Restore steady states (the intermediates stay in RENDER_TARGET; the
  // output goes back to the state the caller's path expects).
  cmd->Barrier(g_r.menu_blur_tex[0], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(g_r.menu_blur_tex[1], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  if (output_in_guest_output_state) {
    cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kGuestOutput);
    cmd->FlushBarriers();
  }
  return active;
}

// Minimal resource bring-up for the emulated-output post-processor path: the
// full EnsurePipeline never runs while the native renderer is disabled or
// yielding, so the blur ensures just what it needs (binding layout, the blur
// PSOs, the half-res intermediates, and a sampled view of the output).
bool EnsureMenuBlurStandalone(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) {
    return false;
  }
  if (!EnsureRootSignature(context)) {
    return false;
  }
  if (g_r.pso_menu_gauss == nullptr) {
    static bool s_attempted = false;
    if (s_attempted) {
      return false;  // creation failed once; don't hammer every frame
    }
    s_attempted = true;
    EnsureBlurPsos(context);
    if (g_r.pso_menu_gauss == nullptr) {
      return false;
    }
  }
  EnsureBlurOutlineTargets(context);
  if (g_r.menu_blur_tex[0] == nullptr) {
    return false;
  }
  // Sampled view of the output, backed by the per-mailbox-image cache (the
  // presenter rotates its guest-output image every refresh; the cached view
  // is reused on rotation instead of being recreated per frame).
  if (EnsureOutputSrvView(context) == nullptr) {
    return false;
  }
  return g_r.output_srv_slot != nullptr;
}

// Registered with the command processors: invoked at the end of the EMULATED
// guest-output refresh (native-rendered frames apply the blur inline in
// RenderScene). Covers boot/startup frames before the native scene takes
// over and manual emulated mode.
void PostProcessGuestOutput(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  const float target = g_settings_menu_blur.load(std::memory_order_relaxed)
                           ? float(REXCVAR_GET(skate3_menu_blur_sigma))
                           : 0.0f;
  // Diagnostics (throttled): the emulated-path blur has several silent
  // early-outs; log which leg each invocation takes.
  const bool log_this = g_post_blur_log_count.load(std::memory_order_relaxed) < 8;
  const bool ensured = EnsureMenuBlurStandalone(context);
  if (log_this) {
    g_post_blur_log_count.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO(
        "native-scene: emulated-post blur invoked (target={:.1f} ensured={} pso={} tex={} "
        "srv={} failed={})",
        target, ensured, (void*)g_r.pso_menu_gauss, (void*)g_r.menu_blur_tex[0],
        (void*)g_r.output_srv_slot, g_r.failed);
  }
  if (!ensured) {
    if (target <= 0.0f) {
      rex::graphics::RequestNativeGuestOutputPostProcess(false);
    }
    return;
  }
  if (!ApplyMenuBlurPass(context, context.cmd, target,
                         /*output_in_guest_output_state=*/true)) {
    // Fully eased out: stop the per-frame post-process invocations until the
    // menu opens again.
    rex::graphics::RequestNativeGuestOutputPostProcess(false);
  }
}

}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12 || REX_HAS_VULKAN
