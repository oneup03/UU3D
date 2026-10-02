# Changelog

Notable changes to UU3D, newest first. Nightly builds are numbered one per release
(`00024`, `00025`, …) — see the
[releases page](https://github.com/oneup03/UU3D/releases). Up to `00046` the
number was the workflow run counter, so those skip values.

From **00037** a release ships **both backends** in one package (Legacy at the
root, Modern under `modern\`), so entries apply to both unless a heading says
otherwise. Earlier nightlies were a single build, alternating between the two
bases depending on which branch produced them.

## Nightly 00047

### Added
- **Alternate Frame Warping without DLSS.** AFW used to engage only when the
  game ran DLSS (or the raw harvest found a DLSS-shaped depth buffer); any
  other title silently fell back to plain AFR. The warp now takes its depth
  from the Flat3D depth source (3D Display page; DSV Observer is the usual
  choice on D3D12) whenever no DLSS depth arrived, so AFW works in titles
  that never load DLSS. The AFW settings show what feeds the warp (*Warp
  depth feed* / *Warp MV feed*).
- **Per-Object Motion Vectors (UE velocity)** under Alternate Frame Warping,
  for titles without DLSS: harvests Unreal's velocity buffer per eye and
  decodes it into motion vectors for moving objects (camera motion from
  depth plus the object's own motion; static pixels stay zero, so *Ignore
  Motion Threshold* keeps working), which tightens fast-moving characters in
  the warped eye. Off by default; with *Warp MV Type* and two scale sliders
  for the sign/magnitude conventions.
- AFW *Warp MV Type* Auto now tells the warp that the previous frame belongs
  to the other eye unless the Ghosting Fix is actually active (per-eye scene
  histories), not merely enabled; the AFW page shows *Ghosting fix active*.
- **Convergence presets on a key.** Under the Convergence slider: a *Cycle
  Key* (default **F2**, click to rebind to any keyboard key or gamepad button)
  and a *Cycle Values* box holding comma-separated convergence distances
  (default `0.5, 1, 2, 4`). Each press steps Convergence to the next entry,
  wrapping at the end. Handy for games that alternate between a close
  dialogue camera and open play.
- **VSync Override: Force 1/2, for frame-sequential 3D.** If you watch
  through Katanga (3D Vision via vrscreencap / geo-11, NV3D-Glass) or
  WibbleWobble, those consumers cover the game window with their own
  fullscreen output, which stops DXGI from throttling the game. Under No-Tear
  Fast the only pacer left is then the `t.MaxFPS` timer, and a timer is not
  the display: the two clocks beat against each other and every few minutes a
  world update is dropped or doubled, which comes through as a periodic
  hitch in the glasses. Force 1/2 hands the clock back to the display: the
  stereo pair rate becomes exactly half the panel refresh, timed on its
  vblank even while the window is covered, with `t.MaxFPS` left uncapped so
  there is no second clock. Native Stereo presents every second refresh;
  AFR / Synced Sequential / AFW present every refresh, one eye each. A
  `[Flat3D][pace]` log line reports how often the guard had to wait. Along
  the way, the frame cap under No-Tear Fast no longer engages at random:
  two code paths were both writing `t.MaxFPS` every frame and disagreeing.
  (**Legacy** also had to start honouring the requested present interval in
  its D3D12 present hook, which it had silently ignored.)

### Fixed
- AFW *Ignore Motion Threshold* and *Clear Before Framewarp* were never saved
  to the profile and reset on every launch.
- **The adaptive crosshair reads the depth under the reticle, not beside it.**
  The depth buffer is a per-eye image, so whatever is under your reticle sits
  a little to one side in it, by an amount that depends on its distance. The
  sampler used to cover that by reading a wide band around the centre and
  taking the nearest surface — which also grabbed any near object merely
  *beside* the reticle. It now shifts the sample to where the target actually
  is and reads a narrow window there. Under Native Stereo Fix that image is
  the right eye's, not the left's as assumed, which had put the correction on
  the wrong side and read as the crosshair taking its depth from the wrong
  place (*Returnal*). The geometry cursor and the depth-adaptive HUD carried
  the same bias and share the fix.
- **The DirectX 11 depth readback no longer copies straight out of the depth
  buffer.** D3D11 silently refuses that for depth-stencil and multisampled
  targets, so every sample read as zero and the adaptive crosshair,
  auto-convergence and HUD depth stayed flat on D3D11 titles. Depth is now
  read through a shader pass, and Per-Draw Capture also hooks the
  render-target bind UE4's D3D11 path uses whenever compute resources are
  bound, which it had been missing. The log now records each depth resource
  it reads (`[depth-src]`) and a per-draw summary. Confirmed on *RAIN CODE*
  with the **Engine Pool** source: adaptive crosshair, auto-convergence and
  HUD depth all read real depth there now, the first D3D11 title to. The
  earlier all-zero readback through the new path was taken on a loading
  screen, where the depth buffer is simply cleared. **Per-Draw Capture on
  D3D11 still does not publish** (it sees the depth targets but attributes no
  draws to them) - use Engine Pool on D3D11 for now.

- **High-DPI displays: the image no longer sits top-left with black bars.**
  On a display scaled above 100%, a UE4 game in Windowed Fullscreen sizes
  itself to the desktop divided by the scale (2560×1440 on a 4K panel at
  150%) and substitutes that for any larger request, so it kept a 2560×1440
  swapchain inside the native window. Whether UU3D's DPI spoof headed that
  off was a startup race, won on some launches and lost on others with
  identical settings. The lost case now has an exact signature (the engine's
  size equals native divided by the scale while the spoof is active), and
  once the ordinary resize requests are ignored UU3D switches the engine to
  Windowed at native, which UE applies literally; native output keeps the
  window borderless, so it looks the same, and the game's own saved settings
  are untouched. (*RAIN CODE*, UE 4.27.) The recovery path has not yet been
  seen running: every launch since it landed has won the race and come up at
  native on its own, and an in-game resolution change cannot reproduce the
  lost case because by then the spoof is in effect. Treat it as unverified
  until a launch logs "applying ... as Windowed".

### Changed
- **Auto-convergence snaps on camera cuts** instead of easing over a second
  at the wrong depth: a cut to a close framing is detected within a couple of
  frames and convergence jumps to it. Only cuts toward the camera snap; cuts
  away still ease, which keeps anything moving close to the camera from
  making convergence thrash. Easing back out is also slower than pulling in
  now, so a receding object no longer reads as the image drifting.
- **README caught up with the Separation scheme** from 00046 (it still
  described Depth / Reference FoV), and now documents Hold Window Size, Keep
  Game's Saved Video Settings, 3D Render Resolution, Applied To, Camera FoV
  Axis, 3D FoV Multiplier, Marker Region Radius and Stem Reach.

### Upstream (Modern only)
The Modern backend merged Joey Hodge's `ue57performance` branch three times
since 00046 (149 upstream commits, 2026-08-13 to 2026-09-29). Those commits
target the VR path, but the parts UU3D shares with it (Native Stereo Fix
renderer discovery, UObject / CVar startup hardening, dedicated UI routing,
UE 5.7 / 5.8 support) may improve these titles under Flat3D as well. None of
them have been verified in UU3D; the list is what upstream's commits name:
- *Kingdom Hearts III* - UObject tracking, Ghost Fix, Native Fix capture.
- *Dune: Awakening* - Native Fix render-frame handoff, D3D12 residency,
  view-extension callbacks, the WinGDK build and the public-test ABI.
- *S.T.A.L.K.E.R. 2* - CVar startup stall, Synced scene publication,
  dedicated UI target, attachment lifetimes, DeepDVC disabled in VR.
- *Star Wars: Zero Company* - DX12 scene and Slate output, hologram passes,
  viewport conversion, the post-update stereo hooks.
- *Days Gone* - native stereo, right-eye snapshots, AHUD flicker and Slate
  composition, weapon aim / reticle projection, UI alpha.
- *Suicide Squad: Kill the Justice League* - Native Fix renderer entry,
  stereo views, per-eye resources, paired render resources.
- *NASCAR 25* and *NASCAR 26* - stereo and UI compatibility, Native Fix
  capture gamma, per-eye copy states.
- *Borderlands 4* - UE 5.5.4 dedicated UI routing, Slate guards.
- *Hi-Fi RUSH* - Native Fix renderer discovery, protected Tick hook.
- *Stellar Blade* - Native Fix renderer entry selection.
- *Bodycam* - UE 5.5 rendering, owned UI, Native Fix eye exposure, OpenXR
  mode transitions.
- *Mafia: The Old Country* - UE 5.4.4 discovery, Native Fix command ownership.
- *Hellblade: Senua's Sacrifice* - UE 4.25 Native stereo rendering.
- *Sifu* - DX11 Native mesh bindings and renderer discovery.
- *Halloween* - validated Native Fix family copies, scene allocation, Slate
  UI ABI.
- *Breathedge 2* - inventory world rendering.
- *The Sinking City 2* - Slate UI routing after the game's update.
- *Deadzone 2*, *The Medium*, *Observer*, *Townfall*, *Far Far West*,
  *StormEscape*, *Captain Tsubasa*, *Pokemon Emerald (UE 5.6)* - one or two
  renderer-hook, capture or projection fixes each.
- Engine-wide: UE 5.7 and 5.8 (up to 5.8.3) stereo, CVar and UI discovery,
  Bink overlay compatibility, Nanite hologram pass guards, praydog's D3D12
  destination-barrier fix, the FRenderTarget gamma hook landing on a garbage
  vtable slot, an on-demand anisotropic filtering CVar.

## Nightly 00046

### Changed
- **Depth is now Separation, and it means something you can see.** The old
  Depth slider was an eye separation in metres, which is not a quantity anyone
  perceives, and it needed a **Reference FoV** slider beside it to stay
  consistent when a game zoomed. Separation replaces both: it is the background
  3D strength expressed as a fraction of your screen's width, so a value of
  0.05 puts distant objects 5% of the screen apart. Your existing Depth,
  Convergence and Reference FoV settings are converted automatically the first
  time a profile loads, and the picture is unchanged - nothing to re-tune.
  **Reference FoV is gone**; it no longer has anything to calibrate.
- **The 3D effect now holds through zoom/ADS instantly.** It was already meant
  to, but the old scaling settled over several frames after a hard FoV cut, so
  depth was briefly wrong every time you aimed down sights. Separation is
  independent of FoV by construction, so there is nothing left to settle.
- **Convergence no longer changes background depth.** Moving the screen plane
  used to quietly weaken or strengthen the whole image; now it only moves what
  sits in front of the screen, and the background stays where Separation put
  it. Worth knowing if you are used to compensating for the old coupling. On
  autostereo panels this also makes Separation a directly comparable crosstalk
  budget across games - lower it if you see ghosting.
- **World Scale and Depth Scale are hidden in 3D mode.** Neither did anything
  useful there. World Scale had become a second, invisible depth multiplier
  that fought the Separation slider, and Depth Scale only ever affected OpenXR
  headset submission.
- **L3+R3 now needs a deliberate long press by default**, so an incidental
  stick click no longer opens the menu.
- **Convergence now goes up to 25m** instead of 5m. The Ctrl+F5/F6 hotkeys
  already reached 25m, so the slider simply could not show or recover from
  where they could put you.
- **The adjust hotkeys are time-based, and convergence moves proportionally.**
  Ctrl+F3..F6 used to apply a fixed step per rendered frame, so a 144Hz game
  swept nearly five times faster than a 30fps one. They are now rates per
  second and behave identically at any framerate. Convergence also scales its
  step with the current value, which is what makes a 0.001-25m range usable:
  about 13 seconds end to end while still moving in ~1cm increments down at
  1m, where you actually tune it.

### Fixed
- **World-anchored HUD icons no longer fall back to flat at higher
  convergence.** Reported in SMT5V: icons held their world depth below 5m
  convergence and popped back to HUD depth somewhere between 5 and 10. The
  classifier's "the camera is moving sideways, so world-anchored UI should
  slide" test was measuring that motion against the convergence distance, so
  pushing the screen plane out raised the bar in proportion - about 0.18 m/s
  of sideways movement at 1m, but 1.8 m/s at 10m, which ordinary walking never
  reaches. It now measures against a fixed reference depth, so convergence has
  no say in it.
- **The HUD no longer stops tracking depth for close-up content.** Its depth
  shift was capped at the same limit in both directions. That limit is right
  for things behind the screen, where too much separation forces your eyes
  apart and the image stops fusing, but in front of the screen your eyes turn
  inward instead and the cap only clipped valid pop-out. Anything nearer than
  about a third of the convergence distance simply stopped tracking. The
  in-front limit is now three times larger; the behind-the-screen one is
  unchanged.
- **The menu no longer clips at larger font sizes.** The window and its sidebar
  were both sized for the old 16px font, so raising the font size ate the page
  area and cut off the longer sidebar labels. Both now scale with the font, and
  the window stays fully on screen at 1080p.
- **Jedi Survivor no longer crashes in Native Stereo (Legacy backend).** Its
  view array resolved to a bogus offset, and the multi-view hide then wrote a
  zero over unrelated engine data. The write is now rejected when the offset
  cannot be real. **The Modern backend still crashes here** - the same fix did
  not address its failure, so it is not applied there; use Legacy, or
  Synchronized Sequential, for this title.
- **3D Render Resolution had no tooltip and the 3D FoV Multiplier showed the
  wrong one.** The render-resolution help had drifted onto the FoV control, and
  the multiplier's own explanation only appeared after you had already changed
  it. Both now describe what they actually do.

## Nightly 00037

### Added
- **Legacy and Modern backends in a single package.** Legacy at the package
  root, Modern under `modern\`, each with its own matching `LuaVR.dll` and
  `PDAFWPlugin.dll`.
- **Backend switch in UU3DI.** Radio pair on the inject row picks Legacy or
  Modern, saved per game profile and carried by Export/Import Config. Defaults
  to Legacy.
- **Auto inject in UU3DI.** Watches for games once a second and injects 20
  seconds after one appears, then stays idle until it exits. A game opts in by
  having a saved profile. Global setting, off by default.
- **Ghost Reduction (Crosstalk).** Two sliders — a contrast squeeze toward
  mid-grey, and a black-floor lift ("foot-room") aimed at the bottom-end
  clipping that displays with their own crosstalk cancellation suffer. SDR only;
  3D screenshots are captured without them.

### Changed
- **Color Correction (SDR) has been removed**, replaced by Ghost Reduction
  above. It existed to fight ghosting and needed eleven sliders to do what those
  two now do. Its `Flat3D_ColorCorrection`, `Flat3D_Lift*`, `Flat3D_Gamma*`,
  `Flat3D_Gain*` and `Flat3D_SCurve` config entries are ignored and dropped the
  next time settings are saved.
- Nightly releases are now published manually or on a schedule, and one release
  contains both backends rather than whichever branch built last.

### Fixed
- **"Applied To" and the FoV axis no longer reset on every injection.** Both
  settings were being applied for the session but never written to `config.txt`,
  so they reverted to their defaults each time. Affected every game.
- Auto camera FoV axis detection, which now defaults to Horizontal.

## Nightly 00035

### Added
- **Legacy backend.** First build on the plain praydog nightly + PureDark AFW
  base, rather than the Joey Hodge merged fork everything before this was built
  from. It is the more conservative of the two and is what later became the
  **Legacy** half of the dual-backend package.

## Nightly 00028

### Added
- **Render resolution now runs through `r.ScreenPercentage`**, with an
  **Applied To** selector choosing where it lands: *Primary* (the upscaler's own
  input resolution, which is what DLSS/TSR set), *Secondary* (stacks on top of
  DLSS/TSR), or *Stereo Render Target* (the previous behaviour). Lets you render
  below native and let the game's own upscaler do the work.

### Fixed
- Alternating/AFR rendering only filling half the frame at some resolutions.
- Camera FoV axis detection.

## Nightly 00026

### Added
- **LeiaSR now releases the display's switchable lens when you stop weaving.**
  Leaving LeiaSR - by switching output mode, or by the weaver failing - used to
  leave the panel lensed over every other 3D mode, other applications, and the
  desktop.
- LeiaSR is driven through SR-lib's wrapper rather than the SR SDK directly,
  which brings the DLL preflight and exception containment with it.

### Fixed
- **Engine-version misdetection on some UE5.6 titles.** A game reporting its own
  version instead of the engine's made every version gate take the wrong branch,
  which could hang the render thread for a minute at startup before failing.
- Eye parity (frames latching to one eye) and the Native Stereo Fix hook.

## Initial Release - 00024

### Features:
- Based on Upstream PureDark AFW Joey Hodge fork
- Additional Alternate Frame Warp render mode - a geometric 3D mode with only 20-30% render cost but some artifacts
- Output modes: SbS, TaB, Row/Column Interlaced, Checkerboard, LeiaSR, FramePacked (untested), Dual Display (untested), Katanga (for VR/3DVision/Frame Sequential via vrscreencap/NV3D-Glass/WWInjector)
- Auto FoV adjustment - working scopes and zoom, comfortable 3D and cinematic cutscenes
- Auto GUI resizing to match current FoV
- Auto Convergence based on depth buffer
- Dynamic Crosshair
- Dynamic GUI elements that can be setup based on several parameters
- Stereo Cursor with either static or dynamic depth
- Open Track (Untested)
- Color Correction - adjust brightness/contrast/colors to help with games that are too dark or have too much crosstalk on your display
### Created profiles for: 
Black Myth Wukong, Final Fantasy 7 Rebirth, The Plucky Squire, Expedition 33, Shin Megami Tensei 5V, Stellar Blade, Returnal, Palworld, Persona 3R, Jedi Survivor, Hogwarts Legacy, Hellblade 2, Gotham Knights, and Denshattack
https://github.com/oneup03/UU3D-Profiles
