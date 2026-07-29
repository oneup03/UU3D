# Unreal Universal 3D

UU3D can output stereo 3D directly to a screen — no headset, no SteamVR
or OpenXR install — as a third runtime alongside OpenVR/OpenXR. The game
renders in stereo through its normal render pipeline; instead of submitting to a
VR compositor, the two eyes are woven into the game's own backbuffer in your
chosen 3D format and presented normally. Works on 3D TVs, 3D projectors,
Interlaced, LeiaSR, AR glasses, Frame Sequential, and plain monitors via
anaglyph output.

This page walks through the **3D Display** tab in the UU3D menu, top to bottom.

## Enabling

1. In the UU3D injector, pick **3D Display** as the runtime, then inject.
   (Or set `Frontend_RequestedRuntime=flat3d` in the game's `config.txt`.)
2. The runtime is chosen at startup — hot-swapping between a headset and 3D
   Display is not supported, so relaunch if you change it.

No `openvr_api.dll` / `openxr_loader.dll` is loaded or required. When 3D
Display is active, the headset-only pages (motion controllers, room setup)
are hidden and their buttons are no-ops.

## Output Mode

Pick how the stereo pair is packed into the screen image:

- **Side by Side**, **Top and Bottom** — for 3D TVs/projectors set to the
  matching SBS/TAB mode, and for most VR video players.
- **Row Interlaced**, **Column Interlaced**, **Checkerboard** — for passive
  3D displays. These patterns are drawn in real display pixels, so any render
  resolution stays line-exact — but the game must run **borderless fullscreen
  at the display's native resolution** (any scaling applied after us breaks
  the pattern; you'll get an on-screen warning if a mismatch is detected).
- **LeiaSR (SR display)** — glasses-free Leia/Dimenco panels. Needs a 
  running SRService (D3D11/D3D12); falls back to Side by Side otherwise.
- **Frame Packed 720p60 / 1080p24 / 1080p60** — HDMI 1.4 "3D" for 3D TVs. Emits
  Top-and-Bottom with the HDMI blanking gap **and auto-applies the matching
  display timing** (NVIDIA/AMD, or a CRU-preconfigured mode), which is what
  makes the TV show its 3D icon. The timing reverts on exit. Intel GPUs need
  the mode pre-added in CRU.
- **Dual Display** / **Dual Display (Flip)** — send one eye to each of two
  monitors arranged side by side (Flip vertically mirrors the left eye for
  mirror-style rigs).
- **Katanga (shared texture)** — publishes the pair as a shared texture for
  VR and Frame Sequential viewers (Katanga.exe / VRScreenCap / NV3D-Glass /
  WibbleWobbleInjector); launch any app in any order. Forces
  the game into a normal window and shows a flat left-eye preview locally.
- **Anaglyph** (Red/Cyan, Green/Magenta, Blue/Amber, plus Dubois / Deghosted /
  Compromise variants) — for colored 3D glasses on any ordinary screen.

**Swap Eyes** — flips left/right if the depth looks inverted (also flips the
interlace phase for passive displays).

**VSync Override** — *No-Tear Fast* (default), *Force On*, or *Use In-Game
Setting*. No-Tear Fast overrides the game's own VSync: presents run uncapped
at sync interval 0 with the DXGI tearing flag stripped, so a flip-model
swapchain (all DX12 games) still flips on vblank only — tear-free **and**
both eyes of an AFR-family pair land every refresh. The frame cap
(`t.MaxFPS`) is managed automatically: twice the display refresh under
Synced Sequential / AFR / AFW, native refresh under Native Stereo. (DX11
exclusive-fullscreen can still tear at interval 0 — use *Force On* there, or
run borderless windowed.)

**Force SDR Output** — HDR output washes out the 3D modes and disables color
correction. Leave this on unless you specifically want to experiment with HDR
passthrough.

> Output is always held at the display's native resolution. To lower GPU cost,
> turn down the **in-game** resolution (or resolution scale) — that path is
> upscaled correctly and won't break interlaced/checkerboard modes.

### Full-width side-by-side panels (32:9)

Some 3D displays are a single ultra-wide panel — e.g. 3840×1080 (32:9), where
each eye is a full 16:9 half. When **Side by Side** is selected on a panel wider
than ~2.5:1, UU3D detects this automatically and switches to **Native Render +
Upscale**: the game is told its window is one eye wide (via a client-rect
spoof), so it renders each eye at native 16:9 instead of being squeezed to 32:9,
and the two eyes are composited back across the full panel. The result is full
per-eye sharpness with no configuration. It's a no-op on ordinary half-width SbS
displays (1920×1080), where the display itself stretches each half.

Because the engine believes its window is one eye wide, the game's UI, the UU3D
menu and the mouse cursor are all authored at that perceived per-eye size and
composited into each eye — see the limitation in *Compatibility notes* for the
rare title that ignores this.

## 3D Calibration

Three settings form one calibrated set, saved together per game:

- **Depth** — eye separation (how strong the 3D is).
- **Convergence** — the distance that sits exactly on the screen plane. Things
  nearer than this pop out toward you; things farther sit behind the screen.
- **Reference FoV** — the field of view at which Depth and Convergence were
  dialed in (recommended to set this to gameplay FoV).

Separation then auto-scales with the game's live FoV (always on), so zooming or
aiming down sights keeps the perceived depth constant. The game's FoV is never
overridden — only the stereo shear is injected into its own projection.

Hotkeys (hold to repeat): **Ctrl+F3 / F4** depth −/+, **Ctrl+F5 / F6**
convergence −/+. **Ctrl+F12** (or the **Take 3D Screenshot** button at the
top of the menu) saves the composited stereo pair — the game, its own HUD, the
crosshair and the stereo cursor at the current convergence, with only the UU3D
menu hidden for the capture and both eyes valid even under AFR — as two PNGs
under `<persistent>/flat3d_screenshots/`, regardless of the on-screen output
mode: a canonical parallel-view (`sbs_<timestamp>.png`, left|right) and a
cross-view companion (`…_crossview.png`, right|left) for cross-eyed free-viewing.
The capture spans the 1–2 presents it takes to refresh both eyes with the menu
hidden, so the menu briefly blinks off on screen as the shot is taken.

## Depth Source

The scene-depth source for **Adaptive Crosshair**, **HUD Depth**, and
**Auto-Convergence**:

- **Per-Draw Capture** (default) — a safe, API-level capture of the game's
  rendering. Works on most titles, but it's blind to depth the game allocates
  once at load time; in that case the depth features simply stay flat.
- **Engine Pool (SceneDepthZ)** — reads the engine's own scene depth from its
  render-target pool. Fixes titles that allocate depth at load (e.g. *SMT V:
  Vengeance*), but it installs an engine hook that crashes a few games (e.g.
  *Jedi: Survivor*). Enable it per-game. (Replaces the old **Use Engine Depth
  Buffer** toggle; saved configs carry over.)
- **DSV Observer** (D3D12 only) — watches depth-stencil views and resource
  barriers at the API level and snapshots the live scene depth each frame.
  Installs **no engine hook** (safe where Engine Pool crashes) and sees depth
  allocated at any time (works where Per-Draw stays flat). A status line under
  the combo shows what the observer is tracing.
- **DLSS Depth** (D3D12 only) — snapshots the exact depth the game feeds DLSS,
  captured from the DLSS call itself (no engine hook, no plugin). Only has
  data while DLSS is enabled in the game's graphics settings; works in any
  rendering method and is the natural choice when playing with AFW.

## Auto-Convergence

Automatically pulls the screen plane just in front of the nearest significant
object so its pop-out never exceeds **Target Disparity** (a fraction of screen
width). Your manual Convergence acts as the ceiling; turning this off snaps back
to it.

- **Smoothing** — how gently the convergence follows depth changes.
- **Min Convergence** — never pulls the plane closer than this (raise it if
  nearby objects drag the whole scene too deep).
- **Log Samples** — diagnostic logging.

Separation auto-scales with the pull-in, so the background stays exactly where
you calibrated it. This reads the engine depth buffer (see above).

## Crosshair

Put the aiming reticle at the right depth so it lands on the target instead of
floating on the screen plane.

- **Mode** — *Off*, *Game Crosshair* (re-projects the game's own reticle to the
  aimed depth by extracting a small region of its UI), or *Laser Sight* (a
  separate depth-aware dot, classic 3D-Vision style).
- **Adaptive Depth** — read the aim depth from the depth buffer. When it's
  unavailable the **Static Depth** slider is used instead.
- Game Crosshair: **Region Radius** (size of the extracted center patch — HUD
  inside it rides along) and **Region Center Y** (its vertical position).
- Laser Sight: **Dot Size** and **Dot Color**.

## GUI

Controls the depth of the game's 2D UI (health bars, menus, markers), the
UU3D menu, and the mouse cursor.

**HUD Depth Mode**

- **Flat (GUI Depth)** — the whole UI sits on one plane at **GUI Depth**.
- **Depth-Adaptive (Auto-Classified)** — UI that *moves with the camera*
  (waypoints, nameplates, floating markers) is placed at the scene depth beneath
  it, while static HUD stays flat. It learns which is which as you look around;
  needs a working scene depth source. See tuning below.
- **World Markers (Auto-Detected)** — hooks the engine's world→screen
  projection and puts UI at each marker's true 3D position. Coverage depends on
  how the game drives its HUD.

**Full-Screen UI Coverage %** — when the game's UI covers at least this much of
the screen, HUD/crosshair depth flattens automatically. This catches full-screen
menus (map, inventory, controller menus) that don't show a mouse cursor. Raise it
if a busy gameplay HUD trips it; lower it if a full-screen menu isn't caught; 0
disables the coverage check (paused-game detection still applies).

**Depth-Adaptive tuning** (only shown in that mode):

- **Icon Region Radius** — how far around a detected moving element the depth
  shift spreads, so a whole icon and its text move as one piece.
- **Show Classification (debug)** — tints the HUD so you can tune it: red =
  treated as world (gets scene depth), green = treated as flat HUD. Look around
  to train it.
- **False-Positive Rejection** — stops animated-but-fixed HUD (a spinning
  minimap, gauges) and menu backdrops from being mistaken for world-tracking UI:
  - **Suppress Permanent Panels** — UI that's on-screen every frame (a minimap
    disc, a gauge cluster) is forced flat, along with a small halo around it.
    **Panel Occupancy** sets how "always there" it must be; **Panel Halo** grows
    the flat margin; **Marker Safe Zone W/H** is a central box where this
    suppression is turned off, so real markers near the middle are never
    flattened.
  - **Reject Large UI Fills** — a big solid region of UI (a menu backdrop or
    blurred scrim) is never a marker — a marker is a small island. This forces
    such fills flat immediately, even in the center. **Fill Radius** sets how
    large "big" is; **Fill Coverage** how solidly filled it must be.
  - **Translation / Rotation Gate**, **Translation Floor** — how much motion
    evidence is required before UI is called "world." Raise the gates for fewer
    false positives (may miss subtle markers); the floor ignores tiny camera
    drift.
  - **Exclusion Zones** — up to four rectangles you mark as always-flat HUD
    (center + half-size in screen fractions, 0..1). Set a zone's half-size to 0
    to disable it.

**GUI Depth** — depth of the flat UI plane. **UU3D Menu Depth** — depth of
the UU3D menu. Depths are relative to convergence: **1.0 = the screen plane**
(no shift), below 1 pops out, above 1 sits behind. Layers scale down as they
shift so nothing is pushed off-screen.

**Stereo Cursor** — replaces the flat system cursor (which the OS would draw at
screen depth in one eye only) with a proper per-eye cursor whenever the game
shows one: *None*, *GUI Depth* (at the UI plane), or *Geometry (Under Cursor)*
(lands on the scene surface right under the tip; needs a scene depth source, and
falls back to GUI depth over full-screen menus). **Cursor Size** sets its size.

> The UU3D menu's font size is set from the main UU3D menu → Configuration →
> Font Size.

## Head Tracking (OpenTrack)

Point OpenTrack's **UDP over network** output at `127.0.0.1:<port>` (default
4242). Two independent gains:

- **Look Sensitivity** — adds head yaw/pitch/roll on top of the game's camera
  (TrackIR-style).
- **Parallax Sensitivity** — shifts the eye origin with head x/y/z for a
  fishtank / head-coupled effect (natural on SR displays).

Use OpenTrack's own **Center** hotkey to zero the neutral pose. Off by
default and a pure no-op when disabled.

## Color Correction (SDR)

Adjust lift / gamma / gain plus an S-curve contrast, applied as a post step in
the weave shader — handy for compensating for the dimming/tint of glasses or
reducing crosstalk. **Enable**, then adjust **S-Curve Contrast** and the per-
channel **Lift / Gamma / Gain** sliders. SDR output only (ignored under HDR).

## Advanced

- **HDR Paper White (nits)** — reference white used when converting HDR frames
  for anaglyph output.
- **D3D12 Debug Layer Log** — diagnostic logging of D3D12 validation messages
  for depth-feature crashes (needs the debug layer enabled on the game's device;
  see the in-menu tooltip). D3D12 only.

## Compatibility notes

- **Synchronized Sequential** shows only complete, matched pairs: both eyes
  come from the same game state (a pair lock holds the first eye until its
  partner arrives), and eye identity is measured per frame rather than
  inferred — no cross-state judder, no inverted eyes, at roughly half the
  effective framerate (each world state is rendered twice).
- **Alternating/AFR** works with a per-eye cache: each frame one eye is fresh
  and the other is one frame old. Fast motion shows the usual AFR shimmer.
- **Alternate Frame Warping (AFW)** renders one eye and reprojects the other
  from color + depth + motion vectors — near-2x scene performance with
  same-state eyes. D3D12 only, needs DLSS enabled in-game (raw-buffer
  fallback exists) and the real `PDAFWPlugin.dll` beside `UEVRBackend.dll`;
  without it, AFW falls back to plain AFR. Pair it with the **DLSS Depth**
  source so the depth features read the same depth the warp uses.
- **Native Stereo Fix** titles are handled (eye layout follows the headset
  logic), including modular DLL builds and engines with customized
  scene-view-family layouts (Returnal, Hellblade 2).
- Games that need **AHUD UI compatibility** (UI drawn via the viewport
  render target, e.g. P3R) can look overly transparent — see **UI Invert
  Alpha** and **UI Color Gate** on the main Compatibility page.
- **Recommended: add `-nohmd` to the game's launch arguments.** Titles with
  their own **native HMD/OpenXR stereo path** can misbehave when UU3D drives
  stereo — either the game HUD/UI disappears (e.g. *Stellar Blade*), or the
  game tries to start up SteamVR/an HMD on launch (e.g. *Storybook*). `-nohmd`
  forces the game to mono so UU3D drives stereo cleanly — add **`-noxr`** as
  well if needed.
- On a **full-width (32:9) SbS panel** (see *Full-width side-by-side panels*
  above), a title that draws its UI across the *entire physical panel* rather
  than the per-eye area — e.g. *FF7 Rebirth*, whose menu composites full-width —
  can't be cropped down to a single eye, so that UI is stretched across the
  pair. The in-game HUD, which respects the perceived per-eye size, is
  unaffected, and standard 16:9 displays are never affected. There is no
  per-game fix for this today.
- Both UE4 (float) and UE5 (double-precision) projection paths are supported.
- Quick sanity check: Side by Side with Depth 0 gives two identical halves;
  raise Depth and nearer-than-convergence objects show crossed disparity (use
  **Swap Eyes** if it's inverted).

## Thanks to

- **[praydog](https://github.com/praydog)** — creator and principal author of
  UEVR, the injection and stereo-rendering framework this mode is built on.
- **joeyhodge** - got more recent UE 5.x games working
- **PureDark** - created Alternate Frame Warping
- The UEVR contributors whose work this builds on: narknon, keton, cursey,
  mark-mon, mrbelowski, npt-1707, Raicuparta, Anton-4, and markmontec.
- **MidlifeCrisis / EvilKermitReturns** — for early exploratory work on Unreal
  Engine stereo 3D that shaped several of the design decisions applied here.
