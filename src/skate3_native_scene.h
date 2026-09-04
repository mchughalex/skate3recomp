#pragma once

// Native scene renderer (data-driven): consumes the per-frame MeshContext
// stream collected by skate3_native_render.cpp, walks the verified guest
// structures on the game thread into a
// compact NativeFrameScene, and renders it with our own shaders into the
// guest output texture via the SDK native-guest-output hook, replacing the
// emulated frame when active.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::graphics::nrhi {
class Device;
}

namespace skate3::native_scene {

struct DrawEntry {
  uint32_t prim;
  uint32_t base_vertex;
  uint32_t start_index;
  uint32_t index_count;
};

struct DrawItem {
  // Guest MeshContext this item was captured from (0 for frame-end world
  // items served from the item cache). Identity key for the palette serve
  // and the entity store: the owning cModelInstance is resolved backward by
  // m_arrMeshContext containment (there is no forward mesh->owner pointer).
  uint32_t ctx = 0;
  // LivingWorld entity store mapping,
  // stamped at scene build when this ctx belongs to a live LW entity:
  // lw_alpha = the entity's authoritative opacity (entity+528.x: exactly
  // the value BindConstants serves this ctx as the shader's output alpha;
  // -1 = unmapped), lw_entity = the owning entity address (the game's own
  // per-instance identity, used to key the pose rings so same-mesh clone
  // reshuffles can never mispair).
  float lw_alpha = -1.0f;
  uint32_t lw_entity = 0;
  uint32_t mesh;      // guest mesh object address (resource cache key)
  uint32_t vb_obj;    // guest renderengine::VertexBuffer object (draw matching)
  uint32_t ib_obj;    // guest renderengine::IndexBuffer object (draw matching)
  uint32_t vb_addr;   // guest address of raw vertex data
  uint32_t ib_addr;   // guest address of raw index data (u16 big-endian)
  uint32_t vb_bytes;
  uint32_t ib_count;
  uint32_t diffuse_tex;   // guest renderengine::Texture address (0 = none)
  uint32_t lightmap_tex;  // guest renderengine::Texture address (0 = none)
  // Streamed-artwork diffuse override (event posters): the Massive ad
  // system rebinds the diffuse fetch slot at draw time with the current
  // event-ad art: either over a 16x16 min-mip placeholder channel (the
  // ace-of-spades "Big Event" grid) or REPLACING a full-size default
  // poster (the letter-writing frames showing the MONDO "THE NEW VIDEO"
  // ad). When the item's own main-pass draw (validated by slot 3 == this
  // item's lightmap) bound a different texture at fetch slot 4, these six
  // draw-time fetch words resolve the diffuse instead (words-keyed texture
  // cache). All zero = no override.
  uint32_t diffuse_fetch[6] = {};
  // Same mechanism for environment.decal: that family samples its decal
  // overlay art at fetch slot 4 (diffuse rides slot 6), and ad frames
  // covered by a decal section get the event art bound there at draw time.
  uint32_t decal_fetch[6] = {};
  // "macrooverlay" material channel (environment shaders): a large-scale
  // grime/crack overlay multiplied over the diffuse at uv *
  // macroOverlayUVScale with macroOverlayOpacity: the ground/wall
  // weathering that reads as "extra grit".
  uint32_t macro_tex;     // guest renderengine::Texture address (0 = none)
  float macro_scale;      // macroOverlayUVScale channel constant
  float macro_opacity;    // macroOverlayOpacity channel constant
  // uAnimationSpeed / vAnimationSpeed channel constants (texcoords per
  // second of the frame's animation time): the scrollincandescent UV
  // scroll (fam 14: the stadium LED chyron band). 0 = no scroll.
  float scroll_u;
  float scroll_v;
  uint16_t pos_offset;
  uint16_t uv_offset;
  uint16_t uv2_offset;
  uint16_t bw_offset;  // blend weights (u8x4), valid when skinned
  uint16_t bi_offset;  // blend indices (u8x4), valid when skinned
  uint16_t normal_offset;  // usage-3 vertex normal (0 fmt = none)
  // usage-6 tangent / usage-7 binormal (k_10_11_11): NPC character meshes
  // (default_cloth / default_hair / livingworld peds / skateboard) carry NO
  // usage-3 normal; the game's VS derives it as cross(tangent, binormal)
  // (verified numerically against the emulated VS outputs, dot 0.9999).
  // tb_fmt = 16 when both are present; DecodeMesh then synthesizes the
  // normal, replacing the faceted ddx/ddy face-normal fallback.
  uint16_t tangent_offset = 0;
  uint16_t binormal_offset = 0;
  uint8_t tb_fmt = 0;
  uint8_t stride;
  uint8_t pos_fmt;    // xenos vertex format (26 s16x4, 32 half4, 57 float3)
  uint8_t uv_fmt;     // xenos vertex format of the first texcoord (0 = none)
  uint8_t uv2_fmt;    // xenos vertex format of the second texcoord (0 = none)
  uint8_t normal_fmt;  // 16 = k_10_11_11 packed (0 = none, face-normal fallback)
  // sky.* materials draw fullbright: per-facet lighting turns the multi-km
  // sky dome into visibly shaded rectangular panels.
  bool unlit;
  bool skinned;
  // Grayscale-tinted material (CAS hair): the shader multiplies the diffuse
  // by a per-character color staged in the PIXEL constant bank. Detected via
  // the AttribulatorMaterialName channel; tint captured with the palette.
  bool hair;
  // Character-family exact shading (defaultcharacter.fx and friends).
  // Classified from AttribulatorMaterialName: 1 = character.default_*
  // (defaultcharacter PS layout), 2 = CAC player pieces (skin / face / cloth
  // / leather / shift / cloth_ropa: cacstamp layout), 3 =
  // character.livingworld_* pedestrians (stamp recolor), 4 = character.hair
  // (CAC), 5 = character.default_hair (NPC hair), 6 =
  // character.livingworld_vehicles (vehicle.fx: colorize_red/blue paint
  // recolor + phong spec + environment-cube reflection), 7 =
  // character.livingworld_vehicles_glass (reflection-only, alpha-blended
  // windows). 0 = not a character.
  // char_rows = the canonical per-draw lighting block captured from the
  // PIXEL constant bank at palette-capture time (CaptureCharLighting):
  // 18 float4 rows consumed by the scene PS character branch (cbuffer CH,
  // b2). rows[14].y = family, > 0 only when the capture validated; items
  // without it fall back to the legacy empirical character shading.
  uint8_t char_family = 0;
  float char_rows[72] = {};
  // Hair strand coverage: the hair mesh's "alpha" channel texture, sampled
  // at the SECOND texcoord (raw float2); hair renders alpha-blended in the
  // sorted sub-pass (the opaque path is the blocky-helmet look).
  uint32_t hair_alpha_tex = 0;
  // AttribulatorMaterialName starts "environment.decal": wall/ground
  // sections whose pixel shader composites a `decal` art texture (graffiti,
  // painted branding, grime) over the base diffuse INSIDE the shader,
  // lerp(base, decal.rgb, decal.a), and draws OPAQUE (disassembled from
  // decalenvironment_defaultPS; these meshes ARE the surface there, so
  // rendering them alpha-blended punches holes in walls). Without the
  // composite the paint is missing (the no-graffiti bug).
  bool decal;
  // environment.decal_tileable: the decal art TILES across the surface
  // (rock/cliff faces) and must sample with WRAP; single-placement decal
  // art samples with CLAMP (its transparent border keeps the area outside
  // the placement clear).
  bool decal_tileable;
  uint32_t decal_art;  // `decal` channel texture (0 = none)
  // AttribulatorMaterialName starts "environment.transparent": mist/cloud
  // sheets, glass, fences, vines. Alpha-BLENDED in a sub-pass after all
  // opaque items (back-to-front, depth test on, z-write off); the opaque
  // pass's alpha-test turned the soft mist gradients into solid hard-edged
  // white cloud blobs.
  bool transparent;
  // AttribulatorMaterialName starts "water.": canal/ocean surfaces
  // (flowingwater.fx family). Drawn in the transparent sub-pass with a
  // dedicated shading branch (ripple normal taps + fresnel reflection);
  // `water_normal` is the material's `normal` channel texture, bound in the
  // macro slot (water never carries a macro overlay).
  bool water;
  // AttribulatorMaterialName starts "water.flowing": the flowingwateralpha
  // shader family (canal / waterfall surfaces). These take the exact water
  // branch (hand-ported from flowingwateralpha_defaultPS and verified
  // per-pixel against the ucode) when the frame's water m_params rows are
  // captured; other water.* / ocean.* materials keep the legacy water
  // shading until their own shader layouts are verified from a capture.
  bool water_flowing = false;
  // ocean materials: 1 = ocean.default (the sea surface, ocean_defaultPS:
  // PCA dual-component normal + ward spec + cube reflection, drawn OPAQUE),
  // 2 = ocean.reflection (the baked horizon reflection sheet, height-ramp
  // alpha). Exact branches gated on the frame's ocean rows.
  uint8_t water_ocean = 0;
  uint32_t water_normal;
  // `normal2` channel: the ocean's second PCA normal component map (the
  // flowingwater families ignore it).
  uint32_t water_normal2 = 0;
  uint32_t water_env;  // `environment` channel cube map (reflection term)
  // Usage-6 tangent present WITHOUT a usage-7 binormal (tan_fmt = 16, the
  // water-mesh layout: pos + uv + lm_norm + tangent). The flowingwater VS
  // reconstructs the frame as N from lm_norm.zw + sign bits, B =
  // sign.x * cross(N, T); DecodeMesh mirrors that (see the water pack).
  uint8_t tan_fmt = 0;
  // character.cloth_ropa (Ropa cloth-simulated garments, e.g. player tees):
  // the VS variant branches on a flag row kept in front of the bone palette
  // - sim active means the dynamic VB already holds deformed root-local
  // positions and the draw is RIGID (one affine at c188/c191), not skinned.
  // Detected via the AttribulatorMaterialName channel; consumed by
  // CaptureSkinnedState.
  bool ropa;
  // Garment-to-hide under nude mode. True when ropa is true (the
  // character.*_ropa cloth-sim variant) OR the material is a plain cloth/
  // leather/jacket character piece (character.cloth with no _ropa suffix —
  // some skater garments are authored without the cloth-sim suffix, so the
  // ropa-only check alone let the tee leak through nude mode). Deliberately
  // does NOT cover skin/face (also char_family 2) so nude keeps the body.
  bool garment;
  // character.alpha: translucent CAC accessory pieces (sunglass lenses).
  // cac_alphaPS outputs alpha = the mesh's "alpha" channel texture sampled
  // at the raw second texcoord x the entity alpha constant (ucode: oC0.w =
  // tf5(uv2).r * c22.x) and the game draws them AFTER every opaque
  // character piece: rendered opaque the lens is a solid black patch.
  // Routed to the sorted alpha sub-pass like hair; the coverage texture
  // rides the same hair_alpha_tex/decal-slot plumbing (ch_misc.z > 0
  // signals the PS fam-2 branch to take alpha from t4@uv2).
  bool char_alpha = false;
  // The game submitted this ctx through an ortho (shadow caster-cascade)
  // bank this frame; the game's caster list is PER-PIECE (the CAS trucker
  // hat never enters the shadow passes) and the native atlas pass mirrors
  // it for character items. Rescued/gap-filled copies inherit the stored
  // flag so a refused-capture frame doesn't blink the entity's shadow.
  bool shadow_caster = false;
  // ROPA shape blending (set by InterpolateDynamicItems): decoded shape
  // generations (DynDecodeJob seq) + weights reproducing the SAME temporal
  // kernel the body bones / garment world were evaluated with this frame:
  // the 8-tap boxcar when the world took it, else the plain bracketing
  // lerp. shape_count == 0 = no pairing this frame (draw the newest cached
  // decode). The stepped cloth shape against the interpolated body was the
  // tee jelly/clip-through (worse at LOWER fps: the excursion scales with
  // the guest period); a plain 2-generation lerp against the BOXCAR-
  // filtered body left the same period-scaled mismatch; the two filters
  // answer the same 60 Hz limb signal with different frequency responses,
  // so the kernels must match exactly.
  // 16 = the hard bound: the 8-tap kernel contributes at most two distinct
  // generations per tap, so no ingest cadence can overflow this.
  static constexpr int kShapeGens = 16;
  uint64_t shape_seq[kShapeGens] = {};
  float shape_w[kShapeGens] = {};
  int shape_count = 0;
  // Exact world-material shading family, classified from
  // AttribulatorMaterialName (models verified against the game's own pixel
  // shaders):
  // 0 = none (legacy empirical shading), 1 = environment.default
  // (baseenvironment), 2 = environmentsimple.default (defaultenvironment),
  // 3 = environment.decal, 4 = environment.decal_tileable(_simple),
  // 5 = environment.reflective, 6 = environment.reflective_simple,
  // 7 = environmentsimple.alphatest, 8 = environmentsimple.diffuse,
  // 9 = tree.default, 10 = animated.tree, 11 = proxyworld.default,
  // 12 = incandescent.default, 13 = environment.reflective_trans
  // (transparentenvironmentreflective: blended glass, alpha sub-pass),
  // 14 = incandescent.backlituvscroll (scrollincandescent: the emissive
  // time-scrolled LED chyron band).
  uint8_t env_family = 0;
  // dynamicobject.fx family (movable props: dispensers, dumpsters, benches,
  // cans, ...). 0 = not a dynamic object, 1 = dynamicobject.default,
  // 2 = dynamicobject.alphatest. These are rigid model-space props with per-
  // frame lighting rows (sun / ambient / exposure) captured from their PS
  // bank, NOT the frame-global world-material rows; hence a separate family
  // and its own scene PS branch (model verified exact against the game's
  // own pixel shader).
  uint8_t dynobj = 0;
  // "specular" channel texture (spec mask / eccentricity / reflection mask)
  // for env families; the "noise" tint texture for animated.tree. Bound in
  // the decal slot (t4) on families that carry no decal art.
  uint32_t spec_tex = 0;
  // "detail" channel texture (env families 5/6): a constant 16x16 BC1 whose
  // hardware-decoded value folds into the normal composition as 2*d - 1,
  // derived per material at draw time (see the refl_bias cvars). On fams
  // 1/3/4 (per-pixel world shading) it is a REAL tiling detail normal map,
  // sampled per-pixel at t8 with the detailNormalUVScale channel constant
  // below.
  uint32_t detail_tex = 0;
  float detail_scale = 0.0f;
  float tint[4];  // rgb + enable flag in w
  // The item's per-draw state (bone palette for skinned, world matrix for
  // rigid model-space props) was not available at capture time; deferred
  // meshes draw AFTER the submit call, and world sort-list captures happen
  // before any draw. Cleared by the post-draw (ib,vb) fixup; items still
  // pending at frame end are dropped (wrong state renders garbage).
  bool pending;
  // Diagnosis: which path staged this item's palette/world. 0 = none/raw,
  // 1 = submit-exit capture, 2 = post-draw (ib,vb) fixup, 3 = bones-cache
  // rescue, 4 = ropa state rescue, 5 = ropa mode-flip hold (previous state
  // re-published while the flipped mode's decode is still in flight),
  // 6 = publish-time incoherent-palette heal (cached palette substituted
  // after the dense 32-sample spread gate failed).
  uint8_t dbg_src = 0;
  // The palette was captured from an ORTHOGRAPHIC (CSM caster-cascade)
  // bank. The game does not reliably re-evaluate fine bone animation for
  // the shadow passes (vehicle wheel spin measured ~40 ms stale in bursts)
  // - publishing such a palette as the item's MAIN pose makes the pose
  // stream jump and resets the motion-smoothing ring (the traffic judder).
  // Caster-sourced captures lose merge arbitration to perspective-bank
  // captures and stay registered for a refresh from the mesh's later
  // main-pass draw.
  bool caster_bank = false;
  // Cloth patch (non-indexed quad list, CPU-simulated absolute world-space
  // float3 verts): the renderer synthesizes quad->triangle indices instead
  // of reading a guest index buffer.
  bool cloth_quads;
  // Park-editor / object-mover selection: the game re-draws the selected
  // object after the sky (twice, stencil-marking it) and a postfx stencil
  // edge-detect adds the blue outline. Items matched to those re-draws by
  // (ib_obj, vb_obj, world translation) render into the native outline mask
  // (see kOutlineShaderSource).
  bool selected = false;
  // Re-appended from the off-screen retention map (edge-of-view guard band):
  // the game view-culled this item this frame, but the re-timed render
  // camera trails the guest pose and can still see it. Retained items
  // outlive their guest-side lifetime guarantees, so the render path draws
  // only their exact cached decodes: no guest-memory reads, no heals, no
  // miss enqueues.
  bool retained = false;
  // Captured through the dynamic-entity paths (characters, props, cloth,
  // quad-list garments) rather than the world sort lists. Set at capture so
  // every later copy (merges, rescues, retention re-publishes) inherits it;
  // the build-up showcase's dynamic-entities layer keys off it.
  bool dyn_entity = false;
  // Bone palette snapshot taken on the game thread: raw staged rows, 3
  // float4s per bone (column-vector affine [R | t], model -> world). The
  // guest staging bank is reused draw to draw, hence the copy.
  std::vector<float> bones;
  // Content fingerprint of the guest payloads. Streaming reuses arena
  // addresses and pages fill in after the mesh is first submitted, so cached
  // GPU copies must be revalidated against this every frame.
  uint64_t fingerprint;
  float world[16];    // row-vector convention, translation in row 3
  // Character-size pivot: the character's root-borne world position (the
  // skinned path's bone 0 = the skeleton root), captured AFTER pose
  // interpolation so it matches the palette the renderer uploads. Rigid
  // (non-skinned) char_family 1/2 pieces scale their world.translation
  // about this point so accessories (hats / jewellery / rigid ropa) track
  // the scaled body: the skinned body scales every bone translation about
  // root, so a rigid piece that keeps its world translation fixed detaches
  // while the body grows. root_valid gates the scale: false = no trusted
  // character root this frame, the translation stays unscaled (pre-fix
  // behavior, never worse than before).
  float root_pos[3] = {};
  bool root_valid = false;
  float bbox_min[3];  // mesh-local bounds, for decode validation
  float bbox_max[3];
  std::vector<DrawEntry> draws;
};

struct FrameScene {
  uint64_t generation = 0;
  float view_proj[16] = {};
  float cam_pos[3] = {};
  // Raw guest projection matrix (viewcam +0x60, row-vector, m23 = 1),
  // published alongside view_proj. Consumed by depth-based post passes
  // (SSAO linearize/unproject via m00/m11/m22/m32). Zeros until the first
  // publish; camera smoothing replaces only the view, never this.
  float proj[16] = {};
  // Global distance-fog parameter rows, captured once per frame from the
  // first main-pass draw's VS constant bank (main-pass layout: c4 = camera,
  // the validation key; c5 = fog ramp scale/bias/exponent, c6 = linear-space
  // fog color rgb + transmittance scale in w; identical across every
  // main-pass draw of a frame). Consumed by environment.transparent items.
  float fog_ramp[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  float fog_color[4] = {};
  // Dynamic-shadow (CSM) receiver constants, captured once per frame from a
  // world-material draw's PIXEL constant bank: raw rows c0..c8
  // (c0/c3/c4 = light-space X/Y/depth rows, c1/c2 = cascade 1/2 scale+offset,
  // c5.x = depth bias, c8 = shadow color + floor; c6 = sun dir, c7 = camera,
  // captured for validation only).
  bool shadow_valid = false;
  // True while the rows above were captured within the last ~2 s of guest
  // frames. shadow_valid alone latches forever, so interior venues whose
  // environment banks never pass the capture sanity gate (park-editor
  // warehouses: near-zero sun, exposure ~0.001) serve stale outdoor rows;
  // sun-derived features (the native static sun-shadow map) must stand down
  // there instead of shading a sunless interior with an outdoor sun.
  bool shadow_fresh = false;
  // Extended to c0..c11: c10.x = scene exposure (2.5), c11.y = the material
  // multiplier (1.0), consumed by the exact world-shading tone chain.
  float shadow_rows[48] = {};
  // Frame-global rows of the OTHER world shader families, captured
  // opportunistically from their PS banks (debug-path classified):
  // [0..1] = tree PS c0.xy (lightmap scale 0.3435 / floor 0.02),
  // [2] = tree PS c4.y (tint multiplier), [3] = proxyworld PS c3.y (0.45).
  // Defaults from the day capture cover frames before the first hit.
  float family_rows[4] = {0.3435f, 0.02f, 1.0f, 0.45f};
  // flowingwateralpha.fx (the water.flowing* materials) m_params rows,
  // captured once per frame from a flowingwateralpha PS bank (debug-path
  // classified): [0..3] = m_params[0] (PS c11: normal scale xzw + the
  // material multiplier in y), [4..7] = m_params[1] (c12: the two animated
  // UV scroll speeds), [8..11] = m_params[2] (c13: the two tap UV scales),
  // [12..15] = m_params[3] (c14: y = mask threshold, z = spec power,
  // w = alpha floor), [16] = g_fAnimationTime (c15.x). Consumed by the
  // exact water scene PS branch (cam_pos.w = -30).
  bool water_valid = false;
  float water_rows[17] = {};
  // ocean_defaultPS material rows, captured once per frame from an ocean PS
  // bank (debug-path classified): [0..3] = PCA mean pre-swizzled for the
  // (R,G,B) channel order (PS c2.x, c2.z, c2.y), [4..27] = the six PCA
  // weight rows in model order (c3, c4, c7, c8, c5, c6), [28..31] =
  // m_params[0] (spec color, c12), [32..35] = m_params[1] (c13: y = mult,
  // z = tonedown scale, w = uv scale), [36..39] = m_params[2] (c14: ward
  // spread u/v + roughness).
  bool ocean_valid = false;
  float ocean_rows[40] = {};
  // oceanreflection_defaultPS row (PS c3): x = multiplier, y/z = the
  // height-ramp alpha fade bounds.
  bool oceanrefl_valid = false;
  float oceanrefl_rows[4] = {};
  // scrollincandescent.fx rows, captured once per frame from a
  // scrollincandescent draw (debug-path classified): [0] = g_fAnimationTime
  // (VS c9.x), [1] = the material multiplier m_params[0].y (PS c3.y).
  // Consumed by the fam-14 branch (the scrolling LED chyron).
  bool scroll_valid = false;
  float scroll_rows[2] = {};
  // dynamicobject.fx frame-global lighting rows, captured from a
  // dynamicobject/alphatestdynamicobject PS bank (debug-path classified):
  // [0..2] sun direction (PS c9), [3] scene exposure (c13.x),
  // [4..6] flat ambient rgb (c15.xyz), [7] bounce scale (c15.w),
  // [8] material multiplier (c14.y), [9] static world-shadow floor (c8.w).
  // Consumed by the exact dynamicobject scene PS branch.
  bool dynobj_valid = false;
  float dynobj_rows[10] = {};
  // dynamicobject.fx static world-shadow transform (PS c5/c6/c7, captured
  // with the rows above): world-space projection rows into the game's
  // 512x512 baked-shade map (uv = dot(wp4, c5/c6) * (+-0.5) + 0.5, ray
  // depth = dot(wp4, c7)). The guest never renders that map in native mode
  // (its memory stays zeroed), so the native path re-renders it from the
  // frame's STATIC world items with these rows (see RenderShadowAtlas) and
  // props sample it exactly like the game's PS: shadow = min(csm * gate,
  // max(world, c8.w)); without it props inside baked building shade lit
  // at full key (the newspaper-machine mint brightening).
  bool dynobj_ws_valid = false;
  float dynobj_ws[12] = {};
  // Character-receiver per-cascade CSM receive biases, captured from a
  // validated fam-2 character PIXEL bank (gameplay c8 / editor c9: one row
  // below the light row; frame-global, e.g. (0.005, 0.014, 0.020)). The
  // game's character shaders build their shadow reference as
  // saturate(rd - bias[cascade]) and take NINE point taps of the atlas at
  // +-1 game-texel, averaged (cacstamp_skin_nisPS ucode; literal 1/9 =
  // c255.w). 0 = not captured -> legacy single-tap char path.
  float char_shadow_bias[3] = {};
  // Sky-dome viewpos: sky.* meshes are camera-relative (sky.fx VS adds
  // g_vViewPos), but the game pins the sky viewpos Y at a fixed level
  // elevation (165.0 in every capture) so the skyline doesn't bob with the
  // skater. Captured per frame from the sky draw's VS bank; the default
  // covers frames before the first capture.
  float sky_height = 165.0f;
  // Sky sun rows, captured together with sky_height from the sky draw's
  // PIXEL bank (sky_defaultPS layout): [0..2] = g_vLightDir (unit vector
  // toward the sun), [3] = sun angular scale (m_params[0].x),
  // [4] = sky pre-tone multiplier (m_params[0].y), [5] = scene exposure
  // (g_envattributes[2].x). Consumed by the exact sky scene-PS branch;
  // the emulated frame's big sun glow is computed IN the sky dome shader
  // from a 1D radial gradient (the sky material's `specular` channel),
  // not by a separate draw.
  bool sky_sun_valid = false;
  float sky_sun[6] = {};
  // Fullscreen UI background blur: 0 = off; otherwise the kernel scale of
  // the game's blur_hBlur/vBlur + basictex pass chain (PS c0.x, 8 in every
  // capture), detected per frame from the blur_hBlurPS draw. The native
  // replica runs after the MSAA resolve, before the 2D overlay draws.
  float ui_blur = 0.0f;
  // Blur modulate color (the blur passes' PS c1; both blur ucodes end in
  // `mul oC0, r0, c1`, so it applies per pass): (1,1,1) under gameplay
  // popups, the dark menu fade in the pause menu; its square is the
  // darkened pause backdrop.
  float ui_blur_color[3] = {1.0f, 1.0f, 1.0f};
  // Selection-outline color (postfx_edgedetectstencilPS c0; the park-editor
  // blue in every capture), refreshed from the guest edge-detect draw when
  // it runs. Consumed by the outline composite when any item is selected.
  float outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
  // Photo-editor postfx chain (photo_fx.hlsl, exact ucode ports): the live
  // PS/VS constant rows of the game's own postfx passes, captured per frame
  // at the SetPending_AluConstants hook while a photo-mission photo editor
  // is up, plus the fetch words of the two static input textures the chain
  // samples (the 512x2 vignette gradient and the grain texture). Pass order:
  // visualfx, dof downsample, tap9dofMotionBlur, tap9dof, uber, fisheye.
  struct PhotoFx {
    bool valid = false;
    float ps[6][32][4] = {};
    float vs[6][8][4] = {};
    uint32_t vignette_fetch[6] = {};
    uint32_t grain_fetch[6] = {};
  };
  PhotoFx photo_fx;
  std::vector<DrawItem> items;
  // MeshContexts the build-side occlusion skip left out of `items` this
  // frame (see skate3_native_render_scene_occlusion_cull_build): the
  // render thread re-publishes them as culled so the guest dispatch filter
  // keeps excluding them between their staggered rebuild frames.
  std::vector<uint32_t> occl_build_skipped;
};

// One frame submission record from the hook layer (see RenderMeshRecord).
struct SubmitRecord {
  uint32_t kind;
  uint32_t a;
  uint32_t b;
  uint32_t c;
};

// Graphics build-up showcase layer registry: the order-string tokens
// (cvar skate3_native_render_scene_showcase_order), the per-layer shader
// mask bits (contract in scene.hlsl ShowcaseMask: the split rows carry
// 256 + the cumulative mask, 0 = showcase off) and the display labels.
// Shared by the sequencer (skate3_native_scene_gpu.cpp) and the F12 setup
// window (skate3_native_debug_dialog.cpp). The material bits are
// progressive looks (materials subsumes lighting subsumes albedo); the
// rest are independent and can reveal in any order or grouping.
// The decals and dyn layers are SUBTRACTIVE: their content is part of the
// normal frame and the layer hides it until revealed. When such a layer is
// left out of the order (or '-'disabled), the sequencer folds its bit into
// every step so the run matches the non-showcase frame from clay onward.
// The decal/grime composite only renders on the full material look, so the
// decals layer reads best placed after materials.
// Bit 1024 (blackout) is NOT an orderable layer: the sequencer brackets
// every run with it (snap to black at start, wipe to black at the end) as
// the recording cut marker.
struct ShowcaseLayer {
  const char* token;
  uint32_t bit;
  const char* label;
};
inline constexpr ShowcaseLayer kShowcaseLayers[] = {
    {"albedo", 1u, "albedo textures"},
    {"lighting", 2u, "baked lighting"},
    {"materials", 4u, "materials & surface detail"},
    {"decals", 256u, "decals & grime"},
    {"dyn", 512u, "dynamic entities"},
    {"shadows", 8u, "dynamic shadows"},
    {"ao", 16u, "ambient occlusion"},
    {"ssr", 32u, "reflections"},
    {"vol", 64u, "volumetrics"},
    {"bloom", 128u, "bloom"},
};
inline constexpr size_t kShowcaseLayerCount =
    sizeof(kShowcaseLayers) / sizeof(kShowcaseLayers[0]);
// Bits of the subtractive layers (see above).
inline constexpr uint32_t kShowcaseSubtractiveMask = 256u | 512u;
inline constexpr const char* kShowcaseOrderDefault =
    "albedo,lighting,materials,decals,dyn,shadows+ao,ssr,vol+bloom";

bool Enabled();

// Host settings-overlay backdrop: while the SDK settings menu is open, run
// the game's own popup blur chain (FrameScene::ui_blur) over every finished
// frame; the game issues no popup blur of its own then. The overlay draws
// its dark tint on top of the blurred scene.
void SetSettingsMenuBlur(bool enabled);

// The captured (un-overridden) sun direction, for the debug dialog's sun
// override to seed its sliders from (unit vector toward the sun).
void GetCapturedSunDir(float out[3]);

// While the drone/free-fly camera is engaged, fills out_pos with the flown
// camera world position and returns true (false = guest camera in control).
// Consumed by the draw-distance hooks to recenter distance culls on the
// drone; the render camera itself is taken over via the ViewCamera::
// SetViewMatrix override in skate3_native_scene.cpp.
bool FreecamGuestPose(float out_pos[3]);

// True while the guest sits on a loading screen or in the frontend: the
// presence context is out of gameplay AND the world has stopped publishing
// perspective scenes. The in-game pause menu keeps publishing behind the
// menu and reports false. Mirrors the pause-vs-loading distinction in
// YieldForMenus (same 300 ms publish-staleness window). With scene capture
// disabled entirely, publishes never arrive and every out-of-gameplay
// context reports true (the conservative side).
bool LoadingOrFrontendActive();

// The game's per-entity spawn/streaming fade as carried by the item's
// validated character-lighting capture (peds c21.x, defaultcharacter c13.x,
// cacstamp c22.x, vehicle body c20.x, hair strand scale). 1.0 for items
// without a validated capture. Defined in skate3_native_scene.cpp; consumed
// by the render passes.
float CharFadeAlpha(const DrawItem& item);

// True when all 8 corners of the item's world-space bbox fall outside one
// clip plane of `vp` (row-vector view*proj). `margin` scales the tested
// frustum: < 1 shrinks it, > 1 widens it. Defined in
// skate3_native_scene.cpp; consumed by the off-screen retention pass and the
// per-item profiling attribution.
bool ItemOutsideFrustum(const DrawItem& it, const float vp[16], float margin);

// Occlusion-cull handoff for the guest-side dispatch filter (defined in
// skate3_native_scene_gpu.cpp): copies the most recent render frame's
// culled-static MeshContext set (sorted ascending) into `out`, clearing it
// instead when the last publish is older than a quarter second (native
// render idle: menus, loading). Returns true when `out` is non-empty.
bool CopyOcclusionCulledCtxs(std::vector<uint32_t>& out);
// Counts sort-list entries the guest-side filter kept away from the guest
// dispatcher (frame-stats telemetry).
void AddGuestOcclSkipped(uint32_t n);

// Called from the cProcessArenaAsset::RegisterTexture hook: guid -> guest
// renderengine::Texture object.
void OnRegisterTexture(uint64_t guid, uint32_t texture);

// Queue one mesh for the prewarm decode workers (guest thread; deduped per
// load).
void OnMeshRegistered(uint8_t* base, uint32_t mesh);

// Called (post-call) from the pegasus::tRModelData::Fixup hook: the
// rw-arena LOAD-time pointer resolve, fired per model while its arena
// streams in. The EARLY prewarm source: it spreads mesh/texture decoding
// across the load's whole disk-streaming phase instead of the
// final-seconds activation burst. (The `Unfix` atoms are the save/size
// path and never run at load.)
void OnModelFixup(uint8_t* base, uint32_t model);

// Called (post-call) from the WorldPresentation::AddRenderInstance hook:
// the world registry add that fires per placed instance during the load's
// final ACTIVATION phase. Walks tInstance -> tRModelData -> mesh table
// (self-validating offsets) and queues every optimesh-form mesh: the
// completeness backstop behind OnModelFixup.
void OnAddRenderInstance(uint8_t* base, uint32_t instance);

// Called from the D3D::SetPending_AluConstants hook (guest render thread,
// invoked from inside the Draw* functions). Records the location of the
// device's positional 256-register VS constant shadow bank.
void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device);

// Called (per guest frame) from the Sk8::Challenge::PhotoReplayController::
// Update hook while a photo-mission's photo editor is up. Heartbeat for the
// photo-editor yield: the editor's depth-of-field / saturation / brightness
// / contrast effects are the game's own postfx chain, which only the
// emulated path executes.
void OnPhotoReplayUpdate();
// Called from the Sk8::FE::FrontEndState_Replay2::TakePhoto hook: a photo
// was just taken (replay editor / photo mission). Arms the photo-grab
// forced-readback window for the frames that render and CPU-grab the
// screenshot target.
void OnTakePhoto();
// ScreenshotBackEnd grab REQUEST (fires 1-2 frames BEFORE the game renders
// the shot + card-composite frame sequence): arms the shutter burst.
void OnPhotoGrabRequest();
// GrabScreenshot COMPLETED (post-call): every card-build read from here on
// is CPU-side of already-copied memory, closes the shutter burst.
void OnPhotoGrabDone();
// True while the photo display-card quad is in the live 2D stream during an
// armed photo flow; the compose auto-trace trigger keys on it.
bool PhotoCardVisible();
void OnMovieDecode();
void OnMovieFrame(uint8_t* base, uint32_t renderer);

// 2D/APT phase bracket (the HUD and every other 2D element is a Flash SWF
// converted to EA APT, rendered through FE::AptRenderingIntegration). Guest
// draws issued while inside a bracket are 2D draws. bit: 0 =
// FrontEndManager::Render2D, 1 = AptMovieIntegration::Render, 2 =
// AptRenderingIntegration::DrawRenderingUnit, 3 =
// AptRenderingIntegration::UpdateRenderToTexture (in gameplay the whole HUD
// renders inside bit 3 into a screen-sized overlay texture at true screen
// coordinates; replaying those draws directly is the composite), 4 =
// Sk8::Render::cFont::DrawstringLocal (glyph text: trick names/scores;
// text can flush outside the APT brackets), 5 =
// Sk8::Render::SimpleDraw::DrawParameters::Draw (immediate-mode quads:
// chase arrows, in-world markers/beams).
void On2dPhase(uint32_t bit, bool enter);

// Current guest shader objects (D3DDevice_SetPixelShader/SetVertexShader),
// recorded per draw so offline analysis can group the 2D draw stream by
// shader variant.
void OnSetShader(bool pixel, uint32_t obj);

// D3D::SetPending_RenderStates(device, mask, bank, ptr): the render-state
// shadow bank (blend/depth state for the 2D pass lives here).
void OnRenderStateUpload(uint64_t mask, uint32_t bank, uint32_t ptr);

// D3DDevice_SetViewport / SetRenderState_ScissorTestEnable-adjacent scissor
// tracking, recorded per draw (render-to-texture APT passes show up as
// non-fullscreen viewports).
void OnSetViewport(uint8_t* base, uint32_t viewport_ptr);
void OnSetScissor(uint8_t* base, uint32_t rect_ptr);

// Guest buffer-binding trackers + post-draw state fixup: each
// DrawIndexedVertices whose bound IB/VB match a pending item fills that
// item's bone palette / world matrix from the constant bank (FIFO one-shot
// per buffer key; clones share mesh assets and draw in submit order).
void OnSetIndices(uint32_t ib_obj);
void OnSetStreamSource(uint32_t stream, uint32_t vb_obj, uint32_t offset, uint32_t stride);
// func: 0 = DrawIndexedVertices (also runs the pending fixup),
// 1 = DrawVertices, 2 = BeginVertices (inline/cloth vertex path).
void OnDrawDone(uint8_t* base, uint32_t func, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7);

// Offline-analysis recording, armed alongside the guest-memory snapshot:
// captures every draw's bound buffers + full VS constant bank, plus the
// per-frame captured items and final scene, every `stride` frames. Written
// as <stem>.scene.jsonl and <stem>.draws.bin next to the .gsnap.
void StartRecording(uint32_t stride);
void WriteRecording(const char* dir, const char* stem);

// Called post-call from the DrawVertices hook for cloth patch draws
// (r6 == 0x80000000 signature): reads the bound dynamic buffer's vertex
// fetch block and builds a world-space quad-list item. Returns a
// dynamic-state index (+1), 0 when the draw is not a cloth patch;
// *out_key receives the garment's stable identity (the buffer object).
uint32_t CaptureClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6,
                          uint32_t r7, uint32_t* out_key);

// Total guest draws completed (any draw function). The RenderMesh hook
// samples this around the original call to detect deferred meshes: if no
// draw ran inside, the constant bank still belongs to an earlier mesh and
// the item's transform/palette must come from the post-draw fixup.
uint64_t DrawSequence();

// Called from the RenderMesh hook (dynamic entities) and the world per-mesh
// draw hook (sub_827FAEA8), AFTER the original call returns (the mesh's
// constants are only flushed to the shadow bank by its own draws). Builds
// the complete DrawItem: geometry from the transient per-frame arena plus
// world matrix / bone palette read live from the shadow bank. drew_inside
// says whether any guest draw ran during the original call; when false the
// bank is stale and the transform is left pending for the post-draw fixup.
// With world_path, bails out early for meshes that need no per-draw
// transform (absolute fmt-57 geometry, handled at frame end) and captures
// only skinned meshes and rigid model-space props (vending machines and
// other movables reach the frame ONLY through the sort lists). Returns a
// dynamic-state index (+1), 0 on failure/skip.
uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx, bool world_path = false,
                             bool drew_inside = false);

// Called on the guest render thread at frame end with the frame's records.
void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count);

// Registers the native guest output renderer with the SDK. Safe to call once
// at startup regardless of cvar state.
void Install();

// Runtime renderer switch (bound to F5): flips skate3_native_render_scene
// live. Capture hooks, scene build, guest-output replacement and
// emulated-draw suppression all re-check the cvar every frame, so the swap
// between the native renderer and the emulated GPU is seamless. Returns the
// new state; refuses (returns false) when the skate3_native_render hook
// layer was not enabled at boot.
bool ToggleSceneEnabled();

// Current native RHI device (null until the scene pipeline has built).
// Used by the F12 diagnostics overlay to pull live per-frame perf/ledger data.
rex::graphics::nrhi::Device* GetNativeSceneDevice();

// True once the scene renderer has hit an unrecoverable pipeline/resource
// failure and yields to the emulated output until re-enabled. Surfaced so
// the render-mode indicator can show the degraded state even though the
// renderer cvars are still enabled.
bool SceneFailed();

// Clears the sticky failure so the next frame retries the full pipeline
// build (ToggleSceneEnabled calls this on re-enable: a transient allocation
// failure can succeed on retry; a persistent one simply re-latches).
void ResetSceneFailure();

// Debug-dialog cache flushes (F12 native-render debug menu): retire every
// cached GPU texture / mesh decode so hot-toggled decode settings (mip
// chains, revalidation, ...) rebuild immediately.
void FlushTextureCache();
void FlushMeshCache();

// Camera-signal recorder (judder diagnosis): for `seconds`, records every
// distinct guest camera pose (1 kHz sampler timestamps) plus the per-frame
// raw and smoothed headings, then writes logs/cam_signal_<ts>.csv. Pan the
// camera with the stick at a steady rate while it runs; offline analysis
// shows whether the game's own pose sequence is irregular at the source.
void RecordCameraSignal(double seconds);

// F7: dump the rolling per-frame scene-composition ring (one compact
// signature per item per frame for the last ~900 frames) to
// logs/scene_ring_<ts>.csv on the render thread's next frame. Diffing the
// artifact frame against its neighbors names exactly which item appeared /
// vanished / changed textures during a 1-2 frame flash no capture can catch.
void RequestSceneRingDump();

// Bone-signal recorder (wheel-guard diagnosis): for `seconds`, records every
// raw entity pose (ring pushes: bone palettes / rigid worlds, timestamps)
// and each frame's interpolated output, then writes
// logs/bone_signal_<ts>.bin. Skate at a steady speed while it runs;
// offline analysis replays candidate guard strategies against the real
// board data.
void RecordBoneSignal(double seconds);

}  // namespace skate3::native_scene
