#pragma once

// GPU-side internals of the native scene renderer, shared by its render
// translation units: skate3_native_scene_gpu.cpp (scene draw + presenter) and
// skate3_native_scene_post.cpp (SSAO/SSR/HDR/menu-blur post passes). Holds
// the RendererState (all GPU-resident resources) and the pipeline-builder
// declarations. Render thread only.

#include "skate3_native_scene_state.h"

#include <unordered_set>
#include <vector>

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)

#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace skate3::native_scene {


using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;
namespace xenos = rex::graphics::xenos;
namespace nrhi = rex::graphics::nrhi;

// App-local vertex/index binding records (replacing D3D12_VERTEX_BUFFER_VIEW /
// D3D12_INDEX_BUFFER_VIEW): the RHI binds (buffer, offset) directly, no GPU
// virtual addresses.
struct VbBinding {
  nrhi::Buffer* buffer = nullptr;
  uint64_t offset = 0;
  uint32_t size_bytes = 0;
  uint32_t stride = 0;
};
struct IbBinding {
  nrhi::Buffer* buffer = nullptr;
  uint64_t offset = 0;
  uint32_t size_bytes = 0;
};

struct MeshBuffers {
  nrhi::Buffer* vb = nullptr;
  nrhi::Buffer* ib = nullptr;
  VbBinding vb_view{};
  IbBinding ib_view{};
  uint64_t fingerprint = 0;
  // Dynamic-payload decode order (DynDecodeJob::seq): the commit drops
  // results older than the cached entry so multi-worker reordering cannot
  // step the cloth backwards a frame. 0 on static decodes.
  uint64_t dyn_seq = 0;
  // Double-sided sheet prop (banners/flags): most triangles have an
  // opposite-winding twin ~1cm behind, and the two faces map to DIFFERENT
  // lightmap atlas cells (lit vs shaded side). Drawn without culling both
  // faces z-fight once their depth gap drops below buffer precision;
  // distant triangles alternate between the two cells per frame (the "flag
  // flicker", stops up close where 1cm still resolves). These meshes draw
  // with the backface-culling PSO instead.
  bool two_sided_sheet = false;
  // ROPA only: the decoded vertex array (num_verts x 14 floats, the scene
  // VS layout) retained for draw-time shape blending onto the play clock.
  std::vector<float> ropa_verts;
  // Store LRU clock: last frame this entry served a draw. Meshes of
  // streamed-out areas age out instead of accumulating across map changes;
  // an evicted mesh re-decodes on miss exactly like first sight.
  uint64_t last_used_frame = 0;
};

struct GuestTexture {
  nrhi::Texture* texture = nullptr;
  nrhi::Buffer* upload = nullptr;  // kept alive; copy recorded in deferred list
  // In-place re-decode ping-pong partner (UpdateGuestTexture2DInPlace): the
  // GPU may still be copying from the OTHER upload buffer for the previous
  // frame's content change, so hot content alternates buffers instead of
  // paying two buffer creations per change.
  nrhi::Buffer* upload_b = nullptr;
  bool upload_flip = false;
  uint32_t fetch_words[6] = {};      // big-endian words as read for revalidation
  nrhi::TextureView* srv = nullptr;
  // For failed decodes: frame number for periodic retry (payload may stream
  // in after the fetch constant is already valid).
  uint64_t retry_after_frame = 0;
  // Payload-content revalidation: a texture first seen while its payload is
  // still STREAMING IN decodes to garbage (blocky macro-tile checkers /
  // blacked-out walls), and the fetch words never change when the content
  // finishes filling in at the same address, so the garbage decode was
  // cached forever. Sampled qwords across mip 0, rechecked periodically.
  uint32_t payload_addr = 0;   // 0xA-mirror guest address of mip 0
  uint32_t payload_size = 0;
  uint64_t payload_fp = 0;
  uint64_t recheck_frame = 0;
  // Consecutive failed decodes of this entry (payload still streaming in):
  // drives the escalating retry backoff at commit; the first failure
  // retries fast (the payload usually lands within a few frames; a fixed
  // +120 held freshly streamed textures white for ~half a second), repeat
  // failures back off toward the old cadence.
  uint8_t fail_count = 0;
  // Successful payload rechecks so far: fresh entries re-verify fast
  // (2/4/8 frames; a mid-stream garbage decode otherwise stays on screen
  // up to the full recheck interval) before settling at the 16-frame
  // steady-state cadence.
  uint8_t recheck_count = 0;
  // Tiled-aware payload probes: absolute 0xA-mirror addresses of up to 16
  // of this texture's OWN mip-0 blocks (a 4x4 spread over the block grid,
  // resolved through the tiled address swizzle). The old contiguous
  // [payload_addr, payload_size) fingerprint was WRONG for streamed world
  // textures: they are pitch-packed into shared mip pools and tiled across
  // padded macro rows, so the range interleaved NEIGHBOR textures' bytes;
  // pool churn kept the fingerprint flapping, the commit-time stability
  // verify then rejected every legitimate heal, and freshly promoted mips
  // stayed stale/low-res for seconds (the medium-distance texture pop-in;
  // one promote re-logged 8x while its heals were refused).
  // probe_count == 0 falls back to the range fingerprint (cubes).
  // 8x8 = 64 probes: the first 4x4 = 16 grid was too sparse; a mid-stream
  // decode whose 16 probed blocks happened to be final (other regions still
  // garbage) COMMITTED and never healed, visible as a black/garbage decal
  // until the next words change replaced it ("decal flash and replace").
  uint32_t probe_addr[64] = {};
  uint8_t probe_count = 0;
  // SRV recipe remnants (2D textures): srv_format is the revalidation
  // compare key and srv_mips gates the single-mip in-place update path.
  // The paired t4/t5 descriptor re-creation this recipe once fed is gone;
  // pairs bind their two views directly via SetTexturePair (descriptor
  // management is backend-internal now).
  nrhi::Format srv_format = nrhi::Format::kUnknown;
  uint32_t srv_mips = 0;
  bool valid = false;
  // Store LRU clock: last frame this entry was served/touched. Superseded
  // words states (old mip levels, pre-demote detail sets) age out once
  // nothing routes to them.
  uint64_t last_used_frame = 0;
  // Estimated GPU footprint of the committed texture (all mips/faces),
  // memoized by the store byte-accounting scan; 0 until first scanned or
  // while no texture is committed. Retained staging buffers are counted
  // live at scan time, not here.
  uint32_t gpu_bytes = 0;
  // A tiled mip's padded macro-row copy faulted and fell back to the
  // reported size; blocks beyond it uploaded as ZERO (the half-black
  // banner mip: tiled addressing puts the image's bottom rows past
  // min_size). The entry serves (better than white) but keeps re-decoding
  // until a complete copy lands; the commit prefers complete decodes.
  bool incomplete = false;
  // Decode-time SampleProbeNearBlack verdict: the payload was near-uniform
  // black when this decode was taken (a lightmap page mid-compose). The
  // lightmap-slot resolve serves the white fallback instead (tint.r == 0 ->
  // the unshadowed-bright window, 59810af semantics) until a heal lands
  // real content; other slots ignore it (black diffuse content is legal).
  bool near_black = false;
  // Same-content confirmations of a near-black verdict (commit dedups of
  // forced re-decodes). The forcing covers ONE race, a decode reading a
  // page mid-compose while the fingerprint sampled the composed result,
  // so a few confirmations prove the content is genuinely uniform and the
  // forcing stops (the plain fp poll still heals a later in-place compose).
  // Unbounded forcing re-decoded permanently-uniform textures every poll
  // forever, each a discarded worker round trip.
  uint8_t nb_redecodes = 0;
  // Frame of the last decode that landed CHANGED content for this key
  // (stamped by the words-key commit and the inline 2D decode). Classifies
  // video-plane content changes as mid-playback (recent change -> serve the
  // stale frame while the worker re-decodes, keeps 30 fps cadence) vs a
  // playback-START edge (quiet for seconds -> the entry holds the PREVIOUS
  // video's last frame; serving it flashed old content at every video
  // boundary; hold the quad black until fresh content commits).
  uint64_t last_change_frame = 0;
};

// THE texture identity key: FNV-1a
// over the six fetch-constant words, console identity semantics for the
// content store. Object resolves, draw-time word bindings, 2D/HUD art, and
// the decode workers all key the same g_r.tex_store entries with it.
inline uint64_t FetchWordsKey(const uint32_t words[6]) {
  uint64_t key = 1469598103934665603ull;
  for (int k = 0; k < 6; ++k) {
    // Clamp modes (dword_0 bits 10-18, clamp_x/y/z) are SAMPLER state, not
    // content identity; the same texture object binds with different clamp
    // bits per draw (observed: one texture decoded twice under 01004802 vs
    // 01024802, bit 17), and keying on them decodes/stores every variant
    // separately. The store holds textures + SRVs only; samplers are
    // static in the replay pipelines, so clamp-variant entries are exact
    // duplicates. Sign bits stay: they select the host SRV format.
    key ^= k == 0 ? (words[k] & ~0x0007FC00u) : words[k];
    key *= 1099511628211ull;
  }
  return key;
}

// Base-level pixel area from stored fetch words (word 2 packs width-1 /
// height-1 in 13-bit fields), the material-detail downgrade compare key.
inline uint64_t FetchWordsArea(const uint32_t words[6]) {
  return uint64_t((words[2] & 0x1FFFu) + 1u) *
         uint64_t(((words[2] >> 13) & 0x1FFFu) + 1u);
}

// Escalating retry backoff (native frames) for failed texture decodes: the
// payload is usually mid-stream and lands within a few frames; the first
// retry is fast (a fixed +120 held freshly streamed textures white for
// ~half a second); repeated failures back off toward the old cadence.
inline uint64_t RetryBackoff(uint8_t fails) {
  const uint32_t n = fails > 0 ? uint32_t(fails) - 1 : 0u;
  return std::min<uint64_t>(120, 8ull << std::min(n, 4u));
}
inline uint8_t BumpFail(uint8_t v) { return v < 250 ? uint8_t(v + 1) : v; }

// FNV-1a guest-payload fingerprint (SEH-guarded reads; streaming can
// decommit the range). Returns 0 only on unreadable payloads.
// Payloads up to 64 KB hash FULLY: the runtime-composed lightmap atlas
// pages (32 KB DXT1) receive REGIONAL in-place writes as the game composes
// late-streamed cells into a page we already decoded; the old 16-qword
// probe missed writes that fell between its samples, so the revalidation
// pass never re-decoded and the affected cells served the half-composed
// first decode forever (the 2x-bright canopy / dark awning reflective_trans
// glass: one texture, early-composed cells correct, late cells
// stale). Larger payloads (streamed assets whose fetch words change when
// content moves) keep a strided sample, densified 16 -> 64 qwords.
inline uint64_t SamplePayloadFingerprint(uint8_t* base, uint32_t addr, uint32_t size) {
  if (addr == 0 || size < 8) {
    return 0;
  }
  uint64_t h = 1469598103934665603ull;
  if (size <= 65536) {
    uint64_t buf[512];
    for (uint32_t off = 0; off < size; off += sizeof(buf)) {
      const uint32_t n = std::min<uint32_t>(sizeof(buf), (size - off) & ~7u);
      if (n == 0) {
        break;
      }
      if (!GuestTryCopy(buf, base + addr + off, n)) {
        return 0;
      }
      for (uint32_t k = 0; k < n / 8; ++k) {
        h = (h ^ buf[k]) * 1099511628211ull;
      }
    }
    return h;
  }
  for (uint32_t k = 0; k < 64; ++k) {
    const uint32_t off = uint32_t(uint64_t(size - 8) * k / 63u) & ~7u;
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + addr + off, sizeof(v))) {
      return 0;
    }
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// Heal-pipeline telemetry (see the 600-frame stats line): verify-fail =
// commit-time stability rejections, demote-hold = mip-0 demotes served
// from the cached full-chain decode without a doomed re-decode.
inline std::atomic<uint64_t> g_heal_verify_fail{0};
inline std::atomic<uint64_t> g_demote_hold{0};
// Sticky/skip serving (the anti-white-flash layer): sticky = draws served
// with the item's previous texture while a fresh object decodes; skipnew =
// first-sight draws suppressed entirely for the in-flight window.
inline std::atomic<uint64_t> g_tex_sticky_served{0};
inline std::atomic<uint64_t> g_skip_new{0};
// Per-mesh pipeline trace (skate3_native_render_scene_trace_mesh). Render
// thread only: the traced mesh address (parsed once per frame), the traced
// mesh's current store keys (so worker-commit events can be matched), and
// the last logged per-ctx state signature (summaries log on change).
inline uint32_t g_trace_mesh_addr = 0;
// 2D-resolver trace: fetch word 1 (base|flags) selected by the trace_2d
// cvar; 0 = off. Render thread only.
inline uint32_t g_trace_2d_w1 = 0;
inline std::unordered_set<uint64_t> g_trace_keys;
inline std::unordered_map<uint32_t, uint64_t> g_trace_sig;
// Words-keyed (event-ad / streamed-artwork) serving: stale = site served
// its previous art while a new-words decode is in flight; none = nothing
// decoded for the site yet (caller shows the baked placeholder).
inline std::atomic<uint64_t> g_ad_stale_served{0};
inline std::atomic<uint64_t> g_ad_placeholder{0};

// Fill GuestTexture::probe_addr with up to 64 of the texture's own mip-0
// blocks: an 8x8 spread over the block grid, each resolved through the same
// tiled/linear addressing the decode uses, clamped to the guarded copy's
// readable range. Probes crossing into the padded macro-row tail are skipped
// rather than clamped; the tail belongs to pool neighbors.
inline void BuildPayloadProbes(const rex::graphics::TextureInfo& info, uint32_t mip0_addr,
                        uint32_t ox, uint32_t oy, uint32_t pitch_blocks,
                        uint32_t copy_size, GuestTexture& out) {
  out.probe_count = 0;
  const rex::graphics::FormatInfo* fi = info.format_info();
  const uint32_t bpb = fi->bytes_per_block();
  if (mip0_addr == 0 || bpb == 0 || (bpb & (bpb - 1)) != 0) {
    return;
  }
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(bpb));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t cols = (width + fi->block_width - 1) / fi->block_width;
  const uint32_t rows = (height + fi->block_height - 1) / fi->block_height;
  // DXT2/3/4/5 blocks LEAD with their 8-byte alpha half, and on fully
  // opaque art that half is the same constant in every block; the leading
  // qword hashed identically across entirely DIFFERENT images (every
  // pause-menu location photo fingerprinted alike), so in-place content
  // swaps were invisible to the liveness probes. Probe the color half of
  // those blocks instead. (DXT1's 8-byte block IS the color half; DXN's
  // leading half is a real content channel; both keep offset 0.)
  using TF = rex::graphics::xenos::TextureFormat;
  const uint32_t probe_off =
      (info.format == TF::k_DXT2_3 || info.format == TF::k_DXT4_5 ||
       info.format == TF::k_DXT2_3_AS_16_16_16_16 ||
       info.format == TF::k_DXT4_5_AS_16_16_16_16)
          ? 8u
          : 0u;
  for (uint32_t gy = 0; gy < 8; ++gy) {
    for (uint32_t gx = 0; gx < 8; ++gx) {
      const uint32_t bx = cols > 1 ? gx * (cols - 1) / 7 : 0;
      const uint32_t by = rows > 1 ? gy * (rows - 1) / 7 : 0;
      uint32_t off;
      if (info.is_tiled) {
        off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(bx + ox), int32_t(by + oy), pitch_blocks, bpb_log2));
      } else {
        off = ((by + oy) * pitch_blocks + bx + ox) * bpb;
      }
      if (off + bpb > copy_size) {
        continue;
      }
      out.probe_addr[out.probe_count++] =
          (0xA0000000u | mip0_addr) + off + probe_off;
    }
  }
}

// Probe-based payload fingerprint: hashes one block-leading qword (or the
// whole block for narrow formats) at each probe address. Returns 0 only when
// a probe is unreadable: same semantics as the range fingerprint. Falls
// back to the range hash when no probes were built (cube maps).
inline uint64_t SampleProbeFingerprint(uint8_t* base, const GuestTexture& t) {
  if (t.probe_count == 0) {
    return SamplePayloadFingerprint(base, t.payload_addr, t.payload_size);
  }
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < t.probe_count; ++i) {
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + t.probe_addr[i], sizeof(v))) {
      return 0;
    }
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// Near-UNIFORM payload detector, sampled over the same probe grid at decode
// time. A COMPOSED lightmap page or weathering overlay is never one flat
// value across its whole mip-0 (a daylight bake / grime map atlases many
// surfaces), but a page decoded mid-compose is uniform fill: zeroed OR a
// flat grey (the reported "black/grey squares": a zero-only detector
// is structurally blind to grey). A real-but-garbage lightmap binds with
// tint.r > 0 so the CSM min-clamp renders BLACK (the door 59810af's
// white-fallback shader gate cannot see), and since 59810af a garbage macro
// multiplies OVER the decal paint. Consumers act on this only for the
// WHITE-NEUTRAL slots (lightmap/macro); uniform diffuse/spec content is
// legal and unaffected.
inline bool SampleProbeNearBlack(uint8_t* base, const GuestTexture& t) {
  if (t.probe_count < 16) {
    return false;  // range-fallback entries (cubes): not enough coverage
  }
  uint64_t a = 0, b = 0;
  bool have_a = false, have_b = false;
  for (uint32_t i = 0; i < t.probe_count; ++i) {
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + t.probe_addr[i], sizeof(v))) {
      return false;
    }
    if (!have_a) {
      a = v;
      have_a = true;
    } else if (v != a && !have_b) {
      b = v;
      have_b = true;
    } else if (v != a && v != b) {
      return false;  // 3+ distinct block values = real composed content
    }
  }
  return true;  // <= 2 distinct block values across the whole probe grid
}

// Host format mapping for the formats Skate 3 uses (mirrors the SDK's
// D3D12 texture cache table). host_swizzle remaps guest data components
// before the fetch-constant swizzle composes on top.
struct HostTextureFormat {
  nrhi::Format resource_format = nrhi::Format::kUnknown;
  nrhi::Format srv_format = nrhi::Format::kUnknown;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
};

inline bool GetHostTextureFormat(xenos::TextureFormat format, HostTextureFormat& out) {
  switch (rex::graphics::GetBaseFormat(format)) {
    case xenos::TextureFormat::k_DXT1:
      out = {nrhi::Format::kBC1_UNORM, nrhi::Format::kBC1_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT2_3:
      out = {nrhi::Format::kBC2_UNORM, nrhi::Format::kBC2_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT4_5:
      out = {nrhi::Format::kBC3_UNORM, nrhi::Format::kBC3_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT5A:
      out = {nrhi::Format::kBC4_UNORM, nrhi::Format::kBC4_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case xenos::TextureFormat::k_DXN:
      out = {nrhi::Format::kBC5_UNORM, nrhi::Format::kBC5_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
      out = {nrhi::Format::kR8G8B8A8_UNORM, nrhi::Format::kR8G8B8A8_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_8:
      out = {nrhi::Format::kR8_UNORM, nrhi::Format::kR8_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case xenos::TextureFormat::k_8_8:
      out = {nrhi::Format::kR8G8_UNORM, nrhi::Format::kR8G8_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case xenos::TextureFormat::k_5_6_5:
      // Red/blue swapped CPU-side while uploading.
      out = {nrhi::Format::kB5G6R5_UNORM, nrhi::Format::kB5G6R5_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB};
      return true;
    default:
      return false;
  }
}

// Compose the fetch-constant swizzle over the host-format component remap
// into a per-channel view swizzle (Xenos 0..5 = X,Y,Z,W,0,1, the same
// order as nrhi::Swizzle).
inline void ComposeSrvSwizzle(uint32_t fetch_swizzle, uint32_t host_swizzle,
                       nrhi::Swizzle out[4]) {
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t guest = (fetch_swizzle >> (3 * c)) & 7u;
    const uint32_t v = guest >= 4 ? guest : ((host_swizzle >> (3 * guest)) & 7u);
    out[c] = nrhi::Swizzle(v);
  }
}

inline void SwapGuestEndian(uint8_t* data, uint32_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i + 2 <= size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i + 4 <= size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i + 4 <= size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

struct RendererState {
  nrhi::Device* device = nullptr;
  nrhi::BindingLayout* layout = nullptr;  // main scene binding layout (was the root signature)
  nrhi::Pipeline* pso = nullptr;
  nrhi::Pipeline* pso_cullback = nullptr;  // two_sided_sheet meshes (see MeshBuffers)
  nrhi::Pipeline* pso_nodepth = nullptr;
  // environment.transparent sub-pass: straight alpha blend, depth test on,
  // z-write OFF; items drawn back-to-front after all opaque items.
  nrhi::Pipeline* pso_transparent = nullptr;
  // Entity-fade variant of the transparent PSO: same straight alpha blend
  // but z-write ON. A fading character/vehicle is a solid object at partial
  // opacity; z-write-off blending composites every overlapping piece (skin
  // under clothes, far-side doors/wheels through the body shell) into an
  // x-ray. With depth writes the nearest surface wins and each pixel blends
  // once, matching the game's main-pass fade.
  nrhi::Pipeline* pso_fade = nullptr;
  // Hair sub-passes: transparent blend state with cull BACK / cull FRONT
  // (the game's cac_hair/defaulthair two-pass draw order).
  nrhi::Pipeline* pso_hair_a = nullptr;
  nrhi::Pipeline* pso_hair_b = nullptr;
  nrhi::Format rtv_format = nrhi::Format::kUnknown;
  nrhi::Texture* depth = nullptr;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  // Cached guest-output texture identity (context.guest_output) for change
  // detection: the presenter recreates the output image on resize.
  nrhi::Texture* rtv_resource = nullptr;
  // MSAA: the scene draws into msaa_color (+ MSAA depth) and a fullscreen
  // pass averages the samples into the guest output texture (the RHI command
  // stream has no resolve copy).
  uint32_t msaa = 1;
  nrhi::Texture* msaa_color = nullptr;
  nrhi::Pipeline* resolve_pso = nullptr;
  nrhi::TextureView* msaa_srv_slot = nullptr;
  bool msaa_srv_allocated = false;
  // Popup background blur (see kBlurShaderSource): two intermediates at the
  // game's fixed 1152x640 internal resolution + a view of the guest output
  // (re-created when the output texture changes). Steady state for
  // blur_tex is RENDER_TARGET.
  static constexpr uint32_t kBlurWidth = 1152;
  static constexpr uint32_t kBlurHeight = 640;
  nrhi::Texture* blur_tex[2] = {nullptr, nullptr};
  nrhi::TextureView* blur_srv[2] = {nullptr, nullptr};
  nrhi::TextureView* output_srv_slot = nullptr;
  bool output_srv_allocated = false;
  // Sampled views of the guest-output mailbox images. The Vulkan presenter
  // rotates among a small set of guest-output images (a 3-image mailbox) on
  // every refresh, so context.guest_output toggles between those images each
  // frame: before the cache, output_srv_slot recreated its VkImageView on
  // EVERY rotation (a per-frame fullscreen view create+destroy -> the
  // recurring DrainRetired micro-stutter). Cache one view per output image
  // and reuse it on rotation; a view is created only when a NEW output
  // texture (genuine resize) appears, and stale entries are forgotten (not
  // deferred-destroyed) because the RHI owns the guest-output wrapper
  // lifetime.
  struct OutputViewEntry {
    nrhi::Texture* tex = nullptr;
    nrhi::TextureView* view = nullptr;
    uint32_t seq = 0;  // last-use stamp (monotonic tick)
  };
  std::vector<OutputViewEntry> output_views;
  uint32_t output_view_seq = 0;
  nrhi::Pipeline* pso_blur = nullptr;
  nrhi::Pipeline* pso_blur_blit = nullptr;
  nrhi::Pipeline* pso_blur_down = nullptr;
  // Settings-menu gaussian backdrop (ps_menu_gauss): clean separable
  // gaussian on half-output-res ping-pong intermediates, applied at the END
  // of the compose so it frosts everything drawn (3D + HUD/2D + FMV).
  // Deliberately independent of the popup-blur port above (that chain
  // reproduces the console's 1152x640 lattice on purpose). Steady state
  // RENDER_TARGET; recreated on output size change.
  nrhi::Pipeline* pso_menu_gauss = nullptr;
  nrhi::Texture* menu_blur_tex[2] = {nullptr, nullptr};
  nrhi::TextureView* menu_blur_srv[2] = {nullptr, nullptr};
  uint32_t menu_blur_w = 0;
  uint32_t menu_blur_h = 0;
  // Native photo grab (photo_grab_native): the blur downsample pass (whose
  // 1152x640 blur space happens to BE the game's screenshot-target raster)
  // renders the finished frame into blur_tex[0], which is then copied into
  // one of these persistently-mapped READBACK buffers; the next armed frame
  // CPU-tiles the completed one big-endian into the guest grab target. Two
  // buffers so no frame ever waits on the GPU (a full kFull-style drain per
  // frame was the old path's stall). Row pitch 1152*4 = 4608 is already
  // 256-aligned.
  static constexpr uint32_t kGrabRowPitch = kBlurWidth * 4;
  nrhi::Buffer* grab_readback[2] = {nullptr, nullptr};
  uint8_t* grab_readback_ptr[2] = {nullptr, nullptr};
  uint64_t grab_submission[2] = {0, 0};
  bool grab_pending[2] = {false, false};
  uint32_t grab_write_index = 0;
  bool grab_failed = false;
  // Window stats for the close log: frames written to guest + CPU tile time.
  uint32_t grab_writes = 0;
  uint64_t grab_cpu_us = 0;
  // Native FMV: overlay-2D pipeline variant whose PS combines the movie
  // player's three CPU-filled YUV plane textures (ps_yuv2d; substituted for
  // the captured movie quad in the 2D replay, see OnMovieFrame).
  nrhi::Pipeline* pso_yuv2d = nullptr;
  // Selection outline (see DrawItem::selected): selected items re-render
  // into a single-sample R8 mask at OUTPUT resolution (a low-res mask
  // stairstepped the contour centerline at 4K) and a fullscreen edge-detect
  // pass with fixed UV-fraction tap pitch adds the blue outline onto the
  // resolved output (postfx_edgedetectstencil equivalent). Mask steady
  // state is RENDER_TARGET.
  nrhi::Texture* outline_mask = nullptr;
  uint32_t outline_mask_width = 0;
  uint32_t outline_mask_height = 0;
  nrhi::TextureView* outline_mask_srv = nullptr;
  bool outline_mask_srv_allocated = false;
  nrhi::Pipeline* pso_outline_mask = nullptr;
  nrhi::Pipeline* pso_outline_edge = nullptr;
  // Photo-editor postfx chain (photo_fx.hlsl: exact ucode ports, see
  // FrameScene::PhotoFx). Own binding layout: CBV b0 (the pass's 256
  // captured/baked constant rows) + eight single-texture tables t0..t7 +
  // three static samplers. Intermediates at the game's exact half/quarter
  // sizes (the DOF tap offsets are baked for them); the full-res stages run
  // at output resolution. pfx_quarter persists across frames (accumulation
  // input). All pfx color targets idle in RENDER_TARGET state.
  static constexpr uint32_t kPfxHalfW = 576, kPfxHalfH = 320;
  static constexpr uint32_t kPfxQuarterW = 288, kPfxQuarterH = 160;
  nrhi::BindingLayout* pfx_layout = nullptr;
  // PSO order: depthpack, visualfx, dof_down, dof_mb, dof, uber, fisheye, blit.
  nrhi::Pipeline* pfx_pso[9] = {};
  nrhi::Texture* pfx_full[2] = {};   // output-res RGBA8 (visualfx out, uber out)
  nrhi::Texture* pfx_half[2] = {};   // 576x320 RGBA8
  nrhi::Texture* pfx_quarter = nullptr;  // 288x160 RGBA8 accumulation
  nrhi::Texture* pfx_depth = nullptr;    // output-res packed-depth RGBA8
  nrhi::Texture* pfx_lut = nullptr;      // 32^3 identity grade LUT (RGBA8)
  nrhi::Buffer* pfx_lut_upload = nullptr;
  bool pfx_lut_uploaded = false;
  nrhi::Buffer* pfx_cb = nullptr;  // upload heap, persistently mapped
  uint8_t* pfx_cb_ptr = nullptr;
  // Fixed SRV views: 0/1 = pfx_full, 2/3 = pfx_half, 4 = quarter, 5 =
  // packed depth, 6 = LUT, 7 = the native MSAA/1x depth resource.
  nrhi::TextureView* pfx_srv[8] = {};
  bool pfx_srv_allocated = false;
  uint32_t pfx_width = 0, pfx_height = 0;
  bool pfx_ready = false;
  bool pfx_failed = false;
  // Screen-space ambient occlusion (ssao.hlsl: GTAO over the resolved
  // scene; see ApplySsaoPass). Own binding layout: root constants b0 + two
  // single-texture tables t0/t1 + point/linear clamp samplers. Full-res
  // intermediates: linearized view-Z (R32F) + two AO ping-pong planes
  // (R8), idle in RENDER_TARGET state. PSOs rebuild when the MSAA level
  // (depth SRV dimension / linearize variant) or the output format
  // (composite target) changes.
  nrhi::BindingLayout* ao_layout = nullptr;
  nrhi::Pipeline* pso_ao_linearize = nullptr;
  nrhi::Pipeline* pso_ao_gtao = nullptr;
  nrhi::Pipeline* pso_ao_blur = nullptr;
  nrhi::Pipeline* pso_ao_luma = nullptr;
  nrhi::Pipeline* pso_ao_composite = nullptr;
  nrhi::Pipeline* pso_ao_debug = nullptr;
  nrhi::Texture* ao_lin_depth = nullptr;
  nrhi::Texture* ao_tex[2] = {};
  // Scene luminance at the AO raster: the sun-lit protection mask input
  // (bright pixels resist AO and skip the march entirely).
  nrhi::Texture* ao_luma = nullptr;
  nrhi::TextureView* ao_lin_srv = nullptr;
  nrhi::TextureView* ao_srv[2] = {};
  nrhi::TextureView* ao_luma_srv = nullptr;
  // View of the scene depth buffer, re-pointed when depth is rebuilt.
  nrhi::TextureView* ao_depth_srv = nullptr;
  nrhi::Texture* ao_depth_srv_of = nullptr;
  uint32_t ao_width = 0, ao_height = 0;          // AO raster (half or full res)
  uint32_t ao_lin_width = 0, ao_lin_height = 0;  // linear depth (always full)
  uint32_t ao_msaa = 0;
  nrhi::Format ao_rtv_format = nrhi::Format::kUnknown;
  bool ao_hdr = false;  // luma/gtao/composite built for the HDR target
  bool ao_failed = false;
  // Fused-AO handoff (HDR path): the AO pass leaves the finished multiplier
  // plane (ao_tex[0]) in PIXEL_SHADER_RESOURCE state for ps_tonemap to
  // consume at t2 (replacing the classic full-res composite draw);
  // ApplyHdrPost restores it to RENDER_TARGET and clears the flag.
  bool ao_plane_in_psr = false;
  // Occlusion-attribution grid (perf-items profiling only): a small
  // conservative tile-MAX reduce of ao_lin_depth (ssao.hlsl ps_depth_max),
  // read back through a two-deep never-waited ring (the photo-grab pattern)
  // and consumed on the render thread 1-2 frames later to classify scene
  // items as fully occluded by already-rendered geometry. Each slot stores
  // the view_proj its depth was rendered with; the CPU test projects with
  // THAT matrix so grid and bounds stay consistent under camera motion.
  static constexpr uint32_t kOcclGridW = 160;
  static constexpr uint32_t kOcclGridH = 90;
  static constexpr uint32_t kOcclRowPitch = 768;  // 160*4 aligned to 256
  nrhi::Pipeline* pso_occl_reduce = nullptr;
  nrhi::Texture* occl_tex = nullptr;  // R32F grid, idles in RENDER_TARGET
  nrhi::Buffer* occl_readback[2] = {};
  uint8_t* occl_readback_ptr[2] = {};
  uint64_t occl_submission[2] = {};
  float occl_vp[2][16] = {};
  float occl_cam[2][3] = {};  // camera position the grid's frame rendered from
  bool occl_pending[2] = {};
  int occl_write_index = 0;
  bool occl_failed = false;
  // Newest completed grid, CPU-side (render thread only).
  std::vector<float> occl_grid;
  float occl_grid_vp[16] = {};
  float occl_grid_cam[3] = {};
  uint64_t occl_grid_frame = 0;  // frame_number the grid was captured on
  bool occl_grid_valid = false;
  // HDR pipeline (hdr.hlsl): the scene renders pre-tonemap linear into a
  // float intermediate (the MSAA color target and/or hdr_resolved), a bloom
  // pyramid extracts real HDR energy, and ps_tonemap applies the game's
  // shared tone chain once into the gamma guest output. Binding layout =
  // the SSAO shape (root constants b0 + two single-texture tables +
  // point/linear clamp samplers). All targets idle in RENDER_TARGET state.
  bool hdr_active = false;  // latched per pipeline build (PSO formats/variants)
  bool hdr_failed = false;  // float targets unavailable -> classic path
  // Latched scene float format (RGBA16F or the packed R11G11B10 option);
  // every HDR-path PSO/target creation reads this, never the cvar.
  nrhi::Format hdr_scene_format = nrhi::Format::kR16G16B16A16_FLOAT;
  nrhi::BindingLayout* hdr_layout = nullptr;
  nrhi::Pipeline* pso_bloom_first = nullptr;  // mip-0 extract (threshold+Karis)
  nrhi::Pipeline* pso_bloom_down = nullptr;
  nrhi::Pipeline* pso_bloom_up = nullptr;     // additive ONE/ONE tent upsample
  nrhi::Pipeline* pso_tonemap = nullptr;
  nrhi::Format hdr_pso_out_format = nrhi::Format::kUnknown;
  // Showcase shader swap: while a build-up run is live the pipeline family
  // rebuilds with the SHOWCASE=1 shader variants (the split/mask gates
  // compiled in); every other session runs shaders with the showcase code
  // compiled out entirely, so normal rendering carries none of it. _want is
  // set by TickShowcase (a run needs the variants BEFORE its first frame),
  // applied by the EnsurePipeline rebuild block; the per-pass built flags
  // let the lazily-built tonemap/SSR pipelines follow the swap.
  bool showcase_shaders_want = false;
  bool showcase_shaders = false;
  bool hdr_showcase = false;
  bool ssr_showcase = false;
  // 1x float scene plane: the MSAA resolve destination (or the scene target
  // itself when MSAA is off); the AO composite, bloom extract and tonemap
  // all consume/write it.
  nrhi::Texture* hdr_resolved = nullptr;
  nrhi::TextureView* hdr_srv = nullptr;
  static constexpr uint32_t kBloomMaxLevels = 6;
  nrhi::Texture* bloom_tex[kBloomMaxLevels] = {};
  nrhi::TextureView* bloom_srv[kBloomMaxLevels] = {};
  uint32_t bloom_w[kBloomMaxLevels] = {};
  uint32_t bloom_h[kBloomMaxLevels] = {};
  uint32_t bloom_levels = 0;
  uint32_t bloom_base_w = 0, bloom_base_h = 0;  // output size the chain fits
  bool targets_hdr = false;  // output-sized targets built for the HDR path
  // Scene float format the output-sized targets were built with (kUnknown
  // when classic); packed-format toggles rebuild them.
  nrhi::Format targets_scene_fmt = nrhi::Format::kUnknown;
  // MSAA level the output-sized targets were built with; hot MSAA changes
  // rebuild the depth buffer and the multisample color target.
  uint32_t targets_msaa = 0;
  // Screen-space reflections (ssr.hlsl + scene.hlsl ps_refl_gbuf): a
  // half-res reflection G-buffer re-rendered from the frame's reflective
  // items (env fams 5/6/13 + water), a half-res screen-space march over the
  // full-res linear depth sampling the pre-tonemap HDR plane, and a
  // src-alpha composite back onto it between the AO pass and the HDR post
  // (so reflections tonemap/bloom like directly-visible scenery). HDR path
  // only. Post binding layout = the SSAO shape with 32 root floats + three
  // single-texture tables; the G-buffer pass runs under the MAIN layout
  // (scene VS + per-item constants). All intermediates idle in
  // RENDER_TARGET state.
  nrhi::BindingLayout* ssr_layout = nullptr;
  nrhi::Pipeline* pso_ssr_gbuf = nullptr;       // scene-VS geometry pass
  nrhi::Pipeline* pso_ssr_linearize = nullptr;  // fallback when SSAO idle
  nrhi::Pipeline* pso_ssr_march = nullptr;
  nrhi::Pipeline* pso_ssr_composite = nullptr;
  nrhi::Texture* ssr_gbuf = nullptr;  // half res RGBA16F reflection G-buffer
  // Half-res depth for the G-buffer pass (test+write LESS): reflective
  // items overlap themselves in screen space (a curved glass tower's far
  // side shares texels with its near side inside ONE mesh), so without a
  // depth test draw/triangle order decides which surface owns a texel and
  // whole visible panes fail the march's scene-depth match. Idles in
  // DEPTH_WRITE state.
  nrhi::Texture* ssr_gbuf_depth = nullptr;
  nrhi::Texture* ssr_tex = nullptr;   // half res RGBA16F march output
  nrhi::Texture* ssr_lin_depth = nullptr;  // full res R32F (own linearize)
  nrhi::TextureView* ssr_gbuf_srv = nullptr;
  nrhi::TextureView* ssr_srv = nullptr;
  nrhi::TextureView* ssr_lin_srv = nullptr;
  // Scene-depth SRV for the own linearize, re-pointed on depth rebuilds.
  nrhi::TextureView* ssr_depth_srv = nullptr;
  nrhi::Texture* ssr_depth_srv_of = nullptr;
  uint32_t ssr_width = 0, ssr_height = 0;  // half-res raster
  uint32_t ssr_lin_width = 0, ssr_lin_height = 0;
  uint32_t ssr_msaa = 0;  // linearize-variant PSO latch
  nrhi::Format ssr_scene_fmt = nrhi::Format::kUnknown;  // composite target
  bool ssr_failed = false;
  // Per-frame handoff: the G-buffer pass drew and left ssr_gbuf in
  // PIXEL_SHADER_RESOURCE for ApplySsrPass (which consumes + restores it).
  bool ssr_gbuf_ready = false;
  // Volumetric lighting (hdr.hlsl ps_vol_*): shadow-marched sun shafts,
  // a half-res world-space march testing per-step sun visibility against
  // the CSM atlas + the static world-shadow map (real shadowed air; no
  // screen-space silhouette dependence), plus an analytic directional
  // haze, both fused into ps_tonemap pre-tonemap (after the AO multiply;
  // in-air light is not surface-occluded). HDR path only; shares the HDR
  // binding layout (which carries the shadow constant slice at b1). The
  // shaft plane idles in RENDER_TARGET state.
  nrhi::Pipeline* pso_vol_linearize = nullptr;  // fallback when SSAO idle
  nrhi::Pipeline* pso_vol_shafts = nullptr;
  // 3x3 tent over the marched plane (the ps_bloom_up shader without the
  // additive blend): integrates the march's per-pixel jitter dither and
  // softens shadow-volume edges.
  nrhi::Pipeline* pso_vol_blur = nullptr;
  nrhi::Texture* vol_tex = nullptr;    // half res RGBA16F march output
  nrhi::Texture* vol_tex_b = nullptr;  // blurred plane (the tonemap input)
  nrhi::TextureView* vol_srv = nullptr;
  nrhi::TextureView* vol_srv_b = nullptr;
  uint32_t vol_width = 0, vol_height = 0;
  nrhi::Texture* vol_lin_depth = nullptr;  // full res R32F (own linearize)
  nrhi::TextureView* vol_lin_srv = nullptr;
  uint32_t vol_lin_width = 0, vol_lin_height = 0;
  // Scene-depth SRV for the own linearize, re-pointed on depth rebuilds.
  nrhi::TextureView* vol_depth_srv = nullptr;
  nrhi::Texture* vol_depth_srv_of = nullptr;
  uint32_t vol_msaa = 0;  // linearize-variant PSO latch
  bool vol_failed = false;
  // Per-frame handoff to ApplyHdrPost: the finished shaft plane and the
  // linear-depth plane used this frame (the SSAO plane or the own
  // linearize) are left in PIXEL_SHADER_RESOURCE for ps_tonemap (t3/t4);
  // ApplyHdrPost restores both and clears the flags.
  bool vol_plane_in_psr = false;
  nrhi::Texture* vol_lin_plane = nullptr;
  nrhi::TextureView* vol_lin_plane_srv = nullptr;
  bool vol_lin_in_psr = false;
  // ps_tonemap's volumetric constant rows (b0 rows vp/vs0/vs1/vs2 tail),
  // staged by ApplyVolumetricPass; zeros disable every term.
  bool vol_tonemap_valid = false;
  float vol_rows[16] = {};
  // Graphics build-up showcase split state for this frame, computed by the
  // sequencer at b1 staging and mirrored here for the SSR composite:
  // {stage left of the split, stage right, split position in output px}.
  // All zeros = showcase off (stage 0 = the full render in every consumer).
  float showcase_rows[3] = {};
  std::unordered_map<uint32_t, MeshBuffers> meshes;
  // (The old D3D12 bookkeeping, the retired-resource vector, the CPU SRV
  // staging heap and its slot allocator/recycling lists, is gone: resource
  // destruction is deferred inside the RHI (Device::DestroyDeferred) and
  // texture bindings are backend-managed view objects.)
  GuestTexture white;
  // Water environment CUBE maps (t6): separate cache; same guest object
  // addresses decode differently (6 faces, TextureCube SRV).
  std::unordered_map<uint32_t, GuestTexture> cube_textures;
  GuestTexture white_cube;
  // Bone palette ring: persistent-mapped upload buffer, one region per
  // in-flight frame.
  static constexpr uint32_t kBoneRegionSize = 1u << 20;
  // 8-deep (was 4): the Vulkan CP was observed letting the GPU lag more
  // than 3 frames under load - 4-deep regions then recycle while still
  // referenced (hair shimmer / collapsed garments). If the ring race is
  // confirmed, the CP-side throttle is the
  // proper fix and this is defense in depth.
  static constexpr uint32_t kBoneRegions = 8;
  nrhi::Buffer* bone_ring = nullptr;
  uint8_t* bone_ring_cpu = nullptr;
  uint32_t bone_ring_offset = 0;
  // ROPA shape-generation ring (per garment mesh): the last decoded vertex
  // arrays keyed by dyn_seq, so the draw can combine the generations under
  // the interp pass's kernel weights (the body's own 8-tap boxcar /
  // pair-lerp: the stepped OR filter-mismatched shape against the
  // interpolated body was the tee jelly). Plus a persistent-mapped upload
  // ring the per-frame blended verts are written into (regioned like the
  // bone ring).
  struct RopaGen {
    uint64_t seq = 0;
    // Host time at commit: the blend refuses to mix generations recorded
    // across a sim-sleep gap (a garment whose cloth sim slept keeps its
    // last drapes in the ring; blending one of those, recorded when the
    // character stood elsewhere / faced differently, against the fresh
    // drape renders the garment rotated/offset from the body).
    double t = 0.0;
    std::vector<float> verts;  // num_verts x 14 floats (scene VS layout)
  };
  std::unordered_map<uint32_t, std::deque<RopaGen>> ropa_shapes;
  static constexpr uint32_t kRopaRegionSize = 1u << 20;
  nrhi::Buffer* ropa_ring = nullptr;
  uint8_t* ropa_ring_cpu = nullptr;
  uint32_t ropa_ring_offset = 0;
  // 2D overlay (HUD/APT replay): alpha-blended depth-less pipeline drawing
  // the captured inline vertices from a per-frame upload ring.
  nrhi::Pipeline* pso_2d = nullptr;
  // In-world neon splines, drawn inside the MSAA scene pass (depth test on,
  // no z-write): darken = straight alpha, default = additive glow.
  nrhi::Pipeline* pso_spline_darken = nullptr;
  nrhi::Pipeline* pso_spline_default = nullptr;
  // Dynamic CSM shadows: casters render
  // into a 3-tile (depth, coverage) atlas with MIN blend (depth clear 1,
  // "uncoverage" clear 1 -> covered texels write 0), then the game's exact
  // Gaussian-coverage + depth-dilation blur runs per tile (5-tap cascade 0,
  // 3-tap cascade 1, format-convert-only cascade 2) into the atlas the
  // scene pass samples at t5.
  nrhi::Pipeline* pso_shadow_caster = nullptr;
  // Alpha-tested caster variant (ps_shadow_caster_clip): binds the item's
  // diffuse at t0 and clips at the world families' ALPHAREF so foliage
  // cards and alphatest fences cast their cutout silhouette.
  nrhi::Pipeline* pso_shadow_caster_clip = nullptr;
  nrhi::Pipeline* pso_shadow_blur = nullptr;
  nrhi::Texture* shadow_raw = nullptr;    // caster pass target
  nrhi::Texture* shadow_mid = nullptr;    // hblur output
  nrhi::Texture* shadow_final = nullptr;  // vblur output, sampled
  uint32_t shadow_tile = 0;               // per-cascade tile size (atlas = 3*tile x tile)
  nrhi::TextureView* shadow_srv_raw = nullptr;
  nrhi::TextureView* shadow_srv_mid = nullptr;
  nrhi::TextureView* shadow_srv_final = nullptr;
  // After a shadow pass all three textures sit in PIXEL_SHADER_RESOURCE;
  // the next pass transitions them back to RENDER_TARGET first.
  bool shadow_in_srv_state = false;
  // Shadow-atlas readback, taken on kShadowDumpFrames CONSECUTIVE frames
  // once per F11 recording session: the raw and blurred planes (R16G16)
  // land in the snapshot dir for offline analysis; the guest's own atlas
  // resolve never reaches CPU memory, so this is the only view of what
  // receivers actually sample. Consecutive frames make temporal-stability
  // diffs possible (silhouette buzz vs animation).
  static constexpr uint32_t kShadowDumpFrames = 4;
  nrhi::Buffer* shadow_dump_buf[kShadowDumpFrames] = {};
  uint8_t* shadow_dump_ptr[kShadowDumpFrames] = {};
  uint64_t shadow_dump_submission[kShadowDumpFrames] = {};
  uint32_t shadow_dump_enqueued = 0;
  uint32_t shadow_dump_written = 0;
  bool shadow_dump_done = false;
  // Per-frame shadow constant buffer ring (CBV b1). 768-byte slices: the
  // first 256 bytes are the original 16-row block, rows 16-18 carry the
  // dynamicobject world-shadow transform (dyn_ws*), rows 19-22 the
  // flowingwateralpha m_params (wat_p*), rows 23-33 the ocean PCA/material
  // rows and the oceanreflection fade row (oc_* / orf), rows 34-35 the
  // PCSS soft-shadow parameters (sh_pcss / sh_pcss2).
  static constexpr uint32_t kShadowCbRegions = 8;
  static constexpr uint32_t kShadowCbSlice = 768;
  nrhi::Buffer* shadow_cb = nullptr;
  uint8_t* shadow_cb_cpu = nullptr;
  // Native static sun-shadow map: a single camera-centered ortho depth map
  // of the STATIC world along the material sun (RenderStaticSunMap),
  // sampled at t10 by every lit branch (SampleStaticSun). RG16 to reuse
  // the caster pipeline/clear conventions; G unused. nsm_rows are this
  // frame's world->map transform rows for the b1 fill.
  nrhi::Texture* static_sun = nullptr;
  nrhi::TextureView* static_sun_srv = nullptr;
  bool static_sun_in_srv = false;
  bool static_sun_valid = false;
  uint32_t static_sun_size = 0;
  // The per-tile size this map was requested at (post backend-limit clamp,
  // pre allocation-failure fallback). The hot-size-change retire keys on
  // this, not static_sun_size: a map that allocated smaller than requested
  // must not be retired and re-tried every frame.
  uint32_t static_sun_requested = 0;
  float nsm_rows[12] = {};
  float nsm_depth_range = 1.0f;  // meters per depth-map unit
  float nsm_radius = 1.0f;       // far-tile ortho half-extent in meters
  // Cross-frame map cache: the map only re-renders when the camera drifts
  // from the built center, the sun moves, the caster cache changed, or on
  // a periodic safety rebuild (newly decoded meshes); statics and the sun
  // are near-constant, so per-frame redraws of the full two-tile atlas
  // were almost pure waste (and re-snapping the origin every frame made
  // the foliage dapple shimmer).
  float nsm_center[3] = {};
  float nsm_sun[3] = {};
  float nsm_built_radius = 0.0f;
  bool nsm_dirty = true;
  uint64_t nsm_rebuild_frame = 0;
  uint64_t nsm_last_build_frame = 0;
  // Persistent static-caster cache for the sun map. scene.items is the
  // game's VIEW-CULLED draw list; rendering the map from it directly made
  // shadows pop with the camera (look away and the caster leaves the list;
  // geometry behind the camera never cast at all). Items upsert into this
  // cache when visible and keep casting from it afterwards, validated
  // against the mesh store's fingerprint at draw time. Eviction: distance
  // from the camera (region streamed away) plus a long staleness timeout
  // (bounds ghosts from content the game actually removed, e.g. deleted
  // park-editor objects).
  struct StaticCaster {
    uint32_t mesh = 0;
    uint64_t fingerprint = 0;
    float world[16] = {};
    float bbox_min[3] = {};
    float bbox_max[3] = {};
    uint32_t diffuse_tex = 0;
    bool clip = false;
    std::vector<DrawEntry> draws;
    uint64_t last_seen_frame = 0;
  };
  std::unordered_map<uint64_t, StaticCaster> static_casters;
  uint64_t static_casters_sweep_frame = 0;
  // dynamicobject static world-shadow map (512x512, same convention as the
  // game's own: x = light-space depth, sge(x >= ray) = lit): re-rendered
  // natively from the frame's STATIC world items with the captured c5/c6/c7
  // transform. MIN blend accumulates across frames WITHOUT clearing; the
  // per-frame item list is view-culled, and dropping off-screen casters
  // would flicker props' shade with the camera. Cleared (re-primed) only
  // when the captured transform changes (streaming region switch).
  nrhi::Texture* world_shadow = nullptr;
  nrhi::TextureView* world_shadow_srv = nullptr;
  bool world_shadow_in_srv = false;
  bool world_shadow_primed = false;
  float world_shadow_rows[12] = {};
  static constexpr uint32_t kWorldShadowSize = 512;
  // Items already accumulated into the current map (mesh + world-transform
  // hash): MIN-blend re-draws of the same geometry are idempotent, so each
  // static item needs to land exactly once per map generation. The periodic
  // accumulation pass then only draws newly streamed/decoded items (normally
  // zero) instead of the whole static item list. Reset on re-prime (clear).
  // Render thread only.
  std::unordered_set<uint64_t> world_shadow_drawn;
  static constexpr uint32_t kUiRegionSize = 1u << 20;
  static constexpr uint32_t kUiRegions = 8;
  nrhi::Buffer* ui_ring = nullptr;
  uint8_t* ui_ring_cpu = nullptr;
  // Last successfully resolved words-key per streamed-art site
  // (mesh << 1 | slot; slot 0 = diffuse override, 1 = decal override).
  // Serves the site's previous art while a new-mip-words decode is in
  // flight (see resolve_fetch_words). Render thread only.
  std::unordered_map<uint64_t, uint64_t> words_sticky;
  // (The fam 5/6 masks+normal t4/t5 descriptor-pair cache is gone: draws
  // bind both views directly via cmd->SetTexturePair(5, spec, normal);
  // pair descriptors are cached inside the backend, so texture replacement
  // can never leave a stale descriptor.)
  // THE texture content store (console identity
  // semantics): fetch-words key -> decode. One
  // words state = one entry; both states of a streaming flap A<->B simply
  // stay resident under their own keys (what the old lookaside simulated
  // with park/take), and superseded states age out via the LRU. Shared by
  // the 3D draw path (through the object routes below), the 2D/HUD pass,
  // and the draw-time fetch-word overrides (posters/ads). Render thread
  // only.
  std::unordered_map<uint64_t, GuestTexture> tex_store;
  // Texture object -> its last STABLE fetch-words state (seqlock
  // double-read at resolve). A route is a lookup aid, never an owner: the
  // game freely retargets objects (mip promote/demote, detail demote,
  // object reuse) and every retarget is just a different store key; a
  // reused object can never serve another binding's art. Render thread
  // only.
  struct TexRoute {
    uint32_t words[6] = {};
    uint64_t key = 0;
    // Live words carry no mip-0 base (streamer demoted the top level; the
    // old pool range is already reused). The route holds the pre-demote
    // state, its cached decode carries the full chain, strictly better,
    // and payload polls are suspended while held (the probes would read
    // the reused pool). A re-promote publishes fresh words and re-routes.
    bool demoted = false;
  };
  std::unordered_map<uint32_t, TexRoute> tex_routes;
  // Sticky texture serving (see resolve_texture in draw_item): the last
  // ADOPTED (served-live) words state per (mesh << 3 | slot). Serves the
  // site's previous art while the current binding's decode is in flight
  // (the console's own mip-transition look), and powers the
  // detail-downgrade hold: a strict base-area shrink keeps serving the
  // previous state's store entry for the hold window. Render thread only.
  struct TexStickySite {
    uint64_t words_key = 0;
    uint64_t area = 0;
    // Nonzero = a downgrade is being held, first seen at this frame. The
    // site adopts the smaller binding once the downgrade persists past
    // skate3_native_render_scene_detail_hold frames.
    uint64_t downgrade_since = 0;
  };
  std::unordered_map<uint64_t, TexStickySite> tex_sticky;
  // First frame a texture object resolved white-with-heal-in-flight:
  // brand-new items (no sticky fallback) skip drawing for a short window
  // instead of flashing white.
  std::unordered_map<uint32_t, uint64_t> tex_pending_first;
  bool failed = false;
  bool announced = false;
};

inline RendererState g_r;

constexpr nrhi::InputElementDesc kSceneInputLayout[7] = {
    {"POSITION", 0, 0, nrhi::Format::kR32G32B32_FLOAT, 0},
    {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 12},
    {"TEXCOORD", 1, 2, nrhi::Format::kR32G32_FLOAT, 20},
    {"BLENDWEIGHT", 0, 3, nrhi::Format::kR8G8B8A8_UNORM, 28},
    {"BLENDINDICES", 0, 4, nrhi::Format::kR8G8B8A8_UINT, 32},
    {"NORMAL", 0, 5, nrhi::Format::kR32G32B32_FLOAT, 36},
    {"TEXCOORD", 2, 6, nrhi::Format::kR32G32_FLOAT, 48}};

// Pipeline / resource-group builders (skate3_native_scene_gpu.cpp). Each is
// idempotent and returns false only on a failure that must abort the native
// path.
bool EnsureRootSignature(const NativeGuestOutputRenderContext& context);
bool EnsureScenePsoFamily(const NativeGuestOutputRenderContext& context);
bool EnsureResolvePso(const NativeGuestOutputRenderContext& context);
bool EnsureBlurPsos(const NativeGuestOutputRenderContext& context);
bool EnsureOutlineEdgePso(const NativeGuestOutputRenderContext& context);
bool Ensure2dPso(const NativeGuestOutputRenderContext& context);
bool EnsureSplinePsos(const NativeGuestOutputRenderContext& context);
bool EnsureShadowPsos(const NativeGuestOutputRenderContext& context);
bool EnsureHeapsAndRings(const NativeGuestOutputRenderContext& context);
bool EnsurePhotoFxPipeline(const NativeGuestOutputRenderContext& context);
bool EnsureShadowResources(const NativeGuestOutputRenderContext& context);
bool EnsureBlurOutlineTargets(const NativeGuestOutputRenderContext& context);
bool EnsureFallbackTextures(const NativeGuestOutputRenderContext& context);
bool EnsureOutputSizedTargets(const NativeGuestOutputRenderContext& context);
// Umbrella builder: runs every group above (plus the base device/queue
// setup) and latches g_r.failed on an unrecoverable failure.
bool EnsurePipeline(const NativeGuestOutputRenderContext& context);
// Sampled view of the current guest-output texture, backed by the
// per-mailbox-image cache (see RendererState::output_views). Returns the view
// for context.guest_output, reusing the cached view on mailbox rotation and
// creating a fresh view only when the output texture is genuinely new.
nrhi::TextureView* EnsureOutputSrvView(const NativeGuestOutputRenderContext& context);
// Shader-desc helper: pairs the HLSL source with its offline-compiled
// SPIR-V blob (Vulkan) by {file, entry, variant}.
nrhi::ShaderDesc MakeShaderDesc(nrhi::ShaderStage stage, const char* file,
                                const char* hlsl_source, const char* entry,
                                const nrhi::ShaderMacro* macros,
                                const char* variant);

// Post passes (skate3_native_scene_post.cpp).
bool EnsureSsaoPipeline(const NativeGuestOutputRenderContext& context);
bool EnsureSsrPipeline(const NativeGuestOutputRenderContext& context);
bool EnsureSsrTargets(const NativeGuestOutputRenderContext& context);
bool EnsureHdrPipeline(const NativeGuestOutputRenderContext& context);
bool ApplySsaoPass(const NativeGuestOutputRenderContext& context,
                   nrhi::Cmd* cmd, const FrameScene& scene,
                   const nrhi::Viewport& viewport, const nrhi::Rect& scissor);
bool ApplySsrPass(const NativeGuestOutputRenderContext& context,
                  nrhi::Cmd* cmd, const FrameScene& scene,
                  const nrhi::Viewport& viewport, const nrhi::Rect& scissor,
                  bool ssao_ran);
bool EnsureVolumetricPipeline(const NativeGuestOutputRenderContext& context);
bool ApplyVolumetricPass(const NativeGuestOutputRenderContext& context,
                         nrhi::Cmd* cmd, const FrameScene& scene,
                         const nrhi::Viewport& viewport,
                         const nrhi::Rect& scissor, bool ssao_ran,
                         uint64_t frame_number);
void ApplyHdrPost(const NativeGuestOutputRenderContext& context,
                  nrhi::Cmd* cmd, const nrhi::Viewport& viewport,
                  const nrhi::Rect& scissor, bool loading_native,
                  uint64_t frame_number);
bool ApplyMenuBlurPass(const NativeGuestOutputRenderContext& context, nrhi::Cmd* cmd,
                       float target_sigma, bool output_in_guest_output_state);
// Blur-over-emulated-frames post processor registered by Install().
void PostProcessGuestOutput(const NativeGuestOutputRenderContext& context,
                            void* user_data);

}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12 || REX_HAS_VULKAN
