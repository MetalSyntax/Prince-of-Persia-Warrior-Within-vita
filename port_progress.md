# Prince of Persia: Warrior Within (iOS) -> PS Vita Port Progress Report

> **Port Identifier:** `PSVPOPWW1`  
> **Source Platform:** iOS (32-bit ARMv6 Mach-O executable `POP`, v1.0.7)  
> **Target Platform:** Sony PlayStation Vita (`armv7-a`, vitaGL, newlib, vitasdk)  
> **Loader Base:** `ios-loader-boilerplate` (Static Mach-O loader with `MACHO_SLIDE = 0x98000000`)  
> **Build Artifact:** `build/princeofpersiawarriorwithin.vpk`  
> **Status:** **Ready for Physical Hardware Testing (Phases 1-14 Complete)**

---

## 1. Executive Summary & Engine Architecture

*Prince of Persia: Warrior Within* for iOS (bundle ID `com.gameloft.POP`) is an official iOS adaptation of Ubisoft's classic console action-adventure game, ported to iOS by Gameloft.

Through extensive disassembly and symbol inspection of the decrypted 11.2 MB executable `POP`, the internal architecture was confirmed to run on **Ubisoft's proprietary Jade Engine** (the engine behind the original *Sands of Time* trilogy and *Beyond Good & Evil*), wrapped within Gameloft's iOS presentation and input layer.

### Key Architectural Findings:
1. **Core Engine:**
   - Ubiquitous Jade Engine subsystems: Animation (`ANI_`), Game Object System (`OBJ_`), Events & Cutscenes (`EVE_`), World & Collision (`COL_`), Sound & Music (`SND_`), and Input Manager (`INO_`).
   - Pure C++ codebase statically compiled into the Mach-O binary.
2. **Dual-Threaded Rendering Loop:**
   - The application delegate `AppController` initializes the game state in `-[AppController applicationDidFinishLaunching:]`.
   - It invokes `_StartThread`, spawning a dedicated POSIX thread executing `__Z13ThreadRoutinePv` (`0x0000a374`).
   - This thread runs `_GameLoop` and issues `[EAGLContext presentRenderbuffer:]`. Concurrently, an `NSTimer` fires 60 FPS callbacks to `-[AppController draw]`.
3. **Graphics API:**
   - OpenGL ES 1.1 fixed-function pipeline with matrix stacks, vertex arrays (`GL_FIXED`), and PVRTC / CgBI PNG texture compression.
4. **Input Subsystem:**
   - Translates capacitive screen interactions via `CTouchScreen` (`PadTouchEvent`, `PadMoveEvent`, `PadUntouchEvent`) into high-level gamepad state stored in `TouchManager`.
   - The Jade Engine polls `_INO_Joystick_Update`, `_INO_b_Joystick_IsButtonDown`, and `_INO_Joystick_Move` to drive the Prince's actions.
5. **Audio Pipeline:**
   - OpenAL 1.1 positional sound effects alongside CoreAudio streaming for music and dialogue.

---

## 2. Phase-by-Phase Implementation Log

### Phase 1: Repository Setup and Tooling
- **Status:** **COMPLETED**
- **Details:**
  - Extracted decrypted IPA (`Prince of Persia - Warrior Within_v.1.0.7.ipa`) with FairPlay `cryptid = 0`.
  - Configured project directory structure with `game_app` pointing to `POP.app`.
  - Analyzed ARM Mach-O binary structure: 32-bit ARMv6, slice base `0x1000-0x8f7000`.
  - Executed `tools/macho_prep.py` to extract all absolute pointers into `ux0_data/princeofpersiawarriorwithin/rebase.bin` (12,475 rebase words across `__const`, `__data`, `__nl_symbol_ptr`, `__objc_*`, etc.).
  - Generated comprehensive symbol inspection report in `docs/macho_report.txt`.

### Phase 2: Decompilation and Import Inventory
- **Status:** **COMPLETED**
- **Details:**
  - Dumped all symbols (`decompiled/macho/symbols.txt`), frameworks (`frameworks.txt`), Objective-C runtime descriptors (`objc_classes.txt`), and unresolved imports (`imports.txt`).
  - Total imports analyzed: 464 unique external symbols.
  - Documented symbol classification in `docs/imports.md`:
    - **Darwin libc & math (215 symbols):** Standard memory (`malloc`, `free`, `memcpy`), string routines, POSIX file I/O (`fopen`, `fread`), and math functions mapped to newlib/Darwin shims (`source/darwin.c`).
    - **Pthreads (22 symbols):** Mutexes, condition variables, thread creation, and thread local storage mapped to PS Vita kernel threads (`source/darwin.c`).
    - **OpenGL ES 1.1 (40 symbols):** Renderbuffer storage, fixed-point math, texture environments, and draw elements mapped to vitaGL (`source/gles.c`).
    - **OpenAL 1.1 (27 symbols):** Context management, listeners, sources, and PCM buffers mapped to vitasdk `libopenal` (`source/openal.c`).
    - **Objective-C & CoreFoundation (75 symbols):** Message dispatch, runtime classes, bundle queries, strings, arrays, dictionaries, and autorelease pools (`source/objc.c`).
    - **Network, Analytics & StoreKit (25 symbols):** In-app purchases, Game Center, and telemetry stubbed out cleanly returning offline/success.

### Phase 3: Engine Analysis & Lifecycle Mapping
- **Status:** **COMPLETED**
- **Details:**
  - Identified App Delegate entry point: `-[AppController applicationDidFinishLaunching:]` located at Mach-O offset `0x1f504`.
  - Discovered secondary game thread created by `_StartThread` invoking `__Z13ThreadRoutinePv` (`0x0000a374`), which drives the internal engine tick and frame presentation.
  - Mapped resource bundle resolution: resources are located using `CFBundleCopyBundleURL` / `CFURLCopyFileSystemPath` and read via standard POSIX `fopen` against the bundle root (`game_app/`).

### Phase 4: Mach-O Loader and Objective-C Bootstrap
- **Status:** **COMPLETED**
- **Details:**
  - **Critical Mach-O Indirect Relocation Fix:** Discovered that in `source/macho.c`, `s->flags & 0xFF` for non-lazy and lazy symbol pointers contained 1,383 entries. 976 of these entries had `(sym->n_type & N_TYPE) == N_SECT`, pointing to internal C++ engine functions rather than external imports. Fixed the loader to calculate `MACHO_SLIDE + sym->n_value` (`| 1` for Thumb) for internal symbols, preventing almost 1,000 internal functions from being erroneously replaced by unimplemented stubs.
  - Implemented static Objective-C runtime in `source/objc.c`:
    - Defined 47 Objective-C classes and 4 metaclasses (`AppController`, `EAGLView`, `TouchManager`, `CTouchScreen`, `SoundManager`, etc.).
    - Bound `_OBJC_CLASS_$_*` and `_OBJC_METACLASS_$_*` data imports (`data = 1`).
    - Implemented `UIApplicationMain`, `objc_msgSendSuper2`, `objc_setProperty`, `objc_enumerationMutation`, `objc_begin_catch`, and `objc_end_catch`.
    - Implemented ring-buffered `NSString` and CoreFoundation string conversions (`CFStringGetCString`, `CFStringCreateWithBytes`).
    - Implemented `NSHomeDirectory()` pointing to `DOCS_PATH` (`ux0:data/princeofpersiawarriorwithin/saves/`).
    - Implemented `CFBundleCopyBundleURL` pointing to `BUNDLE_PATH` (`ux0:data/princeofpersiawarriorwithin/game_app/`).
  - Implemented static C++ initializers (`macho_run_inits()`) iterating through `__mod_init_func`.

### Phase 5: Graphics Pipeline (OpenGL ES 1.1 on vitaGL)
- **Status:** **COMPLETED**
- **Details:**
  - Integrated vendored `vitaGL` compiled with `SOFTFP_ABI=1`.
  - Implemented shims for all missing OpenGL ES 1.1 symbols:
    - Fixed-point functions: `g_glClearColorx`, `g_glTexEnvx`, `g_glAlphaFuncx`, `g_glFogx`, etc.
    - Matrix stack & vertex pointers: `glMatrixMode`, `glLoadIdentity`, `glMultMatrixf`, `glVertexPointer`, `glTexCoordPointer`.
    - Draw calls: `glDrawArrays`, `g_glDrawElements` (properly handling unsigned short and byte indices).
  - Virtual Framebuffer & Orientation:
    - Game renders to a 320x480 virtual surface.
    - Configured rotation and scaling in `source/gles.c` to display on the PS Vita 960x544 native OLED/LCD screen.
    - Aspect ratio preserved (3:2) with an `816x544` centered viewport (`DST_X = 72`).
  - Implemented Apple CgBI PNG texture decrusher in `source/cgbi.c`.

### Phase 6 & 7: Touchscreen and Physical Controls
- **Status:** **COMPLETED**
- **Details:**
  - **Direct Engine Hooking:**
    - Disassembled `CTouchScreen` methods: `PadTouchEvent`, `PadMoveEvent`, `PadUntouchEvent`.
    - Routed Vita front capacitive touchscreen events directly into `CTouchScreen` in `source/patch.c`.
  - **Native Physical Controls Injection:**
    - Hooked `_INO_Joystick_Update` to poll `SceCtrlData` and inject digital buttons into the Jade Engine:
      - **Cross:** Jump / Confirm
      - **Square:** Primary Attack / Strike
      - **Triangle:** Grab / Secondary Weapon / Action
      - **Circle:** Block / Roll / Cancel
      - **L1 / L Trigger:** Rewind Time (Dagger of Time)
      - **R1 / R Trigger:** Wall-run / Special Action
      - **Start:** Pause Menu
      - **Select:** Slow Motion / Eye of the Storm
    - Hooked `_INO_Joystick_Move`: Mapped Left Analog Stick and D-Pad with configurable deadzones (default 8) to Prince movement.
    - Hooked `_INO_Joystick_GetButtonX` / `_INO_Joystick_GetButtonY`: Mapped Right Analog Stick to camera rotation.
    - Added user-customizable mapping parser for `ux0:data/princeofpersiawarriorwithin/controls.txt`.

### Phase 8: Audio Subsystem (OpenAL 1.1 & CoreAudio)
- **Status:** **COMPLETED**
- **Details:**
  - Created `source/openal.c` providing all 27 OpenAL 1.1 API entry points.
  - Added SoftFP wrapper attributes (`__attribute__((pcs("aapcs")))`) for all float-based OpenAL functions (`alBufferData`, `alSourcef`, `alListenerf`, `alSourcefv`, `alListenerfv`, etc.).
  - Registered `openal_imports` in `source/imports.c` and linked `libopenal` in `CMakeLists.txt`.
  - Resolved linker dependency for recording stubs by adding `SceAudioIn_stub` to `target_link_libraries`.
  - CoreAudio / AudioSession audio session initialization hooked to prevent mute states.

### Phase 9: Video Playback & FMV
- **Status:** **COMPLETED**
- **Details:**
  - Implemented `MPMoviePlayerController` Objective-C shims in `source/objc.c` (`setMovieSourceType:`, `play`, `stop`).
  - Added graceful fallback so missing or skipped cutscenes immediately return control to the game loop without black screen lockups.

### Phase 10: Assets, Storage Redirection and Staging
- **Status:** **COMPLETED**
- **Details:**
  - Redirected bundle resource accesses to `ux0:data/princeofpersiawarriorwithin/game_app/`.
  - Redirected savegames and persistent documents to `ux0:data/princeofpersiawarriorwithin/saves/`.
  - Configured rotating logging system writing to `ux0:data/princeofpersiawarriorwithin/logs/princeofpersiawarriorwithin_NNN.log`.
  - Staged complete bundle data using `tools/stage_data.sh`:
    - Cleaned and copied executable `POP` and resources into `ux0_data/princeofpersiawarriorwithin/game_app/`.
    - Generated `rebase.bin` (12,475 pointers).
    - Created `saves/` and `logs/` directories.

### Phase 11: Performance & Clock Configuration
- **Status:** **COMPLETED**
- **Details:**
  - Boosted Vita hardware clocks in `source/main.c` at startup:
    - CPU: 444 MHz
    - GPU: 222 MHz
    - Bus: 222 MHz
    - XBAR: 166 MHz
  - Memory block allocation configured with `SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX` for fast executable execution.

### Phase 12 & 13: Configurator and Trophies
- **Status:** **COMPLETED**
- **Details:**
  - Externalized control settings in `controls.txt`.
  - GameKit achievements and leaderboards stubbed with safe no-op responses.

### Phase 14: LiveArea Assets and VPK Packaging
- **Status:** **COMPLETED**
- **Details:**
  - Created and validated 8-bit indexed PNG LiveArea assets:
    - `icon0.png` (128x128, 2.7 KB)
    - `pic0.png` (960x544, 8.8 KB)
    - `bg0.png` (840x500, 43.7 KB)
    - `startup.png` (280x158, 4.7 KB)
    - `template.xml` (LiveArea configuration)
  - Executed `psvita-toolkit livearea --validate` -> All checks passed.
  - Successfully compiled the executable `eboot.bin` and packaged `build/princeofpersiawarriorwithin.vpk` (4,417,143 bytes) with Title ID `PSVPOPWW1`.

---

### Phase 15: Hardware Debugging & Log Iteration
- **Status:** **IN PROGRESS (Iteration 002 Resolved)**
- **Details:**
  - **Log `princeofpersiawarriorwithin_002.log` Analysis:**
    - Error: `[FATAL] sceKernelAllocMemBlockForVM(image) = 0x80024B05` (`SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_SIZE`).
    - Cause: `macho.c` previously invoked `sceKernelAllocMemBlockForVM` for a contiguous 9.4 MB image block (`0x008F7000` bytes). The Vita OS VM domain allocator does not support allocating fixed base addresses or arbitrary large chunks, returning size error `0x80024B05`. Furthermore, `MACHO_SLIDE` (0x98000000) was bypassed.
  - **Implemented Fixes:**
    1. **Fixed-Base Allocation via `kuKernelAllocMemBlock`:** Switched to the established `kuKernelAllocMemBlock` approach with `HAS_BASE` (`opt.attr = 0x1`), mapping `macho_text` (`0x98001000`, `RX`), `macho_data` (`0x9869C000`, `RW`), and `macho_thunks` (`0x988F7000`, `RX`) at their exact virtual addresses.
    2. **Dual-Mode (Thumb-2 & ARM) `macho_hook`:** Engine functions like `_INO_Joystick_Update` and `_INO_Joystick_Move` are compiled in Thumb mode. Fixed `macho_hook` to inject a proper Thumb-2 literal load (`0xF000F8DF`) with alignment padding, preventing undefined instruction aborts.
    3. **libstdc++ Symbol Resolution (`cxx_imports.c`):** Extracted all 72 mangled C++ symbols (e.g. `std::string`, `_Rb_tree`, `_List_node_base`, `_Znwm -> _Znwj`, `_Znam -> _Znaj`) and mapped them directly to native VitaSDK `libstdc++.a`.
    4. **Automated Clean Build Pipeline (`build.sh`):** Configured out-of-tree building in `/tmp/pop-build` via symlink to avert path whitespace issues with `vita-pack-vpk`.
  - Built updated `build/princeofpersiawarriorwithin.vpk` ready for console testing.

---

## 3. Current Build Status

| Component | Status | Details |
|---|---|---|
| **Loader Executable (`eboot.bin`)** | **BUILT** | Fixed-base `kuKernelAllocMemBlock` at `0x98000000`, `-ffixed-r9`, SoftFP |
| **Vendored vitaGL (`libvitaGL.a`)** | **BUILT** | Compiled with `SOFTFP_ABI=1` |
| **Mach-O Relocator & Symbol Table** | **READY** | 464 iOS imports + 72 libstdc++ symbols + 976 internal indirect symbols |
| **Package (`princeofpersiawarriorwithin.vpk`)** | **BUILT** | `build/princeofpersiawarriorwithin.vpk` (1.9 MB) |
| **Staged Data (`ux0_data/`)** | **READY** | Staged in `ux0_data/princeofpersiawarriorwithin/` |
| **Physical Testing (Phase 15)** | **IN PROGRESS** | Ready for iteration 007 deployment via VitaShell FTP |

---

### Hardware Test Iteration History

#### Iteration 002 (Crash in `macho_load`):
- **Symptom:** `sceKernelAllocMemBlockForVM(image) = 0x80024B05` (Fixed Virtual Address allocation rejected by kernel).
- **Fix:** Migrated executable and data mappings to `kuKernelAllocMemBlock` via `kubridge.skprx` with `HAS_BASE` (`opt.attr = 0x1`).

#### Iteration 003 (Crash in `ThreadRoutine` at `strcpy+0xa4`):
- **Log / Core Dump:** `princeofpersiawarriorwithin_003.log` and `princeofpersiawarriorwithin-psp2core-1793140010-0x000efb2783-eboot.bin.psp2dmp`.
- **Root Cause:**
  1. `ui_UIApplicationMain` passed the raw `Class` metadata pointer (`app_ctrl_class`) instead of an allocated instance to `applicationDidFinishLaunching:`.
  2. `objc_msgSend` lacked guest class/metaclass method dispatching, so `[AppController initDisplay]` and `[AppController sharedInstance]` returned `nil`.
  3. The movie playback / initialization thread dereferenced the returned pointer and invoked `strcpy(dest, NULL)`, causing a Data Abort exception.
- **Fix (Iteration 004):**
  1. Implemented full guest Objective-C 2.0 class and metaclass dynamic registration (`objc_register_classes`) reading `__objc_classlist` directly from Mach-O memory.
  2. Implemented instance allocation `objc_class_alloc` and dynamic message dispatch in `objc_msgSend_impl` and `objc_msgSend_stret`.
  3. Added Foundation/UIKit selectors: `performSelectorOnMainThread:withObject:waitUntilDone:`, `isEqualToString:`, `isCurrentContext`, and `sharedInstance` fallback.
  4. Added defensive string guards in `source/darwin.c` (`d_strcpy`, `d_strncpy`, `d_strcmp`, `d_strlen`, `d_strcat`) protecting against `NULL` pointer crashes.

#### Iteration 004 (Crash in `_fread_r` at `ldrsh.w r3, [r4, #12]` with `r4 = NULL`):
- **Log / Core Dump:** `princeofpersiawarriorwithin_004.log` and `princeofpersiawarriorwithin-psp2core-1793141627-0x000e3b2263-eboot.bin.psp2dmp`.
- **Root Cause:**
  1. Section name matching in `macho.c` used `!strcmp(s->sectname, "__objc_classlist")`. Since `sectname` is a 16-character array without a null terminator when 16 bytes long, `strcmp` compared against `__objc_classlist__DATA...` and failed, so `objc_register_classes` was not invoked.
  2. `NSSearchPathForDirectoriesInDomains` returned `dummy_obj` instead of a simulated array, causing `[paths objectAtIndex:0]` to return `nil`.
  3. The engine attempted `fopen("")` which returned `NULL`, and then executed `fread(buf, 1, 4, NULL)` which dereferenced `r4 = 0x0` in newlib `_fread_r`.
- **Fix (Iteration 005):**
  1. Fixed section name check to use `!strncmp(s->sectname, "__objc_classlist", 16)`, properly triggering `objc_register_classes` for all 9 classes and metaclasses.
  2. Implemented `ns_NSSearchPathForDirectoriesInDomains` returning a valid `docs_arr` containing `DOCS_PATH`.
  3. Implemented Foundation array & path selectors: `objectAtIndex:`, `objectAtIndexedSubscript:`, `lastObject`, `firstObject`, `stringByAppendingPathComponent:`, `stringByAppendingString:`, `pathExtension`, `lastPathComponent`, `stringByDeletingLastPathComponent`, `stringByDeletingPathExtension`, and `count = 1`.
  4. Added defensive stdio guards in `source/darwin.c` (`d_fread`, `d_fwrite`, `d_fseek`, `d_ftell`, `d_fclose`, `d_feof`, `d_ferror`, `d_fflush`, `d_fgetc`, `d_fgets`, `d_fputs`) protecting against `stream == NULL` crashes.

#### Iteration 005 (Stack overflow in `objc_msgSendSuper2` & Uninitialized `__TEXT` reading):
- **Log / Core Dump:** `princeofpersiawarriorwithin_005.log` and `princeofpersiawarriorwithin-psp2core-1793143394-0x000aec2fe9-eboot.bin.psp2dmp`.
- **Root Cause:**
  1. `objc_register_classes` was called before `kuKernelCpuUnrestrictedMemcpy` copied `text_buf` to `0x98001000`, causing class and method names in `__TEXT` (`__cstring`) to be read from uninitialized memory (producing corrupted names in logs).
  2. `objc_msgSendSuper2` dispatched back into `objc_msgSend_impl(receiver, sel, ...)`. For `[super initWithFrame:]` inside `-[EAGLView initWithFrame:]`, this caused infinite recursion into `-[EAGLView initWithFrame:]` until the main thread stack hit the `0x81701000` boundary, crashing with a Data Abort exception.
- **Fix (Iteration 006):**
  1. Relocated `objc_register_classes` after `kuKernelCpuUnrestrictedMemcpy` and cache flush in `source/macho.c`, ensuring all class and selector strings in `__TEXT` are fully written before registration.
  2. Separated `objc_msgSend_system_impl` from `objc_msgSend_impl` in `source/objc.c`. Routed `objc_msgSendSuper2` directly to `objc_msgSend_system_impl` so superclass calls bypass the receiver's own method table and dispatch cleanly to system shims without recursion.
  3. Added UIKit properties: `contentScaleFactor`, `isOpaque`, `setClipsToBounds:`, `setOpaque:`, `setContentScaleFactor:`, `setAutoresizingMask:`, `setClearsContextBeforeDrawing:`.

#### Iteration 006 (Missing assets due to `darwin_path` bundle redirect & C++ exception in `__cxa_throw`):
- **Log:** `princeofpersiawarriorwithin_006.log` (Clean exit without core dump).
- **Achievements:**
  1. All 9 Objective-C guest classes and metaclasses registered with correct names and method counts.
  2. `-[AppController applicationDidFinishLaunching:]` ran and finished successfully (`delegate ok`).
  3. OpenAL 1.1 audio context created and made current.
- **Root Cause:**
  1. The Jade Engine requested relative asset paths: `"text.EN.lang"`, `"POP5Common.MGM"`, `"soundpc.big"`. `darwin_path` mapped all paths to `DATA_PATH` (`ux0:data/princeofpersiawarriorwithin/`), whereas game assets were staged inside `game_app/` (`BUNDLE_PATH`).
  2. Because the game data archives could not be opened, the Jade Engine threw a C++ exception which reached `__cxa_throw` $\rightarrow$ `fatal_error`.
  3. `setDrawableProperties:` and `NSDictionary` dictionary constructors were unhandled in `objc_msgSend`.
- **Fix (Iteration 007):**
  1. Updated `darwin_path` in `source/darwin.c` to default relative game assets to `BUNDLE_PATH` (`game_app/`), routing `saves` and `logs` to `DATA_PATH`.
  2. Added bidirectional search fallback in `d_fopen` between `BUNDLE_PATH` and `DATA_PATH`.
  3. Wrapped `remove`, `unlink`, and `rename` with `darwin_path`.
  4. Added handlers for `setDrawableProperties:`, `drawableProperties`, `dictionaryWithObjectsAndKeys:`, and `objectForKey:` in `source/objc.c`.
  5. Enhanced `d_cxa_throw` with Itanium ABI type name and `what()` inspection.

#### Iteration 008 (Prefetch abort exception at `PC = 0x0`, `LR = 0x98071638` in `__Z8SM_Setupv`):
- **Log / Core Dump:** `princeofpersiawarriorwithin_008.log` and `princeofpersiawarriorwithin-psp2core-1793146278-0x0006c42e2d-eboot.bin.psp2dmp`.
- **Root Cause:**
  1. The dual-threaded rendering loop launched successfully and executed `_Mac_Flip` (`0x984f3994`).
  2. `_Mac_Flip` called `SM_Setup()` (`0x9807161c`).
  3. `SM_Setup()` loaded global instance `_g_MacDebugStatus` (`0x986cdf08`) and attempted to query `g_MacDebugStatus->Get_ShowStatusInfo()` via its C++ virtual table: `ldr pc, [r3, #424]`.
  4. The Mach-O loader (`source/macho.c`) only processed indirect symbols (`S_NON_LAZY_SYMBOL_POINTERS` / `S_LAZY_SYMBOL_POINTERS`), but completely ignored Mach-O external relocations (`dysym->extreloff` / `dysym->nextrel`, 2,112 entries).
  5. Because external relocations were ignored, all 2,112 pointers across `__DATA,__const` (including the entire virtual method tables for `MacDebugStatus`, `CDebugStatus`, `CDebugDisplayInterface`, and C++ RTTI typeinfo pointers) remained `0x00000000`.
  6. Dereferencing `[r3, #424]` jumped directly to address `0x0`, triggering a prefetch abort CPU exception.
- **Fix (Iteration 009):**
  1. **Full Mach-O External Relocation Support (`source/macho.c`):**
     - Implemented parsing and application of all `dysym->nextrel` entries from `dysym->extreloff`.
     - Internal exported symbols (`(sym->n_type & N_TYPE) == N_SECT`) are resolved to `MACHO_SLIDE + sym->n_value` (with Thumb bit 1 preserved).
     - External symbols (`N_UNDF`) are resolved through `imports_lookup()`, binding data pointers or wrapping functions via thunks.
     - Preserved addends (e.g. `+8` for C++ Itanium ABI `type_info` vptrs) by adding the resolved symbol value to the slot's existing content (`*slot += sym_val`).
  2. **Thunk Deduplication Cache:** Added `get_or_make_thunk()` cache so repeated external relocations to the same import (e.g. 430 references to `___cxa_pure_virtual`) share a single thunk, conserving thunk arena space.
  3. **Symbol Table Aliases (`source/objc.c`):** Added explicit 2-underscore and 3-underscore symbol aliases for `__objc_empty_cache`, `__objc_empty_vtable`, `___CFConstantStringClassReference`, and `_OBJC_EHTYPE_id`, ensuring 100% of all 2,112 relocations resolve cleanly.
  4. **Tooling Update (`tools/macho_prep.py`):** Differentiated internal vs external symbols in `extrel` inspection to keep import inventories accurate.

#### Iteration 009 (Data abort exception in `sincosf_c` via `glRotatef`):
- **Log / Core Dump:** `princeofpersiawarriorwithin_009.log` and `princeofpersiawarriorwithin-psp2core-1793237617-0x000a6d243d-eboot.bin.psp2dmp`.
- **Root Cause:**
  1. All 2,112 Mach-O external relocations succeeded (`Applied 2112 / 2112 external relocations`). The rendering thread moved past `SM_Setup` into the Jade Engine 3D pipeline and invoked `glRotatef` / `glRotatex`.
  2. `glRotatef` in `vendor/vitaGL` called `matrix4x4_rotate`, which calls `sincosf_c(rad, cs)` from VitaSDK's `libmathneon.a`.
  3. `vitaGL` was compiled with `SOFTFP_ABI=1` (`-mfloat-abi=softfp`), where `float rad` passes in integer register `r0`, and `float cs[2]` (pointer) passes in `r1`.
  4. However, `sincosf_c` in VitaSDK's `libmathneon.a` is compiled as HardFP (`aapcs-vfp`), expecting `rad` in VFP register `s0` and the output destination pointer in `r0`.
  5. `sincosf_c` executed `vst1.32 {d16}, [r0]` with `r0 = 0x3fc90fdb` (the IEEE 754 float value for 1.570796 rad / $\pi/2$), attempting to write to an invalid address and causing a Data Abort exception.
- **Fix (Iteration 010):**
  1. **Calling Convention Declaration (`vendor/vitaGL/source/utils/math_utils.h`):** Explicitly declared `VGL_HARDFP void sincosf_c(float x, float r[2])` and `VGL_HARDFP float tanf_neon(float x)` with `__attribute__((pcs("aapcs-vfp")))` under `HAVE_SOFTFP_ABI`. GCC now emits the correct register moves (`vmov s0, r0; mov r0, r1`) before invoking `sincosf_c`, ensuring `r0` contains the valid stack pointer destination.
  2. **Comprehensive Foundation / UIKit Selectors (`source/objc.c`):** Implemented missing Objective-C selectors logged in 009: `initWithUTF8String:`, `initWithString:`, `initWithFormat:`, `numberWithBool:`, `sharegroup`, `initWithAPI:sharegroup:`, `sharedAccelerometer`, `currentLocale`, `uniqueIdentifier`, `setDateFormat:`, `stringFromDate:`, `fileExistsAtPath:`, `stringByReplacingOccurrencesOfString:withString:`, and `detachNewThreadSelector:toTarget:withObject:`.

### Iteration 010: Save Directory Path Normalization & Safeguards
- **Status:** **RESOLVED**
- **Hardware Symptoms:**
  - `sincosf_c` Data Abort is completely gone; game progressed into save management and main game boot (`CMainGame::StartGame`).
  - Log 010 showed:
    ```
    [ERROR] objc_msgSend(0x813074b0, uppercaseString) -> nil
    [INFO] NSLog: (nil)
    [WARN] strcpy(dest=0x8e000e0e, src=NULL) called! Protected from crash.
    [ERROR] objc_msgSend(0x813066f8, setUpdateInterval:) -> nil
    [WARN] fopen(ux0:data/princeofpersiawarriorwithin/saves//.\.\save\record.dat, w) -> NULL
    ```
  - Followed by Data Abort exception in thread `'pthread'` at `PC = 0x9800aed8`, `LR = 0x9802ac74`, `R3 = 0x00000000`.
- **Root Cause Analysis:**
  1. **Path Normalization:** The game constructed paths by concatenating `DOCS_PATH` with relative DOS-style paths: `ux0:data/.../saves//.\.\save\record.dat`. Because `darwin_path` saw `ux0:`, it bypassed all path normalization, leaving DOS backslashes (`\`), double slashes (`//`), and relative artifacts (`./`). On PS Vita, SCE file I/O treats `\` literally as part of the filename, causing file creation to fail.
  2. **Missing Subdirectory on Vita Filesystem:** When opening `.../saves/save/record.dat` in `"w"` mode, `fopen` failed with `ENOENT` because the `save/` directory did not exist.
  3. **Null Dereference in `CMainGame::StartGame()`:** `_MACHD_CreateRecord` attempted `fopen(..., "w")`. Because `fopen` returned `NULL`, it skipped allocating the 372-byte `GameRecord` structure and exited without setting `_s_GameRecord` (`0x986b4b8c`), leaving it as `NULL`. Immediately afterward, `StartGame` attempted to read the language byte via `ldrsb r1, [r3]` where `r3 = *_s_GameRecord == 0x0`, causing the crash.
  4. **Missing Objective-C Selectors:** `uppercaseString` and `setUpdateInterval:` returned `nil`.
- **Fix (Iteration 011):**
  1. **Full Path Normalization (`source/darwin.c`):** Updated `darwin_path` to replace all backslashes `\` with `/`, collapse redundant slashes and `/./` sequences, route `save/`, `Profile`, `record`, and `Save` to `DOCS_PATH`, and normalize even absolute `ux0:` paths.
  2. **Auto-Create Parent Directories (`source/darwin.c`):** Added `ensure_parent_dirs` in `d_fopen` whenever opening for write (`"w"`, `"a"`, `"+"`), recursively creating missing subdirectories like `saves/save/`.
  3. **Pre-Create Save Subdirectories (`source/main.c`):** Added `sceIoMkdir(DOCS_PATH "save", 0777)` and `sceIoMkdir(DATA_PATH "game_app/save", 0777)` at boot.
  4. **`_s_GameRecord` Safeguard (`source/patch.c`):** Initialized `_s_GameRecord` to a default static 384-byte buffer (with language 0 = English) at boot so that even in failure cases, `*_s_GameRecord` is never NULL and cannot cause a Data Abort.
  5. **Selectors (`source/objc.c`):** Implemented `uppercaseString` and `setUpdateInterval:`.

### Iteration 012: VFP Short Vector Mode Crash Fix & FFmpeg Video Playback Subsystem
- **Status:** **RESOLVED**
- **Hardware Symptoms:**
  - After fixing save directory creation and `_s_GameRecord`, the game booted past `CMainGame::StartGame` into 3D scene initialization.
  - An immediate crash occurred with an Undefined Instruction exception (`0x30002`):
    - `princeofpersiawarriorwithin-psp2core-1793239656-0x001d792fd9-eboot.bin.psp2dmp`
    - `PC: 0x980af2c8`
    - `LR: 0x980a7c60`
    - `CPSR: 0x60000010` (ARM mode)
  - Also, FMV cutscene playback (`.m4v` movies) was not yet implemented on PS Vita.
- **Root Cause Analysis:**
  1. **ARMv6 VFPv2 Short Vector Mode (`0x30002`):**
     - Disassembly of the crash site at `0x980af2c8` (`Matrix4Vector3Mul`, `__Z17Matrix4Vector3MulPKfS0_Pf`) and adjacent functions `Matrix3Mul` (`0x980af23c`) and `Matrix4Mul` (`0x980af1b8`):
       ```arm
       fmxr fpscr, r3   ; Sets FPSCR.LEN = 2 (vector length 3, 0x20000)
       fmuls s8, s0, s24 ; Short vector multiply!
       ```
     - In ARMv6 (e.g. iPhone 3G / original iPhone), VFPv2 supported hardware "short vector mode" controlled by `FPSCR.LEN` and `FPSCR.STRIDE`.
     - In ARMv7-A (Cortex-A9 on PlayStation Vita), non-zero `FPSCR.LEN` is deprecated and execution of short vector instructions generates an immediate **Undefined Instruction exception (`0x30002`)**.
  2. **Cutscene Video Decoding Requirements:**
     - Cutscene video files in `game_app/*.m4v` are encoded in two distinct formats:
       - MPEG-4 Part 2 (`mp4v`), e.g., `GAMELOFT.m4v` (320x176), `UBISOFT.m4v`.
       - H.264 Baseline (`avc1`), e.g., `CINE0001.m4v` (480x272).
       - Audio tracks are AAC (`mp4a`).
     - Vita hardware video decoders (`SceVideodec`/`SceAvPlayer`) only decode H.264 and fail completely on `mp4v`.
     - VitaSDK's `libavformat.a` lacks `mov.o` (an upstream portlibs issue), so standard `avformat_open_input` cannot demux MP4/M4V files.
- **Fix (Iteration 012):**
  1. **Scalar Matrix Math Replacements (`source/patch.c`):**
     - Implemented standard IEEE 754 scalar C replacements for `Matrix4Vector3Mul`, `Matrix3Mul`, and `Matrix4Mul`.
     - Hooked all three functions both by Mach-O export names and absolute virtual addresses (`0x980af1b8`, `0x980af23c`, `0x980af2a8`) using `macho_hook_addr`.
     - Cleared `FPSCR.LEN` and `FPSCR.STRIDE` bits to zero on thread entry.
  2. **Complete In-Engine FFmpeg Video Subsystem (`source/video.cpp`, `source/video.h`):**
     - Implemented a lightweight, robust ISO-BMFF (MP4/M4V) demuxer that directly parses MP4 box structures (`moov`, `mvhd`, `trak`, `tkhd`, `mdia`, `mdhd`, `hdlr`, `minf`, `stbl`, `stsd`, `stts`, `stsc`, `stsz`, `stco`, `co64`).
     - Extracts extradata (`esds` for MPEG-4 Part 2, `avcC` for H.264) and feeds raw sample packets directly into FFmpeg `libavcodec`.
     - Decoding thread: multi-frame pipeline decoding `AV_CODEC_ID_MPEG4` or `AV_CODEC_ID_H264` via `avcodec_send_packet` / `avcodec_receive_frame`.
     - High-speed ARM NEON YUV420P to RGB565 color space converter (`neon_yuv420p_to_rgb565`).
     - Dedicated audio thread: decodes AAC audio via `libavcodec` + `libswresample`, outputting 44.1kHz 16-bit stereo PCM to `SceAudioOut`.
     - Video presentation: renders scaled, aspect-correct quads on the PS Vita 960x544 screen via vitaGL.
     - Interactive controls: cutscenes can be skipped at any time with Cross, Start, or Circle (`SCE_CTRL_CROSS`, `SCE_CTRL_START`, `SCE_CTRL_CIRCLE`).
     - Engine Hooks: hooked `_StartMovieOnMainThread`, `_LoadMovie`, `_PlayMovie`, `_StopMovie`, and `_IsMoviePlaying`.

- **Iteration 013 (verificación del crash 011, sin corrida de consola todavía):**
  - Escaneo completo de `__TEXT`: solo hay 6 `vmsr fpscr` (entrada/salida de `Matrix4Mul`, `Matrix3Mul`, `Matrix4Vector3Mul`), así que los hooks de 012 cubren todo uso de VFP short-vector. La semántica de los hooks se cotejó contra el desensamblado (LEN=3, banco s0-s7 escalar). Es la misma solución que usa Blades of Fury.
  - `LoadMovie`/`StartMovieOnMainThread` reciben `const char*` (el juego hace `initWithUTF8String:`): los hooks de vídeo son correctos.
  - Fix `objc.c` `fileExistsAtPath:`: declaraba `darwin_path` con 1 argumento (la real tiene 3) → escribía en un puntero basura. Ahora usa `stat()` (sirve también para directorios).
  - Agregado `writeToFile:atomically:[encoding:error:]` (antes `-> nil` en el log 011).
- **Iteration 014 (log 013 + dump 1793306981):**
  - Crash: data abort en vitaGL `_glDrawElements_FixedFunctionIMPL` leyendo índices en NULL, desde `PAG_RenderManager::Render` (quad IBO, `glDrawElements(GL_TRIANGLES, 0x78, GL_UNSIGNED_SHORT, 0)`). Causa: `MacIndexBuffer::Init` hace `glGenBuffers` → `glGetError()`; si no es 0 no llama a `glBufferData` pero igual bindea el IBO → `vbo->ptr == NULL`. Fix: `g_glGetError` en `gles.c` loguea el error de vitaGL y devuelve `GL_NO_ERROR`.
  - Vídeos: `resolve_video_path` buscaba rutas Android; ahora prueba primero `BUNDLE_PATH` (`game_app/`). El libavcodec de vitasdk no trae decoder H.264 (solo mpeg4/aac) → los `CINE*.m4v` (avc1) van por `source/video_avc.c` (backend HW `sceAvcdec` + AAC FFmpeg, copiado de Zombie-Infection-vita). `GAMELOFT.m4v` (mp4v) sigue por el decoder software de `video.cpp`. `UBISOFT/UBIMTL/LOGORP.m4v` no existen en el IPA: el "not found" es esperado.
- **Iteration 015 (log 015 + dump 1793307721):** el fix de 014 funcionó (pasa el render del IBO, `Gameloft.m4v` se abre y decodifica).
  - Crash: `gameswf::hash<tu_stringi,as_value>::set_raw_capacity` pidió `1<<22` entradas → `SwfAlloc` NULL. El hash del dump estaba sano (`entry_count=6, size_mask=7` → debía pedir 16). Causa: `darwin.c` declaraba los `host_*` de libm con `pcs("aapcs-vfp")`, pero la libm linkeada es softfp (sin `Tag_ABI_VFP_args`) → `log()` leía basura. Afectaba a TODAS las funciones matemáticas del juego. Fix: quitar el atributo.
  - `fopen` en lectura que no encuentra el archivo (sondeo de saves/perfiles en primer arranque) pasa a `l_debug`; los fallos en escritura siguen como WARN.
  - Pendiente (siguiente corrida): vídeo mp4v a 1.2 fps con imagen negra (readback 16,16,16) y `could not start the cutscene audio thread`.
- **Iteration 016 (log 016 + dump 1793308519):** crash en vitaGL `glRotatef` → `sincosf_c` (libmathneon) con R0 basura.
  - Causa raíz común con 015: los VPK de 015/016 se compilaron en el Mac, que tiene **vitasdk-softfp** (`--with-float=softfp`: libm y libmathneon softfp). El código asumía el vitasdk estándar de WSL (libs hardfp) y forzaba `pcs("aapcs-vfp")` en `darwin.c` (`HARDFP`) y en vitaGL (`VGL_HARDFP`). El "fix" de 015 (quitar HARDFP) solo valía para el Mac y rompía WSL.
  - Fix: `CMakeLists.txt` detecta el toolchain (`gcc -v` → `with-float=softfp`) y define `VITASDK_SOFTFP_LIBS` + pasa `SOFTFP_LIBS=1` a vitaGL; `HARDFP`/`VGL_HARDFP` quedan vacíos solo en ese caso. `SOFTFP_ABI=1` se mantiene (el wrapper `sceGxmSetViewport_sfp` es por el ABI del SO, que siempre es hard).
  - Probable que la imagen negra del vídeo venga de lo mismo (las matrices de vitaGL usan math_neon con `VGL_HARDFP`): verificar en la próxima corrida.

- **Iteration 017 (estática, sin corrida de consola; auditoría con `tools/decomp_map.py`):**
  - Bug: `darwin.c` exportaba `V(__DefaultRuneLocale)` = `___DefaultRuneLocale`, pero el juego importa `__DefaultRuneLocale` (C `_DefaultRuneLocale`). El lookup es exacto → caía en un thunk "unimpl" (código) y las macros ctype inline de Darwin (`__runetype[c] & mask`) leían instrucciones como tabla. Corregido. También se enlaza `___objc_personality_v0` (último import sin resolver). Ahora 0 imports sin resolver.
  - `objc.c`: auditados los 220 selectores que el juego manda a clases del sistema (`__objc_selrefs` − métodos de clases guest). Agregados `stringWithContentsOfFile:[encoding:error:]` / `initWithContentsOfFile:` (log 016 → nil), `hasPrefix:`/`hasSuffix:`, `intValue`, `substringToIndex:`, `substringWithRange:`, `stringByAppendingFormat:`, `stringByAppendingPathExtension:`, `isAbsolutePath`, `createDirectoryAtPath:attributes:`, `removeItemAtPath:error:`. `isFW3()` (`[[systemVersion substringToIndex:1] intValue] > 2`) devolvía siempre falso. Los ~128 restantes son UI de IGP/GLLive/picker de fotos.
  - Vídeo: el hilo de audio de la cinemática fallaba en `pthread_create` (256 KB). Ahora loguea el código y reintenta con 128/64 KB.
  - Mapa de decompilación: `python3 -I tools/decomp_map.py` → `decompiled/macho/decomp_map.html` (17.867 funciones de `__text`, 31 sin nombre, 12 nativas). Marcar funciones reconstruidas en `docs/decomp_status.tsv`.
  - Build: ahora en el Mac (`./build.sh [Release|Debug|clean]`), sin WSL. vitaGL ya no se compila dentro de `vendor/` (los `.o`/`.a` viejos de WSL parecían actualizados y no linkeaban con GCC 10.3): CMake lo copia con rsync a `<build>/vitaGL-<hash de flags>/` y compila ahí. Build limpia verificada en el Mac.
  - Pendiente de consola: confirmar softfp de 016 (vídeo negro/0.3 fps), hilo de audio, `strcpy(src=NULL)` tras saltar el vídeo.
- **Iteration 018 (log 017):** llega al menú; el audio del vídeo se oye (el reintento del hilo funciona) y el fps sube a 14.7 (fix softfp de 016 confirmado).
  - Vídeo sin imagen: `draw_video_frame` dibujaba en el FBO 320x480 del guest (bindeado desde `gles_init`) con viewport 960x544 y luego `vglSwapBuffers` mostraba el framebuffer 0 → pantalla negra (el readback 16,16,16 leía fuera del FBO). Fix: bindear el framebuffer 0 durante el dibujo del vídeo y restaurar el FBO después.
  - Menú sin botones: la librería 2D (`RenderRegions`, `DrawRegion_NoClip_NoTranslat`, `FillRect`, `FillTriangle`, `DrawLine_NoClip_NoTranslat`) usa `glVertexPointer`/`glTexCoordPointer` con `GL_FIXED` + `glDrawElements`. `gles.c` solo convertía GL_FIXED→float en `glDrawArrays`; en `glDrawElements` vitaGL dibujaba con el puntero del draw anterior. Fix: pasar `GL_FIXED` directo a vitaGL (lo soporta nativamente, conversión en shader) y borrar la conversión en CPU.
  - Corrección (log 018): esas funciones 2D son `gllive::` (online), no el menú. El fix de GL_FIXED sigue siendo válido pero no afectaba al menú.
- **Iteration 019 (log 018):** el vídeo ya se ve, pero borroso; el menú sigue sin botones.
  - Menú: lo dibuja gameswf (`render_handler_irrlicht`), que recorta con máscaras de stencil (`begin_submit_mask`: `glStencilFunc(GL_EQUAL, nivel)` + `GL_INCR`/`GL_DECR`). El FBO de `gles_init` no tenía stencil (vitaGL crea depth sin stencil si no se pide) → todo lo enmascarado fallaba el test. Fix: renderbuffer `GL_DEPTH24_STENCIL8` adjunto al FBO.
  - Vídeo: `lowres=1` (heredado de otro port, donde la conversión tardaba 90 ms) decodificaba 480x320 → 240x160. Ahora `lowres=0` (el benchmark de arranque da 5.6 ms para 800x480).
- **Iteration 020 (log 019 + 2 screenshots):** vídeo a 480x320 OK. Menú: solo el fondo 3D, sin UI de gameswf (`mainmenu.dat` es un SWF `FWS`; `EnGameswf::InitMainMenu`). iOS tenía depth 16 sin stencil (`EAGLView`: `GL_DEPTH_COMPONENT16` en `GL_DEPTH_ATTACHMENT`), así que el stencil no era la diferencia con iOS.
  - Solo diagnóstico (`gles.c`, marcado `ponytail:` temporal): `[glerr]` nombra la función GL que levanta cada error; `[swfdiag]` loguea los draws de gameswf (xyz float stride 12): vértice, clip-space, color, estado depth/stencil/blend/alpha/scissor/viewport; y draw calls cada 300 frames.
- **Iteration 021 (log 020):** `[glerr]` identifica los INVALID_ENUM: `glEnable(GL_DITHER)`, `glEnable(GL_LINE_SMOOTH)`, `glHint(GL_LINE_SMOOTH_HINT)`, `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)` — todos benignos. **Cero draws de gameswf** (~31 draws/frame, todos del fondo 3D): gameswf no dibuja nada.
  - `EnGameswf::Render` (0x4ec368) reimplementado en C en `patch.c` (1:1 con el pseudo-C) y logueando los flags que deciden si llama a `RenderFX::Render` (`+0x4c`, `+0x22`, `+0x6c/+0x70`, MenuFX, `+0x81`, `+0x2a/+0x2b`, `_s_gameoverMainmenu`).
- **Iteration 022 (log 021 + dump 1793810227):** prefetch abort `PC=0x500`, `LR` en gameswf `begin_display`. Causa: el wrapper `[glerr]` de 021 (9 args `uint32_t`) — en AAPCS el callee es dueño del área de args en pila y GCC la usó como spill; el juego llamaba `glHint` con 2 args → pisó el frame del llamador (`0xc52,0x1102,1,0x500` en la pila). Lección: **nunca wrappers genéricos con >4 args para funciones del guest**. Wrapper borrado (ya cumplió: errores identificados).
  - Primer `EnGameswf::Render`: `enabled22=1 show2b=1 menu!=NULL states=0` → llama a `RenderFX::Render(menu)`. `[swfdiag]` ahora loguea cualquier draw hecho dentro de `RenderFX::Render` y cuántos produce cada llamada.
- **Iteration 023 (log 022):** `RenderFX::Render(menu)` se llama cada frame pero produce **0 draws**, y `MenuFX+0x118` (nº de estados) se queda en 0: `InitMainMenu` no encontró ningún `menu_*` en `mainmenu.dat` (SWF `FWS`) → menú vacío.
  - gameswf reporta errores por `gameswf::log_error`/`log_msg` vía un callback que el juego no registra. Hookeados (0x5614b4/0x561448) → `[gameswf]` en el log. `fopen` en lectura que falla ahora es WARN salvo saves/profiles.
- **Iteration 024 (log 023):** sin errores de gameswf ni `fopen` fallidos. Cadena del menú: `InitMainMenu` → `RenderFX::FindCharacters(root, "menu_")` (`CollectCharacters`: `strstr` del nombre, hijos en `+0xec/+0xf0`) → `RegisterMenu` → `MenuFX::RegisterState` (lista `+0x104/+0x108`) → `SetMenu("menu_StartMenu")` → `GetState` (`strcmp`) → push en la pila `+0x114/+0x118`. La pila queda en 0 → `GetState` devuelve NULL.
  - Revisados otros ports Gameloft: Skater-Nation (iOS, mismo loader) tuvo los literales `'FWS'/'CWS'` rebaseados por `macho_prep.py`; aquí **no** aplica (0x562a30/34 no están en rebase.bin; nuestro `macho_prep.py` no escanea `__text`). DH3: gameswf necesita stencil (ya puesto en 019).
  - Diagnóstico: en el primer `Render`, volcado de los estados registrados de MenuFX y del árbol de personajes del SWF (nombre, sprite, hijos, visible).
- **Iteration 025 (log 024):** el volcado muestra el SWF sano: 24 estados registrados (incluido `menu_StartMenu`), árbol completo con `btn_*`, todos `visible=0`, pila vacía. Nadie hace push.
  - Cadena real (iOS): `InitMainMenu` (rama `+0x27`) pone `_s_showLogo=1` → `updateTimerPages()` (0x1f734): `_s_showUBILogo` → 420 frames con `_ubiLogo` → `_s_showPlatform` → 250 frames con `_platLogo` → `_s_showPlatformOver=1` → `EnGameswf::UpdateTimerPages` → `SetMenu("menu_TouchScreen")`.
  - `updateTimerPages()` solo la llama `-[AppController draw]`, disparado por un `NSTimer` de 1/60 s (`applicationDidFinishLaunching:`). `objc.c` devolvía un objeto falso y **nunca disparaba el timer**: `draw` no corría nunca (tampoco `LoadMenuRes`/pantallas de carga que hace `draw`).
  - Fix `objc.c`: los timers con target = AppController se guardan y se disparan desde `CFRunLoopRunInMode`, que `ThreadRoutine` llama cada frame dentro de su mutex → `draw` corre en el hilo dueño de vitaGL, serializado con `GameLoop`.
  - Herramienta: `tools/pcrefs.py` (xrefs exactos PIC ARM: `ldr rX,[pc,#]` → `add/ldr ...,pc,rX`); los xrefs del pseudo-C de Ghidra no ven los accesos vía `__nl_symbol_ptr`.
  - Sin resolver: 16× `GL_INVALID_ENUM` tragados (origen desconocido; probablemente `glHint`/`glEnable` benignos), `strcpy(src=NULL)` tras saltar el vídeo, `UBISOFT.m4v`/`UBIMTL.m4v`/`LOGORP.m4v` no existen en el IPA.

---

## 4. Instructions for Console Testing (Phase 15)

To test the port on physical PlayStation Vita hardware:

1. **Start FTP on PS Vita:**
   - Launch **VitaShell** on your PS Vita.
   - Press **Select** to start the FTP server (note the IP address shown on screen, typically `192.168.1.xxx`).

2. **Deploy Game Data:**
   Run the deployment script or toolkit command from the project root:
   ```bash
   psvita-toolkit deploy --data --ip <VITA_IP>
   # Or using the direct deployment script:
   python3 -I tools/deploy_data.py --ip <VITA_IP>
   ```
   This will upload:
   - `ux0:data/princeofpersiawarriorwithin/game_app/` (Executable `POP` and game resources)
   - `ux0:data/princeofpersiawarriorwithin/rebase.bin`
   - `ux0:data/princeofpersiawarriorwithin/saves/`
   - `ux0:data/princeofpersiawarriorwithin/logs/`

3. **Install the VPK:**
   - Upload `build/princeofpersiawarriorwithin.vpk` to `ux0:downloads/` (or any staging directory) on the Vita.
   - In VitaShell, navigate to the uploaded `.vpk` file, press **Cross**, and confirm installation.

4. **Launch the Game:**
   - Launch **Prince of Persia: Warrior Within** from the PS Vita LiveArea.
   - If any crash or issue occurs, pull the rotating log from the console:
     ```bash
     python3 -I tools/deploy_data.py --ip <VITA_IP> --logs
     ```
   - Check the newest log in `ux0_data/princeofpersiawarriorwithin/logs/princeofpersiawarriorwithin_NNN.log`.
   - If a crash dump (`.psp2dmp`) is generated in `ux0:data/`, analyze it using:
     ```bash
     psvita-toolkit analyze <dump>.psp2dmp --elf build/princeofpersiawarriorwithin
     ```

## Iteración 026 (log 025)
- El NSTimer `draw` se registra (`[OBJC] NSTimer 0.0167s ... sel=draw`), pero el menú sigue vacío. El log terminó unos 10 s después de que empezara el render del menú, y la cadena de logos necesita 420+250 frames (~11 s, o el doble si el timer se saltaba un frame de cada dos por el plazo estricto).
- Ahora el timer tiene una holgura de medio periodo, así que se dispara una vez por frame del bucle del juego.
- `updateTimerPages` (0x1f7a8, 0x1f894, 0x1f89c): las esperas de 420 y 250 frames se parchearon a 2 frames, porque las UIImageView de los logos son falsas y no dibujan nada.
- Diagnóstico `[OBJC] timer draw fire #N imp= showLogo= UBI= plat= platOver=` cada 300 disparos.

## Iteración 027 (log 026): el menú se ve, la entrada no funciona
- El menú y el selector de idioma ya se ven. Lo arregló el parche de la espera de logos de la 026.
- Causa de que no funcionen ni el táctil ni los botones: `hook_INO_Joystick_Update` sustituía `_INO_Joystick_Update` (0xaaba8) por una versión que solo escribía los bits del pad. El original es el único que llama a `CTouchScreen::UpdateEvents`, así que la cola de PadTouchEvent nunca se vaciaba (tope de 300). Además, TouchManager::Update/IsKeyPressed y `EnGameswf::SetCursorState` (el cursor del menú) nunca se ejecutaban.
- Arreglo: reimplementación 1:1 del original (UpdateEvents, TouchManager::Update, mapeo de IsKeyPressed a bits según la tabla del binario, y la cola con 0x6b45b8/0x6bf26c/0x6bf268), más los bits del pad ORed.
- El envío de toques se movió del hilo principal (`patch_frame`) al hilo del juego, justo antes de UpdateEvents. El UITouch* falso ahora es un puntero a un objeto estable por dedo.
- Diagnóstico: `[INPUT] touch down id raw -> game` en los primeros 16 toques. Coordenadas esperadas: horizontal de 480x320, igual que `-[EAGLView touchesChanged:]` (x=view.y, y=320-view.x).

## Iteración 028 (log 027): ¡jugable!
- El táctil y los controles funcionan. El usuario confirma en el juego: CROSS = saltar (bit 0), TRIANGLE = atacar (bit 2), CIRCLE/SQUARE = agarrar/soltar arma (bits 3/1), START = pausa (bit 9).
- La defensa no estaba asignada a ningún botón físico. R1 pasa a usar el bit 6 (0x40), el único bit de acción (0–7) que ningún botón físico usaba. Antes R1 usaba el bit 5. No hay prueba estática del bit, porque los scripts de IA de Jade leen los bits por número.
- Diagnóstico: `[INPUT] on-screen buttons -> INO bits 0x....` registra qué bit activa cada botón en pantalla. Basta tocar el escudo para confirmarlo.
- En `controls.txt` se acepta `BITn` (por ejemplo `R1 = BIT6`), y `BLOCK` pasa a ser el bit 6.
  - Log 027 — personaje translúcido in-game: iOS renderiza en un renderbuffer `kEAGLColorFormatRGB565` (único formato importado) → sin alpha, `GL_DST_ALPHA` = 1. `RenderState::Send` usa blends con `GL_DST_ALPHA`/`GL_ONE_MINUS_DST_ALPHA` (0x304/0x305); nuestro FBO RGBA8 guardaba el alpha de textura del personaje y las pasadas siguientes lo dejaban transparente. Fix (`gles.c`): `glColorMask` nunca escribe alpha y `glClearColor`/`glClearColorx` limpian alpha a 1 → semántica RGB565. Pendiente de verificar en consola.
  - Tras el fix de alpha el príncipe sigue con triángulos faltantes y se le ven caras internas (capturas bg/fb 2026-11-04 19:57). Se descartaron: el combinador de vitaGL (GL_COMBINE MODULATE ×2, CONSTANT/PRIMARY), el fog (solo RGB), la pasada de sombra (`ShadowCasterList::Display` está vacía), el renombrado de VBO en `glBufferSubData` y la recarga del culling al cambiar de FBO. Experimento de una corrida (`gles.c`, marcado `ponytail:`): se ignora `glEnable(GL_CULL_FACE)` y se loguean `[cull] glFrontFace` y `[frustum]` near/far. Si el príncipe se ve sólido, la causa es el culling; si no, es depth.
