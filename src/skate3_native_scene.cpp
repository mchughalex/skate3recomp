// Native scene renderer, game-thread half: captures the hooked MeshContext
// stream and guest render state, and builds/publishes the per-frame
// FrameScene. The render-thread RHI half lives in
// skate3_native_scene_gpu.cpp; state shared between the two is in
// skate3_native_scene_state.h.

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
#include <rex/kernel/guest_presence.h>
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
#include "skate3_native_scene_state.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif

REXCVAR_DECLARE(std::string, skate3_native_render_snapshot_dir);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene, true, "Skate 3",
                    "Render the game scene natively from the hooked MeshContext stream, "
                    "replacing the emulated GPU output (requires skate3_native_render). "
                    "Hot-toggles live between the native and emulated renderers (F5).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lightmaps, true, "Skate 3",
                    "Sample guest lightmap textures in the native scene renderer. The "
                    "old 'lightpages decode black' finding is stale: in gameplay the "
                    "composed atlas payloads are CPU-readable (offline-validated: "
                    "lightmap x2 at the |uv2| unwrap reproduces the emulated baked "
                    "sun/shadow/AO structure), and the texture payload revalidation "
                    "re-decodes pages that were captured before composition.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_menu_blur_sigma, 9.0, "Skate 3",
                      "Settings-menu backdrop gaussian sigma, in 1080p pixels (matches "
                      "the menu design's blur radius; scales with output "
                      "resolution). Only used while the host settings overlay is open.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(skate3_native_render_scene_debug, 0, "Skate 3",
                     "Native scene debug: 0=normal, 1=clear only, 2=solid color per item, "
                     "3=limit to 20 items, 4=depth test disabled")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_msaa, 4, "Skate 3",
                     "MSAA sample count for the native scene (1 = off, 2/4/8). Distant "
                     "thin geometry (railings, wires) shimmers without it; mipmaps only "
                     "fix texture aliasing. Applies live: the scene pipeline family and "
                     "MSAA targets rebuild on change.")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_world_v2_tan_sign, 1.0, "Skate 3",
                      "World-shading v2: polarity relating the stored frame's "
                      "cross(binormal, normal) x handedness to the game's tangent "
                      "(+1/-1). Calibrated against the emulated reference; flips the "
                      "kd sun-direction response if wrong.")
    .range(-1.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_world_v2, true, "Skate 3",
                    "World-shading v2: per-pixel normal/detail/spec maps on the exact "
                    "environment families (the game's own GetTangentLight kd + phong "
                    "terms; v1 folded them to the flat-map constants). Off = the v1 "
                    "flat response, for A/B comparison.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dynobj_v2, true, "Skate 3",
                    "Dynamicobject shading v2: per-pixel normal/detail/spec maps on the "
                    "exact dynamicobject families (mapped world normal, the game's "
                    "GetTangentLight kd and both phong spec variants; v1 folded them "
                    "to the flat-map constants). Off = the v1 flat response, for A/B "
                    "comparison.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssao, true, "Skate 3",
                    "Screen-space ambient occlusion (GTAO) in the native renderer: soft "
                    "contact shading where surfaces meet (under ledges, rails, vehicles, "
                    "the skater). A post pass over the resolved scene depth, an "
                    "enhancement beyond the original game's shading.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssao_radius, 0.8, "Skate 3",
                      "SSAO sample radius in world units. Larger radii darken bigger "
                      "cavities but read as wide soft pools around characters; small "
                      "radii keep it to contact shading.")
    .range(0.1, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssao_luma_protect, 1.2, "Skate 3",
                      "How strongly bright (sun-lit) surfaces resist SSAO darkening, "
                      "a post-tonemap approximation of ambient-only occlusion (direct "
                      "sunlight is not occluded by nearby geometry). 0 = AO applies "
                      "uniformly regardless of lighting.")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssao_intensity, 1.4, "Skate 3",
                      "SSAO strength: exponent on the linear visibility term (0 = "
                      "invisible, 1 = physical, >1 = accentuated).")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssao_full_res, false, "Skate 3",
                    "Evaluate SSAO at full output resolution instead of half. Roughly "
                    "4x the GPU cost for a subtle sharpening of the smallest contact "
                    "shadows; the depth-aware blur hides most of the difference.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssao_debug, false, "Skate 3",
                    "Replace the frame with the SSAO visibility term (tuning aid).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssr, false, "Skate 3",
                    "Screen-space reflections on reflective glass and water: "
                    "on-screen scenery reflected in real time where the original "
                    "game shows only a static environment cube. Rays that miss "
                    "(off-screen content, screen edges) fade back to the cube "
                    "term. Requires the HDR scene intermediate. EXPERIMENTAL: "
                    "off by default pending image-quality calibration (residual "
                    "smearing/noise on some surfaces).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ssr_steps, 48, "Skate 3",
                     "SSR screen-space march taps per ray (half-res pass). More "
                     "taps resolve thinner reflected geometry at proportional "
                     "GPU cost.")
    .range(8, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssr_thickness, 1.0, "Skate 3",
                      "SSR depth thickness (view units): how deep behind a "
                      "visible surface a marching ray still counts as a hit. "
                      "Too small and rays tunnel through thin ledges/rails; too "
                      "large and reflections smear under overhangs.")
    .range(0.05, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssr_intensity, 1.0, "Skate 3",
                      "SSR composite strength over the material's cube "
                      "reflection term (0 = effectively off).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ssr_debug, 0, "Skate 3",
                     "SSR debug view: 1 = raw ray color + confidence, 2 = "
                     "reflection G-buffer normals, 3 = reflectivity (red) x "
                     "depth visibility (green), 4 = ray autopsy (green = "
                     "visibility reject, blue = degenerate, cyan = off-screen, "
                     "magenta = near plane, yellow = out of steps, red = "
                     "too-thick crossings, white = hit x confidence).")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_hdr, true, "Skate 3",
                    "Render the 3D scene into a float (HDR) intermediate and apply the "
                    "game's shared tone chain once in a host post pass, the basis for "
                    "real bloom (and later HDR effects). With bloom off the output is "
                    "equivalent to the classic in-material tonemap. Off = the classic "
                    "path, for A/B parity checks.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_bloom, true, "Skate 3",
                    "Real bloom from the HDR scene: a downsample/upsample pyramid driven "
                    "by pre-tonemap brightness (night lamps, neon, the sun and sky "
                    "glow). Requires the HDR intermediate.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bloom_threshold, 0.9, "Skate 3",
                      "Bloom brightness threshold in pre-tonemap units (1.0 = the tone "
                      "curve's saturation point; 0.85 corresponds to ~97% display "
                      "white). Sunlit surfaces sit around 0.5-0.65 pre-tonemap; "
                      "thresholds that reach them veil the whole daytime frame in "
                      "glow; near-clipped lamp cores, neon, sky glare and the sun "
                      "stay above 0.85.")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bloom_knee, 0.08, "Skate 3",
                      "Bloom soft-knee width around the threshold (0 = hard cutoff).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bloom_intensity, 0.025, "Skate 3",
                      "Bloom strength: the pyramid's contribution added to the "
                      "pre-tonemap scene (0 = off; the pyramid accumulates ~5 levels, "
                      "so small values are already visible around bright lights).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_hdr_packed, true, "Skate 3",
                    "Use the packed R11G11B10 float format for the HDR scene targets "
                    "instead of RGBA16F: halves the scene-pass color bandwidth (most "
                    "of the HDR cost at high resolution + MSAA) at slightly lower "
                    "precision in very dark gradients.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_hdr_debug, 0, "Skate 3",
                     "HDR post debug: 0 = off, 1 = show the bloom term only, 2 = show "
                     "the raw pre-tonemap scene (clamped), 3 = show the fused SSAO "
                     "multiplier plane, 4 = show the sun-shaft plane only, 5 = show "
                     "the directional haze term only.")
    .range(0, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shafts, true, "Skate 3",
                    "Volumetric sun shafts: the air along each view ray is marched "
                    "against the dynamic CSM atlas and the static world-shadow map, "
                    "and the SHADOWED portion proportionally dims the light seen "
                    "through it: dark crepuscular shafts cut by real shadow "
                    "volumes (buildings, trees, underpasses) into the game's own "
                    "sun glow. Fully lit air is untouched, so open scenes stay "
                    "exactly as authored. Requires the HDR intermediate and this "
                    "frame's shadow pass.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shafts_intensity, 0.6,
                      "Skate 3",
                      "Sun-shaft strength: how strongly fully shadowed air dims "
                      "the scene behind it, scaled by the forward-scatter phase "
                      "(at the default, fully shaded air dims ~30-50% at typical "
                      "sun-facing angles and saturates looking straight at the "
                      "sun; 0 = off).")
    .range(0.0, 1.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shafts_reach, 40.0, "Skate 3",
                      "Sun-shaft march reach (world units): how far in front of "
                      "the camera the air is sampled for sun visibility. Longer "
                      "reaches pick up more distant shadow volumes at coarser "
                      "sampling for the same step count.")
    .range(5.0, 300.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shafts_steps, 64, "Skate 3",
                     "Sun-shaft march step ceiling per pixel; rays shorter than "
                     "the full reach take proportionally fewer steps at the same "
                     "per-meter density. More steps resolve thinner shadow "
                     "volumes at proportional GPU cost.")
    .range(8, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shafts_res, 4, "Skate 3",
                     "Sun-shaft march resolution divisor (2 = half res, 4 = "
                     "quarter res). The march output is low frequency (jittered "
                     "average under a tent blur) and upsamples depth-aware, so "
                     "higher divisors trade little quality for a proportional "
                     "GPU saving.")
    .range(2, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_sun_override, false, "Skate 3",
                    "Lighting lab: replace the captured sun direction with the "
                    "azimuth/elevation cvars. Every per-frame light transform is "
                    "rebuilt around the new direction, so the dynamic CSM shadows, "
                    "the static world-shadow map, material shadow receivers and "
                    "the volumetric shafts all follow the moved sun. Baked "
                    "lightmap shade and the sky dome's painted sun cannot move "
                    "(game content), a tuning tool, not a time-of-day system.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_sun_azimuth, 220.0, "Skate 3",
                      "Overridden sun azimuth in degrees (compass heading the sun "
                      "sits toward; 0 = +Z north, 90 = +X east).")
    .range(0.0, 360.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_sun_elevation, 35.0, "Skate 3",
                      "Overridden sun elevation in degrees above the horizon (low "
                      "values = long shadows and the most visible shafts).")
    .range(2.0, 88.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_haze, true, "Skate 3",
                    "Directional atmospheric haze: sun-scattered light added with "
                    "view distance, strongest looking toward the sun, tinted by the "
                    "frame's own fog color (so it tracks time of day and fades at "
                    "night). Kept subtle; the game's authored sky and fog carry "
                    "the base atmosphere, and additive terms read as extra fog "
                    "quickly. Requires the HDR intermediate.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_haze_intensity, 0.08,
                      "Skate 3",
                      "Directional haze strength (pre-tonemap scale of the "
                      "toward-the-sun scattering excess; 0 = off).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_haze_density, 0.005,
                      "Skate 3",
                      "Directional haze density per view unit: how quickly the "
                      "scattering saturates with distance (0.005 reaches ~63% at "
                      "200 units).")
    .range(0.0, 0.05)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_showcase, false, "Skate 3",
                    "Run the graphics build-up showcase: the screen snaps to "
                    "black, flat clay geometry wipes in, the scene is rebuilt "
                    "layer by layer (each layer revealed by a vertical split "
                    "wiping across the screen with both sides rendered live), "
                    "and the run closes by wiping back to black. The black "
                    "bookends double as cut markers for screen recordings. "
                    "Layer order and grouping come from "
                    "skate3_native_render_scene_showcase_order (edited in the F12 "
                    "showcase setup window). Clears itself when the sequence "
                    "finishes; set to false mid-run to cancel. Also on "
                    "Ctrl+Shift+B by default (bind_skate3_showcase). Layers whose feature "
                    "is disabled (or unavailable without the HDR intermediate) "
                    "are skipped.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    skate3_native_render_scene_showcase_order,
    skate3::native_scene::kShowcaseOrderDefault, "Skate 3",
    "Showcase layer order: comma-separated reveal steps, '+' joins layers "
    "into one wipe, a '-' prefix disables a layer while keeping its "
    "position. Tokens: albedo, lighting, materials, decals, dyn, shadows, "
    "ao, ssr, vol, bloom (the run always starts from clay geometry, and a "
    "final full-render step is appended when the list leaves layers "
    "unrevealed). decals (graffiti/paint art + macro weathering; renders on "
    "the full material look, so place it after materials) and dyn (dynamic "
    "entities: characters, props, cloth) HIDE their content until revealed; "
    "left out or disabled, that content renders from clay onward as usual. "
    "The CSM atlas is shared by both sides of the split, so a dyn step "
    "AFTER shadows shows entity shadows before their owners; order dyn "
    "first. Example: \"albedo,lighting,materials,decals,dyn,shadows+ao\".")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_showcase_hold, 2.5, "Skate 3",
                      "Showcase: seconds each build-up stage holds fullscreen "
                      "between wipes.")
    .range(0.0, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_showcase_wipe, 3.0, "Skate 3",
                      "Showcase: seconds each split wipe takes to cross the "
                      "screen.")
    .range(0.2, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_freecam, false, "Skate 3",
                    "Detach the render camera from the game (drone / free-fly "
                    "cam): WASD fly, E/Space up, Q/C down, arrow keys or "
                    "right-mouse drag look, Z/X zoom, Shift fast, Ctrl slow. "
                    "The game keeps simulating, but its main render camera is "
                    "taken over (ViewCamera::SetViewMatrix override), so the "
                    "game culls and submits the world around the flown pose "
                    "itself. Also on the End key (bind_skate3_freecam).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_freecam_speed, 8.0, "Skate 3",
                      "Freecam base fly speed in world units (meters) per "
                      "second. Shift = 4x, Ctrl = 0.2x.")
    .range(0.5, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_freecam_look_speed, 90.0,
                      "Skate 3",
                      "Freecam arrow-key look rate in degrees per second.")
    .range(10.0, 360.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_freecam_capture_input, true,
                    "Skate 3",
                    "While the freecam is engaged, keep keyboard/controller "
                    "input away from the game so flying doesn't also steer "
                    "the skater. Turn off to fly and play at the same time.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_2d, true, "Skate 3",
                    "Replay the game's 2D/APT (Flash HUD) draws as a native overlay pass "
                    "on top of the 3D scene")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_splines, true, "Skate 3",
                    "Replay the game's in-world neon spline draws (waypoint arrows, "
                    "marker beams; spline_darken/spline_default shaders) inside the "
                    "native scene pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_selection_outline, true, "Skate 3",
                    "Replay the park-editor / object-mover selected-object outline: the "
                    "game stencil-marks the selected object after the sky and a postfx "
                    "edge-detect adds the blue contour (postfx_edgedetectstencil port)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_quadlists, false, "Skate 3",
                    "Render captured non-indexed quad-list draws. Off by default: every "
                    "quad-list capture seen so far is a PARTICLE system (disjoint 2-4cm "
                    "sprites), which renders as floating white squares without the game's "
                    "sprite textures and blending.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_mesh_decode_budget, 0, "Skate 3",
                     "Max INLINE mesh decodes per rendered frame (0 = unlimited). Only "
                     "dynamic payloads (skinned/cloth/ropa, whose buffers change every "
                     "frame) decode inline on the render thread; static world meshes "
                     "and all textures decode asynchronously on the prewarm workers.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_warmup_budget_ms, 8, "Skate 3",
                     "Per-frame milliseconds of the post-takeover settle decode pass: "
                     "after a loading screen the native output takes over on the first "
                     "substantial post-load scene (the registration prewarm decoded the "
                     "world behind the load), and for a short window this budget mops "
                     "up whatever prewarm missed while the draw path's miss budgets "
                     "are clamped; leftovers render white/skip for a frame instead of "
                     "freezing. 0 disables the takeover gates + settle pass entirely "
                     "(legacy immediate behavior, stale-scene flash and all).")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_settle_max_frames, 900, "Skate 3",
                     "Hard cap on how far past takeover the settle pass may keep "
                     "extending itself (frames). Content that keeps failing "
                     "revalidation (streamed payloads, old-map items over reused "
                     "arenas after a map change) otherwise re-defers work every frame "
                     "and holds the settle pass - and its full per-frame budget - open "
                     "for the rest of the session (the sticky ~80fps state). Past the "
                     "cap the draw path's own miss budgets take over. 0 = uncapped")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_warmup_min_items, 32, "Skate 3",
                     "Scene item count below which warmup keeps yielding to the "
                     "emulated output: right after the gameplay flip the capture holds "
                     "only a handful of items while the game is still fading in, and "
                     "taking over then shows a black/empty world. Every real gameplay "
                     "scene measures 100+ items.")
    .range(0, 10000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_prewarm_budget_ms, 32, "Skate 3",
                     "Per-frame milliseconds spent decoding freshly REGISTERED world "
                     "meshes (AddRenderInstance hook) and their textures while the "
                     "loading screen is up; the heavy lifting happens behind the load, "
                     "so gameplay starts with hot caches and takeover is immediate. "
                     "The loading screen renders emulated at hundreds of fps, so even "
                     "32 ms/frame keeps it ~30 fps; map-change loads register most of "
                     "the world in their FINAL seconds, so the drain rate in that "
                     "window decides how much pop-in survives into gameplay. "
                     "0 disables registration prewarm.")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_photo_yield, true, "Skate 3",
                    "Yield to the emulated output while a photo-mission's photo editor "
                    "(the FE PhotoSelect screen) is up. The editor's depth of field / "
                    "saturation / brightness / contrast / lens vignette are the game's "
                    "own postfx chain, which native rendering suppresses; natively "
                    "the photo showed the raw scene and the effect controls did "
                    "nothing. The scene is frozen there, so emulated-path performance "
                    "is fine. Detected by polling the FrontEndManager NIS push-state "
                    "stack (plus the PhotoReplayController heartbeat as a backup).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_readback, true, "Skate 3",
    "While a photo flow is active (the photo-mission editor, or a few "
    "seconds after any TakePhoto), arm the SDK's forced small-resolve CPU "
    "readback (native_render_force_resolve_readback_max_length) and lift "
    "emulated-draw suppression. The game takes photos by CPU-reading a "
    "resolved 1152x640 PostFX screenshot target from guest memory "
    "(ScreenshotBackEnd::GrabScreenshot -> JPEG); photo missions keep the "
    "gameplay presence context so no readback path ever ran and the grabbed "
    "memory stayed all-zero; the final photo display and the saved photo "
    "were BLACK. Costs one synchronous readback per small resolve, photo-flow "
    "frames only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_grab_native, true, "Skate 3",
    "Fulfill the photo grab NATIVELY while a photo flow is active: keep "
    "emulated-draw suppression ON (the 40-50 fps editor cost was the "
    "unsuppressed emulated pipeline rendering the whole scene + postfx at "
    "scaled res every armed frame, proven by the kFast run: zero readback "
    "drains, same fps) and instead downsample the native output to the "
    "game's 1152x640 screenshot target each armed frame, read it back "
    "(double-buffered, no GPU drain) and CPU-tile it big-endian into guest "
    "memory at 0x04911000 where ScreenshotBackEnd::GrabScreenshot JPEG-"
    "encodes it. In the photo editor the native output IS the ported postfx "
    "chain result; in the plain TakePhoto window it is the plain frame, "
    "both the correct grab semantics. OFF = the emulated fallback: forced "
    "kFull resolve readbacks + suppression lifted for the whole window "
    "(skate3_native_render_scene_photo_readback). If native frames stop "
    "mid-window (mode toggle, unexpected yield), the window watchdog flips "
    "to the emulated fallback by itself.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_display_yield, false, "Skate 3",
    "Yield the photo DISPLAY-CARD screen to the emulated output while the "
    "framed card is up. OFF (default) since the shutter burst: the game "
    "CPU-composes the framed card into the card texture at the grab (the "
    "burst makes its inputs real), and the native 2D replay samples that "
    "same texture; the display screen renders fully native. Turn ON as a "
    "safety hatch if the display screen misrenders natively.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_native, true, "Skate 3",
    "Render the photo-mission photo editor NATIVELY, applying the game's "
    "own postfx chain (depth of field / saturation / brightness / contrast "
    "/ lens vignette) as exact ucode ports (photo_fx.hlsl: visualfx -> DOF "
    "downsample -> tap9dofMotionBlur -> tap9dof -> uber -> fisheye) driven "
    "by the LIVE constants the game stages for its own (suppressed) postfx "
    "draws each frame. Takes precedence over "
    "skate3_native_render_scene_photo_yield; if the pass captures are not "
    "yet fresh (first frames of the editor), the scene renders without the "
    "effects until they land. Known deltas: the motion-accumulation feed "
    "(see photo_native_accum), the bloom pyramid contribution (disabled in "
    "every editor capture), grade LUT served as identity, deck AO.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_photo_native_accum, 0, "Skate 3",
    "Source for the visualfx pass's quarter-res motion-accumulation input "
    "(the game feeds a jitter-accumulated buffer natively unmodeled): 0 = "
    "black, 1 = downsampled scene (pre-grade), 2 = downsampled final frame. "
    "Compare against the emulated editor (F11 pair) and keep the match.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_photo_native_debug, 0, "Skate 3",
    "Photo-editor native postfx DEBUG VIEW drawn instead of the chain's "
    "final output: 1 = the visualfx CoC map (DoF blur amount; black = in "
    "focus, grey/white = blurred), 2 = packed-depth reconstruction (red = "
    "near band saturate((1-d)*100), green/blue = d fraction ramps; a flat "
    "screen here means the native depth is not reaching the chain). 0 = "
    "off.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_pause_native, true, "Skate 3",
                    "Keep the NATIVE renderer active through the in-game pause menu "
                    "instead of yielding to the emulated output. Pause is told apart "
                    "from loading screens / the boot frontend by the world still "
                    "submitting perspective scenes while the presence context reads 0 "
                    "(loads and the frontend stop publishing within ~300 ms, which "
                    "falls back to the yield path and its cache clears). Native pause "
                    "also skips the pause-entry cache clears, so unpausing costs "
                    "nothing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_loading_native, true, "Skate 3",
                    "Render post-startup loading screens natively, black backdrop "
                    "plus the game's own captured 2D loading UI, instead of "
                    "yielding to the emulated output. The loading-screen "
                    "housekeeping (cache clears, registration prewarm, takeover "
                    "arming) is unchanged; only the presented pixels switch source. "
                    "The boot flow before the first gameplay stays emulated.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_boot_native, true, "Skate 3",
                    "Extend native rendering to the game startup flow (intro videos, "
                    "boot frontend, the first load); i.e. drop the first-gameplay "
                    "prerequisite from the native menu/loading modes, and render the "
                    "pre-takeover boot frames as native 2D-over-black instead of "
                    "yielding. With this and the pause/loading modes on, the emulated "
                    "GPU output is never presented.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_fmv_native, true, "Skate 3",
                    "Render FMVs natively: the movie player CPU-decodes VP6 into "
                    "three YUV plane textures (VideoRenderer_RwTexture members, "
                    "published per frame by the Render hook), and the captured "
                    "movie quad (the AptMovieIntegration stride-24 draw; through "
                    "the plain 2D shader it rendered as an opaque black cover, its "
                    "c8 is black) is substituted with the ps_yuv2d combine inside "
                    "the 2D replay, order-faithful and at the quad's own geometry "
                    "(windowed movies place exactly). When off (or the planes fail "
                    "to publish), FMVs fall back to the emulated yield "
                    "(skate3_native_render_scene_fmv_yield).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_fmv_yield, true, "Skate 3",
                    "Yield to the emulated output while an FMV is playing (intro "
                    "logos, any rw::movie playback). The video frame is CPU-decoded "
                    "into a texture and reaches the screen through the game's postfx "
                    "chain + swap without any capturable 2D draw (F11-proven on the "
                    "boot intro: 15 draws, all postfx passes + fade fills, none "
                    "sampling the video), so the native path has nothing to replay; "
                    "the emulated frame is complete and correct there, same class as "
                    "the photo editor. Detected via the MovieDecoder::Decode "
                    "heartbeat; native rendering resumes within 0.5 s of the last "
                    "decoded frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_cas_yield, false, "Skate 3",
                    "Yield to the emulated output while the create-a-skater editor "
                    "(the CAS 'Edit Skater' screen) is up. OFF by default since the "
                    "editor renders natively: the char-lighting capture understands "
                    "the editor CAC compiles' shifted constant layout (see "
                    "CaptureCharLighting's editor fam-2 retry), the texture-space "
                    "composite passes (cac*_unwrapPS, pitch <= 512) execute under "
                    "the pitch-selective suppression, and the ropa/palette VS layout "
                    "is unchanged. Known native deltas: the deck's shift-recolor "
                    "masks and the editor's own DOF chain are not modeled. Turn ON "
                    "to get the exact emulated editor instead (photo-editor class; "
                    "detected via FE push-state id 15 + the _nis shader heartbeat).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_menu_rtt_passes, true, "Skate 3",
    "While a menu/pause/loading context (presence context 0) renders "
    "natively, drop native_render_suppress_mode to 0 (suppress "
    "framebuffer-sized passes ONLY) so the game's sub-framebuffer "
    "render-to-texture passes execute; the team-menu/Import-Skater skater "
    "portrait boxes are one-shot RTT passes at a surface pitch inside the "
    "mode-2 suppressed band (> 512, != 1024): under mode 2 they never ran "
    "and the boxes stayed empty. The visible frame stays fully native "
    "(framebuffer passes remain suppressed); this is the same "
    "execute-the-composition-passes-emulated class as lightmap pages and "
    "CAS outfit composition. Restored to the configured mode on the first "
    "gameplay frame. Menu-only cost: the midsize postfx-chain passes also "
    "execute there (mode 0 was the long-lived default before mode 2).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_menu_rtt_scope, 1, "Skate 3",
    "When menu_rtt_passes is on, WHEN to hold suppress_mode at 0 during "
    "menus. 0 = the whole menu context (pre-round behavior): the game's "
    "postfx chain then executes emulated at 1152x640 x resolution scale "
    "EVERY menu frame: under the pause menu that is the full world postfx, "
    "on FMV/title screens the movie postfx, pacing the pipeline and "
    "fighting the native renderer for the GPU (the menu sluggishness at a "
    "high fps counter). 1 = only inside a PORTRAIT WINDOW: the FE "
    "push-state stack contains a screen not known steady-safe (known: 0 = "
    "FE root, 56 = pause root, 24 = FMV, 17 = pause challenge map), OR an "
    "unsafe screen was entered/left within the last 3 s (one-shot portrait "
    "RTT renders fire at those transitions), OR the CAS heartbeat is "
    "fresh, OR the stack is unreadable; everything unknown fails OPEN to "
    "mode 0. Outside the window menus run the "
    "gameplay-proven mode-2 suppression (lightmap pages + <= 512 "
    "composites still execute).")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_menu_unsuppress, false, "Skate 3",
    "ESCAPE HATCH, normally unnecessary: while a menu/pause/loading context "
    "(presence context 0) renders natively, temporarily clear "
    "native_render_suppress_emulated_draws so ALL emulated passes execute. "
    "The original motivation, the team-menu skater portrait boxes are "
    "one-shot render-to-texture passes that suppression left forever empty "
    ", is covered without this by the SDK's pitch-selective suppression "
    "(native_render_suppress_mode 2: surfaces <= 512 px wide, incl. the "
    "portrait cards, always execute). Turn on only if small offscreen "
    "composites are missing in menus despite that; costs the full emulated "
    "pipeline's GPU time during menus.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_snapshot_all_draws, false, "Skate 3",
                    "Record the draw stream on every recorded frame instead of 2 of every "
                    "60 (large .draws.bin; for targeted investigations)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadows, true, "Skate 3",
                    "Render the game's dynamic CSM shadows natively (skater, NPCs, "
                    "movable props onto the world). "
                    "Cascade matrices are captured per frame from the game's own "
                    "material constants.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_caster_refresh_all, true, "Skate 3",
                    "Perspective-bank palette refresh covers ALL caster-sourced "
                    "skinned captures: skinned-published ROPA garments join the "
                    "refresh pool (their exclusion left the player's tee/hair riding "
                    "the ortho caster banks, whose fine animation runs ticks stale in "
                    "bursts, the high-fps hair-off-the-head wedge / vanishing "
                    "garment), and the refresh target is picked by palette identity "
                    "(<1.5 m) across every caster-sourced clone instead of "
                    "oldest-only (mesh-sharing NPC clones probed the wrong item, the "
                    "guard refused, and the right clone stayed on its stale caster "
                    "bank, the NPC blink). false = legacy oldest-only, non-ropa "
                    "refresh.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_char_rows_inst, true, "Skate 3",
                    "Per-(mesh,instance) fallback for the character-lighting capture: "
                    "items whose capture failed to validate this frame reuse their own "
                    "last validated rows. Covers instanced pieces and the player, which "
                    "the single-instance mesh fallback and the LW entity cache both "
                    "miss; without it hair flips between the blended sub-pass and the "
                    "legacy opaque path on capture-failed frames (a frame-to-frame "
                    "flicker whose rate scales with fps).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shadow_tile, 0, "Skate 3",
                     "Shadow cascade tile resolution. The game's own atlas is "
                     "three 512 tiles; the blur steps raster texels (the game's "
                     "tfetch offsets, which the emulated GPU applies in scaled "
                     "texels), so higher values sharpen both the silhouette and "
                     "the penumbra like the emulated baseline. 512 = the softer "
                     "original-console look, blocky up close. 0 = auto: 512 x "
                     "the render resolution scale (the Resolution Scale "
                     "setting), matching the emulated renderer's shadow "
                     "crispness at any render resolution. Applies live: the "
                     "atlas chain recreates on change.")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadow_caster_parity, true,
                    "Skate 3",
                    "Character pieces cast dynamic shadows only when the game "
                    "submitted them to its own shadow passes this frame (per-piece "
                    "caster list; e.g. the CAS trucker hat never casts; natively "
                    "casting it painted a hard brim band across the editor face). "
                    "OFF = every published character piece casts (old behavior).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadow_static_casters, true,
                    "Skate 3",
                    "Native static sun-shadow map: render the STATIC world "
                    "(buildings, trees, rails, placed props) into a separate "
                    "camera-centered ortho depth map along the material sun, "
                    "sampled by every lit surface in addition to the game's "
                    "dynamic cascades. Live static shade on characters and "
                    "props (which baked lightmaps cannot shade) and shadows "
                    "that follow a moved sun; receivers keep the darker of "
                    "baked and live shade rather than doubling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_strength, 1.0,
                      "Skate 3",
                      "How dark static-geometry shadows get (1 = the full "
                      "dynamic-shadow clamp, 0 = invisible). Applied at "
                      "receive from the static sun map, so characters and "
                      "props keep full-strength shadows. Moderate values "
                      "keep live static shade believable where it disagrees "
                      "with the baked lighting's own sun.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_radius, 180.0,
                      "Skate 3",
                      "Half-extent in meters of the static sun-shadow map's "
                      "far cascade (camera-centered; the mid and inner "
                      "cascades cover 1/2 and 1/6 of this at 2x and 6x "
                      "the texel density). Larger reaches farther at lower "
                      "texel density; the term fades out at the outer edge.")
    .range(40.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shadow_static_size, 4096,
                     "Skate 3",
                     "Static sun-shadow map resolution per cascade tile "
                     "(the map is three tiles). At the default radius the "
                     "inner cascade gets ~1.5 cm texels, mid ~4 cm, far "
                     "~9 cm. Applies live: the map recreates on change.")
    .range(1024, 8192)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadow_pcss, true, "Skate 3",
                    "Contact-hardening soft shadows (PCSS): a blocker search "
                    "estimates the caster distance per pixel and the filter "
                    "width follows the sun's angular size: crisp at the "
                    "contact point, progressively softer with caster height. "
                    "OFF = the game's fixed-width blurred atlas sampling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_sun_deg, 2.5,
                      "Skate 3",
                      "Sun angular diameter in degrees for the PCSS penumbra "
                      "(the real sun is ~0.53; larger reads softer).")
    .range(0.1, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_max_m, 0.8,
                      "Skate 3",
                      "Maximum PCSS penumbra half-width in meters (caps how "
                      "soft very tall casters can get).")
    .range(0.05, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_blocker_m, 2.5,
                      "Skate 3",
                      "PCSS blocker-search radius in meters. Must be at "
                      "least the max penumbra for distant casters to soften "
                      "fully; larger values let unrelated nearby casters "
                      "soften edges they should not.")
    .range(0.1, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_min_texel, 1.0,
                      "Skate 3",
                      "Minimum PCSS filter radius in physical atlas texels "
                      "(keeps fully hardened edges filtered).")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_bias, 0.08,
                      "Skate 3",
                      "Static sun-shadow receiver bias in METERS; static "
                      "geometry is its own caster, so its surfaces compare "
                      "against their own map depth. Slope-scaled: grazing "
                      "sun angles get up to several times this. Raise on "
                      "acne (stipple on sunlit ground/walls), lower if "
                      "static shadows visibly detach from their casters.")
    .range(0.0, 0.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_bias_vk, 0.0,
                      "Skate 3",
                      "Additional static sun-shadow receiver bias in METERS "
                      "applied on the Vulkan backend only (a tuning margin "
                      "for backend shader-toolchain rounding differences; "
                      "raise only if acne appears on Vulkan specifically).")
    .range(0.0, 0.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_char_shadow_exact, true,
                    "Skate 3",
                    "Characters sample the CSM atlas with the game's own math: "
                    "per-cascade receive biases captured from the character PS "
                    "banks and NINE point taps at +-1 game-texel averaged /9 "
                    "(cacstamp_skin_nisPS ucode port). OFF = the legacy single "
                    "bilinear tap + coverage term.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Reflective-glass (env families 5/6) isolation controls for the F12
// dialog: live A/B of each stage of the cube-reflection term against the
// emulated look (F5).
REXCVAR_DEFINE_INT32(skate3_native_render_scene_refl_mode, 0, "Skate 3",
                     "Reflective glass debug: 0 normal, 1 cube term off, 2 cube at "
                     "the absolute LOD in refl_lod, 3 flat normal (no normal-map "
                     "perturb), 4 visualize the cube sample only, 5 body only (no "
                     "spec, no cube), 6 normal-map LOD bias from the slider.")
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_lod, 0.0, "Skate 3",
                      "Reflective glass debug: mode 2 = absolute cube mip level; "
                      "other modes = EXTRA LOD bias on top of the automatic "
                      "640p-parity bias.")
    .range(-4.0, 12.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Constant tangent-space normal tilt on the reflective glass, live-tunable
// (F12). The defaults are DERIVED, not tuned: the material's 16x16 detail
// texture is a constant BC1 block whose endpoints expand on HARDWARE by bit
// replication: 5-bit red 16 -> (16<<3)|(16>>2) = 132/255, 6-bit green 32
// -> (32<<2)|(32>>4) = 130/255; so the shader's 2*d - 1 fold is exactly
// (0.035294, 0.019608). An earlier fold used our CPU decoder's
// integer-division expansion (131/129 -> 0.028/0.012), leaving a ~0.8 deg
// constant normal tilt = the residual reflection-position offset that was
// dialed out by hand to (0.036, 0.019), matching the hardware value to
// the slider step and confirming the derivation.
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_bias_x, 0.035294, "Skate 3",
                      "Reflective glass: constant tangent-X (horizontal) normal "
                      "tilt added to the normal-map sample (= the detail "
                      "constant's hardware-BC1 fold).")
    .range(-0.2, 0.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_bias_y, 0.019608, "Skate 3",
                      "Reflective glass: constant binormal-Y (vertical) normal "
                      "tilt added to the normal-map sample (= the detail "
                      "constant's hardware-BC1 fold).")
    .range(-0.2, 0.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_refl_bias_auto, true, "Skate 3",
                    "Derive the reflective-glass normal tilt from each material's "
                    "own detail texture (hardware-exact BC1 decode of its constant "
                    "color) instead of the refl_bias_x/y values. The sliders "
                    "remain the fallback/override with auto off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Fine-grained feature gates for the F12 native-render debug dialog: each
// isolates one subsystem so regressions (flicker, wrong shading) can be
// bisected live without rebuilds. All hot-reload, default on.
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_macro, true, "Skate 3",
                    "Apply the macrooverlay grime/crack multiply on world materials")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_decals, true, "Skate 3",
                    "Composite environment.decal art (graffiti/paint) over base diffuse")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_transparents, true, "Skate 3",
                    "Draw environment.transparent items (mist/glass/fences) in the "
                    "alpha-blended sub-pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_world_items, true, "Skate 3",
                    "Publish world sort-list items (static geometry)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_entity_fade, true, "Skate 3",
                    "Honor the game's per-entity spawn/distance fade: LivingWorld "
                    "pres entities (NPCs, traffic vehicles) publish an opacity that "
                    "every character-family PS writes as output alpha (peds c21.x, "
                    "defaultcharacter c13.x, cacstamp c22.x, vehicle body c20.x). "
                    "The game submits their draws at alpha 0 through the whole "
                    "spawn settle (the physics drop) and ramps alpha up afterwards "
                    "/ by distance. Off = the old behavior: entities render fully "
                    "opaque from their first draw (mid-air spawn pop, early "
                    "distant pop-in).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dynamic_items, true, "Skate 3",
                    "Publish dynamic entities (characters, props, cloth)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_fade, true, "Skate 3",
    "Serve LivingWorld NPC/vehicle fade alpha from the entity itself "
    "(entity+528 via the LW entity store, mapped per MeshContext) instead "
    "of the per-draw captured constant row. The captured row is a per-draw "
    "inference: capture races serve alpha=1 (opaque mid-air spawns, no "
    "fade-in) or a clone's foreign row (one-frame invisibility blinks). "
    "Off = the pre-store captured-row behavior.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_gap_fill, true, "Skate 3",
    "Republish a LivingWorld NPC for up to 2 frames when its MeshContext "
    "drops out of the submit records while the entity is still alive (the "
    "1-3 frame publish GAPs: an in-view NPC vanishing for a frame reads as "
    "a blink/teleport). Off = gaps render as-is.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_identity, true, "Skate 3",
    "Key LivingWorld entities' pose-smoothing rings by their MeshContext "
    "(the game's own per-instance identity) instead of (mesh, occurrence) "
    "pairing. Same-mesh clone reshuffles in the sort lists can then never "
    "mispair a ring (the NPC/prop teleport-slide class). Off = the "
    "positional re-pair heuristics alone.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_inline, false, "Skate 3",
                    "Decode CPU-cloth (ROPA) garment VBs inline on the render thread "
                    "instead of the worker jobs. Measured 4.3ms avg per garment "
                    "decode (committed-resource allocation dominates) AND it does not "
                    "address the jelly; the mismatch is the cloth shape having no "
                    "place on the interpolation play clock, not decode latency. Kept "
                    "for experiments only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_blend, true, "Skate 3",
                    "Lerp CPU-cloth shape generations onto the motion-smoothing play "
                    "clock at draw time (pose<->shape pairing via the interp ring); "
                    "the stepped shape against the interpolated body was the tee "
                    "jelly/clip-through, worse at LOWER fps. OFF = newest decode "
                    "(old behavior).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_2d_async_px, 1, "Skate 3",
    "Pixel-count threshold above which 2D/HUD texture decodes are ELIGIBLE "
    "for the words-miss decode workers. Eligible decodes (first sightings "
    "and content changes alike) still run INLINE while the per-frame inline "
    "budget (4 ms; fullscreen-class art gets double) lasts; the run-copy "
    "untiler made even 1280x720 sub-millisecond, and the old always-async "
    "policy made every APT re-raster (NEW address = new key = first "
    "sighting) skip its quad for the 1-3 worker frames: menu elements and "
    "fullscreen backdrops visibly BLINKED through animations. The workers are "
    "the burst-overflow valve: past the budget, first sightings skip 1-3 "
    "frames and content changes serve the stale decode until the commit. "
    "0 = everything inline unconditionally; larger values exempt small art "
    "from async eligibility entirely.")
    .range(0, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_2d_sharp, 0.0, "Skate 3",
                      "Sharp-magnification amount for the 2D/HUD overlay (0 = plain "
                      "bilinear, up to 2). Much of the APT (Flash) HUD samples "
                      "cached-bitmap tiles whose texel count equals their 720p display "
                      "size (score digits, gauge ring, compass; measured density 1.0 "
                      "in the 2D draw stream) while text batches sample 512x512 glyph "
                      "atlases at 8-10 texels/pixel; at 2-3x output scales the tiles "
                      "blur under bilinear while atlas text stays crisp; the same on "
                      "console/emulated, the content is simply 720p. Catmull-Rom + "
                      "clamped unsharp mask where the fetch is magnified (>1.25x), "
                      "ramped to full by 2x; minified/1:1 fetches untouched. DEFAULT "
                      "0 (off): sharpened ramps read worse than the soft "
                      "bilinear; the honest fix is higher-res source pixels (APT "
                      "cache-tile rasterization scale), not edge-contrast synthesis.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_boxcar, true, "Skate 3",
                    "Blend CPU-cloth shape generations through the SAME 8-tap boxcar "
                    "kernel the body bones/garment world are filtered with (see "
                    "smooth_camera_filter_ms) instead of a plain 2-generation lerp. "
                    "The plain lerp reconstructs the 60Hz limb signal SHARPLY while "
                    "the boxcar rounds the body ~a window; the cloth led the body "
                    "through every direction change by an excursion that scales with "
                    "the guest period (the residual tee jelly after the blend fix). "
                    "OFF = plain bracketing lerp (for A/B).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ropa_bias, 0, "Skate 3",
                     "Shift the ROPA pose<->shape pairing by N ring poses (+ = fresher "
                     "shape, - = older), a live trim for any residual constant drape "
                     "lag/lead while skating. 0 = the paired generation.")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ropa_delay, 0, "Skate 3",
                     "Delay CPU-cloth VB snapshots by N guest frames before the worker "
                     "decode, phase-aligning the cloth SHAPE with the motion-smoothing "
                     "play clock the body renders on (~2 periods behind now). Without "
                     "it the drape is ~2 frames AHEAD of the rendered body; it hangs "
                     "where the body WILL be and leads it through direction changes "
                     "(the tee jelly / clip-through-torso). DEFAULT 0: in practice "
                     "the garment LAGGED, and delay made it worse; the "
                     "phase model was backwards. Kept for experiments.")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_scene_trace_mesh, "", "Skate 3",
                      "Hex guest mesh address to trace end-to-end through the "
                      "texture pipeline ('tex-trace:' log lines): per-frame "
                      "served objects + content fingerprints (on change), "
                      "every slot resolve decision (direct/sticky/hold/near-"
                      "black/white), every payload/words poll verdict, and "
                      "every worker commit touching the mesh's objects. "
                      "Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_scene_trace_2d, "", "Skate 3",
                      "Hex fetch word 1 (base|flags) of a 2D overlay texture "
                      "to trace through the 2D resolver ('2d-trace:' log "
                      "lines): per-resolve entry state, liveness probe "
                      "results and the serve/heal route taken, plus the "
                      "worker commits for its words key. Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_detail_hold, 240, "Skate 3",
                     "Frames an item slot keeps serving its previous HIGHER-"
                     "resolution texture after the game's material-detail "
                     "system rebinds a strictly smaller one (streaming "
                     "pressure flaps a nearby mesh's DT material to its UN "
                     "variant for ~0.5 s and back, the visible 'different "
                     "texture set' flash; the detailed decode is still "
                     "cached host-side, so the flap can be invisible). A "
                     "downgrade that persists past the hold is adopted (a "
                     "real demote as you leave the area). 0 = serve the "
                     "guest binding verbatim (the console's own detail pop).")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_tex_store_mb, 1280, "Skate 3",
                     "Texture content-store GPU byte budget in MB. The store "
                     "is count-capped, but per-entry sizes differ per map and "
                     "the idle guards keep a superseded map's working set "
                     "resident for minutes after a switch; over this budget "
                     "the LRU drains oldest-first with a shortened idle "
                     "guard so VRAM does not accumulate across map changes.")
    .range(256, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_mesh_store_mb, 1024, "Skate 3",
                     "Mesh cache GPU byte budget in MB (vertex + index "
                     "buffers of cached decodes). Same byte-pressure LRU "
                     "behavior as the texture-store budget.")
    .range(256, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_retain_offscreen, true, "Skate 3",
                    "Keep recently seen static items in the scene while the game "
                    "view-culls them: the re-timed (smoothed) render camera trails "
                    "the guest pose by up to the filter window, so items leaving "
                    "the guest frustum were visibly torn down right at the screen "
                    "edges during pans/traversal. Items the guest frustum can see "
                    "but stopped submitting (LOD switch, despawn) drop immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_revalidate, true, "Skate 3",
                    "Re-fingerprint cached texture payloads every 16 frames and "
                    "re-decode on change (heals late-composed lightmap pages)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_stretch_guard, true, "Skate 3",
                    "Draw-time stretch veto: skin cached sample verts of each "
                    "skinned mesh's GPU-resident decode with the final palette "
                    "every frame; wider than bind size = the 1-frame mangled-"
                    "ribbon flash; skip the item's draws (blink)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_stretch_guard_dump, false, "Skate 3",
                    "On a stretch veto, dump the full bone palette and probe "
                    "verts to logs/stretch_*.txt (first 6 trips) for offline "
                    "diagnosis of which rows are junk")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lm_dump, false, "Skate 3",
                    "Diagnostic: dump the decoded mip 0 of every generated-mip "
                    "(no-chain composed page) texture to native_texture_dumps/ "
                    "as raw RGBA for offline diffing against the gsnap decode")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_backface_cull, true, "Skate 3",
                    "Backface-cull world env materials like the game does: the "
                    "material XMLs set CULLMODE=FRONT on every environment "
                    "family (banners calibrated game-kept faces = our D3D12 "
                    "BACK faces -> CULL_FRONT). CULL_NONE stacked hidden faces "
                    "into the frame: double glass panes + interior wall faces "
                    "behind the translucent canopy glass = the too-bright "
                    "slope / too-dark awning deltas. Trees/alphatest (fams "
                    "7/9/10) and mirrored instances stay uncull(ed).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_mesh_revalidate, true, "Skate 3",
                    "Re-decode cached meshes when their payload fingerprint changes "
                    "(streaming arena reuse; also picks up CPU-animated buffers)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_char_track, false, "Skate 3",
    "Per-frame diagnostic log line tracking the main character's body and "
    "ropa garment through the motion-smoothing pipeline: pre/post-interp "
    "reference positions, ring state, ingest/reset/claim events, resolved "
    "mode, shape-kernel state, and the garment<->body offset. Heavy "
    "(one line per rendered frame), for artifact diagnosis runs.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_smooth_camera, true, "Skate 3",
                    "Re-time the camera on the host clock: the guest publishes new "
                    "camera poses on its own sim tick (~170-240 Hz, irregular: "
                    "measured streaks of 10 rendered frames on one pose at 400 fps), "
                    "which reads as the world juddering/skipping while panning. "
                    "Interpolates between the last two distinct guest poses, one "
                    "sim-interval behind (a few ms of added camera latency, no "
                    "overshoot). Teleports/cuts snap.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    skate3_native_render_scene_smooth_camera_filter_ms, 50.0, "Skate 3",
    "Boxcar filter window (ms) applied to the smoothed camera pose, centered "
    "on the playback point. The game's camera pose VALUES advance in 60 "
    "Hz-quantized lumps at high render rates (measured: +-2.2 deg off a "
    "constant-rate stick pan, velocity CV 0.84); 50 ms = three 60 Hz "
    "periods nulls the quantization at any render rate (measured 185 -> 7 "
    "deg/s rms frame-to-frame velocity jitter) for ~25 ms extra camera "
    "latency. 0 = off (raw sample interpolation).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Synthetic camera pan: a judder-isolation probe, not a feature. Replaces
// the camera with a host-generated constant-rate horizontal pan so each
// pipeline stage can be judged against a mathematically smooth ground truth
// (the guest's own camera path is irregular; smooth output can never be
// eyeballed against it). Modes pick the injection stage:
//   1 = time-based: pose is a pure function of the host clock at scene-build
//       time, bypassing guest pose sampling AND the smoother entirely. Still
//       judders => the fault is downstream (frame pacing / present / display).
//   2 = fixed-step: pose advances a constant angle per PUBLISHED FRAME
//       (ignores time). Smooth only if displayed frames appear at even
//       intervals, the complement of mode 1 (irregular pacing that mode 1's
//       time base compensates for shows up here, and vice versa).
//   3 = through-smoother: the ~1 kHz sampler thread synthesizes guest-like
//       pose samples (~200 Hz, deliberately irregular) and the normal
//       SmoothCamera reconstruction runs on them; reconstruction error vs
//       the known ideal is measured numerically (err_deg in the log line).
//       Judders here but not in mode 1 => the smoother is at fault.
REXCVAR_DEFINE_INT32(skate3_native_render_scene_synthetic_pan, 0, "Skate 3",
                     "Synthetic constant-rate camera pan (judder isolation): 0 = off, "
                     "1 = time-based at scene build, 2 = fixed angle step per frame, "
                     "3 = synthetic samples through the camera smoother.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_synthetic_pan_rate, 90.0, "Skate 3",
                      "Synthetic pan rate in degrees/second")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_synthetic_pan_amp, 0.0, "Skate 3",
                      "Synthetic pan amplitude in degrees: 0 = continuous full 360 "
                      "rotation (sweep the REAL camera around once with the stick "
                      "after engaging; the probe accumulates a union of every static "
                      "item the game submits, filling in the full surround); > 0 = "
                      "triangle-wave +-amp around the engage heading.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bonesig_auto, 0.0, "Skate 3",
                      "Auto-arm bone-signal recordings of this many seconds "
                      "(entity-pose diagnosis, same output as the F12 button): "
                      "first window ~30 s after the native scene comes up, "
                      "re-armed every 90 s, 3 windows max. 0 = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_sort_opaque, true, "Skate 3",
                    "Draw opaque scene items front-to-back (bbox-center camera "
                    "distance). Early-z rejects occluded pixels before the heavy "
                    "material shading runs; the game's own sort order is by "
                    "render state, not depth.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_mips, true, "Skate 3",
                    "Upload guest texture MIP CHAINS (off = mip 0 only; distant "
                    "surfaces alias but mip-related artifacts disappear). Flush the "
                    "texture cache after toggling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Hook layer master switch, defined in skate3_native_render.cpp. The runtime
// toggle refuses to flip the scene on without it: the hooks that feed the
// scene are only installed when it was set at boot.
REXCVAR_DECLARE(bool, skate3_native_render);
// Defined in native/skate3_native_entity.cpp (the identity store module).
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_ropa_world_primary);
// Defined in native/skate3_native_lw.cpp (the LW entity store module).
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_palette);
// SDK-level emulated-draw suppression (rexglue native_guest_renderer.cpp):
// command processors skip emulated draw/resolve execution while the native
// output is active. Temporarily overridden to false during menu contexts by
// YieldForMenus (see skate3_native_render_scene_menu_unsuppress).
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);
// SDK-level suppression PASS FILTER (d3d12 command_processor.cpp): 0 =
// suppress framebuffer-sized passes only (pitch >= 1280), 2 = suppress all
// except lightmap pages (1024) and small composites (<= 512). YieldForMenus
// drops it to 0 during menu contexts so the skater-portrait RTT passes
// (pitch > 512, in the mode-2 suppressed band) execute; see
// skate3_native_render_scene_menu_rtt_passes.
REXCVAR_DECLARE(int32_t, native_render_suppress_mode);
// SDK-level forced resolve readback window (rexglue command_processor.cpp /
// both backends' IssueCopy): when > 0, resolves up to that byte length are
// synchronously read back to CPU-visible guest memory regardless of
// readback_resolve and gameplay state. Armed by UpdatePhotoGrabWindow while
// a photo flow is active; the game CPU-reads the resolved screenshot
// target to build the photo JPEG.
REXCVAR_DECLARE(int32_t, native_render_force_resolve_readback_max_length);
// SDK-level readback-downscale sample point (resolve_downscale shader): when
// set, the scaled->1x extraction samples the CENTER host pixel of each
// scale_x*scale_y block instead of the top-left one, compensating the
// D3D9-style half-pixel offset that becomes a (scale/2)-pixel content shift
// at scaled resolutions. Armed with the photo window so grabbed photos
// sample host pixel centers.
REXCVAR_DECLARE(bool, readback_resolve_half_pixel_offset);
// SDK-level async pipeline compilation (rexglue command_processor.cpp): the
// d3d12 backend SKIPS draws whose pipeline is still compiling. Forced
// synchronous during menu contexts by YieldForMenus so one-shot portrait
// renders can't lose still-compiling pieces (first-run armless skaters).
REXCVAR_DECLARE(bool, async_shader_compilation);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_perf_log, false, "Skate 3",
                    "Log periodic native-renderer performance breakdown lines "
                    "(see skate3_native_render_scene_perf_interval)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(skate3_native_render_scene_perf_interval, 600, "Skate 3",
                     "Frames between native-scene perf/stats log lines. Lower "
                     "values give finer windows for chasing transient frame-"
                     "rate dips at the cost of log volume.")
    .range(60, 6000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_occlusion_cull, true, "Skate 3",
                    "Skip drawing static world items whose bounds are "
                    "provably hidden behind already-rendered geometry (depth-"
                    "grid test, conservative: anything unprovable still "
                    "draws). Shadow passes are exempt, so hidden geometry "
                    "keeps casting shadows. Dense areas with extended world "
                    "streaming spend roughly half their per-item CPU on such "
                    "items.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_occlusion_cull_build, true,
                    "Skate 3",
                    "Also skip the per-frame scene-item rebuild for statics "
                    "the occlusion cull proved hidden, on 3 of every 4 "
                    "frames (staggered per instance). The every-4th-frame "
                    "rebuild re-enters the item in the rendered scene so "
                    "disocclusion is re-tested and the persistent shadow "
                    "caster cache stays refreshed. Requires "
                    "skate3_native_render_scene_occlusion_cull.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_occlusion_cull_guest, true,
                    "Skate 3",
                    "Also skip the GUEST engine's draw-list dispatch for "
                    "items the occlusion cull proved hidden (the dispatch "
                    "builds command packets the native renderer suppresses "
                    "anyway, and it is the frame-time-critical cost). "
                    "Capture still sees every item, so shadows and scene "
                    "state are unaffected. Requires "
                    "skate3_native_render_scene_occlusion_cull.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_perf_items, false, "Skate 3",
                    "Deep per-item CPU profiling: adds a perf-items log line "
                    "per perf window attributing per-item cost to pipeline "
                    "stages (mesh/texture/constants/submit; cache validate/"
                    "fingerprint/walk/fetch) and splitting draw time between "
                    "items inside vs outside the rendered view frustum. Adds "
                    "measurable overhead at high item counts; leave off "
                    "outside investigations.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ring, false, "Skate 3",
                    "Record a rolling per-frame scene-composition ring (~900 "
                    "frames); F7 dumps it to logs/scene_ring_<ts>.csv for "
                    "diffing 1-2 frame artifacts no capture can catch")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dyn_gap_fill, true, "Skate 3",
                    "Re-publish character/cloth/skinned items for up to two "
                    "missed publish frames (high-frame-rate body flicker)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
namespace skate3::native_scene {
namespace {

// Verified guest structure offsets.
constexpr uint32_t kCtxEffectList = 0x04;
constexpr uint32_t kEffectListPassCount = 0x44;
constexpr uint32_t kCtxDrawCountU16 = 0x38;
constexpr uint32_t kCtxDrawList = 0x48;
constexpr uint32_t kMeshMaterial = 0x24;
constexpr uint32_t kMeshVertexDescriptor = 0x28;
constexpr uint32_t kMeshIndexBuffer = 0x30;
constexpr uint32_t kMeshVertexBuffer = 0x34;
constexpr uint32_t kBufferPhysAddr = 0x18;
constexpr uint32_t kVbBytes = 0x20;
constexpr uint32_t kIbCount = 0x20;
constexpr uint32_t kViewCameraFromView = 0x08;
constexpr uint32_t kViewCamViewProj = 0xA0;

// Cross-thread renderer state (published scene, prewarm queues, capture
// rows, telemetry, perf windows) lives in skate3_native_scene_state.h.

float LoadGuestF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  return std::bit_cast<float>(bits);
}

uint32_t BSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Draws completed since startup (any guest draw function). The RenderMesh
// hook samples it around the original call: an unchanged sequence means the
// mesh's draws were deferred by the dispatcher and the constant bank still
// belongs to some earlier mesh; its transform must come from the post-draw
// fixup, never from the bank at submit-exit (a leftover identity matrix at
// c4 validates as a plausible world and renders the prop at the origin).
std::atomic<uint64_t> g_draw_seq{0};

// Provenance of the last completed guest draw: (ib_obj << 32 | vb_obj) for
// indexed 3D draws, 0 for everything else. The submit-exit capture may only
// consume the constant bank when the mesh's OWN draw was the last one to
// flush it; `drew_inside` alone proved only that SOME draw ran during the
// submit call. When this mesh's draws were deferred but another entity's
// inline draws ran inside the call, the bank holds that entity's palette,
// and the sample-projection acceptance gate cannot reliably refuse it: a
// vehicle right next to the skater, skinned by the skater's foreign
// palette, still projects on-screen at a plausible spread, and rendered
// glued to his walking bones (the player-becomes-the-vehicle bug).
std::atomic<uint64_t> g_last_draw_ibvb{0};

// Where does this bank keep its bone palette? Pre-pass layout: c4 (bone 0's
// affine rows right after viewproj). Main-pass layout: camera position at
// c4, two parameter rows, palette at c7. A camera-position row is easily
// told from a bone rotation row by its norm (hundreds vs ~1). The ropa-cloth
// skinned VS variant (character.cloth_ropa, player tees) keeps one extra
// parameter row (0,0,0,1) in front of the palette; its zero xyz norm fails
// the bone check outright (unlike the NPC cloth/morph variant's (1,0,0,0),
// which passes at c4 and is corrected by RefinePaletteBase), so the palette
// really starts at c5 (pre-pass) / c8 (main-pass); reading c7 there lands
// mid-palette and scrambles every bone by two rows (the mangled player-shirt
// bug). Returns the base register, or 0 when no location holds plausible
// bone rows.
uint32_t BankPaletteBase(uint8_t* base, uint32_t bank) {
  const auto bone_at = [&](uint32_t reg) -> bool {
    for (int r = 0; r < 3; ++r) {
      float f[4];
      for (int i = 0; i < 4; ++i) {
        f[i] = LoadGuestF32(base, bank + ((reg + r) * 4 + i) * 4);
        if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
      }
      const float n = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
      if (!(n > 0.0025f && n < 400.0f)) return false;
      if (!(f[3] > -20000.f && f[3] < 20000.f)) return false;
    }
    return true;
  };
  // A parameter/sentinel row like (0,0,0,1): xyz norm ~0, never a bone row.
  const auto param_row_at = [&](uint32_t reg) -> bool {
    float f[3];
    for (int i = 0; i < 3; ++i) {
      f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
      if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
    }
    return f[0] * f[0] + f[1] * f[1] + f[2] * f[2] <= 0.0025f;
  };
  // The ropa sim-INACTIVE flag row reads (1, ~0, JUNK, ~0); x is written
  // bit-exact 1.0 by the game but z holds uninitialized memory (the CAS
  // editor banks carry ~-2e23 there). The junk z fails every range check
  // above, leaving the bank UNCLASSIFIABLE: the editor tee's main-pass
  // capture then never succeeded, and the shadow (pre-pass) bank instead
  // matched bone_at(7) MID-PALETTE (real palette at c5) = a one-bone-late
  // palette that passes the 6-sample gate but fails the 32-sample publish
  // veto every frame, the invisible edit-skater garment (captured:
  // published bone k == bank bone k+1, bones 14/15
  // junk, spread 112 vs bind 0.7). An out-of-range z under exact
  // (1, ~0, *, ~0) is PROOF the row is a flag, never a bone row.
  const auto ropa_flag_row_at = [&](uint32_t reg) -> bool {
    float f[4];
    for (int i = 0; i < 4; ++i) {
      f[i] = LoadGuestF32(base, bank + (reg * 4 + uint32_t(i)) * 4);
    }
    if (!(std::fabs(f[0] - 1.0f) <= 1e-3f && std::fabs(f[1]) <= 1e-3f &&
          std::fabs(f[3]) <= 1e-3f)) {
      return false;
    }
    return !(f[2] > -1e7f && f[2] < 1e7f);
  };
  // The sim-ACTIVE variant of the ropa flag row reads (~0, JUNK, ~0, JUNK):
  // x is written 0.0 (sim owns the garment) with uninitialized memory in the
  // unwritten lanes (observed ~7e20 on the trick-guide demo skater's tee,
  // c7/c4). The junk lanes fail every range check
  // above, leaving the bank UNCLASSIFIABLE in the true main-pass state
  // (return 0; the capture never resolved and the garment stayed pending),
  // while the post-shadow state matched bone_at(7) mid-palette instead and
  // handed the rigid-matrix read the shadow block's (0,0,0,1) tail row at
  // c191. An out-of-range lane under x ~ 0 and z ~ 0 is the same structural
  // proof as the junk-z inactive variant: a real bone row is never junk.
  const auto ropa_active_flag_row_at = [&](uint32_t reg) -> bool {
    float f[4];
    for (int i = 0; i < 4; ++i) {
      f[i] = LoadGuestF32(base, bank + (reg * 4 + uint32_t(i)) * 4);
    }
    if (!(std::fabs(f[0]) <= 1e-3f && std::fabs(f[2]) <= 1e-3f)) {
      return false;
    }
    return !(f[1] > -1e7f && f[1] < 1e7f) || !(f[3] > -1e7f && f[3] < 1e7f);
  };
  // DETERMINISTIC main-pass detection first: that layout keeps the CAMERA
  // POSITION at c4 (palette at c7, or c8 behind a parameter row). Matching
  // c4 against the frame camera pins the layout without any scoring; the
  // norm checks alone accepted banks whose c4 was the camera followed by
  // leftover bone rows (the trucker cap's single main-pass capture), and
  // score-based c7-vs-c8 arbitration flipped to the row-shifted palette
  // whenever camera rotation clipped a sample of the real one (the cap
  // teleporting to axis-permuted coordinates while rotating).
  {
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    if (dx * dx + dy * dy + dz * dz < 25.0f &&
        (g_fog_cam[0] != 0.0f || g_fog_cam[1] != 0.0f || g_fog_cam[2] != 0.0f)) {
      if ((param_row_at(7) || ropa_flag_row_at(7) ||
           ropa_active_flag_row_at(7)) &&
          bone_at(8)) {
        return 8;
      }
      if (bone_at(7)) return 7;
      return 0;
    }
  }
  if (bone_at(4)) return 4;
  // The ropa junk-lane flag checks must run BEFORE bone_at(7): on a pre-pass
  // ropa bank (flag c4, palette c5) rows c7.. are mid-palette bone rows and
  // bone_at(7) matches them, the one-bone-late class.
  if ((param_row_at(4) || ropa_flag_row_at(4) || ropa_active_flag_row_at(4)) &&
      bone_at(5)) {
    return 5;
  }
  if (bone_at(7)) return 7;
  if ((param_row_at(7) || ropa_flag_row_at(7) || ropa_active_flag_row_at(7)) &&
      bone_at(8)) {
    return 8;
  }
  return 0;
}

// The cloth/morph skinned VS variant (skating-NPC torsos) keeps ONE extra
// parameter row between the viewproj and the bone palette: observed live,
// c4 = (1,0,0,0) with the palette at c5 (pre-pass) / c8 (main-pass). A
// palette read one register early hands every bone [neighbor row, row0,
// row1]; each row is still a perfectly plausible bone row, so the norm
// checks in BankPaletteBase cannot catch it, and the mesh skins to
// component-rotated coordinates ~300 m off in the sky. That was the
// invisible-NPC-torso bug: the torso was captured, non-pending, palette and
// texture resolved, and rendered far outside the view.
//
// Discriminate by skinning a few sample vertices with EVERY candidate base
// (pre-pass c4/c5, main-pass c7/c8, plus the caller's guess) and projecting
// them with the pass's own viewproj (bank c0..c3, column-vector rows): the
// game drew this mesh with these constants, so the correct base puts the
// samples inside the clip volume (validated offline across every skinned
// draw of an F10 capture: correct base 1.00, wrong base <= ~0.3), AND keeps
// the skinned samples' spatial spread near the mesh's bind-pose size. The
// spread test is what rejects a FOREIGN palette that lands coincidentally
// in view: the player's trucker cap draws exactly ONCE per frame (main
// pass, palette at c7) while leftover rows at c4 pass the norm checks,
// junk-skinning it into a ~3 m smear near the world origin (bind size
// ~0.4 m) that was sometimes on screen, i.e. the sometimes-visible
// disappearing-hat bug. Returns the winning base, or 0 when no candidate
// both projects in-clip and keeps a sane spread; the caller then refuses
// the capture and the post-draw fixup retries on a later draw.
// structural_guess: the caller PROVED the guess base's layout structurally
// (the ropa flag row read exactly (1,...) from the mesh's own bank; every
// capture site guarantees own-draw provenance now, so the foreign-bank
// hypothesis the strict gate defends against is off the table). When the
// strict half-in-clip score still refuses every home, accept the guess if
// all samples skin IN FRONT of the projection inside a loose 6x guard band
// at a sane spread: up close the garment fills/overflows the screen and
// most sample verts clip the tight 1.5x band; the strict gate refused the
// CORRECT palette every frame and the torso stayed invisible until the NPC
// walked far enough away. A row-shifted palette still fails this (skins
// hundreds of meters off-view or behind the camera).
uint32_t RefinePaletteBase(uint8_t* base, uint32_t bank, uint32_t palette_base,
                           const DrawItem& item, bool structural_guess = false) {
  if (item.bw_offset == 0 || item.bi_offset == 0 || item.stride == 0) {
    return palette_base;
  }
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2) {
    return palette_base;
  }
  float vp[16];
  for (int i = 0; i < 16; ++i) {
    vp[i] = LoadGuestF32(base, bank + i * 4);
    if (!(vp[i] > -1e9f && vp[i] < 1e9f)) return palette_base;
  }
  constexpr uint32_t kSamples = 6;
  // Sample verts decoded ONCE (native/skate3_native_guest_read.h); only the
  // candidate palette base varies between score() calls.
  SkinSampleVert sverts[kSamples];
  if (!ReadSkinSamplesGuest(base, item, kSamples, sverts)) {
    // Unsupported position format: every candidate is unscorable (-1); keep
    // the caller's guess, as the old per-candidate decode did.
    return palette_base;
  }
  // score: fraction (0..16) of samples that skin in front of and inside the
  // bank's own clip volume; *out_spread = the skinned samples' bbox diagonal
  // (world units) for the bind-size sanity test below. *out_front_all (when
  // asked): every sample is in FRONT of the projection within a loose 6x
  // guard band, the relaxed near-camera criterion (see structural_guess).
  const auto score = [&](uint32_t pb, float* out_spread,
                         bool* out_front_all = nullptr) -> int {
    int ok = 0;
    int loose = 0;
    int n = 0;
    bool rows_sane = true;
    float qmin[3] = {1e9f, 1e9f, 1e9f};
    float qmax[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t s = 0; s < kSamples; ++s) {
      float q[3];
      if (SkinPointBankRows(base, bank, pb, sverts[s], q, &rows_sane) == 0) {
        continue;
      }
      float clip[4];
      for (int r = 0; r < 4; ++r) {
        clip[r] = vp[r * 4] * q[0] + vp[r * 4 + 1] * q[1] + vp[r * 4 + 2] * q[2] +
                  vp[r * 4 + 3];
      }
      const float aw = std::abs(clip[3]) < 1.0f ? 1.0f : std::abs(clip[3]);
      ++n;
      for (int a = 0; a < 3; ++a) {
        qmin[a] = std::min(qmin[a], q[a]);
        qmax[a] = std::max(qmax[a], q[a]);
      }
      // In FRONT of the projection (w > 0) and inside a generous guard band.
      // Without the w check a foreign palette that skins the mesh BEHIND the
      // camera can still land |x|,|y| within the band and score well.
      if (clip[3] > 0.0f && std::abs(clip[0]) <= 1.5f * aw &&
          std::abs(clip[1]) <= 1.5f * aw) {
        ++ok;
      }
      if (clip[3] > 0.0f && std::abs(clip[0]) <= 6.0f * aw &&
          std::abs(clip[1]) <= 6.0f * aw) {
        ++loose;
      }
    }
    if (n == 0) {
      return -1;
    }
    if (!rows_sane) {
      return -2;  // provably not a palette (vs -1 = nothing to judge)
    }
    if (out_front_all) {
      *out_front_all = n >= 2 && loose == n;
    }
    const float dx = qmax[0] - qmin[0];
    const float dy = qmax[1] - qmin[1];
    const float dz = qmax[2] - qmin[2];
    *out_spread = std::sqrt(dx * dx + dy * dy + dz * dz);
    return (ok * 16) / n;
  };
  // Bind-pose size of the SAMPLED span (approximates the mesh diagonal):
  // legit skinning keeps the world spread near it; a foreign palette
  // composes inconsistent transforms and smears the samples several times
  // wider. Generous bound: articulation can stretch a garment's sampled
  // span, junk palettes overshoot by ~10x.
  const float bind_diag = BindDiag(item);
  const float max_spread = std::max(3.0f * bind_diag, bind_diag + 1.0f);
  // Bounded from BELOW too: a palette of non-pose rows can skin every
  // sample to nearly one point that happens to project on-screen (the
  // vehicle-flick garbage). Samples span the whole VB, so legit skinning
  // keeps a large fraction of the bind size; small items (hats: samples
  // can cluster on one bone) skip the floor.
  const float min_spread = bind_diag > 1.0f ? 0.2f * bind_diag : 0.0f;
  // Acceptance gate: skins into the bank's own view AND at a sane size.
  // IMPORTANT selection constraint (offline-validated on the cap capture):
  // a palette shifted by whole rows is a RIGID transform of the mesh, same
  // spread, often still on screen, so "best spread/score wins" mis-picks
  // permuted bases. Selection therefore stays conservative: keep the
  // caller's guess (old base-vs-base+1 arbitration) whenever it passes the
  // gate, and only on gate FAILURE fall through to the other layout homes
  // (pre-pass c4/c5, main-pass c7/c8) in canonical order. The trucker cap
  // is the motivating case: it draws ONCE per frame (main-pass layout,
  // palette at c7) while leftovers at c4 pass the row-plausibility checks;
  // the old code never looked past c4/c5 and skinned it into a ~3 m smear
  // near the origin (the disappearing-hat bug).
  const auto gate = [&](uint32_t pb, int* ok_out) -> bool {
    float spread = 0.0f;
    const int ok = score(pb, &spread);
    if (ok_out) {
      *ok_out = ok;
    }
    return ok >= 8 && spread <= max_spread && spread >= min_spread;
  };
  int s_std = -1;
  int s_plus = -1;
  const bool std_pass = gate(palette_base, &s_std);
  if (s_std == -1) {
    // Unsupported position format / no weighted samples: nothing to judge;
    // keep the caller's guess rather than refusing every capture. (-2 =
    // provably-insane rows falls through: try the other homes, else refuse.)
    return palette_base;
  }
  // The guess wins whenever it passes; +1 (the cloth/morph parameter-row
  // variant) is consulted only on FAILURE. A row-shifted palette is a rigid
  // axis-permutation of the mesh, often still on screen with a sane spread
  // - so "switch when +1 scores strictly better" flip-flopped whenever
  // camera rotation clipped one sample of the real base (cap teleporting
  // while rotating). The genuine +1 layouts skin the guess-base hundreds of
  // meters off-view, which the gate rejects decisively.
  if (std_pass) {
    return palette_base;
  }
  // Fallback homes need their layout's STRUCTURAL signature, not just a
  // passing projection score. The projection gate alone is fooled by
  // ONE-BONE-LATE palettes: a vehicle filling the screen at close range
  // clips half its samples (the correct base FAILS the gate), while a
  // base+3-register home shifts the vehicle BODY; most of its verts hang
  // off the LAST real bone, onto the next leftover rows in the bank,
  // i.e. whatever skinned entity staged before. Standing next to a truck
  // that is the PLAYER: the shifted palette projects beautifully and was
  // accepted, gluing the truck body to the walking skater (the
  // player-becomes-the-vehicle bug; proven in capture:
  // real palette c4..c18, published capture bone k = real bone k+1, body
  // bone 4 = the player's stale c19 rows). Signatures, from the verified
  // layouts:
  //   +1 homes (cloth/morph/ropa): a PARAMETER row directly in front of
  //     the palette: (0,0,0,w) (ropa flag) or (1,0,0,0) (NPC morph).
  //   main-pass homes 7/8: the CAMERA POSITION at c4 (the same key
  //     BankPaletteBase pins the main-pass layout with).
  const auto param_like = [&](uint32_t reg) -> bool {
    float f[4];
    bool in_range = true;
    for (int i = 0; i < 4; ++i) {
      f[i] = LoadGuestF32(base, bank + (reg * 4 + uint32_t(i)) * 4);
      if (!(f[i] > -1e7f && f[i] < 1e7f)) {
        in_range = false;
      }
    }
    if (!in_range) {
      // (1, ~0, JUNK, ~0): the ropa sim-inactive flag row with
      // uninitialized z (CAS editor banks); the out-of-range component
      // PROVES it is not a bone row (see BankPaletteBase's twin check).
      return std::fabs(f[0] - 1.0f) <= 1e-3f && std::fabs(f[1]) <= 1e-3f &&
             std::fabs(f[3]) <= 1e-3f && !(f[2] > -1e7f && f[2] < 1e7f);
    }
    if (f[0] * f[0] + f[1] * f[1] + f[2] * f[2] <= 0.0025f) {
      return true;  // (0,0,0,w): the ropa flag row
    }
    // (1,0,0,0): the NPC cloth/morph variant's parameter row. A bone row0
    // can also be (1,0,0,tx) for an unrotated bone; its w is the world
    // translation x, so require |w| small too.
    return std::fabs(f[0] - 1.0f) <= 1e-3f && std::fabs(f[1]) <= 1e-3f &&
           std::fabs(f[2]) <= 1e-3f && std::fabs(f[3]) <= 1.5f;
  };
  const auto cam_at_c4 = [&]() -> bool {
    if (g_fog_cam[0] == 0.0f && g_fog_cam[1] == 0.0f && g_fog_cam[2] == 0.0f) {
      return false;
    }
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    return dx * dx + dy * dy + dz * dz < 25.0f;
  };
  const auto home_ok = [&](uint32_t pb) -> bool {
    switch (pb) {
      case 4:
        // Never fall back to the pre-pass home on a structurally MAIN-pass
        // bank (camera at c4): palette@4 is then the ONE-BONE-SHIFTED
        // palette: bone 0 = the camera + parameter rows, every other bone
        // = its neighbor's affine. A close vehicle whose real palette@7
        // fails the strict projection gate (screen-filling: samples clip
        // out of the band) fell through here, and the shifted palette
        // skins adjacent bones plausibly enough to PASS, the
        // camera-position-as-bone-0 vehicle mangle (proven in capture:
        // main bank c4 = frame camera, c6.w = -0.5 =
        // the published bone0_t, real palette at c7).
        return !cam_at_c4();
      case 5:
        return param_like(4);
      case 7:
        return cam_at_c4();
      case 8:
        return cam_at_c4() && param_like(7);
      default:
        return false;
    }
  };
  if (home_ok(palette_base + 1) && gate(palette_base + 1, &s_plus)) {
    g_palette_base_plus1.fetch_add(1, std::memory_order_relaxed);
    return palette_base + 1;
  }
  for (const uint32_t pb : {4u, 7u, 5u, 8u}) {
    if (pb == palette_base || pb == palette_base + 1) {
      continue;
    }
    if (home_ok(pb) && gate(pb, nullptr)) {
      return pb;
    }
  }
  // Camera-pinned MAIN-pass home: a close vehicle fills the screen and
  // clips most samples out of the strict band, so gate(7/8) fails even on
  // the REAL palette; with the pre-pass fallback (correctly) blocked
  // above, the capture then refused every frame and the vehicle
  // flickered/vanished during close passes. The camera key at c4 proves
  // the layout structurally; accept on the loose front-of-projection
  // criterion (same relaxation as the ropa structural path below).
  if (cam_at_c4()) {
    const uint32_t pb_main = param_like(7) ? 8u : 7u;
    float spread = 0.0f;
    bool front_all = false;
    const int sc = score(pb_main, &spread, &front_all);
    if (sc >= 0 && front_all && spread <= max_spread && spread >= min_spread) {
      static std::atomic<uint64_t> s_main_relaxed{0};
      const uint64_t n = s_main_relaxed.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 1023u) == 0) {
        REXLOG_INFO(
            "native-scene: main-pass palette relaxed-accept mesh={:08X} "
            "base={} score={} spread={:.2f} bind={:.2f} (n={})",
            item.mesh, pb_main, sc, spread, bind_diag, n);
      }
      return pb_main;
    }
  }
  // Structurally-proven guess (ropa flag row, own-draw bank): near-camera
  // relaxed acceptance; see the function comment. Tried LAST so a strictly
  // passing home always wins first.
  if (structural_guess) {
    float spread = 0.0f;
    bool front_all = false;
    if (score(palette_base, &spread, &front_all) >= 0 && front_all &&
        spread <= max_spread) {
      g_ropa_relaxed.fetch_add(1, std::memory_order_relaxed);
      return palette_base;
    }
  }
  // No candidate skins this mesh into the bank's own view at a sane size:
  // the bank belongs to another mesh. The caller refuses the capture (item
  // stays pending) and the post-draw fixup re-captures from a real draw.
  return 0;
}

// Rigid transform validation: the game uses (at least) two VS constant
// layouts, verified from recorded draw streams: the pre-pass layout has a
// row-vector 4x4 world at c4..c7 (rotation rows end in 0, translation in
// c7); the main-pass layout has the camera position at c4 and the world
// 4x4 at c8..c11. Older meshes use a column-vector affine 4x3 at c4..c6.
// Sanity-check rows so a camera-position or parameter block is never
// mistaken for a matrix.
bool TryRow4x4(uint8_t* base, uint32_t bank, uint32_t reg, float* out) {
  float f[16];
  for (int i = 0; i < 16; ++i) {
    f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
    if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
  }
  if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f || f[15] != 1.0f) return false;
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) return false;
  }
  if (!(f[12] > -20000.f && f[12] < 20000.f && f[13] > -20000.f && f[13] < 20000.f &&
        f[14] > -20000.f && f[14] < 20000.f)) {
    return false;
  }
  std::memcpy(out, f, 16 * sizeof(float));
  return true;
}

bool TryColAffine(uint8_t* base, uint32_t bank, uint32_t reg, float* out) {
  float f[12];
  for (int i = 0; i < 12; ++i) {
    f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
    if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
  }
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) return false;
    if (!(f[r * 4 + 3] > -20000.f && f[r * 4 + 3] < 20000.f)) return false;
  }
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      out[i * 4 + j] = f[j * 4 + i];
    }
    out[i * 4 + 3] = 0.0f;
  }
  out[12] = f[3];
  out[13] = f[7];
  out[14] = f[11];
  out[15] = 1.0f;
  return true;
}

// Fill a rigid item's world matrix from a constant bank, whichever layout
// the staging draw used.
bool BankRigidWorld(uint8_t* base, uint32_t bank, float* out) {
  return TryRow4x4(base, bank, 4, out) || TryRow4x4(base, bank, 8, out) ||
         TryColAffine(base, bank, 4, out);
}

// Orthographic viewproj at c0..c3 (bottom row exactly (0,0,0,1)): the CSM
// caster-cascade banks, verified in capture (a
// truck's three caster draws vs its perspective main-pass draw). See
// DrawItem::caster_bank.
bool BankIsOrtho(uint8_t* base, uint32_t bank) {
  const float x = LoadGuestF32(base, bank + (3 * 4 + 0) * 4);
  const float y = LoadGuestF32(base, bank + (3 * 4 + 1) * 4);
  const float z = LoadGuestF32(base, bank + (3 * 4 + 2) * 4);
  const float w = LoadGuestF32(base, bank + (3 * 4 + 3) * 4);
  return std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f && std::fabs(z) < 1e-6f &&
         std::fabs(w - 1.0f) < 1e-3f;
}

// AUX perspective viewproj (bank c0..c3, column-vector rows): a perspective
// pass whose projection is NOT screen-shaped; the skater-portrait
// render-to-texture passes (team menu boxes / Import Skater card) render a
// tall narrow card (aspect ~0.4-0.5) while every screen view (main, editor,
// water reflection) is >= 4:3 (16:9 default, wider ultrawide). For a
// combined viewproj the projection scales survive as row norms: the view's
// rotation rows are unit, so ||c0.xyz|| = m00 and ||c1.xyz|| = m11, and
// aspect(w/h) = m11/m00 (verified in capture:
// 1.5/0.843 = 1.78 exactly). Captures from aux passes must never enter the
// palette/world stores: the portrait stage sits ~85 m from the player and a
// portrait-pass capture merged onto the player's meshes produced the mixed
// palettes the stretch veto hid (bones 0-1 at the
// player, rows 2+ at the stage). Ortho banks (CSM caster cascades, square
// aspect) are handled by BankIsOrtho and are NOT aux.
bool BankIsAuxPerspective(uint8_t* base, uint32_t bank) {
  if (BankIsOrtho(base, bank)) {
    return false;
  }
  float n0 = 0.0f;
  float n1 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float a = LoadGuestF32(base, bank + (0 * 4 + i) * 4);
    const float b = LoadGuestF32(base, bank + (1 * 4 + i) * 4);
    if (!(a > -1e6f && a < 1e6f && b > -1e6f && b < 1e6f)) {
      return false;  // implausible bank: let the existing gates decide
    }
    n0 += a * a;
    n1 += b * b;
  }
  if (!(n0 > 1e-12f && n1 > 1e-12f)) {
    return false;
  }
  // aspect < 1.2 (n1/n0 < 1.44): narrower than any screen view.
  return n1 < n0 * 1.44f;
}

bool GuestReadableApprox(uint8_t* base, uint32_t addr) {
  // The hook layer only walks pointers the game is actively rendering from;
  // they are mapped. Reject null/small.
  (void)base;
  return addr >= 0x10000;
}

// Guarded bulk copy: skate3::native_scene::GuestTryCopy, moved to
// native/skate3_native_guest_read.cpp so every guest reader shares the one
// correctly-built SEH guard. The compiler-trap documentation
// (volatile fn-ptr + noinline: plain __try{memcpy}__except compiles to an
// UNPROTECTED `jmp memcpy`) moved with it.

}  // namespace
// SEH-guarded single-value guest loads for registry-time walks (the
// loading-screen prewarm): unlike the capture-path walks, whose pointers
// the game is actively rendering from, registry probes dereference
// candidate words that may not be pointers at all, and load-time payloads
// that may not be committed yet. GuestReadableApprox is only a null/small
// filter; these actually survive the fault.
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint32_t raw;
  if (!GuestTryCopy(&raw, REX_RAW_ADDR(addr), 4)) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool GuestTryLoadU64(uint8_t* base, uint32_t addr, uint64_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint64_t raw;
  if (!GuestTryCopy(&raw, REX_RAW_ADDR(addr), 8)) {
    return false;
  }
  *out = __builtin_bswap64(raw);
  return true;
}

// Per-instance world straight from the guest MeshContext: the owning model
// object at ctx+0x30 keeps the instance's row-major 4x4 (translation in row
// 3) at +0xA0. Verified 426/426 against placed captures in a park-editor
// venue, 180-degree-rotated clones included. This is the only per-INSTANCE
// transform source available at capture time: constant banks are shared by
// every clone of a mesh (clones share (ib,vb)), so a bank world can belong
// to whichever clone drew last. Validation mirrors TryRow4x4; callers fall
// back to the bank paths when the owner layout does not match.
bool ReadCtxInstanceWorld(uint8_t* base, uint32_t ctx, float* out) {
  uint32_t owner = 0;
  if (ctx == 0 || !GuestTryLoadU32(base, ctx + 0x30, &owner) ||
      owner < 0x10000) {
    return false;
  }
  uint32_t raw[16];
  if (!GuestTryCopy(raw, REX_RAW_ADDR(owner + 0xA0), sizeof(raw))) {
    return false;
  }
  float f[16];
  for (int i = 0; i < 16; ++i) {
    f[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
    if (!(f[i] > -1e7f && f[i] < 1e7f)) {
      return false;
    }
  }
  if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f || f[15] != 1.0f) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) {
      return false;
    }
  }
  if (!(f[12] > -20000.f && f[12] < 20000.f && f[13] > -20000.f &&
        f[13] < 20000.f && f[14] > -20000.f && f[14] < 20000.f)) {
    return false;
  }
  std::memcpy(out, f, sizeof(f));
  return true;
}

namespace {

// Guarded bounded C-string read (`out` gets up to cap-1 chars + NUL, tail
// zeroed like the old byte-at-a-time loops). Fast path is one bulk guarded
// copy; a short string right before an unmapped page falls back to
// byte-wise guarded reads so it still resolves.
void GuestTryReadString(uint8_t* base, uint32_t addr, char* out, uint32_t cap) {
  std::memset(out, 0, cap);
  if (addr < 0x10000) {
    return;
  }
  if (GuestTryCopy(out, REX_RAW_ADDR(addr), cap - 1)) {
    const size_t n = strnlen(out, cap - 1);
    std::memset(out + n, 0, cap - n);
    return;
  }
  for (uint32_t k = 0; k + 1 < cap; ++k) {
    uint8_t c;
    if (!GuestTryCopy(&c, REX_RAW_ADDR(addr + k), 1) || c == 0) {
      break;
    }
    out[k] = char(c);
  }
}

// Committed-page check for bulk reads (transient ring memory can be
// partially uncommitted, and resources may be released between the game
// thread capturing an address and the render thread reading it; a blind
// memcpy would fault).
bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t size) {
#if defined(_WIN32)
  uint8_t* p = base + addr;
  uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }
    p = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  (void)base;
  (void)addr;
  (void)size;
  return false;
#endif
}

// Payload fingerprint (FNV-1a over bytes sampled across the whole VB/IB) so
// the renderer re-decodes when streaming replaces or fills in the data at
// this address, including middle-of-buffer fills. Guarded reads: capture-
// path payloads are always resident (the game is drawing from them), but
// the registration prewarm fingerprints meshes whose payload pages may not
// be committed yet; the failure defers the mesh instead of faulting the
// thread. Returns false when the payload is unreadable.
bool ComputeItemFingerprint(uint8_t* base, DrawItem& item) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h = (h ^ v) * 1099511628211ull;
  };
  mix(item.vb_addr);
  mix(item.ib_addr);
  mix(item.vb_bytes);
  mix(item.ib_count);
  if (item.vb_bytes >= 8 && item.ib_count >= 4) {
    for (uint32_t k = 0; k < 16; ++k) {
      uint64_t vq = 0, iq = 0;
      const uint32_t vb_off = uint32_t(uint64_t(item.vb_bytes - 8) * k / 15u) & ~7u;
      const uint32_t ib_off = uint32_t(uint64_t(item.ib_count * 2 - 8) * k / 15u) & ~7u;
      if (!GuestTryLoadU64(base, item.vb_addr + vb_off, &vq) ||
          !GuestTryLoadU64(base, item.ib_addr + ib_off, &iq)) {
        return false;
      }
      mix(vq);
      mix(iq);
    }
  }
  item.fingerprint = h;
  return true;
}

// Walk one MeshContext into a DrawItem (geometry, material and draw list;
// world left as identity). Returns false if any pointer in the chain is
// implausible. For dynamic entities this MUST run at RenderMesh hook time;
// the whole chain lives in transient per-frame arenas.
}  // namespace
// Parse a guest mesh struct (vertex descriptor, buffers, bbox, material
// channels, payload fingerprint) into the item. The draw list and world
// transform are the caller's responsibility (world starts as identity).
bool BuildItemFromMesh(uint8_t* base, uint32_t mesh, DrawItem& item) {
  const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
  const uint32_t ib = REX_LOAD_U32(mesh + kMeshIndexBuffer);
  const uint32_t vb = REX_LOAD_U32(mesh + kMeshVertexBuffer);
  if (!GuestReadableApprox(base, vdesc) || !GuestReadableApprox(base, ib) ||
      !GuestReadableApprox(base, vb)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Vertex descriptor: find the stream-0 position element and the first
  // stream-0 texcoord (D3DDECLUSAGE 5) for the diffuse map. Read via ONE
  // guarded bulk copy into scratch: descriptors are runtime renderengine
  // objects, and the registration prewarm can reach a mesh before its
  // descriptor is initialized (a raw read there faults the thread; the
  // capture path also gets marginally faster than the per-field volatile
  // loads).
  uint32_t desc_head[3];
  if (!GuestTryCopy(desc_head, REX_RAW_ADDR(vdesc), sizeof(desc_head))) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t num_elements = BSwap32(desc_head[2]) >> 16;  // u16 at +8
  if (num_elements == 0 || num_elements > 32) return false;
  // Element table at +0x10 (16 bytes per element) followed by the stride
  // byte at +(num_elements + 1) * 16.
  uint8_t desc_tab[32 * 16 + 1];
  const uint32_t desc_tab_bytes = num_elements * 16 + 1;
  if (!GuestTryCopy(desc_tab, REX_RAW_ADDR(vdesc + 0x10), desc_tab_bytes)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto elem_u16 = [&](uint32_t i, uint32_t off) -> uint32_t {
    return (uint32_t(desc_tab[i * 16 + off]) << 8) | desc_tab[i * 16 + off + 1];
  };
  const auto elem_u32 = [&](uint32_t i, uint32_t off) -> uint32_t {
    uint32_t v;
    std::memcpy(&v, desc_tab + i * 16 + off, 4);
    return BSwap32(v);
  };
  bool have_pos = false;
  bool have_bw = false;
  bool have_bi = false;
  item.uv_offset = 0;
  item.uv_fmt = 0;
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.normal_offset = 0;
  item.normal_fmt = 0;
  item.tangent_offset = 0;
  item.binormal_offset = 0;
  item.tb_fmt = 0;
  item.tan_fmt = 0;
  bool have_tan = false;
  bool have_bin = false;
  item.skinned = false;
  for (uint32_t i = 0; i < num_elements; ++i) {
    const uint32_t stream = elem_u16(i, 0);
    const uint32_t usage = desc_tab[i * 16 + 9];
    if (stream != 0) continue;
    if (usage == 0 && !have_pos) {
      item.pos_offset = uint16_t(elem_u16(i, 2));
      item.pos_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
      have_pos = true;
    } else if (usage == 3 && item.normal_fmt == 0) {
      const uint8_t fmt = uint8_t(elem_u32(i, 4) & 0x3F);
      if (fmt == 16) {  // k_10_11_11 packed normal
        item.normal_offset = uint16_t(elem_u16(i, 2));
        item.normal_fmt = fmt;
      }
    } else if (usage == 5 && item.uv_fmt == 0) {
      item.uv_offset = uint16_t(elem_u16(i, 2));
      item.uv_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
    } else if (usage == 5 && item.uv2_fmt == 0) {
      item.uv2_offset = uint16_t(elem_u16(i, 2));
      item.uv2_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
    } else if (usage == 1 && !have_bw) {  // blend weights u8x4
      item.bw_offset = uint16_t(elem_u16(i, 2));
      have_bw = (elem_u32(i, 4) & 0x3F) == 6;
    } else if (usage == 2 && !have_bi) {  // blend indices u8x4
      item.bi_offset = uint16_t(elem_u16(i, 2));
      have_bi = (elem_u32(i, 4) & 0x3F) == 6;
    } else if (usage == 6 && !have_tan &&
               (elem_u32(i, 4) & 0x3F) == 16) {  // tangent k_10_11_11
      item.tangent_offset = uint16_t(elem_u16(i, 2));
      have_tan = true;
    } else if (usage == 7 && !have_bin &&
               (elem_u32(i, 4) & 0x3F) == 16) {  // binormal k_10_11_11
      item.binormal_offset = uint16_t(elem_u16(i, 2));
      have_bin = true;
    }
  }
  if (have_tan && have_bin) {
    item.tb_fmt = 16;
  }
  if (have_tan) {
    item.tan_fmt = 16;  // tangent-only layouts (water meshes) use this
  }
  if (!have_pos) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.skinned = have_bw && have_bi;
  item.stride = desc_tab[num_elements * 16];  // byte at vdesc+(num+1)*16
  if (item.stride == 0) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Mesh BBox = two Vector4s at +0x00/+0x10. Reject NaN/absurd bounds.
  for (int axis = 0; axis < 3; ++axis) {
    const float lo = LoadGuestF32(base, mesh + axis * 4);
    const float hi = LoadGuestF32(base, mesh + 0x10 + axis * 4);
    if (!(hi - lo < 50000.0f)) {
      g_rej_bbox.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    item.bbox_min[axis] = lo;
    item.bbox_max[axis] = hi;
  }

  item.mesh = mesh;
  item.pending = false;
  item.cloth_quads = false;
  item.vb_obj = vb;
  item.ib_obj = ib;
  // Guarded: the registration prewarm can reach buffer objects before they
  // finish initializing (the capture path only ever sees live ones).
  uint32_t vb_words[3] = {}, ib_words[3] = {};
  if (!GuestTryCopy(vb_words, REX_RAW_ADDR(vb + kBufferPhysAddr), sizeof(vb_words)) ||
      !GuestTryCopy(ib_words, REX_RAW_ADDR(ib + kBufferPhysAddr), sizeof(ib_words))) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.vb_addr = BSwap32(vb_words[0]) & 0xFFFFFFFC;
  item.vb_bytes = BSwap32(vb_words[2]);
  item.ib_addr = BSwap32(ib_words[0]) & 0xFFFFFFFC;
  item.ib_count = BSwap32(ib_words[2]);
  // vb_bytes need only COVER a whole number of vertices, not equal one
  // exactly. Community custom maps (ArenaBuilder-built worlds) ship vertex
  // buffers with a few trailing bytes past the last vertex - e.g. 43968
  // bytes at stride 36 leaves 12 over - and requiring an exact multiple
  // rejected the entire mesh, so the world rendered with no ground at all.
  // Every consumer already derives the vertex count as vb_bytes / stride
  // (integer division), so the trailing bytes are simply unused; all that
  // is really required is room for at least one vertex.
  if (item.vb_addr == 0 || item.ib_addr == 0 || item.vb_bytes == 0 ||
      item.ib_count == 0 || item.stride == 0 || item.vb_bytes < item.stride) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Material channels: resolve the "diffuse" and "lightmap" texture guids to
  // registered texture objects.
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  item.macro_tex = 0;
  item.macro_scale = 1.0f;
  item.macro_opacity = 1.0f;
  item.scroll_u = 0.0f;
  item.scroll_v = 0.0f;
  item.decal_art = 0;
  item.hair = false;
  item.char_family = 0;
  item.hair_alpha_tex = 0;
  item.ropa = false;
  item.decal = false;
  item.decal_tileable = false;
  item.transparent = false;
  item.water = false;
  item.water_flowing = false;
  item.water_ocean = 0;
  item.water_normal = 0;
  item.water_normal2 = 0;
  item.water_env = 0;
  item.unlit = false;
  item.env_family = 0;
  item.dynobj = 0;
  item.spec_tex = 0;
  item.tint[0] = item.tint[1] = item.tint[2] = item.tint[3] = 0.0f;
  // Material header + channel array read via guarded bulk copies: the
  // registration prewarm walks materials while their arena is still
  // loading, where raw reads fault the thread. (The capture path gets the
  // same copies; one memcpy per material beats ~8 volatile loads per
  // channel anyway.)
  const uint32_t material = REX_LOAD_U32(mesh + kMeshMaterial);
  uint32_t mat_head[3] = {};
  if (GuestReadableApprox(base, material) &&
      GuestTryCopy(mat_head, REX_RAW_ADDR(material), sizeof(mat_head))) {
    const uint32_t num_channels = BSwap32(mat_head[0]);
    const uint32_t channels = BSwap32(mat_head[2]);
    uint8_t chan_buf[32 * 0x20];
    if (num_channels != 0 && num_channels <= 32 && GuestReadableApprox(base, channels) &&
        GuestTryCopy(chan_buf, REX_RAW_ADDR(channels), num_channels * 0x20)) {
      const auto chan_u32 = [&](uint32_t idx, uint32_t off) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, chan_buf + idx * 0x20 + off, 4);
        return BSwap32(v);
      };
      for (uint32_t i = 0; i < num_channels; ++i) {
        const uint32_t name = chan_u32(i, 0);
        char text[20];
        GuestTryReadString(base, name, text, sizeof(text));
        if (text[0] == '\0') continue;
        uint32_t* slot = nullptr;
        if (std::memcmp(text, "diffuse", 8) == 0) {
          slot = &item.diffuse_tex;
        } else if (std::memcmp(text, "lightmap", 9) == 0) {
          slot = &item.lightmap_tex;
        } else if (std::memcmp(text, "macrooverlay", 13) == 0) {
          slot = &item.macro_tex;
        } else if (std::memcmp(text, "decal", 6) == 0) {
          slot = &item.decal_art;
        } else if (std::memcmp(text, "normal", 7) == 0) {
          // Exact match only. Consumed by the water branches (the flowing
          // ripple map / the ocean's first PCA component); other families
          // ignore it.
          slot = &item.water_normal;
        } else if (std::memcmp(text, "normal2", 8) == 0) {
          // The ocean's second PCA normal component map.
          slot = &item.water_normal2;
        } else if (std::memcmp(text, "alpha", 6) == 0) {
          // Hair strand coverage (cac_hair/defaulthair tf5, sampled at the
          // second texcoord), the hair alpha-blend term.
          slot = &item.hair_alpha_tex;
        } else if (std::memcmp(text, "environment", 12) == 0) {
          // Environment CUBE map, the water and environment.reflective*
          // reflection term.
          slot = &item.water_env;
        } else if (std::memcmp(text, "detail", 7) == 0) {
          // Exact match ("detailNormalUVScale" is a different channel).
          // Constant detail texture folded into the fam 5/6 normal
          // composition (see DrawItem::detail_tex).
          slot = &item.detail_tex;
        } else if (std::memcmp(text, "specular", 9) == 0 ||
                   std::memcmp(text, "noise", 6) == 0) {
          // Spec/eccentricity/reflection-mask map (environment families) /
          // the animated.tree "noise" tint map, bound at t4 on families
          // that carry no decal art (see DrawItem::spec_tex).
          slot = &item.spec_tex;
        } else if (std::memcmp(text, "detailNormalUVScale", 20) == 0) {
          // Shader-constant channel (the game's VS c8.x, typically 8): the
          // detail normal map's UV multiplier, consumed by the v2 kd term.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (f > 0.0f && f < 1e6f) {
            item.detail_scale = f;
          }
          continue;
        } else if (std::memcmp(text, "uAnimationSpeed", 16) == 0 ||
                   std::memcmp(text, "vAnimationSpeed", 16) == 0) {
          // Shader-constant channels (the game's VS c7.x / c8.x): UV scroll
          // speed in texcoords per second, consumed by the fam-14
          // scrollincandescent branch. Signed; the direction is authored.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (std::fabs(f) < 1e6f) {  // false for NaN too
            (text[0] == 'u' ? item.scroll_u : item.scroll_v) = f;
          }
          continue;
        } else if (std::memcmp(text, "macroOverlayUVScale", 20) == 0 ||
                   std::memcmp(text, "macroOverlayOpacity", 20) == 0) {
          // Shader-constant channel: the float lives in the first guid word.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (f > 0.0f && f < 1e6f) {
            (text[12] == 'U' ? item.macro_scale : item.macro_opacity) = f;
          }
          continue;
        } else if (std::memcmp(text, "Attribul", 8) == 0) {
          // AttribulatorMaterialName: chan+0x18 is the material name string.
          // "character.hair" marks the grayscale hair that needs the
          // per-character tint from the pixel constant bank; "sky.*" draws
          // fullbright; "character.cloth_ropa" is the Ropa cloth-sim VS
          // variant (flag-row-switched skinned/rigid, see CaptureSkinnedState).
          const uint32_t s = chan_u32(i, 0x18);
          if (GuestReadableApprox(base, s)) {
            // 40 bytes: "character.livingworld_vehicles_glass" (36 chars) is
            // the longest name that must be distinguishable; the previous
            // 28-byte buffer truncated both vehicle names into the plain
            // "livingworld" pedestrian prefix.
            char mat_name[40];
            GuestTryReadString(base, s, mat_name, sizeof(mat_name));
            // character.*_ropa = the Ropa cloth-sim VS variant (flag-row
            // switched skinned/rigid, see CaptureSkinnedState). The suffix
            // composes with the base family name: cloth_ropa (player tees),
            // default_cloth_ropa (NPC jackets), hair_ropa and
            // default_hair_ropa all exist in the attrib table; so detect it
            // generically and STRIP it, letting the family idioms below see
            // the base name. Matching only the player's character.cloth_ropa
            // left NPC ropa garments on the generic skinned path, where
            // every palette home is refused: the layout's guess register is
            // this VS's FLAG row (scores 0), and the true +1 home fails the
            // (0,0,0,w)/(1,0,0,0) parameter-row signature because this
            // variant's row is (1, junk, junk, junk), the persistent
            // invisible-torso NPC (observed: a
            // character.default_cloth_ropa mesh refused all three captures every
            // frame while the draw-time banks pass the gate at c5/c8).
            item.ropa = false;
            {
              const size_t len = strnlen(mat_name, sizeof(mat_name));
              if (len >= 5 && len < sizeof(mat_name) &&
                  std::memcmp(mat_name, "character.", 10) == 0 &&
                  std::memcmp(mat_name + len - 5, "_ropa", 5) == 0) {
                item.ropa = true;
                mat_name[len - 5] = '\0';
              }
            }
            item.hair = std::memcmp(mat_name, "character.hair", 15) == 0;
            item.unlit = std::memcmp(mat_name, "sky.", 4) == 0;
            // Character shading family (see DrawItem::char_family). Order
            // matters: "default_hair" before the "default" prefix, exact
            // "hair" after (memcmp includes the NUL for exact names).
            if (std::memcmp(mat_name, "character.", 10) == 0) {
              const char* sub = mat_name + 10;
              if (std::memcmp(sub, "default_hair", 13) == 0) {
                item.char_family = 5;
              } else if (std::memcmp(sub, "default", 7) == 0) {
                item.char_family = 1;
              } else if (std::memcmp(sub, "hair", 5) == 0) {
                item.char_family = 4;
              } else if (std::memcmp(sub, "livingworld_vehicles_glass", 27) == 0) {
                item.char_family = 7;  // reflection-only blended windows
              } else if (std::memcmp(sub, "livingworld_vehicles", 21) == 0) {
                item.char_family = 6;  // vehicle.fx paint/spec/cube body
              } else if (std::memcmp(sub, "livingworld", 11) == 0) {
                item.char_family = 3;
              } else {
                item.char_family = 2;  // skin/face/cloth/leather/shift/ropa
              }
              // character.alpha (exact): translucent accessory pieces
              // (sunglass lenses), alpha from the "alpha" channel texture
              // at uv2, blended after every opaque piece (see
              // DrawItem::char_alpha; captured draw order:
              // cac_alpha draws after all opaque CAC, before hair).
              item.char_alpha = std::memcmp(sub, "alpha", 6) == 0;
            }
            // environment.decal / environment.decal_tileable: graffiti and
            // painted-branding overlay meshes (see DrawItem::decal).
            // Tileable art WRAPS (rock/cliff faces tile the art across the
            // whole surface, uv spans many periods; clamping stretches the
            // border texels into giant streaks); single-placement decal art
            // CLAMPS (wrap tiled the graffiti across the plaza).
            item.decal = std::memcmp(mat_name, "environment.decal", 17) == 0;
            item.decal_tileable =
                std::memcmp(mat_name, "environment.decal_tileable", 26) == 0;
            // environment.transparent: alpha-blended world geometry (mist
            // sheets, glass, fences); see DrawItem::transparent.
            item.transparent =
                std::memcmp(mat_name, "environment.transparent", 23) == 0;
            // water.* (canal) and ocean.* (the sea): transparent sub-pass
            // with the dedicated water shading branch (see DrawItem::water).
            // ocean.default has NO diffuse channel at all (ocean.fx computes
            // color purely from the environment cube x lightmap x fresnel);
            // without this branch it rendered as an 8 km white plane (white
            // fallback diffuse x near-white ocean lightmap x2).
            item.water = std::memcmp(mat_name, "water.", 6) == 0 ||
                         std::memcmp(mat_name, "ocean.", 6) == 0;
            // water.flowing* = the flowingwateralpha shader family (canal /
            // waterfall). Takes the exact water branch when the frame's
            // m_params rows are captured (see FrameScene::water_rows).
            item.water_flowing =
                std::memcmp(mat_name, "water.flowing", 13) == 0;
            // ocean.default / ocean.reflection: the sea surface and its
            // baked horizon reflection sheet (see DrawItem::water_ocean).
            if (std::memcmp(mat_name, "ocean.default", 14) == 0) {
              item.water_ocean = 1;
            } else if (std::memcmp(mat_name, "ocean.reflection", 17) == 0) {
              item.water_ocean = 2;
            }
            // Exact world-shading family (see DrawItem::env_family). The
            // attrib <-> pixel-shader-family mapping is 1:1; the shading
            // models were verified per-pixel against the game's own shaders.
            const auto is = [&](const char* s, size_t n) {
              return std::memcmp(mat_name, s, n) == 0;
            };
            if (is("environment.default", 20)) {
              item.env_family = 1;
            } else if (is("environmentsimple.default", 26)) {
              item.env_family = 2;
            } else if (is("environment.decal_tileable", 26)) {
              item.env_family = 4;  // includes decal_tileable_simple
            } else if (is("environment.decal", 18)) {
              item.env_family = 3;
            } else if (is("environment.reflective_simple", 30)) {
              item.env_family = 6;
            } else if (is("environment.reflective_trans", 29)) {
              // transparentenvironmentreflective: the sloped glass canopy /
              // awning panels. Blended in the sorted alpha sub-pass with the
              // fam-5 shading minus kd/macro, premultiplied body, out a^2
              // (model verified 0.0-error against the ucode).
              item.env_family = 13;
            } else if (is("environment.reflective", 23)) {
              item.env_family = 5;
            } else if (is("environmentsimple.alphatest", 28)) {
              item.env_family = 7;
            } else if (is("environmentsimple.diffuse", 26)) {
              item.env_family = 8;
            } else if (is("tree.default", 13)) {
              item.env_family = 9;
            } else if (is("animated.tree", 14)) {
              item.env_family = 10;
            } else if (is("proxyworld.", 11)) {
              item.env_family = 11;
            } else if (is("incandescent.default", 21)) {
              item.env_family = 12;
            } else if (is("incandescent.backlituvscroll", 29)) {
              // scrollincandescent.fx: emissive diffuse scrolled by
              // g_fAnimationTime x the u/vAnimationSpeed channels (the
              // stadium LED chyron band).
              item.env_family = 14;
            }
            // dynamicobject.fx props (dispensers, dumpsters, benches, cans):
            // rigid movable objects with their own dual-shadow lit PS
            // (model verified exact against the game's own pixel shader).
            // Separate from the world env families; the lighting rows come
            // from the draw's PS bank, not the frame-global world rows.
            if (is("dynamicobject.alphatest", 24)) {
              item.dynobj = 2;
            } else if (is("dynamicobject", 13)) {
              item.dynobj = 1;
            }
          }
          continue;
        }
        if (slot != nullptr && *slot == 0) {
          // Prefer the channel's live stream record (chan+0x1C -> word 0 =
          // the renderengine::Texture actually bound): runtime-composed
          // customization textures (CAS face/skin, shoes, deck, wheels)
          // are never registered under an asset GUID. Validate via the
          // fetch-constant type bits before trusting the pointer.
          // Guarded: the prewarm walks materials whose stream records /
          // texture objects may not be loaded yet.
          const uint32_t stream = chan_u32(i, 0x1C);
          uint32_t tex = 0, tex_w0 = 0;
          if (GuestTryLoadU32(base, stream, &tex) &&
              GuestTryLoadU32(base, tex + 7 * 4, &tex_w0) && (tex_w0 & 3u) == 2u) {
            *slot = tex;
          }
          if (*slot == 0) {
            const uint64_t guid =
                (uint64_t(chan_u32(i, 0x10)) << 32) | chan_u32(i, 0x14);
            std::lock_guard<std::mutex> lock(g_texture_map_mutex);
            auto it = g_texture_map.find(guid & kGuidMask);
            if (it != g_texture_map.end()) {
              *slot = it->second;
            }
          }
        }
        // No early-out: channel order varies per material and the float
        // channels ride AFTER their texture (macroOverlayUVScale follows
        // macrooverlay on the plaza asphalt, breaking once the textures
        // resolved left the macro tiling at 1.0 instead of 0.3).
      }
    }
  }

  // Payload fingerprint: see ComputeItemFingerprint (the item cache
  // refreshes it on its own cadence).
  if (!ComputeItemFingerprint(base, item)) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Transform (identity for world geometry, instance matrix for props;
  // characters get their bone array's first matrix, which roughly places
  // them until skinning is implemented).
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  return true;
}

namespace {

// Streamed-artwork override (event ads): the Massive ad system rebinds
// texture fetch slots at draw time to show the current event-ad art in
// place of the asset's default poster. Two observed shapes: a 16x16
// min-mip stub channel whose real art exists ONLY at draw time (the
// ace-of-spades "Big Event" grid), and a full-size default poster (the
// letter-writing frames) whose diffuse is wholesale REPLACED with the
// current ad (the MONDO "THE NEW VIDEO" portrait). Adopt the fetch
// slot-4 words recorded at this mesh's LAST indexed draw this frame,
// but only if that draw's slot 3 is this item's own lightmap
// (mirror-masked base compare), which proves the recorded fetch state
// belongs to the mesh's MAIN-pass draw (the z-prepass leaves another
// material's bindings). Slot targets are family-specific (Skate 2
// shader source): environmentsimple.* sample the diffuse at s4;
// environment.decal samples its decal overlay art at s4 (diffuse rides
// s6); environment.default has DETAIL at s4, so its diffuse adoption
// stays stub-gated; never adopt a detail texture over real poster art.
// Per-frame state (g_frame_draw_fetch), so this runs on every item build;
// it is NOT part of the cacheable item core.
void AdoptDrawFetchOverrides(uint8_t* base, DrawItem& item) {
  if (item.env_family != 0 && item.diffuse_tex != 0 && item.lightmap_tex != 0) {
    // Physical base of a fetch word-1, collapsing the 0xA0000000-style
    // cached/uncached mirrors (the channel object records the mirror
    // address, the draw-time fetch constant the plain one).
    const auto phys_base = [](uint32_t w1) { return w1 & 0x1FFFF000u; };
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    const auto fit =
        g_frame_draw_fetch.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
    if (fit != g_frame_draw_fetch.end()) {
      // Texture-object reads only on a fetch-map hit, and guarded: the map
      // is empty on prewarm/loading frames, and the GUID registry can
      // resolve to a freed previous-map object there (raw reads faulted).
      uint32_t lm_w1 = 0;
      const uint32_t* slot3 = fit->second.data();
      const uint32_t* slot4 = fit->second.data() + 6;
      const uint32_t art_w = (slot4[2] & 0x1FFFu) + 1;
      const uint32_t art_h = ((slot4[2] >> 13) & 0x1FFFu) + 1;
      const bool main_pass = (slot3[0] & 3u) == 2u && (slot4[0] & 3u) == 2u &&
                             GuestTryLoadU32(base, item.lightmap_tex + 8 * 4, &lm_w1) &&
                             phys_base(slot3[1]) == phys_base(lm_w1);
      // Dimension floor: never adopt a placeholder-sized s4 (an idle ad
      // rotation slot) over whatever the channel resolves to.
      //
      // Own-channel guard: on families whose s4 carries a surface map
      // rather than art (environment.default binds its detail normal map
      // there), the stub gate alone is not enough. A material demoted by
      // the streamer has a stub-sized diffuse too, and adopting s4 then
      // serves the material's OWN detail/spec/normal map as the diffuse
      // (distant demoted tree trunks rendered as their flat blue normal
      // map). Real event-ad art is never one of the material's own channel
      // textures, so any s4 that matches one is a normal binding, not a
      // rebind worth adopting.
      const auto s4_is_own_channel = [&](uint32_t chan_tex) {
        uint32_t own_w1 = 0;
        return chan_tex != 0 &&
               GuestTryLoadU32(base, chan_tex + 8 * 4, &own_w1) &&
               phys_base(slot4[1]) == phys_base(own_w1);
      };
      const bool s4_is_own = s4_is_own_channel(item.detail_tex) ||
                             s4_is_own_channel(item.spec_tex) ||
                             s4_is_own_channel(item.water_normal);
      if (main_pass && !s4_is_own && (art_w >= 32 || art_h >= 32)) {
        if (item.env_family == 3) {
          uint32_t da_w1 = 0;
          if (item.decal_art != 0 &&
              GuestTryLoadU32(base, item.decal_art + 8 * 4, &da_w1) &&
              phys_base(slot4[1]) != phys_base(da_w1)) {
            std::memcpy(item.decal_fetch, slot4, 6 * sizeof(uint32_t));
          }
        } else {
          uint32_t diff_w2 = 0, diff_w1 = 0;
          if (!GuestTryLoadU32(base, item.diffuse_tex + 9 * 4, &diff_w2) ||
              !GuestTryLoadU32(base, item.diffuse_tex + 8 * 4, &diff_w1)) {
            diff_w2 = 0;
            diff_w1 = slot4[1];  // unreadable diffuse object: adopt nothing
          }
          const uint32_t diff_w = (diff_w2 & 0x1FFFu) + 1;
          const uint32_t diff_h = ((diff_w2 >> 13) & 0x1FFFu) + 1;
          const bool diff_stub = diff_w <= 32 && diff_h <= 32;
          const bool s4_is_diffuse = item.env_family == 2 ||
                                     item.env_family == 7 ||
                                     item.env_family == 8;
          if ((s4_is_diffuse || diff_stub) &&
              phys_base(slot4[1]) != phys_base(diff_w1)) {
            std::memcpy(item.diffuse_fetch, slot4, 6 * sizeof(uint32_t));
          }
        }
      }
    }
  }

}

// ---- World-item cache ------------------------------------------------------
// BuildItemFromMesh walks the descriptor, the material channels (string
// reads) and the payload fingerprint, and used to run for EVERY visible
// item EVERY frame on the guest render thread. The walk's result is stable
// while a mesh stays loaded, so it is cached per mesh:
//   - every frame: 4-pointer structural validation + buffer address check
//     (streaming reuses arena addresses; re-inits rebuild)
//   - every 4th frame (or every frame for skinned/ropa payloads, whose
//     buffers the CPU sim rewrites): fingerprint refresh
//   - every 32nd frame: full rebuild (material channels re-resolve: late
//     CAS composites, streamed textures binding into channel records)
// Invalidated when the mesh re-registers (tRModelData::Fixup /
// AddRenderInstance -> OnMeshRegistered), on the menus/loading flip and by
// the mesh-cache debug flush. Guarded by its own mutex: builders run on the
// guest render thread, invalidation on loader threads.
struct CachedItemCore {
  DrawItem item;  // draws empty, world identity, fetch overrides zero
  uint64_t fp_frame = 0;       // next fingerprint refresh (guest frames)
  uint64_t rebuild_frame = 0;  // next full rebuild
};
std::mutex g_item_cache_mutex;
std::unordered_map<uint32_t, CachedItemCore> g_item_cache;

void InvalidateCachedItem(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  g_item_cache.erase(mesh);
}

}  // namespace
void ClearItemCache() {
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  g_item_cache.clear();
}

namespace {

bool BuildItemFromMeshCached(uint8_t* base, uint32_t mesh, DrawItem& item) {
  const uint64_t frame = g_guest_frame;
  const bool prof = REXCVAR_GET(skate3_native_render_scene_perf_items);
  {
    std::lock_guard<std::mutex> lock(g_item_cache_mutex);
    auto it = g_item_cache.find(mesh);
    if (it != g_item_cache.end() && frame < it->second.rebuild_frame) {
      CachedItemCore& core = it->second;
      // Structural validation: the mesh's pointer fields must still match
      // the cached walk.
      const bool intact =
          REX_LOAD_U32(mesh + kMeshVertexBuffer) == core.item.vb_obj &&
          REX_LOAD_U32(mesh + kMeshIndexBuffer) == core.item.ib_obj;
      // Buffer payload address/size can move under the same objects
      // (streaming re-init), cheap guarded re-read every frame.
      uint32_t vb_words[3] = {}, ib_words[3] = {};
      if (intact &&
          GuestTryCopy(vb_words, REX_RAW_ADDR(core.item.vb_obj + kBufferPhysAddr),
                       sizeof(vb_words)) &&
          GuestTryCopy(ib_words, REX_RAW_ADDR(core.item.ib_obj + kBufferPhysAddr),
                       sizeof(ib_words)) &&
          (BSwap32(vb_words[0]) & 0xFFFFFFFC) == core.item.vb_addr &&
          BSwap32(vb_words[2]) == core.item.vb_bytes &&
          (BSwap32(ib_words[0]) & 0xFFFFFFFC) == core.item.ib_addr &&
          BSwap32(ib_words[2]) == core.item.ib_count) {
        // Fingerprint cadence: per frame for CPU-rewritten payloads
        // (skinned/ropa), every 4th frame otherwise (mesh_revalidate heals
        // streamed fills within that window; address reuse is caught by the
        // registration invalidation immediately).
        const bool dynamic_payload = core.item.skinned || core.item.ropa;
        if (dynamic_payload || frame >= core.fp_frame) {
          const auto fp_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
          const bool fp_ok = ComputeItemFingerprint(base, core.item);
          if (prof) {
            g_pw_bi_fp.Add(PerfNsSince(fp_t0));
          }
          if (!fp_ok) {
            g_rej_geom.fetch_add(1, std::memory_order_relaxed);
            g_item_cache.erase(it);
            return false;
          }
          core.fp_frame = frame + 4;
        }
        item = core.item;
        g_item_cache_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      g_item_cache.erase(it);
    }
  }
  const auto walk_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
  const bool walked = BuildItemFromMesh(base, mesh, item);
  if (prof) {
    g_pw_bi_walk.Add(PerfNsSince(walk_t0));
  }
  if (!walked) {
    return false;
  }
  g_item_cache_builds.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  if (g_item_cache.size() >= 16384) {
    // Runaway growth backstop (streaming churn over a long session): drop
    // everything and let the next frames rebuild, one frame of full walks.
    g_item_cache.clear();
  }
  CachedItemCore core;
  core.item = item;
  core.fp_frame = frame + 4;
  // Materials with an unresolved diffuse retry the full walk quickly: CAS
  // composites and streamed channel textures bind shortly after first sight
  // (ocean.default legitimately has no diffuse and just rebuilds often,
  // a handful of items).
  core.rebuild_frame = frame + (item.diffuse_tex != 0 ? 32 : 2);
  g_item_cache[mesh] = std::move(core);
  return true;
}

bool BuildItemGeometry(uint8_t* base, uint32_t ctx, DrawItem& item) {
  const uint32_t record = REX_LOAD_U32(ctx);
  if (!GuestReadableApprox(base, record)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t mesh = REX_LOAD_U32(record);
  if (!GuestReadableApprox(base, mesh)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const bool prof = REXCVAR_GET(skate3_native_render_scene_perf_items);
  const auto core_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
  const bool built = BuildItemFromMeshCached(base, mesh, item);
  if (prof) {
    g_pw_bi_core.Add(PerfNsSince(core_t0));
  }
  if (!built) {
    return false;
  }
  // Draw-time fetch overrides are per-frame state, applied after the cached
  // core, on every build.
  const auto fetch_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
  AdoptDrawFetchOverrides(base, item);
  if (prof) {
    g_pw_bi_fetch.Add(PerfNsSince(fetch_t0));
  }
  // Identity for per-instance consumers (the occlusion cull's guest-side
  // dispatch filter keys on it; the dynamic capture path overwrites it with
  // its own value).
  item.ctx = ctx;

  // Culled island draw list from the context.
  const uint32_t draw_count = REX_LOAD_U16(ctx + kCtxDrawCountU16);
  const uint32_t draw_list = REX_LOAD_U32(ctx + kCtxDrawList);
  if (draw_count == 0 || draw_count > 512 || !GuestReadableApprox(base, draw_list)) {
    g_rej_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.draws.reserve(draw_count);
  for (uint32_t i = 0; i < draw_count; ++i) {
    const uint32_t d = draw_list + i * 16;
    DrawEntry entry{REX_LOAD_U32(d), REX_LOAD_U32(d + 4), REX_LOAD_U32(d + 8),
                    REX_LOAD_U32(d + 12)};
    if (entry.index_count == 0 || entry.index_count > item.ib_count) continue;
    item.draws.push_back(entry);
  }
  if (item.draws.empty()) {
    g_rej_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

}  // namespace

bool Enabled() { return SceneEnabled(); }

void SetSettingsMenuBlur(bool enabled) {
  g_settings_menu_blur.store(enabled, std::memory_order_relaxed);
  REXLOG_INFO("native-scene: settings-menu blur {} (post-processor {})", enabled ? "ON" : "off",
              enabled ? "armed" : "disarms after ease-out");
  if (enabled) {
    g_post_blur_log_count.store(0, std::memory_order_relaxed);
    // Arms the emulated-output post-processor (boot frames / manual emulated
    // mode). It disarms itself once the blur has fully eased out.
    rex::graphics::RequestNativeGuestOutputPostProcess(true);
  }
}

void FlushTextureCache() { g_flush_textures.store(true, std::memory_order_relaxed); }

void RequestSceneRingDump() {
  g_scene_ring_dump.store(true, std::memory_order_release);
}
void FlushMeshCache() { g_flush_meshes.store(true, std::memory_order_relaxed); }

// RecordBoneSignal / RecordCameraSignal: native/skate3_native_diagnostics.cpp.

bool ToggleSceneEnabled() {
  if (!REXCVAR_GET(skate3_native_render)) {
    REXLOG_WARN(
        "native-scene: renderer toggle ignored; the skate3_native_render hook layer "
        "is off (set it and restart; it installs the capture hooks the scene needs)");
    return false;
  }
  const bool enabled = !SceneEnabled();
  if (enabled) {
    // Re-enabling doubles as the retry gesture after a hard pipeline
    // failure (which yields to the emulated output while the cvars stay
    // on): clear the latch so EnsurePipeline runs again.
    ResetSceneFailure();
    // Capture idles while the emulated renderer is active, so anything still
    // published is from before the switch away (stale camera). Drop it:
    // RenderScene yields to the emulated frame until the capture hooks
    // publish a fresh scene (the next frame).
    {
      std::lock_guard<std::mutex> lock(g_scene_mutex);
      g_scene.reset();
    }
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      g_scene_2d.clear();
      g_scene_spline.clear();
    }
    // Warm up before taking over: after a long emulated stretch the decode
    // caches can be cold/stale, and the takeover frame would pay the whole
    // decode burst at once. A warm cache completes warmup in one frame.
    g_warmup_armed.store(true, std::memory_order_relaxed);
    // The off-screen retention map is equally stale after the gap.
    g_retained_clear.store(true, std::memory_order_relaxed);
  }
  REXCVAR_SET(skate3_native_render_scene, enabled);
  REXLOG_INFO("native-scene: switched to the {} renderer (runtime toggle)",
              enabled ? "NATIVE" : "EMULATED");
  return enabled;
}

void OnRegisterTexture(uint64_t guid, uint32_t texture) {
  if (texture == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_texture_map_mutex);
  g_texture_map[guid & kGuidMask] = texture;
}

void OnMeshRegistered(uint8_t* base, uint32_t mesh) {
  if (mesh == 0) {
    return;
  }
  // A (re-)registration is the "content changed at this address" signal;
  // drop the cached item core so the next frame re-walks it.
  InvalidateCachedItem(mesh);
  // Publish the guest base here too: at boot the loading-screen prewarm
  // workers run before the first gameplay frame would otherwise publish it.
  g_guest_base.store(base, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  const uint64_t now = g_frames_rendered.load(std::memory_order_relaxed);
  const auto [it, fresh] = g_prewarm_seen.try_emplace(mesh, now);
  // 300-frame (~2 s) dedup window: activation bursts re-register the same
  // model once per clone instance: queue it once; a re-registration past
  // the window is a re-stream (content changed at this address) and must
  // re-decode BEFORE its first draw or it pops in as a visible miss.
  if (!fresh) {
    if (now - it->second < 300) {
      return;
    }
    it->second = now;
  }
  if (g_prewarm_queue.size() < 65536) {
    g_prewarm_queue.push_back({mesh, 60});
    g_prewarm_cv.notify_one();
  }
}

// Validate a candidate tRModelData against the layout the game's OWN
// AddRenderInstance (sub_82791290) walks: mesh table pointer at model+0x24,
// mesh COUNT as a u16 at model+0x32 (`lhz r10,50(r11)`). Table entries are
// 8 bytes: word0 = mesh, word1 = 0 for the optimesh world form (its
// material lives at mesh+0x24, which is also why a {mesh, material} pair
// scan finds nothing) / non-zero for the tRMeshData character/prop form.
// Every dereference is SEH-guarded: the offset probe feeds this arbitrary
// instance words (bbox floats, guids) as candidates.
static bool PlausibleRModel(uint8_t* base, uint32_t model) {
  uint32_t count_w = 0, table = 0;
  if (!GuestTryLoadU32(base, model + 0x30, &count_w) ||
      !GuestTryLoadU32(base, model + 0x24, &table)) {
    return false;
  }
  const uint32_t num_meshes = count_w & 0xFFFF;  // u16 at +0x32
  if (num_meshes == 0 || num_meshes > 512) {
    return false;
  }
  // Entry 0: the mesh must carry a plausible material (channel count sane).
  uint32_t mesh0 = 0, mesh0_mat = 0, mat_channels = 0;
  return GuestTryLoadU32(base, table, &mesh0) &&
         GuestTryLoadU32(base, mesh0 + kMeshMaterial, &mesh0_mat) &&
         mesh0_mat >= 0x10000 &&
         GuestTryLoadU32(base, mesh0_mat, &mat_channels) && mat_channels != 0 &&
         mat_channels <= 64;
}

// Queue every optimesh-form mesh of a validated tRModelData for the prewarm
// decode workers.
static void QueueModelMeshes(uint8_t* base, uint32_t model) {
  uint32_t count_w = 0, table = 0;
  if (!GuestTryLoadU32(base, model + 0x30, &count_w) ||
      !GuestTryLoadU32(base, model + 0x24, &table)) {
    return;
  }
  const uint32_t num_meshes = count_w & 0xFFFF;
  for (uint32_t i = 0; i < num_meshes && i < 512; ++i) {
    uint32_t mesh = 0, entry_mat = 0;
    if (!GuestTryLoadU32(base, table + i * 8, &mesh) ||
        !GuestTryLoadU32(base, table + i * 8 + 4, &entry_mat)) {
      continue;
    }
    if (entry_mat != 0) {
      continue;  // tRMeshData form (characters/props), wrong offsets
    }
    OnMeshRegistered(base, mesh);
  }
}

void OnModelFixup(uint8_t* base, uint32_t model) {
  // Fires per model during the load's DISK-STREAMING phase (arena fixup),
  // the early prewarm source. The validation gate keeps non-render models
  // (and any layout drift) out of the queue.
  if (model != 0 && PlausibleRModel(base, model)) {
    QueueModelMeshes(base, model);
  }
}

void OnAddRenderInstance(uint8_t* base, uint32_t instance) {
  if (instance == 0) {
    return;
  }
  // Resolve tInstance::m_pRModel. The game's own AddRenderInstance reads it
  // at +0x80 (`lwz r11,128(r4)`); the offset is still confirmed by the
  // validation probe before first use so an image-version drift degrades to
  // "prewarm off" instead of queueing garbage.
  uint32_t off = g_instance_rmodel_offset.load(std::memory_order_relaxed);
  if (off == 0) {
    for (uint32_t cand = 0x80; cand < 0xC0; cand += 4) {
      uint32_t model = 0;
      if (GuestTryLoadU32(base, instance + cand, &model) &&
          PlausibleRModel(base, model)) {
        g_instance_rmodel_offset.store(cand, std::memory_order_relaxed);
        REXLOG_INFO("native-scene: tInstance::m_pRModel offset confirmed at +0x{:X}",
                    cand);
        off = cand;
        break;
      }
    }
    if (off == 0) {
      return;  // CModel-only / embedded instance, or the model is not ready
    }
  }
  uint32_t model = 0;
  if (!GuestTryLoadU32(base, instance + off, &model) ||
      !PlausibleRModel(base, model)) {
    return;  // instances without a renderable model are normal
  }
  QueueModelMeshes(base, model);
}

struct PfxCapture {
  float ps[32][4];
  float vs[8][4];
  uint32_t fetch[8][6];
  int64_t ps_ns;  // PerfClock stamp of the last PS-row capture (0 = never)
  bool vs_seen;
};
PfxCapture g_pfx_cap[kPfxPassCount] = {};


void OnPhotoGrabRequest() {
  g_photo_grab_request_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnPhotoGrabDone() {
  g_photo_grab_done_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}

// True while the photo display-card quad is in the live 2D stream during an
// armed photo flow; the compose auto-trace trigger
// (skate3_native_render.cpp) keys on it.
bool PhotoCardVisible() {
  if (!g_photo_flow_frame.load(std::memory_order_relaxed)) {
    return false;
  }
  const int64_t seen = g_photo_card_seen_ns.load(std::memory_order_relaxed);
  if (seen < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  return now_ns - seen < 250'000'000;
}

// Debug-path classification, cached per shader object (guest render thread
// only, like the blur classifier). -1 = not a photo postfx shader.
int ClassifyPfxShader(uint8_t* base, uint32_t obj) {
  if (obj == 0 || !GuestReadableApprox(base, obj)) {
    return -1;
  }
  static std::unordered_map<uint32_t, int> cache;
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char text[112] = {};
  for (int k = 0; k < 111; ++k) {
    text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
    if (text[k] == '\0') break;
  }
  int pass = -1;
  if (std::strstr(text, "postfx_visualfxPS") != nullptr) {
    pass = kPfxVisualFx;
  } else if (std::strstr(text, "bloom_dof_motionblur_dof_dowsample") != nullptr) {
    // sic: the game's own shader name carries the 'dowsample' typo.
    pass = kPfxDofDown;
  } else if (std::strstr(text, "bloom_dof_tap9dofMotionBlur") != nullptr) {
    pass = kPfxDofMB;
  } else if (std::strstr(text, "bloom_dof_tap9dofPS") != nullptr) {
    pass = kPfxDof;
  } else if (std::strstr(text, "postfx_uberPS") != nullptr) {
    pass = kPfxUber;
  } else if (std::strstr(text, "postfx_basictex_fisheye") != nullptr) {
    pass = kPfxFisheye;
  }
  if (cache.size() < 4096) {
    cache.emplace(obj, pass);
  }
  return pass;
}

void CapturePfxConstants(uint8_t* base, uint32_t bank_ptr, uint32_t device,
                         bool pixel) {
  // Shader labels can be swapped in the OnSetShader hook; classify both.
  int pass = ClassifyPfxShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
  if (pass < 0) {
    pass = ClassifyPfxShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
  }
  if (pass < 0) {
    return;
  }
  PfxCapture& cap = g_pfx_cap[pass];
  // First-hit diagnostics: which passes ever capture, and on which side.
  static uint8_t s_seen[kPfxPassCount][2] = {};
  if (!s_seen[pass][pixel ? 0 : 1]) {
    s_seen[pass][pixel ? 0 : 1] = 1;
    REXLOG_INFO("native-scene: pfx capture first hit pass={} {} bank={:08X}", pass,
                pixel ? "PS" : "VS", bank_ptr);
  }
  if (pixel) {
    for (int r = 0; r < 32; ++r) {
      for (int i = 0; i < 4; ++i) {
        cap.ps[r][i] = LoadGuestF32(base, bank_ptr + uint32_t(r * 4 + i) * 4);
      }
    }
    if (device != 0 && GuestReadableApprox(base, device + 0x480)) {
      for (int s = 0; s < 8; ++s) {
        for (int w = 0; w < 6; ++w) {
          cap.fetch[s][w] = REX_LOAD_U32(device + 0x480 + uint32_t(s * 6 + w) * 4);
        }
      }
    }
    cap.ps_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    PerfClock::now().time_since_epoch())
                    .count();
  } else {
    for (int r = 0; r < 8; ++r) {
      for (int i = 0; i < 4; ++i) {
        cap.vs[r][i] = LoadGuestF32(base, bank_ptr + uint32_t(r * 4 + i) * 4);
      }
    }
    cap.vs_seen = true;
  }
}

void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device) {
  (void)base;
  (void)mask;
  if (device != 0) {
    g_device.store(device, std::memory_order_relaxed);
  }
  if (!SceneEnabled() || ptr == 0) {
    return;
  }
  if (bank == 0x4400) {
    g_ps_bank.store(ptr, std::memory_order_relaxed);
    return;
  }
  if (bank != 0x4000) {
    return;
  }
  g_vs_uploads.fetch_add(1, std::memory_order_relaxed);
  g_vs_bank.store(ptr, std::memory_order_relaxed);
  // NOTE: the photo postfx constant capture used to also run here (upload
  // time), but upload-time snapshots can attribute rows staged for the NEXT
  // pass to the previous pass's shader (the game stages constants before
  // rebinding), overwriting the correct draw-time capture taken by the
  // OnDrawDone hook; the F11 per-draw records read the same banks at the
  // draw hook and are correct for every postfx pass, so the draw hook is
  // the sole capture site now.
}


void OnPhotoReplayUpdate() {
  g_photo_replay_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnTakePhoto() {
  g_take_photo_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnMovieDecode() {
  g_movie_decode_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnMovieFrame(uint8_t* base, uint32_t renderer) {
  // Plane texture members of VideoRenderer_RwTexture, from the recompiled
  // Render body (Lock/FillTextureData/Unlock trios): [this+12] = Y,
  // [this+124] = U, [this+68] = V; the fill sources are the renderable's
  // y/u/v buffers at +8/+12/+16 (TransferYUVBuffer order).
  static constexpr uint32_t kPlaneOfs[3] = {12, 124, 68};
  MoviePlanes mf;
  for (int p = 0; p < 3; ++p) {
    uint32_t obj = 0;
    if (!GuestTryLoadU32(base, renderer + kPlaneOfs[p], &obj) || obj < 0x10000) {
      return;
    }
    // Stable fetch-words read at tex+0x1C (the D3D12 section's
    // ReadStableTexWords, inlined to keep this compiled guest-side).
    uint32_t raw[6], raw2[6];
    if (!GuestTryCopy(raw, base + obj + 0x1C, sizeof(raw)) ||
        !GuestTryCopy(raw2, base + obj + 0x1C, sizeof(raw2)) ||
        std::memcmp(raw, raw2, sizeof(raw)) != 0) {
      return;
    }
    for (int i = 0; i < 6; ++i) {
      mf.words[p][i] = BSwap32(raw[i]);
    }
    if ((mf.words[p][0] & 3u) != 2 || mf.words[p][1] == 0) {
      return;  // not a live texture fetch constant
    }
  }
  mf.y_addr = mf.words[0][1];
  mf.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PerfClock::now().time_since_epoch())
              .count();
  bool new_key = false;
  {
    std::lock_guard<std::mutex> lock(g_movie_mutex);
    int slot = 0;
    for (int m = 0; m < kMaxMovies; ++m) {
      if (g_movies[m].y_addr == mf.y_addr) {
        slot = m;
        break;
      }
      if (g_movies[m].ns < g_movies[slot].ns) {
        slot = m;  // evict the stalest entry for a new video
      }
    }
    new_key = g_movies[slot].y_addr != mf.y_addr;
    g_movies[slot] = mf;
  }
  OnMovieDecode();  // shared FMV heartbeat (the emulated-yield fallback)
  static std::atomic<uint32_t> s_logged{0};
  if (new_key && s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO(
        "native-scene: FMV planes published (y=[{:08X} {:08X}] u=[{:08X} "
        "{:08X}] v=[{:08X} {:08X}])",
        mf.words[0][0], mf.words[0][1], mf.words[1][0], mf.words[1][1],
        mf.words[2][0], mf.words[2][1]);
  }
}

void On2dPhase(uint32_t bit, bool enter) {
  if (bit >= 6) {
    return;
  }
  if (enter) {
    g_phase2d_depth[bit].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_phase2d_depth[bit].fetch_sub(1, std::memory_order_relaxed);
  }
}

uint32_t Phase2dFlags() {
  uint32_t flags = 0;
  for (uint32_t bit = 0; bit < 6; ++bit) {
    if (g_phase2d_depth[bit].load(std::memory_order_relaxed) != 0) {
      flags |= 1u << bit;
    }
  }
  return flags;
}



// Guest render thread only: per-object cached debug-path classifier
// (same pattern as ClassifySplineShader).
bool IsCasEditorPs(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return false;
  }
  static std::unordered_map<uint32_t, bool> cache;
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char text[97] = {};
  if (!GuestTryCopy(text, base + obj + 0x54, 96)) {
    return false;  // not cached: unreadable now may be readable later
  }
  text[96] = '\0';
  const char* leaf = std::strrchr(text, '\\');
  leaf = leaf ? leaf + 1 : text;
  const bool cas =
      std::strstr(leaf, "_nis") != nullptr &&
      (std::strncmp(leaf, "cacstamp_", 9) == 0 || std::strncmp(leaf, "cac_", 4) == 0 ||
       std::strncmp(leaf, "defaultcharacter_", 17) == 0);
  if (cache.size() > 4096) {
    cache.clear();
  }
  cache.emplace(obj, cas);
  return cas;
}

void OnSetShader(bool pixel, uint32_t obj) {
  (pixel ? g_cur_ps_obj : g_cur_vs_obj).store(obj, std::memory_order_relaxed);
  // Classify BOTH labels: the hook's pixel/vertex flags are SWAPPED
  // relative to the real shader types (see ClassifySplineShader, which
  // checks both trackers for the same reason); gating on `pixel` alone
  // left the heartbeat dead (the *_nisPS debug paths arrive on the other
  // label), which blackholed the startup-flow editor.
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base != nullptr && IsCasEditorPs(base, obj)) {
    g_cas_ps_last_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           std::memory_order_relaxed);
  }
}

// The CAS editor is ON SCREEN now: its own "_nis" pixel shaders were set
// within the last 0.5 s (see IsCasEditorPs). Consumed by the portrait-pass
// publish gate and the takeover gates; the STARTUP-flow editor's scene is
// small (below warmup_min_items) and can look like a portrait pass, so the
// gates need to know the editor is up.
bool CasEditorHeartbeatFresh() {
  const int64_t last_ns = g_cas_ps_last_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  return now_ns - last_ns < 500'000'000;
}

// Full CAS-editor detection: FE push-state screen id 15 (present in BOTH
// flows, pause: [0,56,63,15], startup new-game: [0,67,15], verified in
// capture) OR the _nis shader heartbeat as backup. Cheap
// (a handful of guarded u32 reads); stateless per frame.
bool CasEditorActive(uint8_t* base) {
  if (CasEditorHeartbeatFresh()) {
    return true;
  }
  constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
  uint32_t mgr = 0, beg = 0, end = 0;
  if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
      GuestTryLoadU32(base, mgr + 0x210, &beg) &&
      GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
      end - beg <= 20 * 16) {
    const uint32_t n = (end - beg) / 20;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t f0 = 0;
      if (GuestTryLoadU32(base, beg + i * 20, &f0) && f0 == 15) {
        return true;
      }
    }
  }
  return false;
}

// Should the emulated sub-framebuffer RTT passes execute right now (the
// menus' suppress_mode -> 0 forcing, see menu_rtt_scope)? The one-shot
// skater-portrait RTT renders are the only reason menus need mode 0, and
// they fire at SCREEN TRANSITIONS (screen open / accepted edit), but
// holding mode 0 for the whole menu context kept the game's postfx chain
// executing emulated at scale every menu frame (full world postfx under
// the pause menu; movie postfx on FMV screens), pacing the pipeline
// against the native renderer. Window = TRUE (mode 0) when:
//  - the FE push-state stack is unreadable (fail open = old behavior), or
//  - it contains any screen id NOT known steady-safe (0 = FE root, 56 =
//    pause root, 24 = FMV, surveyed across captured sessions), or
//  - it CHANGED within the last 3 s (covers unknown screens' entry
//    one-shots and the open-race before the id lands), or
//  - the CAS editor heartbeat is fresh.
// Render thread only (statics). Every stack change logs its ids (capped)
// so unknown portrait screens name themselves in the session log.
bool PortraitRttWindowActive() {
  if (CasEditorHeartbeatFresh()) {
    return true;
  }
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return true;
  }
  constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
  uint32_t mgr = 0, beg = 0, end = 0;
  if (!GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) || mgr == 0 ||
      !GuestTryLoadU32(base, mgr + 0x210, &beg) ||
      !GuestTryLoadU32(base, mgr + 0x214, &end) || beg > end ||
      end - beg > 20 * 16) {
    return true;
  }
  const uint32_t n = (end - beg) / 20;
  uint32_t ids[16] = {};
  bool unknown_screen = false;
  uint64_t sig = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t f0 = 0;
    if (!GuestTryLoadU32(base, beg + i * 20, &f0)) {
      return true;
    }
    ids[i] = f0;
    sig = (sig ^ f0) * 1099511628211ull;
    // Steady-safe screens (no one-shot portrait RTT renders): 0 = FE root,
    // 56 = pause root, 24 = FMV, 17 = pause challenge map (the game FLAPS
    // [0,56]<->[0,17] ~1/s while the map is open, with 17
    // unclassified every flap re-armed the grace window and held mode 0,
    // pinning the map screen at 62-94 fps), 59 = skate-reel browser
    // (measured A/B: the edit-skater flow pushes
    // [0,56,63]->[0,56,63,15] and legitimately needs the window; the reel
    // screen pushes [0,56,59] and its panels are VIDEOS served by the
    // native FMV substitution, not portrait RTTs; the fail-open window
    // there bought only its costs, 73 fps + 167 ms sync-compile stalls).
    if (f0 != 0 && f0 != 56 && f0 != 24 && f0 != 17 && f0 != 59) {
      unknown_screen = true;
    }
  }
  static uint64_t s_last_sig = 0;
  static int64_t s_change_ns = -1;
  static bool s_prev_unknown = false;
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (sig != s_last_sig) {
    s_last_sig = sig;
    // Grace only when an UNSAFE screen is involved; entering one is
    // already covered by presence (fail-open), so the window's real job is
    // the tail after LEAVING one (the accepted-edit portrait re-render
    // fires as the editor pops). Safe<->safe transitions (pause <->
    // challenge map flapping) must not arm it.
    if (unknown_screen || s_prev_unknown) {
      s_change_ns = now_ns;
    }
    s_prev_unknown = unknown_screen;
    static uint32_t s_logs = 0;
    if (s_logs < 64) {
      ++s_logs;
      char buf[128] = "";  // n == 0 leaves the loop unrun; print "[]"
      int off = 0;
      for (uint32_t i = 0; i < n && off < int(sizeof(buf)) - 8; ++i) {
        off += std::snprintf(buf + off, sizeof(buf) - off, "%s%u", i ? "," : "", ids[i]);
      }
      REXLOG_INFO(
          "native-scene: FE stack changed: [{}] (portrait window {}; "
          "steady-safe = 0/56/24/17/59)",
          buf, unknown_screen ? "OPEN - unclassified screen" : "steady");
    }
  }
  if (s_change_ns >= 0 && now_ns - s_change_ns < 3'000'000'000ll) {
    return true;
  }
  return unknown_screen;
}

void OnRenderStateUpload(uint64_t mask, uint32_t bank, uint32_t ptr) {
  (void)mask;
  if (ptr == 0) {
    return;
  }
  // Log each distinct bank id once, the AluConstants analog of discovering
  // 0x4000/0x4400. Tiny lock-free seen-set (at most a handful of banks).
  static std::atomic<uint32_t> seen[8];
  for (auto& slot : seen) {
    uint32_t cur = slot.load(std::memory_order_relaxed);
    if (cur == bank) {
      break;
    }
    if (cur == 0) {
      uint32_t expected = 0;
      if (slot.compare_exchange_strong(expected, bank, std::memory_order_relaxed)) {
        REXLOG_INFO("native-scene: render-state bank id={:#x} ptr={:08X}", bank, ptr);
        break;
      }
      if (expected == bank) {
        break;
      }
    }
  }
  g_rs_bank.store(ptr, std::memory_order_relaxed);
}

void OnSetViewport(uint8_t* base, uint32_t viewport_ptr) {
  if (viewport_ptr == 0) {
    return;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  for (int i = 0; i < 6; ++i) {
    g_cur_viewport[i].store(REX_LOAD_U32(viewport_ptr + i * 4), std::memory_order_relaxed);
  }
}

void OnSetScissor(uint8_t* base, uint32_t rect_ptr) {
  if (rect_ptr == 0) {
    return;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  for (int i = 0; i < 4; ++i) {
    g_cur_scissor[i].store(REX_LOAD_U32(rect_ptr + i * 4), std::memory_order_relaxed);
  }
}

// The character.hair pixel shader keeps the per-character hair color at PS
// c17 (fixed layout for that shader; verified from recorded PS banks;
// c16/c17 hold the dark ambient/diffuse hair color pair while other
// registers carry lighting globals). Used as captured.
void CaptureHairTint(uint8_t* base, DrawItem& item) {
  const uint32_t ps = g_ps_bank.load(std::memory_order_relaxed);
  if (ps == 0) {
    return;
  }
  float rgb[3];
  for (int i = 0; i < 3; ++i) {
    rgb[i] = LoadGuestF32(base, ps + (17 * 4 + i) * 4);
    if (!(rgb[i] >= 0.0f && rgb[i] <= 4.0f)) {
      return;  // implausible bank contents; keep the previous/no tint
    }
  }
  for (int i = 0; i < 3; ++i) {
    item.tint[i] = rgb[i];
  }
  item.tint[3] = 1.0f;
}

// Character-family lighting capture: reads the family-specific rows of the
// PIXEL constant bank into the canonical block the scene PS character branch
// consumes (cbuffer CH at b2). Row maps come from the disassembled Skate 3
// pixel shaders (offline-validated by running the actual ucode per pixel):
//   defaultcharacter (fam 1): light c0, key c6, ambMult c10.w, SH c14..c22
//     scaled by c12.y, exposure c4.z, alpha c13.x.
//   cacstamp/cac_* (fam 2): light c9, key c15, ambMult c19.w, SH c24..c32
//     scaled by c21.y, exposure c13.z, alpha c22.x, diffuse tint c23.
//   livingworld_stamp (fam 3): light c9, key c15, FLAT ambient = c19.w *
//     (0.1, 0.175, 0.3) (shader literal), exposure c13.z, alpha c21.x, stamp
//     recolor tints c22 (red mask) / c23 (blue mask).
//   cac_hair (fam 4): light c4, key c16, ambient c14.w * 0.25, fresnel tint
//     c17 with power c11.w, exposure c8.z, strand-alpha scale c15.x.
//   defaulthair (fam 5): light c4, key c15, ambient c14.w * 0.25, fresnel
//     tint c10 with power c9.z, exposure c8.z, strand-alpha scale c16.x.
//   vehicle (fam 6, character.livingworld_vehicles): light c9, key c15
//     (fresnel power in c15.w), FLAT ambient = c19.w * (0.1, 0.175, 0.3)
//     (same literal as livingworld), exposure c13.z, alpha c20.x (the
//     per-entity spawn/distance fade), phong spec color c16
//     with power c16.w (stored in the unused SH row 0), paint recolor
//     colorize_red c21 / colorize_blue c22 (vehicle.fx: where diffuse green
//     is below the mask threshold, rgb = r * red_tint + b * blue_tint;
//     that is the taxi yellow; validated by executing the captured
//     vehicle_defaultPS offline).
//   vehicle glass (fam 7): same rows; glass tint c18.rgb (zero = the color
//     is reflection-only) in the tintA slot, alpha out = c20.x * c18.w.
// The SH irradiance evaluation is sat(c_base + s*(N.x*r1 + N.y*r2 + N.z*r3)
// + s^2*(NxNz*r4 + NzNy*r5 + NyNx*r6) + (3 s^2 Nz^2 - 1)*r7 + s^2*(Nx^2 -
// Ny^2)*r8); the scale and the -1 are folded into the stored rows so the
// shader evaluates a plain 9-row basis.
//
// Canonical block (18 float4 rows): [0] = light dir + hair fresnel power,
// [1] = key color + exposure, [2] = flat ambient rgb + SH-ambient
// multiplier (hair ambient scalar in w), [3..11] = SH rows, [12] = tintA
// (w = apply), [13] = tintB + strand-alpha scale (fams 1/2: w = the
// material multiplier m_params[0].y instead), [14].x = alpha out,
// [14].y = family (0 = capture failed validation -> legacy shading),
// [14].z = lens-alpha flag, [14].w = rim fresnel power, [15] = key spec
// color + power, [16] = rim spec color + power, [17] = rim light color +
// key-spec fresnel power. The spec/rim rows (defaultcharacter: spec c7,
// rim spec c8, rim c11, fresnel powers c6.w/c5.z; cacstamp: c16/c17/c20,
// c15.w/c14.z [+ the editor row shift]) exist on fams 1/2 only; [15].w
// stays 0 when they fail their range gates and the PS terms vanish.
void CaptureCharLighting(uint8_t* base, DrawItem& item) {
  if (item.char_family == 0) {
    return;
  }
  const uint32_t ps = g_ps_bank.load(std::memory_order_relaxed);
  if (ps == 0) {
    return;
  }
  g_char_attempts.fetch_add(1, std::memory_order_relaxed);
  const auto row = [&](uint32_t r, uint32_t c) {
    return LoadGuestF32(base, ps + (r * 4 + c) * 4);
  };
  // Build locally and commit only on success: the capture can run again on a
  // later draw with the same buffers (the caster-pass bank is stale; its
  // shadowPS touches no PS constants), and a failed refresh must not wipe
  // rows a previous successful capture staged.
  float local[72];
  float* d = local;
  std::memset(local, 0, sizeof(local));
  uint8_t fam = item.char_family;
  // `plus` = register-row shift for the create-a-skater EDITOR compiles of
  // the fam-2 shaders (cacstamp_/cac_*_nisPS + the ropa variants): the whole
  // cacstamp layout sits ONE ROW HIGHER there: light c10, key c16, exposure
  // c14.z, ambMult c20.w, SH c25..c33 scaled by c22.y, alpha c23.x, tint c24
  // (ucode-proven from cac_cloth_nisPS, offline-validated on every fam-2
  // draw in capture: skin tint (0.80,0.68,0.64),
  // key (1.0,0.95,0.9), expo 1.5, unit light at c10). The editor's VS/ropa
  // palette layout is UNCHANGED (flag c7, palette c8, rigid c191 - draw-41
  // VS disasm), and the editor hair compile keeps the gameplay fam-4 rows,
  // so lighting rows are the only editor delta.
  const auto rows_valid = [&](uint8_t f, uint32_t plus, float* light, float* expo,
                              float* key) {
    uint32_t light_r = 9, key_r = 15, expo_r = 13, expo_c = 2;
    switch (f) {
      case 1: light_r = 0; key_r = 6; expo_r = 4; expo_c = 2; break;
      case 2: case 3: case 6: case 7: break;  // defaults above
      case 4: light_r = 4; key_r = 16; expo_r = 8; expo_c = 2; break;
      case 5: light_r = 4; key_r = 15; expo_r = 8; expo_c = 2; break;
    }
    light_r += plus;
    key_r += plus;
    expo_r += plus;
    float norm2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
      light[i] = row(light_r, uint32_t(i));
      if (!(light[i] > -4.0f && light[i] < 4.0f)) return false;
      norm2 += light[i] * light[i];
    }
    // A real light-direction row carries w = 0; the world materials'
    // shadow-transform rows are ALSO unit in xyz but keep the light-space
    // translation (hundreds of meters) in w; reject those banks.
    const float light_w = row(light_r, 3);
    if (!(light_w > -1.0f && light_w < 1.0f)) return false;
    *expo = row(expo_r, expo_c);
    for (int i = 0; i < 3; ++i) {
      key[i] = row(key_r, uint32_t(i));
      if (!(key[i] >= 0.0f && key[i] < 64.0f)) return false;
    }
    // The bank can hold another pass's constants at our capture moment
    // (characters render a shadow-caster pass first); the light-dir norm
    // and exposure gates reject those; the item then keeps its previous /
    // legacy shading.
    return norm2 > 0.25f && norm2 < 2.25f && *expo > 0.25f && *expo < 16.0f;
  };
  float light[3];
  float key[3];
  float expo = 0.0f;
  uint32_t plus = 0;
  if (!rows_valid(fam, 0, light, &expo, key)) {
    // NPC skin (character_skin_defaultPS) shares the "character.skin"
    // attribulator name with the CAC player skin (cacstamp_skin) but uses
    // the DEFAULTCHARACTER register layout; the two banks are mutually
    // exclusive on the light-row position (the other layout's slot holds
    // non-unit data), so a failed fam-2 read retries as fam 1, then as the
    // EDITOR fam-2 layout (+1 row, see `plus` above); the create-a-skater
    // screen's _nis compiles, where both standard maps read junk.
    if (fam == 2 && rows_valid(1, 0, light, &expo, key)) {
      fam = 1;
    } else if (fam == 2 && rows_valid(2, 1, light, &expo, key)) {
      plus = 1;
    } else {
      static std::atomic<uint32_t> rej_log{0};
      const uint32_t n = rej_log.fetch_add(1, std::memory_order_relaxed);
      if (n < 16 || (n & 2047u) == 0) {
        REXLOG_DEBUG(
            "native-scene: char capture REJECTED fam={} light=({:.3f},{:.3f},{:.3f}) "
            "expo={:.3f} key=({:.3f},{:.3f},{:.3f})",
            fam, light[0], light[1], light[2], expo, key[0], key[1], key[2]);
      }
      return;
    }
  }
  d[0] = light[0]; d[1] = light[1]; d[2] = light[2];
  d[4] = key[0]; d[5] = key[1]; d[6] = key[2];
  d[7] = expo;
  if (fam == 1 || fam == 2) {
    const uint32_t sh_base = (fam == 1 ? 14u : 24u) + plus;
    const float s = row((fam == 1 ? 12u : 21u) + plus, 1);
    const float s2 = s * s;
    const float amb_mult = row((fam == 1 ? 10u : 19u) + plus, 3);
    if (!(s > -8.0f && s < 8.0f) || !(amb_mult >= 0.0f && amb_mult < 16.0f)) {
      return;
    }
    d[2 * 4 + 3] = amb_mult;
    for (int r = 0; r < 9; ++r) {
      const float scale = r == 0 ? 1.0f : (r <= 3 ? s : (r == 7 ? 3.0f * s2 : s2));
      for (int c = 0; c < 3; ++c) {
        float v = row(sh_base + uint32_t(r), uint32_t(c)) * scale;
        if (!(v > -64.0f && v < 64.0f)) v = 0.0f;
        d[(3 + r) * 4 + c] = v;
      }
    }
    for (int c = 0; c < 3; ++c) {
      // Fold the (3 s^2 Nz^2 - 1) term's -1 into the base row.
      d[3 * 4 + c] -= row(sh_base + 7u, uint32_t(c));
    }
    if (fam == 2) {
      // CAC diffuse/skin tint (c23; editor c24), multiplies the squared
      // diffuse.
      bool tint_ok = true;
      float tint[3];
      for (int c = 0; c < 3; ++c) {
        tint[c] = row(23u + plus, uint32_t(c));
        tint_ok = tint_ok && tint[c] >= 0.0f && tint[c] < 16.0f;
      }
      if (tint_ok) {
        d[12 * 4 + 0] = tint[0];
        d[12 * 4 + 1] = tint[1];
        d[12 * 4 + 2] = tint[2];
        d[12 * 4 + 3] = 1.0f;
      }
    }
    // Material multiplier m_params[0].y (defaultcharacter c5.y / cacstamp
    // c14.y): the PS multiplies the lit color by it before the tone chain
    // (1.2 on the gameplay banks). Rides the fam-1/2-unused tintB.w row;
    // out-of-range keeps 0 and the PS applies 1.
    {
      const float m0y = row((fam == 1 ? 5u : 14u) + plus, 1);
      if (m0y > 0.25f && m0y < 4.0f) {
        d[13 * 4 + 3] = m0y;
      }
    }
    // Rim light + key/rim phong spec rows (see the header comment; observed
    // gameplay banks: key spec white pow 5, rim spec gray pow 67/255, rim
    // 0.2, fresnel powers 0.2-2.0). All-or-nothing behind range gates;
    // a bank that fails them keeps [15].w at 0 and the PS skips the terms.
    {
      const uint32_t ks_r = (fam == 1 ? 7u : 16u) + plus;
      const uint32_t rs_r = (fam == 1 ? 8u : 17u) + plus;
      const uint32_t rim_r = (fam == 1 ? 11u : 20u) + plus;
      const float kfres = row((fam == 1 ? 6u : 15u) + plus, 3);
      const float rfres = row((fam == 1 ? 5u : 14u) + plus, 2);
      float sr[12];
      bool spec_ok = kfres >= 0.0f && kfres < 512.0f &&
                     rfres >= 0.0f && rfres < 512.0f;
      for (int r = 0; r < 3 && spec_ok; ++r) {
        const uint32_t src = r == 0 ? ks_r : (r == 1 ? rs_r : rim_r);
        for (int c = 0; c < 3; ++c) {
          sr[r * 4 + c] = row(src, uint32_t(c));
          spec_ok = spec_ok && sr[r * 4 + c] >= 0.0f && sr[r * 4 + c] < 64.0f;
        }
        sr[r * 4 + 3] = row(src, 3);
      }
      spec_ok = spec_ok && sr[3] >= 0.5f && sr[3] < 512.0f &&
                sr[7] >= 0.5f && sr[7] < 512.0f;
      if (spec_ok) {
        for (int c = 0; c < 4; ++c) {
          d[15 * 4 + c] = sr[c];
          d[16 * 4 + c] = sr[4 + c];
        }
        d[17 * 4 + 0] = sr[8];
        d[17 * 4 + 1] = sr[9];
        d[17 * 4 + 2] = sr[10];
        d[17 * 4 + 3] = kfres;
        d[14 * 4 + 3] = rfres;
      }
    }
    d[14 * 4 + 0] = std::clamp(row((fam == 1 ? 13u : 22u) + plus, 0), 0.0f, 1.0f);
    // character.alpha accessory (sunglass lens): tell the PS fam-1/2 branch
    // to take alpha from the coverage texture at t4/uv2 (ch_misc.z); the
    // item is routed to the blended sub-pass (see DrawItem::char_alpha).
    if (item.char_alpha && item.hair_alpha_tex != 0) {
      d[14 * 4 + 2] = 1.0f;
    }
  } else if (fam == 3) {
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 0] = amb * 0.1f;
    d[2 * 4 + 1] = amb * 0.175f;
    d[2 * 4 + 2] = amb * 0.3f;
    for (int c = 0; c < 3; ++c) {
      d[12 * 4 + c] = std::clamp(row(22u, uint32_t(c)), 0.0f, 4.0f);
      d[13 * 4 + c] = std::clamp(row(23u, uint32_t(c)), 0.0f, 4.0f);
    }
    d[12 * 4 + 3] = 1.0f;
    d[14 * 4 + 0] = std::clamp(row(21u, 0), 0.0f, 1.0f);
  } else if (fam == 6 || fam == 7) {
    // vehicle.fx body / vehicle_glass.fx windows (row map above). The
    // otherwise-unused SH row 0 carries the phong spec color + power.
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 0] = amb * 0.1f;
    d[2 * 4 + 1] = amb * 0.175f;
    d[2 * 4 + 2] = amb * 0.3f;
    d[3] = std::clamp(row(15u, 3), 1.0f, 64.0f);  // fresnel power c15.w
    for (int c = 0; c < 4; ++c) {
      d[3 * 4 + c] = std::clamp(row(16u, uint32_t(c)), 0.0f, 64.0f);
    }
    if (fam == 6) {
      for (int c = 0; c < 3; ++c) {
        d[12 * 4 + c] = std::clamp(row(21u, uint32_t(c)), 0.0f, 4.0f);
        d[13 * 4 + c] = std::clamp(row(22u, uint32_t(c)), 0.0f, 4.0f);
      }
      d[12 * 4 + 3] = 1.0f;
      // Entity opacity: vehicle_defaultPS ends `max oC0.w, c20.x, c20.x`,
      // the same per-entity constant the glass variant multiplies by its
      // material alpha. The LivingWorld spawn/distance fade rides here;
      // 1.0 once the vehicle is fully faded in.
      d[14 * 4 + 0] = std::clamp(row(20u, 0), 0.0f, 1.0f);
    } else {
      // Glass tint c18.rgb multiplies the ambient/key terms (zero in every
      // capture = reflection-only glass); alpha out = c20.x * c18.w.
      for (int c = 0; c < 3; ++c) {
        d[12 * 4 + c] = std::clamp(row(18u, uint32_t(c)), 0.0f, 4.0f);
      }
      d[12 * 4 + 3] = 1.0f;
      d[14 * 4 + 0] = std::clamp(row(20u, 0) * row(18u, 3), 0.0f, 1.0f);
    }
  } else {
    // Hair: flat ambient scalar + fresnel rim tint, strand-alpha scale.
    const float amb = row(14u, 3) * 0.25f;
    d[2 * 4 + 3] = std::clamp(amb, 0.0f, 4.0f);
    d[3] = std::clamp(row(fam == 4 ? 11u : 9u, fam == 4 ? 3u : 2u), 1.0f, 64.0f);
    const uint32_t fres_r = fam == 4 ? 17u : 10u;
    for (int c = 0; c < 3; ++c) {
      d[13 * 4 + c] = std::clamp(row(fres_r, uint32_t(c)), 0.0f, 8.0f);
    }
    d[13 * 4 + 3] = std::clamp(row(fam == 4 ? 15u : 16u, 0), 0.0f, 4.0f);
    d[14 * 4 + 0] = 1.0f;
  }
  d[14 * 4 + 1] = float(fam);
  // Frame-global character CSM receive biases: one row below the light row
  // in the fam-2 layouts (gameplay light c9 -> biases c8; editor light c10
  // -> biases c9; verified in capture: (0.005,0.014,0.020)
  // with the same values in every char bank). Captured once per frame from
  // any validating fam-2 bank; consumed by the exact 9-tap char shadow
  // sampling (FrameScene::char_shadow_bias).
  if (fam == 2) {
    const uint32_t bias_r = 8u + plus;
    const float bx = row(bias_r, 0);
    const float by = row(bias_r, 1);
    const float bz = row(bias_r, 2);
    if (bx > 0.0f && bx <= by && by <= bz && bz < 0.1f) {
      g_char_shadow_bias[0] = bx;
      g_char_shadow_bias[1] = by;
      g_char_shadow_bias[2] = bz;
      g_char_shadow_bias_frame = g_guest_frame;
    }
  }
  std::memcpy(item.char_rows, local, sizeof(item.char_rows));
  g_char_valid.fetch_add(1, std::memory_order_relaxed);
  // Remember the validated rows per garment for the cross-frame fallback
  // (see g_char_rows_cache). Runaway-growth backstop only; mesh keys are
  // bounded by loaded character content in practice.
  if (g_char_rows_cache.size() > 4096) {
    g_char_rows_cache.clear();
  }
  std::memcpy(g_char_rows_cache[item.mesh].data(), local, sizeof(local));
}

// The game's per-entity spawn/streaming fade. LivingWorld presentation
// entities (NPCs, traffic vehicles, spawned props) publish an opacity:
// 0 through the whole spawn settle (the physics drop after
// CensusMan::SpawnPedestrian/SpawnVehicle), ramping up afterwards and by
// distance (cLivingWorldPresEntity::EvaluateOpacityDistance), which every
// character-family PS writes as its output alpha (the "alpha out" row of
// CaptureCharLighting: peds c21.x, defaultcharacter c13.x, cacstamp c22.x,
// vehicle body c20.x, glass c20.x * c18.w, hair strand * scale). Returns
// that alpha for items whose validated capture carries it, 1.0 otherwise
// (legacy-shaded items keep the old always-opaque behavior).
float CharFadeAlpha(const DrawItem& item) {
  // LW-mapped items: the entity's own
  // opacity is authoritative; it is the exact value the game serves this
  // ctx's shader as output alpha, independent of whether the per-draw row
  // capture validated (or captured a clone's foreign row). Families whose
  // shader COMPOSES the entity fade with another factor (hair strand-scale,
  // vehicle-glass tint alpha) keep their captured value bounded by it.
  if (item.lw_alpha >= 0.0f) {
    const float a = std::clamp(item.lw_alpha, 0.0f, 1.0f);
    switch (item.char_family) {
      case 1:
      case 2:
      case 3:
      case 6:
        return a;
      case 4:
      case 5:
        return item.char_rows[14 * 4 + 1] > 0.0f
                   ? std::min(a, std::clamp(item.char_rows[13 * 4 + 3], 0.0f, 1.0f))
                   : a;
      case 7:
        return item.char_rows[14 * 4 + 1] > 0.0f
                   ? std::min(a, std::clamp(item.char_rows[14 * 4 + 0], 0.0f, 1.0f))
                   : a;
      default:
        break;
    }
  }
  if (item.char_rows[14 * 4 + 1] <= 0.0f) {
    return 1.0f;
  }
  switch (item.char_family) {
    case 1:
    case 2:
    case 3:
    case 6:
    case 7:
      return std::clamp(item.char_rows[14 * 4 + 0], 0.0f, 1.0f);
    case 4:
    case 5:
      // Hair alpha = strand coverage * scale; the scale constant carries the
      // entity fade, so a near-zero scale means the whole garment is
      // invisible this frame.
      return std::clamp(item.char_rows[13 * 4 + 3], 0.0f, 1.0f);
    default:
      return 1.0f;
  }
}

// Copy the staged bone palette (and, for hair, the tint) out of the shadow
// banks into the item. The banks are reused draw to draw, hence the copy.
// The base from BankPaletteBase is refined by +1 for the cloth/morph VS
// layout (extra parameter row before the palette); the mesh's own sample
// vertices projected with the bank's viewproj decide (RefinePaletteBase).
//
// character.cloth_ropa items (Ropa cloth-simulated garments, the player's
// tee) use a VS that BRANCHES on the row in front of the palette (c4
// pre-pass / c7 main-pass; disassembled from a live
// capture): flag.x > 0 skins with the palette one
// register late (c5/c8); flag.x <= 0 means the CPU cloth sim already wrote
// deformed root-local positions into the (dynamic, per-frame) VB and the VS
// ignores palette and blend attributes entirely, applying ONE affine at
// c188 (pre-pass) / c191 (main-pass): 3 column-vector [R | t] rows, same
// packing as palette rows. Skinning the simulated vertices instead renders
// the garment as a mangled ribbon off the body (the distorted-player-shirt
// bug). Offline validation: the rigid rows put 31/31 sampled shirt verts in
// clip at the player's position; the skinned interpretation scores 0/31.
// Transform up to 6 sample vertices of the item by the affine at register m
// (3 column-vector [R | t] rows) and project them with the bank's own
// viewproj (c0..c3). Returns the count of samples inside the clip volume,
// -1 when unscorable. A stale bank (the submit-exit capture can run after
// ANOTHER mesh's draw when this mesh's own draws are deferred) holds some
// other object's matrix at c188/c191; geometry projected with it lands
// far off-clip, while the true matrix scores full (validated offline:
// 31/31 vs 0/31 on an F10 capture).
// *out_front_all (when asked): every sample lands IN FRONT of the
// projection within a loose 6x guard band, the relaxed near-camera
// criterion (a sim-active garment right at the camera clips most of its
// samples out of the tight 1.5x band, and the strict >=8 gate refused the
// CORRECT rigid matrix; same failure mode as the skinned branch).
int ScoreRigidAffine(uint8_t* base, uint32_t bank, uint32_t m, const DrawItem& item,
                     bool* out_front_all = nullptr) {
  if (item.stride == 0) return -1;
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2) return -1;
  float vp[16];
  float rows[12];
  for (int i = 0; i < 16; ++i) {
    vp[i] = LoadGuestF32(base, bank + i * 4);
    if (!(vp[i] > -1e9f && vp[i] < 1e9f)) return -1;
  }
  for (int i = 0; i < 12; ++i) {
    rows[i] = LoadGuestF32(base, bank + (m * 4 + i) * 4);
  }
  constexpr uint32_t kSamples = 6;
  SkinSampleVert sverts[kSamples];
  if (!ReadSkinSamplesGuest(base, item, kSamples, sverts)) {
    return -1;
  }
  int ok = 0;
  int loose = 0;
  int n = 0;
  for (uint32_t s = 0; s < kSamples; ++s) {
    const float* p = sverts[s].p;
    float q[3];
    for (int a = 0; a < 3; ++a) {
      q[a] = rows[a * 4] * p[0] + rows[a * 4 + 1] * p[1] + rows[a * 4 + 2] * p[2] +
             rows[a * 4 + 3];
    }
    float clip[4];
    for (int r = 0; r < 4; ++r) {
      clip[r] = vp[r * 4] * q[0] + vp[r * 4 + 1] * q[1] + vp[r * 4 + 2] * q[2] +
                vp[r * 4 + 3];
    }
    const float aw = std::abs(clip[3]) < 1.0f ? 1.0f : std::abs(clip[3]);
    ++n;
    if (std::abs(clip[0]) <= 1.5f * aw && std::abs(clip[1]) <= 1.5f * aw) {
      ++ok;
    }
    if (clip[3] > 0.0f && std::abs(clip[0]) <= 6.0f * aw &&
        std::abs(clip[1]) <= 6.0f * aw) {
      ++loose;
    }
  }
  if (out_front_all) {
    *out_front_all = n >= 2 && loose == n;
  }
  return n == 0 ? -1 : (ok * 16) / n;
}

// Dense publish-time coherence gate: skin ~32 samples of the LIVE guest VB
// with the item's PUBLISHED palette and require bind-pose spread. The
// capture acceptance gates sample only 6 verts, so a palette whose junk
// rows sit on UNSAMPLED bones (e.g. a staging bank partially overwritten
// by the next entity's rows between this mesh's draw and our capture)
// passes them and stretches the unsampled islands into the map-length-
// ribbon flash. 32 evenly-spaced samples cover the islands the 6-vert
// gates miss. Returns the measured spread via out_spread for the
// diagnosis log.
bool PublishedPaletteSane(uint8_t* base, const DrawItem& item,
                          float* out_spread) {
  *out_spread = 0.0f;
  if (item.stride == 0 || item.bones.size() < 12 || item.bw_offset == 0 ||
      item.bi_offset == 0 || item.vb_addr == 0) {
    return true;
  }
  const float bind_diag = BindDiag(item);
  // 6x, NOT the capture gates' 3x: this judges already-ACCEPTED palettes,
  // where the observed junk measures 183-405 while legit articulation on
  // small meshes (a 0.5 m accessory mid-stride) reached 3.1-3.3x and
  // tripped a 3x bound repeatedly (log 1285 mesh=436E7210, spread 1.6 vs
  // 1.56 - real world-space palette, needlessly healed/frozen).
  const float max_spread = std::max(6.0f * bind_diag, bind_diag + 2.0f);
  constexpr uint32_t kSamples = 32;
  SkinSampleVert sverts[kSamples];
  if (!ReadSkinSamplesGuest(base, item, kSamples, sverts)) {
    return true;  // unsupported position format: nothing to judge
  }
  float spread = 0.0f;
  if (SkinnedSpreadHostRows(sverts, kSamples, item.bones.data(), item.bones.size(),
                            /*min_n=*/4, /*garbage_fails=*/false, &spread) != 1) {
    return true;  // nothing to judge
  }
  *out_spread = spread;
  return spread <= max_spread;
}

// Returns false when the bank could not be consumed for this item (ropa
// rigid matrix implausible or off-clip = stale bank); the caller must
// leave/mark the item pending so a later matching draw re-captures it.
bool CaptureSkinnedState(uint8_t* base, uint32_t bank, uint32_t palette_base,
                         DrawItem& item) {
  // Refuse captures staged by AUX perspective passes (skater-portrait RTTs):
  // the portrait pass draws the SAME meshes as the on-screen player at the
  // off-map portrait stage, and accepting its palette poisons the mesh/ctx
  // stores (see BankIsAuxPerspective). The item stays pending; a later
  // main-view draw of these buffers resolves it, and portrait-only frames
  // publish nothing (BuildFrameScene's aux-view gate).
  if (BankIsAuxPerspective(base, bank)) {
    static std::atomic<uint64_t> s_aux_refused{0};
    const uint64_t n = s_aux_refused.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 || (n & 4095u) == 0) {
      REXLOG_INFO(
          "native-scene: skinned capture refused - aux perspective pass "
          "(portrait RTT) mesh={:08X} (n={})",
          item.mesh, n);
    }
    return false;
  }
  if (item.ropa && palette_base != 0) {
    const bool main_pass = palette_base >= 7;
    const uint32_t flag_reg = main_pass ? 7u : 4u;
    const float flag_x = LoadGuestF32(base, bank + (flag_reg * 4) * 4);
    // Diagnosis logging (rate-limited): the flag/matrix registers were
    // verified against an emulated-mode F10 capture; this confirms what the
    // live native-mode banks actually hold at OUR capture moments.
    static std::atomic<uint32_t> ropa_log_count{0};
    const uint32_t ln = ropa_log_count.fetch_add(1, std::memory_order_relaxed);
    if (ln < 8 || (ln & 1023u) == 0) {
      const uint32_t m = main_pass ? 191u : 188u;
      REXLOG_INFO(
          "native-scene: ropa mesh={:08X} vb={:08X} base={} score={} "
          "flag=({:.3f},{:.3f},{:.3f},{:.3f}) "
          "m[c{}]=({:.3f},{:.3f},{:.3f},{:.2f})({:.3f},{:.3f},{:.3f},{:.2f})({:.3f},{:.3f},{:.3f},{:.2f})",
          item.mesh, item.vb_obj, palette_base, ScoreRigidAffine(base, bank, m, item),
          LoadGuestF32(base, bank + (flag_reg * 4 + 0) * 4),
          LoadGuestF32(base, bank + (flag_reg * 4 + 1) * 4),
          LoadGuestF32(base, bank + (flag_reg * 4 + 2) * 4),
          LoadGuestF32(base, bank + (flag_reg * 4 + 3) * 4), m,
          LoadGuestF32(base, bank + ((m + 0) * 4 + 0) * 4),
          LoadGuestF32(base, bank + ((m + 0) * 4 + 1) * 4),
          LoadGuestF32(base, bank + ((m + 0) * 4 + 2) * 4),
          LoadGuestF32(base, bank + ((m + 0) * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 0) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 1) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 2) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 0) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 1) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 2) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 3) * 4));
    }
    if (flag_x > 0.0f) {
      // Sim inactive: the palette sits one register late (c5/c8). The LAYOUT
      // is exact, but the BANK can still be foreign, the same stale-bank
      // hazard the rigid branch below scores against (the flag itself was
      // read from the foreign bank, so a positive x proves nothing). Blind
      // acceptance here staged whatever the foreign bank held as an 84-bone
      // palette; when the garment's sim was actually ACTIVE (skating NPCs
      // toggle with distance/activity) that skinned the sim-deformed
      // vertices, the mangled map-length ribbon (ScoreRigidAffine's 0/31
      // interpretation), shadows matching because the caster pass shares the
      // item. Gate through the same sample-projection acceptance as every
      // other palette; on refusal the item stays pending (post-draw fixup /
      // ropa state rescue). The game writes exactly 1.0 into the flag row's
      // x for sim-inactive (observed on every live capture, incl. the
      // (1,junk,junk,junk) NPC variant); a bit-exact 1.0 is the structural
      // proof that unlocks the relaxed near-camera acceptance; any other
      // positive x (a bone row of some other layout) keeps the strict gate.
      palette_base = RefinePaletteBase(base, bank, main_pass ? 8u : 5u, item,
                                       /*structural_guess=*/flag_x == 1.0f);
      if (palette_base == 0) {
        g_ropa_stale.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    } else {
      // Dropped-garment gate: a garment the game removed (numCloth back to
      // 0, VB objects freed) must never publish rigid; whatever this bank
      // holds is foreign (the game draws the ctx skinned now), and a rigid
      // publish would pair a live world with our retained stale drape.
      if (skate3::native_entity::RopaGarmentDropped(base, item.ctx,
                                                    item.vb_obj)) {
        g_ropa_stale.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      // The garment's rigid world is the accepted draw-time bank matrix
      // (c188/c191): the game stages it tick-exact with the deformed VB
      // content and with the body palettes packed at EndJobs. (A guest-side
      // entity L2W read at StartJobs predates the tick's locomotion update;
      // serving that as the draw world rendered the whole shirt one guest
      // tick behind the body, a constant velocity-proportional drape lag.)
      uint32_t m = main_pass ? 191u : 188u;
      float rows[12];
      const auto read_rows = [&](uint32_t reg) -> bool {
        bool ok = true;
        for (int r = 0; r < 3 && ok; ++r) {
          float n = 0.0f;
          for (int i = 0; i < 4; ++i) {
            const float f = LoadGuestF32(base, bank + ((reg + r) * 4 + i) * 4);
            if (!(f > -1e7f && f < 1e7f)) {
              ok = false;
              break;
            }
            rows[r * 4 + i] = f;
            if (i < 3) n += f * f;
          }
          ok = ok && n > 0.0025f && n < 400.0f && rows[r * 4 + 3] > -20000.f &&
               rows[r * 4 + 3] < 20000.f;
        }
        return ok;
      };
      const auto tail_row_at = [&](uint32_t reg) -> bool {
        const float x = LoadGuestF32(base, bank + (reg * 4 + 0) * 4);
        const float y = LoadGuestF32(base, bank + (reg * 4 + 1) * 4);
        const float z = LoadGuestF32(base, bank + (reg * 4 + 2) * 4);
        const float w = LoadGuestF32(base, bank + (reg * 4 + 3) * 4);
        return std::fabs(x) <= 1e-4f && std::fabs(y) <= 1e-4f &&
               std::fabs(z) <= 1e-4f && std::fabs(w - 1.0f) <= 1e-4f;
      };
      // The passes stage the matrix as a 4-row block each: main at
      // c191..c194, shadow at c188..c191 (3 affine rows + a (0,0,0,1)
      // homogeneous tail). The tail at m+3 is REQUIRED: it is the
      // structural proof a matrix block lives at m. Without it the read is
      // mid-palette; an 84-bone body bank's palette (c5/c8 onward) covers
      // c188..c194 with bone matrices, and a bone matrix (world *
      // inverse-bind) is a plausible, high-scoring affine that renders the
      // garment rotated by the bind rotation and offset meters from the
      // body while tracking the skater.
      bool plausible = read_rows(m) && tail_row_at(m + 3);
      // The shadow block's tail lands exactly on c191 = the main block's
      // first row. A main-pass read that finds that tail at c191 is seeing
      // the post-shadow bank state, where c188..c190 still hold the same
      // tick's matrix: consume that copy instead of refusing the capture.
      if (!plausible && main_pass && tail_row_at(191u) && read_rows(188u)) {
        m = 188u;
        plausible = true;
      }
      bool front_all = false;
      const int rigid_score =
          plausible ? ScoreRigidAffine(base, bank, m, item, &front_all) : -1;
      // Relaxed near-camera acceptance mirrors the skinned branch: a
      // sim-active garment right at the camera clips most samples out of
      // the strict band, but a stale/foreign matrix throws them behind the
      // projection or far outside even the loose band.
      if (plausible && (rigid_score >= 8 || front_all)) {
        if (rigid_score < 8) {
          g_ropa_relaxed.fetch_add(1, std::memory_order_relaxed);
        }
        // Primary entity-world serve: the accepted bank block is a
        // dereference of the owner entity's m_MatLtoWTrans, so the entity
        // field is value-identical here (bit-compare tripwire counts any
        // drift as wprim divergence) and stays correct through bank states
        // the acceptance can't see. Bank rows remain the fallback.
        const float* src_rows = rows;
        float ent_rows[12];
        if (REXCVAR_GET(skate3_native_render_scene_entity_ropa_world_primary) &&
            skate3::native_entity::ServeRopaWorld(base, item.ctx, item.vb_obj,
                                                  ent_rows)) {
          skate3::native_entity::NoteAcceptedWorldCompare(rows, ent_rows);
          src_rows = ent_rows;
        }
        // Column-vector [R | t] rows -> item.world (row-vector, t in row 3).
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            item.world[i * 4 + j] = src_rows[j * 4 + i];
          }
          item.world[i * 4 + 3] = 0.0f;
          item.world[12 + i] = src_rows[i * 4 + 3];
        }
        item.world[15] = 1.0f;
        item.skinned = false;
        item.bones.clear();
        g_ropa_rigid.fetch_add(1, std::memory_order_relaxed);
        // Identity-store observer: the accepted bank block should be a
        // dereference of the owner entity's bound world pointer (+416
        // transposed for skater-class binds, +352 raw for CAC); the
        // ident[] w416/w352 telemetry proves or refutes that live.
        skate3::native_entity::ObserveRopaWorld(base, item.ctx, rows);
        if (item.hair) {
          CaptureHairTint(base, item);
        }
        CaptureCharLighting(base, item);
        return true;
      }
      // Implausible or off-clip matrix: the bank belongs to another mesh
      // (the submit-exit capture runs after whatever draw happened to be
      // inline). Before refusing, try the identity store: the accepted
      // bank block is a bit-exact dereference of the owner entity's
      // m_MatLtoWTrans (+416), so
      // the entity field serves the same matrix through every bank state
      // the pass sequence can leave behind (post-shadow tail clobber,
      // foreign bank, torn state). Geometry only; the PS lighting rows
      // stay on their caches, since the bank is not proven ours here.
      float ent_rows[12];
      if (skate3::native_entity::ServeRopaWorld(base, item.ctx, item.vb_obj,
                                                ent_rows)) {
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            item.world[i * 4 + j] = ent_rows[j * 4 + i];
          }
          item.world[i * 4 + 3] = 0.0f;
          item.world[12 + i] = ent_rows[i * 4 + 3];
        }
        item.world[15] = 1.0f;
        item.skinned = false;
        item.bones.clear();
        return true;
      }
      g_ropa_stale.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  } else {
    palette_base = RefinePaletteBase(base, bank, palette_base, item);
    if (palette_base == 0) {
      // The bank's palette provably does not skin this mesh into the bank's
      // own view (foreign/stale bank): refuse; the caller keeps the item
      // pending and the post-draw (ib,vb) fixup re-captures on a real draw.
      return false;
    }
  }
  constexpr uint32_t kPaletteFloats = 84 * 12;  // c4..c255 = up to 84 bones
  item.bones.resize(kPaletteFloats);
  for (uint32_t i = 0; i < kPaletteFloats; ++i) {
    const float f = LoadGuestF32(base, bank + (palette_base * 4 + i) * 4);
    item.bones[i] = (f > -1e7f && f < 1e7f) ? f : 0.0f;
  }
  if (item.hair) {
    CaptureHairTint(base, item);
  }
  CaptureCharLighting(base, item);
  return true;
}

uint64_t DrawSequence() { return g_draw_seq.load(std::memory_order_relaxed); }

uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx, bool world_path,
                             bool drew_inside) {
  if (!SceneEnabled()) {
    return 0;
  }
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0) {
    return 0;
  }
  // Streaming can revoke any pointer in the ctx/record/mesh chain between
  // the game queueing the draw and this walk; recover raw-load read faults
  // for the whole capture (POSIX; no-op on Windows).
  GuestReadRecoveryScope guest_read_recovery(base);
  // Items drawn inside an AUX perspective pass (skater-portrait RTTs) never
  // enter the frame at all: they share (ib,vb) buffers with the on-screen
  // player, so letting them sit PENDING lets them steal the player's own
  // post-draw fixups (FIFO oldest-pending) and publish a ghost at the
  // player's pose. drew_inside guarantees the bank is the pass's own, so
  // its c0..c3 viewproj identifies the pass (see BankIsAuxPerspective).
  if (drew_inside && BankIsAuxPerspective(base, bank)) {
    return 0;
  }
  // Guest-thread capture cost telemetry (folded per frame in BuildFrameScene).
  const auto perf_t0 = PerfClock::now();
  struct PerfFold {
    PerfClock::time_point t0;
    ~PerfFold() {
      g_capture_frame_ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
              .count());
    }
  } perf_fold{perf_t0};
  if (world_path) {
    // Cheap pre-check so the world path (~800 calls/frame) only pays the
    // full capture for the meshes that need per-draw transforms: skinned
    // (LOD pedestrians, jointed props) and rigid MODEL-SPACE props (position
    // fmt 32/26). Movable props (vending machines) can reach the frame ONLY
    // through the sort lists, rendered as absolute world geometry they
    // collapse at the origin. Absolute meshes (fmt 57) stay on the identity
    // world path.
    const uint32_t record = REX_LOAD_U32(ctx);
    if (!GuestReadableApprox(base, record)) return 0;
    const uint32_t mesh = REX_LOAD_U32(record);
    if (!GuestReadableApprox(base, mesh)) return 0;
    const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
    if (!GuestReadableApprox(base, vdesc)) return 0;
    const uint32_t num_elements = REX_LOAD_U16(vdesc + 8);
    if (num_elements == 0 || num_elements > 32) return 0;
    bool have_bw = false;
    bool have_bi = false;
    uint32_t pos_fmt = 0;
    bool have_pos = false;
    for (uint32_t i = 0; i < num_elements; ++i) {
      const uint32_t e = vdesc + 0x10 + i * 16;
      if (REX_LOAD_U16(e) != 0) continue;
      const uint32_t usage = REX_LOAD_U8(e + 9);
      if (usage == 0 && !have_pos) {
        pos_fmt = REX_LOAD_U32(e + 4) & 0x3F;
        have_pos = true;
      }
      if (usage == 1 && (REX_LOAD_U32(e + 4) & 0x3F) == 6) have_bw = true;
      if (usage == 2 && (REX_LOAD_U32(e + 4) & 0x3F) == 6) have_bi = true;
    }
    const bool skinned = have_bw && have_bi;
    const bool model_space = pos_fmt == 32 || pos_fmt == 26;
    if (!skinned && !model_space) {
      return 0;
    }
  }
  DrawItem item;
  if (!BuildItemGeometry(base, ctx, item)) {
    return 0;
  }
  item.ctx = ctx;  // identity key for the palette serve / entity store
  // The bank only provably holds THIS mesh's constants when the last draw
  // that flushed it bound this mesh's buffers (see g_last_draw_ibvb);
  // `drew_inside` alone also accepted banks left by another entity's inline
  // draws while this mesh's own draws were deferred (the walking-vehicle /
  // origin-vending-machine captures). Deferring those to the post-draw
  // (ib,vb) fixup pairs the state with the mesh's own real draw.
  const bool own_draw_last =
      drew_inside && g_last_draw_ibvb.load(std::memory_order_relaxed) ==
                         ((uint64_t(item.ib_obj) << 32) | item.vb_obj);
  if (drew_inside && !own_draw_last) {
    g_capture_foreign_bank.fetch_add(1, std::memory_order_relaxed);
  }
  // Rigid transform: deferred (multi-pass) rigid props draw later; the
  // bank belongs to some earlier mesh, and a leftover identity matrix at c4
  // VALIDATES as a plausible world (verified from recorded draw streams:
  // 4 of 6 vending-machine clones captured exact identity and rendered
  // invisibly at the origin). Defer those to the post-draw fixup, like
  // skinned palettes.
  if (!item.skinned) {
    if (!world_path && item.ctx != 0) {
      // Mark the mesh as dynamically dispatched (see g_dyn_dispatch_meshes):
      // far clones of it arriving through the static sort lists need their
      // instance matrix, not the world-item identity.
      if (g_dyn_dispatch_meshes.size() > 16384) {
        g_dyn_dispatch_meshes.clear();
      }
      g_dyn_dispatch_meshes[item.mesh] = g_guest_frame;
    }
    // The instance's own matrix (ReadCtxInstanceWorld) outranks the bank:
    // own_draw_last only proves SOME clone of this mesh drew last (clones
    // share (ib,vb)), so under batched dispatch the bank world routinely
    // belongs to another clone - pieces visibly swap placements and flip
    // per frame (the park-editor shuffle). The bank still wins while it
    // AGREES with the instance matrix (movables: the bank is the exact
    // value the pass consumed this frame); a disagreement beyond small
    // motion is the wrong-clone signature.
    float ctx_world[16];
    const bool have_ctx_world =
        ReadCtxInstanceWorld(base, item.ctx, ctx_world);
    // Aux perspective passes (portrait RTTs) stage worlds at the off-map
    // portrait stage; defer like a foreign bank (see BankIsAuxPerspective).
    const bool bank_ok = own_draw_last && !BankIsAuxPerspective(base, bank) &&
                         BankRigidWorld(base, bank, item.world);
    if (bank_ok && have_ctx_world) {
      // Translation-only trigger: wrong-clone worlds sit meters apart;
      // physics-animated props (dumpsters) can carry live ROTATION in the
      // bank that the instance matrix trails, so same-spot rotation
      // deltas keep the bank.
      const float dx = item.world[12] - ctx_world[12];
      const float dy = item.world[13] - ctx_world[13];
      const float dz = item.world[14] - ctx_world[14];
      if (dx * dx + dy * dy + dz * dz > 0.0625f) {
        std::memcpy(item.world, ctx_world, sizeof(ctx_world));
        g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (have_ctx_world) {
      std::memcpy(item.world, ctx_world, sizeof(ctx_world));
      g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
    } else if (!bank_ok) {
      item.pending = true;
      g_rigid_pending.fetch_add(1, std::memory_order_relaxed);
    }
    if (!item.pending && item.ctx != 0) {
      if (g_rigid_world_cache.size() > 32768) {
        g_rigid_world_cache.clear();  // map-change growth backstop
      }
      RigidWorldCache& rc = g_rigid_world_cache[item.ctx];
      rc.mesh = item.mesh;
      std::memcpy(rc.world, item.world, sizeof(rc.world));
      rc.frame = g_guest_frame;
    }
  }

  if (item.skinned) {
    // Copy the whole possible palette span (c4..c255 = 84 bones) verbatim:
    // 3 float4 rows per bone, column-vector affine [R | t], model space
    // directly to world space (verified live: bone translations match the
    // camera focus). The shader applies rows with explicit dot products,
    // sidestepping HLSL matrix packing. Registers beyond the mesh's real
    // bone count hold stale bank data but are never indexed. World stays
    // identity.
    //
    // The bank only holds this mesh's palette at c4 if its own draws ran
    // inside the call AND used the pre-pass layout. Multi-pass (deferred)
    // meshes draw later, and main-pass-layout banks have the camera at c4;
    // in both cases leave the palette pending for the post-draw fixup.
    const uint32_t effect_list = REX_LOAD_U32(ctx + kCtxEffectList);
    uint32_t passes = 1;
    if (GuestReadableApprox(base, effect_list)) {
      passes = REX_LOAD_U32(effect_list + kEffectListPassCount);
    }
    const uint32_t palette_base = BankPaletteBase(base, bank);
    // World-path captures come from the sort-list hook BEFORE any of the
    // mesh's draws ran; the bank belongs to some other mesh; always defer
    // to the post-draw fixup. Same when the last draw inside the submit
    // call was not this mesh's own (deferred mesh, foreign inline draws):
    // a stale/foreign bank can still hold plausible bone rows.
    item.pending = world_path || !own_draw_last || (passes > 1 && passes < 16) ||
                   palette_base == 0;
    if (!item.pending) {
      if (CaptureSkinnedState(base, bank, palette_base, item)) {
        g_palette_snapshots.fetch_add(1, std::memory_order_relaxed);
        item.dbg_src = 1;
        item.caster_bank = BankIsOrtho(base, bank);
      } else {
        item.pending = true;
      }
    }
    g_skinned_items.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_recording.load(std::memory_order_relaxed)) {
    // Preserve the buffer payload for offline decode (streaming arenas are
    // recycled long before the end-of-window memory snapshot).
    std::lock_guard<std::mutex> lock(g_record_mutex);
    const uint64_t key = uint64_t(item.vb_addr) * 1099511628211ull ^ item.fingerprint;
    if (item.vb_bytes <= (1u << 20) && g_recorded_buffer_bytes < (512u << 20) &&
        g_recorded_buffer_keys.insert(key).second) {
      RecordedBuffer buf;
      buf.vb_addr = item.vb_addr;
      buf.ib_addr = item.ib_addr;
      buf.fingerprint = item.fingerprint;
      buf.vb.resize(item.vb_bytes);
      std::memcpy(buf.vb.data(), base + item.vb_addr, item.vb_bytes);
      buf.ib.resize(size_t(item.ib_count) * 2);
      std::memcpy(buf.ib.data(), base + item.ib_addr, buf.ib.size());
      g_recorded_buffer_bytes += buf.vb.size() + buf.ib.size();
      g_recorded_buffers.push_back(std::move(buf));
    }
  }
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  // Record shadow-pass submissions per ctx (even refused/pending captures):
  // the game's per-piece caster list = exactly the ctxs it submits through
  // ortho banks; the native atlas pass mirrors it (see g_frame_ortho_ctx).
  // Stamp the item copy too; the game draws its shadow passes FIRST, so
  // every later copy (main pass, fixups, rescues, LW store ingest) carries
  // the flag; a final pre-publish pass ORs the set over merged items.
  const bool ortho_submit = !world_path && BankIsOrtho(base, bank);
  if (ortho_submit) {
    g_frame_ortho_ctx.insert(ctx);
  }
  item.shadow_caster =
      ortho_submit || (ctx != 0 && g_frame_ortho_ctx.count(ctx) != 0);
  item.dyn_entity = true;
  g_frame_dynitems.push_back(std::move(item));
  const size_t index = g_frame_dynitems.size() - 1;
  if (g_frame_dynitems[index].pending ||
      (g_frame_dynitems[index].skinned && g_frame_dynitems[index].caster_bank &&
       !g_frame_dynitems[index].ropa)) {
    // Pending items wait for their first fixup; caster-bank captures stay
    // registered so the mesh's later main-pass draw REFRESHES the palette
    // (see DrawItem::caster_bank, stale wheel spin in the shadow banks).
    const DrawItem& d = g_frame_dynitems[index];
    g_frame_pending_by_buffers.emplace((uint64_t(d.ib_obj) << 32) | d.vb_obj, index);
  } else if (g_frame_dynitems[index].char_family != 0 &&
             g_frame_char_refresh.size() < 256) {
    // Character captured at submit-exit: the PS bank there can predate this
    // character's main pass; refresh the lighting rows on its later draws
    // (last successful capture wins).
    const DrawItem& d = g_frame_dynitems[index];
    g_frame_char_refresh.emplace((uint64_t(d.ib_obj) << 32) | d.vb_obj, index);
  }
  return uint32_t(index + 1);
}

uint32_t CaptureClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6,
                          uint32_t r7, uint32_t* out_key) {
  (void)r4;
  (void)r7;
  if (!SceneEnabled() || r6 != 0x80000000u ||
      !REXCVAR_GET(skate3_native_render_scene_quadlists)) {
    return 0;
  }
  // Quad-list draw: stream 0 is a dynamic ping-pong object whose vertex
  // fetch block (+0x18 dword0 = base|flags, +0x20 = size in bytes) points at
  // the CPU-simulated vertices for this frame: stride 24 = {float3 world
  // position (BE), packed normal, float2 uv}, quad-list topology (verified
  // offline from recorded payloads). NOTE: every capture examined so far is
  // a PARTICLE system (disjoint 2-4cm sprites), which is why rendering is
  // gated off by default; see the cvar.
  const uint32_t vb_obj = g_cur_vb.load(std::memory_order_relaxed);
  if (!GuestReadableApprox(base, vb_obj)) {
    return 0;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t addr = REX_LOAD_U32(vb_obj + 0x18) & 0xFFFFFFFC;
  const uint32_t size = REX_LOAD_U32(vb_obj + 0x20);
  constexpr uint32_t kStride = 24;
  if (addr < 0x10000 || size < kStride * 4 || size > (1u << 20) || size % kStride != 0) {
    return 0;
  }
  // The garment's live vertices start at the buffer head; r5 is the live
  // vertex count (verified offline: the zero-fill run starts exactly at r5
  // for every garment). Ring slack past it must not be drawn.
  uint32_t verts = size / kStride;
  if (r5 >= 4 && r5 <= verts) {
    verts = r5;
  }
  const uint32_t quads = verts / 4;
  if (quads == 0) {
    return 0;
  }
  const uint32_t start = 0;

  const uint32_t garment_key = vb_obj ^ (start * 2654435761u);
  DrawItem item{};
  item.mesh = garment_key;
  item.vb_obj = vb_obj;
  item.ib_obj = 0;
  item.vb_addr = addr;
  item.vb_bytes = verts * kStride;
  item.ib_addr = 0;
  item.ib_count = quads * 6;
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  item.pos_offset = 0;
  item.pos_fmt = 57;  // float3
  item.uv_offset = 16;
  item.uv_fmt = 38;  // float2
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.stride = kStride;
  item.skinned = false;
  item.pending = false;
  item.cloth_quads = true;
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  for (int axis = 0; axis < 3; ++axis) {
    item.bbox_min[axis] = -20000.0f;
    item.bbox_max[axis] = 20000.0f;
  }
  item.draws.push_back({4, 0, 0, quads * 6});

  // Content fingerprint (simulation output changes every frame -> the
  // renderer re-decodes each frame).
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(addr);
  mix(item.vb_bytes);
  for (uint32_t k = 0; k < 16; ++k) {
    const uint32_t off = uint32_t(uint64_t(item.vb_bytes - 8) * k / 15u) & ~7u;
    mix(REX_LOAD_U64(addr + off));
  }
  item.fingerprint = h;

  // Distinct draw ranges within one ring buffer are distinct garments.
  *out_key = garment_key;
  item.dyn_entity = true;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  g_frame_dynitems.push_back(std::move(item));
  return uint32_t(g_frame_dynitems.size());
}

namespace {

// Guest shader objects carry their compiled-source debug path at +0x54
// ("D:\P4\xbox2-ww-f\...\spline_darkenPS.updb"); the spline renderer's pixel
// shaders identify the in-world neon guide elements. Cached per object;
// the current pixel/vertex object trackers are checked both ways because the
// hook labels are swapped relative to the real shader types.
uint32_t ClassifySplineShader(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  static std::mutex mu;
  static std::unordered_map<uint32_t, uint32_t> cache;
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char path[97] = {};
  uint32_t kind = 0;
  if (GuestTryCopy(path, base + obj + 0x54, 96)) {
    path[96] = '\0';
    if (std::strstr(path, "spline_darken") != nullptr) {
      kind = 1;
    } else if (std::strstr(path, "spline_default") != nullptr) {
      kind = 2;
    }
  }
  cache.emplace(obj, kind);
  return kind;
}

}  // namespace

void OnSetIndices(uint32_t ib_obj) { g_cur_ib.store(ib_obj, std::memory_order_relaxed); }

void OnSetStreamSource(uint32_t stream, uint32_t vb_obj, uint32_t offset, uint32_t stride) {
  if (stream == 0) {
    g_cur_vb.store(vb_obj, std::memory_order_relaxed);
    g_cur_vb_offset.store(offset, std::memory_order_relaxed);
    g_cur_vb_stride.store(stride, std::memory_order_relaxed);
  }
  if (stream < 4) {
    g_cur_streams[stream][0].store(vb_obj, std::memory_order_relaxed);
    g_cur_streams[stream][1].store(offset, std::memory_order_relaxed);
    g_cur_streams[stream][2].store(stride, std::memory_order_relaxed);
  }
}

void OnDrawDone(uint8_t* base, uint32_t func, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7) {
  g_draw_seq.fetch_add(1, std::memory_order_relaxed);
  const uint32_t flags2d = Phase2dFlags();
  if (flags2d != 0) {
    g_draws_2d.fetch_add(1, std::memory_order_relaxed);
  }
  // Last-draw provenance for the submit-exit capture (see g_last_draw_ibvb):
  // only an indexed 3D draw leaves a bank the palette/world capture may
  // trust, keyed by the buffers it bound.
  g_last_draw_ibvb.store(
      (func == 0 && flags2d == 0)
          ? ((uint64_t(g_cur_ib.load(std::memory_order_relaxed)) << 32) |
             g_cur_vb.load(std::memory_order_relaxed))
          : 0,
      std::memory_order_relaxed);
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0) {
    return;
  }
  // The bank / PS-bank / pending-fixup reads below are raw loads; recover
  // read faults for the whole post-draw path (POSIX; no-op on Windows).
  GuestReadRecoveryScope guest_read_recovery(base);
  // Fog rows: grab c5/c6 from the first 3D draw whose bank c4 row matches
  // the last built scene's camera (main-pass layout; tolerant of one frame
  // of camera motion). See g_fog_rows. The same camera-keyed draws also
  // carry the frame's CSM shadow constants in the PIXEL bank (c0..c8,
  // pass-global on environment-family draws), captured here with an
  // independent done-flag: the first main-pass draw can be a character/tree
  // whose PS allocates differently (rejected by the sanity gate below).
  if ((!g_fog_frame_done || !g_shadow_frame_done || !g_sky_frame_done ||
       !g_tree_frame_done || !g_proxy_frame_done || !g_dynobj_frame_done ||
       !g_water_frame_done || !g_ocean_frame_done ||
       !g_oceanrefl_frame_done || !g_scroll_frame_done) &&
      func == 0 && flags2d == 0 && SceneEnabled() &&
      (g_fog_cam[0] != 0.0f || g_fog_cam[1] != 0.0f || g_fog_cam[2] != 0.0f)) {
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    // The sky draw's bank: c4.xz == camera, c4.y = the fixed level sky
    // elevation (dy ~ +160). dy > 50 excludes normal draws (dy == 0) and
    // any reflection pass (y mirrored DOWN); the bound keeps out garbage.
    if (!g_sky_frame_done && dx * dx + dz * dz < 25.0f && dy > 50.0f && dy < 2000.0f) {
      g_sky_height = LoadGuestF32(base, bank + 17 * 4);
      g_sky_have = true;
      g_sky_frame_done = true;
      // Same draw, PIXEL bank (sky_defaultPS layout, verified in
      // capture): c0.xyz = g_vLightDir (unit vector
      // toward the sun), c4.x = sun angular scale (m_params[0].x, 0.75),
      // c4.y = sky pre-tone multiplier (m_params[0].y, 0.35), c3.x = scene
      // exposure (g_envattributes[2].x, 2.5). Sanity-gated: a stale bank
      // here would put the sun glow in a wrong spot or blow out the tone.
      const uint32_t sky_ps = g_ps_bank.load(std::memory_order_relaxed);
      if (sky_ps != 0) {
        float dir[3];
        for (int k = 0; k < 3; ++k) {
          dir[k] = LoadGuestF32(base, sky_ps + uint32_t(k) * 4);
        }
        const float n2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
        const float scale = LoadGuestF32(base, sky_ps + (4 * 4 + 0) * 4);
        const float mult = LoadGuestF32(base, sky_ps + (4 * 4 + 1) * 4);
        const float expo = LoadGuestF32(base, sky_ps + (3 * 4 + 0) * 4);
        if (n2 > 0.8f && n2 < 1.2f && scale > 1e-3f && scale < 100.0f &&
            mult > 1e-3f && mult < 100.0f && expo > 0.01f && expo < 100.0f) {
          g_sky_sun[0] = dir[0];
          g_sky_sun[1] = dir[1];
          g_sky_sun[2] = dir[2];
          g_sky_sun[3] = scale;
          g_sky_sun[4] = mult;
          g_sky_sun[5] = expo;
          g_sky_sun_have = true;
        }
      }
    }
    if (dx * dx + dy * dy + dz * dz < 25.0f) {
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      // POSITIVE family check for the receiver-row capture, by shader debug
      // path: the value gates below cannot fully discriminate;
      // flowingwater_defaultPS keeps the ENTIRE baseenvironment receiver
      // layout (CSM rows, sun c6, camera c7, dim c8) but its material
      // multiplier c11.y is 0.2, not 1.0. When the canal was in view its
      // draw won the first-pass race and the whole world tone chain ran at
      // x0.2 linear, the "world goes dark at some camera rotations" bug
      // (native exactly 0.50x emulated, uniform). Only the environment
      // families that share the c10.x/c11.y layout are eligible.
      const auto env_receiver_ps = [&]() -> bool {
        const auto check = [&](uint32_t obj) -> int {  // 0 unknown, 1 no, 2 yes
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return 0;
          }
          static std::unordered_map<uint32_t, int> cache;
          auto it = cache.find(obj);
          if (it != cache.end()) {
            return it->second;
          }
          char text[120] = {};
          for (int k = 0; k < 119; ++k) {
            text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
            if (text[k] == '\0') break;
          }
          const bool hit = std::strstr(text, "\\baseenvironment") != nullptr ||
                           std::strstr(text, "\\defaultenvironment") != nullptr ||
                           std::strstr(text, "\\decalenvironment") != nullptr;
          // Empty/garbled paths stay unknown (0) and are not cached-in as
          // negatives forever.
          const int result = text[0] == '\0' ? 0 : (hit ? 2 : 1);
          if (result != 0 && cache.size() < 4096) {
            cache.emplace(obj, result);
          }
          return result;
        };
        const int a = check(g_cur_ps_obj.load(std::memory_order_relaxed));
        if (a != 0) {
          return a == 2;
        }
        return check(g_cur_vs_obj.load(std::memory_order_relaxed)) == 2;
      };
      // Fog rows (VS c5 ramp / c6 color): POSITIVE family gate like the
      // receiver rows below, for the same reason: the camera-keyed c4 check
      // alone lets ANY main-pass-layout draw win the first-draw race, and
      // the dam spillway's water shaders keep the camera at c4 with a
      // water-teal where fog c6 lives, values that PASSED the range gate
      // and tinted every distance-fogged surface (the bank mist band, the
      // far dirt hill) saturated blue for exactly the one frame that
      // capture served (the approach-flicker blue flash; F7 scene-ring
      // proved composition/textures identical across the artifact frame).
      // Same failure class as the flowingwater tone hijack documented at
      // env_receiver_ps.
      if (!g_fog_frame_done && env_receiver_ps()) {
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        // Range gate (kept as a second line of defense): ramp scale is a
        // tiny per-meter slope, the exponent is a small power, the fog color
        // is a dim linear-space rgb and the transmittance scale small.
        const bool sane = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                          rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                          rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                          rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (sane) {
          std::memcpy(g_fog_rows, rows, sizeof(rows));
          g_fog_have = true;
          g_fog_frame_done = true;
        }
      } else if (!g_fog_frame_done) {
        // Confirmation probe for the blue-flash fix: a NON-env draw whose
        // rows would have passed the old value-only gate with a fog color
        // far from the current one is exactly the frame that used to flash
        // - each hit here is one prevented flash, naming the hijacker.
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        const bool would = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                           rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                           rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                           rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (would) {
          const float dr = rows[4] - g_fog_rows[4];
          const float dg = rows[5] - g_fog_rows[5];
          const float db = rows[6] - g_fog_rows[6];
          if (dr * dr + dg * dg + db * db > 0.01f) {
            // Rolling cap (the flat 32 burned at the load screen).
            static std::atomic<uint32_t> s_fog_rejects{0};
            static std::atomic<int64_t> s_fog_win{0};
            const int64_t now_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            int64_t win = s_fog_win.load(std::memory_order_relaxed);
            if (now_s - win >= 5 &&
                s_fog_win.compare_exchange_strong(win, now_s)) {
              s_fog_rejects.store(0, std::memory_order_relaxed);
            }
            if (s_fog_rejects.fetch_add(1, std::memory_order_relaxed) < 8) {
              REXLOG_INFO(
                  "native-scene: fog capture REJECTED by family gate "
                  "(prevented flash): vs_obj={:08X} ps_obj={:08X} "
                  "color=({:.3f},{:.3f},{:.3f}) vs current ({:.3f},{:.3f},{:.3f})",
                  g_cur_vs_obj.load(std::memory_order_relaxed),
                  g_cur_ps_obj.load(std::memory_order_relaxed), rows[4], rows[5],
                  rows[6], g_fog_rows[4], g_fog_rows[5], g_fog_rows[6]);
            }
          }
        }
      }
      // flowingwateralpha m_params + animation time (PS c11..c14, c15.x),
      // POSITIVE debug-path gate like the receiver rows: the water bank
      // keeps the whole baseenvironment receiver layout, so only the name
      // discriminates. One material's rows serve the frame; every
      // flowingwateralpha draw in a capture carried identical m_params, and
      // the animation time is pass-global anyway.
      if (!g_water_frame_done && ps_bank != 0) {
        const auto water_ps = [&]() -> bool {
          const auto check = [&](uint32_t obj) -> bool {
            if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
              return false;
            }
            char text[120] = {};
            for (int k = 0; k < 119; ++k) {
              text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
              if (text[k] == '\0') break;
            }
            return std::strstr(text, "\\flowingwateralpha") != nullptr;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (water_ps()) {
          float rows[17];
          for (int i = 0; i < 16; ++i) {
            rows[i] = LoadGuestF32(base, ps_bank + (44 + i) * 4);  // c11..c14
          }
          rows[16] = LoadGuestF32(base, ps_bank + 60 * 4);  // c15.x = time
          // Range gate: normal scales O(1), material multiplier small
          // positive, scroll speeds/UV scales bounded, spec power a real
          // exponent, alpha floor a blend factor, time non-negative.
          bool sane = rows[1] > 0.0f && rows[1] <= 8.0f &&    // c11.y mult
                      rows[13] >= 0.0f && rows[13] <= 1.0f && // c14.y thr
                      rows[14] >= 1.0f && rows[14] <= 1000.0f && // c14.z pow
                      rows[15] >= 0.0f && rows[15] <= 1.0f && // c14.w floor
                      rows[16] >= 0.0f && rows[16] < 1e7f;    // time
          for (int i = 0; i < 4 && sane; ++i) {
            sane = std::fabs(rows[0 + i]) <= 8.0f &&    // c11
                   std::fabs(rows[4 + i]) <= 64.0f &&   // c12 speeds
                   std::fabs(rows[8 + i]) <= 256.0f;    // c13 scales
          }
          if (sane) {
            const bool first = !g_water_have;
            std::memcpy(g_water_rows, rows, sizeof(rows));
            g_water_have = true;
            g_water_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: water m_params captured: p0=({:.3g},{:.3g},"
                  "{:.3g},{:.3g}) speed=({:.3g},{:.3g},{:.3g},{:.3g}) "
                  "scale=({:.3g},{:.3g},{:.3g},{:.3g}) thr={:.3g} pow={:.3g} "
                  "floor={:.3g} t={:.2f}",
                  rows[0], rows[1], rows[2], rows[3], rows[4], rows[5],
                  rows[6], rows[7], rows[8], rows[9], rows[10], rows[11],
                  rows[13], rows[14], rows[15], rows[16]);
            }
          } else {
            static std::atomic<int> s_water_rejects{0};
            if (s_water_rejects.fetch_add(1, std::memory_order_relaxed) < 4) {
              REXLOG_INFO(
                  "native-scene: water m_params REJECTED by range gate: "
                  "mult={:.3g} thr={:.3g} pow={:.3g} floor={:.3g} t={:.3g}",
                  rows[1], rows[13], rows[14], rows[15], rows[16]);
            }
          }
        }
      }
      // scrollincandescent.fx (the emissive time-scrolled LED chyron):
      // g_fAnimationTime from the VERTEX bank (c9.x) and the material
      // multiplier m_params[0].y from the PIXEL bank (c3.y). Same POSITIVE
      // debug-path gate as the water rows; the bank values alone cannot
      // discriminate this family.
      if (!g_scroll_frame_done && ps_bank != 0) {
        const auto scroll_ps = [&]() -> bool {
          const auto check = [&](uint32_t obj) -> bool {
            if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
              return false;
            }
            char text[120] = {};
            for (int k = 0; k < 119; ++k) {
              text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
              if (text[k] == '\0') break;
            }
            return std::strstr(text, "\\scrollincandescent") != nullptr;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (scroll_ps()) {
          const float t = LoadGuestF32(base, bank + 36 * 4);        // VS c9.x
          const float mult = LoadGuestF32(base, ps_bank + 13 * 4);  // PS c3.y
          // Range gate: the animation clock is a non-negative seconds
          // counter, the multiplier a small positive scale.
          if (t >= 0.0f && t < 1e7f && mult > 0.0f && mult <= 8.0f) {
            const bool first = !g_scroll_have;
            g_scroll_rows[0] = t;
            g_scroll_rows[1] = mult;
            g_scroll_have = true;
            g_scroll_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: scroll rows captured: t={:.2f} mult={:.3g}",
                  t, mult);
            }
          }
        }
      }
      // ocean_defaultPS material rows (PCA mean/weights at c2..c8,
      // m_params[0..2] at c12..c14) and the oceanreflection sheet row (c3).
      // Same POSITIVE debug-path gating as the water rows.
      if ((!g_ocean_frame_done || !g_oceanrefl_frame_done) && ps_bank != 0) {
        const auto ps_name_has = [&](const char* needle) -> bool {
          const auto check = [&](uint32_t obj) -> bool {
            if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
              return false;
            }
            char text[120] = {};
            for (int k = 0; k < 119; ++k) {
              text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
              if (text[k] == '\0') break;
            }
            return std::strstr(text, needle) != nullptr;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (!g_ocean_frame_done && ps_name_has("\\ocean_default")) {
          float c[64];
          for (int i = 0; i < 64; ++i) {
            c[i] = LoadGuestF32(base, ps_bank + (8 + i) * 4);  // c2..c17
          }
          const float* m0 = c + (12 - 2) * 4;  // c12
          const float* m1 = c + (13 - 2) * 4;  // c13
          const float* m2 = c + (14 - 2) * 4;  // c14
          bool sane = m1[1] > 0.0f && m1[1] <= 8.0f &&   // multiplier
                      m1[2] >= 0.0f && m1[2] <= 8.0f &&  // tonedown scale
                      m1[3] > 0.0f && m1[3] <= 4096.0f &&  // uv scale
                      m2[0] > 0.01f && m2[0] <= 1000.0f &&  // spread u
                      m2[1] > 0.01f && m2[1] <= 1000.0f &&  // spread v
                      m2[2] > 0.001f && m2[2] <= 100.0f;    // roughness
          for (int i = 0; i < 4 && sane; ++i) {
            sane = c[i] >= -2.0f && c[i] <= 2.0f;  // PCA mean row
          }
          if (sane) {
            float* o = g_ocean_rows;
            // mean pre-swizzled for (R,G,B): PS c2.x, c2.z, c2.y
            o[0] = c[0];
            o[1] = c[2];
            o[2] = c[1];
            o[3] = 0.0f;
            // weight rows in model order: c3, c4, c7, c8, c5, c6
            const int worder[6] = {3, 4, 7, 8, 5, 6};
            for (int w = 0; w < 6; ++w) {
              std::memcpy(o + 4 + w * 4, c + (worder[w] - 2) * 4,
                          4 * sizeof(float));
            }
            std::memcpy(o + 28, m0, 4 * sizeof(float));
            std::memcpy(o + 32, m1, 4 * sizeof(float));
            std::memcpy(o + 36, m2, 4 * sizeof(float));
            const bool first = !g_ocean_have;
            g_ocean_have = true;
            g_ocean_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: ocean rows captured: mult={:.3g} "
                  "tonedown={:.3g} uvscale={:.3g} ward=({:.3g},{:.3g},{:.3g})",
                  m1[1], m1[2], m1[3], m2[0], m2[1], m2[2]);
            }
          }
        }
        if (!g_oceanrefl_frame_done && ps_name_has("\\oceanreflection")) {
          float row[4];
          for (int i = 0; i < 4; ++i) {
            row[i] = LoadGuestF32(base, ps_bank + (12 + i) * 4);  // c3
          }
          // multiplier small positive; the fade bounds must not coincide.
          if (row[0] > 0.0f && row[0] <= 8.0f &&
              std::fabs(row[1] - row[2]) > 1e-4f) {
            std::memcpy(g_oceanrefl_rows, row, sizeof(row));
            const bool first = !g_oceanrefl_have;
            g_oceanrefl_have = true;
            g_oceanrefl_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: oceanreflection row captured: mult={:.3g} "
                  "fade=({:.3g},{:.3g})",
                  row[0], row[1], row[2]);
            }
          }
        }
      }
      // Not gated on the shadows cvar: the captured rows also carry the
      // scene exposure / material multiplier / sun direction consumed by the
      // exact world shading (rows 40/45/24..26); shading must not die when
      // dynamic shadows are toggled off.
      if (!g_shadow_frame_done && ps_bank != 0 && env_receiver_ps()) {
        float rows[48];
        for (int i = 0; i < 48; ++i) {
          rows[i] = LoadGuestF32(base, ps_bank + i * 4);
        }
        // Receiver-layout sanity:
        // c0/c3 are the light-space X/Y rows with equal magnitude (the
        // cascade-0 extent), c1/c2 the square cascade scale+offsets with
        // scale2 < scale1 < 1, c4 the depth row (a pure height ramp: x/z
        // components tiny), c8 a dim shadow color in [0,1]. Environment
        // family only; other families fail these checks.
        const float mx2 = rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2];
        const float my2 = rows[12] * rows[12] + rows[13] * rows[13] + rows[14] * rows[14];
        const float s1 = rows[4], s2 = rows[8];
        bool sane =
            mx2 > 0.01f && mx2 < 4.0f && std::fabs(mx2 - my2) < 0.05f * mx2 &&
            s1 > 0.0f && s1 < 1.0f && std::fabs(rows[5] - s1) < 1e-4f &&
            s2 > 0.0f && s2 < s1 && std::fabs(rows[9] - s2) < 1e-4f &&
            std::fabs(rows[16]) < 0.02f && std::fabs(rows[18]) < 0.02f &&
            std::fabs(rows[17]) > 0.005f && std::fabs(rows[17]) < 1.0f &&
            rows[32] >= 0.0f && rows[32] <= 1.0f && rows[33] >= 0.0f &&
            rows[33] <= 1.0f && rows[34] >= 0.0f && rows[34] <= 1.0f;
        // c5..c8 are NOT pass-global like c0..c4: only the baseenvironment /
        // defaultenvironment-style bank keeps (bias, ..., sun dir, camera,
        // shadow color) there. Verified in capture: the
        // dynamicobject/livingworld/cacstamp banks pass the geometric checks
        // above with garbage in those rows (c8 ~ (0.0025,0.005,0.012) -> a
        // near-black shadow, c5.x = 0.0245 -> a 29 cm receiver height bias
        // that culled the feet/board shadow); advertisement keeps the color
        // at c7 (c8 = 0 -> pure black); wateralpha has c8 ~ (0.5,0.99,0.5)
        // (-> no shadow at all). Discriminate on the family-specific rows:
        // c6 = unit sun direction with normalize(cross(c0,c3)) == -c6 (the
        // oblique projection axis), c7 = the camera position, c8 dim.
        if (sane) {
          const float cxx = rows[1] * rows[14] - rows[2] * rows[13];
          const float cxy = rows[2] * rows[12] - rows[0] * rows[14];
          const float cxz = rows[0] * rows[13] - rows[1] * rows[12];
          const float cn = std::sqrt(cxx * cxx + cxy * cxy + cxz * cxz);
          const float sn = std::sqrt(rows[24] * rows[24] + rows[25] * rows[25] +
                                     rows[26] * rows[26]);
          const float align =
              cn > 1e-6f && sn > 1e-6f
                  ? (cxx * rows[24] + cxy * rows[25] + cxz * rows[26]) / (cn * sn)
                  : 0.0f;
          const float dcx = rows[28] - g_fog_cam[0];
          const float dcy = rows[29] - g_fog_cam[1];
          const float dcz = rows[30] - g_fog_cam[2];
          sane = align < -0.9f && sn > 0.9f && sn < 1.1f &&
                 dcx * dcx + dcy * dcy + dcz * dcz < 25.0f && rows[32] <= 0.4f &&
                 rows[33] <= 0.4f && rows[34] <= 0.4f &&
                 // c10.x = scene exposure, c11.y = material multiplier,
                 // consumed by the exact world tone chain.
                 rows[40] > 0.1f && rows[40] < 16.0f && rows[45] > 0.0f &&
                 rows[45] < 16.0f;
        }
        if (sane) {
          // Scene-exposure transition log (c10.x): the game's auto-exposure
          // holds this at the per-zone maximum (2.5 in daytime zones) while
          // luminance readbacks are unavailable, enforced by the
          // skate3_autoexposure_pin hook. Transitions are rare and mark
          // either a zone change or an adaptation leak, so log them.
          if (g_shadow_have &&
              std::fabs(rows[40] - g_shadow_rows[40]) > 0.02f * rows[40]) {
            REXLOG_INFO(
                "native-scene: world scene exposure changed {:.3f} -> {:.3f}",
                g_shadow_rows[40], rows[40]);
          }
          std::memcpy(g_shadow_rows, rows, sizeof(rows));
          g_shadow_have = true;
          g_shadow_frame_done = true;
          g_shadow_rows_frame = g_guest_frame;
        }
      }
      // tree / proxyworld frame rows (see FrameScene::family_rows): their PS
      // banks keep family-global lighting constants in different registers
      // than the environment layout. Identified by the shader debug path
      // (cached, like the blur trigger); the camera-keyed VS c4 gate above
      // already filtered to main-pass draws.
      if ((!g_tree_frame_done || !g_proxy_frame_done || !g_dynobj_frame_done) &&
          ps_bank != 0) {
        const auto world_family = [&](uint32_t obj) -> int {
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return 0;
          }
          static std::unordered_map<uint32_t, int> cache;
          auto it = cache.find(obj);
          if (it != cache.end()) {
            return it->second;
          }
          char text[96] = {};
          for (int k = 0; k < 95; ++k) {
            text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
            if (text[k] == '\0') break;
          }
          int fam = 0;
          if (std::strstr(text, "\\tree_defaultPS") != nullptr ||
              std::strstr(text, "\\treeanimate_defaultPS") != nullptr) {
            fam = 1;
          } else if (std::strstr(text, "\\proxyworld_defaultPS") != nullptr) {
            fam = 2;
          } else if (std::strstr(text, "\\dynamicobject_defaultPS") != nullptr ||
                     std::strstr(text, "\\alphatestdynamicobject_defaultPS") !=
                         nullptr) {
            fam = 3;
          }
          if (cache.size() < 4096) {
            cache.emplace(obj, fam);
          }
          return fam;
        };
        int fam = world_family(g_cur_ps_obj.load(std::memory_order_relaxed));
        if (fam == 0) {
          fam = world_family(g_cur_vs_obj.load(std::memory_order_relaxed));
        }
        if (fam == 3 && !g_dynobj_frame_done) {
          // Sun dir c9, exposure c13.x, ambient c15.xyz + bounce c15.w,
          // material multiplier c14.y, static world-shadow floor c8.w.
          float sun[3];
          float n2 = 0.0f;
          for (int k = 0; k < 3; ++k) {
            sun[k] = LoadGuestF32(base, ps_bank + (9 * 4 + k) * 4);
            n2 += sun[k] * sun[k];
          }
          const float expo = LoadGuestF32(base, ps_bank + (13 * 4 + 0) * 4);
          const float mult = LoadGuestF32(base, ps_bank + (14 * 4 + 1) * 4);
          if (n2 > 0.9f && n2 < 1.1f && expo > 0.1f && expo < 16.0f &&
              mult > 0.0f && mult < 16.0f) {
            g_dynobj_rows[0] = sun[0];
            g_dynobj_rows[1] = sun[1];
            g_dynobj_rows[2] = sun[2];
            g_dynobj_rows[3] = expo;
            for (int k = 0; k < 3; ++k) {
              g_dynobj_rows[4 + k] = LoadGuestF32(base, ps_bank + (15 * 4 + k) * 4);
            }
            g_dynobj_rows[7] = LoadGuestF32(base, ps_bank + (15 * 4 + 3) * 4);
            g_dynobj_rows[8] = mult;
            g_dynobj_rows[9] = LoadGuestF32(base, ps_bank + (8 * 4 + 3) * 4);
            // Static world-shadow transform (c5 = u row, c6 = v row,
            // c7 = ray depth row): consumed by the native world-shadow
            // re-render + the PS world term. Region-stable rows; a change
            // re-primes the native map (see RenderShadowAtlas).
            float ws[12];
            bool ws_ok = true;
            float uvmag = 0.0f;
            for (int k = 0; k < 12; ++k) {
              ws[k] = LoadGuestF32(base, ps_bank + ((5 + k / 4) * 4 + k % 4) * 4);
              if (!(ws[k] == ws[k]) || std::fabs(ws[k]) > 1e6f) {
                ws_ok = false;
              }
              if (k < 8 && (k % 4) < 3) {
                uvmag += std::fabs(ws[k]);
              }
            }
            if (ws_ok && uvmag > 1e-6f) {
              std::memcpy(g_dynobj_ws, ws, sizeof(ws));
              g_dynobj_ws_have = true;
            }
            if (!g_dynobj_have) {
              REXLOG_INFO(
                  "native-scene: dynamicobject rows captured sun=({:.3f},{:.3f},"
                  "{:.3f}) expo={:.2f} amb=({:.3f},{:.3f},{:.3f}) bounce={:.3f} "
                  "mult={:.2f} floor={:.3f}",
                  g_dynobj_rows[0], g_dynobj_rows[1], g_dynobj_rows[2],
                  g_dynobj_rows[3], g_dynobj_rows[4], g_dynobj_rows[5],
                  g_dynobj_rows[6], g_dynobj_rows[7], g_dynobj_rows[8],
                  g_dynobj_rows[9]);
            }
            g_dynobj_have = true;
            g_dynobj_frame_done = true;
          }
        } else if (fam == 1 && !g_tree_frame_done) {
          const float c0x = LoadGuestF32(base, ps_bank + 0 * 4);
          const float c0y = LoadGuestF32(base, ps_bank + 1 * 4);
          const float c4y = LoadGuestF32(base, ps_bank + 17 * 4);
          if (c0x > 0.0f && c0x < 4.0f && c0y >= 0.0f && c0y < 1.0f &&
              c4y > 0.0f && c4y < 8.0f) {
            g_family_rows[0] = c0x;
            g_family_rows[1] = c0y;
            g_family_rows[2] = c4y;
            g_tree_frame_done = true;
          }
        } else if (fam == 2 && !g_proxy_frame_done) {
          const float c3y = LoadGuestF32(base, ps_bank + 13 * 4);
          if (c3y > 0.0f && c3y < 4.0f) {
            g_family_rows[3] = c3y;
            g_proxy_frame_done = true;
          }
        }
      }
    }
  }
  // Photo-editor postfx capture (SOLE site): read the game's staging banks
  // at draw time, the same reads the F11 per-draw records use, verified
  // correct for every postfx pass in captures 1783957072/1783963636. The
  // former SetPending upload-time capture mixed neighbouring passes' rows
  // (the game stages the next pass's constants before rebinding shaders)
  // and has been removed.
  if (flags2d == 0 && SceneEnabled() &&
      g_photo_flow_frame.load(std::memory_order_relaxed)) {
    int pfx_pass = ClassifyPfxShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (pfx_pass < 0) {
      pfx_pass = ClassifyPfxShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (pfx_pass >= 0) {
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      const uint32_t vs_bank = g_vs_bank.load(std::memory_order_relaxed);
      const uint32_t dev = g_device.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        CapturePfxConstants(base, ps_bank, dev, /*pixel=*/true);
      }
      if (vs_bank != 0) {
        CapturePfxConstants(base, vs_bank, dev, /*pixel=*/false);
      }
    }
  }
  // UI background blur: while a frontend popup is up, the game appends a
  // dedicated pass chain after the postfx uber: blur_hBlurPS + blur_vBlurPS
  // (func-2 inline quads OUTSIDE the 2D phase) then a postfx_basictex
  // fullscreen replace. The blur_hBlurPS draw itself is the trigger; its
  // PS c0.x is the kernel scale (8 in every capture). See kBlurShaderSource.
  if (func == 2 && flags2d == 0 && SceneEnabled()) {
    const auto is_hblur = [&](uint32_t obj) -> bool {
      if (obj == 0 || !GuestReadableApprox(base, obj)) {
        return false;
      }
      // Guest-render-thread only (like the fog capture globals).
      static std::unordered_map<uint32_t, bool> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      // Debug path at +0x54, e.g. ".../blur_hBlurPS.updb".
      char text[96] = {};
      for (int k = 0; k < 95; ++k) {
        text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
        if (text[k] == '\0') break;
      }
      const bool hit = std::strstr(text, "blur_hBlurPS") != nullptr;
      if (cache.size() < 1024) {
        cache.emplace(obj, hit);
      }
      return hit;
    };
    // Shader labels can be swapped in the hook; accept either slot.
    if (is_hblur(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
        is_hblur(g_cur_vs_obj.load(std::memory_order_relaxed))) {
      // Kernel scale read LIVE under a plausibility gate (k0 == k1, in
      // (0, 8]) so the game's own pause-open ramp animates the native blur
      // like the emulated frame; the old pin-to-8 popped the backdrop to
      // full blur on the first frame, and its exact-(8,8) gate on the fade
      // rejected every mid-ramp c1 (the tint then snapped in late). A
      // failed gate keeps the last values (stale bank mid-rewrite); raw
      // 1-frame value flaps (the tinted-backdrop flicker) are absorbed by
      // the render-thread smoothing at the blur pass.
      g_ui_blur_seen = true;
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        const float k0 = LoadGuestF32(base, ps_bank + 0);
        const float k1 = LoadGuestF32(base, ps_bank + 4);
        float c1[3];
        for (int a = 0; a < 3; ++a) {
          c1[a] = LoadGuestF32(base, ps_bank + 16 + a * 4);
        }
        const bool kernel_ok = k0 == k1 && k0 > 0.0f && k0 <= 8.0f;
        const bool fade_ok = c1[0] >= 0.0f && c1[0] <= 1.0f && c1[1] >= 0.0f &&
                             c1[1] <= 1.0f && c1[2] >= 0.0f && c1[2] <= 1.0f;
        if (kernel_ok) {
          g_ui_blur = k0;
          if (fade_ok) {
            std::memcpy(g_ui_blur_color, c1, sizeof(g_ui_blur_color));
          }
        }
        // Open-ramp tracer (guest render thread only): the first draws
        // after each blur-ON edge print the RAW kernel/fade so the game's
        // actual animate-in curve lands in the log (for an exactness pass
        // if the smoothed approximation reads wrong).
        static int64_t s_last_draw_ns = 0;
        static uint32_t s_edge_logged = 0;
        static uint32_t s_total_logged = 0;
        const int64_t trace_now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
        if (trace_now - s_last_draw_ns > 1'000'000'000ll) {
          s_edge_logged = 0;
        }
        s_last_draw_ns = trace_now;
        if (s_edge_logged < 20 && s_total_logged < 100) {
          ++s_edge_logged;
          ++s_total_logged;
          REXLOG_DEBUG(
              "native-scene: blur ramp k=({:.3f},{:.3f}) c1=({:.3f},{:.3f},{:.3f}) {}",
              k0, k1, c1[0], c1[1], c1[2], kernel_ok ? "accepted" : "held");
        }
      }
    }
  }
  // Selected-object outline capture (see g_frame_selected): the sky draw
  // opens the post-sky window; environmentpark/dynamicobject draws inside it
  // are the selection re-draws (the game excludes the selected object from
  // the main pass and stencil-marks it here). The postfx_edgedetectstencil
  // draw refreshes the outline color.
  if (flags2d == 0 && SceneEnabled() &&
      REXCVAR_GET(skate3_native_render_scene_selection_outline)) {
    // Debug-path family, cached per shader object (guest render thread only,
    // like the blur classifier). 1 = sky, 2 = park piece / dynamic object,
    // 3 = postfx_edgedetectstencil.
    const auto family_of = [&](uint32_t obj) -> int {
      if (obj == 0 || !GuestReadableApprox(base, obj)) {
        return 0;
      }
      static std::unordered_map<uint32_t, int> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      char text[120] = {};
      for (int k = 0; k < 119; ++k) {
        text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
        if (text[k] == '\0') break;
      }
      int fam = 0;
      if (std::strstr(text, "\\sky_") != nullptr) {
        fam = 1;
      } else if (std::strstr(text, "\\environmentpark") != nullptr ||
                 std::strstr(text, "\\dynamicobject") != nullptr) {
        fam = 2;
      } else if (std::strstr(text, "postfx_edgedetectstencil") != nullptr) {
        fam = 3;
      }
      if (cache.size() < 4096) {
        cache.emplace(obj, fam);
      }
      return fam;
    };
    // Shader labels can be swapped in the hook; accept either slot.
    int fam = family_of(g_cur_ps_obj.load(std::memory_order_relaxed));
    if (fam == 0) {
      fam = family_of(g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (func == 0 && fam == 1) {
      g_sky_seen_this_frame = true;
    } else if (func == 0 && fam == 2 && g_sky_seen_this_frame) {
      // World matrix in the VS bank: 3 rotation rows (w == 0) followed by
      // the translation row (w == 1): c11..c14 on the decal variant,
      // c9..c12 on the diffuse variant. Scan for the first such group.
      for (int r = 8; r <= 16; ++r) {
        if (LoadGuestF32(base, bank + (r * 4 + 3) * 4) != 1.0f) continue;
        bool rot = true;
        for (int p = 1; p <= 3 && rot; ++p) {
          rot = LoadGuestF32(base, bank + ((r - p) * 4 + 3) * 4) == 0.0f;
        }
        if (!rot) continue;
        float t[3];
        for (int a = 0; a < 3; ++a) {
          t[a] = LoadGuestF32(base, bank + (r * 4 + a) * 4);
        }
        if (std::fabs(t[0]) < 100000.0f && std::fabs(t[1]) < 100000.0f &&
            std::fabs(t[2]) < 100000.0f) {
          const uint32_t ib = g_cur_ib.load(std::memory_order_relaxed);
          const uint32_t vb = g_cur_vb.load(std::memory_order_relaxed);
          bool merged = false;
          for (SelectedDrawKey& k : g_frame_selected) {
            if (k.ib == ib && k.vb == vb && std::fabs(k.t[0] - t[0]) < 0.05f &&
                std::fabs(k.t[1] - t[1]) < 0.05f && std::fabs(k.t[2] - t[2]) < 0.05f) {
              ++k.count;
              merged = true;
              break;
            }
          }
          if (!merged && g_frame_selected.size() < 64) {
            g_frame_selected.push_back({ib, vb, {t[0], t[1], t[2]}, 1});
          }
        }
        break;
      }
    } else if (fam == 3) {
      // postfx edge-detect: the presence of this draw is what authorizes the
      // native outline this frame (see g_outline_edge_seen); its PS c0 = the
      // outline color as staged (the park-editor blue (0.216, 0.647, 1.0) in
      // every capture).
      g_outline_edge_seen = true;
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        float c[4];
        for (int a = 0; a < 4; ++a) {
          c[a] = LoadGuestF32(base, ps_bank + a * 4);
        }
        if (c[0] >= 0.0f && c[0] <= 8.0f && c[1] >= 0.0f && c[1] <= 8.0f &&
            c[2] >= 0.0f && c[2] <= 8.0f) {
          std::memcpy(g_outline_color, c, sizeof(g_outline_color));
        }
      }
    }
  }
  // Live 2D overlay capture: the HUD renders exclusively through the
  // BeginVertices inline path (func 2). Vertex payloads are read at frame
  // end; everything else (transform constants, texture fetch) is staged now.
  // (An "unbracketed capture" for out-of-bracket boot draws lived here
  // briefly, removed: every real boot/menu UI draw is bracketed, including
  // the intro video quad, and it demonstrably ingested blur/postfx pass
  // quads during takeover-hold windows.)
  if (flags2d != 0 && SceneEnabled() && REXCVAR_GET(skate3_native_render_scene_2d)) {
    if (func == 2) {
      const uint32_t device = g_device.load(std::memory_order_relaxed);
      if (device != 0 && r7 >= 0x10000 && r5 != 0 && r5 <= 65536 && r6 >= 8 &&
          r6 <= 256 && (r6 & 3) == 0) {
        Draw2d d;
        d.prim = r4;
        d.count = r5;
        d.stride = r6;
        d.addr = r7;
        d.flags = flags2d;
        // Slots 0-2 (18 dwords): slot 0 is the draw's texture; slots 1-2
        // matter only for video quads, whose YUV shader binds the U and V
        // planes there (the self-contained YUV-triple detection at replay).
        for (int i = 0; i < 18; ++i) {
          d.fetch[i] = REX_LOAD_U32(device + 0x480 + i * 4);
        }
        for (int i = 0; i < 36; ++i) {
          d.consts[i] = LoadGuestF32(base, bank + i * 4);
        }
        std::lock_guard<std::mutex> lock(g_2d_mutex);
        if (g_frame_2d.size() < 4096) {
          g_frame_2d.push_back(std::move(d));
        } else {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      g_draws_2d_other.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // In-world neon spline capture (see SplineDraw): a DrawVertices strip of
  // 12-byte float3 params on the spline shaders, bound VB object carrying
  // the payload fetch block (+0x18 base | flags, +0x20 size).
  if (func == 1 && flags2d == 0 && SceneEnabled() &&
      REXCVAR_GET(skate3_native_render_scene_splines) && r5 >= 4 && r5 <= 8192) {
    uint32_t kind = ClassifySplineShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (kind == 0) {
      kind = ClassifySplineShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    const uint32_t vb_obj = g_cur_vb.load(std::memory_order_relaxed);
    if (kind != 0 && GuestReadableApprox(base, vb_obj)) {
      const uint32_t addr = REX_LOAD_U32(vb_obj + 0x18) & 0xFFFFFFFCu;
      const uint32_t size = REX_LOAD_U32(vb_obj + 0x20);
      if (addr >= 0x10000 && size >= r5 * 12) {
        SplineDraw s;
        s.pass = kind;
        s.count = r5;
        s.verts.resize(size_t(r5) * 12);
        if (GuestTryCopy(s.verts.data(), base + addr, s.verts.size())) {
          const uint32_t device = g_device.load(std::memory_order_relaxed);
          for (int i = 0; i < 6; ++i) {
            // The gradient texture lives in fetch shadow slot 3 (24 bytes
            // per slot; the spline PS samples tf3).
            s.fetch[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + 3 * 24 + i * 4) : 0;
          }
          for (int i = 0; i < 153 * 4; ++i) {
            s.consts[i] = LoadGuestF32(base, bank + i * 4);
          }
          g_draws_spline.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard<std::mutex> lock(g_2d_mutex);
          if (g_frame_spline.size() < 64) {
            g_frame_spline.push_back(std::move(s));
          }
        }
      }
    }
  }
  const uint32_t cur_ib = g_cur_ib.load(std::memory_order_relaxed);
  const uint32_t cur_vb = g_cur_vb.load(std::memory_order_relaxed);
  if (g_recording.load(std::memory_order_relaxed)) {
    // Sample the full draw stream on a couple of recorded frames out of
    // every 60, spread across the window (ground-truth coverage for the
    // whole recording), and only for frames that are themselves recorded.
    // 2D-phase draws are recorded on EVERY recorded frame; the HUD stream
    // is small and is exactly what the 2D reconstruction needs.
    std::lock_guard<std::mutex> lock(g_record_mutex);
    const bool frame_recorded = (g_frames_seen + 1) % g_record_stride == 0;
    const bool all_draws = REXCVAR_GET(skate3_native_render_snapshot_all_draws);
    // 600k: a 360-frame all-draws photo-compose trace is ~470k records (a
    // 200k cap cuts such a trace around frame 160, before the
    // late-firing card compose).
    const size_t draw_cap = all_draws ? 600000 : 32768;
    if (frame_recorded && (all_draws || flags2d != 0 || (g_record_frame % 60) < 2) &&
        g_recorded_draws.size() < draw_cap) {
      auto rec = std::make_unique<RecordedDraw>();
      rec->func = func;
      rec->ib = cur_ib;
      rec->vb = cur_vb;
      rec->vb_offset = g_cur_vb_offset.load(std::memory_order_relaxed);
      rec->vb_stride = g_cur_vb_stride.load(std::memory_order_relaxed);
      for (int s = 0; s < 4; ++s) {
        for (int k = 0; k < 3; ++k) {
          rec->streams[s][k] = g_cur_streams[s][k].load(std::memory_order_relaxed);
        }
      }
      const uint32_t device = g_device.load(std::memory_order_relaxed);
      for (int i = 0; i < 12; ++i) {
        rec->vfetch[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + i * 4) : 0;
      }
      rec->args[0] = r4;
      rec->args[1] = r5;
      rec->args[2] = r6;
      rec->args[3] = r7;
      for (uint32_t i = 0; i < 1024; ++i) {
        rec->bank[i] = LoadGuestF32(base, bank + i * 4);
      }
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < 256; ++i) {
        rec->ps[i] = ps_bank != 0 ? LoadGuestF32(base, ps_bank + i * 4) : 0.0f;
      }
      rec->frame = g_record_frame;
      rec->flags2d = flags2d;
      rec->ps_obj = g_cur_ps_obj.load(std::memory_order_relaxed);
      rec->vs_obj = g_cur_vs_obj.load(std::memory_order_relaxed);
      for (int i = 0; i < 6; ++i) {
        rec->viewport[i] = g_cur_viewport[i].load(std::memory_order_relaxed);
      }
      for (int i = 0; i < 4; ++i) {
        rec->scissor[i] = g_cur_scissor[i].load(std::memory_order_relaxed);
      }
      const uint32_t rs_bank = g_rs_bank.load(std::memory_order_relaxed);
      for (int i = 0; i < 256; ++i) {
        rec->rstates[i] = rs_bank != 0 ? REX_LOAD_U32(rs_bank + i * 4) : 0;
      }
      for (int i = 0; i < 192; ++i) {
        rec->vfetch_all[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + i * 4) : 0;
      }
      rec->vb_dump = ~0u;
      rec->ib_dump = ~0u;
      if (flags2d != 0) {
        // 2D payload capture. The 2D pass runs on transient dynamic buffers
        // (glyph/shape vertices regenerated per frame); dump them now.
        // One dump per (guest address, frame); recording mode, so the
        // GuestRangeReadable VAD cost is acceptable.
        const auto dump_buffer = [&](uint32_t obj, uint32_t size_off) -> uint32_t {
          if (obj == 0) {
            return ~0u;
          }
          const uint32_t addr = REX_LOAD_U32(obj + 0x18) & 0xFFFFFFFCu;
          if (addr < 0x10000) {
            return ~0u;
          }
          uint32_t bytes = REX_LOAD_U32(obj + size_off);
          if (bytes < 4 || bytes > (8u << 20)) {
            return ~0u;
          }
          const uint64_t key = (uint64_t(g_record_frame) << 32) | addr;
          auto it = g_frame_dump_ids.find(key);
          if (it != g_frame_dump_ids.end()) {
            return it->second;
          }
          if (g_recorded_buffer_bytes + bytes > (512u << 20) ||
              !GuestRangeReadable(base, addr, bytes)) {
            return ~0u;
          }
          RecordedBuffer buf;
          buf.vb_addr = addr;
          buf.ib_addr = 0;
          buf.fingerprint = key;
          buf.vb.resize(bytes);
          std::memcpy(buf.vb.data(), base + addr, bytes);
          const uint32_t dump_id = uint32_t(g_recorded_buffers.size());
          g_recorded_buffer_bytes += bytes;
          g_recorded_buffers.push_back(std::move(buf));
          g_frame_dump_ids.emplace(key, dump_id);
          return dump_id;
        };
        if (func == 0) {
          rec->vb_dump = dump_buffer(cur_vb, 0x20);
          rec->ib_dump = dump_buffer(cur_ib, 0x1C);
        } else if (func == 1) {
          rec->vb_dump = dump_buffer(cur_vb, 0x20);
        } else if (func == 2 && r7 >= 0x10000 && r5 != 0 && r6 != 0 &&
                   uint64_t(r5) * r6 <= (4u << 20)) {
          // Inline-ring vertices (r5 = count, r6 = stride, r7 = the write
          // pointer BeginVertices returned): the CPU writes them after the
          // call; dump at frame end.
          g_pending_inline_dumps.push_back({r7, r5 * r6, g_recorded_draws.size()});
        }
      }
      if (func == 1 && cur_vb != 0 && flags2d == 0) {
        // Non-indexed (cloth) draw: the bound object holds two ping-pong
        // vertex fetch blocks whose ring payloads are recycled long before
        // frame end; dump both now, keyed back to this draw via rec->ib.
        const uint32_t dump_id = uint32_t(g_recorded_buffers.size());
        for (uint32_t block = 0; block < 2; ++block) {
          const uint32_t fetch0 = REX_LOAD_U32(cur_vb + 0x18 + block * 0x30);
          const uint32_t addr = fetch0 & 0xFFFFFFFC;
          if (addr < 0x10000) continue;
          uint32_t bytes = REX_LOAD_U32(cur_vb + 0x20 + block * 0x30);
          if (bytes < 64 || bytes > (256u << 10)) bytes = 128u << 10;
          if (g_recorded_buffer_bytes + bytes > (512u << 20)) break;
          while (bytes >= 4096 && !GuestRangeReadable(base, addr, bytes)) {
            bytes /= 2;
          }
          if (bytes < 4096) continue;
          RecordedBuffer buf;
          buf.vb_addr = addr;
          buf.ib_addr = 0;
          buf.fingerprint = (uint64_t(g_record_frame) << 32) | dump_id | (block << 30);
          buf.vb.resize(bytes);
          std::memcpy(buf.vb.data(), base + addr, bytes);
          g_recorded_buffer_bytes += bytes;
          g_recorded_buffers.push_back(std::move(buf));
        }
        rec->ib = 0x80000000u | dump_id;
      }
      g_recorded_draws.push_back(std::move(rec));
    }
  }
  if (func != 0) {
    return;  // palette fixup below matches indexed draws only
  }
  const uint64_t key = (uint64_t(cur_ib) << 32) | cur_vb;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  // Record this draw's texture fetch slots 3+4 (lightmap + diffuse on the
  // environment families) for the frame-end streamed-artwork diffuse
  // override (see g_frame_draw_fetch). Last writer wins: the z-prepass
  // draws these buffers first with stale bindings, the main pass overwrites.
  if (flags2d == 0 && SceneEnabled()) {
    const uint32_t device = g_device.load(std::memory_order_relaxed);
    if (device != 0 && g_frame_draw_fetch.size() < 4096) {
      auto& w = g_frame_draw_fetch[key];
      for (int i = 0; i < 12; ++i) {
        w[size_t(i)] = REX_LOAD_U32(device + 0x480 + 3 * 24 + uint32_t(i) * 4);
      }
    }
  }
  // Character-lighting refresh: items already fixed up re-read their PS rows
  // from later draws with the same buffers (commit-on-success, so the
  // main-pass draw's rows win over the stale caster-pass bank). CLONES share
  // (ib,vb) with per-instance constants (livingworld stamp tints, hair
  // colors, SH rows); refreshing every registered item on every matching
  // draw collapsed all clones onto the LAST instance's rows (the teal-vest
  // twins). Pair draw to instance by the bone palette: the draw's VS bank
  // holds the instance's palette, which matches exactly one item's captured
  // bones.
  auto refresh = g_frame_char_refresh.equal_range(key);
  if (refresh.first != refresh.second) {
    const uint32_t pb = BankPaletteBase(base, bank);
    float row0[4] = {};
    if (pb != 0) {
      for (int i = 0; i < 4; ++i) {
        row0[i] = LoadGuestF32(base, bank + (pb * 4 + uint32_t(i)) * 4);
      }
    }
    for (auto it = refresh.first; it != refresh.second; ++it) {
      if (it->second >= g_frame_dynitems.size()) continue;
      DrawItem& d = g_frame_dynitems[it->second];
      if (d.bones.size() >= 4 && pb != 0) {
        bool match = true;
        for (int i = 0; i < 4 && match; ++i) {
          // The refined palette can sit one register past BankPaletteBase
          // (cloth/morph layouts); compare against both candidate rows.
          match = std::fabs(d.bones[size_t(i)] - row0[i]) < 1e-4f;
        }
        if (!match) {
          bool match1 = true;
          for (int i = 0; i < 4 && match1; ++i) {
            match1 = std::fabs(d.bones[size_t(i)] -
                               LoadGuestF32(base, bank + ((pb + 1) * 4 + uint32_t(i)) * 4)) <
                     1e-4f;
          }
          match = match1;
        }
        if (!match) continue;
      }
      CaptureCharLighting(base, d);
    }
  }
  auto range = g_frame_pending_by_buffers.equal_range(key);
  if (range.first == range.second) {
    return;
  }
  // This draw's constants belong to the OLDEST still-PENDING item with
  // these buffers (clones share mesh assets; the deferred list draws in
  // submit order, so FIFO one-shot pairing keeps clones' palettes apart).
  // With none pending, the oldest CASTER-sourced item instead gets a
  // REFRESH from this later draw: palettes captured from the ortho
  // caster-cascade banks carry stale fine animation (vehicle wheel spin,
  // ~40 ms behind in bursts); publishing them made the car's pose stream
  // jump, tripping the smoothing ring's discontinuity reset (the traffic
  // judder). The refresh entry is only retired by a perspective-bank
  // (z/main-pass) capture.
  auto oldest = range.second;
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second >= g_frame_dynitems.size() ||
        !g_frame_dynitems[it->second].pending) {
      continue;
    }
    if (oldest == range.second || it->second < oldest->second) {
      oldest = it;
    }
  }
  bool caster_refresh = false;
  if (oldest == range.second) {
    if (REXCVAR_GET(skate3_native_render_scene_caster_refresh_all)) {
      // Pick the refresh target by PALETTE IDENTITY, across every
      // caster-sourced skinned candidate, including skinned-published ROPA
      // garments. Two structural gaps hid here: (1) ropa items were excluded
      // outright, so the player's tee/hair kept their ortho-bank captures all
      // frame; the caster banks' fine animation runs ticks stale in bursts
      // (measured 0.47-0.60 m translation deltas), so those pieces published
      // poses visibly behind the body (the high-fps hair wedge / vanishing
      // garment: head-joint pieces 0.3-0.4 m behind the same frame's own
      // banks while the body matched bit-exact); (2) oldest-only selection
      // probed the wrong clone for
      // mesh-sharing NPCs, the <1.5 m guard refused, and the right clone
      // never refreshed (the NPC blink). Rank by the new bank's bone-0
      // translation; the commit block below re-guards with the full capture.
      const uint32_t pb_sel = BankPaletteBase(base, bank);
      if (pb_sel == 0) {
        return;
      }
      const float nt[3] = {
          LoadGuestF32(base, bank + (pb_sel * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((pb_sel + 1) * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((pb_sel + 2) * 4 + 3) * 4)};
      float best_d2 = 2.25f;  // same 1.5 m identity bound as the commit guard
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second >= g_frame_dynitems.size()) {
          continue;
        }
        const DrawItem& c = g_frame_dynitems[it->second];
        if (!c.caster_bank || !c.skinned || c.bones.size() < 12) {
          continue;  // rigid-resolved ropa has no palette; world capture owns it
        }
        const float dx = c.bones[3] - nt[0];
        const float dy = c.bones[7] - nt[1];
        const float dz = c.bones[11] - nt[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
          best_d2 = d2;
          oldest = it;
        }
      }
    } else {
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second >= g_frame_dynitems.size()) {
          continue;
        }
        const DrawItem& c = g_frame_dynitems[it->second];
        if (!c.caster_bank || !c.skinned || c.ropa) {
          continue;
        }
        if (oldest == range.second || it->second < oldest->second) {
          oldest = it;
        }
      }
    }
    if (oldest == range.second) {
      return;
    }
    caster_refresh = true;
  }
  // Rigid clone disambiguation: same-mesh piece clones share these buffers,
  // and under batched dispatch (park-editor venues: every capture defers,
  // 100+ pending rigids per frame) the FIFO capture order is NOT the draw
  // order; pairing a draw with the wrong clone swaps piece worlds frame to
  // frame (pieces visibly shuffle and flip 180 degrees). Prefer the pending
  // clone whose LAST PUBLISHED world sits nearest this draw's world; FIFO
  // stays the fallback for first-sight clones with no history. Movables stay
  // matchable: they drift well under the 0.5 m bound per frame and the cache
  // re-stamps on every resolved draw.
  if (!caster_refresh && oldest->second < g_frame_dynitems.size() &&
      !g_frame_dynitems[oldest->second].skinned) {
    float w[16];
    if (!BankIsAuxPerspective(base, bank) && BankRigidWorld(base, bank, w)) {
      float best_d2 = 0.25f;
      auto best = range.second;
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second >= g_frame_dynitems.size()) {
          continue;
        }
        const DrawItem& c = g_frame_dynitems[it->second];
        if (!c.pending || c.skinned || c.ctx == 0) {
          continue;
        }
        const auto cit = g_rigid_world_cache.find(c.ctx);
        if (cit == g_rigid_world_cache.end() || cit->second.mesh != c.mesh) {
          continue;
        }
        const float dx = cit->second.world[12] - w[12];
        const float dy = cit->second.world[13] - w[13];
        const float dz = cit->second.world[14] - w[14];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
          best_d2 = d2;
          best = it;
        }
      }
      if (best != range.second) {
        oldest = best;
      }
    }
  }
  DrawItem& d = g_frame_dynitems[oldest->second];
  if (caster_refresh) {
    // Refresh of a caster-sourced capture from a later (ideally main-pass)
    // draw. Re-capture into a PROBE and require the fresher palette to be
    // THIS entity's pose: same-mesh clones share these buffers, and a far
    // twin's later draw otherwise refreshes the near car onto the twin's
    // position (bone-signal recorded 151 m palette teleports; the near
    // car renders across the map, i.e. invisible where it stands).
    const uint32_t palette_base = BankPaletteBase(base, bank);
    if (palette_base == 0) {
      return;
    }
    DrawItem probe = d;
    if (!CaptureSkinnedState(base, bank, palette_base, probe)) {
      return;
    }
    if (probe.bones.size() < 12 || probe.bones.size() != d.bones.size()) {
      return;
    }
    const float dx = probe.bones[3] - d.bones[3];
    const float dy = probe.bones[7] - d.bones[7];
    const float dz = probe.bones[11] - d.bones[11];
    if (dx * dx + dy * dy + dz * dz > 2.25f) {
      return;  // > 1.5 m: a twin's draw, not this entity's
    }
    probe.caster_bank = BankIsOrtho(base, bank);
    probe.pending = false;
    probe.dbg_src = 2;
    d = std::move(probe);
    if (d.char_family != 0 && g_frame_char_refresh.size() < 256) {
      g_frame_char_refresh.emplace(key, oldest->second);
    }
    if (!d.caster_bank) {
      g_frame_pending_by_buffers.erase(oldest);
    }
    return;
  }
  if (d.skinned) {
    // Locate the palette for this draw's layout; a bank without plausible
    // bone rows (parameter blocks, camera rows) must not be consumed; that
    // was the source of screen-wide stretched characters.
    const uint32_t palette_base = BankPaletteBase(base, bank);
    if (palette_base == 0) {
      return;
    }
    if (!CaptureSkinnedState(base, bank, palette_base, d)) {
      return;  // stale bank refused: wait for a later draw with these buffers
    }
    d.caster_bank = BankIsOrtho(base, bank);
  } else {
    // Deferred rigid prop: the instance's own matrix disambiguates clones
    // (they share these buffers, so the bank can be another clone's; see
    // ReadCtxInstanceWorld), while a same-spot bank keeps winning for its
    // live physics rotation. Bank layout: pre-pass c4..c7, main-pass
    // c8..c11. Neither plausible -> wait for a later draw with these
    // buffers. Aux perspective passes (portrait RTTs) stage off-map
    // portrait-stage worlds; those never serve.
    float bank_world[16];
    const bool fb_bank = !BankIsAuxPerspective(base, bank) &&
                         BankRigidWorld(base, bank, bank_world);
    const bool fb_ctx = ReadCtxInstanceWorld(base, d.ctx, d.world);
    if (fb_bank && fb_ctx) {
      const float dx = bank_world[12] - d.world[12];
      const float dy = bank_world[13] - d.world[13];
      const float dz = bank_world[14] - d.world[14];
      if (dx * dx + dy * dy + dz * dz <= 0.0625f) {
        std::memcpy(d.world, bank_world, sizeof(bank_world));
      } else {
        g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (fb_bank) {
      std::memcpy(d.world, bank_world, sizeof(bank_world));
    } else if (fb_ctx) {
      g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
    } else {
      return;
    }
    d.caster_bank = false;
    if (d.ctx != 0) {
      RigidWorldCache& rc = g_rigid_world_cache[d.ctx];
      rc.mesh = d.mesh;
      std::memcpy(rc.world, d.world, sizeof(rc.world));
      rc.frame = g_guest_frame;
    }
  }
  d.pending = false;
  d.dbg_src = 2;
  if (d.char_family != 0 && g_frame_char_refresh.size() < 256) {
    g_frame_char_refresh.emplace(key, oldest->second);
  }
  if (!d.caster_bank) {
    g_frame_pending_by_buffers.erase(oldest);
  }
}

// ---- Camera re-timing (skate3_native_render_scene_smooth_camera) ----------
// The guest publishes camera poses on its own sim tick (~170-240 Hz,
// irregular, measured cam[chg/rep] streaks of 10 rendered frames on one
// pose at 400 fps) while the render loop free-runs: several rendered frames
// reuse one pose, then the camera steps; the world "judders / skips to
// catch up" while panning. The native renderer owns the view transform, so
// each rendered frame samples a TIME-CORRECT pose instead: interpolate
// between the last two distinct guest poses at (now - one sim interval).
// A few ms of camera latency, no overshoot; teleports/cuts snap.

// Standard 3x3 <-> quaternion pair (self-consistent on the view's row-vector
// rotation; only round-tripping matters).
void QuatFromView(const float view[16], float q[4]) {
  const float m00 = view[0], m01 = view[1], m02 = view[2];
  const float m10 = view[4], m11 = view[5], m12 = view[6];
  const float m20 = view[8], m21 = view[9], m22 = view[10];
  const float tr = m00 + m11 + m22;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q[3] = 0.25f * s;
    q[0] = (m21 - m12) / s;
    q[1] = (m02 - m20) / s;
    q[2] = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q[3] = (m21 - m12) / s;
    q[0] = 0.25f * s;
    q[1] = (m01 + m10) / s;
    q[2] = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q[3] = (m02 - m20) / s;
    q[0] = (m01 + m10) / s;
    q[1] = 0.25f * s;
    q[2] = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q[3] = (m10 - m01) / s;
    q[0] = (m02 + m20) / s;
    q[1] = (m12 + m21) / s;
    q[2] = 0.25f * s;
  }
}

void ViewRotFromQuat(const float q[4], float view[16]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  view[0] = 1 - 2 * (y * y + z * z);
  view[1] = 2 * (x * y - z * w);
  view[2] = 2 * (x * z + y * w);
  view[4] = 2 * (x * y + z * w);
  view[5] = 1 - 2 * (x * x + z * z);
  view[6] = 2 * (y * z - x * w);
  view[8] = 2 * (x * z - y * w);
  view[9] = 2 * (y * z + x * w);
  view[10] = 1 - 2 * (x * x + y * y);
}

void QuatSlerp(const float a[4], const float b_in[4], float t, float out[4]) {
  float b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (dot < 0.0f) {
    for (float& v : b) v = -v;
    dot = -dot;
  }
  float wa = 1.0f - t, wb = t;
  if (dot < 0.9995f) {
    const float theta = std::acos(std::min(dot, 1.0f));
    const float s = std::sin(theta);
    wa = std::sin((1.0f - t) * theta) / s;
    wb = std::sin(t * theta) / s;
  }
  float n2 = 0.0f;
  for (int i = 0; i < 4; ++i) {
    out[i] = wa * a[i] + wb * b[i];
    n2 += out[i] * out[i];
  }
  const float inv = n2 > 1e-12f ? 1.0f / std::sqrt(n2) : 1.0f;
  for (int i = 0; i < 4; ++i) out[i] *= inv;
}

struct CamPose {
  double t = 0.0;
  float q[4] = {0, 0, 0, 1};  // view rotation (rows 0..2)
  float c[3] = {};            // camera world position
};

// Shared playback clock (guest render thread): set by SmoothCamera each
// frame it produces a re-timed pose; dynamic items (bone palettes, rigid
// worlds) interpolate at the SAME time so the skater/NPCs/props stay in
// phase with the smoothed camera; smoothing only the camera concentrated
// all the sim stepping on the skater ("he definitively judders now").
double g_smooth_play = 0.0;
bool g_smooth_active = false;
// Last pose SmoothCamera produced (guest render thread), read by the
// synthetic-pan probe (mode 3) to measure reconstruction error against the
// known ideal pose.
CamPose g_smooth_pose;

// Synthetic-pan probe state/helpers: native/skate3_native_diag.h (the
// engage/step orchestration stays in BuildFrameScene below; it reads the
// live camera and scene locals).

// ---- Off-screen retention (edge-of-view teardown fix) ----------------------
// The game culls its submission stream to ITS camera pose; the native frame
// renders with the re-timed (smoothed) pose, which TRAILS the guest pose by
// up to the boxcar window (~25-50 ms). During a pan the guest stops
// submitting items that left its (leading) frustum while the (trailing)
// rendered view still contains them; world geometry visibly tore down right
// at the screen edges (the emulated frame can never show this: it IS the
// guest frame). Recently seen statics stay retained here and are re-appended
// while the RAW guest frustum can NOT see them (i.e. the cull was
// view-driven). An item the guest frustum CAN see but did not submit was
// really removed (LOD switch, despawn, streaming) and drops immediately;
// retaining those would z-fight the replacement LOD. Statics only: dynamic
// poses go stale the moment they stop being captured. Guest render thread
// only (BuildFrameScene); flips request a clear via the atomic.
struct RetainedItem {
  DrawItem item;
  uint64_t last_seen = 0;  // g_guest_frame of the last live submission
};
std::unordered_map<uint64_t, RetainedItem> g_retained_items;
// Dynamic sibling for traffic VEHICLES (char families 6/7): a vehicle
// passing CLOSE leaves the guest frustum while the trailing rendered pose
// still shows it; its captures stop and it tore down in view (and the
// frames just before that alternated perspective / stale caster-bank
// captures; see the caster ingest guard in InterpolateDynamicItems).
// Entries hold the last live NON-caster capture; matched by mesh +
// bone-derived position (clones). Guest render thread only. Characters/
// NPCs are deliberately excluded: their ropa garments cannot be retained
// coherently (a body without its shirt is worse than the teardown).
struct DynRetained {
  DrawItem item;
  uint64_t last_seen = 0;
  float pos[3] = {};  // plausible-bone average (world)
  float half = 2.0f;  // bbox-diagonal half-extent for the frustum test
};
std::vector<DynRetained> g_dyn_retained;

// True when all 8 world-space corners fall outside one clip plane of `vp`
// (row-vector view*proj). `margin` scales the tested frustum: < 1 shrinks
// it (bounds poking just inside an edge still count as outside; the
// game's own cull volumes are tighter than a mesh bbox), > 1 widens it
// (only clearly-outside counts).
bool CornersOutsideFrustum(const float (&corners)[8][3], const float vp[16],
                           float margin) {
  int outside[6] = {};
  for (int c = 0; c < 8; ++c) {
    const float* p = corners[c];
    float clip[4];
    for (int k = 0; k < 4; ++k) {
      clip[k] = p[0] * vp[0 * 4 + k] + p[1] * vp[1 * 4 + k] + p[2] * vp[2 * 4 + k] +
                vp[3 * 4 + k];
    }
    // D3D clip volume: -w <= x <= w, -w <= y <= w, 0 <= z <= w. The game's
    // negative projection x-scale only swaps left/right; the tests are
    // symmetric. Corners behind the camera land in z < 0.
    const float m = margin * clip[3];
    if (clip[0] < -m) ++outside[0];
    if (clip[0] > m) ++outside[1];
    if (clip[1] < -m) ++outside[2];
    if (clip[1] > m) ++outside[3];
    if (clip[2] < 0.0f) ++outside[4];
    if (clip[2] > clip[3]) ++outside[5];
  }
  for (int k = 0; k < 6; ++k) {
    if (outside[k] == 8) {
      return true;
    }
  }
  return false;
}

// The item's world-space bbox against the frustum (statics: bbox is
// mesh-local, world transforms it; fmt-57 absolute geometry carries an
// identity world with world-space bounds).
bool ItemOutsideFrustum(const DrawItem& it, const float vp[16], float margin) {
  float corners[8][3];
  for (int c = 0; c < 8; ++c) {
    const float l[3] = {c & 1 ? it.bbox_max[0] : it.bbox_min[0],
                        c & 2 ? it.bbox_max[1] : it.bbox_min[1],
                        c & 4 ? it.bbox_max[2] : it.bbox_min[2]};
    const float* w = it.world;
    for (int k = 0; k < 3; ++k) {
      corners[c][k] = l[0] * w[0 * 4 + k] + l[1] * w[1 * 4 + k] +
                      l[2] * w[2 * 4 + k] + w[3 * 4 + k];
    }
  }
  return CornersOutsideFrustum(corners, vp, margin);
}

// Axis-aligned cube of half-extent `half` around `center` against the
// frustum, the skinned-item variant (a palette's world position lives in
// its bone translations, not the identity item world).
bool BoxOutsideFrustum(const float center[3], float half, const float vp[16],
                       float margin) {
  float corners[8][3];
  for (int c = 0; c < 8; ++c) {
    corners[c][0] = center[0] + (c & 1 ? half : -half);
    corners[c][1] = center[1] + (c & 2 ? half : -half);
    corners[c][2] = center[2] + (c & 4 ? half : -half);
  }
  return CornersOutsideFrustum(corners, vp, margin);
}

// SynPanAngleDeg / SynPanView: native/skate3_native_diagnostics.cpp.

// ---- High-rate camera sampler ---------------------------------------------
// Pose samples taken once per RENDERED frame alias as soon as the render
// rate drops near/below the sim's camera tick (~200 Hz): at the 140 fps
// pacing cap some frames contain one sim step of rotation and some two, all
// timestamped on the even frame grid; the smoother then faithfully renders
// a 140-vs-200 Hz beat as CONSTANT judder. This thread samples the guest
// ViewCamera at ~1 kHz with precise host timestamps, so the reconstruction
// signal is always finer than the sim tick regardless of render cadence.
// Torn reads (the sim thread writes the matrix concurrently) are rejected
// by a double-read compare.
struct RawCamSample {
  double t;
  float view[16];
  float proj[16];
};
std::atomic<uint32_t> g_sampler_viewcam{0};  // published by BuildFrameScene
std::mutex g_cam_samples_mutex;
std::vector<RawCamSample> g_cam_samples;  // pending; drained by SmoothCamera
std::atomic<bool> g_cam_sampler_started{false};
std::atomic<uint64_t> g_cam_sampler_pushes{0};  // telemetry

void CamSamplerLoop() {
  float last_view[16] = {};
  double syn_next_emit = 0.0;
  int syn_cadence_i = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!SceneEnabled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    // Synthetic pan mode 3: synthesize guest-like pose samples instead of
    // reading the guest camera, ~200 Hz with a deliberately irregular
    // cadence (the real sim tick is irregular; the smoother must cope). The
    // downstream path (ring, period EMA, playback clock, slerp) runs
    // unmodified on them, and its output is compared numerically against
    // the ideal pose in BuildFrameScene.
    if (g_synpan_active.load(std::memory_order_acquire) == 3) {
      static constexpr double kCadenceMs[6] = {4.0, 5.0, 6.0, 4.0, 6.0, 5.0};
      const double now =
          std::chrono::duration<double>(PerfClock::now().time_since_epoch()).count();
      if (syn_next_emit == 0.0) {
        syn_next_emit = now;
      }
      if (now < syn_next_emit) {
        continue;
      }
      syn_next_emit += kCadenceMs[syn_cadence_i++ % 6] * 1e-3;
      if (syn_next_emit < now) {
        syn_next_emit = now;  // fell behind (OS scheduling hitch): re-anchor
      }
      RawCamSample s;
      s.t = now;
      {
        std::lock_guard<std::mutex> lock(g_synpan_mutex);
        const double rate = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate);
        const double amp = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp);
        SynPanView(SynPanAngleDeg((now - g_synpan_t0) * rate, amp), s.view);
        std::memcpy(s.proj, g_synpan_proj0, sizeof(s.proj));
      }
      std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
      if (g_cam_samples.size() < 256) {
        g_cam_samples.push_back(s);
        g_cam_sampler_pushes.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    syn_next_emit = 0.0;
    const uint32_t vc = g_sampler_viewcam.load(std::memory_order_relaxed);
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (vc == 0 || base == nullptr) {
      continue;
    }
    // view at +0x20, proj at +0x60: one 128-byte guarded copy covers both.
    uint32_t raw_a[32], raw_b[32];
    if (!GuestTryCopy(raw_a, base + vc + 0x20, sizeof(raw_a)) ||
        !GuestTryCopy(raw_b, base + vc + 0x20, sizeof(raw_b)) ||
        std::memcmp(raw_a, raw_b, sizeof(raw_a)) != 0) {
      continue;  // unreadable or torn mid-write: try again next tick
    }
    const double now = std::chrono::duration<double>(
                           PerfClock::now().time_since_epoch())
                           .count();
    RawCamSample s;
    s.t = now;
    for (int i = 0; i < 16; ++i) {
      s.view[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[i]));
      s.proj[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[16 + i]));
    }
    // Sanity: perspective proj + a plausible rotation row (stale viewcam
    // addresses after a map change read garbage until re-published).
    const float r0n = s.view[0] * s.view[0] + s.view[1] * s.view[1] +
                      s.view[2] * s.view[2];
    if (s.proj[2 * 4 + 3] != 1.0f || r0n < 0.9f || r0n > 1.1f) {
      continue;
    }
    if (std::memcmp(last_view, s.view, sizeof(last_view)) == 0) {
      continue;  // pose unchanged
    }
    std::memcpy(last_view, s.view, sizeof(last_view));
    // Camera-signal recorder: every DISTINCT pose the guest produced, with
    // the sampler's precise host timestamp (kind 0).
    if (now < g_camsig_deadline.load(std::memory_order_relaxed)) {
      CamSigPush(now, 0.0, YawFromViewRows(s.view), 0);
    }
    std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
    if (g_cam_samples.size() < 256) {
      g_cam_samples.push_back(s);
      g_cam_sampler_pushes.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void EnsureCamSampler() {
  if (g_cam_sampler_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::thread(CamSamplerLoop).detach();
  REXLOG_INFO("native-scene: camera sampler thread started (~1 kHz)");
}

// Rebuild a row-vector view matrix from pose (rows 0..2 = R, row 3 = -c*R)
// and produce view_proj = view x proj.
void ComposeViewProj(const CamPose& p, const float proj[16], float vp_out[16],
                     float cam_out[3]) {
  float v[16] = {};
  ViewRotFromQuat(p.q, v);
  v[3] = v[7] = v[11] = 0.0f;
  for (int k = 0; k < 3; ++k) {
    v[12 + k] = -(p.c[0] * v[0 * 4 + k] + p.c[1] * v[1 * 4 + k] + p.c[2] * v[2 * 4 + k]);
  }
  v[15] = 1.0f;
  for (int r = 0; r < 4; ++r) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += v[r * 4 + k] * proj[k * 4 + col];
      }
      vp_out[r * 4 + col] = sum;
    }
  }
  std::memcpy(cam_out, p.c, 3 * sizeof(float));
}

// Drone / free-fly camera: overrides the published pose with a user-flown
// one while the game keeps simulating on its own camera. Engaged from the
// current guest pose (roll is dropped; the gameplay camera never rolls),
// the projection is frozen at engage so gameplay FOV changes don't disturb
// the shot, and both linear and angular key input drive exponentially
// smoothed velocities for drone-like motion.
//
// Guest-side culling is handled by taking over the game's own render
// camera: the frame build publishes the flown view matrix below, and the
// Sk8::Render::ViewCamera::SetViewMatrix override (bottom of this file)
// rewrites the game's main-camera argument with it, so the game derives
// its view-proj, frustum planes and cull/submit lists from the drone pose
// itself; statics, characters and props are all submitted exactly as if
// the game were looking there. (A draw-item union like the synthetic-pan
// probe's cannot do this: it can only re-append items the game already
// submitted from poses it actually rendered, and rigid props that ride
// entity animation smear at their stale transforms.)
std::mutex g_freecam_guest_mutex;
float g_freecam_guest_view[16] = {};  // row-vector view, host endianness
float g_freecam_guest_pos[3] = {};    // camera world position
std::atomic<uint32_t> g_freecam_guest_active{0};
std::atomic<uint64_t> g_freecam_guest_rewrites{0};
struct FreecamState {
  bool engaged = false;
  double yaw = 0.0, pitch = 0.0;  // radians: yaw about world Y, then pitch
  double pos[3] = {};
  double vel[3] = {};                     // smoothed world-space velocity
  double yaw_vel = 0.0, pitch_vel = 0.0;  // smoothed key-look rates (rad/s)
  double zoom = 1.0;                      // projection scale (Z/X keys)
  float proj0[16] = {};                   // projection frozen at engage
  // Basis and screen-direction signs resolved numerically at engage (the
  // view right/up column signs against cross-product candidates, and the
  // projection's x/y scale signs), so no handedness convention is assumed;
  // the game's projection carries a negative x scale.
  float sign_right = 1.0f, sign_up = 1.0f;
  double look_sign_x = 1.0, look_sign_y = 1.0;
  double last_t = 0.0;
#if defined(_WIN32)
  bool mouse_anchored = false;
  POINT mouse_last = {};
#endif
};
FreecamState g_freecam;

// Guest render thread only. Returns true while engaged; scene.view_proj,
// scene.proj and scene.cam_pos then hold the flown pose.
bool UpdateFreecam(FrameScene& scene, const float cam_view[16], double now) {
  FreecamState& fc = g_freecam;
  if (!REXCVAR_GET(skate3_native_render_scene_freecam)) {
    if (fc.engaged) {
      fc.engaged = false;
      g_freecam_guest_active.store(0, std::memory_order_release);
      REXLOG_INFO(
          "native-scene freecam: off (guest camera restored; {} guest "
          "SetViewMatrix rewrites while engaged)",
          g_freecam_guest_rewrites.exchange(0, std::memory_order_relaxed));
    }
    return false;
  }
  if (!fc.engaged) {
    // Engage from this frame's raw guest pose. The view's upper-3x3 columns
    // are the camera axes in world space (row-vector convention):
    // 0 = right, 1 = up, 2 = forward.
    const float f0[3] = {cam_view[2], cam_view[6], cam_view[10]};
    fc.yaw = std::atan2(double(f0[0]), double(f0[2]));
    fc.pitch = std::asin(std::clamp(double(f0[1]), -1.0, 1.0));
    for (int j = 0; j < 3; ++j) {
      fc.pos[j] = -(cam_view[12] * cam_view[j * 4 + 0] +
                    cam_view[13] * cam_view[j * 4 + 1] +
                    cam_view[14] * cam_view[j * 4 + 2]);
    }
    // Right candidate cross(world_up, forward) = (f.z, 0, -f.x): its sign
    // against the live right column fixes the basis handedness.
    fc.sign_right = f0[2] * cam_view[0] - f0[0] * cam_view[8] >= 0.0f ? 1.0f : -1.0f;
    const float r0[3] = {f0[2] * fc.sign_right, 0.0f, -f0[0] * fc.sign_right};
    const float u0[3] = {f0[1] * r0[2] - f0[2] * r0[1],
                         f0[2] * r0[0] - f0[0] * r0[2],
                         f0[0] * r0[1] - f0[1] * r0[0]};
    fc.sign_up = u0[0] * cam_view[1] + u0[1] * cam_view[5] + u0[2] * cam_view[9] >= 0.0f
                     ? 1.0f
                     : -1.0f;
    std::memcpy(fc.proj0, scene.proj, sizeof(fc.proj0));
    // Screen-relative look/strafe directions: view +x maps to screen right
    // only when the projection x scale is positive (it isn't in this game),
    // so fold the projection signs in once. +yaw rotates forward toward +x
    // (the right column direction times sign_right), hence the product.
    fc.look_sign_x = double(fc.sign_right) * (fc.proj0[0] >= 0.0f ? 1.0 : -1.0);
    fc.look_sign_y = double(fc.sign_up) * (fc.proj0[5] >= 0.0f ? 1.0 : -1.0);
    fc.vel[0] = fc.vel[1] = fc.vel[2] = 0.0;
    fc.yaw_vel = fc.pitch_vel = 0.0;
    fc.zoom = 1.0;
    fc.last_t = now;
#if defined(_WIN32)
    fc.mouse_anchored = false;
#endif
    fc.engaged = true;
    REXLOG_INFO(
        "native-scene freecam: ENGAGED at ({:.1f}, {:.1f}, {:.1f}); WASD fly, "
        "E/Space up, Q/C down, arrows/right-drag look, Z/X zoom, Shift fast, "
        "Ctrl slow",
        fc.pos[0], fc.pos[1], fc.pos[2]);
  }
  const double dt = std::clamp(now - fc.last_t, 0.0, 0.1);
  fc.last_t = now;

  double mv_f = 0.0, mv_r = 0.0, mv_u = 0.0;  // move intent (camera-relative)
  double lk_yaw = 0.0, lk_pitch = 0.0;        // arrow-key look intent
  double mouse_yaw = 0.0, mouse_pitch = 0.0;  // right-drag deltas (radians)
  double speed_mult = 1.0, zoom_dir = 0.0;
#if defined(_WIN32)
  const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
  mv_f = (down('W') ? 1.0 : 0.0) - (down('S') ? 1.0 : 0.0);
  mv_r = (down('D') ? 1.0 : 0.0) - (down('A') ? 1.0 : 0.0);
  mv_u = ((down('E') || down(VK_SPACE)) ? 1.0 : 0.0) -
         ((down('Q') || down('C')) ? 1.0 : 0.0);
  lk_yaw = (down(VK_RIGHT) ? 1.0 : 0.0) - (down(VK_LEFT) ? 1.0 : 0.0);
  lk_pitch = (down(VK_UP) ? 1.0 : 0.0) - (down(VK_DOWN) ? 1.0 : 0.0);
  zoom_dir = (down('Z') ? 1.0 : 0.0) - (down('X') ? 1.0 : 0.0);
  if (down(VK_SHIFT)) {
    speed_mult = 4.0;
  } else if (down(VK_CONTROL)) {
    speed_mult = 0.2;
  }
  if (down(VK_RBUTTON)) {
    POINT p;
    if (GetCursorPos(&p)) {
      if (fc.mouse_anchored) {
        constexpr double kRadPerPixel = 0.0022;  // ~0.13 deg per pixel
        mouse_yaw = (p.x - fc.mouse_last.x) * kRadPerPixel;
        mouse_pitch = -(p.y - fc.mouse_last.y) * kRadPerPixel;
      }
      fc.mouse_last = p;
      fc.mouse_anchored = true;
    }
  } else {
    fc.mouse_anchored = false;
  }
#endif

  // Look: arrow keys drive a smoothed angular velocity (cinematic ease-in/
  // out); mouse deltas apply directly. Pitch is clamped short of the poles.
  const double look_rate =
      REXCVAR_GET(skate3_native_render_scene_freecam_look_speed) *
      (3.14159265358979323846 / 180.0);
  const double ang_k = dt > 0.0 ? 1.0 - std::exp(-dt / 0.08) : 0.0;
  fc.yaw_vel += (lk_yaw * look_rate - fc.yaw_vel) * ang_k;
  fc.pitch_vel += (lk_pitch * look_rate - fc.pitch_vel) * ang_k;
  fc.yaw += (fc.yaw_vel * dt + mouse_yaw) * fc.look_sign_x;
  fc.pitch += (fc.pitch_vel * dt + mouse_pitch) * fc.look_sign_y;
  constexpr double kPitchMax = 89.0 * 3.14159265358979323846 / 180.0;
  fc.pitch = std::clamp(fc.pitch, -kPitchMax, kPitchMax);
  if (zoom_dir != 0.0) {
    // Zoom-in only: the guest camera (and therefore the game's culling)
    // keeps the unzoomed projection, so a narrower rendered FOV is always a
    // subset of what was culled; zooming wider would show unculled edges.
    fc.zoom = std::clamp(fc.zoom * std::exp(zoom_dir * dt), 1.0, 4.0);
  }

  // Basis from yaw/pitch (unit length by construction, signs from engage).
  const double cy = std::cos(fc.yaw), sy = std::sin(fc.yaw);
  const double cp = std::cos(fc.pitch), sp = std::sin(fc.pitch);
  const float fwd[3] = {float(sy * cp), float(sp), float(cy * cp)};
  const float right[3] = {float(cy) * fc.sign_right, 0.0f,
                          float(-sy) * fc.sign_right};
  const float up[3] = {float(-sp * sy) * fc.sign_right * fc.sign_up,
                       float(cp) * fc.sign_right * fc.sign_up,
                       float(-sp * cy) * fc.sign_right * fc.sign_up};

  // Move: forward along the view direction (true drone flight), strafe
  // along screen right, vertical along world up; combined intent normalized
  // so diagonals don't fly faster. Exponential ease toward the target.
  const double ilen = std::sqrt(mv_f * mv_f + mv_r * mv_r + mv_u * mv_u);
  const double speed = REXCVAR_GET(skate3_native_render_scene_freecam_speed) *
                       speed_mult * (ilen > 1.0 ? 1.0 / ilen : 1.0);
  const double strafe = mv_r * fc.look_sign_x * speed;
  const double tvel[3] = {mv_f * speed * fwd[0] + strafe * right[0],
                          mv_f * speed * fwd[1] + mv_u * speed,
                          mv_f * speed * fwd[2] + strafe * right[2]};
  const double vel_k = dt > 0.0 ? 1.0 - std::exp(-dt / 0.25) : 0.0;
  for (int j = 0; j < 3; ++j) {
    fc.vel[j] += (tvel[j] - fc.vel[j]) * vel_k;
    fc.pos[j] += fc.vel[j] * dt;
  }

  // Compose and publish: view columns = right/up/forward, translation row
  // = -pos * R; projection = the engage-frozen one with the zoom folded in
  // (published to scene.proj too so depth-based post passes unproject with
  // what was actually rendered).
  float view[16] = {};
  for (int i = 0; i < 3; ++i) {
    view[i * 4 + 0] = right[i];
    view[i * 4 + 1] = up[i];
    view[i * 4 + 2] = fwd[i];
  }
  const float posf[3] = {float(fc.pos[0]), float(fc.pos[1]), float(fc.pos[2])};
  for (int k = 0; k < 3; ++k) {
    view[12 + k] = -(posf[0] * view[0 * 4 + k] + posf[1] * view[1 * 4 + k] +
                     posf[2] * view[2 * 4 + k]);
  }
  view[15] = 1.0f;
  // Hand the flown view to the guest-camera takeover (the SetViewMatrix
  // override): the game's next camera update culls/submits with this pose.
  {
    std::lock_guard<std::mutex> lock(g_freecam_guest_mutex);
    std::memcpy(g_freecam_guest_view, view, sizeof(view));
    std::memcpy(g_freecam_guest_pos, posf, sizeof(posf));
  }
  g_freecam_guest_active.store(1, std::memory_order_release);
  float pr[16];
  std::memcpy(pr, fc.proj0, sizeof(pr));
  pr[0] *= float(fc.zoom);
  pr[5] *= float(fc.zoom);
  std::memcpy(scene.proj, pr, sizeof(pr));
  for (int r = 0; r < 4; ++r) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += view[r * 4 + k] * pr[k * 4 + col];
      }
      scene.view_proj[r * 4 + col] = sum;
    }
  }
  std::memcpy(scene.cam_pos, posf, sizeof(posf));
  return true;
}

// While the freecam is engaged, fills out_pos with the flown camera world
// position and returns true. Consumed by the draw-distance hooks to
// recenter the guest distance culls on the drone.
bool FreecamGuestPose(float out_pos[3]) {
  if (g_freecam_guest_active.load(std::memory_order_acquire) == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_freecam_guest_mutex);
  std::memcpy(out_pos, g_freecam_guest_pos, 3 * sizeof(float));
  return true;
}

bool LoadingOrFrontendActive() {
  if (rex::kernel::guest_presence::GameplayContextValue() != 0) {
    return false;
  }
  // Same publish-staleness window as YieldForMenus: the pause menu keeps
  // the world resubmitting perspective scenes behind it, loading screens
  // and the frontend stop publishing.
  const int64_t last_ns = g_last_publish_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return true;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  return now_ns - last_ns >= 300'000'000;
}

// Guest render thread only. Returns true when vp_out/cam_out hold a
// re-timed pose for `now`; false = keep the raw guest pose (no history yet,
// or a cut/teleport snapped).
//
// Dejitter structure (the naive last-two-samples lerp still juddered):
// sample DETECTION times are quantized to render frames (+-2.5 ms of noise
// on a ~4.5 ms camera tick), and re-basing the timeline on each new sample
// jerks the output whenever sample spacing varies. Instead: a pose RING
// timestamped at detection, an EMA of the sample period, and a playback
// clock that advances on real frame time and only slews gently toward
// (newest - 2 periods). The bracketing ring samples are slerped at the
// playback time; sample-time noise is absorbed by the slew instead of
// feeding straight into the pose velocity.
// Most recent camera sim-tick time (guest render thread only): entities that
// changed pose this frame changed on the same sim tick; timestamping their
// pose rings with it instead of the frame time avoids the same aliasing.
double g_latest_cam_tick = 0.0;

bool SmoothCamera(const float view[16], const float proj[16], const float raw_vp[16],
                  const float raw_cam[3], double now, float vp_out[16],
                  float cam_out[3]) {
  constexpr int kRing = 64;  // ~1 kHz sampler: cover well past the lag window
  static CamPose s_ring[kRing];
  static int s_count = 0;  // -1 = convention drift, never smooth this run
  static int s_newest = 0;
  static float s_last_view[16] = {};
  static double s_period = 1.0 / 200.0;  // EMA of the guest camera tick
  static double s_play = 0.0;            // playback clock (guest-pose time axis)
  static double s_last_now = 0.0;
  static bool s_play_valid = false;

  const auto push_pose = [&](const CamPose& p) {
    if (s_count < 0) {
      return;
    }
    const CamPose& latest = s_ring[s_newest];
    const float dot = std::fabs(p.q[0] * latest.q[0] + p.q[1] * latest.q[1] +
                                p.q[2] * latest.q[2] + p.q[3] * latest.q[3]);
    const float dx = p.c[0] - latest.c[0], dy = p.c[1] - latest.c[1],
                dz = p.c[2] - latest.c[2];
    const float dist2 = dx * dx + dy * dy + dz * dz;
    if (s_count == 0 || p.t - latest.t > 0.1 || dist2 > 25.0f || dot < 0.9f) {
      // First sample / stale history / teleport / hard cut: snap.
      s_ring[0] = p;
      s_newest = 0;
      s_count = 1;
      s_play_valid = false;
    } else {
      const double dt = p.t - latest.t;
      if (dt <= 0.0) {
        return;  // out-of-order/duplicate
      }
      if (dt > 0.0005 && dt < 0.05) {
        s_period = s_period * 0.9 + dt * 0.1;
      }
      s_newest = (s_newest + 1) % kRing;
      s_ring[s_newest] = p;
      s_count = std::min(s_count + 1, kRing);
    }
    g_latest_cam_tick = p.t;
  };

  // Primary source: the ~1 kHz sampler thread (precise timestamps: pose
  // samples taken per RENDERED frame alias against the ~200 Hz sim tick as
  // soon as the render rate is paced down; see CamSamplerLoop).
  {
    static std::vector<RawCamSample> s_pending;
    {
      std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
      s_pending.swap(g_cam_samples);
    }
    for (const RawCamSample& s : s_pending) {
      CamPose p;
      p.t = s.t;
      QuatFromView(s.view, p.q);
      for (int j = 0; j < 3; ++j) {
        p.c[j] = -(s.view[12] * s.view[j * 4 + 0] + s.view[13] * s.view[j * 4 + 1] +
                   s.view[14] * s.view[j * 4 + 2]);
      }
      push_pose(p);
    }
    s_pending.clear();
  }

  if (std::memcmp(s_last_view, view, sizeof(s_last_view)) != 0) {
    std::memcpy(s_last_view, view, sizeof(s_last_view));
    CamPose p;
    p.t = now;
    QuatFromView(view, p.q);
    std::memcpy(p.c, raw_cam, sizeof(p.c));
    // One-shot reconstruction self-check: the rebuilt view_proj at this
    // exact pose must match the guest's own (catches any convention drift).
    static bool s_checked = false;
    if (!s_checked) {
      s_checked = true;
      float test_vp[16], test_cam[3];
      ComposeViewProj(p, proj, test_vp, test_cam);
      float max_rel = 0.0f;
      for (int i = 0; i < 16; ++i) {
        const float mag = std::max(1.0f, std::fabs(raw_vp[i]));
        max_rel = std::max(max_rel, std::fabs(test_vp[i] - raw_vp[i]) / mag);
      }
      REXLOG_INFO("native-scene: smooth-camera reconstruction check max_rel={:.6f}{}",
                  max_rel, max_rel > 0.01f ? " (BAD - smoothing disabled this run)" : "");
      if (max_rel > 0.01f) {
        s_count = -1;
      }
    }
    // Fallback only: if the sampler has fed the ring recently, the frame-
    // grid pose is a stale duplicate of a sampler pose; pushing it would
    // corrupt the timestamps.
    if (s_count >= 0 && (s_count == 0 || now - s_ring[s_newest].t > 0.05)) {
      push_pose(p);
    }
  }
  if (s_count < 3 || now - s_ring[s_newest].t > 0.1) {
    s_play_valid = false;
    g_smooth_active = false;
    return false;
  }

  // Playback clock: advance by real frame time; slew gently toward the
  // target lag point so detection-time noise never becomes pose velocity.
  const double frame_dt =
      s_play_valid ? std::clamp(now - s_last_now, 0.0, 0.05) : 0.0;
  s_last_now = now;
  // Boxcar pose filter (see below): the playback point must lag far enough
  // that the CENTERED window has ring samples on both sides.
  const double filter_w = std::clamp(
      REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) *
      1e-3;
  const double lag = std::max(2.0 * s_period, filter_w * 0.5 + s_period);
  const double target = s_ring[s_newest].t - lag;
  if (!s_play_valid) {
    s_play = target;
    s_play_valid = true;
  } else {
    s_play += frame_dt;
    const double err = target - s_play;
    if (err > 0.1 || err < -0.1) {
      s_play = target;  // fell far behind/ahead (hitch): snap the clock
    } else {
      s_play += err * 0.06;
    }
  }
  // Never play past the newest sample: when panning STOPS, samples stop
  // arriving while the clock keeps advancing; extrapolating overshot the
  // stop pose and the staleness cutoff then snapped back ("catch-up judder
  // when I stop panning"). Parked at the newest sample, the smoothed pose
  // settles exactly onto the raw stop pose and every later handoff is
  // seamless.
  s_play = std::min(s_play, s_ring[s_newest].t);

  // Evaluate the piecewise-linear pose signal at an arbitrary time by
  // bracketing in the ring (samples are time-ordered oldest -> newest) and
  // slerping. Clamps to the ring's ends.
  const auto eval_ring = [&](double t_eval) {
    int hi = s_newest;
    int lo = (s_newest + kRing - 1) % kRing;
    for (int step = 1; step < s_count - 1; ++step) {
      if (s_ring[lo].t <= t_eval) {
        break;
      }
      hi = lo;
      lo = (lo + kRing - 1) % kRing;
    }
    const CamPose& p0 = s_ring[lo];
    const CamPose& p1 = s_ring[hi];
    const double span = std::max(p1.t - p0.t, 0.0005);
    const double alpha = std::clamp((t_eval - p0.t) / span, 0.0, 1.0);
    CamPose r;
    r.t = t_eval;
    QuatSlerp(p0.q, p1.q, float(alpha), r.q);
    for (int k = 0; k < 3; ++k) {
      r.c[k] = p0.c[k] + (p1.c[k] - p0.c[k]) * float(alpha);
    }
    return r;
  };

  CamPose p;
  if (filter_w > 0.0005 && s_count >= 4) {
    // Boxcar pose filter: average the interpolated signal over a window
    // CENTERED on the playback point (zero phase error at constant
    // velocity). Root-caused need (camera-signal recordings): the
    // game's camera pose VALUES advance in 60 Hz-quantized lumps at high
    // render rates; during a measured constant stick pan the per-tick
    // angular velocity had sd 144 deg/s on a 172 deg/s mean, +-2.2 deg off
    // a constant-rate path. A 50 ms window (three 60 Hz periods) nulls the
    // quantization at any render rate: measured frame-to-frame velocity
    // jitter fell 185 -> 7 deg/s rms on the recorded signal. Taps past the
    // newest sample clamp to it, so a pan STOP still settles exactly onto
    // the raw stop pose (never extrapolates).
    constexpr int kTaps = 16;
    double qacc[4] = {};
    double cacc[3] = {};
    float qref[4] = {};
    for (int k = 0; k < kTaps; ++k) {
      const double tt = std::min(s_play - filter_w * 0.5 + (k + 0.5) * filter_w / kTaps,
                                 s_ring[s_newest].t);
      const CamPose s = eval_ring(tt);
      float sq[4] = {s.q[0], s.q[1], s.q[2], s.q[3]};
      if (k == 0) {
        std::memcpy(qref, sq, sizeof(qref));
      } else if (sq[0] * qref[0] + sq[1] * qref[1] + sq[2] * qref[2] +
                     sq[3] * qref[3] <
                 0.0f) {
        for (float& v : sq) v = -v;
      }
      for (int j = 0; j < 4; ++j) qacc[j] += sq[j];
      for (int j = 0; j < 3; ++j) cacc[j] += s.c[j];
    }
    const double qn = std::sqrt(qacc[0] * qacc[0] + qacc[1] * qacc[1] +
                                qacc[2] * qacc[2] + qacc[3] * qacc[3]);
    p.t = s_play;
    for (int j = 0; j < 4; ++j) {
      p.q[j] = float(qacc[j] / std::max(qn, 1e-12));
    }
    for (int j = 0; j < 3; ++j) {
      p.c[j] = float(cacc[j] / kTaps);
    }
  } else {
    p = eval_ring(s_play);
  }
  ComposeViewProj(p, proj, vp_out, cam_out);
  g_smooth_play = s_play;
  g_smooth_pose = p;
  g_smooth_active = true;
  return true;
}

// Minimum time spacing (seconds) between dynamic-pose ring ingests. The
// ring is consumed per CHANGE, and above ~200 fps the guest re-packs poses
// every rendered frame; without a spacing floor the ring's fixed capacity
// covers ever less real time as the frame rate climbs, until the smoothing
// kernel's ~55 ms reach falls off the old end (the high-fps skater/board
// rubberband). 3.5 ms keeps the kept signal finer than the guest's
// ~200 Hz pose tick while bounding capacity use; at ~285 fps and below
// every change still ingests, so the tuned mid-rate behavior is untouched.
// Applies to POSE ingests only: ropa cloth-decode enqueues must NOT be
// decimated (see the enqueue block; pose/shape pairing relies on their
// per-frame cadence).
constexpr double kMinDynIngestSpacing = 0.0035;

// Interpolate the DYNAMIC items' per-draw state (bone palettes; rigid
// non-identity world matrices) at the camera's playback time. Without this
// the smoothed camera glides while the skater/NPCs/props snap at the guest
// sim tick; the stepping that used to be hidden (camera and entities
// stepped IN PHASE) becomes visible entity judder. History pairs poses by
// (mesh, k-th occurrence this frame): clones publish in stable submit
// order, so the k-th copy pairs with last frame's k-th copy. Componentwise
// lerp is exact enough at adjacent-sim-tick deltas (~5 ms of motion).
// Guest render thread only.
void InterpolateDynamicItems(uint8_t* base, FrameScene& scene, double now) {
  // Per-entity pose RING, like the camera's: the playback clock sits ~2 sim
  // periods behind `now`, so a two-pose history never brackets it (the
  // interpolation alpha pinned at 0 and entities rendered STALE STEPPED
  // poses, visible judder and motion blur). The camera filter
  // pushes the play clock ~(W/2 + period) back AND the entity boxcar
  // (below) reaches another W/2 past that (~55 ms total at the default
  // 50 ms window). With ingests spaced at least kMinDynIngestSpacing apart,
  // 24 slots guarantee >= ~80 ms of coverage at ANY render rate. The shared
  // g_smooth_play keeps entities in phase with the camera.
  constexpr int kRing = 24;
  struct DynPose {
    double t = 0.0;
    std::vector<float> b;  // bone palette (skinned), raw captured rows
    float w[16] = {};      // world (rigid)
    // ROPA: the newest cloth-shape generation (dyn job seq) that existed
    // when this pose was captured, the shape that belongs WITH this pose
    // (constant enqueue offset; the draw lerps the bracketing generations).
    uint64_t shape_seq = 0;
  };
  struct DynHist {
    DynPose ring[kRing];
    int count = 0;
    int newest = 0;
    uint64_t seen = 0;
    // EMA of this entity's own pose-change period. Characters/board update
    // at the 60 Hz sim value cadence (~16.7 ms); traffic vehicles update
    // SLOWER (their own sim rate); those entities need their evaluation
    // point delayed by one own-period or the playback clock runs past
    // their newest sample and they render raw stepped poses (see play_e).
    double period = 0.0;
    // Per-bone skin-weighted vertex centroids in BIND space (w, x, y, z),
    // lazily computed from the mesh's own vertex buffer the first time a
    // bone of this entity trips the spin-collapse guard. The centroid IS
    // the wheel's geometric center (verified in capture:
    // wheel-mesh bone centroids match the motion-solved spin pivot to a
    // millimeter), so the collapse guard can pin it to the boxcar path
    // exactly, no runtime estimation (every estimator variant tested was
    // noisier than the artifact it fixed).
    uint32_t cen_vb = 0;
    uint32_t cen_bytes = 0;
    std::vector<std::array<float, 4>> cen;
    // Timestamp of the last PERSPECTIVE-sourced (non-caster, non-retained)
    // sample ingested: decides whether a caster pose tracks (caster-only
    // stream) or holds (perspective stream fresh; interleaving the two
    // sawtooths the wheel phase).
    double last_persp_t = 0.0;
  };
  static std::unordered_map<uint64_t, DynHist> s_hist;
  static uint64_t s_frame = 0;
  ++s_frame;
  // Bone-signal recorder window state (see BoneSigAppend). Written on a
  // detached thread once the window closes.
  const bool bs_rec = BoneSigTick(now);
  static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // ---- char_track diagnostics (skate3_native_render_scene_char_track) ----
  // One log line per frame following the main character's ropa garment and
  // body through this function: which events fired, what the ring held, and
  // where the item actually rendered. Selection: garment = nearest ropa
  // item to the camera; body = the highest-bone-count skinned non-ropa
  // character item within 5 m (bone count separates the body from the
  // board/accessories at equal distance).
  struct CharTrack {
    int idx = -1;
    float raw[3] = {};     // pre-interp reference position (bone 0 / world t)
    int ing = 0;           // 1 = pose ingested, 2 = change skipped (spacing)
    int rst = 0;           // discontinuity reset fired
    int clm = 0;           // 1 = claimed another ring, 2 = fresh ring slot
    int hold = 0;          // caster/retained hold replaced the pose
    int raw_render = 0;    // 0 = interpolated; 1 = ring<3/stale, 2 = size
                           // mismatch, 3 = hidden (unrepairable)
    int ring_n = 0;        // ring occupancy at entry
    float age_ms = -1.0f;  // ring-newest age at entry
    float per_ms = -1.0f;  // ring period EMA
    float ba = -1.0f;      // pair-lerp alpha at the playback point
    int box = 0;           // took the 8-tap boxcar
    int detach = 0;        // ropa DETACH repair fired
    float sw = 0.0f;       // shape-kernel total weight (garment only)
  };
  CharTrack trk_shirt, trk_body;
  const bool trk_on = REXCVAR_GET(skate3_native_render_scene_char_track);
  if (trk_on) {
    float best_s = 25.0f;  // 5 m gate, squared
    uint32_t best_b_bones = 0;
    float best_b_d2 = 25.0f;
    for (size_t i = 0; i < scene.items.size(); ++i) {
      const DrawItem& it = scene.items[i];
      const bool skn = it.skinned && it.bones.size() >= 12;
      float p[3];
      if (skn) {
        p[0] = it.bones[3];
        p[1] = it.bones[7];
        p[2] = it.bones[11];
      } else {
        p[0] = it.world[12];
        p[1] = it.world[13];
        p[2] = it.world[14];
      }
      const float dx = p[0] - scene.cam_pos[0];
      const float dy = p[1] - scene.cam_pos[1];
      const float dz = p[2] - scene.cam_pos[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (it.ropa) {
        if (d2 < best_s) {
          best_s = d2;
          trk_shirt.idx = int(i);
          std::memcpy(trk_shirt.raw, p, sizeof(p));
        }
      } else if (skn && it.char_family >= 1 && it.char_family <= 5 &&
                 d2 < 25.0f) {
        const uint32_t nb = uint32_t(it.bones.size() / 12);
        if (nb > best_b_bones || (nb == best_b_bones && d2 < best_b_d2)) {
          best_b_bones = nb;
          best_b_d2 = d2;
          trk_body.idx = int(i);
          std::memcpy(trk_body.raw, p, sizeof(p));
        }
      }
    }
  }
  std::unordered_map<uint32_t, uint32_t> occurrence;
  for (DrawItem& item : scene.items) {
    CharTrack* trk = nullptr;
    if (trk_on) {
      const int idx = int(&item - scene.items.data());
      if (idx == trk_shirt.idx) {
        trk = &trk_shirt;
      } else if (idx == trk_body.idx) {
        trk = &trk_body;
      }
    }
    const bool skinned = item.skinned && !item.bones.empty();
    const bool rigid_dyn =
        !skinned && std::memcmp(item.world, kIdent, sizeof(kIdent)) != 0;
    if (!skinned && !rigid_dyn) {
      continue;
    }
    const uint32_t k = occurrence[item.mesh]++;
    // Skinned pose-to-pose translation distance^2 at a NON-SPINNING
    // reference. Fast wheel bones sweep multi-meter circles between ~8 ms
    // samples (the model->world affine's translation carries the axle-
    // pivot compensation; measured 2.9 m single-sample swings on traffic
    // at speed), and bone 0 IS a wheel on the vehicle meshes, so keying
    // the teleport gate on it reset the ring every few samples and
    // traffic spent most of its time on raw stepped poses ("the whole
    // vehicle lags then jumps to catch up"; the board's centimeter wheel
    // offsets never trip it). A genuine teleport/mispair moves EVERY
    // bone, spin moves only the wheels: with the entity's vertex-weighted
    // bone set known (the centroid table), take the MINIMUM jump over
    // real bones; until it exists, bone 0.
    const auto skinned_dist2 = [&](const std::vector<float>& a,
                                   const std::vector<float>& b,
                                   const DynHist& hh) -> float {
      const auto bone_d2 = [&](size_t bone) -> float {
        const size_t bi = bone * 12;
        const float dx = a[bi + 3] - b[bi + 3];
        const float dy = a[bi + 7] - b[bi + 7];
        const float dz = a[bi + 11] - b[bi + 11];
        return dx * dx + dy * dy + dz * dz;
      };
      const size_t nbones = a.size() / 12;
      float best = 1e30f;
      const size_t ncen = std::min(hh.cen.size(), nbones);
      for (size_t bone = 0; bone < ncen; ++bone) {
        if (hh.cen[bone][0] > 0.5f) {
          best = std::min(best, bone_d2(bone));
        }
      }
      return best < 1e30f ? best : bone_d2(0);
    };
    // Distance^2 from this item's new pose to a history's newest pose;
    // 1e30 when the history is empty, stale, or a different palette size.
    const auto hist_dist2 = [&](const DynHist& hh) -> float {
      if (hh.count == 0) {
        return 1e30f;
      }
      const DynPose& lp = hh.ring[hh.newest];
      if (now - lp.t > 0.1) {
        return 1e30f;
      }
      if (skinned) {
        if (lp.b.size() != item.bones.size() || lp.b.size() < 12) {
          return 1e30f;
        }
        return skinned_dist2(item.bones, lp.b, hh);
      }
      const float dx = item.world[12] - lp.w[12];
      const float dy = item.world[13] - lp.w[13];
      const float dz = item.world[14] - lp.w[14];
      const float d2 = dx * dx + dy * dy + dz * dz;
      // Rotation-aware for rigid: mirrored clone twins can sit at the SAME
      // translation (railing pairs), and a translation-only metric pairs
      // their rings freely - the interpolator then lerps across the flip.
      // Fold the rotation delta in as an equivalent distance; a mispair
      // (elementwise delta up to 2.0) lands far outside every claim cap,
      // while a physics prop's one-tick rotation stays well inside.
      float rd = 0.0f;
      for (int r = 0; r < 3; ++r) {
        for (int c2 = 0; c2 < 3; ++c2) {
          rd = std::max(rd, std::fabs(item.world[r * 4 + c2] -
                                      lp.w[r * 4 + c2]));
        }
      }
      return std::max(d2, rd * rd * 4.0f);
    };
    // One-time bind-space vertex-centroid table for this entity (see the
    // pivot-boxcar comment at the collapse guard). Also computed EAGERLY on
    // an entity's first pose so the teleport gate's non-spinning reference
    // (skinned_dist2) exists before the first interpolated frame; a car
    // first seen at full speed otherwise reset its ring off the wheel
    // swings forever and never reached the collapse path that used to
    // build this table.
    const auto ensure_cen = [&](DynHist& hh, size_t nbones) {
          if ((hh.cen_vb != item.vb_addr || hh.cen_bytes != item.vb_bytes) &&
              item.stride != 0 && item.bw_offset != 0 && item.bi_offset != 0) {
            // One-time bind-space centroid pass over this entity's vertex
            // buffer (guest thread; raw guest reads are safe here, same
            // as RefinePaletteBase). u8x4 attributes are big-endian per
            // 32-bit word: component k is byte (24 - 8k) of the host-order
            // load.
            hh.cen_vb = item.vb_addr;
            hh.cen_bytes = item.vb_bytes;
            hh.cen.assign(nbones, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            const uint32_t vcount =
                std::min<uint32_t>(item.vb_bytes / item.stride, 200000);
            for (uint32_t vtx = 0; vtx < vcount; ++vtx) {
              SkinSampleVert sv;
              if (!ReadSkinVertGuest(base, item, vtx, &sv)) {
                break;  // unsupported position format
              }
              for (int k = 0; k < 4; ++k) {
                const uint32_t wgt = sv.w[k];
                if (wgt == 0) {
                  continue;
                }
                const uint32_t bone = sv.bone[k];
                if (bone >= nbones) {
                  continue;
                }
                auto& cb = hh.cen[bone];
                const float wf = float(wgt) * (1.0f / 255.0f);
                cb[0] += wf;
                cb[1] += wf * sv.p[0];
                cb[2] += wf * sv.p[1];
                cb[3] += wf * sv.p[2];
              }
            }
            for (auto& cb : hh.cen) {
              if (cb[0] > 0.5f) {
                cb[1] /= cb[0];
                cb[2] /= cb[0];
                cb[3] /= cb[0];
              }
            }
          }
    };
    // LW-mapped items key their ring by the game's own per-instance
    // identity (the MeshContext); sort-
    // list clone reshuffles cannot mispair an identity key, so the whole
    // positional claim search below is skipped for them. Mesh stays in the
    // key so a model/LOD swap on the same instance starts a fresh ring
    // (pose sizes differ). RIGID items with a ctx take the identity key
    // too: park-editor venues submit dozens of same-mesh piece clones,
    // including mirrored twins at the SAME translation, and any camera pan
    // reshuffles the occurrence ordinals - the positional claim below then
    // swaps the twins' rings every frame (translation distance 0) and the
    // interpolator lerps their 180-degree-apart rotations, the visible
    // piece spin. Legacy (mesh, occurrence) pairing continues to serve
    // everything without a usable ctx (player, CAC, ctx-less captures).
    bool lw_keyed = item.ctx != 0 &&
                    ((item.lw_entity != 0 &&
                      REXCVAR_GET(skate3_native_render_scene_lw_identity)) ||
                     !skinned);
    uint64_t key = lw_keyed ? ((1ull << 63) | (uint64_t(item.ctx) << 32) |
                               uint64_t(item.mesh))
                            : ((uint64_t(item.mesh) << 8) | (k & 0xFF));
    DynHist* hp = &s_hist[key];
    if (lw_keyed && hp->seen == s_frame) {
      // One ctx published twice in a frame (should not happen; dyn_slot
      // dedups per ctx): fall back to the legacy pairing for this copy
      // rather than double-ingesting the identity ring.
      lw_keyed = false;
      key = (uint64_t(item.mesh) << 8) | (k & 0xFF);
      hp = &s_hist[key];
    }
    const float own_d2 = hp->seen == s_frame ? 1e30f : hist_dist2(*hp);
    if (!lw_keyed && own_d2 > 1e-4f) {
      // The k-th slot mispairs: the game's sort lists RESHUFFLE same-mesh
      // clones as they and the camera move. For static props the
      // reset-on-jump guard below was enough (a mispair rendered raw for a
      // few frames), but driving traffic reshuffles CONSTANTLY; the rings
      // never accumulated 3 poses and every moving car rendered raw
      // stepped poses (the vehicle judder/catch-up). Re-pair by POSITION
      // instead: claim the unclaimed history of this mesh whose newest
      // pose is nearest, within the same 1.5 m one-tick jump gate.
      // The search runs whenever the slot is not an (almost) EXACT
      // continuation, not only past the 1.5 m gate: clone placements
      // CLOSER than 1.5 m (the paired newspaper holders, 0.78 m apart)
      // otherwise inherit each other's history on every reshuffle, and the
      // interpolator renders them sliding between the two placements ("the
      // props jitter in and out of position"). Seeding `best` with the own
      // slot's distance keeps a claim strictly-nearer-only, so a genuinely
      // moving entity still prefers its own ring.
      // RIGID claims get a far tighter cap than the vehicles' 1.5 m: when a
      // close clone's OWN ring goes stale (its submission flapped for a few
      // frames), strictly-nearer alone still claims the TWIN's fresh ring
      // (own = 1e30) and slides once, the residual single jitter seen
      // after the strictly-nearer fix. No rigid prop moves 0.3 m in one sim
      // tick (18 m/s at 60 Hz), while clone placements sit >= 0.78 m apart;
      // a rigid item with no history inside 0.3 m starts a fresh ring and
      // renders raw at its correct placement instead.
      const float claim_cap = skinned || item.ropa ? 2.25f : 0.09f;
      float best = std::min(own_d2, claim_cap);
      DynHist* alt = nullptr;
      uint64_t alt_key = key;
      uint32_t fresh_k = 256;  // first index past the dense key range
      for (uint32_t k2 = 0; k2 < 256; ++k2) {
        const uint64_t key2 = (uint64_t(item.mesh) << 8) | k2;
        const auto it2 = s_hist.find(key2);
        if (it2 == s_hist.end()) {
          fresh_k = k2;  // occurrence keys are dense per mesh
          break;
        }
        if (k2 == (k & 0xFF) || it2->second.seen == s_frame) {
          continue;
        }
        const float d2 = hist_dist2(it2->second);
        if (d2 < best) {
          best = d2;
          alt = &it2->second;
          alt_key = key2;
        }
      }
      if (alt != nullptr) {
        hp = alt;
        key = alt_key;
        if (trk != nullptr) {
          trk->clm = 1;
        }
      } else if (hp->seen == s_frame && fresh_k < 256) {
        if (trk != nullptr) {
          trk->clm = 2;
        }
        // The k-th slot already belongs to another clone this frame and no
        // history matches: start a fresh ring instead of corrupting the
        // claimed one with interleaved poses.
        key = (uint64_t(item.mesh) << 8) | fresh_k;
        hp = &s_hist[key];
      }
    }
    DynHist& h = *hp;
    h.seen = s_frame;
    if (trk != nullptr) {
      trk->ring_n = h.count;
      trk->per_ms = float(h.period * 1e3);
      if (h.count > 0) {
        trk->age_ms = float((now - h.ring[h.newest].t) * 1e3);
      }
    }
    if (skinned) {
      ensure_cen(h, item.bones.size() / 12);
    }
    const DynPose& latest = h.ring[h.newest];
    // Per-bone palette repair (vehicles): the sighting captures carried
    // garbage on SOME weighted bones, world positions in the rotation
    // rows, bone 0 gliding along the vehicle's own path, while every
    // sampled-vert gate upstream judged only the bones its samples happen
    // to reference. The survivors rendered as mangled vehicles with zero
    // refusals in the telemetry. Repair the insane bones from the ring's
    // newest pose (the body keeps its live motion; a repaired wheel
    // freezes for the frames its rows are junk); with no sane source for
    // a weighted bone, hide the item for the frame instead.
    if (skinned && (item.char_family == 6 || item.char_family == 7) &&
        !h.cen.empty()) {
      const size_t nbones = item.bones.size() / 12;
      const size_t ncen = std::min(h.cen.size(), nbones);
      const bool ring_ok = h.count > 0 && latest.b.size() == item.bones.size();
      const auto rows_sane = [](const float* bones, size_t bi) {
        for (int r = 0; r < 3; ++r) {
          const float* row = bones + bi + size_t(r) * 4;
          const float n2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
          if (n2 < 0.04f || n2 > 25.0f) {
            return false;
          }
        }
        return true;
      };
      uint32_t repaired = 0;
      bool unrepairable = false;
      for (size_t b = 0; b < ncen && !unrepairable; ++b) {
        if (h.cen[b][0] <= 0.5f) {
          continue;  // not vertex-weighted: staging leftovers are normal
        }
        const size_t bi = b * 12;
        if (rows_sane(item.bones.data(), bi)) {
          continue;
        }
        if (ring_ok && rows_sane(latest.b.data(), bi)) {
          std::memcpy(item.bones.data() + bi, latest.b.data() + bi,
                      12 * sizeof(float));
          ++repaired;
        } else {
          unrepairable = true;
        }
      }
      if (repaired != 0 || unrepairable) {
        static std::atomic<uint64_t> s_repairs{0};
        const uint64_t n = s_repairs.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: vehicle palette {} mesh={:08X} fam={} src={} "
              "caster={} bones={} (n={})",
              unrepairable ? "UNREPAIRABLE (hidden)" : "bone-repair",
              item.mesh, item.char_family, item.dbg_src,
              item.caster_bank ? 1 : 0, repaired, n);
        }
        if (unrepairable) {
          item.draws.clear();
          continue;
        }
      }
    }
    // Caster-bank palettes carry ~40 ms-stale FINE animation but a fresh
    // GROSS pose, when they are the only capture stream they must keep
    // TRACKING (the old 0.5 s ring-hold froze passing vehicles mid-motion
    // in plain view, and the seed/hold/drift-out cycle re-seeded every
    // ~100 ms: "cars stop in their tracks and vanish"). Only while
    // PERSPECTIVE samples are fresh does a caster pose hold the ring pose
    // instead (ingesting both interleaves a wheel-phase sawtooth), a
    // <= 50 ms hold until the next perspective sample, imperceptible.
    // Retained re-publishes always hold: their stored pose is frames old
    // and ingesting it would step the ring backward. Ropa garments
    // included: the gate requires SKINNED mode with an identical palette
    // size, so the substitution cannot mix modes.
    // Stale caster bank: the ortho banks occasionally hold a genuinely OLD
    // palette (not just 40 ms of wheel phase), rendered raw, the vehicle
    // momentarily ghosted 10-20 m back along its own trail. A pose > 3 m
    // from a ring pose younger than 100 ms is physically impossible
    // (> 30 m/s of error); hold the fresh ring pose for the frame. The
    // 30 m ceiling keeps clone ring-swaps (45-75 m in the sighting logs,
    // valid poses of DIFFERENT vehicles) rendering raw.
    const bool caster_stale_jump = [&] {
      if (!skinned || !item.caster_bank || h.count == 0 ||
          latest.b.size() != item.bones.size() || now - latest.t > 0.1) {
        return false;
      }
      const float d2 = skinned_dist2(item.bones, latest.b, h);
      return d2 > 9.0f && d2 < 900.0f;
    }();
    // dbg_src 10 = LW-authoritative palette substitution: the pose IS the
    // entity's current sim tick; holding the ring's (older) pose over it
    // would re-stale exactly what the substitution fixed.
    if (skinned && h.count > 0 && latest.b.size() == item.bones.size() &&
        item.dbg_src != 10 &&
        ((item.caster_bank && now - h.last_persp_t <= 0.05) || item.retained ||
         caster_stale_jump)) {
      if (caster_stale_jump) {
        static std::atomic<uint64_t> s_stale_caster{0};
        const uint64_t n = s_stale_caster.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: stale caster pose held mesh={:08X} fam={} "
              "ring_age_ms={:.0f} (n={})",
              item.mesh, item.char_family, (now - latest.t) * 1e3, n);
        }
      }
      item.bones = latest.b;
      if (trk != nullptr) {
        trk->hold = caster_stale_jump ? 2 : 1;
      }
    }
    const bool changed =
        h.count == 0 ||
        (skinned ? latest.b != item.bones
                 : std::memcmp(latest.w, item.world, sizeof(item.world)) != 0);
    // Timestamp for a new ring pose: the camera sampler's latest sim tick
    // when fresh; this entity's pose changed on the same sim tick, and
    // frame-grid timestamps alias against the sim rate once the render
    // loop is paced (the same problem the camera sampler solves). Frame
    // time otherwise.
    const double stamp =
        (g_latest_cam_tick > 0.0 && now - g_latest_cam_tick < 0.02)
            ? g_latest_cam_tick
            : now;
    // Ingest spacing floor: past ~285 fps the pose changes every rendered
    // frame, FASTER than the ~200 Hz camera tick, so consecutive changed
    // poses would stamp with the SAME tick time (degenerate interpolation
    // spans: the collapsed-bone / cloth-shape pair-lerp alpha pins and the
    // output steps once per slot) while the ring's real-time coverage
    // shrinks below the kernel's reach. Changes closer than the floor to
    // the newest slot are not ingested (their bytes still feed the repair
    // and teleport gates); the next qualifying change carries the signal.
    if (changed &&
        !(h.count > 0 && stamp - latest.t < kMinDynIngestSpacing)) {
      // Discontinuities reset the history (pose-size change = LOD/garment
      // swap; long gap = the entity was gone). CRUCIALLY also a translation
      // jump no entity makes in one sim tick (> 1.5 m): clones pair by
      // occurrence order, and the game's sort lists RESHUFFLE clones as the
      // camera moves; without this guard a vending machine inherited its
      // twin's history and visibly slid/teleported between the two
      // placements. Mispairs now render the raw pose (a no-op for the
      // static props where it was visible).
      if (h.count > 0) {
        bool discontinuity =
            now - latest.t > 0.1 ||
            (skinned && latest.b.size() != item.bones.size());
        if (!discontinuity) {
          // Translation jump at the non-spinning reference (see
          // skinned_dist2; bone 0 is a WHEEL on vehicles); rigid worlds
          // carry t in row 3.
          float d2;
          if (skinned) {
            d2 = skinned_dist2(item.bones, latest.b, h);
          } else {
            const float dx = item.world[12] - latest.w[12];
            const float dy = item.world[13] - latest.w[13];
            const float dz = item.world[14] - latest.w[14];
            d2 = dx * dx + dy * dy + dz * dz;
            // Same rotation fold as hist_dist2: a same-spot mirrored-twin
            // mispair must reset the ring, not lerp across the flip.
            float rd = 0.0f;
            for (int r = 0; r < 3; ++r) {
              for (int c2 = 0; c2 < 3; ++c2) {
                rd = std::max(rd, std::fabs(item.world[r * 4 + c2] -
                                            latest.w[r * 4 + c2]));
              }
            }
            d2 = std::max(d2, rd * rd * 4.0f);
          }
          // Rigid uses the same tight one-tick bound as the claim cap
          // above: 0.78 m clone placements sit inside the vehicles' 1.5 m
          // gate, and lerping across a mispair IS the prop jitter. Ropa
          // garments are EXEMPT from the tight bound even in rigid mode:
          // they ride a character (verified entity-exact worlds, no clone
          // twins to mispair with), and with ingests spaced at the floor a
          // fast skater's world legitimately moves > 0.3 m between ring
          // poses; the tight gate then resets the ring every ingest at
          // speed and the garment renders raw/zero-lag while the body keeps
          // its smoothed lag (the speed-scaled garment jumping). Genuine
          // teleports/respawns still clear the 1.5 m character gate.
          discontinuity = d2 > (skinned || item.ropa ? 2.25f : 0.09f);
          if (discontinuity && !skinned && d2 < 2.25f) {
            // A rigid step that only the tightened bound caught = a
            // close-clone mispair that would have LERPED (the prop jitter).
            static std::atomic<uint64_t> s_rigid_mispair{0};
            const uint64_t n =
                s_rigid_mispair.fetch_add(1, std::memory_order_relaxed);
            if (n < 24 || (n & 255u) == 0) {
              REXLOG_INFO(
                  "native-scene: rigid close-clone mispair reset mesh={:08X} "
                  "k={} d={:.2f}m (n={})",
                  item.mesh, k, std::sqrt(d2), n);
            }
          }
        }
        if (discontinuity) {
          h.count = 0;
          if (trk != nullptr) {
            trk->rst = 1;
          }
        }
      }
      if (trk != nullptr) {
        trk->ing = 1;
      }
      const double prev_t = h.count > 0 ? h.ring[h.newest].t : 0.0;
      h.newest = h.count == 0 ? 0 : (h.newest + 1) % kRing;
      DynPose& p = h.ring[h.newest];
      p.t = stamp;
      if (prev_t > 0.0) {
        // Track the entity's OWN pose-change period (see DynHist::period).
        const double dt = p.t - prev_t;
        if (dt > 0.0005 && dt < 0.1) {
          h.period = h.period == 0.0 ? dt : h.period * 0.75 + dt * 0.25;
        }
      }
      p.b = item.bones;
      std::memcpy(p.w, item.world, sizeof(p.w));
      p.shape_seq = 0;
      if (item.ropa) {
        // Guest thread only, like the enqueue that writes it.
        const auto sit = g_ropa_last_seq.find(item.mesh);
        p.shape_seq = sit != g_ropa_last_seq.end() ? sit->second : 0;
      }
      h.count = std::min(h.count + 1, kRing);
      if (skinned && !item.caster_bank && !item.retained) {
        h.last_persp_t = p.t;
      }
      if (bs_rec) {
        BoneSigAppend(0, key, p.t, 0.0,
                      skinned ? item.bones.data() : item.world,
                      skinned ? uint32_t(item.bones.size()) : 16u);
      }
    } else if (changed && trk != nullptr) {
      trk->ing = 2;  // change withheld by the ingest spacing floor
    }
    if (h.count < 3 || now - h.ring[h.newest].t > 0.1) {
      if (trk != nullptr) {
        trk->raw_render = 1;
      }
      continue;  // not enough history yet: raw stepped pose (one-time snap)
    }
    // Evaluate the ring's piecewise-linear pose signal at time tt into
    // `out_b` (skinned, weighted-accumulated) / `out_w` (rigid), scaled by
    // `weight`. Returns false on a palette-size mismatch inside the window
    // (LOD/garment swap); the caller keeps the raw pose that frame.
    const auto accum_at = [&](double tt, float weight, float* out_b,
                              float out_w[16]) {
      int hi = h.newest;
      int lo = (h.newest + kRing - 1) % kRing;
      for (int step = 1; step < h.count - 1; ++step) {
        if (h.ring[lo].t <= tt) {
          break;
        }
        hi = lo;
        lo = (lo + kRing - 1) % kRing;
      }
      const DynPose& p0 = h.ring[lo];
      const DynPose& p1 = h.ring[hi];
      if (skinned && (p0.b.size() != item.bones.size() ||
                      p1.b.size() != item.bones.size())) {
        return false;
      }
      const double span = std::max(p1.t - p0.t, 0.0005);
      // Clamp at 1.0 (no extrapolation): entities that stop moving must
      // settle exactly onto their raw pose, like the camera.
      const float a = float(std::clamp((tt - p0.t) / span, 0.0, 1.0));
      if (skinned) {
        for (size_t i = 0; i < item.bones.size(); ++i) {
          out_b[i] += (p0.b[i] + (p1.b[i] - p0.b[i]) * a) * weight;
        }
      } else {
        for (int i = 0; i < 16; ++i) {
          out_w[i] += (p0.w[i] + (p1.w[i] - p0.w[i]) * a) * weight;
        }
      }
      return true;
    };
    // Boxcar filter, same as the camera's: the guest's ANIMATION poses are
    // 60 Hz-quantized like its camera; once the camera glides, the 60-vs-
    // render-rate pose alternation reads as skater judder/ghosting against
    // the smooth background. Averaging bone affines componentwise over the
    // window slightly shrinks fast-swinging limb rotations (a subtle motion
    // blur), the trade the game's own 60 Hz presentation makes anyway.
    const double filter_w = std::clamp(
        REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) *
        1e-3;
    // Per-entity playback point. Entities whose own pose stream is SLOWER
    // than the 60 Hz character cadence (traffic vehicles tick on their own
    // sim rate) pin the shared playback clock past their newest sample;
    // alpha clamps at 1.0, the whole smoothing machinery degenerates to
    // raw stepped poses, and every new sample renders as a visible jump
    // (the vehicle judder/catch-up that survived the entity boxcar).
    // Evaluating one own-period earlier keeps the eval point BRACKETED by
    // samples: the staircase renders as continuous piecewise-linear
    // motion, at the cost of that entity lagging one of ITS sim updates
    // behind the world, invisible for background traffic, and never
    // applied to 60 Hz entities (the skater/NPCs keep the shared clock).
    const double play_e =
        g_smooth_play -
        (h.period > 0.020 ? std::min(h.period - 1.0 / 60.0, 0.1) : 0.0);
    static std::vector<float> acc;  // guest render thread only
    float wacc[16] = {};
    bool ok = true;
    // Did the rigid world take the 8-tap boxcar this frame (vs the plain
    // pair-lerp)? The ROPA shape kernel below must match it exactly.
    bool rigid_world_boxcar = false;
    if (filter_w > 0.0005 && h.count >= 4) {
      constexpr int kTaps = 8;
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      for (int tap = 0; tap < kTaps && ok; ++tap) {
        const double tt =
            std::min(play_e - filter_w * 0.5 + (tap + 0.5) * filter_w / kTaps,
                     h.ring[h.newest].t);
        ok = accum_at(tt, 1.0f / kTaps, acc.data(), wacc);
      }
    } else {
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      ok = accum_at(play_e, 1.0f, acc.data(), wacc);
    }
    if (!ok) {
      if (trk != nullptr) {
        trk->raw_render = 2;
      }
      continue;  // palette-size mismatch in the window: raw pose this frame
    }
    if (trk != nullptr) {
      trk->box = filter_w > 0.0005 && h.count >= 4 ? 1 : 0;
    }
    // Fast-spinning bones (skateboard wheels: hundreds of degrees inside
    // the window) COLLAPSE under componentwise averaging; the rotation
    // entries cancel toward zero and the wheel shrinks into the truck /
    // sinks into the ground. Guard per bone: if the averaged 3x3's
    // Frobenius norm dropped versus a raw sample's, the window spans too
    // much rotation; fall back to the plain TWO-ADJACENT-SAMPLE lerp at
    // the playback point for that bone (the pre-filter behavior: samples
    // ~7.5 ms apart lerp with only a few % shrink, and rotation +
    // translation come from the SAME pose pair). Failed alternatives, do
    // not revisit: whole-bone nearest-sample snap = wheels lag the filtered
    // board and snap to catch up; nearest ROTATION + averaged TRANSLATION =
    // wheels ORBIT the axle ~10 cm (palette affines are model->world; the
    // rotation pivots about the MODEL origin and the translation carries
    // the axle-pivot compensation; the pair must stay consistent). Slow
    // bones (the judder sources) pass untouched.
    int b_lo = h.newest, b_hi = h.newest;
    {
      int hi2 = h.newest;
      int lo2 = (h.newest + kRing - 1) % kRing;
      for (int step = 1; step < h.count - 1; ++step) {
        if (h.ring[lo2].t <= play_e) {
          break;
        }
        hi2 = lo2;
        lo2 = (lo2 + kRing - 1) % kRing;
      }
      b_lo = lo2;
      b_hi = hi2;
    }
    const DynPose& q0 = h.ring[b_lo];
    const DynPose& q1 = h.ring[b_hi];
    const double bspan = std::max(q1.t - q0.t, 0.0005);
    const float ba = float(std::clamp((play_e - q0.t) / bspan, 0.0, 1.0));
    if (trk != nullptr) {
      trk->ba = ba;
    }
    if (skinned) {
      if (q0.b.size() == item.bones.size() && q1.b.size() == item.bones.size()) {
        // Pass 1: flag collapsed bones (the churning staged-constant rows
        // past the real skeleton, e.g. rows 53..83 of the 84-row character
        // bank, flag every frame too; they are unreferenced by vertices,
        // so rewriting them is harmless).
        static std::vector<uint8_t> s_collapsed;  // guest render thread only
        const size_t nbones = acc.size() / 12;
        s_collapsed.assign(nbones, 0);
        size_t ncollapsed = 0;
        for (size_t b = 0; b < nbones; ++b) {
          const size_t bi = b * 12;
          double fa = 0.0, fr = 0.0;
          for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
              const float av = acc[bi + r * 4 + c];
              const float rv = q1.b[bi + r * 4 + c];
              fa += double(av) * av;
              fr += double(rv) * rv;
            }
          }
          if (fa < fr * 0.94) {  // norm ratio < ~0.97: rotation collapsed
            s_collapsed[b] = 1;
            ++ncollapsed;
          }
        }
        if (ncollapsed > 0) {
          // Collapsed bone (rotating too fast inside the window to
          // average, spinning wheels, and briefly fast-swinging limbs).
          // Base fallback: plain adjacent-sample lerp at the playback point
          // (R and t from the SAME pose pair, no orbit; ~7.5 ms spacing
          // keeps shrink to a few %). Bones whose mesh vertices yield a
          // skin-weighted BIND-SPACE CENTROID get the exact PIVOT-BOXCAR
          // form instead: render the orthonormalized lerp rotation
          // translated so the centroid rides the same boxcar path as every
          // other filtered bone: t_out = tbar + Rbar*c - Ro*c, where
          // Rbar/tbar is the boxcar affine already in acc[] (its action on
          // ANY fixed bind-space point IS that point's smoothed path). The
          // centroid is the wheel's geometric center (a wheel is symmetric
          // about its axle; verified in capture: mesh
          // centroids match the motion-solved spin pivot to a millimeter),
          // so the wheel is exact at its center and sub-mm across its
          // ~4 cm extent; a limb pins its own centroid to its smoothed
          // path (full-norm rotation, no lerp shrink). Offline-validated:
          // wheel-vs-deck
          // high-frequency wobble 1.3 -> 0.02 cm rms at constant speed /
          // 1.0 -> 0.29 cm at 25 m/s with speed changes, better than the
          // game's own 60 Hz output (0.05-0.56 cm). DO NOT replace the
          // centroid with a motion-ESTIMATED pivot: every estimator shape
          // tried (shared / per-window velocity elimination, quadratic
          // motion models, theta gates, consistency resets, axis
          // projection) was noisier than the artifact it fixed.
          // Junk palette rows
          // (unreferenced by vertices) get no centroid weight and keep the
          // lerp.
          ensure_cen(h, nbones);
          uint64_t pivot_upgraded = 0;
          for (size_t b = 0; b < nbones; ++b) {
            if (!s_collapsed[b]) {
              continue;
            }
            const size_t bi = b * 12;
            float lp[12];
            for (int i = 0; i < 12; ++i) {
              lp[i] = q0.b[bi + i] + (q1.b[bi + i] - q0.b[bi + i]) * ba;
            }
            const bool upgraded = [&]() {
              if (filter_w <= 0.0005 || b >= h.cen.size() ||
                  h.cen[b][0] <= 0.5f) {
                return false;  // no centroid (junk row / undecodable VB)
              }
              const float c[3] = {h.cen[b][1], h.cen[b][2], h.cen[b][3]};
              const float* bar = acc.data() + bi;
              // UNDERSAMPLED spin: traffic wheels at speed turn > 90 deg
              // between adjacent ~8 ms samples; the pair-lerp rotation is
              // then meaningless, and with a car wheel's bind centroid
              // meters from the model origin the (Rbar - Ro)*c pin wobbled
              // the wheel ~1 m around its well (bone-signal measured; the
              // board's slow wheels never hit this). When the pair spans
              // more than ~50 deg (Frobenius dot of the 3x3s: trace(R0^T
              // R1) = 1 + 2cos(theta) for rotations), render the pair's
              // NEWEST rotation instead; spin phase snaps once per
              // sample, invisible at those rev rates, while the centroid
              // pin still holds the wheel exactly on its smoothed path.
              double pair_tr = 0.0;
              for (int i = 0; i < 12; ++i) {
                if ((i & 3) == 3) {
                  continue;  // translation column
                }
                pair_tr += double(q0.b[bi + i]) * q1.b[bi + i];
              }
              const bool snap_spin = pair_tr < 2.28;  // 1 + 2cos(50 deg)
              const float* rsrc = snap_spin ? q1.b.data() + bi : lp;
              // Orthonormalize the source rotation (rows): recovers the
              // full-norm midpoint rotation from the lerp's shrunk chord
              // (or just cleans up the raw newest sample in snap mode).
              double r0[3] = {rsrc[0], rsrc[1], rsrc[2]};
              double r1[3] = {rsrc[4], rsrc[5], rsrc[6]};
              double n0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
              if (n0 < 1e-4) {
                return false;
              }
              for (double& v : r0) v /= n0;
              const double d01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
              for (int i = 0; i < 3; ++i) r1[i] -= d01 * r0[i];
              const double n1 =
                  std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
              if (n1 < 1e-4) {
                return false;
              }
              for (double& v : r1) v /= n1;
              const double r2[3] = {r0[1] * r1[2] - r0[2] * r1[1],
                                    r0[2] * r1[0] - r0[0] * r1[2],
                                    r0[0] * r1[1] - r0[1] * r1[0]};
              const double* rows[3] = {r0, r1, r2};
              // t_out = tbar + Rbar*c - Ro*c: the pivot lands exactly on
              // the boxcar path (in phase with the deck and every other
              // filtered bone), spin phase comes from the lerp pair.
              for (int r = 0; r < 3; ++r) {
                double rb = 0.0, ro = 0.0;
                for (int c2 = 0; c2 < 3; ++c2) {
                  rb += double(bar[r * 4 + c2]) * c[c2];
                  ro += rows[r][c2] * c[c2];
                }
                acc[bi + r * 4 + 0] = float(rows[r][0]);
                acc[bi + r * 4 + 1] = float(rows[r][1]);
                acc[bi + r * 4 + 2] = float(rows[r][2]);
                acc[bi + r * 4 + 3] = float(double(bar[r * 4 + 3]) + rb - ro);
              }
              return true;
            }();
            if (upgraded) {
              ++pivot_upgraded;
            } else {
              for (int i = 0; i < 12; ++i) {
                acc[bi + i] = lp[i];
              }
            }
          }
          // Sparse telemetry: how many bones ride the pivot upgrade.
          static uint64_t s_pivot_frames = 0, s_pivot_bones = 0;
          s_pivot_bones += pivot_upgraded;
          if (++s_pivot_frames % 3000 == 0 && s_pivot_bones > 0) {
            REXLOG_INFO("native-scene pivot: {} bone-upgrades / 3000 frames",
                        s_pivot_bones);
            s_pivot_bones = 0;
          }
        }
      }
      std::copy(acc.begin(), acc.end(), item.bones.begin());
    } else {
      double fa = 0.0, fr = 0.0;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          fa += double(wacc[r * 4 + c]) * wacc[r * 4 + c];
          fr += double(q1.w[r * 4 + c]) * q1.w[r * 4 + c];
        }
      }
      if (fa < fr * 0.94) {
        for (int i = 0; i < 16; ++i) {
          item.world[i] = q0.w[i] + (q1.w[i] - q0.w[i]) * ba;
        }
      } else {
        std::memcpy(item.world, wacc, sizeof(wacc));
        rigid_world_boxcar = filter_w > 0.0005 && h.count >= 4;
      }
      // Post-interpolation identity anchor (ropa garments): the published
      // world and the guest draws are proven bit-exact against the owner
      // entity, so a rendered world grossly diverged from the entity here
      // can only be ring corruption (mispaired claim / stale pose blended
      // in). Motion smoothing legitimately offsets by centimeters; the
      // thresholds (2.5 m translation, ~60 deg rotation) only catch the
      // detached/perpendicular class. Repair = the entity's live matrix
      // (raw for one frame beats a detached garment), and the log names
      // the ring state for diagnosis.
      if (item.ropa) {
        float ent_rows[12];
        if (skate3::native_entity::ReadEntityWorldRows(base, item.ctx,
                                                       ent_rows)) {
          const float dx = item.world[12] - ent_rows[3];
          const float dy = item.world[13] - ent_rows[7];
          const float dz = item.world[14] - ent_rows[11];
          float rot = 0.0f;
          for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
              const float d = item.world[i * 4 + j] - ent_rows[j * 4 + i];
              rot += d * d;
            }
          }
          if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f || rot > 1.5f) {
            static std::atomic<uint32_t> s_fix{0};
            const uint32_t ln = s_fix.fetch_add(1, std::memory_order_relaxed);
            if (ln < 16 || (ln & 255u) == 0) {
              REXLOG_INFO(
                  "native-scene: ropa interp DETACH repaired mesh={:08X} "
                  "ctx={:08X} dt=({:.2f},{:.2f},{:.2f}) rot2={:.2f} ba={:.2f} "
                  "ring[n={} q0t=({:.1f},{:.1f},{:.1f}) "
                  "q1t=({:.1f},{:.1f},{:.1f})] (n={})",
                  item.mesh, item.ctx, dx, dy, dz, rot, ba, h.count, q0.w[12],
                  q0.w[13], q0.w[14], q1.w[12], q1.w[13], q1.w[14], ln);
            }
            for (int i = 0; i < 3; ++i) {
              for (int j = 0; j < 3; ++j) {
                item.world[i * 4 + j] = ent_rows[j * 4 + i];
              }
              item.world[i * 4 + 3] = 0.0f;
              item.world[12 + i] = ent_rows[i * 4 + 3];
            }
            item.world[15] = 1.0f;
            rigid_world_boxcar = false;
            if (trk != nullptr) {
              trk->detach = 1;
            }
          }
        }
      }
    }
    // ROPA shape pairing: express the EXACT temporal kernel the garment's
    // world (and the body's bones) were just evaluated with as weights over
    // the decoded shape generations (DynDecodeJob seq). The previous plain
    // 2-generation lerp removed the stepping but reconstructed the 60 Hz
    // limb signal SHARPLY against the 50 ms boxcar-rounded body, two
    // different frequency responses to the same signal, diverging at every
    // piecewise-linear corner by an excursion that scales with the per-tick
    // step size (the residual tee jelly, worse at a 60 fps guest cap;
    // emulated applies no filter at all and shows none). The kernel follows
    // the world's LIVE decision: boxcar taps only when the world actually
    // took the boxcar this frame (not the spin-collapse pair-lerp
    // fallback), so cloth and body always ride the same filter.
    if (item.ropa && !skinned) {
      item.shape_count = 0;
      const int bias = std::clamp<int>(
          REXCVAR_GET(skate3_native_render_scene_ropa_bias), -2, 2);
      // Bracket the ring at tt exactly like accum_at (same walk, same span
      // clamp) and return the bracketing pair's shape generations + alpha.
      const auto shape_at = [&](double tt, uint64_t& s0, uint64_t& s1,
                                float& a) {
        int hi = h.newest;
        int lo = (h.newest + kRing - 1) % kRing;
        for (int step = 1; step < h.count - 1; ++step) {
          if (h.ring[lo].t <= tt) {
            break;
          }
          hi = lo;
          lo = (lo + kRing - 1) % kRing;
        }
        // Live pairing trim (skate3_native_render_scene_ropa_bias): step
        // the shape source N ring poses fresher/older than the bracket.
        int blo = lo, bhi = hi;
        for (int b = 0; b < bias; ++b) {
          if (bhi == h.newest) break;
          blo = bhi;
          bhi = (bhi + 1) % kRing;
        }
        for (int b = 0; b > bias; --b) {
          bhi = blo;
          blo = (blo + kRing - 1) % kRing;
        }
        const DynPose& p0 = h.ring[blo];
        const DynPose& p1 = h.ring[bhi];
        if (p0.shape_seq == 0 || p1.shape_seq == 0) {
          return false;
        }
        const double span = std::max(h.ring[hi].t - h.ring[lo].t, 0.0005);
        s0 = p0.shape_seq;
        s1 = p1.shape_seq;
        a = float(std::clamp((tt - h.ring[lo].t) / span, 0.0, 1.0));
        return true;
      };
      const auto add_gen = [&](uint64_t seq, float wgt) {
        if (wgt <= 0.0f) {
          return;
        }
        for (int k = 0; k < item.shape_count; ++k) {
          if (item.shape_seq[k] == seq) {
            item.shape_w[k] += wgt;
            return;
          }
        }
        if (item.shape_count < DrawItem::kShapeGens) {
          item.shape_seq[item.shape_count] = seq;
          item.shape_w[item.shape_count] = wgt;
          ++item.shape_count;
        }
        // Unreachable: the 8 taps contribute at most two distinct
        // generations each, and kShapeGens holds all 16.
      };
      if (rigid_world_boxcar &&
          REXCVAR_GET(skate3_native_render_scene_ropa_boxcar)) {
        constexpr int kShapeTaps = 8;  // == the body kernel's kTaps
        for (int tap = 0; tap < kShapeTaps; ++tap) {
          const double tt = std::min(
              play_e - filter_w * 0.5 + (tap + 0.5) * filter_w / kShapeTaps,
              h.ring[h.newest].t);
          uint64_t s0 = 0, s1 = 0;
          float a = 0.0f;
          if (shape_at(tt, s0, s1, a)) {
            add_gen(s0, (1.0f - a) / kShapeTaps);
            add_gen(s1, a / kShapeTaps);
          }
        }
      } else {
        uint64_t s0 = 0, s1 = 0;
        float a = 0.0f;
        if (shape_at(play_e, s0, s1, a)) {
          add_gen(s0, 1.0f - a);
          add_gen(s1, a);
        }
      }
      if (trk != nullptr) {
        for (int k = 0; k < item.shape_count; ++k) {
          trk->sw += item.shape_w[k];
        }
      }
    }
    if (bs_rec) {
      BoneSigAppend(1, key, now, play_e,
                    skinned ? item.bones.data() : item.world,
                    skinned ? uint32_t(item.bones.size()) : 16u);
    }
  }
  // char_track emit: two lines per frame (body, then garment). rp = the
  // pre-interp capture, fp = the post-interp (rendered) reference position,
  // off = |garment - body| rendered offset; its frame-to-frame stability
  // IS the artifact metric.
  if (trk_on && (trk_shirt.idx >= 0 || trk_body.idx >= 0)) {
    const auto fpos = [&](const CharTrack& t, const DrawItem*& it,
                          float p[3]) {
      it = nullptr;
      p[0] = p[1] = p[2] = 0.0f;
      if (t.idx < 0 || size_t(t.idx) >= scene.items.size()) {
        return;
      }
      it = &scene.items[size_t(t.idx)];
      if (it->skinned && it->bones.size() >= 12) {
        p[0] = it->bones[3];
        p[1] = it->bones[7];
        p[2] = it->bones[11];
      } else {
        p[0] = it->world[12];
        p[1] = it->world[13];
        p[2] = it->world[14];
      }
    };
    const DrawItem* si = nullptr;
    const DrawItem* bi = nullptr;
    float sp[3], bp[3];
    fpos(trk_shirt, si, sp);
    fpos(trk_body, bi, bp);
    if (bi != nullptr) {
      const CharTrack& t = trk_body;
      REXLOG_INFO(
          "char-track f={} body mesh={:08X} ctx={:08X} fam={} src={} cb={} "
          "rp=({:.3f},{:.3f},{:.3f}) fp=({:.3f},{:.3f},{:.3f}) ring[n={} "
          "age={:.1f} per={:.1f} ing={} rst={} clm={} hold={} raw={}] "
          "ba={:.2f} box={} play={:.1f}",
          s_frame, bi->mesh, bi->ctx, bi->char_family, bi->dbg_src,
          bi->caster_bank ? 1 : 0, t.raw[0], t.raw[1], t.raw[2], bp[0], bp[1],
          bp[2], t.ring_n, t.age_ms, t.per_ms, t.ing, t.rst, t.clm, t.hold,
          t.raw_render, t.ba, t.box, (now - g_smooth_play) * 1e3);
    }
    if (si != nullptr) {
      const CharTrack& t = trk_shirt;
      float ent_t[3] = {0.0f, 0.0f, 0.0f};
      int ent_ok = 0;
      if (si->ctx != 0) {
        float rows[12];
        if (skate3::native_entity::ReadEntityWorldRows(base, si->ctx, rows)) {
          ent_t[0] = rows[3];
          ent_t[1] = rows[7];
          ent_t[2] = rows[11];
          ent_ok = 1;
        }
      }
      const float off =
          bi != nullptr
              ? std::sqrt((sp[0] - bp[0]) * (sp[0] - bp[0]) +
                          (sp[1] - bp[1]) * (sp[1] - bp[1]) +
                          (sp[2] - bp[2]) * (sp[2] - bp[2]))
              : -1.0f;
      REXLOG_INFO(
          "char-track f={} shirt mesh={:08X} ctx={:08X} fam={} src={} skn={} "
          "cb={} rt={} rp=({:.3f},{:.3f},{:.3f}) fp=({:.3f},{:.3f},{:.3f}) "
          "ent=({:.3f},{:.3f},{:.3f},{}) ring[n={} age={:.1f} per={:.1f} "
          "ing={} rst={} clm={} hold={} raw={}] ba={:.2f} box={} sc={} "
          "sw={:.2f} det={} off={:.3f}",
          s_frame, si->mesh, si->ctx, si->char_family, si->dbg_src,
          si->skinned && !si->bones.empty() ? 1 : 0, si->caster_bank ? 1 : 0,
          si->retained ? 1 : 0, t.raw[0], t.raw[1], t.raw[2], sp[0], sp[1],
          sp[2], ent_t[0], ent_t[1], ent_t[2], ent_ok, t.ring_n, t.age_ms,
          t.per_ms, t.ing, t.rst, t.clm, t.hold, t.raw_render, t.ba, t.box,
          si->shape_count, t.sw, t.detach, off);
    }
  }
  // Prune entities not seen recently (map otherwise grows with streaming).
  if (s_hist.size() > 2048) {
    for (auto it = s_hist.begin(); it != s_hist.end();) {
      it = it->second.seen + 60 < s_frame ? s_hist.erase(it) : std::next(it);
    }
  }
}

// Publish the frame's 2D overlay draws (BuildFrameScene, before any early
// return; menu and empty frames still carry 2D). The inline-ring vertex
// payloads are complete by frame end; convert them to little-endian and
// expand quad lists into triangle lists so the render side stays trivial.
void Publish2dDraws(uint8_t* base) {
  std::vector<Draw2d> frame_2d;
  {
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    frame_2d.swap(g_frame_2d);
  }
  static thread_local std::vector<uint8_t> scratch_2d;
  std::vector<Draw2d> published;
  published.reserve(frame_2d.size());
  for (Draw2d& d : frame_2d) {
    // OFFSCREEN COMPOSITION draws: bracket bits carrying ONLY SimpleDraw
    // (0x20) / font (0x10) with none of the screen-pass brackets (bit 0
    // FrontEndManager::Render2D, bit 1 AptMovieIntegration, bit 2
    // DrawRenderingUnit, bit 3 the HUD render-to-texture pass) are the
    // game's internal render-target helpers, not screen UI; the
    // skater-portrait generator composes the card through bare SimpleDraw
    // quads in the TARGET's coordinate space (traced:
    // flags=20 fullscreen 1152x640 postfx blit + centered
    // 324x640 portrait compose quads on textures 03f47054/03f3f054,
    // replayed on screen they were the centered "poster" flash on every
    // menu entry / skater switch, sampling whatever the resolve arena
    // still held). Every real UI draw in gameplay, pause and the frontend
    // carries at least one screen bracket (observed flags 0d/19/29/2b/2d).
    // flags == 0 (unbracketed boot/loading capture) keeps its own gate.
    if (d.flags != 0 && (d.flags & 0x0Fu) == 0) {
      static std::atomic<uint32_t> s_offscreen_2d{0};
      const uint32_t n = s_offscreen_2d.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 2047u) == 0) {
        REXLOG_INFO(
            "native-scene: offscreen 2D compose draw dropped (flags={:02x} "
            "tex={:08x} count={}) (n={})",
            d.flags, d.fetch[1], d.count, n);
      }
      g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const uint32_t bytes = d.count * d.stride;
    scratch_2d.resize(bytes);
    if (!GuestTryCopy(scratch_2d.data(), base + d.addr, bytes)) {
      continue;
    }
    // Guest dwords are big-endian.
    for (size_t i = 0; i + 4 <= scratch_2d.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, scratch_2d.data() + i, 4);
      v = BSwap32(v);
      std::memcpy(scratch_2d.data() + i, &v, 4);
    }
    // Verified capture-side vertex layouts, all normalized to one 28-byte
    // renderer layout {float4 pos, float2 uv, u32 rgba}:
    //   24-byte {float4 pos, float2 uv}          - APT elements
    //   20-byte {float3 pos, float2 uv}          - glyph text (bit 4)
    //   20-byte {float4 pos, u32 color}          - SimpleDraw untextured
    //   28-byte {float4 pos, u32 color, float2 uv} - SimpleDraw textured
    //   16-byte {float4 pos}                     - SimpleDraw solid fill
    //     (color rides in VS c8 = m[8]; the popup panel strips, Rewards
    //     title bar / teal body / tan footer, are these)
    // (SimpleDraw = bit 5; its DrawParameters ctor orders colours before
    // texcoords.)
    {
      static thread_local std::vector<uint8_t> norm_2d;
      norm_2d.resize(size_t(d.count) * 28);
      const float one = 1.0f;
      const float zero = 0.0f;
      const uint32_t white = 0xFFFFFFFFu;
      const bool font = (d.flags & 0x10u) != 0;
      const bool simple = (d.flags & 0x20u) != 0;
      bool ok = true;
      for (uint32_t v = 0; v < d.count && ok; ++v) {
        uint8_t* dst = norm_2d.data() + size_t(v) * 28;
        const uint8_t* src = scratch_2d.data() + size_t(v) * d.stride;
        // Unbracketed captures (d.flags == 0, native loading/boot frames)
        // take the SimpleDraw layout readings for 16/28: EA's inline-quad
        // helpers order color before texcoords everywhere.
        if (d.stride == 16 && (simple || d.flags == 0)) {  // pos4, untextured
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, &zero, 4);
          std::memcpy(dst + 20, &zero, 4);
          std::memcpy(dst + 24, &white, 4);
        } else if (d.stride == 24) {  // APT: pos4 + uv (with or without the
          // SimpleDraw bracket; the compass needle/icons are 24-byte
          // quads issued through SimpleDraw::Draw inside the HUD pass,
          // same simpledraw_SimpleDrawUVSC shader as plain APT elements;
          // requiring !simple here dropped them, a regression from adding
          // bracket bit 5)
          std::memcpy(dst, src, 24);
          std::memcpy(dst + 24, &white, 4);
        } else if (d.stride == 20 && font) {  // glyphs: pos3 + uv
          std::memcpy(dst, src, 12);
          std::memcpy(dst + 12, &one, 4);
          std::memcpy(dst + 16, src + 12, 8);
          std::memcpy(dst + 24, &white, 4);
        } else if (d.stride == 20 && simple) {  // SimpleDraw: pos4 + color
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, &zero, 4);
          std::memcpy(dst + 20, &zero, 4);
          // The dword byteswap reversed the color's guest byte order
          // (r,g,b,a); restore it for R8G8B8A8_UNORM.
          dst[24] = src[19];
          dst[25] = src[18];
          dst[26] = src[17];
          dst[27] = src[16];
        } else if (d.stride == 28 && (simple || d.flags == 0)) {  // pos4+color+uv
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, src + 20, 8);
          dst[24] = src[19];
          dst[25] = src[18];
          dst[26] = src[17];
          dst[27] = src[16];
        } else {
          ok = false;
        }
      }
      if (!ok) {
        g_draws_2d_other.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (d.stride == 16) {
        // Untextured fill: the captured fetch words are leftover state from
        // the previous textured draw; zero them so the replay binds the
        // white texture instead of tinting the fill with a stale texel.
        std::memset(d.fetch, 0, sizeof(d.fetch));
      }
      scratch_2d = norm_2d;
      d.src_stride = d.stride;
      d.stride = 28;
    }
    if (d.prim == 13 && d.count % 4 == 0) {
      // Quad list -> triangle list (v0,v1,v2)(v0,v2,v3).
      const uint32_t quads = d.count / 4;
      d.verts.resize(size_t(quads) * 6 * d.stride);
      static constexpr uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
      for (uint32_t q = 0; q < quads; ++q) {
        for (uint32_t t = 0; t < 6; ++t) {
          std::memcpy(d.verts.data() + (size_t(q) * 6 + t) * d.stride,
                      scratch_2d.data() + (size_t(q) * 4 + kOrder[t]) * d.stride,
                      d.stride);
        }
      }
      d.prim = 4;
      d.count = quads * 6;
    } else if (d.prim == 4 || d.prim == 5) {
      d.verts.assign(scratch_2d.begin(), scratch_2d.end());
    } else {
      continue;
    }
    // BIG-QUAD TRACER (reported symptom: an unrelated "mongo poster" texture
    // flashes portrait-shaped at screen center on team/import entry and
    // skater switches; idle F11s never catch it). Edge-triggered: log each
    // TEXTURED replayed 2D draw whose transformed extent covers >= 8% of
    // the 1280x720 APT space (the portrait box itself is ~8%), once per
    // texture base per 5 s window. Names the quad's texture / bracket /
    // geometry / timing for the fix.
    if (d.fetch[0] != 0) {
      float mn[2] = {1e9f, 1e9f};
      float mx[2] = {-1e9f, -1e9f};
      const float* m = d.consts;  // c0..c8; c4..c7 = 2D transform rows
      const uint32_t nv = d.count;
      for (uint32_t v = 0; v < nv; ++v) {
        const uint8_t* p = d.verts.data() + size_t(v) * d.stride;
        float pos[4];
        std::memcpy(pos, p, 16);
        for (int c = 0; c < 2; ++c) {
          const float t = pos[0] * m[16 + c] + pos[1] * m[20 + c] +
                          pos[2] * m[24 + c] + pos[3] * m[28 + c];
          mn[c] = std::min(mn[c], t);
          mx[c] = std::max(mx[c], t);
        }
      }
      const float w = mx[0] - mn[0];
      const float h = mx[1] - mn[1];
      if (w > 0.0f && h > 0.0f && w * h >= 0.08f * 1280.0f * 720.0f &&
          w * h < 1e8f) {
        static std::unordered_set<uint32_t> s_seen;
        static int64_t s_window_s = 0;
        static std::atomic<uint32_t> s_big{0};
        const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        if (now_s - s_window_s >= 5) {
          s_window_s = now_s;
          s_seen.clear();
        }
        if (s_seen.size() < 24 && s_seen.insert(d.fetch[1]).second) {
          REXLOG_DEBUG(
              "native-scene: BIG 2D quad tex=({:08x},{:08x},{:08x}) flags={:02x} "
              "src_stride={} count={} bbox=({:.0f},{:.0f})-({:.0f},{:.0f}) "
              "c8=({:.2f},{:.2f},{:.2f},{:.2f}) (n={})",
              d.fetch[0], d.fetch[1], d.fetch[2], d.flags, d.src_stride, d.count,
              mn[0], mn[1], mx[0], mx[1], m[32], m[33], m[34], m[35],
              s_big.fetch_add(1, std::memory_order_relaxed));
        }
      }
    }
    // TRANSITION-FADE TRACER: the game's screen-to-screen fades are
    // fullscreen SimpleDraw fills (RenderMan::FinalQuadFade ->
    // Draw_QuadListColoured, stride 16, color+ramping alpha in VS c8; the
    // fade color/alpha global sits at [0x83083C38]+31376+848/864, enable
    // byte +880). A mid-ramp alpha logged here proves the fade is captured
    // and replaying natively; if transitions still look like hard cuts
    // with these lines present, the problem is render pacing, not capture.
    // Rolling-capped: 6 lines per 2 s window.
    if (d.src_stride == 16 && d.count >= 4) {
      float alpha = d.consts[35];
      if (alpha > 0.02f && alpha < 0.98f) {
        float x0, y0, x1, y1;
        std::memcpy(&x0, d.verts.data(), 4);
        std::memcpy(&y0, d.verts.data() + 4, 4);
        std::memcpy(&x1, d.verts.data() + size_t(2) * d.stride, 4);
        std::memcpy(&y1, d.verts.data() + size_t(2) * d.stride + 4, 4);
        if (std::fabs(x1 - x0) >= 1200.0f && std::fabs(y1 - y0) >= 680.0f) {
          static std::atomic<uint32_t> s_fade_logs{0};
          static std::atomic<int64_t> s_fade_win{0};
          const int64_t now_s =
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          int64_t win = s_fade_win.load(std::memory_order_relaxed);
          if (now_s - win >= 2 && s_fade_win.compare_exchange_strong(win, now_s)) {
            s_fade_logs.store(0, std::memory_order_relaxed);
          }
          if (s_fade_logs.fetch_add(1, std::memory_order_relaxed) < 6) {
            REXLOG_DEBUG(
                "native-scene: transition fade fill alpha={:.2f} "
                "rgb=({:.2f},{:.2f},{:.2f}) flags={:02x}",
                alpha, d.consts[32], d.consts[33], d.consts[34], d.flags);
          }
        }
      }
    }
    // Photo display-card sighting (see g_photo_card_seen_ns): fetch word2
    // encodes exactly 504x640, the JPEG-decode card texture's unique dims.
    if (g_photo_flow_frame.load(std::memory_order_relaxed) &&
        (d.fetch[2] & 0x03FFFFFFu) == 0x004FE1F7u && d.src_stride == 24) {
      g_photo_card_seen_ns.store(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              PerfClock::now().time_since_epoch())
              .count(),
          std::memory_order_relaxed);
    }
    published.push_back(std::move(d));
  }
  std::lock_guard<std::mutex> lock(g_2d_mutex);
  g_scene_2d = std::move(published);
  ++g_scene_2d_generation;
}

// Publish the frame's in-world spline draws: evaluate the guest B-spline
// VS on the CPU (see SplineDraw for the decoded algorithm) into WORLD-space
// strip vertices; the render side projects them with the scene's (smoothed)
// view_proj like every other world item, keeping them in phase with the
// re-timed camera.
void PublishSplineDraws(uint8_t* base) {
  std::vector<SplineDraw> frame_spline;
  {
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    frame_spline.swap(g_frame_spline);
  }
  std::vector<SplineDraw> published;
  published.reserve(frame_spline.size());
  for (SplineDraw& s : frame_spline) {
    const float* c = s.consts;
    const auto row = [&](int r) { return c + r * 4; };
    std::vector<uint8_t> out(size_t(s.count) * 28);
    float* dst = reinterpret_cast<float*>(out.data());
    const uint8_t* src = s.verts.data();
    bool ok = true;
    for (uint32_t v = 0; v < s.count; ++v, src += 12, dst += 7) {
      float p[3];
      for (int k = 0; k < 3; ++k) {
        uint32_t w;
        std::memcpy(&w, src + k * 4, 4);
        w = BSwap32(w);
        std::memcpy(&p[k], &w, 4);
      }
      if (!(p[0] >= 0.0f && p[0] < 142.0f)) {
        ok = false;
        break;
      }
      const int idx = int(p[0]);
      const float t = p[0] - float(idx);
      const int side = p[2] >= 0.5f ? 1 : 0;
      // Uniform cubic B-spline basis (matches the shader's embedded
      // coefficients exactly).
      const float t2 = t * t;
      const float t3 = t2 * t;
      const float wgt[4] = {1.0f - 3.0f * t + 3.0f * t2 - t3,
                            3.0f * t3 - 6.0f * t2 + 4.0f,
                            -3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f, t3};
      // Blend the world-transformed control points (world columns c4..c6,
      // translation in .w), /6, plus the world-rotated extrusion offset.
      float wp[3] = {0.0f, 0.0f, 0.0f};
      for (int k = 0; k < 4; ++k) {
        const float* cp = row(7 + idx + k);
        for (int a = 0; a < 3; ++a) {
          const float* wr = row(4 + a);
          wp[a] += wgt[k] *
                   (wr[0] * cp[0] + wr[1] * cp[1] + wr[2] * cp[2] + wr[3] * cp[3]);
        }
      }
      const float* off = row(151 + side);
      for (int a = 0; a < 3; ++a) {
        const float* wr = row(4 + a);
        wp[a] = wp[a] * (1.0f / 6.0f) +
                (wr[0] * off[0] + wr[1] * off[1] + wr[2] * off[2] + wr[3] * off[3]);
      }
      // Fade still evaluates against the draw's own clip z (the ramps
      // span hundreds of meters; the ~30 ms offset from the smoothed
      // camera is invisible), but the PUBLISHED position is WORLD-space:
      // the render side projects with the scene's (smoothed) view_proj so
      // the neon signs ride the exact same camera timeline as the world.
      // Baking the guest VP here was the "waypoint sign judders / lags
      // and catches up" bug once smooth_camera re-timed everything else.
      float clip_z;
      {
        const float* pr = row(2);
        clip_z = pr[0] * wp[0] + pr[1] * wp[1] + pr[2] * wp[2] + pr[3];
      }
      // Near/far fade against i_clipvalues (clip-space z, pre-divide). A
      // zero range degenerates to a step, like the shader's rcp(0) = inf.
      const float* cv = row(150);
      const auto ramp = [](float z, float start, float range) {
        if (range > 1e-20f || range < -1e-20f) {
          const float r = (z - start) / range;
          return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        }
        return z - start > 0.0f ? 1.0f : 0.0f;
      };
      const float fade =
          std::min(ramp(clip_z, cv[0], cv[1]), 1.0f - ramp(clip_z, cv[2], cv[3]));
      dst[0] = wp[0];
      dst[1] = wp[1];
      dst[2] = wp[2];
      dst[3] = 1.0f;
      dst[4] = p[1];
      dst[5] = p[2];
      dst[6] = fade;
    }
    if (!ok) {
      continue;
    }
    s.verts.swap(out);
    published.push_back(std::move(s));
  }
  std::lock_guard<std::mutex> lock(g_2d_mutex);
  g_scene_spline = std::move(published);
}

void GetCapturedSunDir(float out[3]) {
  out[0] = g_sun_captured[0].load(std::memory_order_relaxed);
  out[1] = g_sun_captured[1].load(std::memory_order_relaxed);
  out[2] = g_sun_captured[2].load(std::memory_order_relaxed);
}

// Rebuild a world->light affine row (xyz = axis * scale, w = translation)
// around a new axis, preserving the row's scale and its value at the camera
// - cascade centering and bias conventions carry over unchanged.
static void RebasisLightRow(float* row, const float axis[3],
                            const float cam[3]) {
  const float scale = std::sqrt(row[0] * row[0] + row[1] * row[1] +
                                row[2] * row[2]);
  if (scale < 1e-8f) {
    return;
  }
  const float at_cam =
      row[0] * cam[0] + row[1] * cam[1] + row[2] * cam[2] + row[3];
  row[0] = axis[0] * scale;
  row[1] = axis[1] * scale;
  row[2] = axis[2] * scale;
  row[3] = at_cam - (row[0] * cam[0] + row[1] * cam[1] + row[2] * cam[2]);
}

// Rebuild the frame's captured shadow transforms around the given unit
// direction TOWARD the sun: the light basis rows via RebasisLightRow
// (preserving each row's scale, so cascade sizes and bias conventions
// carry over) plus a re-center of the base cascade and the sub-boxes
// (c1/c2 scale.xy + offset.zw) around the camera, biased along the shadow
// direction; the game fits its offsets for ITS axis each frame, and
// under a rotated basis the finest cascade drifts off the player and
// their shadow (fallback to the coarser tiles reads visibly blocky).
// Caster pass and receivers share these rows, so the re-fit stays
// self-consistent. Sole caller: the lighting-lab sun override.
static void RebasisShadowRows(FrameScene& scene, const float sun[3],
                              bool include_ws) {
  // Light basis: depth axis points away from the sun; X/Y span the shadow
  // plane. Handedness is irrelevant; casters and receivers share the rows.
  const float zl[3] = {-sun[0], -sun[1], -sun[2]};
  float xl[3] = {zl[2], 0.0f, -zl[0]};  // cross((0,1,0), zl)
  const float xll = std::sqrt(xl[0] * xl[0] + xl[2] * xl[2]);
  if (xll > 1e-5f) {
    xl[0] /= xll;
    xl[2] /= xll;
  } else {
    xl[0] = 1.0f;
    xl[2] = 0.0f;
  }
  const float yl[3] = {zl[1] * xl[2] - zl[2] * xl[1],
                       zl[2] * xl[0] - zl[0] * xl[2],
                       zl[0] * xl[1] - zl[1] * xl[0]};
  const float* cam = scene.cam_pos;
  // Shadows extend along the sun's ground projection, so each cascade
  // centers ~35% of its radius in that direction instead of symmetrically
  // on the camera; a rotated low sun otherwise stretches character
  // shadows out of the fine cascade into the 4x/16x coarser tiles within
  // a few meters (the moved-sun blocky shadow). Near-vertical suns get no
  // bias (shadows stay under their casters).
  float sd[3] = {-sun[0], 0.0f, -sun[2]};
  const float sdl = std::sqrt(sd[0] * sd[0] + sd[2] * sd[2]);
  if (sdl > 0.1f) {
    sd[0] /= sdl;
    sd[2] /= sdl;
  } else {
    sd[0] = 0.0f;
    sd[2] = 0.0f;
  }
  if (scene.shadow_valid) {
    RebasisLightRow(scene.shadow_rows + 0, xl, cam);    // c0 light-space X
    RebasisLightRow(scene.shadow_rows + 12, yl, cam);   // c3 light-space Y
    RebasisLightRow(scene.shadow_rows + 16, zl, cam);   // c4 depth ramp
    float* rows = scene.shadow_rows;
    const float xlen =
        std::sqrt(rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2]);
    const float ylen = std::sqrt(rows[12] * rows[12] + rows[13] * rows[13] +
                                 rows[14] * rows[14]);
    // Base cascade: center on the camera pushed along the shadow
    // direction by 35% of its own radius (the base tile's world radius is
    // 1 / row scale).
    const float rbase = 1.0f / std::max(std::max(xlen, ylen), 1e-8f);
    const float pb[3] = {cam[0] + sd[0] * 0.35f * rbase, cam[1],
                         cam[2] + sd[2] * 0.35f * rbase};
    rows[3] = -(rows[0] * pb[0] + rows[1] * pb[1] + rows[2] * pb[2]);
    rows[15] = -(rows[12] * pb[0] + rows[13] * pb[1] + rows[14] * pb[2]);
    // Sub-boxes: same construction at their own (wider) radii.
    for (int c = 0; c < 2; ++c) {
      const float sx = rows[4 + c * 4];
      const float sy = rows[5 + c * 4];
      const float rc =
          1.0f / std::max(std::max(xlen * std::fabs(sx), ylen * std::fabs(sy)),
                          1e-8f);
      const float pc[3] = {cam[0] + sd[0] * 0.35f * rc, cam[1],
                           cam[2] + sd[2] * 0.35f * rc};
      const float lsx =
          rows[0] * pc[0] + rows[1] * pc[1] + rows[2] * pc[2] + rows[3];
      const float lsy =
          rows[12] * pc[0] + rows[13] * pc[1] + rows[14] * pc[2] + rows[15];
      rows[6 + c * 4] = -lsx * sx;   // offset.x
      rows[7 + c * 4] = -lsy * sy;   // offset.y
    }
  }
  if (include_ws && scene.dynobj_ws_valid) {
    RebasisLightRow(scene.dynobj_ws + 0, xl, cam);
    RebasisLightRow(scene.dynobj_ws + 4, yl, cam);
    RebasisLightRow(scene.dynobj_ws + 8, zl, cam);
  }
}

// Lighting-lab sun override (skate3_native_render_scene_sun_override):
// rotates every captured per-frame light transform in the published scene
// to the azimuth/elevation-driven direction. The native CSM caster pass,
// the material receivers, the static world-shadow map (which re-primes
// automatically when its rows change) and the volumetric shafts all read
// these rows, so the whole dynamic lighting stack follows the moved sun.
// Baked lightmap shade and the sky dome's painted sun are game content and
// stay put.
static void ApplySunOverride(FrameScene& scene) {
  if (!REXCVAR_GET(skate3_native_render_scene_sun_override)) {
    return;
  }
  const float kDeg = 0.01745329252f;
  const float az = float(REXCVAR_GET(skate3_native_render_scene_sun_azimuth)) * kDeg;
  const float el =
      float(REXCVAR_GET(skate3_native_render_scene_sun_elevation)) * kDeg;
  // Unit vector TOWARD the sun (y up; azimuth 0 = +Z, 90 = +X).
  const float sun[3] = {std::cos(el) * std::sin(az), std::sin(el),
                        std::cos(el) * std::cos(az)};
  RebasisShadowRows(scene, sun, /*include_ws=*/true);
  if (scene.shadow_valid) {
    scene.shadow_rows[24] = sun[0];  // c6 sun direction
    scene.shadow_rows[25] = sun[1];
    scene.shadow_rows[26] = sun[2];
  }
  if (scene.dynobj_valid) {
    scene.dynobj_rows[0] = sun[0];
    scene.dynobj_rows[1] = sun[1];
    scene.dynobj_rows[2] = sun[2];
  }
  if (scene.sky_sun_valid) {
    scene.sky_sun[0] = sun[0];
    scene.sky_sun[1] = sun[1];
    scene.sky_sun[2] = sun[2];
  }
}

// Hor+ ultrawide: clip-space X scale that widens the game's screen-shaped
// projection to the wide guest-output aspect (see the SDK's
// ApplyNativeGuestOutputWideAspect). 1.0 while the output is not wide. The
// projection aspect is read off the matrix itself (|m11/m00|) so the FOV
// override and freecam zoom compose naturally.
float WideOutputHorScale(const float proj[16]) {
  const double wide_aspect = rex::graphics::GetNativeGuestOutputWideAspect();
  if (wide_aspect <= 0.0 || !rex::graphics::IsNativeGuestOutputActive()) {
    return 1.0f;
  }
  const float m00 = std::fabs(proj[0]);
  const float m11 = std::fabs(proj[5]);
  if (!(m00 > 1e-6f) || !(m11 > 1e-6f)) {
    return 1.0f;
  }
  const double base_aspect = double(m11) / double(m00);
  if (wide_aspect <= base_aspect + 0.01) {
    return 1.0f;
  }
  return float(base_aspect / wide_aspect);
}

// Applies the Hor+ scale to the published camera: column 0 of the row-vector
// projection and view*proj (clip.x = dot(v, column 0)). Every consumer (draw
// MVPs, post-pass unprojection, splines, frustum tests) reads these published
// matrices, so the frame stays self-consistent, exactly as if the game camera
// itself had the wide aspect.
void WidenPublishedCamera(FrameScene& scene, float scale) {
  if (scale == 1.0f) {
    return;
  }
  for (int r = 0; r < 4; ++r) {
    scene.proj[r * 4 + 0] *= scale;
    scene.view_proj[r * 4 + 0] *= scale;
  }
}

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  if (!SceneEnabled()) {
    return;
  }
  // The frame-end walks below chase captured pointers whose ranges world
  // streaming may have revoked during the frame; recover raw-load read
  // faults for the whole build (POSIX; no-op on Windows).
  GuestReadRecoveryScope guest_read_recovery(base);
  // Published every frame (not just on world submissions): boot/menu frames
  // carry only 2D, and the render thread's 2D texture decodes need the
  // guest base from the very first natively rendered boot frame.
  g_guest_base.store(base, std::memory_order_relaxed);
  ++g_guest_frame;  // paces the world-item cache revalidation
  // Perf telemetry: guest frame interval + this frame's capture-hook cost.
  static PerfClock::time_point s_last_frame_tp{};
  // Previous frame's phase costs, for the slow-frame attribution below (a
  // frame's dt is only known at the NEXT frame's entry).
  static uint64_t s_prev_build_ns = 0, s_prev_b2d_ns = 0, s_prev_bspl_ns = 0,
                  s_prev_bpal_ns = 0, s_prev_pal_tail_ns = 0, s_prev_cap_ns = 0;
  const auto build_t0 = PerfClock::now();
  if (s_last_frame_tp.time_since_epoch().count() != 0) {
    const uint64_t dt_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(build_t0 - s_last_frame_tp)
            .count());
    g_pw_guest_dt.Add(dt_ns);
    const double dt_ms = double(dt_ns) * 1e-6;
    g_dt_hist[dt_ms < 3.6 ? 0 : dt_ms < 4.5 ? 1 : dt_ms < 6.0 ? 2 : dt_ms < 10.0 ? 3 : 4]
        .fetch_add(1, std::memory_order_relaxed);
    // Attribute dips: the previous frame stretched the interval - log its
    // phase breakdown. "rest" = the game's own frame work + hook overhead
    // outside the build (dt minus our measured blocks).
    if (dt_ms >= 4.5 && dt_ms < 100.0 &&
        g_slow_frame_log_budget.load(std::memory_order_relaxed) > 0 &&
        g_slow_frame_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const double ours_ms =
          double(s_prev_build_ns + s_prev_cap_ns) * 1e-6;
      REXLOG_DEBUG(
          "native-scene: slow guest frame dt={:.2f}ms prev[cap={:.2f} build={:.2f} "
          "(2d={:.2f} spl={:.2f} pal={:.2f} ptail={:.2f} walk={:.2f}) rest={:.2f}]ms",
          dt_ms, double(s_prev_cap_ns) * 1e-6, double(s_prev_build_ns) * 1e-6,
          double(s_prev_b2d_ns) * 1e-6, double(s_prev_bspl_ns) * 1e-6,
          double(s_prev_bpal_ns) * 1e-6, double(s_prev_pal_tail_ns) * 1e-6,
          double(s_prev_build_ns - std::min(s_prev_build_ns,
                                            s_prev_b2d_ns + s_prev_bspl_ns +
                                                s_prev_bpal_ns + s_prev_pal_tail_ns)) *
              1e-6,
          dt_ms - ours_ms);
    }
  }
  s_last_frame_tp = build_t0;
  g_pw_capture.Add(g_capture_frame_ns);
  s_prev_cap_ns = g_capture_frame_ns;
  g_capture_frame_ns = 0;
  g_frame_b2d_ns = 0;
  g_frame_bspl_ns = 0;
  g_frame_bpal_ns = 0;
  g_frame_pal_tail_ns = 0;
  struct BuildPerf {
    PerfClock::time_point t0;
    uint64_t* prev_build_ns;
    uint64_t* prev_b2d_ns;
    uint64_t* prev_bspl_ns;
    uint64_t* prev_bpal_ns;
    uint64_t* prev_pal_tail_ns;
    ~BuildPerf() {
      const uint64_t build_ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
              .count());
      g_pw_build.Add(build_ns);
      *prev_build_ns = build_ns;
      *prev_b2d_ns = g_frame_b2d_ns;
      *prev_bspl_ns = g_frame_bspl_ns;
      *prev_bpal_ns = g_frame_bpal_ns;
      *prev_pal_tail_ns = g_frame_pal_tail_ns;
    }
  } build_perf{build_t0, &s_prev_build_ns, &s_prev_b2d_ns, &s_prev_bspl_ns,
               &s_prev_bpal_ns, &s_prev_pal_tail_ns};
  if (g_recording.load(std::memory_order_relaxed)) {
    // Flush this frame's deferred inline-ring payloads (2D BeginVertices
    // draws): the CPU has finished writing them by frame end, and the ring
    // has not yet been reused.
    std::lock_guard<std::mutex> lock(g_record_mutex);
    for (const PendingInlineDump& p : g_pending_inline_dumps) {
      if (p.draw_index >= g_recorded_draws.size() ||
          g_recorded_buffer_bytes + p.bytes > (512u << 20) ||
          !GuestRangeReadable(base, p.addr, p.bytes)) {
        continue;
      }
      RecordedBuffer buf;
      buf.vb_addr = p.addr;
      buf.ib_addr = 0;
      buf.fingerprint = (uint64_t(g_recorded_draws[p.draw_index]->frame) << 32) | p.addr;
      buf.vb.resize(p.bytes);
      std::memcpy(buf.vb.data(), base + p.addr, p.bytes);
      g_recorded_draws[p.draw_index]->vb_dump = uint32_t(g_recorded_buffers.size());
      g_recorded_buffer_bytes += p.bytes;
      g_recorded_buffers.push_back(std::move(buf));
    }
    g_pending_inline_dumps.clear();
  }
  {
    const auto t0 = PerfClock::now();
    Publish2dDraws(base);
    const auto t1 = PerfClock::now();
    PublishSplineDraws(base);
    const auto t2 = PerfClock::now();
    g_frame_b2d_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    g_frame_bspl_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    g_pw_b2d.Add(g_frame_b2d_ns);
    g_pw_bspl.Add(g_frame_bspl_ns);
  }
  // Take this frame's hook-time dynamic items regardless of how we exit,
  // leaving them in place across an early return (no perspective view, empty
  // frame) would desynchronize the indices stored in the records.
  std::vector<DrawItem> dynitems;
  std::unordered_set<uint32_t> ortho_ctx;
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    dynitems.swap(g_frame_dynitems);
    ortho_ctx.swap(g_frame_ortho_ctx);
    g_frame_pending_by_buffers.clear();
    g_frame_char_refresh.clear();
  }
  // Take this frame's selection re-draw captures and re-arm the post-sky
  // window (must happen on every exit path, like the dynitems swap).
  std::vector<SelectedDrawKey> frame_selected;
  frame_selected.swap(g_frame_selected);
  g_sky_seen_this_frame = false;
  const bool outline_edge_seen = g_outline_edge_seen;
  g_outline_edge_seen = false;
  if (count == 0) {
    return;
  }

  // Multiple SceneRenderViews can submit per frame (main, shadow cascades,
  // reflections). Pick the perspective one (proj[2][3] == 1 in row-vector
  // convention) and only take its items, deduplicated (the same context can
  // appear in several of the view's sort lists).
  uint32_t view = 0;
  uint32_t viewcam = 0;
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    if (r.kind != 1 || r.c == 0) {
      continue;
    }
    const uint32_t cam = REX_LOAD_U32(r.c + kViewCameraFromView);
    if (!GuestReadableApprox(base, cam)) {
      continue;
    }
    const float persp_w = LoadGuestF32(base, cam + 0x60 + (2 * 4 + 3) * 4);
    if (persp_w == 1.0f) {
      // Screen-shaped views only. The skater-portrait render-to-texture
      // passes (team menu boxes, Import Skater card) submit their OWN
      // perspective SceneRenderView with a tall narrow projection; picking
      // it here published the portrait as the world scene: the skater
      // flashed FULL SCREEN behind the menu on every entry/scroll, and the
      // publish refreshed g_last_publish_ns so the mode flapped
      // pause-native <-> loading for the 300 ms freshness window each time.
      // aspect(w/h) = m11/m00 of the
      // raw projection; every real screen view is >= 4:3.
      const float m00 = std::fabs(LoadGuestF32(base, cam + 0x60 + 0 * 4));
      const float m11 = std::fabs(LoadGuestF32(base, cam + 0x60 + (1 * 4 + 1) * 4));
      if (!(m00 > 1e-6f) || m11 < m00 * 1.2f) {
        static std::atomic<uint64_t> s_aux_views{0};
        const uint64_t n = s_aux_views.fetch_add(1, std::memory_order_relaxed);
        if (n < 4 || (n & 255u) == 0) {
          REXLOG_DEBUG(
              "native-scene: aux perspective view skipped (portrait RTT "
              "pass, proj aspect {:.2f}) (n={})",
              m00 > 1e-6f ? m11 / m00 : 0.0f, n);
        }
        continue;
      }
      view = r.c;
      viewcam = cam;
      break;
    }
  }
  if (view == 0) {
    return;
  }

  FrameScene scene;
  scene.items.reserve(count);
  std::unordered_set<uint32_t> seen;
  // Pre-size the per-frame bookkeeping: these fill with thousands of
  // entries every frame, and growing from empty rehashes repeatedly.
  seen.reserve(count);
  // Dynamic contexts are submitted several times per frame (once per pass);
  // each submission carries that pass's culled island list. Keep the fullest
  // one; a shadow-pass list can be missing body parts the main view needs.
  std::unordered_map<uint32_t, size_t> dyn_slot;
  // First refused/pending ropa capture per mesh, candidates for the
  // post-merge rescue, which must restore last frame's resolved MODE
  // (rigid vs skinned), never blindly re-skin (see g_ropa_state_cache).
  std::unordered_map<uint32_t, const DrawItem*> pending_ropa_by_mesh;
  // Per-INSTANCE skinned candidates (ctx-keyed, character families): a
  // refused capture re-publishes with the instance's own last palette
  // (see g_bones_cache_ctx).
  std::unordered_map<uint32_t, const DrawItem*> pending_skinned_by_ctx;
  // Per-instance rigid candidates whose post-draw world fixup never landed
  // this frame; rescued with their cached world (see g_rigid_world_cache).
  std::unordered_map<uint32_t, const DrawItem*> pending_rigid_by_ctx;
  // Sort-list records of dynamically-dispatched piece meshes (see
  // g_dyn_dispatch_meshes): built here, published AFTER the record loop with
  // the ctx owner's instance matrix, and only for ctxs no live dynamic
  // capture already covered this frame.
  std::unordered_map<uint32_t, DrawItem> sortlist_local_by_ctx;
  const auto total_indices = [](const DrawItem& d) {
    uint64_t n = 0;
    for (const DrawEntry& e : d.draws) n += e.index_count;
    return n;
  };
  const bool wloop_prof = REXCVAR_GET(skate3_native_render_scene_perf_items);
  const auto wloop_t0 = wloop_prof ? PerfClock::now() : PerfClock::time_point{};
  if (wloop_prof) {
    g_bi_records.fetch_add(count, std::memory_order_relaxed);
  }
  // Build-side occlusion skip: the render thread's culled-ctx set (the same
  // sticky snapshot the guest dispatch filter uses, expiring with it when
  // the native render idles or the cull's motion gate stands down). A ctx
  // in it skips the whole item rebuild on 3 of every 4 frames; the
  // staggered rebuild frame re-enters it in scene.items so the render side
  // re-tests occlusion (disocclusion drops it from the set) and the shadow
  // caster caches keep refreshing.
  static std::vector<uint32_t> s_build_culled;  // guest render thread only
  s_build_culled.clear();
  if (REXCVAR_GET(skate3_native_render_scene_occlusion_cull_build)) {
    CopyOcclusionCulledCtxs(s_build_culled);
    if (!s_build_culled.empty()) {
      scene.occl_build_skipped.reserve(s_build_culled.size());
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    // Primary opaque list of the chosen view only; other lists (shadow
    // culling, transparents, z-prepass) duplicate the same geometry through
    // different MeshContext objects and z-fight.
    if (r.kind == 1 && (r.c != view || r.b != 20160)) {
      continue;
    }
    if (r.kind == 2 && r.b != view) {
      // World-path capture from another view (shadow cascade): rendering it
      // duplicates the entity as a ghost.
      seen.insert(r.a);
      continue;
    }
    if (r.kind == 0 || r.kind == 2 || r.kind == 3) {
      // Dynamic entity (kind 0), main-view world-path capture (kind 2) or
      // quad-list capture (kind 3): the complete item was built at hook time.
      seen.insert(r.a);
      if (r.c == 0) {
        g_rej_no_dynstate.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (r.c > dynitems.size()) {
        g_rej_dyn_range.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const DrawItem& cand = dynitems[r.c - 1];
      if (cand.pending) {
        // Deferred mesh whose draw never came, or a capture the palette
        // acceptance gates refused. Remember it: if the instance does not
        // publish this frame, the post-merge rescue re-publishes it with
        // LAST frame's palette; one frame of pose lag beats a
        // one-frame-missing hat.
        if (cand.ropa) {
          pending_ropa_by_mesh.try_emplace(cand.mesh, &cand);
        } else if (cand.skinned && cand.ctx != 0 && cand.char_family != 0) {
          pending_skinned_by_ctx.try_emplace(cand.ctx, &cand);
        } else if (!cand.skinned && !cand.cloth_quads && cand.ctx != 0) {
          pending_rigid_by_ctx.try_emplace(cand.ctx, &cand);
        }
        (cand.skinned ? g_skinned_skipped : g_rigid_dropped)
            .fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (!REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
        continue;
      }
      auto [slot, inserted] = dyn_slot.try_emplace(r.a, scene.items.size());
      if (inserted) {
        scene.items.push_back(cand);
      } else {
        // Merge the per-pass captures of one context: PALETTE from the
        // perspective (z/main-pass) bank; the ortho caster-cascade banks
        // carry stale fine animation (see DrawItem::caster_bank), but
        // GEOMETRY from the fullest culled island list. The two must be
        // decided independently: a shadow list can be missing body parts
        // the main view needs, and conversely the main view CULLS most of
        // a vehicle the camera is standing inside/next to (wholesale
        // "prefer main pass" replacement published that partial list and
        // near vehicles went invisible).
        DrawItem& cur = scene.items[slot->second];
        const bool fresher = !cand.caster_bank && cur.caster_bank;
        const bool staler = cand.caster_bank && !cur.caster_bank;
        const bool fuller = total_indices(cand) > total_indices(cur);
        // State/geometry grafts require the same resolved mode: ropa
        // garments flip between rigid and skinned per capture, and mixing
        // one copy's palette with another's interpretation is the
        // mangled-ribbon bug. Same-mode ropa IS graftable; excluding ropa
        // wholesale meant a fuller caster-cascade list wholesale-won the
        // merge and the garment published the ortho bank's ~40 ms-stale
        // bone rows while the body meshes (graftable) got fresh palettes:
        // the garment rode ~10 cm off the moving body (speed x 40 ms), the
        // visible successor of the invisible-torso bug once the near-camera
        // acceptance landed. The mode equality check is what prevents the
        // ribbon, not the ropa flag.
        const bool graftable = cand.skinned == cur.skinned &&
                               cand.ropa == cur.ropa && cand.mesh == cur.mesh;
        if (fresher && fuller) {
          cur = cand;
        } else if (fresher && graftable) {
          // Fresher palette, smaller list: adopt the state, keep the
          // fuller geometry (same mesh and buffers, lists differ only in
          // which islands each pass kept).
          cur.bones = cand.bones;
          std::memcpy(cur.world, cand.world, sizeof(cur.world));
          std::memcpy(cur.char_rows, cand.char_rows, sizeof(cur.char_rows));
          std::memcpy(cur.tint, cand.tint, sizeof(cur.tint));
          cur.caster_bank = false;
        } else if (staler && fuller && graftable) {
          // Fuller caster list vs a fresher partial item: keep the fresh
          // palette, adopt the full geometry.
          const DrawItem state = cur;
          cur = cand;
          cur.bones = state.bones;
          std::memcpy(cur.world, state.world, sizeof(cur.world));
          std::memcpy(cur.char_rows, state.char_rows, sizeof(cur.char_rows));
          std::memcpy(cur.tint, state.tint, sizeof(cur.tint));
          cur.caster_bank = false;
        } else if (!fresher && !staler && fuller) {
          cur = cand;  // same pass class: fullest wins, as before
        } else if (staler && fuller) {
          cur = cand;  // ungraftable (ropa): pre-arbitration fullest-wins
        }
      }
      continue;
    }
    if (!seen.insert(r.a).second) {
      continue;
    }
    if (!s_build_culled.empty() &&
        ((g_guest_frame + ((r.a >> 4) * 2654435761u >> 8)) & 3u) != 0u &&
        std::binary_search(s_build_culled.begin(), s_build_culled.end(),
                           r.a)) {
      scene.occl_build_skipped.push_back(r.a);
      g_occl_build_skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!REXCVAR_GET(skate3_native_render_scene_world_items)) {
      continue;
    }
    DrawItem item;
    if (BuildItemGeometry(base, r.a, item)) {
      if (item.skinned) {
        // Skinned meshes reached through the world sort lists (flags,
        // banners) have no captured palette, bind-pose garbage; skip.
        g_skinned_skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (item.pos_fmt != 57) {
        // Model-space prop (vending machines, dispensers): its vertices need
        // the per-draw transform, which only the kind-2 hook-time capture
        // has; rendered here with identity it collapses at the world
        // origin. The capture handles it (or it is dropped when pending).
        g_world_props.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const auto dit = g_dyn_dispatch_meshes.find(item.mesh);
      if (dit != g_dyn_dispatch_meshes.end() &&
          g_guest_frame - dit->second <= 1800) {
        // Instance-transformed piece batched through the static sort lists
        // (the game hands FAR clones of dynamically-dispatched pieces to
        // the batcher): fmt-57 but MESH-LOCAL, so the identity world of
        // this path collapses it at the origin. Defer; the post-loop
        // publish serves the ctx owner's instance matrix, and only for
        // ctxs no live dynamic capture covered this frame.
        sortlist_local_by_ctx.try_emplace(r.a, std::move(item));
        continue;
      }
      scene.items.push_back(std::move(item));
    } else {
      // Silent world-item drop attribution (the F7 rings show world meshes
      // MISSING for 8-11 frame episodes, the mesh
      // flicker): a transient BuildItemGeometry failure (guarded reads
      // fail while the streamer relocates the mesh/material structures)
      // is indistinguishable from a game-culled record without this log.
      static std::atomic<uint32_t> s_wbuild_logs{0};
      static std::atomic<int64_t> s_wbuild_win{0};
      const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      int64_t win = s_wbuild_win.load(std::memory_order_relaxed);
      if (now_s - win >= 5 && s_wbuild_win.compare_exchange_strong(win, now_s)) {
        s_wbuild_logs.store(0, std::memory_order_relaxed);
      }
      if (s_wbuild_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO("native-scene: world item build FAILED ctx={:08X}", r.a);
      }
    }
  }
  if (wloop_prof) {
    g_pw_bi_wloop.Add(PerfNsSince(wloop_t0));
  }
  // Cross-frame palette rescue + cache refresh (see g_bones_cache_ctx and
  // g_ropa_state_cache): published copies refresh their cache entries; a
  // refused/pending capture re-publishes with the cached palette (one frame
  // of pose lag instead of a one-frame disappearance).
  if (REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
    // Authoritative palette serve: give every mapped skinned character item
    // its instance's own packed m_matrices rows (snapshotted at the game's
    // Pack/UpdateBoneTransforms exits, the exact bytes the VS upload
    // consumes; per piece, one sim tick, coherent by construction). Runs
    // FIRST so the sanity gate below validates the authoritative rows and
    // the rescue caches learn them; unmapped items keep the bank pipeline.
    const auto pal_t0 = PerfClock::now();
    g_pal_served_frame = skate3::native_palette::ServeAuthoritativePalettes(base, scene);
    g_frame_bpal_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   PerfClock::now() - pal_t0)
                                   .count());
    g_pw_bpal.Add(g_frame_bpal_ns);
    g_pal_served_total.fetch_add(g_pal_served_frame, std::memory_order_relaxed);
    // Dense publish-time coherence gate (see PublishedPaletteSane): a
    // 1-frame junk palette that passed the 6-sample capture gates would
    // otherwise draw the map-length-ribbon flash AND poison the rescue
    // caches (this runs before the refresh below). Re-publish the cached
    // state when a same-mode one exists; else drop the draws; a 1-frame
    // blink beats the ribbon.
    for (DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.empty() || item.pending ||
          item.dbg_src == 10 ||  // LW-substituted palettes are authoritative
          item.dbg_src == 11) {  // pack-served palettes are authoritative
        continue;
      }
      float spread = 0.0f;
      if (PublishedPaletteSane(base, item, &spread)) {
        continue;
      }
      g_pub_incoherent.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> incoh_logged{0};
      const uint32_t ln = incoh_logged.fetch_add(1, std::memory_order_relaxed);
      if (ln < 32 || (ln & 255u) == 0) {
        REXLOG_DEBUG(
            "native-scene: publish INCOHERENT palette mesh={:08X} fam={} "
            "ropa={} src={} spread={:.2f} bind=({:.2f},{:.2f},{:.2f}) "
            "bone0_t=({:.1f},{:.1f},{:.1f})",
            item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
            spread, item.bbox_max[0] - item.bbox_min[0],
            item.bbox_max[1] - item.bbox_min[1],
            item.bbox_max[2] - item.bbox_min[2], item.bones[3], item.bones[7],
            item.bones[11]);
      }
      bool healed = false;
      if (item.ropa) {
        const auto cit = g_ropa_state_cache.find(item.mesh);
        if (cit != g_ropa_state_cache.end() && cit->second.skinned &&
            cit->second.bones.size() == item.bones.size() &&
            g_guest_frame - cit->second.frame <= 30) {
          item.bones = cit->second.bones;
          item.dbg_src = 6;
          healed = true;
        }
      } else {
        // Per-instance cache only (clone-exact); healing a clone with its
        // TWIN's palette is a teleport.
        const auto cit = item.ctx != 0 ? g_bones_cache_ctx.find(item.ctx)
                                       : g_bones_cache_ctx.end();
        if (cit != g_bones_cache_ctx.end() &&
            cit->second.bones.size() == item.bones.size() &&
            g_guest_frame - cit->second.frame <= 10) {
          item.bones = cit->second.bones;
          item.dbg_src = 6;
          healed = true;
        }
      }
      if (!healed) {
        item.draws.clear();
        // CRITICAL: also drop the junk palette. Leaving it on the item let
        // the cache-refresh loop below store it into the rescue caches;
        // the NEXT frame's trip then "healed" with the poisoned entry and
        // published the ribbon with draws intact (log 1284/1285: the
        // fam=6/fam=1 spread 183-405 bind-local palettes, bone0_t=(0,0,-.5),
        // were re-captured every frame by the src=2 fixup).
        item.bones.clear();
      }
    }
    std::unordered_map<uint32_t, uint32_t> pub_count;
    for (const DrawItem& item : scene.items) {
      // Ropa garments count in EITHER resolved mode (a rigid-resolved copy
      // has no bones but is very much alive; rescuing a pending clone next
      // to it would draw a duplicate garment).
      if (item.ropa || (item.skinned && !item.bones.empty())) {
        ++pub_count[item.mesh];
      }
      if (item.ropa && item.caster_bank) {
        g_ropa_caster.fetch_add(1, std::memory_order_relaxed);
      }
    }
    for (const DrawItem& item : scene.items) {
      if (!(item.ropa || (item.skinned && !item.bones.empty())) ||
          pub_count[item.mesh] != 1) {
        continue;
      }
      if (item.draws.empty()) {
        // Vetoed/dropped this frame (incoherent-palette trip): whatever
        // state it carries must not become the "last good" rescue entry.
        continue;
      }
      if (item.ropa) {
        // Remember the resolved mode + transform (see g_ropa_state_cache).
        if (g_ropa_state_cache.size() < 512) {
          RopaResolvedState& c = g_ropa_state_cache[item.mesh];
          c.skinned = item.skinned && !item.bones.empty();
          std::memcpy(c.world, item.world, sizeof(c.world));
          c.bones = item.bones;
          c.frame = g_guest_frame;
        }
      }
    }
    // Per-instance palette cache refresh (see g_bones_cache_ctx): every
    // published skinned character-family item keeps its OWN last palette
    // keyed by ctx: no pub_count gate needed, the key IS the instance.
    for (const DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.empty() || item.caster_bank ||
          item.ctx == 0 || item.char_family == 0 || item.ropa ||
          item.draws.empty()) {
        continue;
      }
      if (g_bones_cache_ctx.size() > 2048) {
        for (auto it = g_bones_cache_ctx.begin();
             it != g_bones_cache_ctx.end();) {
          it = g_guest_frame - it->second.frame > 60
                   ? g_bones_cache_ctx.erase(it)
                   : std::next(it);
        }
      }
      CachedBones& cb = g_bones_cache_ctx[item.ctx];
      cb.bones = item.bones;
      cb.frame = g_guest_frame;
    }
    // Per-instance rescue: a refused/pending skinned character capture
    // whose ctx did not publish this frame re-publishes with ITS OWN last
    // palette (<= 10 frames fresh), regardless of how many clones of the
    // mesh are alive.
    for (const auto& [ctxk, cand] : pending_skinned_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      const auto bit = g_bones_cache_ctx.find(ctxk);
      if (bit == g_bones_cache_ctx.end() ||
          g_guest_frame - bit->second.frame > 10) {
        continue;  // stale = an old pose; one missing frame beats a teleport
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.bones = bit->second.bones;
      rescued.pending = false;
      rescued.dbg_src = 3;
      ++pub_count[rescued.mesh];
      // Mark the ctx published so the LW gap fill below cannot double-
      // publish the same instance this frame.
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_lw_ctx_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // Rigid clone rescue: a piece whose world-bearing draw was missed this
    // frame (culled, or claimed by another clone of the mesh) re-publishes
    // with its own last published world instead of vanishing for a frame.
    // Statics never move, and movables at most drift one frame behind; the
    // mesh check keeps a reused ctx address from wearing a stale placement.
    for (const auto& [ctxk, cand] : pending_rigid_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      const auto cit = g_rigid_world_cache.find(ctxk);
      if (cit == g_rigid_world_cache.end() || cit->second.mesh != cand->mesh ||
          g_guest_frame - cit->second.frame > 600) {
        continue;  // no trusted placement; a dropped frame beats an origin ghost
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      std::memcpy(rescued.world, cit->second.world, sizeof(rescued.world));
      rescued.pending = false;
      rescued.dbg_src = 12;
      ++pub_count[rescued.mesh];
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_rigid_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // Sort-list copies of dynamically-dispatched piece meshes (see
    // g_dyn_dispatch_meshes): the game batches FAR clones through the
    // static lists, where no per-entity capture carries their placement.
    // Publish with the ctx owner's instance matrix; a live dynamic capture
    // for the same ctx always wins (near clones arrive through both paths
    // in the same frame, and double-publishing would z-fight).
    for (auto& [ctxk, item] : sortlist_local_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      if (!ReadCtxInstanceWorld(base, ctxk, item.world)) {
        g_world_props.fetch_add(1, std::memory_order_relaxed);
        continue;  // no placement: dropping beats an origin ghost
      }
      ++pub_count[item.mesh];
      scene.items.push_back(std::move(item));
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_sortlist_local_pub.fetch_add(1, std::memory_order_relaxed);
    }
    // Refused ropa captures re-publish last frame's resolved state: mode,
    // world AND palette together (mixing frames' interpretations is the
    // mangled-ribbon bug; see g_ropa_state_cache).
    for (const auto& [mesh, cand] : pending_ropa_by_mesh) {
      if (pub_count.find(mesh) != pub_count.end()) {
        continue;  // a live copy published; nothing to rescue
      }
      // Dropped-garment gate: when the owning entity's garment table no
      // longer claims this VB (the game removed the cloth model; its
      // pieces render skinned with the body now), the cached rigid state
      // is a ghost: the retained drape tracks the live character as a
      // floating garment. Suppress instead of rescuing.
      if (skate3::native_entity::RopaGarmentDropped(base, cand->ctx,
                                                    cand->vb_obj)) {
        // The game renders a dropped garment SKINNED with the body (its
        // sim-inactive path); the cached rigid state is a ghost. Resolve
        // with the owning instance's own live packed palette; when that
        // fails, suppress (invisible beats floating).
        float rows[96 * 12];
        const uint32_t n = skate3::native_entity::ServeInstancePalette(
            base, cand->ctx, rows, 96);
        static std::atomic<uint32_t> s_drop_logged{0};
        const uint32_t ln =
            s_drop_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 511u) == 0) {
          REXLOG_INFO(
              "native-scene: ropa rescue DROPPED-GARMENT {} mesh={:08X} "
              "ctx={:08X} vb={:08X} rows={} (n={})",
              n ? "resolved SKINNED" : "suppressed", cand->mesh, cand->ctx,
              cand->vb_obj, n, ln);
        }
        if (n != 0) {
          scene.items.push_back(*cand);
          DrawItem& resolved = scene.items.back();
          resolved.skinned = true;
          resolved.bones.assign(rows, rows + size_t(n) * 12);
          resolved.pending = false;
          resolved.caster_bank = false;
          resolved.dbg_src = 4;
          if (resolved.ctx != 0) {
            dyn_slot.try_emplace(resolved.ctx, scene.items.size() - 1);
          }
          g_ropa_rescued.fetch_add(1, std::memory_order_relaxed);
        }
        continue;
      }
      const auto rit = g_ropa_state_cache.find(mesh);
      if (rit == g_ropa_state_cache.end() ||
          g_guest_frame - rit->second.frame > 30) {
        continue;
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.skinned = rit->second.skinned;
      rescued.bones = rit->second.bones;
      std::memcpy(rescued.world, rit->second.world, sizeof(rescued.world));
      rescued.pending = false;
      rescued.dbg_src = 4;
      if (rescued.ctx != 0) {
        dyn_slot.try_emplace(rescued.ctx, scene.items.size() - 1);
      }
      g_ropa_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // LW gap fill (see g_lw_last_items): republish live entities' items
    // whose ctx skipped this frame's records entirely, the class the
    // pending rescues cannot see (no capture happened at all).
    if (REXCVAR_GET(skate3_native_render_scene_lw_gap_fill)) {
      const uint64_t now = g_guest_frame;
      for (auto it = g_lw_last_items.begin(); it != g_lw_last_items.end();) {
        if (dyn_slot.find(it->first) != dyn_slot.end()) {
          ++it;  // published live this frame; refreshed below
          continue;
        }
        LwRetained& r = it->second;
        float alpha = 1.0f;
        uint32_t entity = 0;
        if (now - r.frame > 2 ||
            !skate3::native_lw::LookupLwCtx(it->first, &alpha, &entity)) {
          it = g_lw_last_items.erase(it);
          continue;
        }
        scene.items.push_back(r.item);
        scene.items.back().dbg_src = 9;  // gap fill (refresh below skips it)
        g_lw_gap_filled.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_fill_logged{0};
        const uint32_t ln = s_fill_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: LW gap fill ctx={:08X} mesh={:08X} fam={} "
              "age={} alpha={:.2f} (n={})",
              it->first, r.item.mesh, r.item.char_family, now - r.frame,
              alpha, ln);
        }
        ++it;
      }
      for (const DrawItem& item : scene.items) {
        // NOTE: lw_entity is not stamped yet here (the stamp pass runs just
        // before the smoothing block); LW membership is enforced at FILL
        // time by the store lookup; non-LW entries simply expire unused.
        if (item.char_family == 0 || item.ctx == 0 || item.ropa ||
            item.pending || item.caster_bank || item.dbg_src == 9 ||
            !item.skinned || item.bones.empty() || item.draws.empty()) {
          continue;
        }
        if (g_lw_last_items.size() > 512 &&
            g_lw_last_items.find(item.ctx) == g_lw_last_items.end()) {
          continue;  // growth backstop
        }
        LwRetained& r = g_lw_last_items[item.ctx];
        r.item = item;
        r.frame = now;
      }
    } else if (!g_lw_last_items.empty()) {
      g_lw_last_items.clear();
    }
  }
  // Per-INSTANCE character-lighting fallback (see g_char_rows_cache_inst):
  // the general rescue for capture-failed frames, keyed (mesh,ctx) and
  // entity-checked. Runs before the legacy single-instance mesh fallback so
  // instanced pieces (and the player, which is not LW-mapped) hold their
  // last validated rows, keeping hair in the blended sub-pass instead of
  // flipping to the legacy opaque path (the high-fps hair/garment flicker).
  if (REXCVAR_GET(skate3_native_render_scene_char_rows_inst)) {
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      float lw_alpha_unused = 1.0f;
      uint32_t lw_entity = 0;
      skate3::native_lw::LookupLwCtx(item.ctx, &lw_alpha_unused, &lw_entity);
      const uint64_t key = (uint64_t(item.mesh) << 32) | item.ctx;
      if (item.char_rows[14 * 4 + 1] > 0.0f) {
        if (g_char_rows_cache_inst.size() > 8192) {
          g_char_rows_cache_inst.clear();
        }
        CharRowsInst& ci = g_char_rows_cache_inst[key];
        std::memcpy(ci.rows.data(), item.char_rows, sizeof(item.char_rows));
        ci.entity = lw_entity;
        if (item.ropa) {
          skate3::native_entity::CtxInfo ident;
          if (skate3::native_entity::LookupCtx(item.ctx, &ident)) {
            if (g_char_rows_cache_ent.size() > 4096) {
              g_char_rows_cache_ent.clear();
            }
            CharRowsEnt& ce = g_char_rows_cache_ent[ident.entity];
            std::memcpy(ce.rows.data(), item.char_rows,
                        sizeof(item.char_rows));
            ce.fam = uint8_t(item.char_family);
          }
        }
      } else {
        const auto iit = g_char_rows_cache_inst.find(key);
        if (iit != g_char_rows_cache_inst.end() &&
            iit->second.entity == lw_entity) {
          std::memcpy(item.char_rows, iit->second.rows.data(),
                      sizeof(item.char_rows));
          g_char_rows_inst_served.fetch_add(1, std::memory_order_relaxed);
        } else if (item.ropa) {
          // Sim-switch bridge: rows from the garment's sibling renderable
          // via the common owning entity (see g_char_rows_cache_ent). The
          // row layout is family-specific, so a cross-family serve routes
          // the receiver through the donor family's shading path too.
          skate3::native_entity::CtxInfo ident;
          if (skate3::native_entity::LookupCtx(item.ctx, &ident)) {
            const auto eit = g_char_rows_cache_ent.find(ident.entity);
            if (eit != g_char_rows_cache_ent.end()) {
              std::memcpy(item.char_rows, eit->second.rows.data(),
                          sizeof(item.char_rows));
              if (eit->second.fam != 0 &&
                  eit->second.fam != item.char_family) {
                static std::atomic<uint32_t> s_fam_logged{0};
                const uint32_t ln =
                    s_fam_logged.fetch_add(1, std::memory_order_relaxed);
                if (ln < 8 || (ln & 1023u) == 0) {
                  REXLOG_DEBUG(
                      "native-scene: rows bridge cross-family serve "
                      "mesh={:08X} fam {}->{} (n={})",
                      item.mesh, item.char_family, eit->second.fam, ln);
                }
                item.char_family = eit->second.fam;
              }
              g_char_rows_ent_served.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    }
  }
  // Cross-frame character-lighting fallback (see g_char_rows_cache): items
  // whose capture chain never validated THIS frame reuse their garment's
  // last good rows instead of dropping to the empirical look (and, for
  // hair, out of the blended sub-pass). Single-instance meshes only;
  // clones carry per-instance rows.
  {
    std::unordered_map<uint32_t, uint32_t> char_mesh_count;
    for (const DrawItem& item : scene.items) {
      if (item.char_family != 0) {
        ++char_mesh_count[item.mesh];
      }
    }
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.char_rows[14 * 4 + 1] > 0.0f ||
          char_mesh_count[item.mesh] != 1) {
        continue;
      }
      const auto cit = g_char_rows_cache.find(item.mesh);
      if (cit != g_char_rows_cache.end()) {
        std::memcpy(item.char_rows, cit->second.data(), sizeof(item.char_rows));
        g_char_rows_reused.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  // Publish-gap telemetry and fill ("skater flickered invisible for a
  // moment"): a skinned/ropa/character mesh that published recently can
  // vanish from the publish stream for 1-2 built frames at high render
  // rates (the body serve races the sim; more frequent the faster the
  // renderer runs, so Vulkan flickered worst). Re-publish the previous
  // frame's capture for up to two missed frames - a one-frame-old pose is
  // imperceptible at those rates, and a real despawn stops the fill at the
  // cap. The rate-limited log keeps naming mesh and gap so sightings stay
  // diagnosable.
  {
    struct PubTrack {
      uint64_t frame = 0;
      DrawItem item;
    };
    static std::unordered_map<uint32_t, PubTrack> s_last_pub;
    static uint64_t s_pub_frame = 0;
    ++s_pub_frame;
    if (s_last_pub.size() > 4096) {
      s_last_pub.clear();
    }
    for (DrawItem& item : scene.items) {
      if (!(item.ropa || item.char_family != 0 ||
            (item.skinned && !item.bones.empty()))) {
        continue;
      }
      PubTrack& t = s_last_pub[item.mesh];
      const uint64_t gap = t.frame != 0 ? s_pub_frame - t.frame : 0;
      if (item.char_family != 0 && item.caster_bank) {
        // The main-pass fixup missed and this piece reached publish as its
        // raw ortho-bank capture (shadow-pass constants, no lighting rows):
        // consumers reading its rows misjudge it for the frame (the caster
        // fade gate skipped the piece, blinking the skater's shadow off).
        // Serve the previous frame's main-view capture instead, and never
        // store shadow-bank state as the good copy.
        if (t.frame != 0 && gap <= 2 && !t.item.caster_bank) {
          item = t.item;
          t.frame = s_pub_frame;
          static std::atomic<uint64_t> s_bank_swaps{0};
          const uint64_t n = s_bank_swaps.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 255u) == 0) {
            REXLOG_INFO(
                "native-scene: caster-bank publish swapped for last main-view "
                "capture mesh={:08X} fam={} (n={})",
                item.mesh, item.char_family, n + 1);
          }
        }
        continue;
      }
      t.frame = s_pub_frame;
      t.item = item;
      if (gap >= 2 && gap <= 4) {
        g_dyn_gap.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> gap_logged{0};
        const uint32_t ln = gap_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: dyn publish GAP mesh={:08X} missing {} frame(s) "
              "ropa={} skinned={} fam={} src={}",
              item.mesh, gap - 1, item.ropa ? 1 : 0, item.skinned ? 1 : 0,
              item.char_family, item.dbg_src);
        }
      }
    }
    if (REXCVAR_GET(skate3_native_render_scene_dyn_gap_fill)) {
      for (auto& [mesh, t] : s_last_pub) {
        if (t.frame == 0 || t.frame == s_pub_frame) {
          continue;  // published this frame (or placeholder)
        }
        const uint64_t gap = s_pub_frame - t.frame;
        if (gap <= 2) {
          // t.frame stays at the last REAL publish, so the fill self-limits
          // to two frames and the telemetry above still sees the true gap
          // when the stream resumes.
          scene.items.push_back(t.item);
          static std::atomic<uint64_t> s_gap_filled{0};
          const uint64_t n = s_gap_filled.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 1023u) == 0) {
            REXLOG_INFO(
                "native-scene: dyn gap fill mesh={:08X} age={} ropa={} fam={} (n={})",
                mesh, gap, t.item.ropa ? 1 : 0, t.item.char_family, n + 1);
          }
        }
      }
    }
  }
  if (scene.items.empty()) {
    return;
  }
  // Selected-object outline: flag items matching this frame's post-sky
  // re-draw captures. >= 2 identical draws = the stencil-marking pair; a
  // single occurrence is a legitimately late-drawn object, not a selection.
  // BOTH signals are required: the guest's own postfx_edgedetectstencil draw
  // must have run this frame too (park editor with an active selection);
  // gameplay draws some small props twice after the sky as well, which used
  // to outline distant objects during normal play.
  {
    uint32_t outline_items = 0;
    for (const SelectedDrawKey& k : frame_selected) {
      if (!outline_edge_seen || k.count < 2) {
        continue;
      }
      for (DrawItem& item : scene.items) {
        if (item.ib_obj == k.ib && item.vb_obj == k.vb &&
            std::fabs(item.world[12] - k.t[0]) < 0.05f &&
            std::fabs(item.world[13] - k.t[1]) < 0.05f &&
            std::fabs(item.world[14] - k.t[2]) < 0.05f && !item.selected) {
          item.selected = true;
          ++outline_items;
        }
      }
    }
    std::memcpy(scene.outline_color, g_outline_color, sizeof(scene.outline_color));
    static uint32_t s_outline_items = 0;
    if (outline_items != s_outline_items) {
      REXLOG_INFO("native-scene: selection outline {} item(s) (captures={})",
                  outline_items, frame_selected.size());
      s_outline_items = outline_items;
    }
  }
  for (int i = 0; i < 16; ++i) {
    scene.view_proj[i] = LoadGuestF32(base, viewcam + kViewCamViewProj + i * 4);
  }
  // The raw projection (+0x60) rides along for the depth-based post passes
  // (SSAO linearize/unproject). Camera smoothing below replaces only the
  // view; the projection is whatever the game rendered this frame with.
  for (int i = 0; i < 16; ++i) {
    scene.proj[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
  }
  // RAW guest view*proj, the pose the game culled its submissions with,
  // kept for the off-screen retention frustum tests below (smoothing
  // replaces scene.view_proj with the re-timed pose).
  float guest_vp[16];
  std::memcpy(guest_vp, scene.view_proj, sizeof(guest_vp));
  // Camera cadence telemetry (see g_cam_changes): does the guest publish a
  // NEW camera every rendered frame, or step it on a slower sim tick?
  {
    static float s_last_vp[16] = {};
    static uint32_t s_streak = 0;
    if (std::memcmp(s_last_vp, scene.view_proj, sizeof(s_last_vp)) == 0) {
      ++s_streak;
      g_cam_repeats.fetch_add(1, std::memory_order_relaxed);
      uint64_t prev = g_cam_max_streak.load(std::memory_order_relaxed);
      while (s_streak > prev && !g_cam_max_streak.compare_exchange_weak(
                                    prev, s_streak, std::memory_order_relaxed)) {
      }
    } else {
      s_streak = 0;
      g_cam_changes.fetch_add(1, std::memory_order_relaxed);
      std::memcpy(s_last_vp, scene.view_proj, sizeof(s_last_vp));
    }
  }
  // Camera position from the view matrix (+0x20, row-vector convention):
  // cam = -t * R^T.
  float cam_view[16];
  for (int i = 0; i < 16; ++i) {
    cam_view[i] = LoadGuestF32(base, viewcam + 0x20 + i * 4);
  }
  for (int j = 0; j < 3; ++j) {
    scene.cam_pos[j] =
        -(cam_view[12] * cam_view[j * 4 + 0] + cam_view[13] * cam_view[j * 4 + 1] +
          cam_view[14] * cam_view[j * 4 + 2]);
  }
  // The game's projection uses a negative x scale which already yields
  // correct D3D NDC orientation; use the view*proj matrix as captured.
  // (Negating column 0 here mirrors the image left-right.)

  // Vehicle retention (see g_dyn_retained): re-publish the last live
  // capture of a view-culled vehicle for a short window. Runs BEFORE the
  // smoothing block on purpose; the re-published copy re-claims its pose
  // ring in InterpolateDynamicItems (position re-pairing) and renders the
  // ring's coherent history instead of a frozen step.
  if (REXCVAR_GET(skate3_native_render_scene_retain_offscreen) &&
      REXCVAR_GET(skate3_native_render_scene_dynamic_items) &&
      g_synpan_active.load(std::memory_order_relaxed) == 0 &&
      !REXCVAR_GET(skate3_native_render_scene_freecam)) {
    // Covers the camera-smoothing lag only; vehicle pose data just ages.
    constexpr uint64_t kDynRetainFrames = 10;
    const uint64_t rnow = g_guest_frame;
    // World center from the palette: average the plausible bone
    // translations (junk rows past the real skeleton carry garbage; gate
    // on finiteness and distance to bone 0).
    const auto bone_center = [](const DrawItem& it, float out[3]) -> bool {
      const size_t nb = it.bones.size() / 12;
      if (nb == 0) {
        return false;
      }
      const float b0[3] = {it.bones[3], it.bones[7], it.bones[11]};
      double sum[3] = {0.0, 0.0, 0.0};
      int n = 0;
      for (size_t b = 0; b < nb; ++b) {
        const float t[3] = {it.bones[b * 12 + 3], it.bones[b * 12 + 7],
                            it.bones[b * 12 + 11]};
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) ||
            !std::isfinite(t[2])) {
          continue;
        }
        const float dx = t[0] - b0[0], dy = t[1] - b0[1], dz = t[2] - b0[2];
        if (dx * dx + dy * dy + dz * dz > 100.0f) {
          continue;  // > 10 m from bone 0: junk row
        }
        sum[0] += t[0];
        sum[1] += t[1];
        sum[2] += t[2];
        ++n;
      }
      if (n == 0) {
        return false;
      }
      out[0] = float(sum[0] / n);
      out[1] = float(sum[1] / n);
      out[2] = float(sum[2] / n);
      return true;
    };
    struct LivePos {
      uint32_t mesh;
      float p[3];
    };
    std::vector<LivePos> live;
    live.reserve(16);
    for (const DrawItem& it : scene.items) {
      if ((it.char_family != 6 && it.char_family != 7) || !it.skinned ||
          it.bones.empty() || it.pending || it.retained) {
        continue;
      }
      float p[3];
      if (!bone_center(it, p)) {
        continue;
      }
      live.push_back({it.mesh, {p[0], p[1], p[2]}});
      if (it.caster_bank) {
        continue;  // stale ortho pose: never store as the rescue state
      }
      DynRetained* slot = nullptr;
      for (DynRetained& r : g_dyn_retained) {
        const float dx = r.pos[0] - p[0], dy = r.pos[1] - p[1],
                    dz = r.pos[2] - p[2];
        if (r.item.mesh == it.mesh && dx * dx + dy * dy + dz * dz < 4.0f) {
          slot = &r;
          break;
        }
      }
      if (slot == nullptr) {
        if (g_dyn_retained.size() >= 64) {
          continue;
        }
        g_dyn_retained.emplace_back();
        slot = &g_dyn_retained.back();
      }
      slot->item = it;
      slot->item.retained = true;
      slot->item.selected = false;
      std::memcpy(slot->pos, p, sizeof(slot->pos));
      const float ex = it.bbox_max[0] - it.bbox_min[0];
      const float ey = it.bbox_max[1] - it.bbox_min[1];
      const float ez = it.bbox_max[2] - it.bbox_min[2];
      slot->half = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
      slot->last_seen = rnow;
    }
    for (size_t i = 0; i < g_dyn_retained.size();) {
      DynRetained& r = g_dyn_retained[i];
      if (r.last_seen == rnow) {
        ++i;
        continue;
      }
      bool keep = rnow - r.last_seen <= kDynRetainFrames;
      bool publish = keep;
      if (keep) {
        for (const LivePos& lp : live) {
          const float dx = lp.p[0] - r.pos[0], dy = lp.p[1] - r.pos[1],
                      dz = lp.p[2] - r.pos[2];
          if (lp.mesh == r.item.mesh && dx * dx + dy * dy + dz * dz < 36.0f) {
            // A live copy nearby (caster-only capture frame, or the 2 m
            // matcher missed a fast mover): keep the entry warm for the
            // frame the captures stop, but never double the vehicle.
            r.last_seen = rnow;
            publish = false;
            break;
          }
        }
      }
      if (publish) {
        // Unsubmitted + visible to the guest camera = really gone
        // (despawn); only view-culled vehicles re-publish.
        if (!BoxOutsideFrustum(r.pos, r.half, guest_vp, 0.97f)) {
          keep = false;
        }
      }
      if (!keep) {
        g_dyn_retained[i] = std::move(g_dyn_retained.back());
        g_dyn_retained.pop_back();
        continue;
      }
      if (publish) {
        scene.items.push_back(r.item);
        static std::atomic<uint64_t> s_dyn_retained_pub{0};
        const uint64_t n =
            s_dyn_retained_pub.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 1023u) == 0) {
          REXLOG_INFO(
              "native-scene: vehicle retention re-publish mesh={:08X} fam={} "
              "age_frames={} (n={})",
              r.item.mesh, r.item.char_family, rnow - r.last_seen, n);
        }
      }
      ++i;
    }
  }

  // (Moved BEFORE the smoothing block: the interp ring's pose ingestion
  // pairs each pose with g_ropa_last_seq; the shape snapshot of the SAME
  // frame must be enqueued first or every pose pairs with the previous
  // frame's shape, a constant one-period drape lag. The snapshot also now
  // reads RAW captured items, matching the raw VB it snapshots.)
  // Dynamic cloth decode jobs (see DynDecodeJob): snapshot CHANGED (or
  // first-seen) skinned/ropa payloads for the workers; the render thread
  // never decodes them inline; a first-sight NPC/garment appears 1-2 frames
  // late instead of hitching the frame it streams in on. GuestTryCopy is
  // safe here: the game just drew from these payloads this frame.
  {
    // mesh -> last enqueued dedup key (guest render thread only). Static
    // skinned meshes enqueue once; ropa re-enqueues whenever its cloth
    // payload changed: EVERY rendered frame while the sim runs. That
    // cadence is load-bearing: each pose the interp ring ingests pairs with
    // the SAME-frame generation via g_ropa_last_seq (a constant zero
    // enqueue offset). Do not decimate these enqueues on a timer of their
    // own; an enqueue clock that slips independently of the pose-ring
    // ingest clock makes the pairing offset jitter by a frame, and the
    // drape blends against shapes from the wrong instant (speed-scaled
    // garment flicker). High-rate capacity lives in the CONSUMERS instead
    // (the resident generation ring and kShapeGens).
    static std::unordered_map<uint32_t, uint64_t> s_dyn_fp_sent;
    static uint64_t s_dyn_seq = 0;
    if (s_dyn_fp_sent.size() > 4096) {
      s_dyn_fp_sent.clear();
    }
    std::vector<DynDecodeJob> jobs;
    for (const DrawItem& item : scene.items) {
      if (!(item.skinned || item.ropa) || item.cloth_quads || item.ib_addr == 0) {
        continue;
      }
      if (item.ropa && REXCVAR_GET(skate3_native_render_scene_ropa_inline)) {
        continue;  // ropa decodes inline on the render thread (zero-lag)
      }
      if (item.ropa && item.dbg_src == 4) {
        // Rescued ropa re-publishes LAST frame's resolved state (mode +
        // palette + world); decoding THIS frame's payload under it mixes
        // frames, at a sim flip that is the ribbon. Keep the previous
        // decode on the GPU: a fully coherent N-1 garment (the draw path
        // tolerates the fingerprint mismatch for dynamic payloads).
        continue;
      }
      // The decode is MODE-dependent for ropa (rigid decodes zero the blend
      // weight/index attributes, see DecodeMesh), so a mode flip must
      // re-enqueue even when the payload bytes did not change: fold the
      // resolved mode into the dedup key.
      const uint64_t fp_key =
          item.fingerprint ^
          ((item.ropa && item.skinned && !item.bones.empty()) ? 1u : 0u);
      const auto prev = s_dyn_fp_sent.find(item.mesh);
      const bool first_sight = prev == s_dyn_fp_sent.end();
      if (!first_sight && prev->second == fp_key) {
        continue;
      }
      DynDecodeJob job;
      job.item = item;
      job.item.bones.clear();
      job.seq = ++s_dyn_seq;
      job.vb.resize(item.vb_bytes);
      if (!GuestTryCopy(job.vb.data(), base + item.vb_addr, item.vb_bytes)) {
        continue;
      }
      job.ib.resize(size_t(item.ib_count) * 2);
      if (!GuestTryCopy(job.ib.data(), base + item.ib_addr, job.ib.size())) {
        continue;
      }
      s_dyn_fp_sent[item.mesh] = fp_key;
      if (item.ropa) {
        // The pose <-> shape pairing key (see DynPose::shape_seq). Recorded
        // at CREATION (the delay queue below postpones submission, not
        // identity).
        g_ropa_last_seq[item.mesh] = job.seq;
        if (g_ropa_last_seq.size() > 256) {
          g_ropa_last_seq.clear();
        }
      }
      jobs.push_back(std::move(job));
    }
    // ROPA phase alignment (skate3_native_render_scene_ropa_delay): the
    // body renders on the motion-smoothing play clock, ~2 guest periods
    // behind now; a cloth snapshot submitted immediately decodes into a
    // shape ~2 frames AHEAD of the rendered body (the drape hangs where
    // the body WILL be; jelly / clip-through on direction changes; the
    // console pairs body N with shape N). Ropa snapshots pass through a
    // small per-mesh delay queue so the committed shape lands on the same
    // clock as the interpolated body.
    {
      const int32_t delay = REXCVAR_GET(skate3_native_render_scene_ropa_delay);
      static std::unordered_map<uint32_t, std::deque<DynDecodeJob>> s_ropa_delay;
      if (delay > 0) {
        std::vector<DynDecodeJob> ready;
        ready.reserve(jobs.size());
        for (DynDecodeJob& j : jobs) {
          if (!j.item.ropa) {
            ready.push_back(std::move(j));
            continue;
          }
          auto& q = s_ropa_delay[j.item.mesh];
          q.push_back(std::move(j));
          while (q.size() > size_t(delay)) {
            ready.push_back(std::move(q.front()));
            q.pop_front();
          }
        }
        jobs.swap(ready);
        if (s_ropa_delay.size() > 256) {
          s_ropa_delay.clear();  // outfit-change growth backstop
        }
      } else if (!s_ropa_delay.empty()) {
        s_ropa_delay.clear();
      }
    }
    if (!jobs.empty()) {
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      for (DynDecodeJob& j : jobs) {
        if (g_dyn_jobs.size() >= 32) {
          break;  // workers behind: the cloth skips a sim frame
        }
        g_dyn_jobs.push_back(std::move(j));
      }
      g_prewarm_cv.notify_all();
    }
  }

  // LivingWorld entity store stamp: map
  // each character-family item's MeshContext to its owning LW entity and
  // stamp the entity's authoritative opacity + per-instance identity. Runs
  // after every dyn publish path (captures, merges, rescues, vehicle
  // retention) and before the smoothing block so the pose rings can key by
  // identity. Items with no fresh store entry (player skater - a different
  // fade system, CAC entities, despawned/retained leftovers) keep the
  // captured-row behavior untouched.
  if (REXCVAR_GET(skate3_native_render_scene_lw_fade) ||
      REXCVAR_GET(skate3_native_render_scene_lw_identity)) {
    // Fade serving also honors the master entity-fade switch: with it off
    // the routing ignores fades entirely, so the alpha row must keep its
    // captured value (the shader still reads it on some paths).
    const bool serve_fade =
        REXCVAR_GET(skate3_native_render_scene_lw_fade) &&
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    const bool serve_id = REXCVAR_GET(skate3_native_render_scene_lw_identity);
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      float alpha = 1.0f;
      uint32_t entity = 0;
      if (!skate3::native_lw::LookupLwCtx(item.ctx, &alpha, &entity)) {
        continue;
      }
      g_lw_stamped.fetch_add(1, std::memory_order_relaxed);
      if (serve_id) {
        item.lw_entity = entity;
      }
      // Per-ctx lighting/paint rows (see g_char_rows_cache_ctx): refresh
      // from validated captures, serve on capture-failed frames, entity-
      // checked so a recycled instance never inherits foreign paint. This
      // keeps edge-of-view vehicles (caster-only capture stretches, every
      // read rejected) on their own shading instead of legacy flat.
      if (item.char_rows[14 * 4 + 1] > 0.0f) {
        if (g_char_rows_cache_ctx.size() > 4096) {
          g_char_rows_cache_ctx.clear();
        }
        CharRowsCtx& cr = g_char_rows_cache_ctx[item.ctx];
        std::memcpy(cr.rows.data(), item.char_rows, sizeof(item.char_rows));
        cr.entity = entity;
      } else {
        const auto rit = g_char_rows_cache_ctx.find(item.ctx);
        if (rit != g_char_rows_cache_ctx.end() &&
            rit->second.entity == entity) {
          std::memcpy(item.char_rows, rit->second.rows.data(),
                      sizeof(item.char_rows));
          g_lw_rows_served.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // Authoritative palette for every vehicle pose that did NOT come
      // from a fresh perspective capture: caster-only frames (ortho banks
      // = GUESSED palette base + ~40 ms-stale animation, the edge-of-view
      // mangle class), retention re-publishes (last capture aging up to 10
      // frames; at driving speed a stale republish renders a GHOST
      // meters BEHIND the live position right after the vehicle exits the
      // view; a garbage last capture makes the ghost sideways), rescues
      // and gap fills. The entity's OWN packed palette (m_matrices,
      // written by the pack writer this sim tick) is its true current
      // pose: a genuinely-exited vehicle lands off-screen (no ghost), a
      // pan-trailing retention case lands exactly right.
      if (item.skinned && !item.ropa &&
          (item.char_family == 6 || item.char_family == 7) &&
          (item.caster_bank || item.retained || item.dbg_src == 3 ||
           item.dbg_src == 6 || item.dbg_src == 9) &&
          !item.bones.empty() &&
          REXCVAR_GET(skate3_native_render_scene_lw_palette)) {
        float rows[96 * 12];
        const uint32_t n =
            skate3::native_lw::LookupLwPalette(item.ctx, rows, 96 * 12);
        if (n != 0 && size_t(n) * 12 <= item.bones.size()) {
          std::memcpy(item.bones.data(), rows, size_t(n) * 12 * sizeof(float));
          item.caster_bank = false;
          item.dbg_src = 10;  // lw palette substitution
          g_lw_pal_sub.fetch_add(1, std::memory_order_relaxed);
          static std::atomic<uint32_t> s_sub_logged{0};
          const uint32_t ln =
              s_sub_logged.fetch_add(1, std::memory_order_relaxed);
          if (ln < 16 || (ln & 511u) == 0) {
            REXLOG_DEBUG(
                "native-scene: LW palette substituted ctx={:08X} mesh={:08X} "
                "fam={} rows={} (n={})",
                item.ctx, item.mesh, item.char_family, n, ln);
          }
        }
      }
      if (serve_fade) {
        item.lw_alpha = alpha;
        // The exact shading path reads the alpha ROW (cbuffer CH row 14.x)
        // - overwrite it with the entity value for the families where that
        // row IS the raw entity fade on console (c13.x / c21.x / c22.x /
        // c20.x). Hair (strand-scale composed) and vehicle glass
        // (tint-composed) keep their captured rows; CharFadeAlpha bounds
        // them by the entity alpha instead.
        if (item.char_rows[14 * 4 + 1] > 0.0f &&
            (item.char_family == 1 || item.char_family == 2 ||
             item.char_family == 3 || item.char_family == 6)) {
          item.char_rows[14 * 4 + 0] = std::clamp(alpha, 0.0f, 1.0f);
        }
      }
    }
  }

  // Presentation-entity identity store observer
  // (native/skate3_native_entity.h): per-item coverage of the
  // ctx -> entity map built from the game's BindConstants walks, and the
  // skater-family entity+496 opacity vs the alpha the routing decided on.
  // Read-only: the ident[] stats line is the evidence for flipping any
  // serve over to the direct fields.
  {
    const bool serve_ent_fade =
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      // Skater-family fade from the entity's own opacity (+496): the
      // value the game binds as the shader's alpha parameter. Same serving
      // contract as the LW store (lw_alpha preferred by CharFadeAlpha;
      // families whose shader composes the fade keep their captured rows
      // bounded by it). LW-mapped items were already stamped above.
      float ent_alpha = 1.0f;
      if (serve_ent_fade && item.lw_alpha < 0.0f &&
          skate3::native_entity::ReadSkaterFade(base, item.ctx, &ent_alpha)) {
        item.lw_alpha = std::clamp(ent_alpha, 0.0f, 1.0f);
        if (item.char_rows[14 * 4 + 1] > 0.0f &&
            (item.char_family == 1 || item.char_family == 2 ||
             item.char_family == 3 || item.char_family == 6)) {
          item.char_rows[14 * 4 + 0] = item.lw_alpha;
        }
      }
      skate3::native_entity::ObserveCharItem(base, item.ctx,
                                             item.char_family,
                                             item.lw_entity != 0,
                                             CharFadeAlpha(item));
    }
    skate3::native_entity::EmitStats();
  }

  // Camera re-timing (see SmoothCamera): replace the guest's sim-stepped
  // pose with a host-clock-interpolated one so panning is smooth at render
  // rate. All consumers (items, sky follow, sorting, outline) use the
  // smoothed pose coherently.
  if (REXCVAR_GET(skate3_native_render_scene_smooth_camera)) {
    // Publish the ViewCamera for the ~1 kHz sampler thread and make sure it
    // runs (see CamSamplerLoop).
    g_sampler_viewcam.store(viewcam, std::memory_order_relaxed);
    EnsureCamSampler();
    float proj[16];
    for (int i = 0; i < 16; ++i) {
      proj[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
    }
    const double now_s =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    // Auto-armed bone-signal recordings (diagnosis, see the cvar): first
    // window ~30 s after the scene comes up, re-armed every 90 s, 3 max.
    {
      const double auto_s = REXCVAR_GET(skate3_native_render_scene_bonesig_auto);
      if (auto_s > 0.0) {
        static int s_auto_count = 0;
        static double s_auto_next = 0.0;
        if (s_auto_next == 0.0) {
          s_auto_next = now_s + 30.0;
        } else if (s_auto_count < 3 && now_s >= s_auto_next) {
          ++s_auto_count;
          s_auto_next = now_s + 90.0;
          REXLOG_INFO("native-scene: auto bone-signal recording {} ({} s)",
                      s_auto_count, auto_s);
          RecordBoneSignal(std::min(auto_s, 30.0));
        }
      }
    }
    // Walking-vehicle detector (diagnosis): a livingworld_vehicles item
    // whose bone-0 translation sits on top of a character item's bone-0 is
    // the "player becomes the vehicle" bug. Log the coincidence WITH the
    // first bone rows of both palettes: identical rows = the same bank was
    // captured for both (a capture-attribution bug); distinct rows = the
    // vehicle's own palette tracks the character (a different mechanism).
    {
      static double s_last_attach_log = 0.0;
      if (now_s - s_last_attach_log > 2.0) {
        bool logged = false;
        for (const DrawItem& v : scene.items) {
          if ((v.char_family != 6 && v.char_family != 7) || !v.skinned ||
              v.bones.size() < 12) {
            continue;
          }
          for (const DrawItem& c : scene.items) {
            if (&c == &v || !c.skinned || c.bones.size() < 12 ||
                c.char_family == 0 || c.char_family >= 6) {
              continue;
            }
            const float dx = v.bones[3] - c.bones[3];
            const float dy = v.bones[7] - c.bones[7];
            const float dz = v.bones[11] - c.bones[11];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 4.0f) {
              const bool same_rows =
                  std::memcmp(v.bones.data(), c.bones.data(),
                              12 * sizeof(float)) == 0;
              REXLOG_INFO(
                  "native-scene ATTACH: vehicle mesh={:08X} fam={} src={} "
                  "pend={} d={:.2f} char mesh={:08X} fam={} src={} "
                  "same_bone0={} v_r0=({:.3f},{:.3f},{:.3f},{:.2f}) "
                  "c_r0=({:.3f},{:.3f},{:.3f},{:.2f})",
                  v.mesh, v.char_family, v.dbg_src, v.pending, std::sqrt(d2),
                  c.mesh, c.char_family, c.dbg_src, same_rows, v.bones[0],
                  v.bones[1], v.bones[2], v.bones[3], c.bones[0], c.bones[1],
                  c.bones[2], c.bones[3]);
              s_last_attach_log = now_s;
              logged = true;
              break;
            }
          }
          if (logged) {
            break;
          }
        }
      }
    }
    float smooth_vp[16], smooth_cam[3];
    if (SmoothCamera(cam_view, proj, scene.view_proj, scene.cam_pos, now_s, smooth_vp,
                     smooth_cam)) {
      std::memcpy(scene.view_proj, smooth_vp, sizeof(smooth_vp));
      std::memcpy(scene.cam_pos, smooth_cam, sizeof(smooth_cam));
      // Keep the skater/NPCs/props in phase with the smoothed camera:
      // interpolate their palettes/worlds at the same playback time.
      InterpolateDynamicItems(base, scene, now_s);
    }
  }

  // Hor+ ultrawide scale for this frame's camera (1.0 when the output is not
  // wide). The matrices are widened after the camera overrides below; the
  // retention frustum guards divide by it here so their tests already match
  // the frame's real (wider) view.
  const float wide_hor_scale = WideOutputHorScale(scene.proj);

  // Off-screen retention (see g_retained_items): re-append statics the game
  // view-culled this frame while the trailing rendered pose can still see
  // them. Runs AFTER the smoothing block so retained copies never enter the
  // dynamic pose histories. Stands down while the synthetic-pan probe
  // maintains its own full-surround union, and while the freecam has taken
  // over the guest camera (the game then culls around the rendered pose
  // directly, so re-appends would only risk double draws).
  if (REXCVAR_GET(skate3_native_render_scene_retain_offscreen) &&
      g_synpan_active.load(std::memory_order_relaxed) == 0 &&
      !REXCVAR_GET(skate3_native_render_scene_freecam)) {
    const auto retain_t0 = PerfClock::now();
    uint64_t retain_appended = 0;
    if (g_retained_clear.exchange(false, std::memory_order_relaxed)) {
      g_retained_items.clear();
      g_dyn_retained.clear();
    }
    const uint64_t now = g_guest_frame;
    std::unordered_set<uint64_t> submitted;
    const size_t published = scene.items.size();
    submitted.reserve(published);
    for (size_t i = 0; i < published; ++i) {
      const DrawItem& it = scene.items[i];
      if (it.skinned || it.cloth_quads || it.ropa || it.pending ||
          !it.bones.empty()) {
        continue;
      }
      const uint64_t key = SynPanItemKey(it);
      submitted.insert(key);
      if (g_retained_items.size() >= 20000 &&
          g_retained_items.find(key) == g_retained_items.end()) {
        continue;  // growth backstop
      }
      auto [slot, inserted] = g_retained_items.try_emplace(key);
      slot->second.last_seen = now;
      // Copy the item core on first sight and whenever its payload identity
      // or transform moved on; a retained copy with a stale fingerprint
      // skips at draw (see draw_item's retained gate) and would re-tear.
      if (inserted || slot->second.item.fingerprint != it.fingerprint ||
          std::memcmp(slot->second.item.world, it.world, sizeof(it.world)) != 0) {
        slot->second.item = it;
        slot->second.item.retained = true;
        slot->second.item.selected = false;
      }
    }
    // TTL bounds how long a never-resubmitted entry lives (streaming reuses
    // arena addresses; the render side additionally fingerprint-gates
    // retained draws). ~90 guest frames = 0.6-1.5 s across fps caps,
    // far beyond the smoothing lag it needs to cover.
    constexpr uint64_t kRetainTtlFrames = 90;
    for (auto rit = g_retained_items.begin(); rit != g_retained_items.end();) {
      if (submitted.find(rit->first) != submitted.end()) {
        ++rit;
        continue;
      }
      const RetainedItem& r = rit->second;
      // Unsubmitted + visible to the guest camera = the game really removed
      // it. The 0.97 margin shrinks the tested frustum so bounds poking
      // just inside an edge still count as view-culled.
      if (now - r.last_seen > kRetainTtlFrames ||
          !ItemOutsideFrustum(r.item, guest_vp, 0.97f / wide_hor_scale)) {
        rit = g_retained_items.erase(rit);
        continue;
      }
      // Draw it only if the RENDERED pose can actually see it (widened
      // frustum: only clearly-outside skips); after a fast 180 the trail
      // behind the camera stays retained but costs nothing.
      if (!ItemOutsideFrustum(r.item, scene.view_proj, 1.05f / wide_hor_scale)) {
        scene.items.push_back(r.item);
        ++retain_appended;
      }
      ++rit;
    }
    g_pw_bi_retain.Add(PerfNsSince(retain_t0));
    g_retained_appended.fetch_add(retain_appended, std::memory_order_relaxed);
    g_retained_live.store(g_retained_items.size(), std::memory_order_relaxed);
  }

  // Camera-signal recorder: per-frame raw + smoothed heading while the
  // window is open; write + reset when it closes (CamSigFrameTick).
  if (g_camsig_deadline.load(std::memory_order_relaxed) > 0.0) {
    const double rec_now =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    float rot[16] = {};
    const float* smoothed = nullptr;
    if (g_smooth_active) {
      ViewRotFromQuat(g_smooth_pose.q, rot);
      smoothed = rot;
    }
    CamSigFrameTick(rec_now, cam_view, smoothed, g_smooth_play);
  }

  // Synthetic camera pan probe (see the synthetic_pan cvar comment for the
  // mode semantics). Runs AFTER the smoothing block: modes 1/2 override the
  // published pose outright (their point is to bypass the guest pose path);
  // mode 3 leaves the smoothed pose in place, the sampler thread is feeding
  // the smoother synthetic samples, and measures reconstruction error
  // against the known ideal.
  {
    const double syn_now =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    int syn_mode =
        std::clamp(int(REXCVAR_GET(skate3_native_render_scene_synthetic_pan)), 0, 3);
    if (syn_mode == 3 && !REXCVAR_GET(skate3_native_render_scene_smooth_camera)) {
      static bool s_warned = false;
      if (!s_warned) {
        s_warned = true;
        REXLOG_WARN(
            "native-scene synthetic-pan: mode 3 needs smooth_camera ON; "
            "running mode 1 instead");
      }
      syn_mode = 1;
    }
    static int s_engaged_mode = 0;
    if (syn_mode != s_engaged_mode) {
      s_engaged_mode = syn_mode;
      if (syn_mode == 0) {
        g_synpan_active.store(0, std::memory_order_release);
        g_synpan_union.clear();
        REXLOG_INFO("native-scene synthetic-pan: off (guest camera restored)");
      } else {
        // (Re-)engage from THIS frame's raw guest pose: heading, position
        // and projection are frozen; only the synthetic yaw moves.
        std::lock_guard<std::mutex> lock(g_synpan_mutex);
        std::memcpy(g_synpan_view0, cam_view, sizeof(g_synpan_view0));
        for (int i = 0; i < 16; ++i) {
          g_synpan_proj0[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
        }
        for (int j = 0; j < 3; ++j) {
          g_synpan_c0[j] =
              -(cam_view[12] * cam_view[j * 4 + 0] + cam_view[13] * cam_view[j * 4 + 1] +
                cam_view[14] * cam_view[j * 4 + 2]);
        }
        g_synpan_t0 = syn_now;
        g_synpan_step_phase = 0.0;
        g_synpan_ema_dt = 0.0;
        g_synpan_frames = 0;
        g_synpan_last_build = 0.0;
        g_synpan_dt_sum = g_synpan_dt_sum2 = 0.0;
        g_synpan_dt_min = g_synpan_dt_max = 0.0;
        g_synpan_err_sum2 = g_synpan_err_max = 0.0;
        g_synpan_err_n = 0;
        g_synpan_union.clear();
        g_synpan_active.store(syn_mode, std::memory_order_release);
        static const char* kModeNames[] = {"off", "time-based", "fixed-step",
                                           "through-smoother"};
        REXLOG_INFO(
            "native-scene synthetic-pan: ENGAGED mode={} ({}) rate={:.1f} deg/s "
            "amp=+-{:.1f} deg",
            syn_mode, kModeNames[syn_mode],
            REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate),
            REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp));
      }
    }
    if (syn_mode != 0) {
      const double rate = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate);
      const double amp = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp);
      const double prev_build = g_synpan_last_build;
      g_synpan_last_build = syn_now;
      const double dt = prev_build > 0.0 ? std::clamp(syn_now - prev_build, 0.0, 0.05) : 0.0;
      if (dt > 0.0) {
        g_synpan_dt_sum += dt;
        g_synpan_dt_sum2 += dt * dt;
        g_synpan_dt_min = g_synpan_dt_min == 0.0 ? dt : std::min(g_synpan_dt_min, dt);
        g_synpan_dt_max = std::max(g_synpan_dt_max, dt);
      }
      if (syn_mode == 1 || syn_mode == 2) {
        double phase;
        if (syn_mode == 1) {
          phase = (syn_now - g_synpan_t0) * rate;
        } else {
          // Fixed step: constant angle per published frame. The step is
          // rate * (slow EMA of dt) so deg/s stays roughly honest while the
          // per-frame advance is effectively constant over any short window.
          if (dt > 0.0) {
            g_synpan_ema_dt =
                g_synpan_ema_dt == 0.0 ? dt : g_synpan_ema_dt * 0.995 + dt * 0.005;
          }
          g_synpan_step_phase += rate * g_synpan_ema_dt;
          phase = g_synpan_step_phase;
        }
        float sview[16];
        SynPanView(SynPanAngleDeg(phase, amp), sview);
        CamPose pose;
        QuatFromView(sview, pose.q);
        std::memcpy(pose.c, g_synpan_c0, sizeof(pose.c));
        ComposeViewProj(pose, g_synpan_proj0, scene.view_proj, scene.cam_pos);
      } else if (g_smooth_active) {
        // Mode 3: the smoother just reconstructed a pose from the synthetic
        // samples at playback time g_smooth_play; compare against the ideal
        // pose at that exact time (both are functions of the same clock).
        float iview[16];
        SynPanView(SynPanAngleDeg((g_smooth_play - g_synpan_t0) * rate, amp), iview);
        float qi[4];
        QuatFromView(iview, qi);
        const float dq =
            std::fabs(qi[0] * g_smooth_pose.q[0] + qi[1] * g_smooth_pose.q[1] +
                      qi[2] * g_smooth_pose.q[2] + qi[3] * g_smooth_pose.q[3]);
        const double err_deg =
            2.0 * std::acos(std::min(dq, 1.0f)) * (180.0 / 3.14159265358979323846);
        g_synpan_err_sum2 += err_deg * err_deg;
        g_synpan_err_max = std::max(g_synpan_err_max, err_deg);
        ++g_synpan_err_n;
      }
      // World union: accumulate this frame's static items and append every
      // previously seen one the game didn't submit this frame (it culls to
      // ITS frustum; the probe camera looks elsewhere). Statics only:
      // skinned/cloth poses go stale immediately.
      {
        std::unordered_set<uint64_t> cur;
        cur.reserve(scene.items.size());
        const size_t published = scene.items.size();
        for (size_t i = 0; i < published; ++i) {
          const DrawItem& it = scene.items[i];
          if (it.skinned || it.cloth_quads || it.ropa || it.pending ||
              !it.bones.empty()) {
            continue;
          }
          const uint64_t key = SynPanItemKey(it);
          cur.insert(key);
          if (g_synpan_union.size() < 20000) {
            auto [slot, inserted] = g_synpan_union.try_emplace(key, it);
            if (!inserted &&
                std::memcmp(slot->second.world, it.world, sizeof(it.world)) != 0) {
              slot->second = it;  // a movable prop moved: refresh
            }
          }
        }
        for (const auto& [key, it] : g_synpan_union) {
          if (cur.find(key) == cur.end()) {
            scene.items.push_back(it);
          }
        }
      }
      if (++g_synpan_frames % 600 == 0) {
        const double n = std::max<double>(1.0, double(g_synpan_frames - 1));
        const double avg = g_synpan_dt_sum / n;
        const double sd =
            std::sqrt(std::max(0.0, g_synpan_dt_sum2 / n - avg * avg));
        if (syn_mode == 3) {
          REXLOG_INFO(
              "native-scene synthetic-pan: mode=3 frames={} build_dt_ms[avg/min/max/sd]="
              "{:.2f}/{:.2f}/{:.2f}/{:.2f} smoother_err_deg[rms/max]={:.4f}/{:.4f} (n={}) "
              "union={}",
              g_synpan_frames, avg * 1e3, g_synpan_dt_min * 1e3, g_synpan_dt_max * 1e3,
              sd * 1e3,
              std::sqrt(g_synpan_err_sum2 / std::max<uint64_t>(1, g_synpan_err_n)),
              g_synpan_err_max, g_synpan_err_n, g_synpan_union.size());
        } else {
          REXLOG_INFO(
              "native-scene synthetic-pan: mode={} frames={} build_dt_ms[avg/min/max/sd]="
              "{:.2f}/{:.2f}/{:.2f}/{:.2f} union={}",
              syn_mode, g_synpan_frames, avg * 1e3, g_synpan_dt_min * 1e3,
              g_synpan_dt_max * 1e3, sd * 1e3, g_synpan_union.size());
        }
      }
    }
  }

  // Drone / free-fly camera (skate3_native_render_scene_freecam, End key):
  // runs after the smoothing and synthetic-pan blocks so the flown pose
  // wins while engaged. No draw-item union here; the SetViewMatrix
  // override hands the flown pose to the game, whose own culling then
  // submits exactly what the drone sees (statics AND animated entities).
  UpdateFreecam(scene, cam_view,
                std::chrono::duration<double>(build_t0.time_since_epoch()).count());

  // Hor+ ultrawide: widen the published camera to the wide output aspect.
  // Applied after every camera override (smoothing, synthetic pan, freecam)
  // so it survives their view_proj/proj rewrites.
  WidenPublishedCamera(scene, wide_hor_scale);

  // Publish the frame's captured fog rows and re-arm the OnDrawDone capture
  // (keyed to this frame's camera) for the next frame.
  if (g_fog_have) {
    std::memcpy(scene.fog_ramp, g_fog_rows, 4 * sizeof(float));
    std::memcpy(scene.fog_color, g_fog_rows + 4, 4 * sizeof(float));
  }
  if (g_water_have) {
    std::memcpy(scene.water_rows, g_water_rows, sizeof(scene.water_rows));
    scene.water_valid = true;
  }
  if (g_ocean_have) {
    std::memcpy(scene.ocean_rows, g_ocean_rows, sizeof(scene.ocean_rows));
    scene.ocean_valid = true;
  }
  if (g_oceanrefl_have) {
    std::memcpy(scene.oceanrefl_rows, g_oceanrefl_rows,
                sizeof(scene.oceanrefl_rows));
    scene.oceanrefl_valid = true;
  }
  if (g_scroll_have) {
    std::memcpy(scene.scroll_rows, g_scroll_rows, sizeof(scene.scroll_rows));
    scene.scroll_valid = true;
  }
  if (g_shadow_have) {
    std::memcpy(scene.shadow_rows, g_shadow_rows, sizeof(g_shadow_rows));
    scene.shadow_valid = true;
    // Same freshness pattern as the char CSM biases below: outdoor scenes
    // re-capture every frame, so anything past a couple of seconds means the
    // current venue's environment shader never produces a sane bank.
    scene.shadow_fresh = g_guest_frame - g_shadow_rows_frame <= 120;
  }
  // Character CSM receive biases (see the header comment): frame-coherent,
  // served while fresh enough that a few capture-less frames don't flap the
  // char shadow between the exact and legacy paths.
  if (g_char_shadow_bias[0] > 0.0f &&
      g_guest_frame - g_char_shadow_bias_frame <= 120) {
    std::memcpy(scene.char_shadow_bias, g_char_shadow_bias,
                sizeof(scene.char_shadow_bias));
  }
  std::memcpy(scene.family_rows, g_family_rows, sizeof(g_family_rows));
  if (g_dynobj_have) {
    std::memcpy(scene.dynobj_rows, g_dynobj_rows, sizeof(g_dynobj_rows));
    scene.dynobj_valid = true;
    if (g_dynobj_ws_have) {
      std::memcpy(scene.dynobj_ws, g_dynobj_ws, sizeof(g_dynobj_ws));
      scene.dynobj_ws_valid = true;
    }
  }
  if (g_sky_have) {
    scene.sky_height = g_sky_height;
  }
  if (g_sky_sun_have) {
    std::memcpy(scene.sky_sun, g_sky_sun, sizeof(g_sky_sun));
    scene.sky_sun_valid = true;
  }
  // Publish the captured sun direction BEFORE any override (the debug
  // dialog seeds the override sliders from it).
  {
    const float* cap = scene.shadow_valid ? scene.shadow_rows + 24
                                          : scene.sky_sun;
    if (scene.shadow_valid || scene.sky_sun_valid) {
      g_sun_captured[0].store(cap[0], std::memory_order_relaxed);
      g_sun_captured[1].store(cap[1], std::memory_order_relaxed);
      g_sun_captured[2].store(cap[2], std::memory_order_relaxed);
    }
  }
  ApplySunOverride(scene);
  // The game's own popup blur chain (pause menu etc.), untouched by the
  // host settings menu, whose gaussian reads host state directly at render
  // time (see ApplyMenuBlurPass call sites).
  const bool blur_active = g_ui_blur_seen || g_ui_blur_hold > 0;
  scene.ui_blur = blur_active ? g_ui_blur : 0.0f;
  std::memcpy(scene.ui_blur_color, g_ui_blur_color, sizeof(scene.ui_blur_color));
  if (g_ui_blur_seen) {
    g_ui_blur_hold = 2;
  } else if (g_ui_blur_hold > 0) {
    --g_ui_blur_hold;
  }
  {
    static bool s_blur_was_active = false;
    if (blur_active != s_blur_was_active) {
      REXLOG_INFO(
          "native-scene: popup background blur {} (kernel scale {:.1f}, fade "
          "{:.2f}/{:.2f}/{:.2f})",
          blur_active ? "ON" : "off", scene.ui_blur, scene.ui_blur_color[0],
          scene.ui_blur_color[1], scene.ui_blur_color[2]);
      s_blur_was_active = blur_active;
      if (!blur_active) {
        // Don't carry one popup's fade into the next popup's first frame
        // if its own c1 read misses the coherence gate.
        g_ui_blur_color[0] = g_ui_blur_color[1] = g_ui_blur_color[2] = 1.0f;
      }
    }
  }
  g_ui_blur_seen = false;
  // Photo-editor postfx: publish the live pass captures when the photo
  // EDITOR itself is up (not the wider TakePhoto readback window: the
  // chain must never run over ordinary gameplay frames) and every pass has
  // fresh rows (the game stages its postfx constants each frame regardless
  // of draw suppression). The vignette and grain fetch words come from the
  // fisheye/uber fetch-shadow snapshots (slot 2 / slot 6 per the
  // ring-verified bindings).
  if (uint8_t* pfx_base = g_guest_base.load(std::memory_order_relaxed);
      pfx_base != nullptr && g_photo_flow_frame.load(std::memory_order_relaxed) &&
      PhotoEditorSignal(pfx_base) != nullptr) {
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerfClock::now().time_since_epoch())
                               .count();
    bool fresh = true;
    for (int p = 0; p < kPfxPassCount; ++p) {
      if (g_pfx_cap[p].ps_ns == 0 || now_ns - g_pfx_cap[p].ps_ns > 1'000'000'000 ||
          !g_pfx_cap[p].vs_seen) {
        fresh = false;
      }
    }
    if (!fresh) {
      // Name the missing pass(es), once per ~2 s while in the window.
      static int64_t s_diag_ns = 0;
      if (now_ns - s_diag_ns > 2'000'000'000) {
        s_diag_ns = now_ns;
        char buf[160];
        int off = 0;
        for (int p = 0; p < kPfxPassCount; ++p) {
          const int64_t age_ms =
              g_pfx_cap[p].ps_ns == 0 ? -1 : (now_ns - g_pfx_cap[p].ps_ns) / 1'000'000;
          off += std::snprintf(buf + off, sizeof(buf) - off, " p%d[ps%lldms vs%d]", p,
                               (long long)age_ms, g_pfx_cap[p].vs_seen ? 1 : 0);
          if (off >= int(sizeof(buf)) - 24) break;
        }
        REXLOG_INFO("native-scene: pfx captures NOT fresh:{}", buf);
      }
    }
    if (fresh) {
      scene.photo_fx.valid = true;
      for (int p = 0; p < kPfxPassCount; ++p) {
        std::memcpy(scene.photo_fx.ps[p], g_pfx_cap[p].ps,
                    sizeof(scene.photo_fx.ps[p]));
        std::memcpy(scene.photo_fx.vs[p], g_pfx_cap[p].vs,
                    sizeof(scene.photo_fx.vs[p]));
      }
      std::memcpy(scene.photo_fx.vignette_fetch, g_pfx_cap[kPfxFisheye].fetch[2],
                  sizeof(scene.photo_fx.vignette_fetch));
      std::memcpy(scene.photo_fx.grain_fetch, g_pfx_cap[kPfxUber].fetch[6],
                  sizeof(scene.photo_fx.grain_fetch));
      static bool s_pfx_logged = false;
      if (!s_pfx_logged) {
        s_pfx_logged = true;
        REXLOG_INFO(
            "native-scene: photo postfx captures LIVE (visualfx c0=({:.3f},{:.3f},"
            "{:.3f},{:.3f}) dof c0.x={:.4f} uber c5=({:.3f},{:.3f},{:.3f},{:.3f}) "
            "fisheye c1=({:.3f},{:.3f},{:.3f}) vignette=[{:08X} {:08X}] "
            "grain=[{:08X} {:08X}])",
            g_pfx_cap[kPfxVisualFx].ps[0][0], g_pfx_cap[kPfxVisualFx].ps[0][1],
            g_pfx_cap[kPfxVisualFx].ps[0][2], g_pfx_cap[kPfxVisualFx].ps[0][3],
            g_pfx_cap[kPfxDof].ps[0][0], g_pfx_cap[kPfxUber].ps[5][0],
            g_pfx_cap[kPfxUber].ps[5][1], g_pfx_cap[kPfxUber].ps[5][2],
            g_pfx_cap[kPfxUber].ps[5][3], g_pfx_cap[kPfxFisheye].ps[1][0],
            g_pfx_cap[kPfxFisheye].ps[1][1], g_pfx_cap[kPfxFisheye].ps[1][2],
            scene.photo_fx.vignette_fetch[0], scene.photo_fx.vignette_fetch[1],
            scene.photo_fx.grain_fetch[0], scene.photo_fx.grain_fetch[1]);
      }
    }
  }
  std::memcpy(g_fog_cam, scene.cam_pos, sizeof(g_fog_cam));
  g_fog_frame_done = false;
  g_shadow_frame_done = false;
  g_sky_frame_done = false;
  g_tree_frame_done = false;
  g_proxy_frame_done = false;
  g_dynobj_frame_done = false;
  g_water_frame_done = false;
  g_ocean_frame_done = false;
  g_oceanrefl_frame_done = false;
  g_scroll_frame_done = false;


  // Draw-time STRETCH VETO: the last line of defense, judging what the GPU
  // will ACTUALLY draw: the RESIDENT decode's sample verts (g_skin_probe,
  // cached by DecodeMesh) skinned with the FINAL palette (after the merge,
  // rescues and interpolation above). Every upstream gate judges the LIVE
  // guest VB, so a decode-content/palette pairing mismatch, or junk
  // introduced by an interpolation substitution, passes all of them and
  // still flashes the 1-frame map-length ribbon. Trip: clear the item's
  // draws (a blink, and the caster pass shares the item so no ribbon
  // shadow either), log, and dump palette + samples for offline diagnosis.
  if (REXCVAR_GET(skate3_native_render_scene_stretch_guard)) {
    for (DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.size() < 12 || item.pending ||
          item.draws.empty()) {
        continue;
      }
      SkinProbe probe;
      {
        std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
        const auto pit = g_skin_probe.find(item.mesh);
        if (pit == g_skin_probe.end()) {
          continue;  // not decoded yet: nothing will be drawn either
        }
        probe = pit->second;
      }
      const float bind_diag = BindDiag(item);
      // 6x like PublishedPaletteSane: judging accepted palettes, where junk
      // measures 100-400x and legit small-mesh articulation reaches ~3.3x.
      const float max_spread = std::max(6.0f * bind_diag, bind_diag + 2.0f);
      // Decoded-buffer convention: component k = byte k (see DecodeMesh's
      // SwapU32 + per-byte extraction); unpack into the shared sample form.
      std::vector<SkinSampleVert> sverts(probe.s.size());
      for (size_t i = 0; i < probe.s.size(); ++i) {
        const SkinProbeSample& ps = probe.s[i];
        SkinSampleVert& sv = sverts[i];
        sv.p[0] = ps.p[0];
        sv.p[1] = ps.p[1];
        sv.p[2] = ps.p[2];
        sv.pos_finite = true;
        for (int k = 0; k < 4; ++k) {
          sv.w[k] = uint8_t((ps.bw >> (8 * k)) & 0xFF);
          sv.bone[k] = uint8_t((ps.bi >> (8 * k)) & 0xFF);
        }
      }
      float spread = 0.0f;
      int n = 0;
      if (SkinnedSpreadHostRows(sverts.data(), uint32_t(sverts.size()),
                                item.bones.data(), item.bones.size(), /*min_n=*/4,
                                /*garbage_fails=*/false, &spread, &n) != 1) {
        continue;
      }
      if (spread <= max_spread) {
        continue;
      }
      g_stretch_veto.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_stretch_logged{0};
      const uint32_t ln =
          s_stretch_logged.fetch_add(1, std::memory_order_relaxed);
      if (ln < 24 || (ln & 255u) == 0) {
        REXLOG_INFO(
            "native-scene: STRETCH veto mesh={:08X} fam={} ropa={} src={} "
            "caster={} spread={:.1f} bind={:.2f} n={} fp_match={} "
            "bone0_t=({:.1f},{:.1f},{:.1f})",
            item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
            item.caster_bank ? 1 : 0, spread, bind_diag, n,
            probe.fp == item.fingerprint ? 1 : 0, item.bones[3],
            item.bones[7], item.bones[11]);
      }
      if (ln < 6 && REXCVAR_GET(skate3_native_render_scene_stretch_guard_dump)) {
        // Full palette + probe dump for offline diagnosis (which rows are
        // junk, which bones the stretched samples weight to).
        char path[128];
        std::snprintf(path, sizeof(path), "logs/stretch_%u_%08X.txt", ln,
                      item.mesh);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fprintf(f,
                       "mesh=%08X fam=%u ropa=%d src=%u caster=%d spread=%f "
                       "bind=%f fp=%016llX probe_fp=%016llX vb=%08X\n",
                       item.mesh, item.char_family, item.ropa ? 1 : 0,
                       item.dbg_src, item.caster_bank ? 1 : 0, spread,
                       bind_diag, (unsigned long long)item.fingerprint,
                       (unsigned long long)probe.fp, item.vb_addr);
          const uint32_t bones = uint32_t(item.bones.size() / 12);
          for (uint32_t b = 0; b < bones; ++b) {
            const float* r = item.bones.data() + b * 12;
            std::fprintf(f,
                         "bone %3u | %9.4f %9.4f %9.4f %10.3f | %9.4f %9.4f "
                         "%9.4f %10.3f | %9.4f %9.4f %9.4f %10.3f\n",
                         b, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                         r[8], r[9], r[10], r[11]);
          }
          for (const SkinProbeSample& ps : probe.s) {
            std::fprintf(f, "vert p=(%f,%f,%f) bw=%08X bi=%08X\n", ps.p[0],
                         ps.p[1], ps.p[2], ps.bw, ps.bi);
          }
          std::fclose(f);
        }
      }
      item.draws.clear();
    }
  }

  if (g_recording.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    if (++g_frames_seen % g_record_stride == 0) {
      RecordedFrame rf;
      rf.generation = g_generation + 1;
      std::memcpy(rf.view_proj, scene.view_proj, sizeof(rf.view_proj));
      std::memcpy(rf.cam_pos, scene.cam_pos, sizeof(rf.cam_pos));
      rf.dynitems = dynitems;
      rf.items = scene.items;
      g_recorded_frames.push_back(std::move(rf));
      ++g_record_frame;
    }
  }

  // The draw-time fetch map served this frame's item builds (streamed-artwork
  // diffuse override); next frame's draws repopulate it. Cleared HERE, not at
  // the top with the other per-frame structures; the world items that
  // consume it are built above, after that swap.
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    g_frame_draw_fetch.clear();
  }

  // Frame tail: rotate the bone-palette snapshot ring. Timed as its own
  // block in the perf line; this tail runs on the guest render thread,
  // where spikes are visible stutter.
  {
    const auto pal_tail_t0 = PerfClock::now();
    skate3::native_palette::OnFrameBuilt();
    g_frame_pal_tail_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 PerfClock::now() - pal_tail_t0)
                                 .count());
    g_pw_pal_tail.Add(g_frame_pal_tail_ns);
  }

  // AUX-publish gate: the skater-portrait render-to-texture passes submit a
  // real perspective SceneRenderView, and on the frontend menu screens
  // (team boxes, Import Skater) it is the ONLY publisher; publishing it
  // rendered the portrait FULL SCREEN behind the menu for the whole
  // pause-freshness window on every entry/scroll (flapping
  // pause-native <-> loading in 300 ms cycles). Its projection is
  // screen-shaped (the aspect gate at view selection above never fired), so
  // identify it by CONTENT: a menu-context (presence 0) scene consisting of
  // nothing but character-family pieces (skater + board, every material
  // "character.*") is a portrait pass, never the visible frame. Real menu
  // backdrops always carry world geometry (the pause plaza ~700 items, the
  // CAS editor room has env-family walls). Skipped publishes also skip the
  // freshness stamp, so the mode never flips.
  // CAS-editor exemption: the STARTUP-flow editor's scene is skater-only
  // (no garage world pre-gameplay: 10 all-char items, exactly a portrait
  // pass's shape; without the exemption this gate ate it, 3D black
  // behind a live menu). The editor's _nis shader heartbeat separates the
  // two: editor draws stamp it every frame, portrait passes use the
  // _default compiles and never do.
  if (rex::kernel::guest_presence::GameplayContextValue() == 0 &&
      !scene.items.empty() && scene.items.size() <= 48 &&
      !CasEditorActive(base)) {
    bool all_char = true;
    for (const DrawItem& it : scene.items) {
      if (it.char_family == 0) {
        all_char = false;
        break;
      }
    }
    if (all_char) {
      static std::atomic<uint32_t> s_aux_pub{0};
      const uint32_t n = s_aux_pub.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 255u) == 0) {
        const float m00 = LoadGuestF32(base, viewcam + 0x60 + 0 * 4);
        const float m11 = LoadGuestF32(base, viewcam + 0x60 + (1 * 4 + 1) * 4);
        REXLOG_INFO(
            "native-scene: portrait-pass publish skipped ({} char items, cam "
            "({:.1f},{:.1f},{:.1f}), proj m00={:.3f} m11={:.3f}) (n={})",
            scene.items.size(), scene.cam_pos[0], scene.cam_pos[1],
            scene.cam_pos[2], m00, m11, n);
      }
      return;
    }
  }
  // Game shadow-pass parity (see g_frame_ortho_ctx): mark the items whose
  // ctx the game submitted through an ortho caster bank. Runs after every
  // item source (merge, rescues, gap fill) and is OR-only. The ortho
  // capture races the publish (more often the higher the render rate), and
  // a fresh capture defaults false: marking from THIS frame's set alone
  // blinked the skater's shadow off for single frames while the body kept
  // rendering. A ctx therefore counts as a caster while it was
  // ortho-submitted within the last kOrthoHoldFrames build frames;
  // never-submitted pieces (the trucker hat) stay non-casters forever.
  {
    static std::unordered_map<uint32_t, uint64_t> s_ortho_last;
    static uint64_t s_ortho_frame = 0;
    ++s_ortho_frame;
    if (s_ortho_last.size() > 4096) {
      s_ortho_last.clear();
    }
    for (uint32_t ctx : ortho_ctx) {
      s_ortho_last[ctx] = s_ortho_frame;
    }
    constexpr uint64_t kOrthoHoldFrames = 60;
    for (DrawItem& item : scene.items) {
      if (item.ctx == 0) {
        continue;
      }
      const auto it = s_ortho_last.find(item.ctx);
      if (it != s_ortho_last.end() &&
          s_ortho_frame - it->second <= kOrthoHoldFrames) {
        item.shadow_caster = true;
      }
    }
  }
  g_last_publish_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count(),
                          std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_scene_mutex);
  scene.generation = ++g_generation;
  g_scene = std::make_shared<const FrameScene>(std::move(scene));
}

// StartRecording / WriteRecording: native/skate3_native_diagnostics.cpp.

}  // namespace skate3::native_scene

// Sk8::Render::ViewCamera::SetViewMatrix(const rw::math::Matrix44&): the
// game passes each frame's freshly computed view matrix here and derives
// the stored view (+0x20), its transpose (+0xE0), the view-proj (+0xA0) and
// the frustum data used for culling from the argument. While the freecam is
// engaged, rewrite the incoming matrix for the MAIN scene camera (the one
// the frame build publishes as g_sampler_viewcam) with the flown view
// before the game consumes it; culling, submission and every derived
// consumer then operate around the drone pose natively. Other ViewCameras
// (shadow cascades, portrait render-to-texture passes) pass through
// untouched.
extern "C" REX_FUNC(sub_82802A00) {
  namespace ns = skate3::native_scene;
  const uint32_t cam = ctx.r3.u32;
  const uint32_t mtx = ctx.r4.u32;
  if (ns::g_freecam_guest_active.load(std::memory_order_acquire) != 0 &&
      mtx != 0 &&
      cam == ns::g_sampler_viewcam.load(std::memory_order_relaxed)) {
    float v[16];
    {
      std::lock_guard<std::mutex> lock(ns::g_freecam_guest_mutex);
      std::memcpy(v, ns::g_freecam_guest_view, sizeof(v));
    }
    for (int i = 0; i < 16; ++i) {
      uint32_t raw;
      std::memcpy(&raw, &v[i], 4);
      raw = __builtin_bswap32(raw);
      std::memcpy(base + mtx + i * 4, &raw, 4);
    }
    ns::g_freecam_guest_rewrites.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82802A00(ctx, base);
}

