/**
 * @file objc.c
 * @brief Lightweight Objective-C and Foundation/UIKit runtime emulation.
 * @details Implements selector-name dispatching (selrefs point to strings in __objc_methname)
 *          to satisfy Foundation/UIKit calls required by iOS game engines:
 *          - Typography: NSString + UIFont (sizeWithFont:, drawAtPoint:withFont:) rendered
 *            onto a 512x32 grayscale CGBitmapContext glyph atlas via FreeType.
 *          - Bundle paths/dates: resourcePath, objectForInfoDictionaryKey:, NSCalendar.
 *          - Text input: EAGLView text input dialogs routed through sceImeDialog.
 *          All other selectors log a warning and return nil/0.
 *
 *          Custom NSString structures mirror __cfstring layout {isa, flags, cstr, len},
 *          ensuring @"..." binary constants and dynamic strings share identical field offsets (cstr at +8).
 */
#include <ctype.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <math.h>
#include <psp2/kernel/threadmgr.h>

#include "darwin.h"
#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

// ---------------------------------------------------------------- NSString

typedef struct {
    void *isa;
    uint32_t flags;
    char *str;
    uint32_t len;
} ns_string;

// Autorelease ring buffer: NSString instances in iOS are autoreleased; here they live in a ring
// buffer of 256 entries. If long-term persistence is needed, migrate to dedicated malloc.
static ns_string *ns_new(const char *s) {
    static ns_string ring[256];
    static int next;
    ns_string *o = &ring[next++ & 255];
    free(o->str);
    o->str = strdup(s ? s : "");
    o->len = strlen(o->str);
    o->flags = 0x7C8;
    return o;
}

static const char *ns_cstr(const void *o) { return o ? ((const ns_string *)o)->str : ""; }

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra ? 0x3F >> extra : 0xFF;
    while (extra-- && (*s & 0xC0) == 0x80)
        c = (c << 6) | (*s++ & 0x3F);
    *p = s;
    return c;
}

static int utf16_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        n += utf8_next(&p) > 0xFFFF ? 2 : 1;
    return n;
}

// stringWithFormat: using Darwin variadic printf (%@, unaligned 64-bit words).
static ns_string *ns_format(const char *fmt, va_list ap) {
    char out[1024];
    va_list cp;
    va_copy(cp, ap);
    words w = {&cp, NULL};
    darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(cp);
    return ns_new(out);
}

// ---------------------------------------------------------------- fonts (FreeType)

typedef struct {
    uint32_t key; // size << 24 | codepoint
    int16_t left, top, adv;
    uint16_t w, h;
    uint8_t *bmp;
} glyph;

#define GLYPH_CACHE 4096
static glyph cache[GLYPH_CACHE];
static FT_Library ft;
static FT_Face faces[4];
static int nfaces = -1;

static void fonts_init(void) {
    // Custom font.ttf checked first, then PS Vita system fonts.
    static const char *paths[] = {DATA_PATH "font.ttf", "sa0:data/font/pvf/ltn0.pvf",
                                  "sa0:data/font/pvf/kr0.pvf", "sa0:data/font/pvf/jpn0.pvf"};
    nfaces = 0;
    if (FT_Init_FreeType(&ft))
        return l_error("FreeType failed to initialize: font rendering disabled");
    for (int i = 0; i < 4; i++)
        if (!FT_New_Face(ft, paths[i], 0, &faces[nfaces])) {
            l_info("loaded font: %s", paths[i]);
            nfaces++;
        }
    if (!nfaces)
        l_error("no fonts loaded: text rendering disabled");
}

static const glyph *get_glyph(int size, uint32_t cp) {
    static glyph tmp;
    uint32_t key = (uint32_t)size << 24 | (cp & 0xFFFFFF);
    uint32_t h = (key * 2654435761u) % GLYPH_CACHE;
    glyph *g = NULL;
    for (int i = 0; i < 16; i++) {
        glyph *c = &cache[(h + i) % GLYPH_CACHE];
        if (c->key == key)
            return c;
        if (!c->key) {
            g = c;
            break;
        }
    }
    if (!g) { // cache full for this bucket: rasterize directly without caching
        free(tmp.bmp);
        g = &tmp;
    }
    memset(g, 0, sizeof(*g));
    g->key = key;
    if (nfaces < 0)
        fonts_init();
    FT_Face f = NULL;
    for (int i = 0; i < nfaces && !f; i++)
        if (FT_Get_Char_Index(faces[i], cp))
            f = faces[i];
    if (!f) {
        g->adv = cp == ' ' ? size / 3 : size / 2;
        return g;
    }
    FT_Set_Pixel_Sizes(f, 0, size);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER))
        return g;
    FT_GlyphSlot s = f->glyph;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    g->adv = s->advance.x >> 6;
    g->w = s->bitmap.width;
    g->h = s->bitmap.rows;
    g->bmp = malloc(g->w * g->h + 1);
    for (int y = 0; y < g->h; y++)
        memcpy(g->bmp + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
    return g;
}

// Line height and baseline approximations matching iOS 3 Helvetica (14pt->18, 13->16, 11->14).
// Adjust here if vertical alignment requires calibration.
static int line_height(int size) { return size == 14 ? 18 : size == 13 ? 16 : size == 11 ? 14 : (size * 5 + 2) / 4; }
static int baseline(int size) { return (line_height(size) - size) / 2 + (size * 4 + 2) / 5; }

static int text_width(int size, const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        w += get_glyph(size, utf8_next(&p))->adv;
    return w;
}

// ---------------------------------------------------------------- CoreGraphics / UIKit

typedef struct {
    uint8_t *data;
    int w, h, bpr;
    float gray;
} cg_ctx;

static cg_ctx *cur_ctx;

static cg_ctx *cg_BitmapContextCreate(void *data, int w, int h, int bpc, int bpr, void *cs, int info) {
    cg_ctx *c = calloc(1, sizeof(*c));
    *c = (cg_ctx){data, w, h, bpr, 1.0f};
    return c;
}
static void cg_ContextRelease(cg_ctx *c) {
    if (cur_ctx == c)
        cur_ctx = NULL;
    free(c);
}
SOFTFP static void cg_SetGrayFillColor(cg_ctx *c, float gray, float alpha) { c->gray = gray * alpha; }
static void *cg_ColorSpaceCreate(void) { return (void *)1; }
static void cg_noop(void) {}
static void ui_PushContext(cg_ctx *c) { cur_ctx = c; }
static void ui_PopContext(void) { cur_ctx = NULL; }

typedef struct {
    uint32_t tag;
    int size;
} ui_font;

static ui_font *font_of_size(float pt) {
    static ui_font fonts[32];
    int size = (int)pt;
    for (int i = 0; i < 32; i++)
        if (!fonts[i].tag || fonts[i].size == size) {
            fonts[i] = (ui_font){0x464F4E54, size};
            return &fonts[i];
        }
    return &fonts[31];
}

// UIKit draws with Y-axis pointing downwards; atlas context is un-inverted,
// placing text upside-down in memory (glyph row j -> row h-1-j), as expected by guest quad blitters.
static void draw_text(cg_ctx *c, const char *s, int x, int y, int size) {
    int pen = x, base = baseline(size);
    uint8_t ink = (uint8_t)(c->gray * 255.0f);
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        const glyph *g = get_glyph(size, utf8_next(&p));
        for (int gy = 0; gy < g->h; gy++) {
            int row = c->h - 1 - (y + base - g->top + gy);
            if (row < 0 || row >= c->h)
                continue;
            for (int gx = 0; gx < g->w; gx++) {
                int col = pen + g->left + gx;
                if (col < 0 || col >= c->w)
                    continue;
                uint8_t v = g->bmp[gy * g->w + gx] * ink / 255, *d = &c->data[row * c->bpr + col];
                if (v > *d)
                    *d = v;
            }
        }
        pen += g->adv;
    }
}

// ---------------------------------------------------------------- text input (IME)

static int ime_type = -1;
static char ime_utf8[128];
static uint16_t ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_title[16], ime_initial[1];

static void ime_open(int type) {
    static const char *titles[] = {"Name", "Number"};
    const char *t = titles[type == 1];
    for (int i = 0; (ime_title[i] = t[i]); i++) {}
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.type = type == 1 ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    p.title = ime_title;
    p.maxTextLength = type == 1 ? 4 : 12;
    p.initialText = ime_initial;
    p.inputTextBuffer = ime_buf;
    ime_utf8[0] = 0;
    int r = sceImeDialogInit(&p);
    if (r < 0)
        l_error("sceImeDialogInit = 0x%08X", r);
    ime_type = type; // even if init fails: ime_poll will close dialog event so game doesn't stall
}

int ime_active(void) { return ime_type >= 0; }

void ime_poll(void) {
    if (ime_type < 0)
        return;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
        return;
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            size_t n = 0;
            for (int i = 0; ime_buf[i] && n < sizeof(ime_utf8) - 4; i++) {
                uint16_t c = ime_buf[i];
                if (c < 0x80)
                    ime_utf8[n++] = c;
                else if (c < 0x800)
                    n += sprintf(ime_utf8 + n, "%c%c", 0xC0 | c >> 6, 0x80 | (c & 0x3F));
                else
                    n += sprintf(ime_utf8 + n, "%c%c%c", 0xE0 | c >> 12, 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
            }
            ime_utf8[n] = 0;
        }
        sceImeDialogTerm();
    }
    int type = ime_type;
    ime_type = -1;
    l_info("input text entered (%d): \"%s\"", type, ime_utf8);
    uintptr_t text_close_fn = macho_sym("_Ex_HandleEventTextClose");
    if (text_close_fn)
        call_guest((void *)text_close_fn, type, 0, 0, 0);
}

// ---------------------------------------------------------------- ObjC Dummy Classes & Metadata

typedef struct {
    void *isa;
    void *superclass;
    const char *name;
    uint32_t version;
    uint32_t info;
    uint32_t instance_size;
    void *ivars;
    void *methodLists;
    void *cache;
    void *protocols;
} objc_class_dummy;

#define DECL_CLASS(n) static objc_class_dummy cls_##n = { .name = #n };
#define DECL_META(n) static objc_class_dummy meta_##n = { .name = #n };

DECL_CLASS(CAEAGLLayer)
DECL_CLASS(EAGLContext)
DECL_CLASS(MPMoviePlayerController)
DECL_CLASS(NSArray)
DECL_CLASS(NSAutoreleasePool)
DECL_CLASS(NSBundle)
DECL_CLASS(NSData)
DECL_CLASS(NSDate)
DECL_CLASS(NSDateFormatter)
DECL_CLASS(NSDictionary)
DECL_CLASS(NSError)
DECL_CLASS(NSFileManager)
DECL_CLASS(NSLocale)
DECL_CLASS(NSMutableArray)
DECL_CLASS(NSMutableData)
DECL_CLASS(NSMutableURLRequest)
DECL_CLASS(NSNotificationCenter)
DECL_CLASS(NSNumber)
DECL_CLASS(NSObject)
DECL_CLASS(NSString)
DECL_CLASS(NSThread)
DECL_CLASS(NSTimer)
DECL_CLASS(NSURL)
DECL_CLASS(NSURLConnection)
DECL_CLASS(NSURLRequest)
DECL_CLASS(NSUserDefaults)
DECL_CLASS(UIAccelerometer)
DECL_CLASS(UIActivityIndicatorView)
DECL_CLASS(UIAlertView)
DECL_CLASS(UIApplication)
DECL_CLASS(UIButton)
DECL_CLASS(UIColor)
DECL_CLASS(UIDevice)
DECL_CLASS(UIFont)
DECL_CLASS(UIImage)
DECL_CLASS(UIImagePickerController)
DECL_CLASS(UIImageView)
DECL_CLASS(UILabel)
DECL_CLASS(UINavigationController)
DECL_CLASS(UIScreen)
DECL_CLASS(UITextField)
DECL_CLASS(UITextView)
DECL_CLASS(UIView)
DECL_CLASS(UIViewController)
DECL_CLASS(UIWebView)
DECL_CLASS(UIWindow)

DECL_META(NSObject)
DECL_META(UINavigationController)
DECL_META(UIView)
DECL_META(UIViewController)

static void *s_OBJC_EHTYPE_id[4] = {0};
static void *s_objc_empty_cache[16] = {0};
static void *s_objc_empty_vtable[16] = {0};
static void *s_CFConstantStringClassReference[4] = {0};

// Static keys / notification strings
static const char *s_kCFAllocatorDefault = "kCFAllocatorDefault";
static const char *s_kCFRunLoopDefaultMode = "kCFRunLoopDefaultMode";
static const char *s_kEAGLColorFormatRGB565 = "kEAGLColorFormatRGB565";
static const char *s_kEAGLDrawablePropertyColorFormat = "kEAGLDrawablePropertyColorFormat";
static const char *s_kEAGLDrawablePropertyRetainedBacking = "kEAGLDrawablePropertyRetainedBacking";
static const char *s_NSLocaleCountryCode = "NSLocaleCountryCode";
static const char *s_NSErrorFailingURLStringKey = "NSErrorFailingURLStringKey";
static const char *s_MPMoviePlayerPlaybackDidFinishNotification = "MPMoviePlayerPlaybackDidFinishNotification";
static const char *s_MPMoviePlayerContentPreloadDidFinishNotification = "MPMoviePlayerContentPreloadDidFinishNotification";
static const char *s_UIKeyboardWillShowNotification = "UIKeyboardWillShowNotification";
static const char *s_UIKeyboardWillHideNotification = "UIKeyboardWillHideNotification";

// ---------------------------------------------------------------- CoreFoundation & CoreGraphics & UIKit

static double SOFTFP cf_CFAbsoluteTimeGetCurrent(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)(tv.tv_sec - 978307200) + (double)tv.tv_usec / 1000000.0;
}

static int cf_CFAbsoluteTimeGetDayOfWeek(double at, void *tz) { return 1; }
static void cf_CFAbsoluteTimeGetGregorianDate(double at, void *tz, void *out_date) {}
static void *cf_CFAllocatorGetDefault(void) { return (void *)s_kCFAllocatorDefault; }
static void *cf_CFBundleGetMainBundle(void) { return (void *)0xCFB00001; }
static void *cf_CFBundleCopyBundleURL(void *bundle) { return (void *)0xCFB00002; }
static void *cf_CFURLCopyFileSystemPath(void *anURL, int pathStyle) {
    char p[256];
    snprintf(p, sizeof(p), "%s", BUNDLE_PATH);
    p[strlen(p) - 1] = 0;
    return ns_new(p);
}
static void cf_CFRelease(void *cf) {}
static int cf_CFRunLoopRunInMode(void *mode, double seconds, int returnAfterSourceHandled) {
    sceKernelDelayThread(1000);
    return 0;
}

static void *cf_CFStringCreateWithBytes(void *alloc, const uint8_t *bytes, int numBytes, int encoding, int isExt) {
    char *buf = malloc(numBytes + 1);
    memcpy(buf, bytes, numBytes);
    buf[numBytes] = '\0';
    void *res = ns_new(buf);
    free(buf);
    return res;
}

static int cf_CFStringGetCString(void *theString, char *buffer, int bufferSize, int encoding) {
    const char *s = ns_cstr(theString);
    if (!s) return 0;
    strncpy(buffer, s, bufferSize);
    buffer[bufferSize - 1] = '\0';
    return 1;
}

static void *cf_CFTimeZoneCopySystem(void) { return (void *)0xCF100001; }

SOFTFP static void cg_CGAffineTransformMakeRotation(float *out, float angle) {
    out[0] = cosf(angle);
    out[1] = sinf(angle);
    out[2] = -sinf(angle);
    out[3] = cosf(angle);
    out[4] = 0.0f;
    out[5] = 0.0f;
}

SOFTFP static void cg_CGAffineTransformRotate(float *out, const float *t, float angle) {
    float rot[6];
    cg_CGAffineTransformMakeRotation(rot, angle);
    memcpy(out, rot, sizeof(rot));
}

static void cg_CGContextDrawImage(void *c, float *rect, void *image) {}
static void cg_CGContextFillRect(void *c, float *rect) {}
static void cg_CGContextScaleCTM(void *c, float sx, float sy) {}
static void cg_CGContextTranslateCTM(void *c, float tx, float ty) {}
static void cg_CGContextSetRGBFillColor(void *c, float r, float g, float b, float a) {}

static void *cg_CGImageGetColorSpace(void *image) { return (void *)0xC0100001; }
static size_t cg_CGImageGetHeight(void *image) { return 320; }
static size_t cg_CGImageGetWidth(void *image) { return 480; }

// ---------------------------------------------------------------- dummy objc objects
static uint32_t dummy_obj[4];
static void *ret_obj(void) { return (void *)dummy_obj; }

static void *ns_NSHomeDirectory(void) {
    char p[256];
    snprintf(p, sizeof(p), "%s", DOCS_PATH);
    p[strlen(p) - 1] = '\0';
    return ns_new(p);
}

static void *ns_NSSearchPathForDirectoriesInDomains(int directory, int domainMask, int expandTilde) {
    (void)directory; (void)domainMask; (void)expandTilde;
    static void *docs_arr[4];
    char p[256];
    snprintf(p, sizeof(p), "%s", DOCS_PATH);
    size_t len = strlen(p);
    if (len > 0 && p[len - 1] == '/') p[len - 1] = '\0';
    docs_arr[0] = ns_new(p);
    return docs_arr;
}

// ---------------------------------------------------------------- Guest Mach-O ObjC Class Registry

typedef struct {
    const char *name;
    uint32_t imp;
} guest_method;

typedef struct {
    const char *name;
    uint32_t cls_ptr;
    uint32_t instance_size;
    int is_meta;
    guest_method *methods;
    int method_count;
} guest_class_info;

#define MAX_GUEST_CLASSES 32
static guest_class_info registered_classes[MAX_GUEST_CLASSES];
static int registered_class_count = 0;
static void *s_app_delegate = NULL;

static inline uintptr_t to_guest_mem(uint32_t va) {
    if (!va) return 0;
    return (va < MACHO_SLIDE) ? (MACHO_SLIDE + va) : va;
}

void objc_init(void) {
}

void objc_register_classes(uint32_t classlist_addr, int count) {
    l_info("[OBJC] Registering guest Mach-O classes from 0x%08X (count: %d)...", classlist_addr, count);
    uint32_t *cl_ptrs = (uint32_t *)classlist_addr;
    for (int i = 0; i < count; i++) {
        uint32_t cls_va = cl_ptrs[i];
        if (!cls_va) continue;
        uint32_t cls_mem = to_guest_mem(cls_va);

        // 1. Instance class
        uint32_t ro_va = *(uint32_t *)(cls_mem + 16);
        uint32_t ro_mem = to_guest_mem(ro_va);
        uint32_t name_va = *(uint32_t *)(ro_mem + 16);
        const char *cname = (const char *)to_guest_mem(name_va);
        uint32_t isize = *(uint32_t *)(ro_mem + 8);
        uint32_t methods_va = *(uint32_t *)(ro_mem + 20);

        if (registered_class_count < MAX_GUEST_CLASSES) {
            guest_class_info *ci = &registered_classes[registered_class_count++];
            ci->name = cname;
            ci->cls_ptr = cls_mem;
            ci->instance_size = isize;
            ci->is_meta = 0;
            ci->methods = NULL;
            ci->method_count = 0;

            if (methods_va) {
                uint32_t methods_mem = to_guest_mem(methods_va);
                uint32_t mcount = *(uint32_t *)(methods_mem + 4);
                ci->method_count = mcount;
                ci->methods = calloc(mcount, sizeof(guest_method));
                for (uint32_t m = 0; m < mcount; m++) {
                    uint32_t m_entry = methods_mem + 8 + m * 12;
                    uint32_t m_name_va = *(uint32_t *)m_entry;
                    uint32_t m_imp_va = *(uint32_t *)(m_entry + 8);
                    ci->methods[m].name = (const char *)to_guest_mem(m_name_va);
                    ci->methods[m].imp = to_guest_mem(m_imp_va);
                }
            }
            l_info("[OBJC] Registered guest class %s (size %u, %d methods)", cname, (unsigned)isize, ci->method_count);
        }

        // 2. Metaclass (class methods)
        uint32_t meta_va = *(uint32_t *)cls_mem;
        if (meta_va) {
            uint32_t meta_mem = to_guest_mem(meta_va);
            uint32_t m_ro_va = *(uint32_t *)(meta_mem + 16);
            if (m_ro_va) {
                uint32_t m_ro_mem = to_guest_mem(m_ro_va);
                uint32_t m_methods_va = *(uint32_t *)(m_ro_mem + 20);
                if (registered_class_count < MAX_GUEST_CLASSES) {
                    guest_class_info *mci = &registered_classes[registered_class_count++];
                    mci->name = cname;
                    mci->cls_ptr = meta_mem;
                    mci->instance_size = 0;
                    mci->is_meta = 1;
                    mci->methods = NULL;
                    mci->method_count = 0;
                    if (m_methods_va) {
                        uint32_t m_methods_mem = to_guest_mem(m_methods_va);
                        uint32_t mcount = *(uint32_t *)(m_methods_mem + 4);
                        mci->method_count = mcount;
                        mci->methods = calloc(mcount, sizeof(guest_method));
                        for (uint32_t m = 0; m < mcount; m++) {
                            uint32_t m_entry = m_methods_mem + 8 + m * 12;
                            uint32_t m_name_va = *(uint32_t *)m_entry;
                            uint32_t m_imp_va = *(uint32_t *)(m_entry + 8);
                            mci->methods[m].name = (const char *)to_guest_mem(m_name_va);
                            mci->methods[m].imp = to_guest_mem(m_imp_va);
                        }
                    }
                    l_info("[OBJC] Registered guest metaclass %s (%d class methods)", cname, mci->method_count);
                }
            }
        }
    }
}

void *objc_find_class(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < registered_class_count; i++) {
        if (!registered_classes[i].is_meta && !strcmp(registered_classes[i].name, name))
            return (void *)registered_classes[i].cls_ptr;
    }
    return NULL;
}

uintptr_t objc_get_imp(void *cls, const char *sel_name) {
    if (!cls || !sel_name) return 0;
    for (int i = 0; i < registered_class_count; i++) {
        if (registered_classes[i].cls_ptr == (uint32_t)cls) {
            for (int m = 0; m < registered_classes[i].method_count; m++) {
                if (!strcmp(registered_classes[i].methods[m].name, sel_name))
                    return registered_classes[i].methods[m].imp;
            }
            break;
        }
    }
    return 0;
}

int objc_has_method(void *cls, const char *sel_name) {
    return objc_get_imp(cls, sel_name) != 0;
}

void *objc_class_alloc(void *cls) {
    if (!cls) return NULL;
    uint32_t size = 128;
    for (int i = 0; i < registered_class_count; i++) {
        if (registered_classes[i].cls_ptr == (uint32_t)cls) {
            if (registered_classes[i].instance_size > 0)
                size = registered_classes[i].instance_size;
            break;
        }
    }
    void **obj = calloc(1, size + 16);
    if (!obj) return NULL;
    *obj = cls;
    return obj;
}

static void *ns_NSClassFromString(void *aName) {
    const char *name = ns_cstr(aName);
    void *cls = objc_find_class(name);
    if (cls) return cls;
    char buf[128];
    snprintf(buf, sizeof(buf), "_OBJC_CLASS_$_%s", name);
    uintptr_t s = macho_sym(buf);
    if (!s) {
        const import_entry *imp = imports_lookup(buf);
        if (imp) s = imp->addr;
    }
    return (void *)s;
}

static void *ui_UIImageJPEGRepresentation(void *image, float compressionQuality) { return ret_obj(); }
static void *ui_UIImagePNGRepresentation(void *image) { return ret_obj(); }
static void ui_UIGraphicsBeginImageContext(float *size) {}
static void ui_UIGraphicsEndImageContext(void) {}
static void *ui_UIGraphicsGetCurrentContext(void) { return (void *)0xC6000001; }
static void *ui_UIGraphicsGetImageFromCurrentImageContext(void) { return ret_obj(); }

// ---------------------------------------------------------------- objc_msgSend

static void log_unknown(void *self, const char *sel) {
    static const char *seen[512];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == sel)
            return;
    if (nseen < 512)
        seen[nseen++] = sel;
    l_error("objc_msgSend(%p, %s) -> nil", self, sel);
}

static int sel_is(const char *sel, const char *name) { return !strcmp(sel, name); }

extern void gles_present(void);

static uint32_t objc_msgSend(void *self, const char *sel, ...);

// Built-in Foundation / UIKit handling
static uint32_t objc_msgSend_system_impl(void *self, const char *sel, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    uint32_t a = va_arg(ap_copy, uint32_t);
    uint32_t r = 0;
    const struct tm *now;
    time_t t;

    if (sel_is(sel, "alloc") || sel_is(sel, "allocWithZone:")) {
        r = (uint32_t)objc_class_alloc(self);
        if (!r) r = (uint32_t)self;
    } else if (sel_is(sel, "init") || sel_is(sel, "initWithFrame:") ||
               sel_is(sel, "initWithImage:") || sel_is(sel, "initWithFile:") ||
               sel_is(sel, "initWithAPI:") || sel_is(sel, "initWithAPI:sharegroup:") ||
               sel_is(sel, "retain") || sel_is(sel, "autorelease")) {
        r = (uint32_t)self;
    } else if (sel_is(sel, "initWithUTF8String:")) {
        r = (uint32_t)ns_new((const char *)a);
    } else if (sel_is(sel, "initWithString:")) {
        r = (uint32_t)ns_new(ns_cstr((void *)a));
    } else if (sel_is(sel, "initWithFormat:")) {
        r = (uint32_t)ns_format(ns_cstr((void *)a), ap_copy);
    } else if (sel_is(sel, "numberWithBool:") || sel_is(sel, "numberWithInt:") || sel_is(sel, "numberWithInteger:")) {
        r = (uintptr_t)ret_obj();
    } else if (sel_is(sel, "sharegroup") || sel_is(sel, "sharedAccelerometer") || sel_is(sel, "currentLocale")) {
        r = (uintptr_t)ret_obj();
    } else if (sel_is(sel, "uniqueIdentifier")) {
        r = (uint32_t)ns_new("0123456789abcdef0123456789abcdef01234567");
    } else if (sel_is(sel, "uppercaseString")) {
        const char *s = ns_cstr(self);
        if (s) {
            char *up = strdup(s);
            for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
            r = (uint32_t)ns_new(up);
            free(up);
        } else {
            r = (uint32_t)self;
        }
    } else if (sel_is(sel, "setUpdateInterval:")) {
        r = 0;
    } else if (sel_is(sel, "setDateFormat:")) {
        r = 0;
    } else if (sel_is(sel, "stringFromDate:")) {
        t = time(NULL);
        now = localtime(&t);
        char buf[64];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
        r = (uint32_t)ns_new(buf);
    } else if (sel_is(sel, "fileExistsAtPath:")) {
        char real_p[512];
        struct stat st;
        r = stat(darwin_path(ns_cstr((void *)a), real_p, sizeof(real_p)), &st) == 0;
    } else if (sel_is(sel, "writeToFile:atomically:") || sel_is(sel, "writeToFile:atomically:encoding:error:")) {
        const char *s = ns_cstr(self);
        char real_p[512];
        FILE *f = s ? fopen(darwin_path(ns_cstr((void *)a), real_p, sizeof(real_p)), "wb") : NULL;
        if (f) {
            r = fwrite(s, 1, strlen(s), f) == strlen(s);
            fclose(f);
        }
    } else if (sel_is(sel, "stringByReplacingOccurrencesOfString:withString:")) {
        const char *orig = ns_cstr(self);
        const char *target = ns_cstr((void *)a);
        const char *rep = ns_cstr(va_arg(ap_copy, void *));
        if (!target || !*target || !orig) {
            r = (uint32_t)self;
        } else {
            char buf[512];
            const char *p = strstr(orig, target);
            if (!p) {
                r = (uint32_t)self;
            } else {
                size_t prefix_len = p - orig;
                snprintf(buf, sizeof(buf), "%.*s%s%s", (int)prefix_len, orig, rep ? rep : "", p + strlen(target));
                r = (uint32_t)ns_new(buf);
            }
        }
    } else if (sel_is(sel, "detachNewThreadSelector:toTarget:withObject:")) {
        r = 0;
    } else if (sel_is(sel, "release") || sel_is(sel, "dealloc") || sel_is(sel, "drain")) {
        r = 0;
    } else if (sel_is(sel, "performSelectorOnMainThread:withObject:waitUntilDone:") ||
               sel_is(sel, "performSelectorOnMainThread:withObject:")) {
        const char *target_sel = (const char *)a;
        uint32_t obj = va_arg(ap_copy, uint32_t);
        r = objc_msgSend(self, target_sel, obj);
    } else if (sel_is(sel, "performSelector:withObject:")) {
        const char *target_sel = (const char *)a;
        uint32_t obj = va_arg(ap_copy, uint32_t);
        r = objc_msgSend(self, target_sel, obj);
    } else if (sel_is(sel, "performSelector:")) {
        const char *target_sel = (const char *)a;
        r = objc_msgSend(self, target_sel);
    } else if (sel_is(sel, "isEqualToString:")) {
        const char *s1 = ns_cstr(self);
        const char *s2 = ns_cstr((void *)a);
        r = (s1 && s2 && strcmp(s1, s2) == 0) ? 1 : 0;
    } else if (sel_is(sel, "stringWithUTF8String:") || sel_is(sel, "stringWithCString:encoding:")) {
        r = (uint32_t)ns_new((const char *)a);
    } else if (sel_is(sel, "stringWithFormat:")) {
        r = (uint32_t)ns_format(ns_cstr((void *)a), ap_copy);
    } else if (sel_is(sel, "length")) {
        r = utf16_len(ns_cstr(self));
    } else if (sel_is(sel, "UTF8String") || sel_is(sel, "cStringUsingEncoding:")) {
        r = (uint32_t)ns_cstr(self);
    } else if (sel_is(sel, "getCString:maxLength:encoding:")) {
        uint32_t max = va_arg(ap_copy, uint32_t);
        snprintf((char *)a, max, "%s", ns_cstr(self));
        r = 1;
    } else if (sel_is(sel, "systemFontOfSize:") || sel_is(sel, "boldSystemFontOfSize:")) {
        union { uint32_t u; float f; } v = {a};
        r = (uint32_t)font_of_size(v.f);
    } else if (sel_is(sel, "mainBundle") || sel_is(sel, "currentCalendar") || sel_is(sel, "date") ||
               sel_is(sel, "components:fromDate:") || sel_is(sel, "standardUserDefaults") ||
               sel_is(sel, "defaultCenter") || sel_is(sel, "defaultManager") || sel_is(sel, "mainScreen") ||
               sel_is(sel, "currentContext") || sel_is(sel, "blackColor") || sel_is(sel, "whiteColor") ||
               sel_is(sel, "clearColor") || sel_is(sel, "imageWithContentsOfFile:")) {
        r = (uintptr_t)ret_obj();
    } else if (sel_is(sel, "sharedInstance") || sel_is(sel, "getSharedInstance")) {
        r = s_app_delegate ? (uint32_t)s_app_delegate : (uint32_t)ret_obj();
    } else if (sel_is(sel, "layerClass")) {
        r = (uint32_t)&cls_CAEAGLLayer;
    } else if (sel_is(sel, "layer")) {
        r = (uint32_t)self;
    } else if (sel_is(sel, "currentDevice")) {
        r = (uint32_t)&cls_UIDevice;
    } else if (sel_is(sel, "sharedApplication")) {
        r = (uint32_t)&cls_UIApplication;
    } else if (sel_is(sel, "systemVersion")) {
        r = (uint32_t)ns_new("3.1.3");
    } else if (sel_is(sel, "model") || sel_is(sel, "name")) {
        r = (uint32_t)ns_new("iPhone");
    } else if (sel_is(sel, "resourcePath")) {
        char p[256];
        snprintf(p, sizeof(p), "%s", BUNDLE_PATH);
        p[strlen(p) - 1] = 0;
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "pathForResource:ofType:")) {
        const char *name = ns_cstr((void *)a);
        const char *ext = ns_cstr(va_arg(ap_copy, void *));
        char path[256];
        if (ext && ext[0])
            snprintf(path, sizeof(path), "%s%s.%s", BUNDLE_PATH, name, ext);
        else
            snprintf(path, sizeof(path), "%s%s", BUNDLE_PATH, name);
        r = (uint32_t)ns_new(path);
    } else if (sel_is(sel, "objectForInfoDictionaryKey:")) {
        const char *k = ns_cstr((void *)a);
        r = !strcmp(k, "CFBundleVersion") || !strcmp(k, "CFBundleShortVersionString") ? (uint32_t)ns_new("1.0.7") : 0;
    } else if (sel_is(sel, "year") || sel_is(sel, "month") || sel_is(sel, "day")) {
        t = time(NULL);
        now = localtime(&t);
        r = sel[0] == 'y' ? now->tm_year + 1900 : sel[0] == 'm' ? now->tm_mon + 1 : now->tm_mday;
    } else if (sel_is(sel, "presentRenderbuffer:")) {
        gles_present();
        r = 1;
    } else if (sel_is(sel, "setCurrentContext:") || sel_is(sel, "renderbufferStorage:fromDrawable:") ||
               sel_is(sel, "isCurrentContext")) {
        r = 1;
    } else if (sel_is(sel, "setMultipleTouchEnabled:") || sel_is(sel, "setExclusiveTouch:") ||
               sel_is(sel, "setFrame:") || sel_is(sel, "setHidden:") || sel_is(sel, "addSubview:") ||
               sel_is(sel, "makeKeyAndVisible") || sel_is(sel, "setTransform:") ||
               sel_is(sel, "setIdleTimerDisabled:") || sel_is(sel, "setStatusBarHidden:") ||
               sel_is(sel, "setStatusBarOrientation:animated:") || sel_is(sel, "setBackgroundColor:") ||
               sel_is(sel, "setDelegate:") || sel_is(sel, "setUserInteractionEnabled:") ||
               sel_is(sel, "setContentScaleFactor:") || sel_is(sel, "setOpaque:") ||
               sel_is(sel, "setClipsToBounds:") || sel_is(sel, "setClearsContextBeforeDrawing:") ||
               sel_is(sel, "setAutoresizingMask:")) {
        r = 0;
    } else if (sel_is(sel, "contentScaleFactor")) {
        union { float f; uint32_t u; } v = {1.0f};
        r = v.u;
    } else if (sel_is(sel, "isOpaque")) {
        r = 1;
    } else if (sel_is(sel, "setDrawableProperties:")) {
        r = 1;
    } else if (sel_is(sel, "drawableProperties")) {
        r = (uintptr_t)ret_obj();
    } else if (sel_is(sel, "dictionaryWithObjectsAndKeys:") ||
               sel_is(sel, "dictionaryWithObjects:forKeys:count:") ||
               sel_is(sel, "dictionary")) {
        r = (uintptr_t)ret_obj();
    } else if (sel_is(sel, "objectForKey:")) {
        r = 0;
    } else if (sel_is(sel, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:")) {
        r = (uintptr_t)ret_obj();
    } else if (sel_is(sel, "objectAtIndex:") || sel_is(sel, "objectAtIndexedSubscript:")) {
        uint32_t idx = a;
        if ((uintptr_t)self >= 0x80000000) {
            void **arr = (void **)self;
            r = arr[idx] ? (uint32_t)arr[idx] : (uint32_t)ns_new(DOCS_PATH);
        } else {
            r = (uint32_t)ns_new(DOCS_PATH);
        }
    } else if (sel_is(sel, "lastObject") || sel_is(sel, "firstObject")) {
        if ((uintptr_t)self >= 0x80000000) {
            void **arr = (void **)self;
            r = arr[0] ? (uint32_t)arr[0] : (uint32_t)ns_new(DOCS_PATH);
        } else {
            r = (uint32_t)ns_new(DOCS_PATH);
        }
    } else if (sel_is(sel, "stringByAppendingPathComponent:")) {
        const char *base = ns_cstr(self);
        const char *sub = ns_cstr((void *)a);
        char p[512];
        size_t blen = strlen(base);
        if (blen > 0 && base[blen - 1] == '/')
            snprintf(p, sizeof(p), "%s%s", base, sub ? sub : "");
        else
            snprintf(p, sizeof(p), "%s/%s", base, sub ? sub : "");
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "stringByAppendingString:")) {
        const char *base = ns_cstr(self);
        const char *sub = ns_cstr((void *)a);
        char p[512];
        snprintf(p, sizeof(p), "%s%s", base ? base : "", sub ? sub : "");
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "pathExtension")) {
        const char *s = ns_cstr(self);
        const char *dot = strrchr(s, '.');
        r = (uint32_t)ns_new(dot ? dot + 1 : "");
    } else if (sel_is(sel, "lastPathComponent")) {
        const char *s = ns_cstr(self);
        const char *slash = strrchr(s, '/');
        r = (uint32_t)ns_new(slash ? slash + 1 : s);
    } else if (sel_is(sel, "stringByDeletingLastPathComponent")) {
        char p[512];
        snprintf(p, sizeof(p), "%s", ns_cstr(self));
        char *slash = strrchr(p, '/');
        if (slash) *slash = '\0';
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "stringByDeletingPathExtension")) {
        char p[512];
        snprintf(p, sizeof(p), "%s", ns_cstr(self));
        char *dot = strrchr(p, '.');
        if (dot) *dot = '\0';
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "count")) {
        r = 1;
    } else if (sel_is(sel, "countByEnumeratingWithState:objects:count:")) {
        r = 0;
    } else if (sel_is(sel, "textInputOpen:")) {
        ime_open(a);
    } else if (sel_is(sel, "textInputGetUTF8")) {
        r = (uint32_t)ime_utf8;
    } else if (sel_is(sel, "textInputGetLength")) {
        r = utf16_len(ime_utf8);
    } else {
        log_unknown(self, sel);
    }
    va_end(ap_copy);
    return r;
}

static uint32_t objc_msgSend_impl(void *self, const char *sel, va_list ap) {
    if (!self || !sel) return 0;

    // Check if self is a guest class instance or class object
    void *target_cls = NULL;
    uint32_t self_val = (uint32_t)self;
    if (self_val >= 0x80000000) {
        uint32_t isa_val = *(uint32_t *)self;
        if (isa_val >= 0x80000000) {
            for (int i = 0; i < registered_class_count; i++) {
                if (registered_classes[i].cls_ptr == isa_val) {
                    target_cls = (void *)isa_val;
                    break;
                }
            }
        }
        if (!target_cls) {
            for (int i = 0; i < registered_class_count; i++) {
                if (registered_classes[i].cls_ptr == self_val) {
                    target_cls = (void *)self_val;
                    break;
                }
            }
        }
    }

    if (target_cls && objc_has_method(target_cls, sel)) {
        uintptr_t imp = objc_get_imp(target_cls, sel);
        va_list ap_guest;
        va_copy(ap_guest, ap);
        uint32_t a0 = va_arg(ap_guest, uint32_t);
        uint32_t a1 = va_arg(ap_guest, uint32_t);
        uint32_t a2 = va_arg(ap_guest, uint32_t);
        uint32_t a3 = va_arg(ap_guest, uint32_t);
        va_end(ap_guest);
        return call_guest6((void *)imp, (uint32_t)self, (uint32_t)sel, a0, a1, a2, a3);
    }

    return objc_msgSend_system_impl(self, sel, ap);
}

static uint32_t objc_msgSend(void *self, const char *sel, ...) {
    va_list ap;
    va_start(ap, sel);
    uint32_t r = objc_msgSend_impl(self, sel, ap);
    va_end(ap);
    return r;
}

struct objc_super2 {
    void *receiver;
    void *current_class;
};

static uint32_t objc_msgSendSuper2(struct objc_super2 *s, const char *sel, ...) {
    if (!s || !s->receiver || !sel) return 0;
    va_list ap;
    va_start(ap, sel);
    // In our single-inheritance model with external UIKit superclasses (UIView, UIViewController, etc.),
    // super calls must dispatch directly to system shims to avoid recursing infinitely back into the receiver's own method.
    uint32_t r = objc_msgSend_system_impl(s->receiver, sel, ap);
    va_end(ap);
    return r;
}

static void objc_setProperty(void *self, const char *cmd, ptrdiff_t offset, void *new_val, int atomic, int copy) {
    if (self) {
        *(void **)((char *)self + offset) = new_val;
    }
}

static void objc_enumerationMutation(void *obj) {}
static void *objc_begin_catch(void *exc) { return exc; }
static void objc_end_catch(void) {}

static int ui_UIApplicationMain(int argc, char *argv[], void *principalClassName, void *delegateClassName) {
    l_info("[UI] UIApplicationMain starting...");
    void *app_ctrl_class = objc_find_class("AppController");
    if (!app_ctrl_class) {
        app_ctrl_class = (void *)macho_sym("_OBJC_CLASS_$_AppController");
    }
    if (!app_ctrl_class) {
        fatal_error("[UI] Could not find AppController class!");
    }

    s_app_delegate = objc_class_alloc(app_ctrl_class);
    l_info("[BOOT] Allocated AppController instance at %p", s_app_delegate);

    if (objc_has_method(app_ctrl_class, "init")) {
        uintptr_t init_imp = objc_get_imp(app_ctrl_class, "init");
        s_app_delegate = (void *)call_guest6((void *)init_imp, (uint32_t)s_app_delegate, (uint32_t)"init", 0, 0, 0, 0);
    }

    uintptr_t did_finish = objc_get_imp(app_ctrl_class, "applicationDidFinishLaunching:");
    if (!did_finish) {
        did_finish = macho_sym("-[AppController applicationDidFinishLaunching:]");
    }

    if (did_finish) {
        l_info("[BOOT] Calling -[AppController applicationDidFinishLaunching:] at 0x%08X", (unsigned)did_finish);
        call_guest6((void *)did_finish, (uint32_t)s_app_delegate, (uint32_t)"applicationDidFinishLaunching:", (uint32_t)&cls_UIApplication, 0, 0, 0);
        l_info("[BOOT] delegate ok");
    } else {
        l_error("[BOOT] Could not find -[AppController applicationDidFinishLaunching:]");
    }
    return 0;
}

// Methods returning composite structs (CGSize/CGPoint/CGRect): first argument is return pointer.
static void objc_msgSend_stret(float *ret, void *self, const char *sel, ...) {
    va_list ap;
    va_start(ap, sel);

    // Check if self is a guest class instance or class object
    void *target_cls = NULL;
    uint32_t self_val = (uint32_t)self;
    if (self_val >= 0x80000000) {
        uint32_t isa_val = *(uint32_t *)self;
        if (isa_val >= 0x80000000) {
            for (int i = 0; i < registered_class_count; i++) {
                if (registered_classes[i].cls_ptr == isa_val) {
                    target_cls = (void *)isa_val;
                    break;
                }
            }
        }
        if (!target_cls) {
            for (int i = 0; i < registered_class_count; i++) {
                if (registered_classes[i].cls_ptr == self_val) {
                    target_cls = (void *)self_val;
                    break;
                }
            }
        }
    }

    if (target_cls && objc_has_method(target_cls, sel)) {
        uintptr_t imp = objc_get_imp(target_cls, sel);
        uint32_t a0 = va_arg(ap, uint32_t);
        uint32_t a1 = va_arg(ap, uint32_t);
        uint32_t a2 = va_arg(ap, uint32_t);
        call_guest6((void *)imp, (uint32_t)ret, (uint32_t)self, (uint32_t)sel, a0, a1, a2);
        va_end(ap);
        return;
    }
    if (sel_is(sel, "bounds") || sel_is(sel, "frame")) {
        ret[0] = 0.0f;
        ret[1] = 0.0f;
        ret[2] = 320.0f;
        ret[3] = 480.0f;
    } else if (sel_is(sel, "sizeWithFont:")) {
        ui_font *f = va_arg(ap, ui_font *);
        int size = f ? f->size : 14;
        ret[0] = text_width(size, ns_cstr(self));
        ret[1] = line_height(size);
    } else if (sel_is(sel, "drawAtPoint:withFont:")) {
        union { uint32_t u; float f; } x = {va_arg(ap, uint32_t)}, y = {va_arg(ap, uint32_t)};
        ui_font *f = va_arg(ap, ui_font *);
        int size = f ? f->size : 14;
        if (cur_ctx)
            draw_text(cur_ctx, ns_cstr(self), (int)x.f, (int)y.f, size);
        ret[0] = text_width(size, ns_cstr(self));
        ret[1] = line_height(size);
    } else {
        log_unknown(self, sel);
        ret[0] = ret[1] = 0;
    }
    va_end(ap);
}

// ---------------------------------------------------------------- table

#define F(name, fn) {"_" #name, (uintptr_t)(fn), 0}
#define D(name) {"_" #name, (uintptr_t)(name), 0}
#define V(name, ptr) {"_" #name, (uintptr_t)(ptr), 1}

const import_entry objc_imports[] = {
    F(objc_msgSend, objc_msgSend),
    F(objc_msgSendSuper2, objc_msgSendSuper2),
    F(objc_msgSend_stret, objc_msgSend_stret),
    F(objc_setProperty, objc_setProperty),
    F(objc_enumerationMutation, objc_enumerationMutation),
    F(objc_begin_catch, objc_begin_catch),
    F(objc_end_catch, objc_end_catch),
    F(UIApplicationMain, ui_UIApplicationMain),

    // CoreFoundation & CoreGraphics
    F(CFAbsoluteTimeGetCurrent, cf_CFAbsoluteTimeGetCurrent),
    F(CFAbsoluteTimeGetDayOfWeek, cf_CFAbsoluteTimeGetDayOfWeek),
    F(CFAbsoluteTimeGetGregorianDate, cf_CFAbsoluteTimeGetGregorianDate),
    F(CFAllocatorGetDefault, cf_CFAllocatorGetDefault),
    F(CFBundleGetMainBundle, cf_CFBundleGetMainBundle),
    F(CFBundleCopyBundleURL, cf_CFBundleCopyBundleURL),
    F(CFURLCopyFileSystemPath, cf_CFURLCopyFileSystemPath),
    F(CFRelease, cf_CFRelease),
    F(CFRunLoopRunInMode, cf_CFRunLoopRunInMode),
    F(CFStringCreateWithBytes, cf_CFStringCreateWithBytes),
    F(CFStringGetCString, cf_CFStringGetCString),
    F(CFTimeZoneCopySystem, cf_CFTimeZoneCopySystem),
    F(CGAffineTransformMakeRotation, cg_CGAffineTransformMakeRotation),
    F(CGAffineTransformRotate, cg_CGAffineTransformRotate),
    F(CGContextDrawImage, cg_CGContextDrawImage),
    F(CGContextFillRect, cg_CGContextFillRect),
    F(CGContextScaleCTM, cg_CGContextScaleCTM),
    F(CGContextTranslateCTM, cg_CGContextTranslateCTM),
    F(CGContextSetRGBFillColor, cg_CGContextSetRGBFillColor),
    F(CGImageGetColorSpace, cg_CGImageGetColorSpace),
    F(CGImageGetHeight, cg_CGImageGetHeight),
    F(CGImageGetWidth, cg_CGImageGetWidth),

    F(CGBitmapContextCreate, cg_BitmapContextCreate),
    F(CGContextRelease, cg_ContextRelease),
    F(CGContextSetGrayFillColor, cg_SetGrayFillColor),
    F(CGContextSetTextDrawingMode, cg_noop),
    F(CGColorSpaceCreateDeviceGray, cg_ColorSpaceCreate),
    F(CGColorSpaceCreateDeviceRGB, cg_ColorSpaceCreate),
    F(CGColorSpaceRelease, cg_noop),
    F(UIGraphicsPushContext, ui_PushContext),
    F(UIGraphicsPopContext, ui_PopContext),
    F(UIGraphicsBeginImageContext, ui_UIGraphicsBeginImageContext),
    F(UIGraphicsEndImageContext, ui_UIGraphicsEndImageContext),
    F(UIGraphicsGetCurrentContext, ui_UIGraphicsGetCurrentContext),
    F(UIGraphicsGetImageFromCurrentImageContext, ui_UIGraphicsGetImageFromCurrentImageContext),
    F(UIImageJPEGRepresentation, ui_UIImageJPEGRepresentation),
    F(UIImagePNGRepresentation, ui_UIImagePNGRepresentation),
    F(NSHomeDirectory, ns_NSHomeDirectory),
    F(NSSearchPathForDirectoriesInDomains, ns_NSSearchPathForDirectoriesInDomains),
    F(NSClassFromString, ns_NSClassFromString),

    // Keys and notifications
    V(kCFAllocatorDefault, &s_kCFAllocatorDefault),
    V(kCFRunLoopDefaultMode, &s_kCFRunLoopDefaultMode),
    V(kEAGLColorFormatRGB565, &s_kEAGLColorFormatRGB565),
    V(kEAGLDrawablePropertyColorFormat, &s_kEAGLDrawablePropertyColorFormat),
    V(kEAGLDrawablePropertyRetainedBacking, &s_kEAGLDrawablePropertyRetainedBacking),
    V(NSLocaleCountryCode, &s_NSLocaleCountryCode),
    V(NSErrorFailingURLStringKey, &s_NSErrorFailingURLStringKey),
    V(MPMoviePlayerPlaybackDidFinishNotification, &s_MPMoviePlayerPlaybackDidFinishNotification),
    V(MPMoviePlayerContentPreloadDidFinishNotification, &s_MPMoviePlayerContentPreloadDidFinishNotification),
    V(UIKeyboardWillShowNotification, &s_UIKeyboardWillShowNotification),
    V(UIKeyboardWillHideNotification, &s_UIKeyboardWillHideNotification),
    V(__CFConstantStringClassReference, s_CFConstantStringClassReference),
    {"__CFConstantStringClassReference", (uintptr_t)s_CFConstantStringClassReference, 1},
    {"___CFConstantStringClassReference", (uintptr_t)s_CFConstantStringClassReference, 1},
    {"__objc_empty_cache", (uintptr_t)s_objc_empty_cache, 1},
    {"___objc_empty_cache", (uintptr_t)s_objc_empty_cache, 1},
    {"__objc_empty_vtable", (uintptr_t)s_objc_empty_vtable, 1},
    {"___objc_empty_vtable", (uintptr_t)s_objc_empty_vtable, 1},
    {"_OBJC_EHTYPE_id", (uintptr_t)s_OBJC_EHTYPE_id, 1},
    {"OBJC_EHTYPE_id", (uintptr_t)s_OBJC_EHTYPE_id, 1},

    // ObjC Classes
    V(OBJC_CLASS_$_CAEAGLLayer, &cls_CAEAGLLayer),
    V(OBJC_CLASS_$_EAGLContext, &cls_EAGLContext),
    V(OBJC_CLASS_$_MPMoviePlayerController, &cls_MPMoviePlayerController),
    V(OBJC_CLASS_$_NSArray, &cls_NSArray),
    V(OBJC_CLASS_$_NSAutoreleasePool, &cls_NSAutoreleasePool),
    V(OBJC_CLASS_$_NSBundle, &cls_NSBundle),
    V(OBJC_CLASS_$_NSData, &cls_NSData),
    V(OBJC_CLASS_$_NSDate, &cls_NSDate),
    V(OBJC_CLASS_$_NSDateFormatter, &cls_NSDateFormatter),
    V(OBJC_CLASS_$_NSDictionary, &cls_NSDictionary),
    V(OBJC_CLASS_$_NSError, &cls_NSError),
    V(OBJC_CLASS_$_NSFileManager, &cls_NSFileManager),
    V(OBJC_CLASS_$_NSLocale, &cls_NSLocale),
    V(OBJC_CLASS_$_NSMutableArray, &cls_NSMutableArray),
    V(OBJC_CLASS_$_NSMutableData, &cls_NSMutableData),
    V(OBJC_CLASS_$_NSMutableURLRequest, &cls_NSMutableURLRequest),
    V(OBJC_CLASS_$_NSNotificationCenter, &cls_NSNotificationCenter),
    V(OBJC_CLASS_$_NSNumber, &cls_NSNumber),
    V(OBJC_CLASS_$_NSObject, &cls_NSObject),
    V(OBJC_CLASS_$_NSString, &cls_NSString),
    V(OBJC_CLASS_$_NSThread, &cls_NSThread),
    V(OBJC_CLASS_$_NSTimer, &cls_NSTimer),
    V(OBJC_CLASS_$_NSURL, &cls_NSURL),
    V(OBJC_CLASS_$_NSURLConnection, &cls_NSURLConnection),
    V(OBJC_CLASS_$_NSURLRequest, &cls_NSURLRequest),
    V(OBJC_CLASS_$_NSUserDefaults, &cls_NSUserDefaults),
    V(OBJC_CLASS_$_UIAccelerometer, &cls_UIAccelerometer),
    V(OBJC_CLASS_$_UIActivityIndicatorView, &cls_UIActivityIndicatorView),
    V(OBJC_CLASS_$_UIAlertView, &cls_UIAlertView),
    V(OBJC_CLASS_$_UIApplication, &cls_UIApplication),
    V(OBJC_CLASS_$_UIButton, &cls_UIButton),
    V(OBJC_CLASS_$_UIColor, &cls_UIColor),
    V(OBJC_CLASS_$_UIDevice, &cls_UIDevice),
    V(OBJC_CLASS_$_UIFont, &cls_UIFont),
    V(OBJC_CLASS_$_UIImage, &cls_UIImage),
    V(OBJC_CLASS_$_UIImagePickerController, &cls_UIImagePickerController),
    V(OBJC_CLASS_$_UIImageView, &cls_UIImageView),
    V(OBJC_CLASS_$_UILabel, &cls_UILabel),
    V(OBJC_CLASS_$_UINavigationController, &cls_UINavigationController),
    V(OBJC_CLASS_$_UIScreen, &cls_UIScreen),
    V(OBJC_CLASS_$_UITextField, &cls_UITextField),
    V(OBJC_CLASS_$_UITextView, &cls_UITextView),
    V(OBJC_CLASS_$_UIView, &cls_UIView),
    V(OBJC_CLASS_$_UIViewController, &cls_UIViewController),
    V(OBJC_CLASS_$_UIWebView, &cls_UIWebView),
    V(OBJC_CLASS_$_UIWindow, &cls_UIWindow),

    // Metaclasses
    V(OBJC_METACLASS_$_NSObject, &meta_NSObject),
    V(OBJC_METACLASS_$_UINavigationController, &meta_UINavigationController),
    V(OBJC_METACLASS_$_UIView, &meta_UIView),
    V(OBJC_METACLASS_$_UIViewController, &meta_UIViewController),
};
const int objc_imports_count = sizeof(objc_imports) / sizeof(objc_imports[0]);
