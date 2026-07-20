# UEVR-3D — "Flat 3D Monitor" Output Mode (FLAT3D runtime)

## Context

UEVR-3D (`~/Documents/claude/UEVR-3D`, clean fork of praydog's UEVR master, Windows, C++/D3D11/D3D12) injects VR stereo rendering into Unreal Engine games and submits to OpenXR/OpenVR. Goal: a **third mode alongside those two** that takes over the injected game's presentation and outputs stereo-3D **to the monitor** — no headset, no SteamVR/OpenXR install — with VRto3D-style output formats and off-axis 3D math. Must stay rebase-friendly on upstream UEVR: all logic mass in new files, upstream edits small and `is_flat3d()`-gated.

**User decisions (fixed):** Windows only. Output modes v1 = core set (SbS, TaB, row/col-interlaced, checkerboard, anaglyph variants) **+ LeiaSR weaver**; **no Mono**; WibbleWobble handoff / frame-packed / dual-display deferred. Off-axis frustum with depth+convergence like VRto3D. **FoV stays game-controlled.** Depth-dynamic crosshair + auto 3D scaling by world scale & FoV (perfect_dark_3D techniques). GUI depth+size = **single depth slider with auto-scale so the UI always fills the screen** (not fixed 1.0). Auto-convergence with VRto3D auto-depth-style settings. Weapon convergence: **auto-convergence only** in v1 (no bracketable gun pass in UE; per-primitive & depth-reprojection approaches noted as future research). **HDR supported in v1** (PQ/scRGB swapchains). **OpenTrack head tracking = v2.**

**Key verified facts (don't re-derive):**
- `is_hmd_active()` == `runtime->ready()` == `runtime->loaded` (VR.hpp:265-277) → a stub runtime with `loaded=true` engages the ENTIRE stereo pipeline. Pose getters (VR.cpp:2797-2973) return zero/identity for unknown runtime types; `get_eye_offset`/`get_projection_matrix` (VR.cpp:3003/3063) read generic `runtime->eyes[]`/`projections[]`.
- The stereo pair already exists each frame as ONE double-wide texture: `m_fake_stereo_hook->get_render_target_manager()->get_render_target()` (D3D11Component.cpp:240). Per-eye = half width, B8G8R8A8_UNORM(_SRGB).
- Submission lives in the D3D components (not the runtime): D3D11Component::on_frame (D3D11Component.cpp:209; OpenXR end_frame :719, OpenVR Submit :592/:760/:824), D3D12Component::on_frame (D3D12Component.cpp:27; :677/:483/:586/:616). Desktop-fix block (D3D11Component.cpp:847-957) is the backbuffer-composite template. on_post_present clears the real backbuffer when HMD active (D3D11Component.cpp:962-998) — must be gated. on_frame force-disables vsync (`set_next_present_interval(0)` :222) — flat3d must NOT (interlaced needs vsync).
- Projection: `calculate_stereo_projection_matrix` (FFakeStereoRenderingHook.cpp:4929) calls the original (game matrix) then OVERWRITES with headset projection at :5056-5061 — flat3d instead keeps the game matrix and mutates only `[2][0]`. UE convention (OpenVR.cpp:259-264): `[0][0]=P00`, `[2][0]`=h-shear, `[2][3]=1`, `[3][2]=nearz`, reversed-Z infinite far. UE5 double-precision branch exists (`m_has_double_precision`, :5058).
- View: `calculate_stereo_view_offset` (FFakeStereoRenderingHook.cpp:4617); eye sep = `eye_offset × world_to_meters × m_world_scale` (:4779, :4802, :4813). Existing `is_2d_screen` gate (:4795,:4816-4841) skips head translation + rotation overwrite but keeps eye separation — exactly the no-HMD neutralization needed.
- Config: ModValue pattern (Mod.hpp:19-284): declare in VR.hpp (~:947-952) → add to `m_options` (VR.hpp:1029-1099) → auto-persisted (per-game config dirs) → draw in sidebar page (VR.cpp:2284/2348-2376). Sidebar entries VR.hpp:110-121.
- VRto3D reference math: `eyeOffset = ±(depth·0.5)/convergence` on horizontal frustum tangents (hmd_device_driver.cpp:1889-1919); depth default 0.1 m, convergence default 1.0 m (floor 0.001); repack shader `vrto3d/shaders/repack.frag:127-333` + HLSL mirror `kPsSource` in `presenter/window_presenter.cpp` (port source); auto-depth control loop `FeedAutoDepthSample` (hmd_device_driver.cpp:2069-2118): asymmetric EMA (up .20/down .05, deadband .005), target_disparity .005 (clamp .001-.01), smoothing .08 (clamp .005-.25), manual value = ceiling, snap-back on disable. LeiaSR: `vrto3d/src/presenter/leiasr_presenter.cpp` — SEH-guarded soft `SRContext::create` (:52-66,:501), `CreateDX11Weaver` (:527), `setShaderSRGBConversion(true,true)` (:537), `setLatencyInFrames(1)`, SbS in → weaver → backbuffer.
- perfect_dark_3D: clip-space skew `mf[3][0] += d·P00; mf[2][0] += d·P00/conv`; FoV-adaptive sep `ipd *= tan(fov/2)/tan(refFov/2)`; crosshair depth from engine aim solution, per-eye shift `∝ ipd·(1/z − 1/conv)/tan(hFov/2)`; convergence auto-clamp `[near·1.5, far·0.9]`.
- Depth access for crosshair/auto-conv: `RenderTargetPoolHook::get_texture(L"SceneDepthZ")` (RenderTargetPoolHook.hpp:36).
- Pre-existing upstream bug (do NOT entangle; fix separately): inverted float/double branch in the no-hook fallback at FFakeStereoRenderingHook.cpp:4999-5003.
- Build: cmake.toml globs `src/**` (:232) — new files auto-registered; regenerate CMakeLists.txt via cmkr, never hand-edit.

## 1. New runtime: `Type::FLAT3D` + `runtimes::Flat3D`

- **Edit** `src/mods/vr/runtimes/VRRuntime.hpp`: add `FLAT3D` to Type (:22-26) + `is_flat3d()` helper (:117-123). ~5 lines.
- **New** `src/mods/vr/runtimes/Flat3D.{hpp,cpp}`: `type()`→FLAT3D; `ready()` inherited (`loaded`); `synchronize_frame` sets `frame_synced` (non-blocking); `update_poses` sets `got_first_poses/got_first_valid_poses`; `update_render_target_size`/`get_width/height` = real backbuffer size (per-eye render = native res); `update_matrices` fills `eyes[]` = identity rot + translation `{∓sep_m/2,0,0}` (view path then applies world-scale for free); `projections[]` filled by the hook via `set_game_projection(eye, final_matrix, tan_half_h, tan_half_v, nearz)` (stores under eyes_mtx + captures FoV atomics). Live atomics: `separation_m` (effective, post auto-scale), `convergence_m` (post auto-conv), `game_tan_half_h/v`, `center_depth_uu`.
- **Edit** `src/mods/VR.hpp`: `m_flat3d` shared_ptr next to :730-732; `is_using_flat3d()`; `get_flat3d_runtime()`. (AFR stays available — see §2 eye cache.)
- **Edit** `src/mods/VR.cpp::clean_initialize` (:34-56): if `m_flat3d_enabled` or requested runtime == "flat3d" → `initialize_flat3d()` instead of the OpenVR/OpenXR chain (~8 lines).
- **New** `src/mods/VR_Flat3D.cpp`: `VR::initialize_flat3d()` (set openvr/openxr error strings, `m_flat3d->loaded=true`, `m_runtime=m_flat3d`, `update_render_target_size`, `m_d3d11/m_d3d12.on_reset` like initialize_openvr :132-136), the sidebar page body, keybind handler. No VR DLLs loaded/required. Enable toggle = restart-required (hot-swap out of scope v1). `m_2d_screen_mode` mutually exclusive (flat3d branch takes priority).

## 2. Composite: third branch in D3D11/D3D12 → real backbuffer

- **New** `src/mods/vr/flat3d/Flat3DShaders.hpp`: `enum class Flat3DOutputMode { SBS, TAB, ROW_INTERLACED, COL_INTERLACED, CHECKERBOARD, ANAGLYPH_RC, ANAGLYPH_RC_DUBOIS, ANAGLYPH_RC_DEGHOSTED, ANAGLYPH_RC_COMPROMISE, ANAGLYPH_GM, ANAGLYPH_GM_DUBOIS, ANAGLYPH_GM_DEGHOSTED, ANAGLYPH_BLUE_AMBER, LEIA_SR, COUNT }` (**no MONO** per user); embedded HLSL (fullscreen-triangle VS + repack PS ported 1:1 from repack.frag/kPsSource; cbuffer: out_size, eye_size, mode, eye_swap, row/col parity); `Flat3DFrameParams { mode, eye_swap, ui_shift_px, ui_scale, crosshair_shift_px, ... }` computed CPU-side once per frame. Runtime `D3DCompile` (precedent: window_presenter.cpp:327-358; d3dcompiler already linked).
- **New** `src/mods/vr/flat3d/Flat3DCompositorD3D11.{hpp,cpp}`: `setup(dev, eye_w, eye_h, fmt)` / `composite(ctx, double_wide_srv, ui_srv, backbuffer_rtv, params)` / `reset()`. Per frame (inside DX11StateBackup, mirroring :851): (1) CopyResource double-wide → own `m_composite_tex` (never draw into engine RT); (2) SpriteBatch: UI texture drawn twice (per eye half) at ±ui_shift_px (§5); (3) SpriteBatch: crosshair per eye at ∓crosshair_shift_px (§4); (4) repack PS → real backbuffer. **LeiaSR mode**: instead of repack PS, hand `m_composite_tex` (SbS) to the SR weaver — port leiasr_presenter.cpp's soft-load pattern (SEH SRContext::create, CreateDX11Weaver, setShaderSRGBConversion, latency 1); SR SDK headers under a compile-time option + graceful runtime absence (mode hidden if SRService/DLLs missing).
- **New** `src/mods/vr/flat3d/D3D11Component_Flat3D.cpp`: member `D3D11Component::on_frame_flat3d(VR*)` in its own TU. Fetch double-wide like :239-244; non-SRGB SRV (:275); ui_target like :345-349; ensure `m_backbuffer_rtv` (factor creation from on_post_present :984-992 into a helper); do NOT set present interval 0; call compositor; no VR submit; parallel first-frame window-activation path (don't set m_submitted).
- **Edit** `src/mods/vr/D3D11Component.{hpp,cpp}`: `on_frame` branch `if (runtime->is_flat3d()) return on_frame_flat3d(vr);` after :257; on_post_present clear gated `&& !is_flat3d()` (:996); on_reset += compositor reset. ~8 lines.
- **New** `src/mods/vr/flat3d/Flat3DCompositorD3D12.{hpp,cpp}` + `D3D12Component_Flat3D.cpp`: same architecture — root sig (CBV+2 SRV+sampler), PSO keyed on backbuffer format, `d3d12::TextureContext` composite (pattern: m_backbuffer_copy, D3D12Component.hpp:63, :98-147), DX12 SpriteBatch (already used :73). Branch at on_frame :27 + clear_backbuffer gate. LeiaSR on D3D12: use SR SDK DX12 weaver if available, else LeiaSR mode is D3D11-only in v1 (documented).
- Shared across APIs: shader source, mode enum, frame params, all parallax math, auto-conv control loop. Per-API: resource plumbing only.
- **AFR supported (user requirement):** the compositor keeps two persistent per-eye textures; on a full-stereo frame both are refreshed from the double-wide, on an AFR frame only the rendered eye's half is copied (frame-role flags already exist: `is_left/right_eye_frame`, D3D11Component.cpp:262-265). The composite always weaves the freshest cached pair — one eye is one frame older, same as HMD AFR. No `is_using_afr()` early-out.
- **Native-stereo-fix supported (user requirement):** honor the same eye-layout selection the desktop-fix path uses (it samples the LEFT half under native-stereo, :892) — the per-eye copy step keys off `m_native_stereo_fix` exactly like the existing submit paths so the correct halves land in the eye cache.
- **Upscale to display resolution BEFORE pattern (user requirement, interlaced + LeiaSR):** the repack PS runs at backbuffer resolution: it first bilinear-samples each cached eye texture to the output pixel grid, *then* applies the row/col/checker selection in output-pixel coordinates — so the line pattern is always display-pixel-exact regardless of the eye/render resolution. Same for LeiaSR: the SbS handed to the weaver is built at backbuffer res. Remaining constraint (undetectable from inside the shader): the backbuffer itself must be display-native (borderless fullscreen, no DXGI/DPI scaling after us) — detect backbuffer-vs-display mismatch via `GetContainingOutput` desc and surface a warning for interlaced/SR modes.
- **HDR support (first-class, user requirement):** detect the swapchain color space at setup (backbuffer format + `IDXGISwapChain3::GetColorSpace1` / the format's implied space: `R10G10B10A2_UNORM`+`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P709` = HDR10/PQ; `R16G16B16A16_FLOAT` = scRGB linear). PSO/RTV keyed on the actual backbuffer format (already planned). Repack behavior by mode class:
  - **Resampling modes (SbS/TaB/interlaced/checkerboard) and LeiaSR input prep:** pure UV remap — pass HDR pixels through untouched; works natively in any format/color space.
  - **Anaglyph modes:** color-matrix math is defined for display-referred SDR primaries. Add `colorspace` to the repack cbuffer: decode PQ→linear (ST-2084 EOTF) or take scRGB as-is, normalize by a `paper-white nits` setting (ModSlider, def 200, HDR pages only), apply the Dubois/deghost matrix in normalized linear, re-encode to the swapchain space. Documented as approximate under HDR (glasses filters are calibrated for SDR); accurate SDR path unchanged.
  - **UI/crosshair SpriteBatch draws:** blend in the swapchain's space; UI texture is SDR sRGB → decode + scale by paper-white when compositing onto HDR (single multiply in the sprite shader or a pre-scaled tint).
  - **LeiaSR weaver:** SR SDK expects SDR sRGB input — when the game swapchain is HDR, tone-map the composite to SDR for the weaver (simple Reinhard/clamp with paper-white) or surface "LeiaSR: SDR swapchain required" — verify SDK capability at M3 and pick then.

## 3. Projection & view (game FoV preserved by construction)

- **Edit** `FFakeStereoRenderingHook::calculate_stereo_projection_matrix` (:4929): flat3d block after :4996-5004, before :5006, early-return (~22 lines):
  - float path: `p00=(*out)[0][0]; (*out)[2][0] += dir·(sep·0.5/conv)·p00;` (dir: left +1, right −1; verify sign live in M2, eye_swap as backstop); `set_game_projection(...)`; also set `VR::m_nearz` like :5048.
  - double path (`m_has_double_precision`): same on `Matrix4x4d`.
  - Nothing else touched → game FoV/aspect/near flow through untouched (zoom/ADS tracks automatically).
- **Edit** `calculate_stereo_view_offset` :4795 (1 line): `is_2d_screen = vr->is_using_2d_screen() || vr->is_using_flat3d();` → head translation + rotation-overwrite skipped; eye separation still applied via our `eyes[]` along the game camera right vector, world-scale-correct via :4779.
- Clamp effective convergence per frame to `[nearz·1.5/w2m, ∞)` (perfect_dark auto-clamp) before use.

## 4. Depth-dynamic crosshair — two modes, shared depth source

- Depth source (shared): `RenderTargetPoolHook::get_texture(L"SceneDepthZ")` (enable hook when crosshair or auto-conv on). Per frame copy 8×8 patch at left-eye center → 3-deep staging ring → Map entry from 2 frames ago (no stall). Reversed-Z: `z_view_uu = nearz / depth_sample`. Median of patch + EMA (α≈0.25, ~1% deadband) → `center_depth_uu`. Fallback when missing: static-depth slider (far-plane shift if invalid).
- Per-eye shift (shared, CPU-side): `shift_px = dir·(sep_uu·P00/2)·(1/conv_uu − 1/z_uu)·(eye_w/2)`; zero at convergence.
- **Mode A — "Game crosshair" (grab the game's own reticle):** feasible because the game's crosshair lives in the redirected UI layer (`ui_target`). During the per-eye UI composite, a small center region (radius setting, default ~4% of eye width) is *excluded* from the flat gui-depth pass (alpha mask in the UI sprite shader) and drawn again from the same `ui_target` region at the dynamic aim-depth shift. The actual game reticle — whatever it looks like — sits on the aimed geometry. Caveats (documented, tunable): center-anchored non-crosshair HUD elements inside the radius ride along; games whose crosshair isn't screen-centered or is scene-drawn need Mode B.
- **Mode B — "Laser sight" (classic 3D Vision style):** our own embedded 32×32 dot/reticle texture, SpriteBatch per eye half at the dynamic shift, tint/size settings — depth-aware overlay independent of the game's HUD, exactly like the 3DVision laser sight.
- Crosshair setting = combo {Off, Game crosshair, Laser sight} + adaptive toggle (live depth vs static-depth slider).

## 5. GUI depth — single slider (size auto-scales so UI always fills the screen)

- UEVR already redirects Slate/UMG into `ui_target` (`m_engine_ui_ref`, D3D11Component.cpp:345-349). Composite it per-eye at `ui_shift_px = dir·(sep_uu·P00/2)·(1/conv_uu − 1/(D·w2m))·(eye_w/2)`, D = `m_flat3d_gui_depth` (m).
- **Auto-scale, not fixed 1.0 (user requirement):** a parallax-shifted full-frame UI at scale 1.0 would clip one edge and leave a bare gap on the other. Scale each eye's UI copy about its half-center by `ui_scale = (eye_w + 2·|ui_shift_px|)/eye_w` (mild zoom, grows with depth) so the UI fully covers the eye view after the shift. The single slider is "depth"; size follows automatically. Cost: at most |shift| px of over-scan crop at the outer edges at extreme depths. Both shift and scale computed CPU-side into `Flat3DFrameParams`.
- OverlayComponent VR-quad paths inert in flat3d (early-out if needed). If `ui_target == nullptr` (UI baked into scene), slider is a no-op → surface hint in the settings page.

## 6. Auto-convergence (VRto3D auto-depth analog, depth-buffer based)

- **New** `src/mods/vr/flat3d/Flat3DAutoConvergence.{hpp,cpp}` (CPU control loop, API-agnostic) + a small histogram CS in each compositor: quarter-res over SceneDepthZ left-eye half, ROI 84%×90%, 128-bucket histogram of 1/z, absolute count floor rejects specks; non-blocking double-buffered readback. (Real depth ⇒ no SAD matcher, no gradient/luminance gates needed.)
- Control law (mirrors FeedAutoDepthSample): `conv_target = 1/(1/z_near + 2·target_disparity/(sep_uu·P00))` (converge just behind nearest object, bounded pop-out); asymmetric EMA on z_near (up .20 / down .05, deadband .005); output lerp `smoothing` (.005–.25, def .08); target_disparity .001–.01 def .005; **manual convergence = ceiling; snap back on disable**; optional 1 Hz log line.
- **Joint depth+convergence adjustment (user: "may need, not sure yet"):** structure the loop's output as `{convergence, depth_scale}` with `depth_scale = 1.0` in the default mode. The hook: when the disparity budget can't be met by convergence alone (conv_target would fall below the near-clamp), the loop may additionally scale separation down (VRto3D auto-depth behavior) and restore it as the scene relaxes. Shipped as an "Adjust depth too" experimental toggle so it can be evaluated live without a redesign.
- This is also the v1 weapon-comfort mechanism (weapon ≈ nearest object → its pop-out is bounded by target disparity). Future research (out of scope): per-primitive UObjectHook weapon tagging; depth-based near-pixel reprojection in the repack shader.

## 7. Auto 3D scaling (world scale + FoV) — always on

- Per frame (no toggle — user decision): `fov_scale = game_tan_half_h / tan(radians(reference_fov)/2)` (EMA ~0.2 against FoV cuts); `separation_m = user_depth × fov_scale`.
- **`reference_fov` is the calibration anchor:** the saved depth and convergence are defined *at* the reference FoV; `depth`, `convergence`, and `reference_fov` form one coherent calibrated triple, persisted together in `config.txt` (automatic — all three are ModValues in `m_options`; the settings page groups them visually and the tooltip states the correlation). Changing reference_fov rescales the effect exactly as if the game FoV changed.
- World scale needs no extra term: view path multiplies by `world_to_meters × m_world_scale` (:4779) and the shear uses unit-free `sep/conv` — VRto3D/perfect_dark's world-scale correction is inherent.

## 8. Settings / UI / keybinds (all `VR_Flat3D_*` via generate_name, auto-persisted per game)

Declare in VR.hpp (one contiguous block) + m_options; draw in new sidebar page "3D Monitor" (`get_sidebar_entries` VR.hpp:110-121; `case "3D Monitor"_fnv` VR.cpp:2348; body in VR_Flat3D.cpp):
- enable (restart req) · output_mode combo (14 modes incl LeiaSR, no mono) · eye_swap (also covers interlace phase — a one-line parity flip is identical to swapping eyes, so no separate parity setting)
- calibrated triple (grouped in UI, saved together): depth 0–0.5 m def 0.1 · convergence 0.01–25 m def 1.0 · reference_fov 40–140° def 90 (FoV auto-scale always on)
- autoconv: enable / target_disparity / smoothing / adjust-depth-too (experimental) / logging
- crosshair: mode combo {Off, Game crosshair, Laser sight} / adaptive toggle / game-crosshair region radius / laser size_px def 8 + color ARGB / static_depth m
- gui_depth 0.1–10 m def 1.0
- hdr_paper_white nits def 200 (shown when HDR swapchain detected)
- 4× ModKey keybinds matching VRto3D's hotkeys and step sizes: **Ctrl+F3/F4 = depth −/+0.001 m, Ctrl+F5/F6 = convergence ±0.005 m** (held-repeat, remappable), handled via `handle_flat3d_keybinds()` called from `VR::handle_keybinds()` (VR.cpp:1924, 1 line)
- Live readout of effective sep/conv/fov_scale; warnings (not in flat3d runtime; ui_target missing; SR runtime missing; backbuffer ≠ display res in interlaced/SR modes).

## 9. Rebase-friendliness — upstream files touched

| File | Change | Size |
|---|---|---|
| runtimes/VRRuntime.hpp | enum + is_flat3d() | ~5 |
| VR.hpp | ptr/accessors/ModValues/m_options/sidebar entry | ~70 contiguous + 2 one-liners |
| VR.cpp | clean_initialize branch, sidebar case, handle_keybinds call | ~15 |
| FFakeStereoRenderingHook.cpp | projection block + view gate | ~23 |
| D3D11Component.hpp/.cpp | branch + clear gate + reset | ~8 |
| D3D12Component.hpp/.cpp | same | ~8 |

Everything else in new files: `runtimes/Flat3D.*`, `mods/VR_Flat3D.cpp`, `mods/vr/flat3d/*`. All edits `is_flat3d()`-gated → HMD paths byte-identical; rebases conflict only on small call-site hunks. cmake.toml globs pick up new files (regen via cmkr).

## 10. Milestones (each runnable on a real UE game)

- **M1 — Skeleton + SbS passthrough (D3D11):** enum, Flat3D runtime, initialize_flat3d, on_frame_flat3d with SBS repack only, clear gate, gates in place with sep=0. Verify: injects with no SteamVR installed; two identical halves at native res; game FoV identical to vanilla (screenshot diff); menu usable; no flicker; vsync intact.
- **M2 — Real stereo + depth/conv + keybinds:** eyes[] separation, [2][0] shear, sliders/hotkeys. Verify: free-view/cross-eye parallax; zero-disparity plane moves with convergence; depth scales parallax; **sign check** (nearer-than-conv = crossed disparity, left image displaced right); eye_swap flips; in-game ADS changes FoV freely.
- **M3 — Full mode matrix + LeiaSR + D3D12 + HDR + AFR/native-stereo:** all repack modes, LeiaSR weaver (soft-load), D3D12 compositor, HDR color-space paths (PQ + scRGB) with paper-white setting, AFR eye cache, native-stereo-fix layout, upscale-then-pattern + res-mismatch warning. Verify: red-cyan anaglyph with glasses; interlaced row pattern is display-pixel-exact in zoomed capture even with render scale ≠ 100%; LeiaSR weave on the SR display; UE5/D3D12 title (double-precision) repeats M2 checks; HDR title outputs correct pass-through (no washed-out/dim shift) and usable anaglyph; resolve LeiaSR-under-HDR (tone-map vs SDR-required message); AFR mode composites correctly (one eye a frame older, no black/stale eye); a native-stereo-fix title shows correct eye layout.
- **M4 — GUI depth + crosshair (both modes):** per-eye UI composite, single depth slider with auto-scale; laser sight (static then adaptive), then game-crosshair extraction (center-region mask + re-draw at aim depth). Verify: HUD floats at slider depth and always fills the screen (no edge gap as depth increases); laser dot tracks aimed geometry near→skybox; game's own reticle rides at aim depth with the rest of the HUD at gui_depth; region radius tunable; graceful fallback when depth missing.
- **M5 — Auto-convergence:** histogram CS, control loop (conv-only default + experimental adjust-depth-too), settings. Verify: walk at wall → convergence eases, no over-pop; skybox → relax to ceiling; disable → snap back; ADS zoom keeps landmark's pixel disparity ~constant (always-on FoV scaling); logging sane; experimental joint mode reduces separation only when conv hits the near-clamp.
- **M6 — Polish + extras:** disparity debug OSD, 3D screenshot hotkey, docs; regression pass (UE4/D3D11 + UE5/D3D12); OpenVR/OpenXR smoke test proving zero diff with flat3d off.

## 11. Risks

- UE5 double-precision path must be exercised (M3); pre-existing inverted branch at :4999-5003 flagged for a separate upstream fix.
- AFR: composite pairs one fresh + one frame-old eye — fast camera motion shows retinal-rivalry shimmer (inherent to AFR, same as HMD; documented). Eye cache must invalidate on resize/reset.
- Native-stereo-fix titles change eye layout in the double-wide (desktop-fix samples left half, :892) — per-eye copy keys off the same flag; verify per title at M3.
- ui_target null in some titles → GUI slider and game-crosshair mode inert (hinted in UI; laser sight still works).
- Game-crosshair extraction: center-anchored HUD elements inside the region radius ride to aim depth (interaction prompts, hit markers) — radius tunable, laser-sight fallback.
- Interlaced/SR require a display-native backbuffer (borderless fullscreen, no DXGI/DPI scaling) — detected and warned; pattern itself is generated at output res (upscale-then-pattern). HDR anaglyph is approximate by nature (glasses filters are SDR-calibrated) — pass-through modes are exact; paper-white setting mitigates brightness mismatch.
- SceneDepthZ absent/TAA-jittered in some titles (median+EMA mitigates; static fallback).
- Window activation / mouse capture keys off m_submitted — parallel flat3d path required.
- SR SDK availability: LeiaSR compile-time optional + runtime soft-load (VRto3D pattern), mode hidden when absent.

## 12. Future (explicitly deferred)

**OpenTrack head tracking (v2, user-requested):** UDP pose input (VRto3D has the reference receiver + filter: `use_open_track`, track-filter settings in stereo_config.h) feeding a small head-pose offset into the flat3d view path — TrackIR-style look or head-coupled perspective on SR displays; slots in cleanly since `eyes[]`/rotation are already ours. Also deferred: WibbleWobble client handoff (frame-sequential); frame-packed HDMI-1.4; dual-display; weapon per-primitive convergence / near-reprojection shader; Lua API exposure of depth/conv; VRto3D-style display color-correction pass; frontend checkbox for flat3d (until then: in-game menu/config selection).

## Verification (overall)

Dev loop: build with cmkr/CMake (VS2026 toolchain per COMPILING.md), inject into a known-good UEVR title. Each milestone has its own checks above; the standing invariants across all of them: (1) game FoV byte-identical to vanilla (only [2][0] differs in the projection), (2) HMD modes unaffected when flat3d disabled (OpenVR/OpenXR smoke test), (3) native-res per-eye output, (4) no dependency on SteamVR/OpenXR being installed.
