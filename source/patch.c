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
#include "patch.h"
#include "video.h"

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
    .cross_action = (1 << 0),    // Jump
    .square_action = (1 << 1),   // Attack
    .triangle_action = (1 << 2), // Grab / Sub Attack
    .circle_action = (1 << 3),   // Action / Block / Roll
    .l1_action = (1 << 4),       // Time Rewind
    .r1_action = (1 << 5),       // Wall Run
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
            else if (!strcasecmp(act, "ATTACK")) mask = (1 << 1);
            else if (!strcasecmp(act, "GRAB")) mask = (1 << 2);
            else if (!strcasecmp(act, "BLOCK")) mask = (1 << 3);
            else if (!strcasecmp(act, "REWIND")) mask = (1 << 4);
            else if (!strcasecmp(act, "WALLRUN")) mask = (1 << 5);
            else if (!strcasecmp(act, "SLOWMO")) mask = (1 << 7);
            else if (!strcasecmp(act, "MENU")) mask = (1 << 9);

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

// Hook for _INO_Joystick_Update: injects PS Vita button states into engine joystick bitmask
static void hook_INO_Joystick_Update(void) {
    // Inject physical buttons into the engine's current joystick state
    uint32_t *p_curr = (uint32_t *)(MACHO_SLIDE + ADDR_INO_GSL_CURRENT_STATE);
    uint32_t btn_mask = 0;

    if (pad.buttons & SCE_CTRL_CROSS)    btn_mask |= s_cfg.cross_action;
    if (pad.buttons & SCE_CTRL_SQUARE)   btn_mask |= s_cfg.square_action;
    if (pad.buttons & SCE_CTRL_TRIANGLE) btn_mask |= s_cfg.triangle_action;
    if (pad.buttons & SCE_CTRL_CIRCLE)   btn_mask |= s_cfg.circle_action;
    if (pad.buttons & SCE_CTRL_L1)       btn_mask |= s_cfg.l1_action;
    if (pad.buttons & SCE_CTRL_R1)       btn_mask |= s_cfg.r1_action;
    if (pad.buttons & SCE_CTRL_SELECT)   btn_mask |= s_cfg.select_action;
    if (pad.buttons & SCE_CTRL_START)    btn_mask |= s_cfg.start_action;

    *p_curr |= btn_mask;
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

    // Safeguard _s_GameRecord: ensure pointer is never NULL to avoid crash in StartGame
    uint32_t *p_gr = (uint32_t *)macho_sym("_s_GameRecord");
    if (p_gr && !*p_gr) {
        l_info("[PATCH] Initializing _s_GameRecord default buffer (safeguard).");
        *p_gr = (uint32_t)s_default_game_record;
    }

    l_info("[PATCH] Joystick and button hooks installed successfully.");
}

void patch_frame(void) {
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
            call_guest((void *)s_touch_down, touches[i].gx, touches[i].gy, touches[i].id + 1, 0);
        } else {
            call_guest((void *)s_touch_move, touches[i].gx, touches[i].gy, touches[i].id + 1, 0);
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
            call_guest((void *)s_touch_up, last_frame_touches[j].gx, last_frame_touches[j].gy, last_frame_touches[j].id + 1, 0);
        }
    }

    memcpy(last_frame_touches, touches, sizeof(touches));
    last_frame_touch_count = touch_count;
}
