# Change Matrix — Skate 3 Native Recomp

**Last updated:** 2026-09-05. Committed: new PERF-menu/SDK-key-conflict commit (below). Uncommitted: none (SDK submodule dirty by design).
**Base:** upstream `f6e0ae8` / v2.0.2. HEAD = PERF-menu key/menu fix (F3 perf, F4 2D).
**Note:** I do NOT build in the VM — the build tree is host-keyed to `lespink`. Build fixes are log-only; user verifies by compiling on the host (clang++-21).

---

## IMPLEMENTED (committed in `61c7875`→latest PERF-menu commit, user building)

| # | Feature | Files | Risk | Status |
|---|---------|-------|------|--------|
| 1 | **Drain fix — output SRV cache** | `scene_gpu.cpp`, `_gpu_internal.h`, `_scene_post.cpp` | Low | Reuses VkImageView across 3-image mailbox rotation instead of recreate every frame. Root cause of recurring 4-5ms drain stalls. |
| 2 | **Character-size bone-palette fix** | `scene_gpu.cpp` | Medium | Scales bone-palette translation around character root instead of world matrix. Fixes mesh flying off + camera/perspective breakage. Removed dead world-shadow code. |
| 3 | **Deadcode tool fix** | `tests/check_deadcode.py` | None | Eliminated ~200 false-positive "unreferenced" entries from intra-TU static calls and function pointers. |
| 4 | **Perf menu refactor (v2)** | `debug_dialog.cpp` | Low | MangoHUD-style consolidated "Performance" dashboard: big colored FPS, frame-time graph w/ 1%/0.1% lows, GPU resource bars, RHI drain/view graphs, fullscreen view trace, scene stats, pacing controls. Moved RHI diag out of Diagnostics, fps-cap out of Pacing. Sun/haze dedup verified clean. |
| 5 | **Perf window (v3) — standalone** | `debug_dialog.cpp` | Low | Perf moved OUT of the F12 debug window into a dedicated standalone "Performance" window (opened via "Open Performance window" button in F12). Consolidated: pacing (smooth camera + filter + fps cap + opaque sort), perf-log + perf-items + capture-hotkeys all into Performance. Diagnostics keeps only visual views (SSAO debug, 2D sharp). Caches untouched. |
| 6 | **Rigid-piece char-size pivot fix** | `scene.h`, `scene.cpp`, `scene_gpu.cpp` | Medium | Rigid (non-skinned) char_family 1/2 accessories now scale their world translation about the character's root bone (bone 0 of a same-entity skinned sibling), so hats/jewellery/rigid ropa track the scaled body. Added `DrawItem::root_pos[3]` + `root_valid`. Gated on `cs != 1.0`; falls back to pre-fix when no trusted root. Inert at cs=1. |
| 7 | **PERF = F3 key, 2D toggle = F4** | `app_common.{h,cpp}`, `debug_dialog.{h,cpp}` | Low | Performance dashboard is its own **PERF menu opened/closed by F3** (new `PerformanceDialog`, draws independently of F12). Removed the "Open Performance window" button + perf draw call from the F12 debug window entirely — perf is fully decoupled from F12. Restored a plain function key for the 2D/APT HUD toggle: **F4** (was Ctrl+F10 since `58f8618`). `bind_skate3_perf_menu` registered/unregistered in `OnCreateDialogs`/`OnShutdown`. |
| 8 | **SDK hotkey collision fix (F3/F4 actually work)** | `app_common.cpp` | Low | Root cause of "Perf removed / F4 not hiding replay HUD": the rexglue SDK ships **F2 = FPS counter, F3 = debug overlay, F4 = settings overlay** and `ProcessKeyEvent` hands a key to the FIRST registered bind — since the SDK registers before `OnCreateDialogs`, the app's F2/F4 binds never fired (F2 opened the SDK FPS counter, F4 opened/ignored the SDK settings overlay). Fix: at `OnCreateDialogs` start, `SetFlagByName("bind_debug_overlay","Ctrl+F3")` + `SetFlagByName("bind_settings","Ctrl+F4")`, freeing plain F3/F4 for the skate3 PERF menu and 2D-HUD toggle. F2 keeps the SDK FPS counter. |

## REVERTED (do not re-add as-is)

| Feature | Why reverted | Next step |
|---------|--------------|-----------|
| **Physics editor** (`skate3_physics_editor.cpp`) | `sub_82c67f10` is NOT a registered recomp function entry — the address is mid-function code inside a larger aggregated body. `REX_FUNC` can only hook registered function entries. The GHIDRA `.pdata` boundary (Function_82C57F10) is real, but rexglue merges it into a fat function. | Find the ACTUAL recomp entry wrapping this code, or hook a different address. Verify against `PPCFuncMappings[]`. |
| **Perf menu overhaul (v1)** | Was accidentally bundled with the physics editor in the reverted `debug_dialog.cpp`. | Being re-implemented cleanly by the perf/debug sub-agent. |

## PREVIOUSLY COMMITTED (since upstream f6e0ae8)

| Commit | Description |
|--------|-------------|
| `6c66d6c` | Cross-platform drone/freecam input for Linux |
| `5e7c5b8` | Crash-log, nude mode (Fix H hair), night brightness, F12 controls, AZERTY fly F7, F11 2D toggle |
| `ebb73dd` | Character-size slider, crash-log user-primary, stretch_guard default OFF |
| `765cd70` | Static cvar-consistency + deadcode test suite |
| `c798e58` | Park-char adaptive smoothing, palette memo, tex-retire + dialog/sun cleanups |
| `00ed5ff` | Dedupe sun/haze sliders, park-char kRing 24→32, F3 bone-ring reuse |
| `21aec37` | Reuse texture+SRV on staged content swaps (PrewarmCommit churn fix) |
| `f7cf9d5` | Change matrix + milestone verification docs |
| `d20d0cc` | Live F12 RHI diagnostics — drain/view-churn perf ring, fullscreen view trace, resource ledger |
| `e81ad7a` | Docs: change matrix through d20d0cc |
| `479af5f` | feat: drain/output-SRV cache, character-size root-pivot scaling, F12 perf v2, build + deadcode fixes |
| `58f8618` | fix: move 2D toggle to Ctrl+F10, freeing F11 for draw capture |
| `ff389a4` | fix: constant-bank overread cap, dead submission param, F3 shadow-bone cs validation |
| `61c7875` | docs: change matrix through ff389a4 |
| `1f5bab8` | feat: standalone Performance window, rigid-piece char-size pivot |
| `8a8d82c` | feat: PERF menu on F2, perf decoupled from F12, 2D HUD toggle on F4 |
| `ce834e3` | docs: change matrix through 8a8d82c (PERF=F2, 2D=F4) |

---

## CONTINUE-LATER NOTE (session end 2026-09-04)

| Item | Feasibility | Next step |
|------|-------------|-----------|
| **Fix B3 (nude body preservation)** | FEASIBLE (research confirmed). `item.bones.size()/12` degenerate (always 84); real bone count at `mesh+0x48`. Per-entity grouping via `LookupCtx`. | One-frame `char_track` diagnostic dump to confirm body material, then implement per-entity max-real-bone prune in `BuildFrameScene`. |
| **Scratch/blood body damage** | **FEASIBLE** — Path A recommended (agent C confirmed). Add wound slot to `DrawItem`, relax `AdoptDrawFetchOverrides` gate for `char_family==2`, add `overlay.w==5` wound branch in `scene_char.hlsli`. Three code paths suppress wounds but all are addressable. | F11 `.draws.bin` capture in emulated mode to confirm Path A (mesh decal) vs Path B (standalone overlay). Once confirmed, implement wound slot + shader branch (~medium effort). |
| **Physics editor** | BLOCKED. `sub_82c67f10` is NOT a registered recomp function entry (mid-function code in fat aggregated body). | Find actual recomp entry via `PPCFuncMappings[]` or hook a different address. Research needed. |

## TEST SUITE STATUS

| Check | Status | Notes |
|-------|--------|-------|
| `check_cvars.py` | PASS | Cvar visibility/duplication/undefined — clean |
| `check_deadcode.py` | PASS | FIXED crash: `.count()` on a `set` tokenizer (deduped) at line 170. Added `token_counts_in()` occurrence counter. Now clean EXIT=0. |
| `run_tests.sh` | OK | No bug — it is a bash script; it was mis-invoked via `python3`. Run with `bash tests/run_tests.sh`. |

## FILES CHANGED

```
 src/skate3_app_common.cpp              |   1 +-     (Ctrl+F10 key binding) [committed 58f8618]
 src/skate3_native_debug_dialog.cpp     | 519 ++-     (perf v2 + v3 standalone window + build-fix)
 src/skate3_native_scene.cpp            |  72 +-     (bank overread cap + root_pos population)
 src/skate3_native_scene.h              |  12 +      (DrawItem::root_pos + root_valid)
 src/skate3_native_scene_gpu.cpp        | 190 ++-     (drain + char-size + dead param + F3 cs + rigid pivot)
 src/skate3_native_scene_gpu_internal.h |  23 +      (OutputViewEntry for drain cache)
 src/skate3_native_scene_post.cpp       |  15 +-     (EnsureMenuBlurStandalone cache)
 src/skate3_native_scene_state.h        |   1 +      (#include <fstream> build fix)
 tests/check_deadcode.py                |  35 ++-     (false-positive elim + crash fix)
 src/skate3_app_common.{h,cpp}          |  23 ++-     (F2 PERF-menu bind + TogglePerformanceWindow + F4 2D toggle)
 src/skate3_native_debug_dialog.{h,cpp} |  75 ++-     (PerformanceDialog, F12 perf hooks removed)
 src/skate3_app_common.cpp              |  12 ++-     (SDK hotkey remap + perf F2→F3)
```

Committed: through PERF-menu commits (`8a8d82c` + SDK-key fix, all source changes). Pending: none in-tree (SDK submodule dirty by design). Awaiting user verification of the F3 (PERF) / F4 (2D-HUD) / Ctrl+F3 (SDK debug) / Ctrl+F4 (SDK settings) key map on host.
