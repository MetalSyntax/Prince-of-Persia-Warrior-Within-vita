/**
 * @file patch.c
 * @brief Game-specific hooks, physical controls, and touch dispatching for Prince of Persia.
 */
#include <math.h>
#include <psp2/ctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include <kubridge.h>
#include "patch.h"
#include "video.h"
#include "gles.h"
#include "darwin_printf.h"
#include <stdarg.h>

// Forward externs from input.c
extern touch_point touches[SCE_TOUCH_MAX_REPORT];
extern int touch_count;
extern SceCtrlData pad;

static uintptr_t s_touch_down = 0;
static uintptr_t s_touch_move = 0;
static uintptr_t s_touch_up = 0;

static touch_point last_frame_touches[SCE_TOUCH_MAX_REPORT];
static int last_frame_touch_count = 0;

// Addresses of hooked engine functions
#define ADDR_INO_JOYSTICK_UPDATE   0x000aaba8
#define ADDR_INO_JOYSTICK_MOVE     0x000ab234
#define ADDR_INO_JOYSTICK_GETBTN_X 0x000ab044
#define ADDR_INO_JOYSTICK_GETBTN_Y 0x000ab080

// Addresses of global engine joystick state
#define ADDR_INO_GSL_CURRENT_STATE 0x006bf264
#define ADDR_INO_GSL_PREV_STATE    0x006bf260

// Configurable button mappings
typedef struct {
    uint32_t cross_action;
    uint32_t square_action;
    uint32_t triangle_action;
    uint32_t circle_action;
    uint32_t l1_action;
    uint32_t r1_action;
    uint32_t select_action;
    uint32_t start_action;
} control_config;

static control_config s_cfg = {
    .cross_action = (1 << 0),    // Jump (confirmed on console, log 027)
    .square_action = (1 << 1),   // Drop weapon (confirmed)
    .triangle_action = (1 << 2), // Attack (confirmed)
    .circle_action = (1 << 3),   // Grab (confirmed)
    .l1_action = (1 << 4),       // Time Rewind
    // Block = bit 5: the shield is VirtualButton 11, one of the three round action buttons (5 attack,
    // 7 jump, 11 shield; inset rects in TouchManager::Init); ButtonKeyMap(7) = 11, and key 7 sets 0x20
    // in _INO_Joystick_Update. Bit 6 (key 4 -> button 8) was a guess and blocked nothing.
    .r1_action = (1 << 5),
    .select_action = (1 << 7),   // Slow Motion
    .start_action = (1 << 9),    // Pause / Menu
};

static void load_controls_config(void) {
    char path[128];
    snprintf(path, sizeof(path), "%scontrols.txt", DATA_PATH);
    FILE *f = fopen(path, "r");
    if (!f) {
        l_info("[INPUT] Using default PS Vita button mappings.");
        return;
    }
    l_info("[INPUT] Loading custom button mappings from %s", path);
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char btn[32], act[32];
        if (sscanf(line, "%31s = %31s", btn, act) == 2) {
            uint32_t mask = 0;
            if (!strcasecmp(act, "JUMP")) mask = (1 << 0);
            else if (!strcasecmp(act, "ATTACK")) mask = (1 << 2);
            else if (!strcasecmp(act, "GRAB")) mask = (1 << 3);
            else if (!strcasecmp(act, "DROP")) mask = (1 << 1);
            else if (!strcasecmp(act, "BLOCK")) mask = (1 << 5);
            else if (!strcasecmp(act, "REWIND")) mask = (1 << 4);
            else if (!strcasecmp(act, "SLOWMO")) mask = (1 << 7);
            else if (!strcasecmp(act, "MENU")) mask = (1 << 9);
            else if (!strncasecmp(act, "BIT", 3)) mask = 1u << (atoi(act + 3) & 31); // raw INO bit, e.g. BIT5

            if (!strcasecmp(btn, "CROSS")) s_cfg.cross_action = mask;
            else if (!strcasecmp(btn, "SQUARE")) s_cfg.square_action = mask;
            else if (!strcasecmp(btn, "TRIANGLE")) s_cfg.triangle_action = mask;
            else if (!strcasecmp(btn, "CIRCLE")) s_cfg.circle_action = mask;
            else if (!strcasecmp(btn, "L1") || !strcasecmp(btn, "L")) s_cfg.l1_action = mask;
            else if (!strcasecmp(btn, "R1") || !strcasecmp(btn, "R")) s_cfg.r1_action = mask;
            else if (!strcasecmp(btn, "SELECT")) s_cfg.select_action = mask;
            else if (!strcasecmp(btn, "START")) s_cfg.start_action = mask;
        }
    }
    fclose(f);
}

// The UITouch* the game gets from -touchesChanged: is an opaque per-finger identity: give each Vita
// report id a stable fake object instead of a small integer, in case TouchManager dereferences it.
static void *touch_obj(int id) {
    static uint32_t objs[16][16];
    return objs[id & 15];
}

static void touch_dispatch(void) {
    // Dispatch front capacitive touchscreen events directly into CTouchScreen
    if (!s_touch_down) {
        s_touch_down = macho_sym("__ZN12CTouchScreen13PadTouchEventEiiPv");
        s_touch_move = macho_sym("__ZN12CTouchScreen12PadMoveEventEiiPv");
        s_touch_up   = macho_sym("__ZN12CTouchScreen15PadUntouchEventEiiPv");
    }
    if (!s_touch_down)
        return;

    // Dispatched down/move
    for (int i = 0; i < touch_count; i++) {
        int was_down = 0;
        for (int j = 0; j < last_frame_touch_count; j++) {
            if (last_frame_touches[j].id == touches[i].id) {
                was_down = 1;
                break;
            }
        }
        if (!was_down) {
            static int logged;
            if (logged++ < 16)
                l_info("[INPUT] touch down id=%d raw=%d,%d -> game %d,%d", touches[i].id, touches[i].x, touches[i].y,
                       touches[i].gx, touches[i].gy);
            call_guest((void *)s_touch_down, touches[i].gx, touches[i].gy, (uint32_t)touch_obj(touches[i].id), 0);
        } else {
            call_guest((void *)s_touch_move, touches[i].gx, touches[i].gy, (uint32_t)touch_obj(touches[i].id), 0);
        }
    }

    // Dispatched up
    for (int j = 0; j < last_frame_touch_count; j++) {
        int still_down = 0;
        for (int i = 0; i < touch_count; i++) {
            if (touches[i].id == last_frame_touches[j].id) {
                still_down = 1;
                break;
            }
        }
        if (!still_down) {
            call_guest((void *)s_touch_up, last_frame_touches[j].gx, last_frame_touches[j].gy, (uint32_t)touch_obj(last_frame_touches[j].id), 0);
        }
    }

    memcpy(last_frame_touches, touches, sizeof(touches));
    last_frame_touch_count = touch_count;
}

// 1:1 reimplementation of _INO_Joystick_Update (0xaaba8) plus the Vita pad. The old hook replaced it with
// only the pad bits, so CTouchScreen::UpdateEvents never drained the touch queue and TouchManager (menu
// cursor via EnGameswf::SetCursorState, on-screen buttons) never saw a touch (log 026).
// Runs on the game thread inside GameLoop, so the touches are queued here too (no race with UpdateEvents).
#define GSL_HELD_MASK 0x006b45b8 // u32 mask applied to the current state
#define GSL_START_OK  0x006b45bc // u8: key 9 (pause) allowed
#define GSL_HOLD_CNT  0x006bf26c // frames to force an empty state
#define GSL_FORCE_PAUSE 0x006bf268
static void hook_INO_Joystick_Update(void) {
    static const uint8_t key_bit[16] = {0, 1, 2, 3, 6, 7, 4, 5, 255, 9, 11, 10, 12, 13, 14, 15}; // bit -> key
    uint32_t *cur = (uint32_t *)(MACHO_SLIDE + ADDR_INO_GSL_CURRENT_STATE);
    uint32_t *prev = (uint32_t *)(MACHO_SLIDE + ADDR_INO_GSL_PREV_STATE);

    touch_dispatch();
    call_guest((void *)(MACHO_SLIDE + 0xa8fc), 0, 0, 0, 0); // CTouchScreen::UpdateEvents
    uint32_t tm = call_guest((void *)(MACHO_SLIDE + 0xadb1c), 0, 0, 0, 0); // TouchManager::GetInstance
    call_guest((void *)(MACHO_SLIDE + 0xad9e8), tm, 0, 0, 0);             // TouchManager::Update
    *prev = *cur;
    *cur = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (key_bit[bit] == 255)
            continue;
        if ((int)call_guest((void *)(MACHO_SLIDE + 0xac264), tm, key_bit[bit], 0, 0) > 0 && // IsKeyPressed
            (bit != 9 || *(uint8_t *)(MACHO_SLIDE + GSL_START_OK)))
            *cur |= 1u << bit;
    }

    // Touch-only bits: logs which INO bit each on-screen button sets, to map the pad (log 027: block).
    static uint32_t last_touch_bits;
    if (*cur != last_touch_bits && *cur)
        l_info("[INPUT] on-screen buttons -> INO bits 0x%04x", (unsigned)*cur);
    last_touch_bits = *cur;

    // ponytail: pad -> INO bit meanings are the earlier guesses (controls.txt overrides); verify in game.
    uint32_t btn_mask = 0;
    if (pad.buttons & SCE_CTRL_CROSS)    btn_mask |= s_cfg.cross_action;
    if (pad.buttons & SCE_CTRL_SQUARE)   btn_mask |= s_cfg.square_action;
    if (pad.buttons & SCE_CTRL_TRIANGLE) btn_mask |= s_cfg.triangle_action;
    if (pad.buttons & SCE_CTRL_CIRCLE)   btn_mask |= s_cfg.circle_action;
    // L + R toggles the on-screen touch controls (hidden by default, gles.c), as in Hero of Sparta II.
    // L1/R1 also carry actions (rewind/block), so each acts only after 3 frames held alone: pressing
    // both for the toggle never leaks a rewind or block.
    extern int gles_hud_hidden;
    static int l_frames, r_frames, lr_combo;
    // Vita L/R are SCE_CTRL_LTRIGGER/RTRIGGER (0x100/0x200); SCE_CTRL_L1/R1 (0x400/0x800) only exist on
    // DS3/DS4 pads (PS TV). Log 032: checking only L1/R1 meant L, R and L + R never did anything.
    int l = (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) != 0, r = (pad.buttons & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) != 0;
    static int l_seen, r_seen; // log 033 had no [HUD] toggle line: confirm L/R reach this hook
    if (l && !l_seen++)
        l_info("[INPUT] L pressed (pad 0x%08x)", (unsigned)pad.buttons);
    if (r && !r_seen++)
        l_info("[INPUT] R pressed (pad 0x%08x)", (unsigned)pad.buttons);
    l_frames = l ? l_frames + 1 : 0;
    r_frames = r ? r_frames + 1 : 0;
    if (l && r) {
        if (!lr_combo) {
            gles_hud_hidden = !gles_hud_hidden;
            l_info("[HUD] touch controls %s", gles_hud_hidden ? "hidden" : "visible");
            extern int gles_hud_dump;
            gles_hud_dump = 1; // gles.c: log the next frame's draws (calibration)
        }
        lr_combo = 1;
    } else if (!l && !r) {
        lr_combo = 0;
    }
    if (!lr_combo && l_frames >= 3) btn_mask |= s_cfg.l1_action;
    if (!lr_combo && r_frames >= 3) btn_mask |= s_cfg.r1_action;
    if (pad.buttons & SCE_CTRL_SELECT)   btn_mask |= s_cfg.select_action;
    if (pad.buttons & SCE_CTRL_START)    btn_mask |= s_cfg.start_action;
    *cur |= btn_mask;

    // Tail of the original: mask, forced-empty countdown, forced pause request.
    int32_t *hold = (int32_t *)(MACHO_SLIDE + GSL_HOLD_CNT);
    uint8_t *force_pause = (uint8_t *)(MACHO_SLIDE + GSL_FORCE_PAUSE);
    *cur &= *(uint32_t *)(MACHO_SLIDE + GSL_HELD_MASK);
    if (*hold) {
        *prev = 0;
        *cur = 0;
        (*hold)--;
    }
    if (*force_pause) {
        *prev &= ~0x200u;
        *cur = 0x200;
        *force_pause = 0;
        *hold = 10;
    }
}

// Hook for _INO_Joystick_Move: delivers Left Analog Stick and D-Pad movement vectors
static void hook_INO_Joystick_Move(int r0, float *out_vec, int r2) {
    if (!out_vec) return;

    // Check Left Analog Stick with ~12% deadzone
    float lx = ((float)pad.lx - 128.0f) / 128.0f;
    float ly = ((float)pad.ly - 128.0f) / 128.0f;
    float len = sqrtf(lx * lx + ly * ly);

    if (len > 0.12f) {
        out_vec[0] = lx;
        out_vec[1] = -ly; // Invert Y to match 3D camera / world direction
    } else if (pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        float dx = 0.0f, dy = 0.0f;
        if (pad.buttons & SCE_CTRL_LEFT)  dx -= 1.0f;
        if (pad.buttons & SCE_CTRL_RIGHT) dx += 1.0f;
        if (pad.buttons & SCE_CTRL_UP)    dy += 1.0f;
        if (pad.buttons & SCE_CTRL_DOWN)  dy -= 1.0f;
        out_vec[0] = dx;
        out_vec[1] = dy;
    } else {
        out_vec[0] = 0.0f;
        out_vec[1] = 0.0f;
    }
}

// Right analog stick camera horizontal control
static int hook_INO_Joystick_GetButtonX(int r0) {
    float rx = ((float)pad.rx - 128.0f) / 128.0f;
    if (fabsf(rx) > 0.15f) {
        return (int)(rx * 100.0f);
    }
    return 0;
}

// Right analog stick camera vertical control
static int hook_INO_Joystick_GetButtonY(void) {
    float ry = ((float)pad.ry - 128.0f) / 128.0f;
    if (fabsf(ry) > 0.15f) {
        return (int)(ry * 100.0f);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// VFPv2 Short Vector Mode Replacements
// Cortex-A9 (ARMv7-A) generates Undefined Instruction (0x30002) when FPSCR.LEN != 0.
// ---------------------------------------------------------------------------

void hook_Matrix4Vector3Mul(const float *m, const float *v, float *out) {
    float x = v[0], y = v[1], z = v[2];
    out[0] = m[0] * x + m[4] * y + m[8] * z;
    out[1] = m[1] * x + m[5] * y + m[9] * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
}

void hook_Matrix3Mul(const float *a, const float *b, float *out) {
    float res[12];
    for (int col = 0; col < 3; col++) {
        float a0 = a[col * 4 + 0];
        float a1 = a[col * 4 + 1];
        float a2 = a[col * 4 + 2];
        res[col * 4 + 0] = b[0] * a0 + b[4] * a1 + b[8] * a2;
        res[col * 4 + 1] = b[1] * a0 + b[5] * a1 + b[9] * a2;
        res[col * 4 + 2] = b[2] * a0 + b[6] * a1 + b[10] * a2;
        res[col * 4 + 3] = 0.0f;
    }
    memcpy(out, res, sizeof(res));
}

void hook_Matrix4Mul(const float *a, const float *b, float *out) {
    float res[16];
    for (int col = 0; col < 3; col++) {
        float a0 = a[col * 4 + 0];
        float a1 = a[col * 4 + 1];
        float a2 = a[col * 4 + 2];
        res[col * 4 + 0] = b[0] * a0 + b[4] * a1 + b[8] * a2;
        res[col * 4 + 1] = b[1] * a0 + b[5] * a1 + b[9] * a2;
        res[col * 4 + 2] = b[2] * a0 + b[6] * a1 + b[10] * a2;
        res[col * 4 + 3] = 0.0f;
    }
    float a0 = a[12], a1 = a[13], a2 = a[14];
    res[12] = b[0] * a0 + b[4] * a1 + b[8] * a2 + b[12];
    res[13] = b[1] * a0 + b[5] * a1 + b[9] * a2 + b[13];
    res[14] = b[2] * a0 + b[6] * a1 + b[10] * a2 + b[14];
    res[15] = 1.0f;
    memcpy(out, res, sizeof(res));
}

// ---------------------------------------------------------------------------
// Video Cutscene Playback Hooks (FFmpeg)
// ---------------------------------------------------------------------------
static volatile int s_is_movie_playing = 0;
static char s_pending_movie[256] = {0};

int hook_IsMoviePlaying(void) {
    return s_is_movie_playing;
}

void hook_StopMovie(void) {
    l_info("[VIDEO] StopMovie called.");
    s_is_movie_playing = 0;
}

void hook_StartMovieOnMainThread(const char *name) {
    l_info("[VIDEO] StartMovieOnMainThread(\"%s\") called", name ? name : "NULL");
    if (!name || !name[0]) return;
    s_is_movie_playing = 1;
    video_play(name);
    s_is_movie_playing = 0;
}

void hook_LoadMovie(const char *name) {
    l_info("[VIDEO] LoadMovie(\"%s\") called", name ? name : "NULL");
    if (name) {
        strncpy(s_pending_movie, name, sizeof(s_pending_movie) - 1);
        s_pending_movie[sizeof(s_pending_movie) - 1] = '\0';
    }
}

void hook_PlayMovie(void) {
    l_info("[VIDEO] PlayMovie() called for \"%s\"", s_pending_movie);
    if (s_pending_movie[0]) {
        s_is_movie_playing = 1;
        video_play(s_pending_movie);
        s_is_movie_playing = 0;
        s_pending_movie[0] = '\0';
    }
}

const import_entry patch_imports[] = {
    // Custom symbol intercepts if required
};
const int patch_imports_count = sizeof(patch_imports) / sizeof(patch_imports[0]);

static uint8_t s_default_game_record[384] = { 0 };

// gameswf::log_error / log_msg forward to a callback the game never registers: route them to our log.
static void hook_swf_log_error(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    l_warn("[gameswf] %s", buf);
}
static void hook_swf_log_msg(const char *fmt, ...) {
    static int n;
    if (++n > 64) // ponytail: capped, gameswf can be chatty
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    l_info("[gameswf] %s", buf);
}

/*
 * EnGameswf::Render() @ 0x4ec368, reimplemented 1:1 from decompiled/macho/POP.c. Log 031: the
 * [swfdiag] tree dump that used to run here on every state change wrote ~700 lines (~10 ms each) when
 * the pause menu opened and froze the game for seconds; the diagnostics are gone.
 */
static void hook_EnGameswf_Render(uint8_t *self) {
    uint8_t *menu = *(uint8_t **)self;
    uint8_t *alt = *(uint8_t **)(self + 0x70);
    uint8_t gameover = *(uint8_t *)(MACHO_SLIDE + 0x6b92b5); // _s_gameoverMainmenu
    int states = menu ? *(int *)(menu + 0x118) : -1;

    void *set_orient = (void *)(MACHO_SLIDE + 0x58fc84), *set_vp = (void *)(MACHO_SLIDE + 0x590b20);
    void *fx_render = (void *)(MACHO_SLIDE + 0x58fcb8);
    if (self[0x4c] || !self[0x22])
        return;
    uint8_t *fx;
    if (self[0x6c] && alt) {
        fx = alt;
    } else {
        if (!menu)
            return;
        call_guest((void *)(MACHO_SLIDE + 0x2bc18), 0, 0, 0, 0); // MenuSetting::Instance()
        call_guest(set_orient, (uint32_t)menu, 1, 0, 0);
        call_guest6(set_vp, (uint32_t)menu, 0, 0, 320, 480, 0);
        if (self[0x81] && states > 0)
            call_guest((void *)(MACHO_SLIDE + 0x590dd8), (uint32_t)menu, 0, 0, 0); // MenuFX::GetCurrentState
        if (!self[0x2a] && !self[0x2b] && !gameover)
            return;
        fx = menu;
    }
    if (fx == alt) {
        call_guest((void *)(MACHO_SLIDE + 0x2bc18), 0, 0, 0, 0);
        call_guest(set_orient, (uint32_t)alt, 1, 0, 0);
        call_guest6(set_vp, (uint32_t)alt, 0, 0, 320, 480, 0);
    }
    extern int gles_in_swf;
    gles_in_swf = fx == menu ? 2 : 1; // 2: menus are never touched by the HUD filter (gles.c)
    call_guest(fx_render, (uint32_t)fx, 0, 0, 0);
    gles_in_swf = 0;
}

void patch_init(void) {
    l_info("Initializing game-specific patches for Prince of Persia...");
    load_controls_config();

    // Hook joystick update and movement routines
    macho_hook("_INO_Joystick_Update", hook_INO_Joystick_Update);
    macho_hook("_INO_Joystick_Move", hook_INO_Joystick_Move);
    macho_hook("_INO_Joystick_GetButtonX", hook_INO_Joystick_GetButtonX);
    macho_hook("_INO_Joystick_GetButtonY", hook_INO_Joystick_GetButtonY);

    // Ensure FPSCR LEN/STRIDE bits are 0 (prevent VFP short vector mode traps on Cortex-A9)
    uint32_t fpscr = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    if (fpscr & 0x00370000) {
        fpscr &= ~0x00370000;
        __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr));
    }

    // Hook VFP Short Vector Mode routines
    macho_hook("__Z10Matrix4MulPKfS0_Pf", hook_Matrix4Mul);
    macho_hook("__Z10Matrix3MulPKfS0_Pf", hook_Matrix3Mul);
    macho_hook("__Z17Matrix4Vector3MulPKfS0_Pf", hook_Matrix4Vector3Mul);
    macho_hook_addr(MACHO_SLIDE + 0x000af1b8, hook_Matrix4Mul);
    macho_hook_addr(MACHO_SLIDE + 0x000af23c, hook_Matrix3Mul);
    macho_hook_addr(MACHO_SLIDE + 0x000af2a8, hook_Matrix4Vector3Mul);
    l_info("[PATCH] VFP Short Vector Mode replacements installed for Matrix4Mul, Matrix3Mul, Matrix4Vector3Mul.");

    // Hook Movie Player functions
    macho_hook("_StartMovieOnMainThread", hook_StartMovieOnMainThread);
    macho_hook("_LoadMovie", hook_LoadMovie);
    macho_hook("_PlayMovie", hook_PlayMovie);
    macho_hook("_StopMovie", hook_StopMovie);
    macho_hook("_IsMoviePlaying", hook_IsMoviePlaying);
    macho_hook_addr(MACHO_SLIDE + 0x00008fb8, hook_StartMovieOnMainThread);
    macho_hook_addr(MACHO_SLIDE + 0x0002c058, hook_LoadMovie);
    macho_hook_addr(MACHO_SLIDE + 0x0002c028, hook_PlayMovie);
    macho_hook_addr(MACHO_SLIDE + 0x0002bfc8, hook_StopMovie);
    macho_hook_addr(MACHO_SLIDE + 0x0002cf68, hook_IsMoviePlaying);
    l_info("[PATCH] Cutscene video playback hooks installed.");
    // updateTimerPages() holds the UBI logo view 420 frames and the platform logo 250 frames before
    // _s_showPlatformOver -> menu_TouchScreen. Those UIImageViews are fake here (nothing is drawn), so
    // the wait is ~11 s of blank screen: shorten both to 2 frames (cmp #420, cmp #250 / sub #250).
    static const struct { uint32_t off, insn; } logo_wait[] = {
        {0x1f7a8, 0xE3530002}, {0x1f894, 0xE3530002}, {0x1f89c, 0xE2433002}};
    for (unsigned i = 0; i < sizeof(logo_wait) / sizeof(*logo_wait); i++) {
        kuKernelCpuUnrestrictedMemcpy((void *)(MACHO_SLIDE + logo_wait[i].off), &logo_wait[i].insn, 4);
        kuKernelFlushCaches((void *)(MACHO_SLIDE + logo_wait[i].off), 4);
    }
    macho_hook_addr(MACHO_SLIDE + 0x004ec368, hook_EnGameswf_Render);
    macho_hook_addr(MACHO_SLIDE + 0x005614b4, hook_swf_log_error);
    macho_hook_addr(MACHO_SLIDE + 0x00561448, hook_swf_log_msg);

    // Safeguard _s_GameRecord: ensure pointer is never NULL to avoid crash in StartGame
    uint32_t *p_gr = (uint32_t *)macho_sym("_s_GameRecord");
    if (p_gr && !*p_gr) {
        l_info("[PATCH] Initializing _s_GameRecord default buffer (safeguard).");
        *p_gr = (uint32_t)s_default_game_record;
    }

    l_info("[PATCH] Joystick and button hooks installed successfully.");
}

