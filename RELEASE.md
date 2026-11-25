# Prince of Persia: Warrior Within (PS Vita) — Beta Release v1.1.0

Second public beta of the native iOS-to-PS Vita wrapper port of *Prince of Persia: Warrior Within* (iOS v1.0.7). It adds an in-game options menu, on-screen control toggling, native PS Vita trophies and a fixed intro video. The game is controllable with touch and physical buttons.

## 📦 Release Package Contents

- `princeofpersiawarriorwithin.vpk`: the loader (Title ID `PSVPOPWW1`, version `01.10`). Install the full VPK: the trophy set is inside it.
- No game files are included. `rebase.bin` is still derived from your own copy of the game executable (see v1.0 notes below).

## 🚀 Player Installation Guide

1. **Requirements**: Enso 3.60/3.65, `ur0:tai/kubridge.skprx` under `*KERNEL`, and `libshacccg.suprx` in `ur0:data/`.
2. **Trophies (optional)**: `ur0:tai/NoTrpDrm.suprx` under `*main` in `ur0:tai/config.txt`. Reboot the Vita after installing it.
3. **Assets**: from your FairPlay-decrypted v1.0.7 `.ipa`, copy the contents of `Payload/<App>.app/` to `ux0:data/princeofpersiawarriorwithin/game_app/`. Put `rebase.bin` in `ux0:data/princeofpersiawarriorwithin/`.
4. **Install** the full `princeofpersiawarriorwithin.vpk` with VitaShell.

## 🎮 Controls

| Input | Action | Status |
| :---: | :--- | :---: |
| **Touchscreen** | Original on-screen virtual pad and buttons | ✅ Working |
| **Left stick / D-Pad** | Move | ✅ Working |
| **Right stick** | Camera | ✅ Working |
| **Cross** | Jump | ✅ Working |
| **Triangle** | Attack | ✅ Working |
| **Circle** | Grab | ✅ Working |
| **Square** | Drop weapon | ✅ Working |
| **Start** | Pause / menu; in the pause menu, taps **CONTINUAR** to resume | ✅ Working |
| **Cross** or **Start** | Continue past the "TOUCH THE SCREEN TO CONTINUE" screens | ✅ Working |
| **R** | Block | ✅ Working |
| **L** | Time rewind | ⚠️ Unverified |
| **L + R** | Show / hide the on-screen touch controls | ✅ Working |
| **Select** | Slow motion | ⚠️ Unverified |
| **Select + Start** | Options menu (see below) | ✅ Working |

Options menu (Select + Start, game paused behind a translucent panel): button remap editor (saved to `controls.txt`), on-screen controls, screen size, screen filter, FPS counter, FPS limit, iPhone profile.

## 📋 What's New in v1.1.0

- **Options menu**: Select + Start opens the port options.
  - **Button remap**: Jump, Drop weapon, Attack, Grab weapon, Rewind time, Block, Slow motion and Pause can be remapped; saved to `controls.txt`.
  - **Screen size**: full screen (default) or original aspect (816x544).
  - **Screen filter**: Bilinear or Sharp bilinear (sharp is a fragment shader on the final blit).
  - **FPS counter** and **FPS limit** (off / 30).
  - **iPhone profile**: hw.machine values iPhone 2G/3G/3GS (default) and iPod touch 2G/3G; applies after a restart. It only affects a menu flag in this game, not graphics. *Not verified on console.*
  - Settings are saved to `ux0:data/princeofpersiawarriorwithin/options.txt`.
- **On-screen controls**: L + R hides or shows the touch controls. They are hidden at 0% opacity; the health/sands dial, held weapon and pause button stay visible, and hidden controls still respond to touch.
- **Pause and continue**: START pauses the game (taps the on-screen pause button) and resumes via CONTINUAR. CROSS or START also continue the "TOUCH THE SCREEN TO CONTINUE" screens on the title and level-loading screens.
- **Trophies**: 13 trophies + platinum (Time Twister, Time Blocker, Warrior's Revenge, Warrior's Wrath, Triumph of Time, The Master of Technique, Falcon, Watery Embrace, God of Sand, Blessings of Sand, Ravaging Tempest, Lightspeed Runner, Don't Look Back), mapped from the game's own achievements, including ones already earned in an existing save.

## 🐛 Fixes in v1.1.0

- **Intro video**: the Gameloft intro video (black screen before the title) plays again.

## ⚠️ Known Issues

- **Trophies**: the trophy set installs on first launch, but individual unlocks are not verified on console yet.
- **iPhone profile**: not verified on console; no graphics effect in this game.
- **L / Select** mappings have not been confirmed in game.
- **Cutscenes** play at ~14 FPS (software decoding; the hardware decoder fails to initialize).
- **Online (GLLive)** features are not supported.
- **UBISOFT / UBIMTL / LOGORP videos** do not exist in the iOS IPA either, so their absence is expected.

Installation and build instructions: see [README.md](README.md).

---

# Prince of Persia: Warrior Within (PS Vita) — Beta Release v1.0

First public beta of the native iOS-to-PS Vita wrapper port of *Prince of Persia: Warrior Within* (iOS v1.0.7). A custom Mach-O loader runs the original ARMv6 executable on the Vita. The game boots through intro videos and menus into gameplay at ~60 FPS, and is controllable with touch and physical buttons.

## 📦 Release Package Contents

- `princeofpersiawarriorwithin.vpk`: the loader (Title ID `PSVPOPWW1`, version `01.00`).
- No game files are included. `rebase.bin` is derived from the game executable, so you generate it yourself from your own copy (see below).

## 🛠️ What is rebase.bin?

The iOS executable is linked to run at its original addresses. On the Vita it is mapped at `0x98000000`, so every absolute pointer stored in `__DATA` must be relocated. `rebase.bin` lists those pointer locations: 12475 candidate words, 10204 of which get relocated at load. It is generated from your decrypted executable:

```bash
python3 -I tools/macho_prep.py Payload/<App>.app/POP rebase.bin
```

The tool also checks that the binary is decrypted (`cryptid 0`).

## 🚀 Player Installation Guide

1. **Requirements**: Enso 3.60/3.65, `ur0:tai/kubridge.skprx` under `*KERNEL`, and `libshacccg.suprx` in `ur0:data/`.
2. **Assets**: from your FairPlay-decrypted v1.0.7 `.ipa`, copy the contents of `Payload/<App>.app/` to `ux0:data/princeofpersiawarriorwithin/game_app/`. Put `rebase.bin` in `ux0:data/princeofpersiawarriorwithin/`.
3. **Install** `princeofpersiawarriorwithin.vpk` with VitaShell.

## 🎮 Controls

| Input | Action | Status |
| :---: | :--- | :---: |
| **Touchscreen** | Original on-screen virtual pad and buttons | ✅ Working |
| **Left stick / D-Pad** | Move | ✅ Working |
| **Right stick** | Camera | ✅ Working |
| **Cross** | Jump | ✅ Working |
| **Triangle** | Attack | ✅ Working |
| **Circle** | Grab | ✅ Working |
| **Square** | Drop weapon | ✅ Working |
| **Start** | Pause / menu | ✅ Working |
| **R** | Block | ✅ Working |
| **L** | Time rewind | ⚠️ Unverified |
| **L + R** | Show / hide the on-screen touch controls | ⚠️ Unverified |
| **Select** | Slow motion | ⚠️ Unverified |

Buttons can be remapped in `ux0:data/princeofpersiawarriorwithin/controls.txt` (`R1 = BLOCK`, `L1 = REWIND`, `CROSS = BIT0`...).

## 📋 Changelog (v1.0)

- **Loader**: Mach-O mapping, 2112 external relocations, rebase table, 53 global constructors, and a minimal ObjC/UIKit runtime (EAGLView, AppController, NSTimer-driven draw loop).
- **Graphics**: GLES 1.1 → vitaGL with an off-screen 320x480 framebuffer, rotated and stretched fullscreen to 960x544 (no black bars). A depth + stencil buffer makes the stencil-masked gameswf menus render. `GL_FIXED` vertex arrays are passed natively in `glDrawElements` (menu buttons were invisible before).
- **Framebuffer alpha**: iOS renders into an RGB565 renderbuffer (no alpha), so the alpha channel is now never written and is always cleared to 1. This matches iOS `GL_DST_ALPHA` blending.
- **Menus**: the 420/250-frame logo waits are skipped, since the fake UIImageViews draw nothing.
- **Input**: `_INO_Joystick_Update` is reimplemented 1:1, so the touch event queue is drained again. Touches are dispatched on the game thread. Physical buttons are ORed into the pad bits, and Block is on R (bit 5, the shield button: `VirtualButton` 11).
- **Cutscenes**: FFmpeg software decoding of the `.m4v` movies, AAC audio thread (with stack-size retry), skip with Cross/Circle/Start.
- **Character rendering**: vitaGL is built without `DRAW_SPEEDHACK`, which let the GPU read large client vertex arrays at scene end. The CPU-skinned Prince (one reused buffer) drew every body part with the last part's vertices: missing triangles and a moving "ghost".
- **Pause menu**: opening it no longer freezes the game for seconds (a debug dump of the Flash UI tree was written to the log on every menu change).
- **L / R**: the Vita shoulder buttons are `SCE_CTRL_LTRIGGER`/`RTRIGGER`; only the DS3/DS4 `L1`/`R1` codes were checked, so L and R did nothing.
- **Hooks / stack alignment**: every game-function hook now enters through `ml_align_call`. Picking up the first sword plays `UP_POWC.m4v` from a game event with a 4-byte aligned stack, which crashed in vitaGL (`vld1.64 [sp :64]`).
- **Block**: on R, using input bit 5 (the shield button, confirmed by touching it on console). The old bit 6 blocked nothing.
- **Toolchain**: built with the VitaSDK softfp toolchain so libm/libmathneon use the iOS float calling convention.

## ⚠️ Known Issues

- **Cutscenes** play at ~14 FPS (software decoding; the hardware decoder fails to initialize).
- **L / Select** mappings have not been confirmed in game.
- **Hiding the touch controls** (L + R) does not work yet: the controls stay visible. Physical buttons work regardless.
- **Online (GLLive)** features are not supported.

Installation and build instructions: see [README.md](README.md).
