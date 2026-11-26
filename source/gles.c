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
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "options.h"
#include "trophies.h"

#define FB_W 320
#define FB_H 480
#define SCR_W 960
#define SCR_H 544

// Destination rectangle on screen (input.c maps touches back with it). Full screen (default, as in
// Hero of Sparta II) stretches 480x320 to 960x544; original aspect is 816x544 centered (options.c).
void gles_dest_rect(int *x, int *w) {
    *w = opt_stretch ? SCR_W : SCR_H * FB_H / FB_W;
    *x = (SCR_W - *w) / 2;
}

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

// patch.c: 0 % opacity for the on-screen touch controls (the draw still writes depth/stencil like the game expects).
static int mask_off;
static GLboolean saved_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE};
void gles_color_mask_off(int on) {
    mask_off = on;
    if (on)
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    else
        glColorMask(saved_mask[0], saved_mask[1], saved_mask[2], GL_FALSE);
}

// ---------------------------------------------------------------- on-screen controls at 0 % opacity (L + R)
// Logs 040-042: the controls are client-array GL_TRIANGLES batches of ONE texture drawn near the end of the frame
// (box (-9,-9)-(478,312): joystick on the left, buttons on the right; plus small ones such as the pause button).
// The texture is learned from the first batch whose screen box spans both sides inside the screen, then every
// client-array draw with that texture is drawn with the color mask off (touch input does not depend on drawing).
// A deleted texture is unlearned (level change). Logs 035-042 ruled out the earlier hooks (see patch.c).
static GLuint hud_tex;
static int hud_logs;

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

// Screen box of vertices [first, first + n) in 480x320 game space; 0 if a vertex is behind the camera.
static int draw_box(int first, int n, float *b) {
    float p[16], v[16];
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    glGetFloatv(GL_MODELVIEW_MATRIX, v);
    b[0] = b[1] = 1e9f, b[2] = b[3] = -1e9f;
    for (int i = first; i < first + n; i++) {
        float x = vtx_get(i, 0), y = vtx_get(i, 1), z = vtx.size > 2 ? vtx_get(i, 2) : 0, e[4], c[4];
        for (int k = 0; k < 4; k++)
            e[k] = v[k] * x + v[4 + k] * y + v[8 + k] * z + v[12 + k];
        for (int k = 0; k < 4; k++)
            c[k] = p[k] * e[0] + p[4 + k] * e[1] + p[8 + k] * e[2] + p[12 + k] * e[3];
        if (c[3] <= 0)
            return 0;
        float gx = 240 * (1 - c[1] / c[3]), gy = 160 * (1 - c[0] / c[3]);
        b[0] = gx < b[0] ? gx : b[0], b[2] = gx > b[2] ? gx : b[2];
        b[1] = gy < b[1] ? gy : b[1], b[3] = gy > b[3] ? gy : b[3];
    }
    return 1;
}

// Log 044: the texture also holds the health and the held weapon (same 177-vertex batch), so only the triangles
// whose centre sits on a control are hidden. Control spots in 480x320 game space (console screenshots/touch logs).
static const struct { float x, y, r; } hud_spots[] = {
    {77, 246, 58},  // virtual joystick (ring + arrows + knob travel)
    {446, 174, 30}, // jump
    {387, 221, 30}, // block (shield)
    {340, 279, 30}, // attack (sword)
    {458, 288, 32}, // grab (hand)
    {466, 30, 22},  // top right: slow motion (touch log 039, INO 0x80)
    {426, 33, 22},  // top right: INO 0x40 (log 039)
    // nothing on the top left: the health / sands dial is there, (27,27)-(103,99) (log 045 + screenshot)
};
#define NSPOTS (int)(sizeof(hud_spots) / sizeof(hud_spots[0]))
static uint16_t hud_idx[600];

// Triangle centre (vertices first + k*3 .. +2) on a control spot?
static int tri_on_control(const float *m, int v0) {
    float cx = 0, cy = 0;
    for (int i = v0; i < v0 + 3; i++) {
        float x = vtx_get(i, 0), y = vtx_get(i, 1), z = vtx.size > 2 ? vtx_get(i, 2) : 0;
        float c0 = m[0] * x + m[4] * y + m[8] * z + m[12], c1 = m[1] * x + m[5] * y + m[9] * z + m[13];
        float c3 = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (c3 <= 0)
            return 0;
        cx += 240 * (1 - c1 / c3) / 3, cy += 160 * (1 - c0 / c3) / 3;
    }
    for (int s = 0; s < NSPOTS; s++) {
        float dx = cx - hud_spots[s].x, dy = cy - hud_spots[s].y;
        if (dx * dx + dy * dy <= hud_spots[s].r * hud_spots[s].r)
            return 1;
    }
    return 0;
}

// Draws a control-texture batch without the triangles on a control (0 % opacity); returns 0 to draw it as is.
static int hud_draw_filtered(GLenum mode, int first, int count) {
    if (mode != GL_TRIANGLES || count % 3 || count > (int)(sizeof(hud_idx) / sizeof(hud_idx[0])) || first + count > 0xffff)
        return 0;
    float p[16], v[16], m[16];
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    glGetFloatv(GL_MODELVIEW_MATRIX, v);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            m[c * 4 + r] = p[r] * v[c * 4] + p[4 + r] * v[c * 4 + 1] + p[8 + r] * v[c * 4 + 2] + p[12 + r] * v[c * 4 + 3];
    int n = 0, hidden = 0;
    for (int t = first; t < first + count; t += 3) {
        int on = tri_on_control(m, t);
        if (on) {
            hidden++;
            continue;
        }
        hud_idx[n++] = t, hud_idx[n++] = t + 1, hud_idx[n++] = t + 2;
    }
    if (!hidden)
        return 0;
    if (n) {
        GLint eb = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
        if (eb)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, hud_idx);
        if (eb)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
    }
    return 1;
}

// 1 = this client-array batch uses the controls texture (and the controls are hidden).
static int hud_hide(GLenum mode, int first, int count) {
    if (!vtx.ptr || vtx.size < 2)
        return 0;
    GLint ab = 0, t = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    if (ab)
        return 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    if (!hud_tex && t && mode == GL_TRIANGLES && count >= 60 && count <= 600) {
        float b[4];
        if (draw_box(first, count, b) && b[0] > -40 && b[1] > -40 && b[2] < 520 && b[3] < 360 && b[0] < 120 &&
            b[2] > 360 && b[3] > 240) {
            hud_tex = t;
            l_info("[HUD] controls texture %d learned (%d vertices, box (%d,%d)-(%d,%d))", t, count, (int)b[0],
                   (int)b[1], (int)b[2], (int)b[3]);
        }
    }
    return opt_hud_hidden && hud_tex && (GLuint)t == hud_tex;
}

static void g_glDeleteTextures(GLsizei n, const GLuint *ids) {
    for (int i = 0; i < n; i++)
        if (hud_tex && ids[i] == hud_tex) {
            if (hud_logs++ < 8)
                l_info("[HUD] controls texture %d deleted: relearning", hud_tex);
            hud_tex = 0;
        }
    glDeleteTextures(n, ids);
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    if (hud_hide(mode, first, count) && hud_draw_filtered(mode, first, count))
        return;
    glDrawArrays(mode, first, count);
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
    options_load(); // MSAA is fixed here (vitaGL applies it to the screen and every FBO)
    // RAM left outside vitaGL: 32 MB (was 8). vitaGL is initialized before the Mach-O is loaded, so the image,
    // thread stacks and FFmpeg come out of this margin too. Log 049-050: sceNpTrophyInit failed with 0x80024302
    // (SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE) with 8 MB; log 046: a 510 KB static buffer already broke the
    // video decode thread. vitaGL had ~195 MB free in game (logs 041-042).
    vglInitExtended(0, SCR_W, SCR_H, 32 * 1024 * 1024, options_msaa_mode());
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
    int dx, dw;
    gles_dest_rect(&dx, &dw);
    l_info("vitaGL: FBO %ux%u -> screen %ux%u at x=%d", FB_W, FB_H, dw, SCR_H, dx);
}

// ---------------------------------------------------------------- scaling to screen (options.c picks the filter)
// Sharp bilinear as a fragment shader on the final blit (same math as RetroArch's sharp-bilinear): each texel
// stays a flat block and only the 1-pixel seams between texels are interpolated. Logs 039-042: the former
// version (nearest x2 into an intermediate FBO, from Ferrari) crashed in SceGxm at its first draw into that FBO,
// with plenty of memory and in any frame order (vitaGL ignores a failed render target); no FBO is used now.
// Cg, compiled at runtime by vitaGL with libshacccg (already required for its fixed-function shaders).
static const char sharp_vs[] =
    "void main(float2 position, float2 texcoord, float2 out vTexcoord : TEXCOORD0, float4 out vPos : POSITION) {\n"
    "  vPos = float4(position, 0.0, 1.0);\n"
    "  vTexcoord = texcoord;\n"
    "}\n";
static const char sharp_fs[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex, uniform float2 texSize, uniform float2 scale) {\n"
    "  float2 texel = vTexcoord * texSize;\n"
    "  float2 base = floor(texel);\n"
    "  float2 dist = frac(texel) - 0.5;\n"
    "  float2 range = 0.5 - 0.5 / scale;\n"
    "  float2 f = (dist - clamp(dist, -range, range)) * scale + 0.5;\n"
    "  return tex2D(tex, (base + f) / texSize);\n"
    "}\n";
static GLuint sharp_prog;
static GLint sharp_u_size, sharp_u_scale, sharp_u_tex;
static int sharp_failed;

static GLuint sharp_compile(GLenum type, const char *src) {
    // Log 043: a plain GL_*_SHADER goes through vitaGL's GLSL translator, which crashed on this Cg source
    // (glsl_handle_ubos); the Cg shader types are compiled as Cg directly.
    GLuint sh = glCreateShader(type == GL_VERTEX_SHADER ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT);
    GLint len = (GLint)strlen(src), ok = 0;
    glShaderSource(sh, 1, &src, &len);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char msg[256] = "";
        glGetShaderInfoLog(sh, sizeof(msg), NULL, msg);
        l_warn("sharp bilinear: %s shader failed: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", msg);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int sharp_init(void) {
    if (sharp_prog || sharp_failed)
        return sharp_prog != 0;
    // If compiling crashes (log 043), the next boot must not try again: options.txt says bilinear meanwhile.
    opt_filter = FILTER_BILINEAR;
    options_save();
    GLuint vs = sharp_compile(GL_VERTEX_SHADER, sharp_vs), fs = vs ? sharp_compile(GL_FRAGMENT_SHADER, sharp_fs) : 0;
    GLint ok = 0;
    if (vs && fs) {
        sharp_prog = glCreateProgram();
        glAttachShader(sharp_prog, vs);
        glAttachShader(sharp_prog, fs);
        glBindAttribLocation(sharp_prog, 0, "position");
        glBindAttribLocation(sharp_prog, 1, "texcoord");
        glLinkProgram(sharp_prog);
        glGetProgramiv(sharp_prog, GL_LINK_STATUS, &ok);
    }
    if (!ok) {
        l_warn("sharp bilinear: shader not available, using bilinear");
        sharp_failed = 1, sharp_prog = 0, opt_filter = FILTER_BILINEAR;
        return 0;
    }
    sharp_u_size = glGetUniformLocation(sharp_prog, "texSize");
    sharp_u_scale = glGetUniformLocation(sharp_prog, "scale");
    sharp_u_tex = glGetUniformLocation(sharp_prog, "tex");
    l_info("sharp bilinear: shader ready (uniforms %d %d %d)", sharp_u_size, sharp_u_scale, sharp_u_tex);
    opt_filter = FILTER_SHARP;
    options_save();
    return 1;
}

// Draws the game FBO on the screen (FFP state already set up by gles_present).
static void draw_screen(void) {
    int dx, dw;
    gles_dest_rect(&dx, &dw);
    GLfloat cc[4]; // black side bars in original aspect, then back to the game's clear color
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    // Screen corners (top-left, top-right, bottom-left, bottom-right) -> portrait FBO texture,
    // landscape-right rotation (top edge of FBO aligns to left edge of screen).
    static const float uv[] = {1, 1, 1, 0, 0, 1, 0, 0};
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    if (opt_filter == FILTER_SHARP && sharp_init()) {
        // corners in normalized device coordinates (y up)
        static float ndc[8];
        float x0 = dx * 2.0f / SCR_W - 1, x1 = (dx + dw) * 2.0f / SCR_W - 1;
        float v[8] = {x0, 1, x1, 1, x0, -1, x1, -1};
        memcpy(ndc, v, sizeof(v));
        glUseProgram(sharp_prog);
        glUniform1i(sharp_u_tex, 0);
        glUniform2f(sharp_u_size, FB_W, FB_H);
        // texture u (FB_W texels) runs along the screen height, v (FB_H texels) along the image width
        glUniform2f(sharp_u_scale, (float)SCR_H / FB_W, (float)dw / FB_H);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, ndc);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glUseProgram(0);
        glEnableClientState(GL_VERTEX_ARRAY); // in case the generic attributes alias the fixed-function ones
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        return;
    }
    const float xy[] = {dx, 0, dx + dw, 0, dx, SCR_H, dx + dw, SCR_H};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// The intro logos are UIImageViews (fake in objc.c) that updateTimerPages() shows for 420 and 250 frames:
// 1st phase _s_showUBILogo + _ubiLogo, 2nd _s_showPlatform (until _s_showPlatformOver) + _platLogo.
static int logo_phase(void) {
    const uint8_t *f = (const uint8_t *)(MACHO_SLIDE + 0x6b92bd);   // _s_showPlatformOver, _s_showPlatform, _s_showUBILogo
    const uint32_t *v = (const uint32_t *)(MACHO_SLIDE + 0x6ce2c4); // _ubiLogo, _platLogo
    return f[2] && v[0] ? 1 : f[1] && !f[0] && v[1] ? 2 : 0;
}
int gles_logo_active(void) { return logo_phase(); }
unsigned gles_presents;

static void draw_logo(void) {
    // ponytail: user's order (Forgotten Sands ad right after the video, then UBILogo); the binary pairs them the other way.
    static const char *names[] = {NULL, "PlatformLogo", "UBILogo"};
    static GLuint ltex[3];
    static int tried[3];
    int p = logo_phase();
    if (!p) {
        for (int i = 1; i < 3; i++)
            if (ltex[i])
                glDeleteTextures(1, &ltex[i]), ltex[i] = 0;
        return;
    }
    if (!tried[p]) {
        tried[p] = 1;
        char path[256];
        snprintf(path, sizeof(path), "%s%s.dat", BUNDLE_PATH, names[p]); // plain PNG despite the extension
        png_image img = {.version = PNG_IMAGE_VERSION};
        if (png_image_begin_read_from_file(&img, path)) {
            img.format = PNG_FORMAT_RGBA;
            void *px = malloc(PNG_IMAGE_SIZE(img));
            if (px && png_image_finish_read(&img, NULL, px, 0, NULL)) {
                glGenTextures(1, &ltex[p]);
                glBindTexture(GL_TEXTURE_2D, ltex[p]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
            png_image_free(&img);
            free(px);
        }
        l_info("[LOGO] %s: %s (%ux%u)", path, ltex[p] ? "shown" : "could not load", img.width, img.height);
    }
    if (!ltex[p])
        return;
    int dx, dw;
    gles_dest_rect(&dx, &dw);
    // Portrait PNG (row 0 on top) rotated like the FBO in draw_screen: its right edge becomes the screen top.
    static const float uv[] = {1, 0, 1, 1, 0, 0, 0, 1};
    const float xy[] = {dx, 0, dx + dw, 0, dx, SCR_H, dx + dw, SCR_H};
    glBindTexture(GL_TEXTURE_2D, ltex[p]);
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void gles_present(void) {
    gles_presents++;
    static int trophies_ready;
    if (!trophies_ready) { // psvita-trophies rule 6: on the rendering thread, before the first frame
        trophies_ready = 1;
        trophies_init();
    }
    static const GLenum caps[] = {GL_BLEND,      GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST,
                                  GL_STENCIL_TEST, GL_CULL_FACE, GL_TEXTURE_2D};
    enum { NCAPS = sizeof(caps) / sizeof(caps[0]) };
    GLboolean on[NCAPS], tca, va, ca, na;
    GLint tex, mode, active, vp[4], bsrc, bdst;
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc); // options.c draws its panel / FPS counter with alpha blending
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    for (int i = 0; i < NCAPS; i++)
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
    for (int i = 0; i < NCAPS - 1; i++)
        glDisable(caps[i]);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    draw_screen();
    draw_logo();
    if (options_pending())
        options_run(draw_screen); // modal: the game thread waits here (paused) until the menu closes
    options_frame();              // FPS counter / 30 FPS limit
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
    glBlendFunc(bsrc, bdst);
    for (int i = 0; i < NCAPS; i++)
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

static void g_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    saved_mask[0] = r, saved_mask[1] = g, saved_mask[2] = b;
    if (!mask_off) // gles_color_mask_off: the hidden HUD page keeps writing no color
        glColorMask(r, g, b, GL_FALSE);
}

static void g_glTexEnvx(GLenum target, GLenum pname, GLfixed param) {
    if (pname == GL_TEXTURE_ENV_MODE)
        glTexEnvi(target, pname, param);
    else
        glTexEnvf(target, pname, (float)param / 65536.0f);
}

static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    gles_draw_calls++;
    glDrawElements(mode, count, type, indices);
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
