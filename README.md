<h1 align="center">
  Prince of Persia: Warrior Within · PS Vita Port
</h1>

<p align="center">
  <a href="#-status--playability">Status & Playability</a> •
  <a href="#setup-instructions-for-players">How to install</a> •
  <a href="#controls">Controls</a> •
  <a href="#known-issues">Known Issues</a> •
  <a href="#build-instructions-for-developers">How to compile</a> •
  <a href="#credits--acknowledgments">Credits</a>
</p>

An unofficial, native **iOS-to-PS Vita wrapper port** of *Prince of Persia: Warrior Within* (iOS v1.0.7, originally developed by **Ubisoft / Gameloft**). The original 32-bit ARMv6 iOS executable runs unmodified on the Vita's CPU through a custom **Mach-O loader**. Native Vita code replaces the Apple frameworks it depends on: Darwin libc/pthreads, a minimal Objective-C/UIKit runtime, OpenGL ES 1.1 (on vitaGL), OpenAL, CoreAudio and the cutscene movie player (on FFmpeg).

---

## 🎮 Status & Playability

**Status: PLAYABLE (beta) (v01.00)**

On physical hardware the game boots through intro videos and menus into gameplay, and is fully controllable with touch and physical buttons.

- [x] **Mach-O Loader**: `__TEXT`/`__DATA` mapped at `0x98000000`, 2112/2112 external relocations, 10204 rebase words (from `rebase.bin`), 53 global constructors, 372 import thunks. *Verified on console.*
- [x] **Graphics**: OpenGL ES 1.1 fixed-function → vitaGL. The 480x320 iOS framebuffer is rendered off-screen and stretched fullscreen to 960x544 (no black bars; touch is remapped to match). ~60 FPS in-game. *Verified on console.*
- [x] **Menus (gameswf Flash UI)**: menus, language selection and tutorials render, including stencil-masked elements. *Verified on console.*
- [x] **Touch & Controls**: front touchscreen drives the on-screen virtual pad and buttons. Physical buttons and both analog sticks are mapped. *Verified on console.*
- [x] **Cutscenes**: `.m4v` movies decoded in software (FFmpeg) with AAC audio. They can be skipped with Cross, Circle or Start. *Verified on console.*
- [x] **Audio**: OpenAL 1.1 (sound effects) and CoreAudio (streams) mapped to native Vita audio.
- [x] **Saves**: the iOS `Documents/` folder is mapped to `ux0:data/princeofpersiawarriorwithin/saves/`.
- [x] **Performance**: CPU 444 MHz, bus 222 MHz, GPU 222 MHz. VFP short-vector matrix routines replaced with native code.
- [x] **Character rendering**: CPU-skinned models (the Prince) draw correctly; client vertex arrays are copied per draw. *Verified on console.*
- [ ] **Online features (GLLive)**: not supported.

---

## Disclaimer

**Prince of Persia** and **Prince of Persia: Warrior Within** are registered trademarks of **Ubisoft Entertainment**. The work presented in this repository is unofficial and is not produced, endorsed, or sanctioned by Ubisoft, Gameloft or any affiliated entities.

This software does NOT contain original game code, binaries, copyrighted graphics, or game assets. The authors do not promote or condone piracy in any way. To play, you must provide your own legally obtained, **decrypted** copy of the iOS game (`.ipa`, v1.0.7).

---

## Setup Instructions (For Players)

1. **CFW & Plugins**: PS Vita on Custom Firmware (**Enso 3.60 / 3.65** recommended) with **kubridge** in `ur0:tai/config.txt`:
   ```ini
   *KERNEL
   ur0:tai/kubridge.skprx
   ```
2. **Shader Compiler**: `libshacccg.suprx` must be in `ur0:data/` (extract it with **ShaRKBR33D**).
3. **Game Files**: use a FairPlay-decrypted `Prince of Persia - Warrior Within` **v1.0.7** `.ipa` (`cryptid 0`; encrypted binaries crash at load):
   - Unzip the `.ipa` and take the `Payload/*.app/` folder.
   - Generate `rebase.bin` from the executable:
     ```bash
     python3 -I tools/macho_prep.py Payload/<App>.app/POP rebase.bin
     ```
4. **Copy to the Vita** (`ux0:data/princeofpersiawarriorwithin/`):
   ```
   ux0:data/princeofpersiawarriorwithin/
     game_app/       contents of the .app folder (POP executable, .big/.MGM data, .m4v movies...)
     rebase.bin      rebase table generated in step 3
     controls.txt    optional — custom button mapping (see Controls)
     saves/          created automatically — your save games
     logs/           created automatically — one log per run
   ```
   `tools/stage_data.sh` builds this layout from the `.app` folder.
5. **Install VPK**: install `princeofpersiawarriorwithin.vpk` (Title ID `PSVPOPWW1`) with VitaShell.

---

## Controls

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

> [!TIP]
> Buttons can be remapped with `ux0:data/princeofpersiawarriorwithin/controls.txt`, one `BUTTON = ACTION` per line.
> Buttons: `CROSS SQUARE TRIANGLE CIRCLE L1 R1 SELECT START`. Actions: `JUMP ATTACK GRAB DROP BLOCK REWIND SLOWMO MENU`, or a raw input bit such as `BIT5`.
> ```
> R1 = BLOCK
> L1 = REWIND
> ```
> Blocking is always available through the on-screen shield button.

---

## Known Issues

- **Cutscene frame rate**: movies are decoded in software (the hardware decoder fails to initialize) and play at ~14 FPS.
- **L / Select mappings** have not been confirmed in game yet. L and R act after being held for 3 frames (~50 ms), so pressing L + R together never triggers a rewind or block.
- **Hiding the touch controls** (L + R) does not work yet: the on-screen controls stay visible.
- **Online features disabled**: the GLLive online services are not supported.
- **Crash dumps** are written by the system to `ux0:data/` (not to the game folder).

---

## Build Instructions (For Developers)

### Prerequisites

- [VitaSDK](https://vitasdk.org/) **softfp** toolchain (`$VITASDK`), since iOS code passes floats in integer registers. vitaGL is vendored in `vendor/vitaGL` and built automatically with `SOFTFP_ABI=1`.
- CMake 3.14+, Python 3 (for the tools).
- Your own decrypted `.ipa`, unpacked locally (`game_app` symlink → the `.app` folder). It is never committed (`.gitignore`).

### Compilation Commands

```bash
cd Prince-of-Persia-Warrior-Within-vita
./build.sh            # Release -> build/princeofpersiawarriorwithin.vpk
./build.sh Debug      # adds loader debug logging
./build.sh clean      # full rebuild
```

### Deploy & Debug

```bash
tools/stage_data.sh                               # build ux0_data/princeofpersiawarriorwithin/
python3 -I tools/deploy_data.py --ip <VITA_IP>    # incremental FTP upload (VitaShell FTP)
python3 -I tools/deploy_data.py --ip <VITA_IP> --logs   # fetch the run logs
```

Guest addresses in logs and crash dumps are `0x98000000 + Mach-O vmaddr` (minus `0x1000` for the file offset of `__TEXT`). Architecture, ABI rules (`r9`, softfp, 4-byte stack alignment) and progress notes: [`PORTING_PLAN.md`](PORTING_PLAN.md) and [`port_progress.md`](port_progress.md).

---

## Credits & Acknowledgments

- **Prince of Persia: Warrior Within** is intellectual property of **Ubisoft Entertainment**, built on Ubisoft's Jade engine. The iOS version was published by Gameloft.
- **vitaGL** graphics library by [Rinnegatamante](https://github.com/Rinnegatamante/vitaGL).
- **kubridge** plugin by [Bythos](https://github.com/bythos14/kubridge).
- **so-loader** architecture and [soloader-boilerplate](https://github.com/Rinnegatamante/soloader-boilerplate) by Rinnegatamante (inspiration for this Mach-O loader).
- **FFmpeg** for cutscene decoding, **OpenAL Soft** for audio (VitaSDK packages).
- Special thanks to the PS Vita homebrew and porting community.
