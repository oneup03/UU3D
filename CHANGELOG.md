# Changelog

Notable changes to UU3D, newest first. Nightly builds are tagged by run number
(`00024`, `00025`, …) — see the
[releases page](https://github.com/oneup03/UU3D/releases).

A release ships **both backends** in one package (Legacy at the root, Modern
under `modern\`), so entries apply to both unless a heading says otherwise.

## Unreleased

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

## Initial Release - 00024

## Unreal Universal 3D (UU3D) - a fork of UEVR for 3D displays
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
### Full readme and download: https://github.com/oneup03/UU3D#unreal-universal-3d
### Created profiles for: 
Black Myth Wukong, Final Fantasy 7 Rebirth, The Plucky Squire, Expedition 33, Shin Megami Tensei 5V, Stellar Blade, Returnal, Palworld, Persona 3R, Jedi Survivor, Hogwarts Legacy, Hellblade 2, Gotham Knights, and Denshattack
https://github.com/oneup03/UU3D-Profiles
