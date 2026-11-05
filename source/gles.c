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
// Destination rectangle: 480x320 scaled to Vita screen height (1.7x) and centered horizontally.
#define DST_H SCR_H
#define DST_W (DST_H * FB_H / FB_W)
#define DST_X ((SCR_W - DST_W) / 2)

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

// ---------------------------------------------------------------- diagnostics (log 019: no menu UI)
// ponytail: temporary console-run instrumentation; remove once the gameswf menu renders.

static const void *color_ptr;
static GLenum color_type;
static void g_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    color_ptr = ptr, color_type = type;
    glColorPointer(size, type, stride, ptr);
}

// Logs draws issued while gameswf renders (first 8, then every 2000th).
int gles_in_swf; // set by patch.c around RenderFX::Render

static void diag_swf_draw(GLenum mode, GLint first, GLsizei count) {
    static uint32_t n;
    if (!gles_in_swf)
        return;
    n++;
    if (n > 8 && n % 2000)
        return;
    GLint vbo = 0, vp[4], sc[4], tex = 0, df = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_DEPTH_FUNC, &df);
    float p[16], m[16], v[3] = {0}, c[4];
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    if (!vbo && vtx.ptr && vtx.type == GL_FLOAT)
        memcpy(v, (const char *)vtx.ptr + first * (vtx.stride ? vtx.stride : vtx.size * 4), vtx.size * 4);
    float e[4];
    for (int i = 0; i < 4; i++)
        e[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i];
    for (int i = 0; i < 4; i++)
        c[i] = p[i] * e[0] + p[4 + i] * e[1] + p[8 + i] * e[2] + p[12 + i] * e[3];
    uint32_t rgba = 0;
    if (glIsEnabled(GL_COLOR_ARRAY) && color_type == GL_UNSIGNED_BYTE && color_ptr && !vbo)
        memcpy(&rgba, (const char *)color_ptr + first * 4, 4);
    l_info("[swfdiag] #%u mode=%x n=%d vfmt=%d/%x/%d v0=(%.1f,%.1f,%.2f) clip=(%.2f,%.2f,%.2f,%.2f) rgba=%08x tex=%d vbo=%d "
           "vp=%d,%d,%d,%d sc=%d,%d,%d,%d en:dep=%d(f=%x) stn=%d bl=%d at=%d cull=%d sci=%d t2d=%d carr=%d tarr=%d",
           n, mode, count, vtx.size, vtx.type, vtx.stride, v[0], v[1], v[2], c[0], c[1], c[2], c[3], rgba, tex, vbo, vp[0], vp[1], vp[2], vp[3],
           sc[0], sc[1], sc[2], sc[3], glIsEnabled(GL_DEPTH_TEST), df, glIsEnabled(GL_STENCIL_TEST),
           glIsEnabled(GL_BLEND), glIsEnabled(GL_ALPHA_TEST), glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_SCISSOR_TEST),
           glIsEnabled(GL_TEXTURE_2D), glIsEnabled(GL_COLOR_ARRAY), glIsEnabled(GL_TEXTURE_COORD_ARRAY));
}


static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    diag_swf_draw(mode, first, count);
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
    static uint32_t frames, last_draws;
    if (++frames % 300 == 0 && frames <= 3000) {
        l_info("[swfdiag] frame %u: %u draw calls in last 300 frames", frames, gles_draw_calls - last_draws);
        last_draws = gles_draw_calls;
    }
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

// Log 028 diagnostics: prince shows missing triangles; ignoring GL_CULL_FACE did not fix it.
// ponytail: temporary logs (front face modes, frustum near/far) for the depth investigation.
static void g_glFrontFace(GLenum mode) {
    static GLenum seen[2];
    if (mode != seen[0] && mode != seen[1]) {
        seen[seen[0] ? 1 : 0] = mode;
        l_info("[cull] glFrontFace(0x%X) first use", mode);
    }
    glFrontFace(mode);
}

static void g_glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    static int logged;
    if (logged < 4) {
        logged++;
        l_info("[frustum] l=%.3f r=%.3f b=%.3f t=%.3f near=%.4f far=%.1f", l, r, b, t, n, f);
    }
    glFrustumf(l, r, b, t, n, f);
}

static void g_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask(r, g, b, GL_FALSE); }

static void g_glTexEnvx(GLenum target, GLenum pname, GLfixed param) {
    if (pname == GL_TEXTURE_ENV_MODE)
        glTexEnvi(target, pname, param);
    else
        glTexEnvf(target, pname, (float)param / 65536.0f);
}

static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    gles_draw_calls++;
    diag_swf_draw(mode + 0x100, 0, count); // +0x100 marks DrawElements (v0 = first array vertex)
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
    D(glCopyTexSubImage2D), D(glDeleteTextures), D(glDepthMask), D(glDisable), D(glDisableClientState),
    D(glEnable), D(glEnableClientState), D(glGenTextures), D(glGetIntegerv), D(glIsEnabled),
    D(glLoadIdentity), D(glLoadMatrixx), D(glMatrixMode), D(glOrthox), D(glPopMatrix), D(glPushMatrix),
    D(glRotatex), D(glScalex), D(glScissor), D(glShadeModel), D(glTexImage2D), D(glTexParameteri),
    D(glTexSubImage2D), D(glTranslatex), D(glViewport),
    D(glAlphaFunc), D(glBindBuffer), D(glBufferData), D(glBufferSubData),
    F(glClearColor, g_glClearColor), D(glClearDepthf), D(glClearStencil), D(glColor4ub),
    F(glColorMask, g_glColorMask), F(glColorPointer, g_glColorPointer), D(glCompressedTexImage2D), D(glDeleteBuffers),
    D(glDepthFunc), D(glFogf), D(glFogfv), F(glFrontFace, g_glFrontFace), F(glFrustumf, g_glFrustumf),
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
