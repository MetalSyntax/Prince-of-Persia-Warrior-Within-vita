/**
 * @file gles.c
 * @brief OpenGL ES 1.1 pipeline translation over vitaGL.
 * @details The game renders into a virtual 320x480 portrait renderbuffer created on iOS by EAGLView
 *          (with internal projection rotation to landscape). Here that renderbuffer is an FBO with a texture
 *          of the same size: viewport, scissor, and glCopyTexImage2D remain in iOS coordinate space untouched.
 *          During presentation (gles_present), the FBO is drawn rotated and scaled onto the native 960x544 screen.
 *
 *          All imported gl* functions take only integer/pointer arguments (*x variants): no SoftFP issues.
 *          GL_FIXED vertex arrays are passed to vitaGL as-is (it converts them in the shader).
 */
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "objc.h"

#define FB_W 320
#define FB_H 480
#define SCR_W 960
#define SCR_H 544
// Fullscreen (as in Hero of Sparta II): the 480x320 game image is stretched to the whole 960x544
// screen (x2 horizontally, x1.7 vertically). input.c maps touches with the same stretch.
#define DST_H SCR_H
#define DST_W SCR_W
#define DST_X 0

#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43

static GLuint fbo, fbo_tex;

// Cached state overwritten during blit that glGet cannot fully query:
typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
} array_ptr;
static array_ptr vtx, tc[2];
static int client_unit; // glClientActiveTexture - GL_TEXTURE0
static GLint env_mode = GL_MODULATE;
static GLfixed color[4] = {0x10000, 0x10000, 0x10000, 0x10000};

// ---------------------------------------------------------------- vertex arrays
// vitaGL takes GL_FIXED pointers natively (shader-side conversion). The old CPU float conversion
// only ran in glDrawArrays: glDrawElements (the 2D UI lib: RenderRegions, FillRect...) drew with a
// stale pointer, so menu buttons were invisible (log 017). State is mirrored for gles_present.

static void apply_vertex(void) {
    if (vtx.size)
        glVertexPointer(vtx.size, vtx.type, vtx.stride, vtx.ptr);
}

static void apply_texcoord(int unit) {
    array_ptr *a = &tc[unit];
    if (!a->size)
        return;
    glClientActiveTexture(GL_TEXTURE0 + unit);
    glTexCoordPointer(a->size, a->type, a->stride, a->ptr);
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
}

static void g_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    vtx = (array_ptr){size, type, stride, ptr};
    apply_vertex();
}

static void g_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    tc[client_unit] = (array_ptr){size, type, stride, ptr};
    apply_texcoord(client_unit);
}

static void g_glClientActiveTexture(GLenum unit) {
    client_unit = (unit - GL_TEXTURE0) & 1;
    glClientActiveTexture(unit);
}

static uint32_t gles_draw_calls;

uint32_t gles_get_draw_calls(void) {
    return gles_draw_calls;
}

// gameswf render in progress: 1 = in-game HUD movie, 2 = MenuFX menu (pause etc.). Set by patch.c.
int gles_in_swf;

// ---------------------------------------------------------------- touch controls (L + R)
// The on-screen controls are hidden at draw level like in Hero of Sparta II: in 2D (orthographic)
// quad draws using a HUD texture, every quad sitting on a control (joystick, jump, block, attack,
// grab; 480x320 game space, measured on console screenshots) collapses to a point. HUD textures are
// learned from a square control-sized sprite centered on a control, so subtitles, menus and the
// health/sand dial are never touched. Touch input is unaffected (read before drawing).
int gles_hud_hidden = 1;
static const struct { float x, y, r; } hud_spots[] = {
    {77, 246, 58},  // virtual joystick (ring + arrows + knob travel)
    {446, 174, 30}, // jump
    {387, 221, 30}, // block (shield)
    {340, 279, 30}, // attack (sword)
    {458, 288, 32}, // grab (hand)
};
#define NSPOTS (sizeof(hud_spots) / sizeof(hud_spots[0]))
static GLuint hud_tex[4];
static float *hud_buf;
static int hud_cap;

static void g_glDeleteTextures(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (unsigned i = 0; i < 4; i++)
            if (hud_tex[i] == ids[k])
                hud_tex[i] = 0;
    glDeleteTextures(n, ids);
}

static float vtx_get(int i, int c) {
    int es = vtx.type == GL_SHORT ? 2 : vtx.type == GL_BYTE ? 1 : 4;
    const uint8_t *p = (const uint8_t *)vtx.ptr + i * (vtx.stride ? vtx.stride : vtx.size * es) + c * es;
    switch (vtx.type) {
    case GL_FLOAT: return *(const float *)p;
    case GL_FIXED: return *(const int32_t *)p * (1.0f / 65536.0f);
    case GL_SHORT: return *(const int16_t *)p;
    default: return *(const int8_t *)p;
    }
}

static int hud_spot(float cx, float cy) {
    for (unsigned s = 0; s < NSPOTS; s++) {
        float dx = cx - hud_spots[s].x, dy = cy - hud_spots[s].y;
        if (dx * dx + dy * dy <= hud_spots[s].r * hud_spots[s].r)
            return s;
    }
    return -1;
}

// ponytail: calibration log (log 032: nothing was hidden and no quad batch matched). Until a HUD
// texture is learned, logs up to 24 small draws (<= 64 vertices; VBO, first and projection not
// filtered) whose first vertex lands on a control, so the filter can be matched to how they draw.
static void hud_calib(GLenum mode, GLint first, int n, int count) {
    static int calib;
    if (hud_tex[0] || calib >= 24 || !gles_hud_hidden || gles_in_swf == 2 || n > 64 || !vtx.size)
        return;
    GLint ab = 0, t = 0;
    float p[16], v[16];
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    if (ab || !vtx.ptr) { // VBO: position unknown here, log the draw shape once per kind
        static int vbo_logged;
        if (vbo_logged++ < 4)
            l_info("[HUD] calib: VBO draw mode=%x verts=%d count=%d vbo=%d swf=%d", mode, n, count, ab, gles_in_swf);
        return;
    }
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    glGetFloatv(GL_MODELVIEW_MATRIX, v);
    float x = vtx_get(first, 0), y = vtx_get(first, 1), z = vtx.size > 2 ? vtx_get(first, 2) : 0;
    float e[4];
    for (int i = 0; i < 4; i++)
        e[i] = v[i] * x + v[4 + i] * y + v[8 + i] * z + v[12 + i];
    float c[4];
    for (int i = 0; i < 4; i++)
        c[i] = p[i] * e[0] + p[4 + i] * e[1] + p[8 + i] * e[2] + p[12 + i] * e[3];
    if (c[3] == 0)
        return;
    int gx = (int)(240 * (1 - c[1] / c[3])), gy = (int)(160 * (1 - c[0] / c[3]));
    if (hud_spot(gx, gy) < 0)
        return;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    calib++;
    l_info("[HUD] calib: mode=%x first=%d verts=%d count=%d vfmt=%d/%x/%d tex=%d swf=%d pm11=%.2f pm15=%.2f v0 at (%d,%d)",
           mode, first, n, count, vtx.size, vtx.type, vtx.stride, t, gles_in_swf, p[11], p[15], gx, gy);
}

// Returns the vertex array to draw with (hidden quads collapsed) or NULL to draw the game's own.
static const void *hud_filter(GLenum mode, int n, int count) {
    if (!gles_hud_hidden || gles_in_swf == 2 || !vtx.ptr || vtx.size < 2 || n > 4096)
        return NULL;
    if (n < 4 || (n & 3))
        return NULL;
    // Quad batches only: indexed triangles (6 indices per 4 vertices), 4-vertex strip/fan sprites.
    if (!(mode == GL_TRIANGLES && count == n / 4 * 6) && !((mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) && n == 4))
        return NULL;
    GLint ab = 0, tex = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    if (ab)
        return NULL;
    float pm[16], mv[16], m[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    if (pm[15] == 0) // perspective (3D scene)
        return NULL;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    int known = 0;
    for (unsigned i = 0; i < 4; i++)
        known |= tex && hud_tex[i] == (GLuint)tex;
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            m[c * 4 + r] = pm[r] * mv[c * 4] + pm[4 + r] * mv[c * 4 + 1] + pm[8 + r] * mv[c * 4 + 2] + pm[12 + r] * mv[c * 4 + 3];
    int hidden = 0;
    for (int q = 0; q < n; q += 4) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = q; i < q + 4; i++) {
            float x = vtx_get(i, 0), y = vtx_get(i, 1), z = vtx.size > 2 ? vtx_get(i, 2) : 0;
            float nx = m[0] * x + m[4] * y + m[8] * z + m[12], ny = m[1] * x + m[5] * y + m[9] * z + m[13];
            // NDC -> 480x320 landscape game space (inverse of the blit in gles_present).
            float gx = 240 * (1 - ny), gy = 160 * (1 - nx);
            x0 = gx < x0 ? gx : x0, x1 = gx > x1 ? gx : x1, y0 = gy < y0 ? gy : y0, y1 = gy > y1 ? gy : y1;
        }
        float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (w > 120 || h > 120)
            continue;
        int s = hud_spot(cx, cy);
        if (s < 0)
            continue;
        if (!known) {
            // Learn: a square control-sized sprite centered on a control.
            float dx = cx - hud_spots[s].x, dy = cy - hud_spots[s].y;
            if (!tex || w < 24 || h < 24 || w > 1.3f * h || h > 1.3f * w || dx * dx + dy * dy > 12 * 12)
                continue;
            for (unsigned i = 0; i < 4; i++)
                if (!hud_tex[i]) {
                    hud_tex[i] = tex;
                    break;
                }
            known = 1;
            l_info("[HUD] control texture %d learned from a %dx%d sprite at (%d,%d)", tex, (int)w, (int)h, (int)cx, (int)cy);
            q = -4; // rescan this batch now that the texture is known
            hidden = 0;
            continue;
        }
        if (!hidden) {
            if (n * vtx.size > hud_cap) {
                hud_cap = n * vtx.size;
                hud_buf = realloc(hud_buf, hud_cap * sizeof(float));
            }
            for (int i = 0; i < n; i++)
                for (int c = 0; c < vtx.size; c++)
                    hud_buf[i * vtx.size + c] = vtx_get(i, c);
        }
        hidden++;
        for (int i = q + 1; i < q + 4; i++)
            memcpy(&hud_buf[i * vtx.size], &hud_buf[q * vtx.size], vtx.size * sizeof(float));
    }
    if (!hidden)
        return NULL;
    glVertexPointer(vtx.size, GL_FLOAT, 0, hud_buf);
    return hud_buf;
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    hud_calib(mode, first, first + count, count);
    const void *h = first ? NULL : hud_filter(mode, count, count);
    glDrawArrays(mode, first, count);
    if (h)
        apply_vertex();
}

static void g_glGetPointerv(GLenum pname, void **out) {
    if (pname == GL_VERTEX_ARRAY_POINTER)
        *out = (void *)vtx.ptr;
    else if (pname == GL_TEXTURE_COORD_ARRAY_POINTER)
        *out = (void *)tc[client_unit].ptr;
    else
        glGetPointerv(pname, out);
}

// ---------------------------------------------------------------- *x state

static void g_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    color[0] = r, color[1] = g, color[2] = b, color[3] = a;
    glColor4x(r, g, b, a);
}

static void g_glTexEnvi(GLenum target, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_ENV_MODE)
        env_mode = v;
    glTexEnvi(target, pname, v);
}

static void g_glTexEnvxv(GLenum target, GLenum pname, const GLfixed *v) {
    if (pname == GL_TEXTURE_ENV_MODE) // enum value, not fixed point
        return g_glTexEnvi(target, pname, v[0]);
    glTexEnvxv(target, pname, (GLfixed *)v);
}

// Texture parameters used by the engine are enums (filters, wrap modes): pass unscaled.
static void g_glTexParameterx(GLenum target, GLenum pname, GLfixed v) { glTexParameteri(target, pname, v); }

static void g_glGetFixedv(GLenum pname, GLfixed *out) {
    float f[16];
    int n = 1;
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX)
        n = 16;
    else if (pname == GL_CURRENT_COLOR) {
        memcpy(out, color, sizeof(color));
        return;
    }
    glGetFloatv(pname, f);
    for (int i = 0; i < n; i++)
        out[i] = (GLfixed)(f[i] * 65536.0f);
}

static void g_glGetLightxv(GLenum light, GLenum pname, GLfixed *out) { memset(out, 0, 4 * sizeof(GLfixed)); }

// ---------------------------------------------------------------- EAGLView framebuffer emulation

static GLuint next_name = 1;
static void g_glGenNames(GLsizei n, GLuint *ids) {
    for (int i = 0; i < n; i++)
        ids[i] = next_name++;
}
static void g_glBindFramebufferOES(GLenum target, GLuint fb) { glBindFramebuffer(GL_FRAMEBUFFER, fbo); }
static void g_glNoop(void) {}
static GLenum g_glCheckFramebufferStatusOES(GLenum target) { return GL_FRAMEBUFFER_COMPLETE_OES; }
static void g_glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *v) {
    *v = pname == GL_RENDERBUFFER_WIDTH_OES ? FB_W : pname == GL_RENDERBUFFER_HEIGHT_OES ? FB_H : 0;
}

// ---------------------------------------------------------------- init / present

void gles_init(void) {
    vglUseTripleBuffering(GL_FALSE);
    vglInitExtended(0, SCR_W, SCR_H, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FB_W, FB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    // Depth + stencil: gameswf (menu UI) clips with stencil masks (GL_EQUAL on the mask depth).
    // Without a stencil buffer every masked shape failed the test -> no buttons (log 018).
    GLuint rb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, FB_W, FB_H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, FB_W, FB_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // RGB565 semantics, see g_glColorMask
    l_info("vitaGL: FBO %ux%u -> screen %ux%u at x=%d", FB_W, FB_H, DST_W, DST_H, DST_X);
}

void gles_present(void) {
    static const GLenum caps[] = {GL_BLEND, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_TEXTURE_2D};
    GLboolean on[5], tca, va, ca, na;
    GLint tex, mode, active, vp[4];
    for (int i = 0; i < 5; i++)
        on[i] = glIsEnabled(caps[i]);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    tca = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    va = glIsEnabled(GL_VERTEX_ARRAY);
    ca = glIsEnabled(GL_COLOR_ARRAY);
    na = glIsEnabled(GL_NORMAL_ARRAY);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetIntegerv(GL_VIEWPORT, vp);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCR_W, SCR_H);
    for (int i = 0; i < 4; i++)
        glDisable(caps[i]);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT);
    // Screen corners (top-left, top-right, bottom-left, bottom-right) -> vertical FBO texture.
    // Assumed landscape-right rotation (top edge of FBO aligns to left edge of screen).
    // Adjust UV mapping table if target game renders mirrored or inverted.
    static const float uv[] = {1, 1, 1, 0, 0, 1, 0, 0};
    const float xy[] = {DST_X, 0, DST_X + DST_W, 0, DST_X, SCR_H, DST_X + DST_W, SCR_H};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vglSwapBuffers(ime_active()); // GL_TRUE while system dialog is active

    // Restore guest OpenGL state.
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(mode);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode);
    for (int i = 0; i < 5; i++)
        on[i] ? glEnable(caps[i]) : glDisable(caps[i]);
    if (!va)
        glDisableClientState(GL_VERTEX_ARRAY);
    if (!tca)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca)
        glEnableClientState(GL_COLOR_ARRAY);
    if (na)
        glEnableClientState(GL_NORMAL_ARRAY);
    glColor4x(color[0], color[1], color[2], color[3]);
    apply_vertex();
    apply_texcoord(0);
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
    glActiveTexture(active);
}

// iOS draws into a kEAGLColorFormatRGB565 renderbuffer: no alpha, so GL_DST_ALPHA reads as 1.
// RenderState::Send blends with GL_DST_ALPHA / GL_ONE_MINUS_DST_ALPHA; with the RGBA8 FBO the
// character's texture alpha stayed in the buffer and later passes showed it see-through (log 027).
// Keep FBO alpha at 1: never write alpha, always clear it to 1.
static void g_glClearColorx(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    glClearColor((float)r / 65536.0f, (float)g / 65536.0f, (float)b / 65536.0f, 1.0f);
}

static void g_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glClearColor(r, g, b, 1.0f); }

static void g_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask(r, g, b, GL_FALSE); }

static void g_glTexEnvx(GLenum target, GLenum pname, GLfixed param) {
    if (pname == GL_TEXTURE_ENV_MODE)
        glTexEnvi(target, pname, param);
    else
        glTexEnvf(target, pname, (float)param / 65536.0f);
}

static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    gles_draw_calls++;
    const void *h = NULL;
    GLint eb = 0;
    float pm[16];
    if (gles_hud_hidden) {
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
        glGetFloatv(GL_PROJECTION_MATRIX, pm);
    }
    // Index scan only for 2D (orthographic) draws, or small ones for the calibration log.
    if (gles_hud_hidden && !eb && (pm[15] != 0 || (!hud_tex[0] && count <= 96)) && indices &&
        (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_BYTE)) {
        int n = 0;
        for (int i = 0; i < count; i++) {
            int v = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : ((const uint16_t *)indices)[i];
            if (v >= n)
                n = v + 1;
        }
        hud_calib(mode, 0, n, count);
        h = hud_filter(mode, n, count);
    }
    glDrawElements(mode, count, type, indices);
    if (h)
        apply_vertex();
}

// MacIndexBuffer::Init treats any error after glGenBuffers as "no VBO" and skips glBufferData,
// then binds the empty buffer anyway -> NULL index read in vitaGL. Errors vitaGL raises that iOS
// doesn't must not leak into the game: log them and report GL_NO_ERROR.
static GLenum g_glGetError(void) {
    GLenum e = glGetError();
    static int logged;
    if (e != GL_NO_ERROR && logged < 16) {
        logged++;
        l_warn("glGetError: vitaGL 0x%04X swallowed", e);
    }
    return GL_NO_ERROR;
}

static void g_glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) {
    GLint iv[4] = {0};
    glGetTexEnviv(target, pname, iv);
    params[0] = (GLfloat)iv[0];
}

// ---------------------------------------------------------------- import table

#define F(name, fn) {"_" #name, (uintptr_t)(fn)}
#define D(name) {"_" #name, (uintptr_t)(name)}

const import_entry gles_imports[] = {
    D(glActiveTexture), D(glBindTexture), D(glBlendFunc), D(glClear), D(glCopyTexImage2D),
    D(glCopyTexSubImage2D), F(glDeleteTextures, g_glDeleteTextures), D(glDepthMask), D(glDisable), D(glDisableClientState),
    D(glEnable), D(glEnableClientState), D(glGenTextures), D(glGetIntegerv), D(glIsEnabled),
    D(glLoadIdentity), D(glLoadMatrixx), D(glMatrixMode), D(glOrthox), D(glPopMatrix), D(glPushMatrix),
    D(glRotatex), D(glScalex), D(glScissor), D(glShadeModel), D(glTexImage2D), D(glTexParameteri),
    D(glTexSubImage2D), D(glTranslatex), D(glViewport),
    D(glAlphaFunc), D(glBindBuffer), D(glBufferData), D(glBufferSubData),
    F(glClearColor, g_glClearColor), D(glClearDepthf), D(glClearStencil), D(glColor4ub),
    F(glColorMask, g_glColorMask), D(glColorPointer), D(glCompressedTexImage2D), D(glDeleteBuffers),
    D(glDepthFunc), D(glFogf), D(glFogfv), D(glFrontFace), D(glFrustumf),
    D(glGenBuffers), F(glGetError, g_glGetError), D(glHint), D(glLineWidth), D(glLoadMatrixf),
    D(glMultMatrixf), D(glNormalPointer), D(glPixelStorei), D(glPointSize),
    D(glPolygonOffset), D(glReadPixels), D(glRotatef), D(glScalef),
    D(glStencilFunc), D(glStencilMask), D(glStencilOp), D(glTexEnvf),
    D(glTexEnvfv), D(glTranslatef),
    F(glClearColorx, g_glClearColorx), F(glTexEnvx, g_glTexEnvx),
    F(glDrawElements, g_glDrawElements), F(glGetTexEnvfv, g_glGetTexEnvfv),
    F(glColor4x, g_glColor4x), F(glTexEnvi, g_glTexEnvi), F(glTexEnvxv, g_glTexEnvxv),
    F(glTexParameterx, g_glTexParameterx), F(glGetFixedv, g_glGetFixedv), F(glGetLightxv, g_glGetLightxv),
    F(glVertexPointer, g_glVertexPointer), F(glTexCoordPointer, g_glTexCoordPointer),
    F(glClientActiveTexture, g_glClientActiveTexture), F(glDrawArrays, g_glDrawArrays),
    F(glGetPointerv, g_glGetPointerv),
    F(glGenFramebuffersOES, g_glGenNames), F(glGenRenderbuffersOES, g_glGenNames),
    F(glBindFramebufferOES, g_glBindFramebufferOES), F(glBindRenderbufferOES, g_glNoop),
    F(glDeleteFramebuffersOES, g_glNoop), F(glDeleteRenderbuffersOES, g_glNoop),
    F(glRenderbufferStorageOES, g_glNoop), F(glFramebufferRenderbufferOES, g_glNoop),
    F(glCheckFramebufferStatusOES, g_glCheckFramebufferStatusOES),
    F(glGetRenderbufferParameterivOES, g_glGetRenderbufferParameterivOES),
};
const int gles_imports_count = sizeof(gles_imports) / sizeof(gles_imports[0]);
