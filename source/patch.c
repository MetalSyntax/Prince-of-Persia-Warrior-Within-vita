/**
 * @file patch.c
 * @brief Game-specific hooks, physical controls, and touch dispatching for Prince of Persia.
 */
#include <math.h>
#include <psp2/ctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hle.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include <kubridge.h>
#include "patch.h"
#include "video.h"
#include "gles.h"
#include "options.h"
#include "trophies.h"
#include "darwin_printf.h"
#include <stdarg.h>

// Forward externs from input.c
extern touch_point touches[SCE_TOUCH_MAX_REPORT];
extern int touch_count;
extern SceCtrlData pad;

static uintptr_t s_touch_down = 0;
static uintptr_t s_touch_move = 0;
static uintptr_t s_touch_up = 0;

static touch_point last_frame_touches[SCE_TOUCH_MAX_REPORT + 1];
static int last_frame_touch_count = 0;
static int s_menu_frames;
static int s_fake_tap, s_fake_x = 240, s_fake_y = 160, s_game_touched; // fake tap (CROSS/START), see waits_for_tap()
static volatile int s_is_movie_playing;

// Addresses of hooked engine functions
#define ADDR_INO_JOYSTICK_UPDATE   0x000aaba8
#define ADDR_INO_JOYSTICK_MOVE     0x000ab234
#define ADDR_INO_JOYSTICK_GETBTN_X 0x000ab044
#define ADDR_INO_JOYSTICK_GETBTN_Y 0x000ab080

// Addresses of global engine joystick state
#define ADDR_INO_GSL_CURRENT_STATE 0x006bf264
#define ADDR_INO_GSL_PREV_STATE    0x006bf260

// Button mapping: each game action (INO bit) and the Vita button(s) that trigger it. The options menu
// (options.c, SELECT+START) edits this table and saves it to controls.txt (one BUTTON = ACTION per line).
static struct {
    const char *name, *label;
    uint32_t bit, buttons;
} s_act[] = {
    {"JUMP", "Jump", 1u << 0, SCE_CTRL_CROSS},           // confirmed on console (log 027)
    {"DROP", "Drop weapon", 1u << 1, SCE_CTRL_SQUARE},   // confirmed
    {"ATTACK", "Attack", 1u << 2, SCE_CTRL_TRIANGLE},    // confirmed
    {"GRAB", "Grab weapon", 1u << 3, SCE_CTRL_CIRCLE},   // confirmed
    {"REWIND", "Rewind time", 1u << 4, SCE_CTRL_LTRIGGER},
    // Block = bit 5: the shield is VirtualButton 11, one of the three round action buttons (5 attack,
    // 7 jump, 11 shield; inset rects in TouchManager::Init); ButtonKeyMap(7) = 11, and key 7 sets 0x20
    // in _INO_Joystick_Update. Confirmed on console (log 033: shield touch gives INO bits 0x0020).
    {"BLOCK", "Block", 1u << 5, SCE_CTRL_RTRIGGER},
    {"SLOWMO", "Slow motion", 1u << 7, SCE_CTRL_SELECT},
    {"MENU", "Pause", 1u << 9, SCE_CTRL_START},
};
#define NACT (int)(sizeof(s_act) / sizeof(s_act[0]))

int input_bind_count(void) { return NACT; }
const char *input_bind_label(int i) { return s_act[i].label; }
uint32_t input_bind_mask(int i) { return s_act[i].buttons; }
void input_bind_set(int i, uint32_t mask) { s_act[i].buttons = mask; }

void input_save_controls(void) {
    FILE *f = fopen(DATA_PATH "controls.txt", "w");
    if (!f)
        return;
    for (int i = 0; i < NACT; i++)
        for (uint32_t m = 1; m; m <<= 1)
            if (s_act[i].buttons & m && options_button_name(m))
                fprintf(f, "%s = %s\n", options_button_name(m), s_act[i].name);
    fclose(f);
    l_info("[INPUT] controls.txt saved");
}

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
        if (sscanf(line, "%31s = %31s", btn, act) != 2)
            continue;
        uint32_t m = options_button_mask(btn), bit = 0;
        if (!strncasecmp(act, "BIT", 3))
            bit = 1u << (atoi(act + 3) & 31); // raw INO bit, e.g. BIT5
        int a = -1;
        for (int i = 0; i < NACT; i++)
            if (!strcasecmp(act, s_act[i].name) || bit == s_act[i].bit)
                a = i;
        if (!m || a < 0) {
            l_warn("[INPUT] controls.txt: ignored line '%s = %s'", btn, act);
            continue;
        }
        for (int i = 0; i < NACT; i++) // a button triggers a single action: the file line wins
            s_act[i].buttons &= ~m;
        s_act[a].buttons |= m;
    }
    fclose(f);
}

// Both buttons of a combo reach the game only after being held alone for 3 frames, so the combo never
// leaks an action (L+R toggles the HUD: rewind/block; SELECT+START opens the menu: slow motion/pause).
// A quick tap (released before 3 frames, without the other button) is sent for 3 frames after release:
// log 037, a short START tap never paused. Returns 1 on the frame the combo is pressed.
typedef struct {
    uint32_t a, b;
    int combo, fa, fb, pa, pb;
} combo_gate;
static int combo_filter(combo_gate *c, uint32_t *btn) {
    int a = (*btn & c->a) != 0, b = (*btn & c->b) != 0, hit = 0;
    // Log 044: a 3-frame pulse did not pause the game; 10 frames (~170 ms) is like a normal press.
    if (!a && c->fa > 0 && c->fa < 3 && !c->combo)
        c->pa = 10;
    if (!b && c->fb > 0 && c->fb < 3 && !c->combo)
        c->pb = 10;
    c->fa = a ? c->fa + 1 : 0;
    c->fb = b ? c->fb + 1 : 0;
    if (a && b) {
        hit = !c->combo;
        c->combo = 1;
    } else if (!a && !b) {
        c->combo = 0;
    }
    if (c->combo || c->fa < 3)
        *btn &= ~c->a;
    if (c->combo || c->fb < 3)
        *btn &= ~c->b;
    if (c->pa > 0)
        c->pa--, *btn |= c->a;
    if (c->pb > 0)
        c->pb--, *btn |= c->b;
    return hit;
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
    static int s_fake_left; // fake tap frames still to send, for the touch-up
    touch_point cur[SCE_TOUCH_MAX_REPORT + 1];
    int n = touch_count;
    memcpy(cur, touches, n * sizeof(cur[0]));
    if (s_fake_tap) {
        s_fake_left = 4, s_fake_tap = 0; // held 4 frames, then released
        l_info("[INPUT] tap to continue (CROSS/START)");
    }
    if (s_fake_left > 0) {
        s_fake_left--;
        cur[n] = (touch_point){0};
        cur[n].id = 99, cur[n].gx = s_fake_x, cur[n].gy = s_fake_y, cur[n].down = 1;
        n++;
    }
    if (n)
        s_game_touched = 1;

    // Dispatched down/move
    for (int i = 0; i < n; i++) {
        int was_down = 0;
        for (int j = 0; j < last_frame_touch_count; j++) {
            if (last_frame_touches[j].id == cur[i].id) {
                was_down = 1;
                break;
            }
        }
        if (!was_down) {
            static int logged;
            if (logged++ < 16)
                l_info("[INPUT] touch down id=%d raw=%d,%d -> game %d,%d", cur[i].id, cur[i].x, cur[i].y,
                       cur[i].gx, cur[i].gy);
            call_guest((void *)s_touch_down, cur[i].gx, cur[i].gy, (uint32_t)touch_obj(cur[i].id), 0);
        } else {
            call_guest((void *)s_touch_move, cur[i].gx, cur[i].gy, (uint32_t)touch_obj(cur[i].id), 0);
        }
    }

    // Dispatched up
    for (int j = 0; j < last_frame_touch_count; j++) {
        int still_down = 0;
        for (int i = 0; i < n; i++) {
            if (cur[i].id == last_frame_touches[j].id) {
                still_down = 1;
                break;
            }
        }
        if (!still_down) {
            call_guest((void *)s_touch_up, last_frame_touches[j].gx, last_frame_touches[j].gy, (uint32_t)touch_obj(last_frame_touches[j].id), 0);
        }
    }

    memcpy(last_frame_touches, cur, n * sizeof(cur[0]));
    last_frame_touch_count = n;
}

// On-screen touch controls hidden (L + R / options menu): done in gles.c (texture of the control batches drawn
// at 0 % opacity). Logs 035-041 ruled out here: the AI "hide buttons" message (hides the whole interface), the
// GFX interface items (deferred dials), magma::PageStack::Draw (draws nothing) and the virtual DrawButton
// methods (dead code: TouchManager::DrawButtons is an empty function).

// "TOCA LA PANTALLA PARA CONTINUAR" screens (title and level loading, screenshots 2026-11-24 23:23): CROSS or
// START taps the screen centre there. Level loading = CMainGame::IsDrawLevelLoading() (0xad34). The title has no
// such flag: it is the first screen that waits for a touch, so the rule holds until the game gets its first
// touch (outside the logo movies, which CROSS skips on its own).
// Level loading: _s_isLevelLoading != 0 while loading. Log 042: on the "touch to continue" screen it is already
// 0; that screen is _s_LevelLoadingEnd == 2 (s_OneTrame skips the game update while it is 2).
static int *s_level_loading, *s_level_loading_end;
static int level_loading_state(void) {
    if (!s_level_loading)
        s_level_loading = (int *)macho_sym("_s_isLevelLoading"), s_level_loading_end = (int *)macho_sym("_s_LevelLoadingEnd");
    return (s_level_loading && *s_level_loading) || (s_level_loading_end && *s_level_loading_end == 2);
}

static int waits_for_tap(void) {
    if (s_is_movie_playing)
        return 0;
    return !s_game_touched || level_loading_state() != 0;
}

// Trophies (psvita-trophies, comm_id POPW00001): the game keeps its 13 achievements as bits of
// _sul_Achievements[0] (Achievement_Is_Attend / Achievement_Do_Attend, 0xd8264 / 0xd8744; saved in the profile).
// Engine id -> achievement as in EnGameswf::ShowArchivement: ids 0..9 -> 0..9, 11..13 -> 10..12 (10 and 15 are
// internal, no popup). Trophy = achievement + 1 (0 = platinum, unlocked by the OS). Polling the mask about once a
// second also brings in achievements earned before trophies existed (old saves).
static void trophies_sync(void) {
    static uint32_t *mask, done, tick;
    if (++tick % 60)
        return;
    if (!mask)
        mask = (uint32_t *)macho_sym("_sul_Achievements");
    if (!mask)
        return;
    uint32_t m = *mask & 0x3bff & ~done; // ids 0..9 and 11..13
    for (int id = 0; id < 14; id++)
        if (m & (1u << id)) {
            int trophy = id < 10 ? id + 1 : id; // 11..13 -> trophies 11..13 (achievements 10..12)
            l_info("[TROPHY] achievement id %d -> trophy %d", id, trophy);
            trophies_unlock(trophy);
            done |= 1u << id;
        }
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

    // Vita L/R are SCE_CTRL_LTRIGGER/RTRIGGER (0x100/0x200); SCE_CTRL_L1/R1 (0x400/0x800) only exist on
    // DS3/DS4 pads (PS TV). Log 032: checking only L1/R1 meant L, R and L + R never did anything.
    uint32_t b = pad.buttons;
    static uint32_t raw_prev;
    uint32_t pressed = b & ~raw_prev;
    raw_prev = b;
    if (waits_for_tap()) {
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
            s_fake_tap = 1, s_fake_x = 240, s_fake_y = 160;
            l_info("[INPUT] CROSS/START on a touch-to-continue screen (loading %d, end %d, touched %d)",
                   s_level_loading ? *s_level_loading : -1, s_level_loading_end ? *s_level_loading_end : -1,
                   s_game_touched);
        }
        b &= ~(SCE_CTRL_CROSS | SCE_CTRL_START); // no jump / pause on these screens
    }
    if (b & SCE_CTRL_L1)
        b |= SCE_CTRL_LTRIGGER;
    if (b & SCE_CTRL_R1)
        b |= SCE_CTRL_RTRIGGER;
    static combo_gate lr = {SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER}, menu = {SCE_CTRL_SELECT, SCE_CTRL_START};
    if (combo_filter(&lr, &b)) { // L + R toggles the on-screen touch controls (hidden by default)
        opt_hud_hidden = !opt_hud_hidden;
        options_save();
        l_info("[HUD] touch controls %s", opt_hud_hidden ? "hidden" : "visible");
    }
    if (combo_filter(&menu, &b))
        options_open(); // gles_present runs the menu with the game paused
    // START = tap: in game on the on-screen pause button (5,11)-(37,43) (log 046: the INO pause bit alone was
    // ignored until that button had been touched once); with the MenuFX pause menu on screen on "CONTINUAR"
    // (screenshot 2026-11-25 00:00: centre 480,172 on screen = 240,86). Log 047: TouchManager+8 is 2 both in
    // game and paused, so the menu is detected from hook_EnGameswf_Render instead.
    if (s_menu_frames)
        s_menu_frames--;
    trophies_sync();
    static int prev_start, start_tapped;
    int start = (b & SCE_CTRL_START) != 0;
    if (start && !prev_start) {
        static int logs;
        if (logs < 8)
            logs++, l_info("[INPUT] START -> tap on %s", s_menu_frames ? "CONTINUAR" : "pause button");
        start_tapped = 1;
        s_fake_tap = 1;
        s_fake_x = s_menu_frames ? 240 : 21, s_fake_y = s_menu_frames ? 86 : 27;
    }
    prev_start = start;
    if (!start)
        start_tapped = 0;
    if (start_tapped) // the tap pauses / resumes; do not also send the pause bit
        b &= ~SCE_CTRL_START;
    uint32_t btn_mask = 0;
    for (int i = 0; i < NACT; i++)
        if (b & s_act[i].buttons)
            btn_mask |= s_act[i].bit;
    *cur |= btn_mask;
    static int pause_logs;
    if ((btn_mask & 0x200) && !(*prev & 0x200) && pause_logs < 6) // ponytail: verify START reaches the game
        pause_logs++, l_info("[INPUT] START -> INO bit 9 (held mask 0x%08x)", (unsigned)*(uint32_t *)(MACHO_SLIDE + GSL_HELD_MASK));

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
    if (fx == menu)
        s_menu_frames = 4; // MenuFX on screen (pause menu); counted down by the joystick hook
    call_guest(fx_render, (uint32_t)fx, 0, 0, 0);
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
    // updateTimerPages() keeps its original logo waits (420 + 250 frames): gles.c draws both images.
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

