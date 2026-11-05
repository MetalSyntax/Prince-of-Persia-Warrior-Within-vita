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
| **Circle / Square** | Grab / drop weapon | ✅ Working |
| **Start** | Pause / menu | ✅ Working |
| **R1** | Block | ⚠️ Unverified |
| **L1** | Time rewind | ⚠️ Unverified |
| **Select** | Slow motion | ⚠️ Unverified |

Buttons can be remapped in `ux0:data/princeofpersiawarriorwithin/controls.txt` (`R1 = BLOCK`, `L1 = REWIND`, `CROSS = BIT0`...).

## 📋 Changelog (v1.0)

- **Loader**: Mach-O mapping, 2112 external relocations, rebase table, 53 global constructors, and a minimal ObjC/UIKit runtime (EAGLView, AppController, NSTimer-driven draw loop).
- **Graphics**: GLES 1.1 → vitaGL with an off-screen 320x480 framebuffer, rotated and scaled to 816x544. A depth + stencil buffer makes the stencil-masked gameswf menus render. `GL_FIXED` vertex arrays are passed natively in `glDrawElements` (menu buttons were invisible before).
- **Framebuffer alpha**: iOS renders into an RGB565 renderbuffer (no alpha), so the alpha channel is now never written and is always cleared to 1. This matches iOS `GL_DST_ALPHA` blending.
- **Menus**: the 420/250-frame logo waits are skipped, since the fake UIImageViews draw nothing.
- **Input**: `_INO_Joystick_Update` is reimplemented 1:1, so the touch event queue is drained again. Touches are dispatched on the game thread. Physical buttons are ORed into the pad bits, and Block is moved to R1 (bit 6).
- **Cutscenes**: FFmpeg software decoding of the `.m4v` movies, AAC audio thread (with stack-size retry), skip with Cross/Circle/Start.
- **Toolchain**: built with the VitaSDK softfp toolchain so libm/libmathneon use the iOS float calling convention.

## ⚠️ Known Issues

- **Prince model**: parts of the Prince are missing and his inner faces show through (partially see-through look, dark moving silhouette). The cause is still under investigation.
- **Cutscenes** play at ~14 FPS (software decoding; the hardware decoder fails to initialize).
- **R1 / L1 / Select** mappings have not been confirmed in game.
- **Online (GLLive)** features are not supported.

Installation and build instructions: see [README.md](README.md).
