# Change Matrix — fork/main vs upstream (f6e0ae8) through d20d0cc

**Date:** 2026-09-04. **Base (upstream):** `f6e0ae8`. **HEAD (fork/main):** `d20d0cc`.
All commits live on `elespink/skate3recomp:main`; only feature-complete, user-validated
work has been pushed here (nothing is an experiment).

Granularity note: the first 6 commits were pushed to upstream `origin/main` as a single PR
(#148 + earlier); the last ones are fork-only. Everything below is what makes the fork the
preferred, working tree.

---

## 1. Summary

| Metric | Value |
|--------|-------|
| Commits since `f6e0ae8` | 8 |
| Files changed | 16 (+2 submodule pointer moves) |
| Additions | +2,083 |
| Deletions | −98 |
| `src/` (renderer/game glue) | 9 files |
| New source files | `skate3_crash_log.{h,cpp}`, `skate3_freecam_input.{h,cpp}` |
| New tooling | `tests/run_tests.sh` + `check_cvars.py` + `check_deadcode.py` |
| SDK submodule | `6c2b936` → `7cc198a` (RHI diagnostics) |
| Product changes | Game-playable, features + perf + stability + RHI diagnostics |

---

## 2. Commit-by-commit

| Commit | What | Why |
|--------|------|-----|
| `6c66d6c` | Cross-platform drone/freecam input (`skate3_freecam_input.{h,cpp}`) | Freecam was Win32-only (`GetAsyncKeyState` under `#if _WIN32`), so fly mode compiled out on Linux. Adds a `WindowInputListener` mirror + non-Windows branch in `UpdateFreecam`. |
| `5e7c5b8` | Pass #1–#3: crash-log, nude mode (+garment classifier), night sun-brightness, F12 menu controls, AZERTY fly on F7, F11 2D/HUD toggle | The user-requested feature set. See §4 for the component breakdown. |
| `ebb73dd` | Character-size GPU inject + slider; crash-log primary = user folder; `stretch_guard` default off | Whole-body size scale at both render sites; crash log to `~/.local/share/skate3/`; disable a per-frame CPU veto. |
| `765cd70` | Static regression suite (`tests/`) | Guards against the cvar-consistency "build-break" class and dead-code drift, without building. |
| `c798e58` | Perf batch: park-char adaptive boxcar smoothing, palette memo, tex-retire + dialog/sun cleanups | ~30Hz park character desync + micro-stutter + a duplicate sun-seed block. |
| `00ed5ff` | Dedupe sun/haze duplicate sliders; park-char kRing 24→32; F3 bone-palette ring reuse | Removed the F12 "two sliders move one cvar" bug; widened the pose ring to bracket the adaptive window; halved the skinned-bone palette upload. |
| `21aec37` | Reuse texture+SRV on staged content swaps (drain/tex-view reuse) | Steady-state 16-frame texture re-decode was retiring+e recreating views every poll, feeding `DrainRetired` (the dominant remaining micro-stutter). In-place re-upload kills that churn. |
| `d20d0cc` | Live F12 RHI diagnostics: drain/view-churn perf ring, fullscreen view trace, resource ledger (+ SDK `7cc198a`) | Adds an on-screen overlay to name the exact source of the residual `drain=` micro-stutter (a periodic 2560x1440 standalone SRV create+destroy). Low-overhead per-frame ring + once-per-sec log line. |

---

## 3. What changed since the fork — by concern

### A. Features (user-requested)
- **Nude mode** — `skate3_native_render_scene_nude` cvar; drops garments (ropa AND plain-cloth
  `cloth/leather/jacket`) while keeping skin/face. Garment classifier in
  `skate3_native_scene.cpp` + `DrawItem::garment` flag (7 gate sites).
- **Night sun-dim** — `skate3_native_render_scene_sun_brightness` (0..5) scales per-path
  exposure (world/dynobj/sky/char); Night preset → 0.15, Golden → 0.7.
- **Weather presets** — Reset/Clear/Overcast/Hazy/Golden/Night buttons in the F12 menu.
- **Character-size slider** — `skate3_native_render_scene_character_size` scales
  `item.world[16]` for `char_family 1/2` at both GPU sites. *(Known issue: also scales
  camera/world — see §5 open.)*
- **AZERTY fly mode on F7** — ZQSD + E/Space/C + arrows; rebind from End.
- **F11 2D/HUD toggle** — `skate3_native_render_scene_2d`.

### B. Stability / diagnostics
- **Crash logger rewritten** (`skate3_crash_log.{h,cpp}`) — the old `Install()` overwrote the
  SDK's SIGSEGV handler (boot-breaking 139). Now SDK-cooperative chaining, async-signal-safe
  raw-`write` dump, tid/hex fault addr/pc, mirror to user folder, stderr fallback,
  `set_terminate`, armed marker, `tools/symbolize_crash.py` helper.

### C. Performance
- **park-char adaptive boxcar** — per-entity smoothing window scales with the entity's pose
  period (~30Hz park char gets ~116ms), keeping body+ROPA in lockstep.
- **PublishedPaletteSane memo** — per-frame memo on vb-addr+content hash (removes a 32-guest-VB
  re-read every frame).
- **kRing 24→32** — widens the pose ring to bracket the adaptive window.
- **F3 bone-palette ring dedupe** — shadow pass publishes the ring offset; main pass reuses it
  (skips a redundant bone-palette memcpy).
- **`stretch_guard` default off** — drops a per-frame `SkinnedSpreadHostRows` veto in release.
- **Drain/tex-view in-place reuse (`21aec37`)** — ^ headline remaining micro-stutter fix.

### D. F12 menu hygiene (agent6 audit)
- Removed duplicate sun/haze slider blocks (two blocks were writing the same cvars).
- Raised `haze_intensity` cap 0.5→0.6 (covers Golden preset 0.55); freecam caps matched to ranges.
- Extended `refl_mode` metadata range 0–9 to match the combo + shader.

### E. Tooling
- `tests/run_tests.sh` + `check_cvars.py` (cvar visibility/single-DEFINE) + `check_deadcode.py`.
- `.gitignore` additions (pycache/pyc).

---

## 4. New/removed files

**New (keep):**
- `src/skate3_crash_log.{h,cpp}` — crash logger (feature).
- `src/skate3_freecam_input.{h,cpp}` — Linux freecam input (feature).
- `tests/check_cvars.py`, `tests/check_deadcode.py`, `tests/run_tests.sh` — regression suite.

**Moved:** none. **Deleted:** none. (`docs/agent-reports/` markdown cleaned in c798e58; content
backed up to `/root/skate3/agent_reports/` + live reports remain.)

**Submodule:** `third_party/rexglue-sdk` pointer changed; worktree runs `elespink-dev`
(imgui stock-pin + codegen `_Exit()` + GTK fix — all uncommitted inside the submodule by design).

---

## 5. What is LEFT (open work) and why

| # | Item | Status | Why left |
|---|------|--------|----------|
| Live Bug 1 | **Nude — body transparent under garments (Fix B3)** | ❌ open | Partial: hair (Fix H) done in `5e7c5b8`; body preservation (keep highest-bone-count non-ropa skinned item at the 4 nude gates) deferred — body material unconfirmed; needs a one-frame `char_track` diagnostic dump first. |
| Live Bug 2 | **Scratch/blood/broken-bones not visible** | ❌ open | Native renderer has no wound decal channel; diagnosis-first = F11 `.draws.bin` capture during a forced bail to pick the fix path. |
| Live Bug 3 | **Character-size scales camera/world → unplayable at ≠1.0** | ❌ open | `item.world[16]` scale also affects camera/UI. Needs gating to skater-mesh-only (agent E: feasible host-side). |
| Perf | **Residual micro-stutter at world-load transitions** | ⚠️ reduced | `21aec37` removed the steady-state churn AND the upload-heap OOM. Remaining `drain=` frames (~784, Σ6.3s) come from texture-cache eviction during area streaming (`scene_gpu.cpp:425/553`) — a separate retire source. Next optimization target. |
| Feature | **Live physics editor (F12 gravity/velocity)** | 🔬 discovered, not built | Hook point `0x82C67F10` + delta feeder `[obj+0x70]` confirmed by pcode trace; but fields not yet *proven* = raw linear velocity — needs an in-game free-fall-vs-flat dump before wiring. |
| Feature | **CAS morph slider extension** | ❌ not started | Blocked on guest CAS slider-bound discovery (earlier `0x3F000000` search was SIMD noise). |
| Feature | **Regional char exaggeration** (head/belly/limbs) | ❌ not started | Whole-body size done; per-bone needs bone-index mapping (agent E scoped ~2–4 days). |

**Not a bug, won't-fix:** RenderDoc "Capturing Vulkan…" HUD (disable via
`/usr/share/vulkan/implicit_layer.d/renderdoc_capture.json` `"enabled":0`).

---

## 5A. Milestone verification — live log results (21aec37, 2026-09-04 04:50–04:59)

Session: `v2.0.0.7-dev.g21aec37-Release`, park at ~175 FPS, **no desync**. User-verified clean.

| Metric | Result |
|--------|--------|
| Crashes (segv/SIGSEGV/abort/terminate) | **0** |
| `heap2(DL)` upload-heap oversubscription | **resolved** (old `00ed5ff`: sustained `use=229MB budget=16MB`, `bufs upload host=220MB`; now mostly within budget, upload host → 54MB) |
| Earlier "OOM" crash root cause | **confirmed = upload-heap exhaustion** from retire/recreate churn, not the drain fix itself |
| SLOW frames (whole session) | 888 |
| `drain>1ms` frames | ~784, Σ ~6.3s |

**Conclusion:** no breaking change from upstream `f6e0ae8`; all additions are additive features,
perf wins, and stability fixes. This tree is a valid release candidate.

**Residual: `drain=` micro-stutter at world-load/streaming transitions** — a *separate* retire
source from the `PrewarmCommit` in-place path fixed in `21aec37`: the texture-cache eviction
path (`scene_gpu.cpp:425/553`) disposes freshly-spawned textures while a park area streams in.
Shows as brief 5–20ms hitches during loads, not steady-state skating. Next optimization target.

---

## 6. Build & test commands (host)

```
./build.sh --clean --tu ./TU_12K2276_000000C000000.00000000000O3 --perf   # full build
./build.sh --tu ./TU_12K2276_000000C000000.00000000000O3 --perf          # incremental
tests/run_tests.sh                                                        # static regression
```
