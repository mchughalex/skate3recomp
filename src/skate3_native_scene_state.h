#pragma once

// Shared state of the native scene renderer, used by both of its translation
// units: skate3_native_scene.cpp (game-thread capture + frame build) and
// skate3_native_scene_gpu.cpp (render-thread RHI drawing). Everything here is
// written on one thread and read on another as documented per member; inline
// variables keep a single instance across the TUs.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_native_scene.h"

REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ring);

namespace skate3::native_scene {

inline std::mutex g_scene_mutex;
// Published scene: immutable once published (BuildFrameScene builds a fresh
// one per guest frame). shared_ptr so the render thread grabs a reference
// under the mutex instead of deep-copying ~800 items (with per-item heap
// vectors) every frame.
inline std::shared_ptr<const FrameScene> g_scene;
inline uint64_t g_generation = 0;
// Steady-clock stamp of the last scene publish (BuildFrameScene only
// publishes when a perspective view submitted this frame). YieldForMenus
// uses its freshness to tell the in-game pause menu (the world keeps
// resubmitting behind the menu) apart from loading screens and the boot
// frontend (publishes stop), for skate3_native_render_scene_pause_native.
inline std::atomic<int64_t> g_last_publish_ns{-1};
// Debug-dialog cache flushes: consumed at the top of RenderScene so texture/
// mesh-affecting toggles (mip chains, 565 fixes, ...) take effect immediately
// instead of only for newly streamed content.
inline std::atomic<bool> g_flush_textures{false};
inline std::atomic<bool> g_flush_meshes{false};
// Off-screen retention clear request (see g_retained_items further down):
// set on the menus/loading flip (render thread) and the F5 re-enable,
// consumed at the top of BuildFrameScene's retention block (guest thread).
inline std::atomic<bool> g_retained_clear{false};

// Camera/bone signal recorders + synthetic-pan probe + offline recording:
// state and file writers live in native/skate3_native_diagnostics.cpp
// (native/skate3_native_diag.h); capture call sites below read per-frame
// locals and stay here.
inline std::atomic<uint8_t*> g_guest_base{nullptr};
inline std::atomic<uint64_t> g_frames_rendered{0};

// Loading -> gameplay takeover. Armed while the game reports menus/loading
// (and by the F5 enable toggle, hence the atomic); the loading-screen
// prewarm (below) does the decode heavy lifting behind the load, and the
// FIRST substantial post-load scene renders natively immediately, no
// emulated-gameplay stretch. The frames right after takeover run a budgeted
// "settle" decode pass for whatever prewarm missed (dynamic entities,
// late-registered textures); the draw path's miss budgets are clamped while
// settling so leftovers render white/skip for a frame instead of freezing
// the takeover frame. Everything except the armed flag is touched on the
// render thread only, inside RenderScene.
inline std::atomic<bool> g_warmup_armed{true};
// BuildFrameScene stops publishing during loading (no submissions / no
// perspective view), so g_scene holds the PREVIOUS map's scene through the
// whole loading screen. Only scenes published at or after this generation
// (stamped while the loading screen shows) are eligible for takeover;
// rendering the stale scene shows old-map garbage at the flip.
inline uint64_t g_warmup_fresh_generation = 0;
// Settle window: frames (of the native frame counter) still running the
// per-frame settle decode pass after a takeover.
inline uint64_t g_settle_until_frame = 0;

// Loading-screen prewarm queue: world meshes pushed by the
// tROptiMeshData::Unfix hook (the rw-arena LOAD-time pointer resolve; it
// fires once per world mesh as its arena streams in, i.e. exactly during
// loading screens and gameplay world streaming). The render thread drains
// the queue in RenderScene: with the prewarm budget behind the loading
// screen (where the heavy lifting belongs), with the warmup budget's
// leftover while warming, and with a small fixed slice during gameplay
// (streamed-in areas decode before their first draw instead of hitching
// it). Entries whose buffer objects are not initialized yet retry a bounded
// number of drains.
struct PrewarmEntry {
  uint32_t mesh;
  uint16_t retries;
  // Environment-cube entry (mesh == 0, tex != 0): the one object-keyed
  // texture path left (see EnqueueCubeMiss).
  uint32_t tex = 0;
  // Fetch-words entry (mesh == 0, tex == 0, wkey != 0): a content-store
  // miss decoded from the captured stable words snapshot (see
  // EnqueueWordsMiss): every 2D/3D texture miss and heal.
  uint64_t wkey = 0;
  uint32_t words[6] = {};
  // Environment-cube entry (tex != 0 && cube): a cube-cache miss: a single
  // cube decode measured up to ~100 ms, the largest remaining traversal
  // hitch when a vehicle / reflective area streamed in.
  bool cube = false;
  // Draw-path miss (vs speculative prewarm registration): the result is
  // visible RIGHT NOW, so the commit takes it this frame regardless of the
  // gameplay per-frame commit cap.
  bool miss = false;
  // 2D/HUD overlay miss (large-art async routing, see
  // skate3_native_render_scene_2d_async_px): the commit skips the
  // payload-stability verify for these; APT re-rasterizes animating UI
  // art every guest frame, so "payload moved between decode and commit" is
  // the NORMAL state mid-animation and the verify would reject every
  // commit, freezing the element (spinners, ramping fades). The per-frame
  // content probe in the 2D resolve is the heal path for torn reads, the
  // same exposure the old inline decode had.
  bool ui = false;
};
inline std::mutex g_prewarm_mutex;
inline std::condition_variable g_prewarm_cv;  // wakes the decode workers
inline std::vector<PrewarmEntry> g_prewarm_queue;
// Draw-path misses (EnqueueMeshMiss/EnqueueWordsMiss/EnqueueCubeMiss):
// content that is visible RIGHT NOW (white / skipped),
// served FIFO with strict priority over the speculative registration
// backlog in g_prewarm_queue. Guarded by g_prewarm_mutex.
inline std::vector<PrewarmEntry> g_miss_queue;
// Steady-state draw-path misses currently being decoded on the workers (one
// in-flight decode per key; the commit erases). Guarded by g_prewarm_mutex.
// This is the panning-hitch fix: a texture decode averages ~10 ms (max ~70 ms
// measured) and a pan surfaces dozens of new textures/meshes in one frame;
// decoding them inline on the render thread was the lag spike. Misses now
// render white / keep the previous decode for the 1-3 frames the workers
// need.
inline std::unordered_set<uint32_t> g_miss_inflight_mesh;
inline std::unordered_set<uint32_t> g_miss_inflight_tex;
inline std::unordered_set<uint64_t> g_miss_inflight_words;
// Dynamic-payload decode jobs: cloth-sim garments (ropa) have their vertex
// buffers rewritten by the CPU sim EVERY frame, so their mesh decode used to
// run inline on the render thread every frame (~0.7 ms per garment; a
// 4-garment outfit = ~2.9 ms = the 160 fps cap during real play once
// everything else was fixed; the demo character wears no ropa, which hid
// it). The guest thread snapshots the changed payload at publish time; a
// worker converts it into fresh upload buffers; PrewarmCommit swaps the mesh
// cache entry. The render thread keeps drawing the previous decode, one
// frame of cloth lag, invisible (the ropa sim is frame-paced anyway).
struct DynDecodeJob {
  DrawItem item;            // offsets/formats/fingerprint (bones not needed)
  uint64_t seq = 0;         // enqueue order; the commit drops stale results
  std::vector<uint8_t> vb;  // big-endian guest payload snapshots
  std::vector<uint8_t> ib;
};
inline std::vector<DynDecodeJob> g_dyn_jobs;  // FIFO; guarded by g_prewarm_mutex
// Push-side dedupe: clone instances share one tRModelData, so the same mesh
// registers many times per load, but a permanent per-address set silently
// DISABLED the prewarm for re-streamed content (the streamer reuses the
// same mesh/texture object addresses all session): after a few minutes
// every re-registered mesh was "seen", its re-decode was skipped, and every
// streamed texture surfaced as a draw-path first-sight miss instead:
// 388 white-flash textures in one 45 s traversal = the
// residual medium-distance pop-in. Now a mesh -> last-queued-frame map:
// clone bursts still dedupe inside the window, re-streams past it re-queue
// (registration is the game's own "content changed here" signal, see
// InvalidateCachedItem above). Cleared when the game enters menus/loading.
inline std::unordered_map<uint32_t, uint64_t> g_prewarm_seen;
// Texture-object dedupe for the workers (they cannot read the render
// thread's g_r caches): address -> fetch-words key at last staging; a
// words change at a reused address re-stages, an unchanged one skips.
inline std::unordered_map<uint32_t, uint64_t> g_prewarm_tex_seen;
inline std::atomic<uint64_t> g_prewarm_done{0};     // entries fully warm-decoded
inline std::atomic<uint64_t> g_prewarm_dropped{0};  // entries given up on
inline std::atomic<bool> g_prewarm_workers_started{false};
// tInstance::m_pRModel offset, confirmed at runtime by the material
// cross-check probe in OnAddRenderInstance (Skate 2 layout: +0x78).
inline std::atomic<uint32_t> g_instance_rmodel_offset{0};

// guid -> guest renderengine::Texture, from the RegisterTexture hook. Keys
// masked of the top bit: material channel guids carry an extra flag bit
// relative to the registered cAssetIDs.
constexpr uint64_t kGuidMask = 0x7FFFFFFFFFFFFFFFull;
inline std::mutex g_texture_map_mutex;
inline std::unordered_map<uint64_t, uint32_t> g_texture_map;

// VS constant staging bank (bank id 0x4000, device+1920).
// D3D::SetPending_AluConstants(device, dirty group mask, bank, ptr) is
// called from inside the guest Draw* functions; ptr is the device's
// positional 256-register shadow (partial uploads leave the rest intact).
// Two verified layouts: pre-pass keeps the per-draw transform / bone
// palette at c4; main-pass keeps camera at c4, palette at c7, rigid world
// at c8..c11 (see BankPaletteBase / BankRigidWorld). Because the dynamic
// capture runs right after RenderMesh returns, the bank holds exactly that
// mesh's constants when its draws ran inline.
inline std::atomic<uint32_t> g_vs_bank{0};
// Pixel constant shadow bank (bank id 0x4400, device+6016): material colors
// (e.g. the CAS hair tint) are staged here per draw.
inline std::atomic<uint32_t> g_ps_bank{0};
// Global per-frame fog rows (see FrameScene::fog_ramp): main-pass VS banks
// keep the camera at c4 and the frame's fog params at c5 (ramp) / c6 (color)
// - identical across every main-pass draw of a frame (verified from a
// recorded draw stream). OnDrawDone grabs them from the first 3D draw whose
// c4 matches the last built scene's camera; BuildFrameScene publishes them
// and re-arms the capture. All on the guest render thread.
inline float g_fog_cam[3] = {};
inline float g_fog_rows[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
inline bool g_fog_have = false;
inline bool g_fog_frame_done = false;
// Sky-dome viewpos height (see FrameScene::sky_height): sky.fx's VS adds
// g_vViewPos to every dome vertex, but the game feeds the SKY draw a viewpos
// whose Y is a fixed level elevation (165.0 in every Port Carverton capture)
// instead of the camera's; the baked skyline must not bob with the skater.
// Captured from the sky bank's unique signature: c4.xz matches the camera
// while c4.y sits far above it (every other main-pass draw has c4 == camera;
// water-reflection passes mirror y DOWNWARD and are excluded by dy > 0).
inline float g_sky_height = 165.0f;  // every Port Carverton capture to date
inline bool g_sky_have = false;
inline bool g_sky_frame_done = false;
// Sky sun rows from the same draw's PIXEL bank (see FrameScene::sky_sun):
// light dir (c0.xyz), sun angular scale (c4.x), pre-tone multiplier (c4.y),
// exposure (c3.x). Persist like g_sky_height once captured.
inline float g_sky_sun[6] = {};
inline bool g_sky_sun_have = false;
// The CAPTURED (un-overridden) sun direction, published each frame for the
// debug dialog: enabling the sun override seeds its azimuth/elevation from
// this so the sun starts at its true position instead of jumping.
inline std::atomic<float> g_sun_captured[3] = {0.0f, 1.0f, 0.0f};
// UI background blur (see FrameScene::ui_blur and kBlurShaderSource): while
// a frontend popup is up the game appends blur_hBlur/vBlur + basictex passes
// after the postfx uber. g_ui_blur holds the captured kernel scale (PS c0.x,
// 8 in every capture); g_ui_blur_seen latches per frame on the blur_hBlurPS
// draw and is cleared at publish; the pass chain only exists while the
// popup is actually up, so this can never stick on.
inline float g_ui_blur = 8.0f;
inline bool g_ui_blur_seen = false;
// Blur modulate color (the blur passes' PS c1): both blur ucodes end in
// `mul oC0, r0, c1`: the game's menu fade. Gameplay popups stage (1,1,1)
// (why the exact port originally had no multiply); the pause menu stages
// ~(0.35,0.33,0.32), squared across the H+V passes = the darkened pause
// backdrop. Read live on the blur_hBlurPS draw, accepted only when the
// bank's c0 kernel row reads exactly (8,8); a stale mid-stage bank fails
// that gate (the staleness class that pulsed the blur radius when c0.x was
// fed live, see g_ui_blur above).
inline float g_ui_blur_color[3] = {1.0f, 1.0f, 1.0f};
// Hold the blur across publishes that carry no blur draw: the game does not
// issue the pass chain on every swap while the popup is up, and publishing
// the raw per-swap flag alternated blur on/off: a visible brightness
// shimmer. Two publishes of hysteresis bridges the gaps; on popup close the
// blur lingers ~2 guest frames, imperceptible.
inline int g_ui_blur_hold = 0;
// Host settings-overlay backdrop: forces the same blur chain while the SDK
// settings menu is open (the game issues no popup blur then). Set from the
// app UI thread, read at scene publish.
inline std::atomic<bool> g_settings_menu_blur{false};
// Emulated-post blur diagnostics: reset on each menu open so every open
// logs a fresh burst.
inline std::atomic<uint32_t> g_post_blur_log_count{0};
// Selected-object outline capture (see DrawItem::selected): in the park
// editor / object mover the game EXCLUDES the selected object from the main
// color pass and re-draws it right after the sky, twice, back to back, the
// stencil-marking passes the postfx edge-detect turns into the blue contour
// (verified in capture: consecutive environmentpark decal +
// diffuse parts, identical VS banks, vs the same mesh at OTHER placements in
// the main pass). Hook side records each post-sky environmentpark/
// dynamicobject draw's (ib, vb, world translation); entries seen >= 2 times
// mark the matching published items. Guest render thread only, like the fog
// capture state.
struct SelectedDrawKey {
  uint32_t ib;
  uint32_t vb;
  float t[3];
  uint32_t count;
};
inline bool g_sky_seen_this_frame = false;
inline std::vector<SelectedDrawKey> g_frame_selected;
// The guest issued its postfx_edgedetectstencil draw this frame: the game's
// own "an outline is being drawn" signal (park editor / object mover with an
// active selection). Post-sky double-draws alone are NOT sufficient evidence
// of a selection: normal gameplay draws some small props twice after the sky
// (far LOD/imposter passes), which used to sprout phantom outlines on distant
// objects. Guest render thread only; reset per frame in BuildFrameScene.
inline bool g_outline_edge_seen = false;
// Outline color, refreshed from the guest postfx_edgedetectstencil draw's
// PS c0 (the park-editor blue in every capture) whenever that pass runs.
inline float g_outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
// PIXEL banks keep the CSM constants at c0..c8, pass-global (identical on
// every environment-family draw of the pass; character/hair/tree PSes
// allocate differently and are rejected by the sanity gate). Captured on the
// same camera-keyed main-pass draws as the fog rows.
inline float g_shadow_rows[48] = {};
inline bool g_shadow_have = false;
inline bool g_shadow_frame_done = false;
// Guest frame of the last SANE capture. Interior venues (the park-editor
// warehouses) run a different environment shader whose bank never passes the
// sanity gate (near-zero sun, exposure ~0.001), so the rows latch stale
// outdoor state there; consumers that must not act on stale sun state key
// off this freshness stamp (see FrameScene::shadow_fresh).
inline uint64_t g_shadow_rows_frame = 0;
// Frame-global rows of the tree / proxyworld shader families (see
// FrameScene::family_rows), captured from their PS banks when a draw with
// the matching debug path runs. Guest render thread only.
inline float g_family_rows[4] = {0.3435f, 0.02f, 1.0f, 0.45f};
inline bool g_tree_frame_done = false;
inline bool g_proxy_frame_done = false;
// flowingwateralpha m_params + animation-time rows (see
// FrameScene::water_rows), captured from a flowingwateralpha PS bank
// (debug-path classified). Guest render thread only.
inline float g_water_rows[17] = {};
inline bool g_water_have = false;
inline bool g_water_frame_done = false;
// ocean_defaultPS / oceanreflection_defaultPS rows (see
// FrameScene::ocean_rows / oceanrefl_rows). Guest render thread only.
inline float g_ocean_rows[40] = {};
inline bool g_ocean_have = false;
inline bool g_ocean_frame_done = false;
inline float g_oceanrefl_rows[4] = {};
inline bool g_oceanrefl_have = false;
inline bool g_oceanrefl_frame_done = false;
// scrollincandescent.fx rows (see FrameScene::scroll_rows): [0] =
// g_fAnimationTime (VS c9.x), [1] = the material multiplier m_params[0].y
// (PS c3.y). Captured from a scrollincandescent draw (debug-path
// classified). Guest render thread only.
inline float g_scroll_rows[2] = {};
inline bool g_scroll_have = false;
inline bool g_scroll_frame_done = false;
// dynamicobject.fx frame-global lighting rows (see FrameScene::dynobj_rows),
// captured from a dynamicobject/alphatestdynamicobject PS bank (debug-path
// classified). Guest render thread only.
inline float g_dynobj_rows[10] = {};
inline bool g_dynobj_have = false;
inline bool g_dynobj_frame_done = false;
// dynamicobject static world-shadow transform rows (PS c5/c6/c7), captured
// alongside g_dynobj_rows, see FrameScene::dynobj_ws.
inline float g_dynobj_ws[12] = {};
inline bool g_dynobj_ws_have = false;
// Dynamic entities (characters, movable props) live entirely in transient
// per-frame arenas: the context, its record, mesh pointers, transforms and
// bone palettes are all recycled before frame end. The complete DrawItem is
// therefore built at RenderMesh hook time and stored here.
inline std::mutex g_palette_mutex;
inline std::vector<DrawItem> g_frame_dynitems;
// (ib_obj << 32 | vb_obj) -> indices of pending items in g_frame_dynitems,
// for the post-draw state fixup (deferred / world-path captures draw after
// their capture; the fixup fills palette or world from the actual draw).
inline std::unordered_multimap<uint64_t, size_t> g_frame_pending_by_buffers;
// Contexts submitted through an ORTHO (shadow caster-cascade) bank this
// frame, i.e. items the GAME drew into its own shadow passes. The game's
// caster list is per-piece: the CAS trucker hat submits ONLY to the main
// view (verified in capture: every char piece has shadow-pass draws
// except the hat); natively casting it painted a hard low-sun brim band
// across the editor face that the emulated frame never shows. The native
// atlas pass renders char pieces only when their ctx is in this set
// (rescued/gap items inherit last frame's flag). Guarded by
// g_palette_mutex; cleared at the end of BuildFrameScene.
inline std::unordered_set<uint32_t> g_frame_ortho_ctx;
// (ib_obj << 32 | vb_obj) -> dynitem indices whose character-lighting rows
// should refresh on every later matching draw this frame. The one-shot
// palette fixup usually pops on the shadow-CASTER draw, whose PS bank is
// stale (shadowPS uploads no PS constants); re-capturing on the main-pass
// draw (last writer) lands the real per-instance rows (stamp tints etc.).
inline std::unordered_multimap<uint64_t, size_t> g_frame_char_refresh;
// (ib_obj << 32 | vb_obj) -> device fetch-shadow slots 3+4 (12 dwords) at
// this frame's LAST indexed 3D draw with those buffers (the z-prepass draws
// first, so the main pass wins). Consumed at frame end by the
// streamed-artwork diffuse override (DrawItem::diffuse_fetch): poster/advert
// materials keep a 16x16 min-mip placeholder in the material channel while
// the engine binds the streamed art via the fetch constants at draw time.
// Guarded by g_palette_mutex; cleared at the end of BuildFrameScene.
inline std::unordered_map<uint64_t, std::array<uint32_t, 12>> g_frame_draw_fetch;
// mesh -> last VALIDATED character-lighting rows (see CaptureCharLighting).
// The per-frame capture depends on a fragile draw-order chain (capture at
// submit-exit -> pending fixup -> per-draw refresh, commit-on-success); on
// frames where no draw in the chain carried this character's main-pass PS
// bank the item ends with INVALID rows and the renderer falls back to the
// legacy empirical shading; the player visibly flickered between the two
// looks while moving, and the hair left the blended sub-pass (opaque "hair
// helmet" covering the cap crown). The rows are frame-coherent (sun/ambient
// move slowly), so reusing the previous valid capture is visually seamless.
// TRAP: NPC clones share a mesh with PER-INSTANCE rows (stamp tints, the
// teal-vest twins), so the fallback applies only to meshes with a single
// instance in the frame. Guest-render-thread only (capture hooks +
// BuildFrameScene), no lock needed.
inline std::unordered_map<uint32_t, std::array<float, 72>> g_char_rows_cache;
// Frame-global character CSM receive biases (see FrameScene::char_shadow_bias)
// + the guest frame they were captured on (served while <120 frames stale;
// they only change with the scene lighting setup). Guest render thread only.
inline float g_char_shadow_bias[3] = {};
inline uint64_t g_char_shadow_bias_frame = 0;
inline std::atomic<uint64_t> g_char_rows_reused{0};
// Cached palette entry for the per-instance rescue below. A refused
// capture re-publishes with the cached palette: one frame of pose lag is
// invisible; a one-frame disappearance is not. Guest-render-thread only.
struct CachedBones {
  std::vector<float> bones;
  // g_guest_frame at refresh: rescues/heals must be FRESH; a close-pass
  // refusal resurrecting a seconds-old palette rendered the vehicle 10-20 m
  // BEHIND its live position (the momentary ghost-back).
  uint64_t frame = 0;
};
// ctx -> last published palette for skinned character-family items. The
// MeshContext is the game's own per-instance identity, so this cache
// rescues each instance with ITS OWN last palette regardless of how many
// clones are alive.
inline std::unordered_map<uint32_t, CachedBones> g_bones_cache_ctx;
inline std::atomic<uint64_t> g_lw_ctx_rescued{0};
// LW entity store consumption counters (stats line lw[...]).
inline std::atomic<uint64_t> g_lw_stamped{0};
inline std::atomic<uint64_t> g_lw_fade0{0};
inline std::atomic<uint64_t> g_lw_gap_filled{0};
// Authoritative caster-palette substitutions + per-ctx lighting-rows serves
// (edge-of-view vehicles; see the stamp pass).
inline std::atomic<uint64_t> g_lw_pal_sub{0};
inline std::atomic<uint64_t> g_lw_rows_served{0};
// ctx -> last VALIDATED lighting/paint rows, entity-checked (a recycled
// instance must not inherit the previous occupant's paint). The mesh-keyed
// g_char_rows_cache fallback is gated to single-instance meshes; cloned
// traffic never qualified, so a caster-only stretch (main view culls the
// vehicle at the screen edge before it leaves the screen) dropped to
// legacy flat shading: the "vehicle loses its texture/color at the edge"
// sighting.
struct CharRowsCtx {
  std::array<float, 72> rows;
  uint32_t entity = 0;
};
inline std::unordered_map<uint32_t, CharRowsCtx> g_char_rows_cache_ctx;
// (mesh,ctx) -> last VALIDATED rows for EVERY character item with an
// instance context: the general rescue the two caches above each cover
// only a slice of: the mesh-keyed cache is gated to single-instance meshes
// (character pieces also publish caster items of the same mesh, so gameplay
// pieces rarely qualify) and the ctx cache is gated to LW-mapped entities
// (the PLAYER is not one). A capture-failed frame without a rescue drops
// hair out of the blended sub-pass onto the legacy opaque path: a
// frame-to-frame hair/garment appearance flip (the
// failure rate scales with guest fps, so it reads as a high-fps
// flicker). Entity-stamped like CharRowsCtx (0 for non-LW instances) so a
// recycled ctx never inherits the previous occupant's rows.
struct CharRowsInst {
  std::array<float, 72> rows;
  uint32_t entity = 0;
};
inline std::unordered_map<uint64_t, CharRowsInst> g_char_rows_cache_inst;
// Entity-keyed lighting rows for ROPA garments only: the sim on/off switch
// swaps between a garment's two renderables (cloth-target vs original
// model, different mesh AND ctx), so the (mesh,ctx) caches never carry
// rows across the switch and the freshly resolved model rendered dark/flat
// until its own capture validated. Both models map to the same owning
// entity in the identity store, and they are the same garment; sharing
// rows across the pair is exact. Garment-to-garment only; body pieces keep
// their per-piece caches.
struct CharRowsEnt {
  std::array<float, 72> rows;
  uint8_t fam = 0;  // donor family: the row layout is family-specific
};
inline std::unordered_map<uint32_t, CharRowsEnt> g_char_rows_cache_ent;
inline std::atomic<uint64_t> g_char_rows_ent_served{0};
inline std::atomic<uint64_t> g_char_rows_inst_served{0};
// Items served authoritative pack palettes this frame / total (see
// native_palette::ServeAuthoritativePalettes).
inline uint32_t g_pal_served_frame = 0;
inline std::atomic<uint64_t> g_pal_served_total{0};
// Route-flip probe: counts hair/character-alpha pieces whose pass route
// (blended sub-pass vs legacy opaque) changed between consecutive frames;
// each count is one visible flicker event.
inline std::atomic<uint64_t> g_hair_route_flips{0};
// A,B,A,B alternation probes: bone palettes (per piece) and the shadow
// cascade state (scene-wide) returning EXACTLY to the previous-but-one
// frame's value (legitimate animation/lighting drift never does).
inline std::atomic<uint64_t> g_hair_bone_alternations{0};
inline std::atomic<uint64_t> g_shadow_alternations{0};
// ctx -> last PUBLISHED item of a live LW entity (skinned character
// families, non-ropa): when a ctx drops out of the submit records for a
// frame or two while its entity is still alive in the store (observed as
// "dyn publish GAP" incidents on fam-3/5 ped meshes, 1-3
// frames each: an in-view NPC vanishing for a frame reads as a blink or,
// moving, a small teleport), the whole item republishes. Console behavior:
// a live entity never skips a frame. Self-limiting: entries expire after 2
// unpublished frames and refresh only from LIVE publishes (never from a
// fill, dbg_src 9), so a real despawn shows at most 2 filled frames, at
// the entity's own served alpha.
struct LwRetained {
  DrawItem item;
  uint64_t frame = 0;
};
inline std::unordered_map<uint32_t, LwRetained> g_lw_last_items;
// mesh -> last RESOLVED ropa garment state (rigid world OR skinned palette).
// Ropa items must NOT ride the palette-only rescue above: a ropa capture is
// refused exactly when the bank is stale (g_ropa_stale), and while the cloth
// sim is ACTIVE the VB holds sim-deformed root-local positions; skinning
// those with a cached palette is the mangled-ribbon interpretation (verified
// 0/31 in-clip; on-screen as map-length stretched strips, with matching
// shadows since the caster pass shares the item). And with no rescue at all
// a refused frame drops the garment (the momentary invisible torso). The
// dyn decode workers already keep the drawn cloth VB one frame behind the
// sim, so re-publishing last frame's resolved MODE + transform is exactly
// age-consistent with the vertices being drawn. Single-instance meshes only
// - clones share meshes with per-instance transforms. Guest-render-thread
// only.
struct RopaResolvedState {
  bool skinned = false;
  float world[16] = {};
  std::vector<float> bones;
  // Publish frame of the cached state: the rescues below exist to bridge
  // 1-3 frame capture gaps, and re-publishing an unbounded-age state pins a
  // ghost of the garment at a stale pose for as long as captures keep
  // failing (the frozen-in-the-air garment behind a skater whose capture
  // path was refusing every frame).
  uint64_t frame = 0;
};
inline std::unordered_map<uint32_t, RopaResolvedState> g_ropa_state_cache;
inline std::atomic<uint64_t> g_ropa_rescued{0};
// mesh -> sample verts of the most recent DECODE of a skinned mesh (bind-
// space position + raw blend attribs, ~32 evenly spaced), written by
// DecodeMesh on whichever thread decodes (workers / render), read by the
// guest thread's draw-time STRETCH VETO in BuildFrameScene. The veto skins
// these samples with the FINAL post-interpolation palette, i.e. it judges
// what the GPU will ACTUALLY draw. Every upstream gate (capture acceptance,
// publish coherence) judges the LIVE guest VB instead, so a decode-content
// vs palette pairing mismatch, or junk introduced by the interpolation
// substitutions, passes all of them and still flashes the 1-frame
// map-length ribbon (observed with every ropa[] counter clean).
// `fp` = the decode's payload fingerprint so the veto can log whether the
// drawn content even matches the frame's payload (content lag vs junk
// palette: the decisive diagnostic split).
struct SkinProbeSample {
  float p[3];
  uint32_t bw;
  uint32_t bi;
};
struct SkinProbe {
  std::vector<SkinProbeSample> s;
  uint64_t fp = 0;
};
inline std::mutex g_skin_probe_mutex;
inline std::unordered_map<uint32_t, SkinProbe> g_skin_probe;
inline std::atomic<uint32_t> g_cur_ib{0};
inline std::atomic<uint32_t> g_cur_vb{0};
inline std::atomic<uint32_t> g_cur_vb_offset{0};
inline std::atomic<uint32_t> g_cur_vb_stride{0};
// Guest D3D::CDevice: the fetch-constant shadow lives at device+0x480,
// 6 dwords per fetch group (from recompiled SetPending_FetchConstants).
inline std::atomic<uint32_t> g_device{0};
// 2D/APT phase depth counters (see On2dPhase). All HUD/menu 2D elements are
// Flash SWFs converted to APT; draws issued inside these brackets are the 2D
// draw stream.
inline std::atomic<uint32_t> g_phase2d_depth[6];
inline std::atomic<uint64_t> g_draws_2d{0};
// 2D draws seen through paths the overlay does not replay yet
// (DrawIndexedVertices / DrawVertices in the 2D phase; gameplay HUD uses
// only the BeginVertices inline path, verified by capture).
inline std::atomic<uint64_t> g_draws_2d_other{0};
inline std::atomic<uint64_t> g_draws_2d_dropped{0};
// Large-art async decode routing (see skate3_native_render_scene_2d_async_px):
// quads skipped while their first decode is in flight on the workers, and
// stale decodes served while a content-change re-decode is in flight.
inline std::atomic<uint64_t> g_2d_async_skip{0};
inline std::atomic<uint64_t> g_2d_async_stale{0};

// One captured 2D draw (verified layout): BeginVertices
// inline vertices, stride 24 = {float x, y, z, w; float u, v} in 1280x720
// APT movie space; VS c0..c3 = ortho projection rows, c4..c7 = the
// element's 2D transform, c8 = color multiplier; diffuse texture in fetch
// shadow slot 0. Vertex payload is written by the CPU after BeginVertices
// returns, so it is read at frame end (addr) rather than at capture.
// NOTE on render-to-texture: in gameplay the ENTIRE HUD renders inside
// AptRenderingIntegration::UpdateRenderToTexture (bracket bit 3) into a
// screen-sized overlay texture, at true screen coordinates; the game
// composites that overlay over the 3D frame in a later (emulated,
// suppressed) pass. Replaying the draws directly onto the native output IS
// that composite, so bit 3 is diagnostic only, not a routing signal. The
// overlay content is straight-alpha art (verified by decoding the clock
// face/needle/sheen textures offline).
struct Draw2d {
  uint32_t prim;    // xenos primitive: 4 trilist, 5 tristrip, 13 quadlist
  uint32_t count;   // vertex count
  uint32_t stride;  // bytes per vertex at capture (see publish normalization)
  // Capture-time stride, preserved across the publish normalization (which
  // rewrites stride to the renderer's 28). The FMV substitution keys on it:
  // the movie quad is the stride-24 textured layout inside the
  // AptMovieIntegration bracket (bit 1).
  uint32_t src_stride = 0;
  uint32_t addr;    // guest inline-ring write pointer
  uint32_t flags;   // bracket bits at capture (layout disambiguation)
  // Texture fetch constants, shadow slots 0-2 (6 dwords each). Slot 0 is
  // the draw's own texture (all the replay paths read only [0..5]); slots
  // 1-2 exist for the VIDEO quads, whose console YUV shader binds the U and
  // V planes there; the replay's YUV-triple detection reads them.
  uint32_t fetch[18];
  float consts[36];   // VS c0..c8
  std::vector<uint8_t> verts;  // filled at frame end (little-endian dwords)
};
inline std::mutex g_2d_mutex;
inline std::vector<Draw2d> g_frame_2d;  // capture in submission order
inline std::vector<Draw2d> g_scene_2d;  // published at frame end
inline uint64_t g_scene_2d_generation = 0;

// One captured in-world neon spline draw (waypoint arrows / marker beams;
// decoded from a live capture + the game's own spline.fx source): a
// DrawVertices triangle strip whose 12-byte float3 "vertices" are only
// parameters: x = control-point index + fractional t, y = U texcoord,
// z = side flag (0/1) selecting the extrusion offset and V texcoord. The
// guest VS (43309A8C) evaluates a uniform cubic B-spline through i_cp[]
// (VS c7..) transformed by world columns c4..c6, adds the world-rotated
// extrusion offset c[151 + z], projects by VP columns c0..c3, and fades by
// clip-z against i_clipvalues (c150) with i_intensity (c149) as the pass
// gain. Two passes per element: spline_darkenPS (straight alpha) then
// spline_defaultPS (additive glow), both depth-tested with no z-write,
// replayed inside the native scene pass with the spline evaluated on the
// CPU at publish time.
struct SplineDraw {
  uint32_t pass;      // 1 = darken (straight alpha), 2 = default (additive)
  uint32_t count;     // strip vertex count
  uint32_t fetch[6];  // texture fetch slot 3 (the neon gradient)
  float consts[153 * 4];  // VS c0..c152 as staged
  // capture: raw big-endian float3 params; publish: evaluated little-endian
  // {float4 clip_pos, float2 uv, float fade} (28-byte stride).
  std::vector<uint8_t> verts;
};
inline std::vector<SplineDraw> g_frame_spline;  // guarded by g_2d_mutex
inline std::vector<SplineDraw> g_scene_spline;
inline std::atomic<uint64_t> g_draws_spline{0};
// Current guest shader objects and render-state shadow bank, for the 2D
// recon recording (grouping the stream by shader / reading blend state).
inline std::atomic<uint32_t> g_cur_ps_obj{0};
inline std::atomic<uint32_t> g_cur_vs_obj{0};
inline std::atomic<uint32_t> g_rs_bank{0};
inline std::atomic<uint32_t> g_cur_viewport[6];
inline std::atomic<uint32_t> g_cur_scissor[4];
// All four vertex streams (vb, offset, stride); cloth binds its simulated
// vertices on a stream other than 0.
inline std::atomic<uint32_t> g_cur_streams[4][3];

// Offline-analysis recording structs/state: native/skate3_native_diag.h.
inline std::atomic<uint64_t> g_vs_uploads{0};
inline std::atomic<uint64_t> g_palette_snapshots{0};
// Palettes captured at base+1 (the cloth/morph VS layout with an extra
// parameter row before the palette, see RefinePaletteBase).
inline std::atomic<uint64_t> g_palette_base_plus1{0};
// Reflective-glass (fam 5/6) normal-map pair telemetry: pair = draws with
// the masks+normal t4/t5 descriptor pair bound (overlay.w == 4), flat =
// spec-bound reflective draws still on the flat-normal path, gate = the
// last no-pair reason bitmask (1 no spec masks, 2 no normal channel, 4
// masks SRV recipe missing, 8 normal unresolved/white, 16 normal invalid,
// 32 normal recipe missing, 64 pair slots exhausted).
inline std::atomic<uint64_t> g_refl_pair{0};
inline std::atomic<uint64_t> g_refl_flat{0};
inline std::atomic<uint32_t> g_refl_gate{0};

// ---- F7 scene-composition ring (see RequestSceneRingDump in the header) ----
struct SceneRingItem {
  uint32_t ctx;
  uint32_t mesh;
  uint32_t diffuse;
  uint32_t lightmap;
  uint32_t vb_obj;
  // Full material texture set: a one-frame spec/normal/macro/decal-art
  // resolution glitch changes shading (e.g. an unmasked sky reflection on a
  // reflective bank face) with the diffuse/lightmap identical; the first
  // ring revision could not see those.
  uint32_t spec;
  uint32_t macro;
  uint32_t decal_art;
  uint32_t wnormal;
  uint32_t indices;      // summed index counts of the item's draw entries
  uint16_t drawn;        // draw calls actually issued (0 = skipped at draw)
  uint8_t env_family;
  uint8_t char_family;
  uint8_t flags;  // 1 transparent 2 water 4 skinned 8 retained 16 pending
                  // 32 caster_bank 64 decal
  uint8_t route;  // 0 skipped pre-pass, 1 opaque pass, 2 blended sub-pass
  // SERVED-content fingerprints, stamped in draw_item after the resolves:
  // the object pointers above cannot see an in-place content swap (a heal
  // commit changes what a texture shows with every pointer identical);
  // a fp that changes A->B->A across the flash frame names the texture,
  // the slot, and both contents. 0 = white fallback / not resolved.
  uint64_t fp_diffuse;
  uint64_t fp_lightmap;
  uint64_t fp_macro;
  uint64_t fp_decal;
};
struct SceneRingFrame {
  uint64_t frame = 0;
  float cam[3] = {};
  // Per-frame captured globals: a one-frame glitch in any of these shifts
  // shading on every consumer with the composition identical.
  float fog[6] = {};        // ramp xyz + color rgb
  float family_rows[4] = {};
  float sky_height = 0.0f;
  bool shadow_valid = false;
  // Shadow-atlas outcome for the frame: a character shadow blinking off
  // for one frame with the item composition identical lives in this pass,
  // and nothing else in the ring could see it.
  bool shadow_ready = false;
  uint16_t shadow_draws = 0;
  bool static_sun_valid = false;
  // The frame's captured cascade rows: receivers project through these, so
  // a single-frame foreign capture moves every dynamic shadow off-target
  // with the composition and the atlas identical. They vary legitimately
  // with the camera, so only a recorded per-frame trace can tell an
  // outlier from drift.
  float shadow_rows[48] = {};
  std::vector<SceneRingItem> items;
};
inline std::deque<SceneRingFrame> g_scene_ring;  // render thread only
constexpr size_t kSceneRingFrames = 2400;
inline std::atomic<bool> g_scene_ring_dump{false};

// Render thread, frame end: write the whole ring as CSV. One F line per
// frame, then one I line per item (hex object addresses to match every
// other diagnostic).
inline void MaybeDumpSceneRing() {
  if (!g_scene_ring_dump.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  char path[128];
  std::snprintf(path, sizeof(path), "logs/scene_ring_%lld.csv",
                static_cast<long long>(std::time(nullptr)));
  std::ofstream f(path);
  if (!f) {
    REXLOG_WARN("native-scene ring: cannot open {}", path);
    return;
  }
  f << "kind,frame,ctx,mesh,diffuse,lightmap,vb,spec,macro,decal_art,"
       "wnormal,indices,drawn,env_fam,char_fam,flags,route,fp_diffuse,"
       "fp_lightmap,fp_macro,fp_decal\n";
  char line[256];
  for (const SceneRingFrame& fr : g_scene_ring) {
    std::snprintf(line, sizeof(line),
                  "F,%llu,cam,%.2f,%.2f,%.2f,items,%zu,fog,%.5f,%.4f,"
                  "%.3f,%.4f,%.4f,%.4f,fam,%.4f,%.4f,%.4f,%.4f,sky,%.1f,"
                  "shadow,%d,atlas,%d,%u,nsm,%d\n",
                  static_cast<unsigned long long>(fr.frame), double(fr.cam[0]),
                  double(fr.cam[1]), double(fr.cam[2]),
                  fr.items.size(), double(fr.fog[0]), double(fr.fog[1]),
                  double(fr.fog[2]), double(fr.fog[3]), double(fr.fog[4]),
                  double(fr.fog[5]), double(fr.family_rows[0]),
                  double(fr.family_rows[1]), double(fr.family_rows[2]),
                  double(fr.family_rows[3]), double(fr.sky_height),
                  fr.shadow_valid ? 1 : 0, fr.shadow_ready ? 1 : 0,
                  unsigned(fr.shadow_draws), fr.static_sun_valid ? 1 : 0);
    f << line;
    f << "S," << fr.frame;
    for (float v : fr.shadow_rows) {
      std::snprintf(line, sizeof(line), ",%.6g", double(v));
      f << line;
    }
    f << "\n";
    for (const SceneRingItem& it : fr.items) {
      std::snprintf(line, sizeof(line),
                    "I,%llu,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,"
                    "%u,%u,%u,%u,%u,%u,%016llX,%016llX,%016llX,%016llX\n",
                    static_cast<unsigned long long>(fr.frame), it.ctx, it.mesh,
                    it.diffuse, it.lightmap, it.vb_obj, it.spec, it.macro,
                    it.decal_art, it.wnormal, it.indices, unsigned(it.drawn),
                    unsigned(it.env_family), unsigned(it.char_family),
                    unsigned(it.flags), unsigned(it.route),
                    static_cast<unsigned long long>(it.fp_diffuse),
                    static_cast<unsigned long long>(it.fp_lightmap),
                    static_cast<unsigned long long>(it.fp_macro),
                    static_cast<unsigned long long>(it.fp_decal));
      f << line;
    }
  }
  REXLOG_INFO("native-scene ring: wrote {} frames -> {}", g_scene_ring.size(),
              path);
}
// Character-lighting capture telemetry: attempts vs validated captures per
// family (see CaptureCharLighting).
inline std::atomic<uint64_t> g_char_attempts{0};
inline std::atomic<uint64_t> g_char_valid{0};
inline std::atomic<uint64_t> g_char_drawn{0};
inline std::atomic<uint64_t> g_dynobj_drawn{0};
inline std::atomic<uint64_t> g_water_drawn{0};  // exact-branch water draws
// character.cloth_ropa items captured in the sim-active RIGID mode (world
// from c188/c191 instead of a bone palette, see CaptureSkinnedState).
inline std::atomic<uint64_t> g_ropa_rigid{0};
// ropa rigid captures REFUSED because the bank's c188/c191 was implausible
// or projected the garment off-clip (stale bank from another mesh's draw);
// the item stays pending for the post-draw fixup.
inline std::atomic<uint64_t> g_ropa_stale{0};
// ropa captures accepted through the RELAXED near-camera criterion (all
// samples in FRONT of the projection inside a loose 6x guard band + sane
// spread) after the strict half-in-clip score refused them. Up close the
// garment fills/overflows the screen and most of the 6 sample verts clip;
// the strict gate refused the CORRECT palette every frame, and with no
// cached resolved state to rescue from the torso stayed invisible until the
// NPC was far enough away again (log signature: `ropa mesh=... score=2
// flag=(1.000,...)` repeating while stale climbed with rescued flat).
inline std::atomic<uint64_t> g_ropa_relaxed{0};
// mesh -> newest enqueued cloth-shape generation (DynDecodeJob seq). GUEST
// render thread only: written by the dyn-job enqueue, read by the interp
// ring's pose ingestion (the pose <-> shape pairing).
inline std::unordered_map<uint32_t, uint64_t> g_ropa_last_seq;
// Shape blends served / skipped (generation missing from the ring) per
// stats window.
inline std::atomic<uint64_t> g_ropa_blend_drawn{0};
inline std::atomic<uint64_t> g_ropa_blend_miss{0};
// Ropa garments PUBLISHED with a caster-cascade (ortho) bank's palette:
// the ~40 ms-stale rows behind the garment-offset-from-body symptom. The
// same-mode graft in the dyn_slot merge should keep this at ~0 whenever a
// perspective capture exists in the same frame.
inline std::atomic<uint64_t> g_ropa_caster{0};
// Published skinned items whose palette FAILED the dense 32-sample
// publish-time coherence gate (see PublishedPaletteSane): the 1-frame
// junk-palette ribbon flash that passes every 6-sample capture gate. The
// item re-publishes its cached state instead (or drops for one frame).
inline std::atomic<uint64_t> g_pub_incoherent{0};
// Draw-time STRETCH VETO trips (see g_skin_probe): the final palette skins
// the resident decode's sample verts wider than bind size; the item's
// draws are cleared for the frame (blink, not ribbon) and the first events
// dump full palette + samples to logs/stretch_*.txt for offline diagnosis.
inline std::atomic<uint64_t> g_stretch_veto{0};
// skinned/ropa meshes that published after a 1-3 frame publish GAP (was on
// screen, vanished, came back): the "flickered invisible" telemetry.
inline std::atomic<uint64_t> g_dyn_gap{0};
inline std::atomic<uint64_t> g_skinned_items{0};
inline std::atomic<uint64_t> g_skinned_skipped{0};
// Rigid items whose transform had to wait for the post-draw fixup (deferred
// draws), and those still pending at frame end (dropped).
inline std::atomic<uint64_t> g_rigid_pending{0};
inline std::atomic<uint64_t> g_rigid_dropped{0};
// Pending rigid clones re-published with their cached world at merge time
// (see g_rigid_world_cache).
inline std::atomic<uint64_t> g_rigid_rescued{0};
// Rigid worlds served from the guest instance matrix (ReadCtxInstanceWorld)
// instead of a constant bank.
inline std::atomic<uint64_t> g_rigid_ctx_world{0};
// Sort-list piece copies published with the instance matrix (see
// g_dyn_dispatch_meshes).
inline std::atomic<uint64_t> g_sortlist_local_pub{0};
// Last published world per rigid dynamic context. Guest render thread only.
// Same-mesh piece clones (park-editor venues submit dozens) share (ib,vb),
// and their hook-time captures defer to the post-draw fixup whenever the
// venue batches dispatch (own_draw_last false); FIFO pairing there scrambles
// clone worlds whenever capture order and draw order diverge - pieces
// visibly swap placements and flip 180 degrees frame to frame. The fixup
// pairs each draw to the pending clone whose cached world matches instead,
// and clones whose draw never landed re-publish their cached world at merge.
struct RigidWorldCache {
  uint32_t mesh = 0;
  float world[16] = {};
  uint64_t frame = 0;
};
inline std::unordered_map<uint32_t, RigidWorldCache> g_rigid_world_cache;
// Meshes seen on the per-entity dynamic dispatch (kind-0 captures), stamped
// with the guest frame of the last sighting. The game batches FAR instances
// of such pieces through the static sort lists instead of dispatching them
// per entity; their fmt-57 vertices are MESH-LOCAL, so the world-item
// path's identity world collapses them at the origin (the far-corner
// "teardown": one sub-mesh of each distant piece went invisible). A
// sort-list record whose mesh is fresh here publishes with the ctx owner's
// instance matrix instead. The freshness window doubles as map-change
// hygiene: a mesh address reused by a later load expires out on its own.
// Guest render thread only.
inline std::unordered_map<uint32_t, uint64_t> g_dyn_dispatch_meshes;
// Model-space props in the world sort lists, excluded from the identity
// world path (their placed copies come from the kind-2 capture).
inline std::atomic<uint64_t> g_world_props{0};
inline std::atomic<uint64_t> g_rej_no_dynstate{0};
inline std::atomic<uint64_t> g_rej_dyn_range{0};
// Render-side silent-skip diagnostics: a scene item that decodes badly or
// loses its bone binding produces no (or wrong) pixels with no other trace.
inline std::atomic<uint64_t> g_rr_decode_fail{0};
inline std::atomic<uint64_t> g_rr_no_bones{0};
// Decodes pushed past the per-frame budget (item/texture appears a few
// frames late instead of hitching the render thread).
inline std::atomic<uint64_t> g_rr_mesh_deferred{0};
inline std::atomic<uint64_t> g_rr_tex_deferred{0};
inline std::atomic<uint64_t> g_rej_chain{0};
inline std::atomic<uint64_t> g_rej_geom{0};
inline std::atomic<uint64_t> g_rej_draws{0};
inline std::atomic<uint64_t> g_rej_bbox{0};

// ---- Perf telemetry (windowed; logged + reset with the interval stats) ----
// Written by the guest render thread (capture/build) and the command
// processor thread (render segments); read by the render thread's stats log.
// Atomics with relaxed ordering: telemetry, not synchronization.
using PerfClock = std::chrono::steady_clock;
inline uint64_t PerfNsSince(PerfClock::time_point t0) {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
          .count());
}
struct PerfWindow {
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::atomic<uint64_t> count{0};
  void Add(uint64_t v) {
    ns.fetch_add(v, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = max_ns.load(std::memory_order_relaxed);
    while (v > prev &&
           !max_ns.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
  }
  double AvgMs() const {
    const uint64_t c = count.load(std::memory_order_relaxed);
    return c != 0 ? double(ns.load(std::memory_order_relaxed)) / double(c) * 1e-6
                  : 0.0;
  }
  double MaxMs() const { return double(max_ns.load(std::memory_order_relaxed)) * 1e-6; }
  void Reset() {
    ns.store(0, std::memory_order_relaxed);
    max_ns.store(0, std::memory_order_relaxed);
    count.store(0, std::memory_order_relaxed);
  }
};
// Guest frame counter (guest render thread only; incremented at
// BuildFrameScene entry): paces the world-item cache's revalidation.
inline uint64_t g_guest_frame = 0;

// Guest render thread: per-frame sums (one Add per guest frame).
inline PerfWindow g_pw_capture;   // hook-time capture work (sort lists + RenderMesh)
inline PerfWindow g_pw_build;     // BuildFrameScene
inline PerfWindow g_pw_pal_tail;  // frame tail: palette snapshot-ring rotation
inline PerfWindow g_pw_guest_dt;  // guest frame interval (guest fps + spike max)
inline uint64_t g_capture_frame_ns = 0;  // guest render thread only; folded per frame
// Command processor thread: per-RenderScene-call segments.
inline PerfWindow g_pw_render;       // whole native render
inline PerfWindow g_pw_items;        // opaque + transparent item loop (incl. decodes)
inline PerfWindow g_pw_shadow;       // dynamic-shadow atlas pass
inline PerfWindow g_pw_mesh_decode;  // inline DecodeMesh calls (count = decodes)
inline PerfWindow g_pw_tex_decode;   // inline texture-ish decodes (cubes, HUD words)
inline PerfWindow g_pw_commit;       // PrewarmCommit batches (count = non-empty runs)
// RenderScene phase attribution (the sustained "render=13ms, items=0.7ms"
// Vulkan mystery): everything render= covers that items=/shadow=
// do not. pre = EnsurePipeline..prewarm+evict+retire before the scene pass;
// twod = the 2D/HUD overlay replay (incl. inline decodes + video planes);
// tail = MSAA resolve, outline, pfx chain, photo grab, popup blur, final
// transitions: RenderScene minus everything else.
inline PerfWindow g_pw_pre;
inline PerfWindow g_pw_2d;
inline PerfWindow g_pw_tail;
// Settle-pass attribution (the sticky ~80fps post-map-change state:
// pre=8.4ms == the whole warmup budget, GPU idle at
// P0): the takeover settle pass extends itself +8 frames whenever a frame's
// warm walk leaves work deferred, so content that keeps failing revalidation
// (old-map items over reused arenas, streamed payloads) can hold it open for
// the rest of the session, burning the full budget every frame. settle= times
// the block; wdec/wdef count warm-path decodes/deferrals; the offender logs
// name what churns.
inline PerfWindow g_pw_settle;
// Guest-thread build-phase attribution (the standing-still 320->250 fps
// oscillation): build= runs on the guest render thread and its
// variance IS guest frame-time variance. b2d/bspl/bpal split the phases;
// the item walk is the remainder. dt histogram + slow-frame log quantify
// and attribute the dips.
inline PerfWindow g_pw_b2d;   // Publish2dDraws
inline PerfWindow g_pw_bspl;  // PublishSplineDraws
inline PerfWindow g_pw_bpal;  // ServeAuthoritativePalettes
// This frame's phase costs (guest render thread only - no atomics needed).
inline uint64_t g_frame_b2d_ns = 0;
inline uint64_t g_frame_bspl_ns = 0;
inline uint64_t g_frame_bpal_ns = 0;
inline uint64_t g_frame_pal_tail_ns = 0;
inline std::atomic<uint32_t> g_dt_hist[5] = {};  // <3.6 / <4.5 / <6 / <10 / >=10 ms
inline std::atomic<uint32_t> g_slow_frame_log_budget{0};
inline std::atomic<uint64_t> g_warm_decodes{0};
inline std::atomic<uint64_t> g_warm_deferred{0};
inline std::atomic<uint32_t> g_warm_mesh_log_budget{0};
inline std::atomic<uint32_t> g_warm_tex_log_budget{0};
inline uint64_t g_takeover_frame = 0;
// Camera-update cadence (the "world judders while panning at 320 fps"
// question): if the guest updates its camera on a fixed sim tick while the
// render loop free-runs, view_proj repeats for several rendered frames and
// then steps, visible as the surroundings skipping to catch up. Counted per
// published scene on the guest render thread.
inline std::atomic<uint64_t> g_cam_changes{0};
inline std::atomic<uint64_t> g_cam_repeats{0};
inline std::atomic<uint64_t> g_cam_max_streak{0};

inline bool SceneEnabled() { return REXCVAR_GET(skate3_native_render_scene); }

// ---- Photo / movie / CAS-editor state shared with the render thread ----

// drew_inside captures whose bank provably belonged to another mesh's draw
// (now deferred to the post-draw fixup instead of consumed).
inline std::atomic<uint64_t> g_capture_foreign_bank{0};

inline std::atomic<uint64_t> g_item_cache_hits{0};
inline std::atomic<uint64_t> g_item_cache_builds{0};

// ---- Per-item pipeline profiling (skate3_native_render_scene_perf_items) --
// Deep attribution of the per-item CPU cost, logged as a perf-items line on
// the perf-log interval. Two questions: where the per-item time goes (mesh
// serve, texture serve, constants, submit on the render side; cache
// validate, fingerprint, full walk, fetch overrides on the build side), and
// whether it is spent on items the rendered frustum can actually see (in
// vs out draw windows, indices, retained-item share). All of it is opt-in:
// the extra clock reads cost real time at 2000+ items/frame.
// Render thread (command processor): per-draw_item totals split by the
// item's bbox-vs-frustum verdict, and per-stage sections of completed draws.
inline PerfWindow g_pw_di_in;      // draw_item total, item potentially visible
inline PerfWindow g_pw_di_occ;     // draw_item total, in-frustum but occlusion-grid-proven hidden
inline PerfWindow g_pw_di_out;     // draw_item total, bbox fully outside the rendered frustum
inline PerfWindow g_pw_di_mesh;    // mesh store lookup/heal (incl. rare inline decodes)
inline PerfWindow g_pw_di_tex;     // texture route/serve section
inline PerfWindow g_pw_di_const;   // constants assembly
inline PerfWindow g_pw_di_submit;  // buffer binds + DrawIndexed recording
inline std::atomic<uint64_t> g_vis_in_indices{0};   // indices submitted, visible items
inline std::atomic<uint64_t> g_vis_occ_indices{0};  // indices submitted, occluded items
inline std::atomic<uint64_t> g_vis_out_indices{0};  // indices submitted, out-of-frustum items
inline std::atomic<uint64_t> g_vis_out_retained{0};  // out-of-frustum draws from retention
// Session-cumulative count of statics skipped by the occlusion cull (the
// skate3_native_render_scene_occlusion_cull cvar; reported on the frame
// stats line like the other cumulative counters there).
inline std::atomic<uint64_t> g_occl_culled{0};
// Session-cumulative count of sort-list entries the guest-side dispatch
// filter removed before the guest's draw-list renderer ran (see
// skate3_native_render_scene_occlusion_cull_guest).
inline std::atomic<uint64_t> g_occl_guest_skipped{0};
// Session-cumulative count of item rebuilds the build-side occlusion skip
// left out (see skate3_native_render_scene_occlusion_cull_build).
inline std::atomic<uint64_t> g_occl_build_skipped{0};
// Guest render thread: decomposition of the per-item build walk.
inline PerfWindow g_pw_bi_core;    // BuildItemFromMeshCached total (hit validate + misses)
inline PerfWindow g_pw_bi_fp;      // ComputeItemFingerprint on the cache-hit path
inline PerfWindow g_pw_bi_walk;    // full BuildItemFromMesh rebuilds (cache miss)
inline PerfWindow g_pw_bi_fetch;   // AdoptDrawFetchOverrides per-frame guest reads
// Whole submit-record loop of BuildFrameScene (one Add per frame). Contains
// bi_core/bi_fetch plus everything they do not: per-record hash-set
// bookkeeping, the dynamic merge, draw-list copies and DrawItem churn.
// build - b2d - bspl - wloop - bpal - retain - remainder = the post-loop
// tail (camera smoothing, capture blocks, publish).
inline PerfWindow g_pw_bi_wloop;
inline std::atomic<uint64_t> g_bi_records{0};  // submit records per window
// Off-screen retention pass (always measured, one Add per frame: two clock
// reads): its cost and how many items it re-appends beyond the guest's own
// submission are core visibility-culling telemetry, not deep profiling.
inline PerfWindow g_pw_bi_retain;
inline std::atomic<uint64_t> g_retained_appended{0};  // re-appended items this window
inline std::atomic<uint64_t> g_retained_live{0};      // retained-map size after the sweep

// ---- Photo-editor postfx capture (FrameScene::PhotoFx / photo_fx.hlsl) ----
// While a photo flow is active (g_photo_flow_frame, armed by
// UpdatePhotoGrabWindow), the OnDrawDone hook snapshots each postfx pass's
// PS/VS constant rows (g_ps_bank/g_vs_bank, the game's staging banks) plus
// the device fetch-constant shadow (device+0x480) at DRAW time, the same
// reads the F11 per-draw records use, verified correct for every postfx
// pass in captures 1783957072 and 1783963636. Upload-time (SetPending)
// capture was removed: the game stages the next pass's rows while the
// previous pass's shader is still bound, so upload-time snapshots mix
// neighbouring passes' constants.
enum PfxPass {
  kPfxVisualFx = 0,
  kPfxDofDown,
  kPfxDofMB,
  kPfxDof,
  kPfxUber,
  kPfxFisheye,
  kPfxPassCount
};

inline std::atomic<bool> g_photo_flow_frame{false};

// Photo display-card sighting stamp (see the photo-grab helpers for the
// full story): stamped by Publish2dDraws on the guest thread whenever the
// card-signature quad is in the 2D stream; read by YieldForPhotoDisplay.
inline std::atomic<int64_t> g_photo_card_seen_ns{-1};

// Last ScreenshotBackEnd grab REQUEST (PerfClock ns): the game arms a grab
// 1-2 frames before it renders the shot + card-composite frame sequence and
// CPU-reads the resolves (GrabScreenshot -> OnScreenShot compose). The
// shutter burst (UpdatePhotoGrabWindow) keys on this: the ONLY moment the
// card-build's inputs can be made real under the native path.
inline std::atomic<int64_t> g_photo_grab_request_ns{-1};

// GrabScreenshot returned: the grab CPU-read the resolved shot, and the
// OnScreenShot card compose that follows (same guest thread, same Update)
// only reads memory the burst already copied. The burst closes here:
// event-driven, no timers (a short grace in the window logic covers the
// aux-target grab some flows issue on the following frame).
inline std::atomic<int64_t> g_photo_grab_done_ns{-1};

// Last PhotoReplayController::Update heartbeat, nanoseconds on PerfClock
// (steady). -1 until the first heartbeat. Written from the guest thread,
// read by RenderScene on the render thread.
inline std::atomic<int64_t> g_photo_replay_last_ns{-1};

// Last FrontEndState_Replay2::TakePhoto heartbeat: a photo was just taken
// (replay editor or photo mission). The frames that follow render and
// CPU-grab the 1152x640 screenshot target, so the forced-readback window
// (UpdatePhotoGrabWindow) stays armed for a few seconds after it. Same
// thread contract as the photo-replay heartbeat above.
inline std::atomic<int64_t> g_take_photo_last_ns{-1};

// Last rw::movie::MovieDecoder::Decode heartbeat (FMV playback: intro
// logos, any full-motion video). Same contract as the photo-replay
// heartbeat above.
inline std::atomic<int64_t> g_movie_decode_last_ns{-1};

// Last frame the 2D replay actually SUBSTITUTED a movie quad (ps_yuv2d).
// Consulted by YieldForMovie: while the native substitution is serving the
// video, the emulated yield must never engage; the emulated framebuffer
// is STALE under draw suppression, and presenting it flashed the previous
// video's last frame at every video boundary.
inline std::atomic<int64_t> g_movie_native_last_ns{-1};

// Last frame the 2D replay CONTAINED a video quad at all (substituted or
// not; detection runs regardless of movie_sub). Consulted by YieldForMovie:
// with no quad on screen there is nothing a yield could display - a
// skipped/force-completed video (intro skip) leaves the decoder heartbeat
// alive ~0.5 s with no quad anywhere, and yielding then just flashed the
// stale emulated buffer (the "momentary emulated switch" at intro skip).
inline std::atomic<int64_t> g_movie_quad_last_ns{-1};

// YUV plane fetch words of the FMV frames handed to
// VideoRenderer_RwTexture::Render (Y full res, U/V half res, CPU-filled via
// Texture::Lock). Multiple videos play at once (the camera-angle select
// page runs TWO preview movies), so this is a small table keyed by the Y
// plane's fetch-words hash; the 2D replay matches each captured draw's own
// slot-0 fetch against it; a video quad samples its Y plane there (the
// camera-page previews rendered as slow GREYSCALE through the plain 2D
// shader: exactly that luma). Movie threads publish under the mutex;
// RenderScene copies per frame.
struct MoviePlanes {
  // Y plane identity = the fetch constant's ADDRESS dword (words[0][1]).
  // NOT a full-words hash: the words staged into the device shadow at draw
  // time carry per-draw sampling flags that differ from the texture
  // object's stored constant (observed: both camera-page videos
  // published, exact-words match still 0 hits): the address dword is the
  // physical identity both sides agree on.
  uint32_t y_addr = 0;
  uint32_t words[3][6];  // Y, U, V fetch constants
  int64_t ns = -1;
};
constexpr int kMaxMovies = 4;
inline std::mutex g_movie_mutex;
inline MoviePlanes g_movies[kMaxMovies];

// Heartbeat of the create-a-skater editor's OWN pixel shaders: the editor's
// in-view skater draws use "_nis" compiles (cacstamp_skin_nisPS,
// cac_cloth_nisPS, cac_face_nisPS, cac_hair_nisPS, defaultcharacter_nisPS,
// cacstamp_shift_nisPS) that exist nowhere else in the game; a fresh
// sighting means the CAS editor is on screen NOW. Backs the FE-stack id-15
// detection in YieldForCasEditor: the startup new-game flow reaches the
// editor through a different FE screen (observed: the indicator stayed
// NATIVE there). Deliberately NOT matched: the "_unwrap" texture-space
// composite variants; outfit composition also runs mid-gameplay after an
// outfit change and must never yield a gameplay frame.
inline std::atomic<int64_t> g_cas_ps_last_ns{-1};

// Capture-side helpers the render thread calls (defined in
// skate3_native_scene.cpp).
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out);
void ClearItemCache();
bool CasEditorActive(uint8_t* base);
bool PortraitRttWindowActive();
// Parse a guest mesh struct into a DrawItem (prewarm decode workers build
// items off the queue without a live capture).
bool BuildItemFromMesh(uint8_t* base, uint32_t mesh, DrawItem& item);

// Render-side helper the capture thread calls (defined in
// skate3_native_scene_gpu.cpp): photo-editor detection shared by the yield
// and the photo-grab readback window: nullptr when inactive, else the name
// of the active signal.
const char* PhotoEditorSignal(uint8_t* base);

}  // namespace skate3::native_scene
