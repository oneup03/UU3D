# Changelog

Notable changes to UU3D, newest first. Nightly builds are tagged by run number
(`00024`, `00025`, …) — see the
[releases page](https://github.com/oneup03/UU3D/releases).

From **00037** a release ships **both backends** in one package (Legacy at the
root, Modern under `modern\`), so entries apply to both unless a heading says
otherwise. Earlier nightlies were a single build, alternating between the two
bases depending on which branch produced them.

## Nightly 00040

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
