// Menú de opciones (SELECT+START), menú v2 de HAWX-vita (base: menú de video de Ferrari-GT-Evolution-vita, al estilo
// de Zenonia2-vita): panel con la fuente 8x8 de PSPSDK en una textura translúcida sobre el juego, que queda en pausa
// (gles_present no vuelve hasta cerrar). Filas: una por acción (X: apretar el botón nuevo, triángulo: sin botón; si
// el botón ya lo usa otra acción, se intercambian), controles en pantalla (L+R), tamaño, filtro, MSAA, FPS, límite.
// Se guarda en DATA_PATH options.txt (los botones en controls.txt), editable a mano:
//     filter=bilinear (bilinear | sharp)   msaa=0 (ignorado, ver options_load)   stretch=1 (0 = aspecto original)
//     show_fps=0   fps_cap=0 (0 | 30)   hud_hidden=1
//     device=iPhone2,1  (hw.machine the game sees, as in Asphalt-Audi-RS3-iOS-vita; applies after restarting)
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vitaGL.h>

#include "hle.h"
#include "log.h"
#include "options.h"

#define OPTIONS_PATH DATA_PATH "options.txt"

extern const unsigned char font8x8[];

// Valores por defecto = lo verificado en consola antes del menú (v1.0.0): bilineal, pantalla completa, HUD oculto.
int opt_filter = FILTER_BILINEAR, opt_msaa = 0, opt_msaa_active = 0, opt_stretch = 1;
int opt_show_fps = 0, opt_fps_cap = 0, opt_hud_hidden = 1;
static volatile int open_req;

static const char *filter_names[] = {"bilinear", "sharp"};
// Emulated iPhone (hw.machine). PoP WW only checks it once at launch: "iPhone2,1" sets g_is3GS (used by the
// menu page logic, UpdatePage); graphics do not depend on it. Default = the 3GS profile verified on console.
static const char *devices[][2] = {{"iPhone1,1", "IPHONE 2G"}, {"iPhone1,2", "IPHONE 3G"}, {"iPod2,1", "IPOD TOUCH 2G"},
                                   {"iPhone2,1", "IPHONE 3GS"}, {"iPod3,1", "IPOD TOUCH 3G"}};
#define NDEV (int)(sizeof(devices) / sizeof(devices[0]))
#define DEV_DEFAULT 3
static int opt_device = DEV_DEFAULT, opt_device_active = DEV_DEFAULT;
const char *options_machine(void) { return devices[opt_device_active][0]; }
static const char *filter_labels[] = {"BILINEAR (SMOOTH)", "SHARP BILINEAR"};

void options_load(void) {
    FILE *f = fopen(OPTIONS_PATH, "r");
    if (f) {
        char line[64], s[16];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "filter=%15s", s) == 1)
                opt_filter = !strcmp(s, "sharp") ? FILTER_SHARP : FILTER_BILINEAR;
            // msaa= is ignored: log 036, with msaa=4 the game quit (exit(1)) right after launch, before drawing.
            else if (sscanf(line, "stretch=%d", &v) == 1)
                opt_stretch = !!v;
            else if (sscanf(line, "show_fps=%d", &v) == 1)
                opt_show_fps = !!v;
            else if (sscanf(line, "fps_cap=%d", &v) == 1)
                opt_fps_cap = v == 30 ? 30 : 0;
            else if (sscanf(line, "hud_hidden=%d", &v) == 1)
                opt_hud_hidden = !!v;
            else if (sscanf(line, "device=%15s", s) == 1)
                for (int i = 0; i < NDEV; i++)
                    if (!strcmp(s, devices[i][0]))
                        opt_device = i;
        }
        fclose(f);
    }
    opt_msaa_active = opt_msaa;
    opt_device_active = opt_device;
    l_info("options.txt: iPhone profile %s", devices[opt_device][0]);
    l_info("options.txt: filtro %s, MSAA %dx, pantalla completa %d, fps %d (límite %d), controles ocultos %d",
           filter_names[opt_filter], opt_msaa, opt_stretch, opt_show_fps, opt_fps_cap, opt_hud_hidden);
}

void options_save(void) {
    FILE *f = fopen(OPTIONS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "filter=%s\nmsaa=%d\nstretch=%d\nshow_fps=%d\nfps_cap=%d\nhud_hidden=%d\n", filter_names[opt_filter],
            opt_msaa, opt_stretch, opt_show_fps, opt_fps_cap, opt_hud_hidden);
    fprintf(f, "device=%s\n", devices[opt_device][0]);
    fclose(f);
}

int options_msaa_mode(void) {
    return opt_msaa == 4 ? SCE_GXM_MULTISAMPLE_4X : opt_msaa == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
}

void options_open(void) { open_req = 1; }
int options_pending(void) { return open_req; }

// ---------------------------------------------------------------- texto en texturas (fuente 8x8)

#define C_BG 0xD0201008 // ABGR (GL_RGBA/UNSIGNED_BYTE en little endian); alfa = translúcido
#define C_SEL 0xFF603818
#define C_TEXT 0xFFE0E0E0
#define C_DIM 0xFF909090
#define C_HEAD 0xFF40C0FF
#define C_EDIT 0xFF40FFFF

static void fill(uint32_t *px, int w, int y, int h, uint32_t c) {
    for (int i = y * w; i < (y + h) * w; i++)
        px[i] = c;
}
static void text(uint32_t *px, int w, int x, int y, uint32_t c, const char *s) {
    for (; *s && x + 8 <= w; s++, x += 8)
        for (int r = 0; r < 8; r++) {
            unsigned char bits = font8x8[(unsigned char)*s * 8 + r];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    px[(y + r) * w + x + col] = c;
        }
}
// Textura w x h dibujada en (x, y, sw, sh) de la pantalla; la proyección del present es 0..960 x 0..544, y hacia abajo.
static void blit(GLuint *tex, const uint32_t *px, int w, int h, float x, float y, float sw, float sh) {
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const float xy[] = {x, y, x + sw, y, x, y + sh, x + sw, y + sh};
    static const float uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // gles_present restaura la función de mezcla del juego
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

// ---------------------------------------------------------------- botones de la Vita
// Sin cruceta: en PoP mueve al príncipe (igual que el stick izquierdo).

static const struct {
    const char *name;
    uint32_t mask;
} pads[] = {
    {"CROSS", SCE_CTRL_CROSS}, {"CIRCLE", SCE_CTRL_CIRCLE},   {"SQUARE", SCE_CTRL_SQUARE}, {"TRIANGLE", SCE_CTRL_TRIANGLE},
    {"L", SCE_CTRL_LTRIGGER},  {"R", SCE_CTRL_RTRIGGER},      {"SELECT", SCE_CTRL_SELECT}, {"START", SCE_CTRL_START},
};
#define NPADS (int)(sizeof(pads) / sizeof(pads[0]))

uint32_t options_button_mask(const char *name) {
    if (!strcasecmp(name, "L1"))
        name = "L";
    else if (!strcasecmp(name, "R1"))
        name = "R";
    for (int i = 0; i < NPADS; i++)
        if (!strcasecmp(name, pads[i].name))
            return pads[i].mask;
    return 0;
}

const char *options_button_name(uint32_t mask) {
    for (int i = 0; i < NPADS; i++)
        if (mask == pads[i].mask)
            return pads[i].name;
    return NULL;
}

static const char *mask_str(uint32_t m) {
    static char out[96];
    out[0] = 0;
    for (int i = 0; i < NPADS; i++)
        if (m & pads[i].mask) {
            if (out[0])
                strcat(out, "+");
            strcat(out, pads[i].name);
        }
    return out[0] ? out : "(NONE)";
}

static uint32_t pad(void) {
    SceCtrlData c;
    sceCtrlPeekBufferPositive(0, &c, 1);
    return c.buttons;
}

// ---------------------------------------------------------------- panel

enum { R_HUD, R_SIZE, R_FILTER, R_FPS, R_CAP, R_DEVICE, R_CLOSE, R_FIXED };
#define PW 480
#define PH 272
// Allocated only while the menu is open. Log 046: as a 510 KB static buffer, video.cpp's pthread_create for
// the 512 KB decode thread failed from log 035 on (black screen instead of the Gameloft intro).
static uint32_t *px;
static GLuint panel_tex;
static int cursor, capture, binds_dirty;

static int nrows(void) { return input_bind_count() + R_FIXED; }
static int fixed_row(int r) { return r - input_bind_count(); } // fila -> R_*, o < 0 si es una acción

static void render(void) {
    static const char *labels[R_FIXED] = {"On-screen controls (L+R)", "Screen size", "Screen filter",
                                          "FPS counter", "FPS limit", "iPhone profile", "Save & close"};
    int rows = nrows(), nb = input_bind_count();
    char lab[64], val[64];
    fill(px, PW, 0, PH, C_BG);
    text(px, PW, 16, 8, C_HEAD, "OPTIONS");
    for (int r = 0; r < rows; r++) {
        int y = 24 + r * 14, o = fixed_row(r); // 15 filas: hasta y = 234 (ayuda abajo)
        if (r == cursor)
            fill(px, PW, y - 3, 14, C_SEL);
        uint32_t c = C_TEXT;
        if (o < 0) {
            snprintf(lab, sizeof(lab), "%.26s", input_bind_label(r));
            if (r == cursor && capture)
                strcpy(val, "PRESS A BUTTON..."), c = C_EDIT;
            else
                snprintf(val, sizeof(val), "%s", mask_str(input_bind_mask(r)));
        } else {
            strcpy(lab, labels[o]);
            val[0] = 0;
            if (o == R_HUD)
                strcpy(val, opt_hud_hidden ? "HIDDEN" : "SHOWN");
            else if (o == R_SIZE)
                strcpy(val, opt_stretch ? "FULL SCREEN" : "ORIGINAL ASPECT");
            else if (o == R_FILTER)
                strcpy(val, filter_labels[opt_filter]);
            else if (o == R_FPS)
                strcpy(val, opt_show_fps ? "ON" : "OFF");
            else if (o == R_CAP)
                strcpy(val, opt_fps_cap ? "30 FPS" : "OFF (GAME)");
            else if (o == R_DEVICE)
                snprintf(val, sizeof(val), "%s%s", devices[opt_device][1],
                         opt_device != opt_device_active ? " (RESTART)" : "");
        }
        text(px, PW, 16, y, o == R_CLOSE ? C_HEAD : C_TEXT, lab);
        text(px, PW, 232, y, c, val);
    }
    const char *help = cursor < nb ? (capture ? "Press the new button   START: cancel"
                                              : "X: change button   TRIANGLE: no button   O: close")
                                   : "LEFT/RIGHT or X: change   O: save & close";
    text(px, PW, 16, PH - 26, C_DIM, help);
    text(px, PW, 16, PH - 13, C_DIM, opt_device != opt_device_active ? "iPhone profile applies after restarting the game"
                                                                      : "L+R in game: show/hide on-screen controls");
}

static void change(int o, int d) {
    if (o == R_HUD)
        opt_hud_hidden = !opt_hud_hidden;
    else if (o == R_SIZE)
        opt_stretch = !opt_stretch;
    else if (o == R_FILTER)
        opt_filter = !opt_filter;
    else if (o == R_FPS)
        opt_show_fps = !opt_show_fps;
    else if (o == R_CAP)
        opt_fps_cap = opt_fps_cap ? 0 : 30;
    else if (o == R_DEVICE)
        opt_device = (opt_device + d + NDEV) % NDEV;
}

// Botón nuevo para la acción i; si otra acción ya lo usa, recibe el botón viejo de esta (intercambio).
static void assign(int i, uint32_t m) {
    uint32_t old = input_bind_mask(i);
    for (int j = 0; j < input_bind_count(); j++)
        if (j != i && (input_bind_mask(j) & m))
            input_bind_set(j, (input_bind_mask(j) & ~m) | old);
    input_bind_set(i, m);
    binds_dirty = 1;
}

void options_run(void (*draw_game)(void)) {
    open_req = 0, capture = 0, binds_dirty = 0;
    px = malloc(PW * PH * sizeof(*px));
    if (!px) {
        l_warn("menú de opciones: sin memoria para el panel");
        return;
    }
    uint32_t prev = pad(); // SELECT+START siguen apretados
    l_info("menú de opciones abierto");
    for (;;) {
        uint32_t b = pad(), p = b & ~prev;
        prev = b;
        int rows = nrows(), o = fixed_row(cursor);
        if (capture) {
            if (p & SCE_CTRL_START)
                capture = 0;
            else
                for (int i = 0; i < NPADS; i++)
                    if (p & pads[i].mask) {
                        assign(cursor, pads[i].mask);
                        capture = 0;
                        break;
                    }
        } else if (p & SCE_CTRL_CIRCLE || p & SCE_CTRL_START || (p & SCE_CTRL_CROSS && o == R_CLOSE))
            break;
        else if (p & SCE_CTRL_UP)
            cursor = (cursor + rows - 1) % rows;
        else if (p & SCE_CTRL_DOWN)
            cursor = (cursor + 1) % rows;
        else if (o < 0 && p & SCE_CTRL_CROSS)
            capture = 1;
        else if (o < 0 && p & SCE_CTRL_TRIANGLE)
            input_bind_set(cursor, 0), binds_dirty = 1;
        else if (o >= 0 && p & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS))
            change(o, 1);
        else if (o >= 0 && p & SCE_CTRL_LEFT)
            change(o, -1);
        draw_game(); // filtro/tamaño nuevos en vivo
        render();
        blit(&panel_tex, px, PW, PH, 0, 0, 960, 544);
        vglSwapBuffers(GL_FALSE);
    }
    options_save();
    if (binds_dirty)
        input_save_controls();
    l_info("opciones: filtro %s, MSAA %dx (activo %dx), pantalla completa %d, fps %d (límite %d), controles ocultos %d",
           filter_names[opt_filter], opt_msaa, opt_msaa_active, opt_stretch, opt_show_fps, opt_fps_cap, opt_hud_hidden);
    // no devolver el botón de cierre al juego (patch.c lo leería como acción)
    while (pad()) {
        draw_game();
        vglSwapBuffers(GL_FALSE);
    }
    draw_game(); // lo que presenta el swap de gles_present al volver
    free(px);
    px = NULL;
}

// ---------------------------------------------------------------- FPS (contador y límite)

void options_frame(void) {
    static uint64_t t0, last;
    static int frames, shown;
    static GLuint fps_tex;
    static uint32_t fps_px[64 * 8];
    uint64_t now = sceKernelGetProcessTimeWide();
    frames++;
    if (!t0)
        t0 = now;
    else if (now - t0 >= 1000000) {
        shown = (int)(frames * 1e6 / (now - t0) + 0.5);
        frames = 0, t0 = now;
    }
    if (opt_show_fps) {
        char s[16];
        snprintf(s, sizeof(s), "%d FPS", shown);
        fill(fps_px, 64, 0, 8, 0xA0000000);
        text(fps_px, 64, 0, 0, 0xFF40FF40, s);
        blit(&fps_tex, fps_px, 64, 8, 4, 4, 128, 16);
    }
    if (opt_fps_cap) {
        uint64_t frame_us = 1000000 / opt_fps_cap;
        if (last && now - last < frame_us)
            sceKernelDelayThread(frame_us - (now - last));
        last = sceKernelGetProcessTimeWide();
    }
}
