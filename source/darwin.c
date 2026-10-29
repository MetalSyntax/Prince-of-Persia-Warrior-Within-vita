/**
 * @file darwin.c
 * @brief iOS Darwin libc, libgcc, and pthread emulation layer over Vita newlib.
 * @details Implements ARMv6 softfp Darwin system calls, POSIX threads with SceUID safety,
 *          virtual filesystem path translation to ux0:data/, high-resolution timers,
 *          and mathematical builtins.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

// ---------------------------------------------------------------- paths

static void normalize_path_slashes(char *p) {
    if (!p) return;
    // Replace all backslashes with forward slashes
    for (char *s = p; *s; s++) {
        if (*s == '\\') *s = '/';
    }
    // Collapse consecutive slashes and /./
    // Preserving "ux0:/", "ur0:/", "app0:/"
    char *dst = p;
    char *src = p;
    if (!strncmp(src, "ux0:/", 5) || !strncmp(src, "ur0:/", 5) || !strncmp(src, "app0:/", 6)) {
        while (*src && *src != '/') *dst++ = *src++;
        if (*src == '/') *dst++ = *src++;
    } else if (!strncmp(src, "ux0:", 4) || !strncmp(src, "ur0:", 4) || !strncmp(src, "app0:", 5)) {
        while (*src && *src != ':') *dst++ = *src++;
        if (*src == ':') *dst++ = *src++;
    }
    while (*src) {
        if (*src == '/') {
            // Skip repeated slashes
            while (*(src + 1) == '/') src++;
            // Check for /./
            if (*(src + 1) == '.' && (*(src + 2) == '/' || *(src + 2) == '\0')) {
                src += 2;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static void ensure_parent_dirs(const char *path) {
    if (!path) return;
    char buf[512];
    strncpy(buf, path, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) return;
    *slash = '\0';
    char *p = buf;
    if (!strncmp(p, "ux0:", 4) || !strncmp(p, "ur0:", 4) || !strncmp(p, "app0:", 5)) {
        p = strchr(buf, '/');
        if (!p) return;
        p++;
    } else {
        while (*p == '/') p++;
    }
    for (char *s = p; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            sceIoMkdir(buf, 0777);
            *s = '/';
        }
    }
    sceIoMkdir(buf, 0777);
}

// Paths created by the game (NSHomeDirectory, bundle) are redirected appropriately.
const char *darwin_path(const char *in, char *out, size_t outlen) {
    if (!in || !in[0]) {
        if (outlen > 0) out[0] = '\0';
        return out;
    }

    char clean[512];
    strncpy(clean, in, sizeof(clean));
    clean[sizeof(clean) - 1] = '\0';

    // Normalize backslashes first
    for (char *s = clean; *s; s++) {
        if (*s == '\\') *s = '/';
    }

    // If it's already an absolute Vita path:
    if (!strncmp(clean, "ux0:", 4) || !strncmp(clean, "ur0:", 4) || !strncmp(clean, "app0:", 5)) {
        normalize_path_slashes(clean);
        snprintf(out, outlen, "%s", clean);
        return out;
    }

    // Clean relative path: skip leading slashes and ./
    const char *rel = clean;
    while (*rel == '/' || (*rel == '.' && (*(rel + 1) == '/' || *(rel + 1) == '\0'))) {
        if (*rel == '/') rel++;
        else if (*rel == '.') {
            rel++;
            if (*rel == '/') rel++;
        }
    }

    // Route saves / records / profiles to DOCS_PATH
    if (!strncmp(rel, "save", 4) || !strncmp(rel, "Profile", 7) || !strncmp(rel, "record", 6) || !strncmp(rel, "Save", 4)) {
        snprintf(out, outlen, "%s%s", DOCS_PATH, rel);
    } else if (!strncmp(rel, "saves", 5) || !strncmp(rel, "logs", 4) || !strncmp(rel, "game_app", 8)) {
        snprintf(out, outlen, "%s%s", DATA_PATH, rel);
    } else {
        snprintf(out, outlen, "%s%s", BUNDLE_PATH, rel);
    }

    normalize_path_slashes(out);
    return out;
}

static FILE *d_fopen(const char *path, const char *mode) {
    if (!path || !path[0]) {
        l_warn("fopen(NULL or empty, %s) -> NULL", mode ? mode : "");
        return NULL;
    }
    char p[512];
    darwin_path(path, p, sizeof(p));
    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
        ensure_parent_dirs(p);
    }
    FILE *f = fopen(p, mode);
    // If not found in primary path, try fallback between BUNDLE_PATH and DATA_PATH
    if (!f && strncmp(path, "ux0:", 4) != 0 && strncmp(path, "ur0:", 4) != 0) {
        char fallback[1024];
        char clean[512];
        strncpy(clean, path, sizeof(clean));
        clean[sizeof(clean) - 1] = '\0';
        for (char *s = clean; *s; s++) if (*s == '\\') *s = '/';
        const char *rel = clean;
        while (*rel == '/' || (*rel == '.' && (*(rel + 1) == '/' || *(rel + 1) == '\0'))) {
            if (*rel == '/') rel++;
            else if (*rel == '.') {
                rel++;
                if (*rel == '/') rel++;
            }
        }
        if (!strncmp(p, BUNDLE_PATH, strlen(BUNDLE_PATH))) {
            snprintf(fallback, sizeof(fallback), DATA_PATH "%s", rel);
            normalize_path_slashes(fallback);
            if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
                ensure_parent_dirs(fallback);
            }
            f = fopen(fallback, mode);
            if (f) {
                l_info("fopen(%s) found in DATA_PATH fallback: %s", path, fallback);
                return f;
            }
        } else if (!strncmp(p, DATA_PATH, strlen(DATA_PATH))) {
            snprintf(fallback, sizeof(fallback), BUNDLE_PATH "%s", rel);
            normalize_path_slashes(fallback);
            if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
                ensure_parent_dirs(fallback);
            }
            f = fopen(fallback, mode);
            if (f) {
                l_info("fopen(%s) found in BUNDLE_PATH fallback: %s", path, fallback);
                return f;
            }
        }
    }
    if (!f) {
        // Read misses are the game probing for saves/profiles that don't exist yet: not an error.
        if (mode && mode[0] == 'r' && !strchr(mode, '+'))
            l_debug("fopen(%s [orig %s], %s) -> NULL (not found)", p, path, mode);
        else
            l_warn("fopen(%s [orig %s], %s) -> NULL", p, path, mode ? mode : "");
    }
    return f;
}

static size_t d_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) {
        l_warn("fread(ptr=%p, size=%u, nmemb=%u, stream=NULL) called! Protected from crash.", ptr, (unsigned)size, (unsigned)nmemb);
        return 0;
    }
    if (!ptr || !size || !nmemb) return 0;
    return fread(ptr, size, nmemb, stream);
}

static size_t d_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) {
        l_warn("fwrite(stream=NULL) called! Protected from crash.");
        return 0;
    }
    if (!ptr || !size || !nmemb) return 0;
    return fwrite(ptr, size, nmemb, stream);
}

static int d_fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    return fseek(stream, offset, whence);
}

static long d_ftell(FILE *stream) {
    if (!stream) return -1;
    return ftell(stream);
}

static int d_fclose(FILE *stream) {
    if (!stream) return 0;
    return fclose(stream);
}

static int d_feof(FILE *stream) {
    if (!stream) return 1;
    return feof(stream);
}

static int d_ferror(FILE *stream) {
    if (!stream) return 0;
    return ferror(stream);
}

static int d_fflush(FILE *stream) {
    if (!stream) return 0;
    return fflush(stream);
}

static int d_fgetc(FILE *stream) {
    if (!stream) return EOF;
    return fgetc(stream);
}

static char *d_fgets(char *s, int size, FILE *stream) {
    if (!stream || !s) return NULL;
    return fgets(s, size, stream);
}

static int d_fputs(const char *s, FILE *stream) {
    if (!stream || !s) return 0;
    return fputs(s, stream);
}

static int d_mkdir(const char *path, int mode) {
    char p[512];
    return mkdir(darwin_path(path, p, sizeof(p)), mode);
}

static int d_remove(const char *path) {
    if (!path) return -1;
    char p[512];
    return remove(darwin_path(path, p, sizeof(p)));
}

static int d_unlink(const char *path) {
    if (!path) return -1;
    char p[512];
    return unlink(darwin_path(path, p, sizeof(p)));
}

static int d_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -1;
    char p1[512], p2[512];
    return rename(darwin_path(oldpath, p1, sizeof(p1)), darwin_path(newpath, p2, sizeof(p2)));
}

// ---------------------------------------------------------------- stdio / errno / _chk

static FILE *d_stderrp, *d_stdinp, *d_stdoutp;

static int *d___error(void) { return &errno; }

static void *d___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { return memcpy(d, s, n); }

static char *d_strcpy(char *dest, const char *src) {
    if (!dest) return NULL;
    if (!src) {
        l_warn("strcpy(dest=%p, src=NULL) called! Protected from crash.", dest);
        dest[0] = '\0';
        return dest;
    }
    return strcpy(dest, src);
}

static char *d_strncpy(char *dest, const char *src, size_t n) {
    if (!dest) return NULL;
    if (!src) {
        l_warn("strncpy(dest=%p, src=NULL, n=%u) called! Protected from crash.", dest, (unsigned)n);
        if (n > 0) dest[0] = '\0';
        return dest;
    }
    return strncpy(dest, src, n);
}

static int d_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return strcmp(s1, s2);
}

static size_t d_strlen(const char *s) {
    if (!s) return 0;
    return strlen(s);
}

static char *d_strcat(char *dest, const char *src) {
    if (!dest) return NULL;
    if (!src) return dest;
    return strcat(dest, src);
}

static char *d___strcpy_chk(char *dest, const char *src, size_t dn) {
    (void)dn;
    return d_strcpy(dest, src);
}

static char *d___strncpy_chk(char *dest, const char *src, size_t len, size_t dn) {
    (void)dn;
    return d_strncpy(dest, src, len);
}

// ---------------------------------------------------------------- printf with Darwin ABI (darwin_printf.c)

static int d_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    printf("%s", buf);
    return r;
}

static int d_fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    fputs(buf, stream);
    return r;
}

static int d_snprintf(char *out, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(out, size, fmt, &w);
    va_end(ap);
    return r;
}

static int d_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, INT_MAX, fmt, &w);
    va_end(ap);
    return r;
}
static int d___sprintf_chk(char *buf, int flag, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, len, fmt, &w);
    va_end(ap);
    return r;
}
static int d_vsprintf(char *buf, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, INT_MAX, fmt, &w);
}
static int d_vsnprintf(char *buf, size_t size, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, size, fmt, &w);
}

static void d___assert_rtn(const char *func, const char *file, int line, const char *expr) {
    l_error("guest assertion failed: %s (%s:%d %s)", expr, file, line, func);
}

// NSLog(@"format", ...): format is a CFString (C string pointer at +8).
static void d_NSLog(void *cfstr, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, cfstr);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), cfstr ? *(const char **)((uint8_t *)cfstr + 8) : "(nil)", &w);
    va_end(ap);
    l_info("NSLog: %s", buf);
}

static int qsort_guest_cmp;
static int qsort_tramp(const void *a, const void *b) {
    return (int)call_guest((void *)qsort_guest_cmp, (uint32_t)a, (uint32_t)b, 0, 0);
}
static void d_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    int prev = qsort_guest_cmp; // nested qsort: restored upon return
    qsort_guest_cmp = (int)cmp;
    qsort(base, n, sz, qsort_tramp);
    qsort_guest_cmp = prev;
}

// ---------------------------------------------------------------- time / system

static uint64_t d_mach_absolute_time(void) { return sceKernelGetProcessTimeWide(); }

typedef struct { uint32_t numer, denom; } mach_timebase_info_t;
static int d_mach_timebase_info(mach_timebase_info_t *info) {
    info->numer = 1000; // mach_absolute_time is in microseconds
    info->denom = 1;
    return 0;
}

static int d_usleep(unsigned us) {
    sceKernelDelayThread(us);
    return 0;
}

static int d_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (!strcmp(name, "hw.machine")) {
        const char *model = "iPhone2,1"; // iPhone 3GS: game selects capable GLES1 texture profile
        size_t len = strlen(model) + 1;
        if (oldp && *oldlenp >= len)
            memcpy(oldp, model, len);
        *oldlenp = len;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static void d_exit(int code) {
    l_info("guest invoked exit(%d)", code);
    sceKernelExitProcess(0);
}

// ---------------------------------------------------------------- networking (offline stub)

static int d_net_fail(void) {
    errno = ENETDOWN;
    return -1;
}
static void *d_gethostbyname(const char *n) { return NULL; }
static int d_select(int n, void *r, void *w, void *e, void *t) { return 0; }

// ---------------------------------------------------------------- pthreads

// Darwin: pthread_mutex_t = {long sig; char opaque[40]}. Vita mutex stored in opaque.
// Darwin PTHREAD_MUTEX_INITIALIZER signature: 0x32AAABA7 (lazy initialized on first use).
#define DARWIN_MUTEX_SIG_INIT 0x32AAABA7
#define ML_MUTEX_SIG 0x4D4C5458
#define ML_MUTEXATTR_SIG 0x4D4C5441
typedef struct { uint32_t sig; pthread_mutex_t m; } d_mutex;
typedef struct { uint32_t sig; int type; } d_mutexattr;

static int d_pthread_mutexattr_init(d_mutexattr *a) {
    a->sig = ML_MUTEXATTR_SIG;
    a->type = 0;
    return 0;
}
static int d_pthread_mutexattr_settype(d_mutexattr *a, int type) {
    a->type = type; // Darwin: 0 normal, 1 errorcheck, 2 recursive
    return 0;
}
static int d_pthread_mutex_init(d_mutex *m, const d_mutexattr *a) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (a && a->sig == ML_MUTEXATTR_SIG)
        pthread_mutexattr_settype(&attr, a->type == 2 ? PTHREAD_MUTEX_RECURSIVE
                                         : a->type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    int r = pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    m->sig = ML_MUTEX_SIG;
    return r;
}
static void mutex_lazy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG) {
        if (m->sig != DARWIN_MUTEX_SIG_INIT)
            l_error("uninitialized mutex (sig 0x%08X) at %p", (unsigned)m->sig, m);
        d_pthread_mutex_init(m, NULL);
    }
}
static int d_pthread_mutex_lock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_lock(&m->m);
}
static int d_pthread_mutex_unlock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_unlock(&m->m);
}
static int d_pthread_mutex_destroy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG)
        return 0;
    m->sig = 0;
    return pthread_mutex_destroy(&m->m);
}

typedef struct { void *fn, *arg; } guest_thread;
static void *thread_tramp(void *p) {
    guest_thread t = *(guest_thread *)p;
    free(p);
    return (void *)call_guest(t.fn, (uint32_t)t.arg, 0, 0, 0);
}
static int d_pthread_create(pthread_t *thr, const void *darwin_attr, void *fn, void *arg) {
    guest_thread *t = malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); // iOS default 512 KB secondary thread stack
    int r = pthread_create(thr, &attr, thread_tramp, t);
    pthread_attr_destroy(&attr);
    l_debug("pthread_create(0x%08X) -> %d", (unsigned)fn - MACHO_SLIDE, r);
    return r;
}
static int d_pthread_detach(pthread_t t) {
    if (!t)
        return 0;
    // If handle is a Sony Vita SceUID (0x4001xxxx)
    if (((uintptr_t)t & 0xFFFF0000) == 0x40010000) {
        l_info("pthread_detach(SceUID 0x%08X) -> ok", (unsigned)(uintptr_t)t);
        return 0;
    }
    // If handle is a pointer to pte_thread_t in user memory
    uintptr_t addr = (uintptr_t)t;
    if (addr >= 0x81000000 && addr < 0x90000000 && (addr & 3) == 0) {
        uint32_t first = *(uint32_t *)addr;
        if ((first & 0xFFFF0000) == 0x40010000) {
            return pthread_detach(t);
        }
    }
    l_info("pthread_detach(%p) safely ignored", (void *)t);
    return 0;
}

static pthread_t d_pthread_self(void) {
    pthread_t p = pthread_self();
    if (p)
        return p;
    return (pthread_t)sceKernelGetThreadId();
}

static int d_pthread_setschedparam(pthread_t t, int policy, const void *param) { return 0; }

// Named semaphores (FMOD): Darwin sem_t* -> Vita SceUID.
typedef struct { SceUID id; } d_sem;
static d_sem *d_sem_open(const char *name, int oflag, ...) {
    va_list ap;
    va_start(ap, oflag);
    va_arg(ap, int); // mode
    unsigned value = va_arg(ap, unsigned);
    va_end(ap);
    d_sem *s = malloc(sizeof(*s));
    s->id = sceKernelCreateSema("ml_sem", 0, value, 0x7FFFFFFF, NULL);
    if (s->id < 0) {
        free(s);
        return (d_sem *)-1; // SEM_FAILED
    }
    return s;
}
static int d_sem_close(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    sceKernelDeleteSema(s->id);
    free(s);
    return 0;
}
static int d_sem_unlink(const char *name) { return 0; }
static int d_sem_wait(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelWaitSema(s->id, 1, NULL) < 0 ? -1 : 0;
}
static int d_sem_post(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelSignalSema(s->id, 1) < 0 ? -1 : 0;
}

// ---------------------------------------------------------------- C++ / blocks / exceptions

static int d___cxa_atexit(void *f, void *a, void *d) { return 0; } // games never terminate cleanly
static int d___cxa_guard_acquire(uint8_t *g) { return *g == 0; }
static void d___cxa_guard_release(uint8_t *g) { *g = 1; }
static void d___cxa_pure_virtual(void) { fatal_error("__cxa_pure_virtual"); }
static void d_sjlj_noop(void *ctx) {}
static void d_sjlj_resume(void *ctx) { fatal_error("C++/ObjC exception thrown (_Unwind_SjLj_Resume)"); }

static uint8_t d_NSConcreteStackBlock[32];
static void d_Block_object_assign(void **dst, void *src, int flags) { *dst = src; }
static void d_Block_object_dispose(void *obj, int flags) {}

static uint8_t d_dispatch_main_q[64];
static void *d_dispatch_get_global_queue(long prio, unsigned long flags) { return d_dispatch_main_q; }
static void d_dispatch_async(void *q, void *block) { l_debug("dispatch_async ignored"); }

// ---------------------------------------------------------------- libgcc softfp (Apple *vfp)

SOFTFP static float f_add(float a, float b) { return a + b; }
SOFTFP static float f_sub(float a, float b) { return a - b; }
SOFTFP static float f_mul(float a, float b) { return a * b; }
SOFTFP static float f_div(float a, float b) { return a / b; }
SOFTFP static double d_add(double a, double b) { return a + b; }
SOFTFP static double d_sub(double a, double b) { return a - b; }
SOFTFP static double d_mul(double a, double b) { return a * b; }
// compiler-rt *vfp comparisons: return 1 if condition is true (unlike libgcc -1/0/1).
SOFTFP static int f_eq(float a, float b) { return a == b; }
SOFTFP static int f_ne(float a, float b) { return !(a == b); }
SOFTFP static int f_gt(float a, float b) { return a > b; }
SOFTFP static int f_ge(float a, float b) { return a >= b; }
SOFTFP static int f_lt(float a, float b) { return a < b; }
SOFTFP static int f_le(float a, float b) { return a <= b; }
SOFTFP static int dd_gt(double a, double b) { return a > b; }
SOFTFP static int dd_ge(double a, double b) { return a >= b; }
SOFTFP static int dd_lt(double a, double b) { return a < b; }
SOFTFP static double f_ext(float a) { return a; }
SOFTFP static float d_trunc(double a) { return (float)a; }
SOFTFP static int f_toi(float a) { return (int)a; }
SOFTFP static int d_toi(double a) { return (int)a; }
SOFTFP static long long f_toll(float a) { return (long long)a; }
SOFTFP static long long d_toll(double a) { return (long long)a; }
SOFTFP static unsigned long long f_toull(float a) { return (unsigned long long)a; }
SOFTFP static float i_tof(int a) { return (float)a; }
SOFTFP static float u_tof(unsigned a) { return (float)a; }
SOFTFP static double i_tod(int a) { return (double)a; }
SOFTFP static double ll_tod(long long a) { return (double)a; }
SOFTFP static float ll_tof(long long a) { return (float)a; }

static int i_div(int a, int b) { return a / b; }
static int i_mod(int a, int b) { return a % b; }
static unsigned u_div(unsigned a, unsigned b) { return a / b; }
static unsigned u_mod(unsigned a, unsigned b) { return a % b; }
static long long ll_div(long long a, long long b) { return a / b; }
static long long ll_mod(long long a, long long b) { return a % b; }
static long long ll_mul(long long a, long long b) { return a * b; }
static unsigned long long ull_div(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long ull_mod(unsigned long long a, unsigned long long b) { return a % b; }
static int ll_cmp(long long a, long long b) { return a < b ? 0 : a == b ? 1 : 2; }


// Host libm, same softfp ABI as the rest of the build (libm has no Tag_ABI_VFP_args). Forcing
// pcs("aapcs-vfp") here passed args in d0 while log() read r0:r1: log2(15) came out ~21 and
// gameswf hash::set_raw_capacity asked for 1<<22 entries -> NULL from SwfAlloc (log 015).
extern float  host_acosf(float) __asm__("acosf");
extern float  host_cosf(float) __asm__("cosf");
extern float  host_sinf(float) __asm__("sinf");
extern float  host_tanf(float) __asm__("tanf");
extern float  host_expf(float) __asm__("expf");
extern float  host_logf(float) __asm__("logf");
extern float  host_log10f(float) __asm__("log10f");
extern float  host_floorf(float) __asm__("floorf");
extern long   host_lrintf(float) __asm__("lrintf");
extern float  host_asinf(float) __asm__("asinf");
extern float  host_atanf(float) __asm__("atanf");
extern float  host_ceilf(float) __asm__("ceilf");
extern float  host_roundf(float) __asm__("roundf");

extern double host_acos(double) __asm__("acos");
extern double host_asin(double) __asm__("asin");
extern double host_atan(double) __asm__("atan");
extern double host_atan2(double, double) __asm__("atan2");
extern double host_ceil(double) __asm__("ceil");
extern double host_cos(double) __asm__("cos");
extern double host_exp(double) __asm__("exp");
extern double host_floor(double) __asm__("floor");
extern double host_fmod(double, double) __asm__("fmod");
extern double host_ldexp(double, int) __asm__("ldexp");
extern double host_log(double) __asm__("log");
extern double host_log10(double) __asm__("log10");
extern double host_pow(double, double) __asm__("pow");
extern double host_sin(double) __asm__("sin");
extern double host_tan(double) __asm__("tan");
extern double host_fmax(double, double) __asm__("fmax");
extern double host_fmin(double, double) __asm__("fmin");

SOFTFP static float m_acosf(float x) { return host_acosf(x); }
SOFTFP static float m_cosf(float x) { return host_cosf(x); }
SOFTFP static float m_sinf(float x) { return host_sinf(x); }
SOFTFP static float m_tanf(float x) { return host_tanf(x); }
SOFTFP static float m_expf(float x) { return host_expf(x); }
SOFTFP static float m_logf(float x) { return host_logf(x); }
SOFTFP static float m_log10f(float x) { return host_log10f(x); }
SOFTFP static float m_floorf(float x) { return host_floorf(x); }
SOFTFP static long m_lrintf(float x) { return host_lrintf(x); }
SOFTFP static double m_cos(double x) { return host_cos(x); }
SOFTFP static double m_sin(double x) { return host_sin(x); }
SOFTFP static double m_floor(double x) { return host_floor(x); }
SOFTFP static double m_log10(double x) { return host_log10(x); }
SOFTFP static double m_atan2(double y, double x) { return host_atan2(y, x); }
SOFTFP static double m_fmod(double x, double y) { return host_fmod(x, y); }
SOFTFP static double m_pow(double x, double y) { return host_pow(x, y); }
SOFTFP static double m_ldexp(double x, int e) { return host_ldexp(x, e); }

// ---------------------------------------------------------------- ctype / rune locale
typedef struct {
    char magic[8];
    char encoding[32];
    void *invalid_rune;
    int32_t min_rune;
    int32_t max_rune;
    int32_t runetype[256];
    int32_t maplower[256];
    int32_t mapupper[256];
} darwin_rune_locale;

static darwin_rune_locale d_DefaultRuneLocale;
static void init_rune_locale(void) {
    memcpy(d_DefaultRuneLocale.magic, "RuneMagi", 8);
    memcpy(d_DefaultRuneLocale.encoding, "NONE", 5);
    for (int i = 0; i < 256; i++) {
        int32_t t = 0;
        if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z')) t |= 0x00000100;
        if (i >= '0' && i <= '9') t |= 0x00000004;
        if (i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\f' || i == '\v') t |= 0x00004000;
        d_DefaultRuneLocale.runetype[i] = t;
        d_DefaultRuneLocale.maplower[i] = tolower(i);
        d_DefaultRuneLocale.mapupper[i] = toupper(i);
    }
}
static int d_tolower(int c) { return tolower(c); }
static int d_toupper(int c) { return toupper(c); }

// ---------------------------------------------------------------- extra math softfp
SOFTFP static float m_asinf(float x) { return host_asinf(x); }
SOFTFP static float m_atanf(float x) { return host_atanf(x); }
SOFTFP static float m_ceilf(float x) { return host_ceilf(x); }
SOFTFP static float m_roundf(float x) { return host_roundf(x); }
SOFTFP static double m_acos(double x) { return host_acos(x); }
SOFTFP static double m_asin(double x) { return host_asin(x); }
SOFTFP static double m_atan(double x) { return host_atan(x); }
SOFTFP static double m_ceil(double x) { return host_ceil(x); }
SOFTFP static double m_tan(double x) { return host_tan(x); }
SOFTFP static double m_exp(double x) { return host_exp(x); }
SOFTFP static double m_log(double x) { return host_log(x); }
SOFTFP static double m_log2(double x) {
    if (x <= 0.0) return -INFINITY;
    return host_log(x) * 1.442695040888963407359924681001892137;
}
SOFTFP static double m_fmax(double x, double y) { return host_fmax(x, y); }
SOFTFP static double m_fmin(double x, double y) { return host_fmin(x, y); }

// ---------------------------------------------------------------- SystemConfiguration / Network
static void *d_SCNetworkReachabilityCreateWithName(void *allocator, const char *nodename) {
    return (void *)0x5C000001;
}
static int d_SCNetworkReachabilityGetFlags(void *target, uint32_t *flags) {
    if (flags) *flags = 0x00020002;
    return 1;
}

// ---------------------------------------------------------------- C++ atomic / exceptions / ABI
static void d_atomic_add(volatile int *p, int val) { __sync_fetch_and_add(p, val); }
static int d_exchange_and_add(volatile int *p, int val) { return __sync_fetch_and_add(p, val); }
static void *d_cxa_allocate_exception(size_t thrown_size) { return malloc(thrown_size); }
static void d_cxa_free_exception(void *thrown_exception) { free(thrown_exception); }
static void d_cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *)) {
    const char *type_name = "unknown";
    if (tinfo) {
        const char *name = *(const char **)((char *)tinfo + 4);
        if (name && (uintptr_t)name >= 0x80000000) {
            type_name = name;
        }
    }
    const char *what_msg = "no what() available";
    if (thrown_exception && (uintptr_t)thrown_exception >= 0x80000000) {
        uintptr_t *vtable = *(uintptr_t **)thrown_exception;
        if (vtable && (uintptr_t)vtable >= 0x80000000) {
            typedef const char *(*what_fn)(void *);
            what_fn what = (what_fn)vtable[2];
            if (what && (uintptr_t)what >= 0x80000000) {
                const char *w = what(thrown_exception);
                if (w && (uintptr_t)w >= 0x80000000) what_msg = w;
            }
        }
    }
    l_error("[FATAL] C++ exception thrown via __cxa_throw: type '%s', what: '%s'", type_name, what_msg);
    fatal_error("C++ exception thrown via __cxa_throw! type '%s', what: '%s'", type_name, what_msg);
}
static void *d_cxa_begin_catch(void *exceptionObject) { return exceptionObject; }
static void d_cxa_end_catch(void) {}
static void d_cxa_guard_abort(int64_t *guard) { *guard = 0; }
static void d_cxa_rethrow(void) { fatal_error("C++ rethrow!"); }
static void d_cxa_call_unexpected(void *exc) { fatal_error("C++ unexpected exception!"); }
static void *d_dynamic_cast(void *sub, void *src, void *dst, ptrdiff_t src2dst_offset) { return sub; }
static void d_throw_stub(const char *msg) { fatal_error("C++ exception: %s", msg); }

// ---------------------------------------------------------------- GCC 4.x std::string / std::wstring
typedef struct {
    size_t _M_length;
    size_t _M_capacity;
    int _M_refcount;
} gcc_rep;

static size_t s_empty_rep[4] = {0, 0, -1, 0};
static size_t s_wempty_rep[4] = {0, 0, -1, 0};

#define EMPTY_STRING ((char *)&s_empty_rep[3])
#define EMPTY_WSTRING ((wchar_t *)&s_wempty_rep[3])

static void *str_alloc(const char *s, size_t len) {
    if (!s || len == 0) return EMPTY_STRING;
    gcc_rep *rep = (gcc_rep *)malloc(sizeof(gcc_rep) + len + 1);
    rep->_M_length = len;
    rep->_M_capacity = len;
    rep->_M_refcount = 0;
    char *data = (char *)(rep + 1);
    memcpy(data, s, len);
    data[len] = '\0';
    return data;
}

static void str_free(char *p) {
    if (p && p != EMPTY_STRING) {
        gcc_rep *rep = ((gcc_rep *)p) - 1;
        if (__sync_sub_and_fetch(&rep->_M_refcount, 1) < 0) {
            free(rep);
        }
    }
}

static void str_init_empty(void **this_ptr) { *this_ptr = EMPTY_STRING; }
static void str_init_cstr(void **this_ptr, const char *s, void *alloc) { *this_ptr = str_alloc(s, s ? strlen(s) : 0); }
static void str_init_cstr_n(void **this_ptr, const char *s, size_t n, void *alloc) { *this_ptr = str_alloc(s, n); }
static void str_init_copy(void **this_ptr, void **other_ptr) {
    char *p = *other_ptr;
    if (p != EMPTY_STRING) {
        gcc_rep *rep = ((gcc_rep *)p) - 1;
        __sync_add_and_fetch(&rep->_M_refcount, 1);
    }
    *this_ptr = p;
}
static void str_init_sub(void **this_ptr, void **other_ptr, size_t pos, size_t n) {
    char *other_p = *other_ptr;
    size_t len = (other_p == EMPTY_STRING) ? 0 : (((gcc_rep *)other_p) - 1)->_M_length;
    if (pos > len) pos = len;
    if (pos + n > len) n = len - pos;
    *this_ptr = str_alloc(other_p + pos, n);
}
static void str_destruct(void **this_ptr) { str_free((char *)*this_ptr); }

static void *str_assign_cstr(void **this_ptr, const char *s, size_t n) {
    str_free((char *)*this_ptr);
    *this_ptr = str_alloc(s, n);
    return *this_ptr;
}

static void *str_assign_str(void **this_ptr, void **other_ptr) {
    char *p = *other_ptr;
    if (p != EMPTY_STRING) {
        gcc_rep *rep = ((gcc_rep *)p) - 1;
        __sync_add_and_fetch(&rep->_M_refcount, 1);
    }
    str_free((char *)*this_ptr);
    *this_ptr = p;
    return *this_ptr;
}

static void *str_append_cstr(void **this_ptr, const char *s, size_t n) {
    char *old_p = (char *)*this_ptr;
    size_t old_len = (old_p == EMPTY_STRING) ? 0 : (((gcc_rep *)old_p) - 1)->_M_length;
    size_t new_len = old_len + n;
    gcc_rep *new_rep = (gcc_rep *)malloc(sizeof(gcc_rep) + new_len + 1);
    new_rep->_M_length = new_len;
    new_rep->_M_capacity = new_len;
    new_rep->_M_refcount = 0;
    char *new_p = (char *)(new_rep + 1);
    if (old_len) memcpy(new_p, old_p, old_len);
    if (n) memcpy(new_p + old_len, s, n);
    new_p[new_len] = '\0';
    str_free(old_p);
    *this_ptr = new_p;
    return new_p;
}

static void *str_append_str(void **this_ptr, void **other_ptr) {
    char *other_p = (char *)*other_ptr;
    size_t other_len = (other_p == EMPTY_STRING) ? 0 : (((gcc_rep *)other_p) - 1)->_M_length;
    return str_append_cstr(this_ptr, other_p, other_len);
}

static void *str_append_char(void **this_ptr, size_t n, char c) {
    char *buf = malloc(n);
    memset(buf, c, n);
    void *ret = str_append_cstr(this_ptr, buf, n);
    free(buf);
    return ret;
}

static void str_push_back(void **this_ptr, char c) { str_append_cstr(this_ptr, &c, 1); }

static int str_compare_cstr(void **this_ptr, const char *s) {
    const char *p = (const char *)*this_ptr;
    return strcmp(p ? p : "", s ? s : "");
}

static int str_compare_str(void **this_ptr, void **other_ptr) {
    const char *p1 = (const char *)*this_ptr;
    const char *p2 = (const char *)*other_ptr;
    return strcmp(p1 ? p1 : "", p2 ? p2 : "");
}

static int str_compare_sub_cstr(void **this_ptr, size_t pos, size_t n, const char *s) {
    const char *p = (const char *)*this_ptr;
    size_t len = (p == EMPTY_STRING) ? 0 : (((gcc_rep *)p) - 1)->_M_length;
    if (pos > len) pos = len;
    return strncmp(p + pos, s, n);
}

static size_t str_find_cstr(void **this_ptr, const char *s, size_t pos, size_t n) {
    const char *p = (const char *)*this_ptr;
    if (!p) return (size_t)-1;
    size_t len = (p == EMPTY_STRING) ? 0 : (((gcc_rep *)p) - 1)->_M_length;
    if (pos >= len) return (size_t)-1;
    const char *found = strstr(p + pos, s);
    return found ? (size_t)(found - p) : (size_t)-1;
}

static size_t str_find_char(void **this_ptr, char c, size_t pos) {
    const char *p = (const char *)*this_ptr;
    if (!p) return (size_t)-1;
    size_t len = (p == EMPTY_STRING) ? 0 : (((gcc_rep *)p) - 1)->_M_length;
    if (pos >= len) return (size_t)-1;
    const char *found = strchr(p + pos, c);
    return found ? (size_t)(found - p) : (size_t)-1;
}

static void *str_erase(void **this_ptr, size_t pos, size_t n) {
    char *p = (char *)*this_ptr;
    if (p == EMPTY_STRING) return p;
    gcc_rep *rep = ((gcc_rep *)p) - 1;
    if (pos >= rep->_M_length) return p;
    if (pos + n > rep->_M_length) n = rep->_M_length - pos;
    memmove(p + pos, p + pos + n, rep->_M_length - (pos + n) + 1);
    rep->_M_length -= n;
    return p;
}

static void str_reserve(void **this_ptr, size_t res) {}
static void str_resize(void **this_ptr, size_t n, char c) {}
static void *str_mutate(void **this_ptr, size_t a, size_t b, size_t c) { return *this_ptr; }
static void *str_leak_hard(void **this_ptr) { return *this_ptr; }
static void str_rep_destroy(void *rep, void *alloc) { free(rep); }
static char *str_end(void **this_ptr) {
    char *p = (char *)*this_ptr;
    size_t len = (p == EMPTY_STRING) ? 0 : (((gcc_rep *)p) - 1)->_M_length;
    return p + len;
}

// wstring shims
static void wstr_init_empty(void **this_ptr) { *this_ptr = EMPTY_WSTRING; }
static void wstr_destruct(void **this_ptr) {
    wchar_t *p = *this_ptr;
    if (p && p != EMPTY_WSTRING) {
        gcc_rep *rep = ((gcc_rep *)p) - 1;
        if (__sync_sub_and_fetch(&rep->_M_refcount, 1) < 0) free(rep);
    }
}
static void wstr_init_copy(void **this_ptr, void **other_ptr) {
    wchar_t *p = *other_ptr;
    if (p != EMPTY_WSTRING) {
        gcc_rep *rep = ((gcc_rep *)p) - 1;
        __sync_add_and_fetch(&rep->_M_refcount, 1);
    }
    *this_ptr = p;
}
static void *wstr_assign_cstr(void **this_ptr, const wchar_t *s, size_t n) { return *this_ptr; }
static void *wstr_assign_str(void **this_ptr, void **other_ptr) { return *this_ptr; }
static void *wstr_erase(void **this_ptr, void *it) { return *this_ptr; }
static void *wstr_insert(void **this_ptr, void *it, wchar_t c) { return *this_ptr; }
static void *wstr_replace(void **this_ptr, void *a, void *b, const wchar_t *s, size_t n) { return *this_ptr; }
static void wstr_init_cstr(void **this_ptr, const wchar_t *s, void *alloc) { *this_ptr = EMPTY_WSTRING; }
static void wstr_init_sub(void **this_ptr, void **other_ptr, size_t pos, size_t n) { *this_ptr = EMPTY_WSTRING; }
static void wstr_init_fill(void **this_ptr, size_t n, wchar_t c, void *alloc) { *this_ptr = EMPTY_WSTRING; }
static void *wstr_leak_hard(void **this_ptr) { return *this_ptr; }
static void *wstr_construct(size_t n, wchar_t c, void *alloc) { return EMPTY_WSTRING; }
static void wstr_rep_destroy(void *rep, void *alloc) {}
static void wstr_alloc_hider_init(void *this_ptr, wchar_t *dat, void *alloc) {}

// C++ IO / RTTI stubs
static void *std_ostringstream_init(void *this_ptr, int mode) { return this_ptr; }
static void std_ostringstream_destruct(void *this_ptr) {}
static void std_ios_base_init(void *this_ptr) {}
static void std_ios_base_destruct(void *this_ptr) {}
static void *std_stringbuf_str(void *ret, void *this_ptr) {
    *(char **)ret = EMPTY_STRING;
    return ret;
}
static char std_basic_ios_widen(void *this_ptr, char c) { return c; }
static void *std_ostream_insert_int(void *this_ptr, int val) { return this_ptr; }
static void *std_ostream_insert_long(void *this_ptr, long val) { return this_ptr; }

static void *cxx_type_info_vtable[16] = {0};

static int d_pthread_join(pthread_t thread, void **retval) {
    if (((uintptr_t)thread & 0xFFFF0000) == 0x40010000) {
        return sceKernelWaitThreadEnd((SceUID)(uintptr_t)thread, NULL, NULL);
    }
    return pthread_join(thread, retval);
}
static int d_pthread_cancel(pthread_t thread) { return 0; }
static int d_pthread_setcanceltype(int type, int *oldtype) { return 0; }
static void d_pthread_testcancel(void) {}
static int d_pthread_mutexattr_destroy(pthread_mutexattr_t *attr) { return 0; }

static int d_ioctl(int fd, unsigned long request, ...) { return 0; }
static ssize_t d_recvfrom(int s, void *buf, size_t len, int flags, void *from, void *fromlen) { return -1; }
static ssize_t d_sendto(int s, const void *msg, size_t len, int flags, const void *to, uint32_t tolen) { return -1; }
static int d_getsockname(int s, void *name, void *namelen) { return -1; }
static int d_getsockopt(int s, int level, int optname, void *optval, void *optlen) { return -1; }
static int d_gethostname(char *name, size_t len) { if (name && len) strncpy(name, "PSVita", len); return 0; }
static void *d_getprotobyname(const char *name) { return NULL; }
static char *d_inet_ntoa(uint32_t in) { return "127.0.0.1"; }
static int d_sigaction(int signum, const void *act, void *oldact) { return 0; }
static int d_wprintf(const wchar_t *fmt, ...) { return 0; }

// ---------------------------------------------------------------- import table

#define F(name, fn) {"_" #name, (uintptr_t)(fn), 0}
#define F_RAW(name, fn) {name, (uintptr_t)(fn), 0}
#define D(name) {"_" #name, (uintptr_t)(name), 0}
#define D_RAW(name) {name, (uintptr_t)(name), 0}
#define V(name, ptr) {"_" #name, (uintptr_t)(ptr), 1}
#define V_RAW(name, ptr) {name, (uintptr_t)(ptr), 1}

const import_entry darwin_imports[] = {
    // memory / strings
    D(malloc), D(calloc), D(realloc), D(free), D(memcpy), D(memmove), D(memset), D(memcmp),
    D(memchr), D(bzero), F(strlen, d_strlen), F(strcmp, d_strcmp), F(strcpy, d_strcpy), F(strncpy, d_strncpy), D(strstr), D(strtok),
    F(strcat, d_strcat), D(strchr), D(strrchr), D(strpbrk), D(strncmp), D(strncat), D(strcasecmp),
    D(strncasecmp), D(strlcpy), D(strtod), D(strtol), D(atol), D(atof), D(bsearch),
    D(atoi), D(rand), D(srand), F(sprintf, d_sprintf), F(vsprintf, d_vsprintf),
    F(vsnprintf, d_vsnprintf), F(snprintf, d_snprintf), F(printf, d_printf),
    F(fprintf, d_fprintf), D(sscanf), D(puts),
    F(remove, d_remove), F(rename, d_rename), F(unlink, d_unlink), D(chdir), D(clearerr),
    D(time), D(localtime), D(mktime), D(strftime), D(perror),
    F(qsort, d_qsort), F(_ZdlPv, free), F(_ZdaPv, free), F(_Znwm, malloc), F(_Znam, malloc),
    F(__memcpy_chk, d___memcpy_chk), F(__strcpy_chk, d___strcpy_chk), F(__strncpy_chk, d___strncpy_chk),
    F(__sprintf_chk, d___sprintf_chk), F(__assert_rtn, d___assert_rtn), F(__error, d___error),
    F(NSLog, d_NSLog),
    F(tolower, d_tolower), F(toupper, d_toupper),
    F(__tolower, d_tolower), F(__toupper, d_toupper),
    V(__DefaultRuneLocale, &d_DefaultRuneLocale),

    // wchar
    D(wcscmp), D(wcscpy), D(wcslen), D(wcsncpy), D(wcstombs), D(mbstowcs),
    F(wprintf, d_wprintf),

    // setjmp
    D(setjmp), D(longjmp),

    // stdio
    F(fopen, d_fopen), F(fclose, d_fclose), F(fread, d_fread), F(fwrite, d_fwrite),
    F(fseek, d_fseek), F(ftell, d_ftell), F(feof, d_feof), F(ferror, d_ferror),
    F(fflush, d_fflush), F(fputs, d_fputs), F(mkdir, d_mkdir), D(close),
    V(__stdoutp, &d_stdoutp), V(__stderrp, &d_stderrp), V(__stdinp, &d_stdinp),

    // time / system
    D(gettimeofday), F(usleep, d_usleep), D(sleep), D(sched_yield),
    F(mach_absolute_time, d_mach_absolute_time),
    F(mach_timebase_info, d_mach_timebase_info), F(sysctlbyname, d_sysctlbyname), F(exit, d_exit),

    // network
    F(socket, d_net_fail), F(connect, d_net_fail), F(bind, d_net_fail), F(listen, d_net_fail),
    F(accept, d_net_fail), F(send, d_net_fail), F(recv, d_net_fail), F(setsockopt, d_net_fail),
    F(fcntl, d_net_fail), F(inet_addr, d_net_fail), F(gethostbyname, d_gethostbyname),
    F(select, d_select), F(ioctl, d_ioctl), F(recvfrom, d_recvfrom), F(sendto, d_sendto),
    F(getsockname, d_getsockname), F(getsockopt, d_getsockopt), F(gethostname, d_gethostname),
    F(getprotobyname, d_getprotobyname), F(inet_ntoa, d_inet_ntoa), F(sigaction, d_sigaction),
    F(SCNetworkReachabilityCreateWithName, d_SCNetworkReachabilityCreateWithName),
    F(SCNetworkReachabilityGetFlags, d_SCNetworkReachabilityGetFlags),

    // pthreads / semaphores
    F(pthread_create, d_pthread_create), F(pthread_detach, d_pthread_detach), F(pthread_self, d_pthread_self),
    F(pthread_setschedparam, d_pthread_setschedparam), F(pthread_mutex_init, d_pthread_mutex_init),
    F(pthread_mutex_lock, d_pthread_mutex_lock), F(pthread_mutex_unlock, d_pthread_mutex_unlock),
    F(pthread_mutex_destroy, d_pthread_mutex_destroy), F(pthread_mutexattr_init, d_pthread_mutexattr_init),
    F(pthread_mutexattr_settype, d_pthread_mutexattr_settype), F(pthread_mutexattr_destroy, d_pthread_mutexattr_destroy),
    F(pthread_join, d_pthread_join), F(pthread_cancel, d_pthread_cancel),
    F(pthread_setcanceltype, d_pthread_setcanceltype), F(pthread_testcancel, d_pthread_testcancel),
    F(sem_open, d_sem_open), F(sem_close, d_sem_close), F(sem_unlink, d_sem_unlink),
    F(sem_wait, d_sem_wait), F(sem_post, d_sem_post),

    // C++ / blocks / exceptions / GCD
    F(__cxa_atexit, d___cxa_atexit), F(__cxa_guard_acquire, d___cxa_guard_acquire),
    F(__cxa_guard_release, d___cxa_guard_release), F(__cxa_guard_abort, d_cxa_guard_abort),
    F(__cxa_pure_virtual, d___cxa_pure_virtual),
    F(__cxa_allocate_exception, d_cxa_allocate_exception),
    F(__cxa_free_exception, d_cxa_free_exception),
    F(__cxa_throw, d_cxa_throw),
    F(__cxa_begin_catch, d_cxa_begin_catch),
    F(__cxa_end_catch, d_cxa_end_catch),
    F(__cxa_rethrow, d_cxa_rethrow),
    F(__cxa_call_unexpected, d_cxa_call_unexpected),
    F(__dynamic_cast, d_dynamic_cast),
    F(__gxx_personality_sj0, d_sjlj_noop),
    F(_Unwind_SjLj_Register, d_sjlj_noop), F(_Unwind_SjLj_Unregister, d_sjlj_noop),
    F(_Unwind_SjLj_Resume, d_sjlj_resume), V(_NSConcreteStackBlock, d_NSConcreteStackBlock),
    F(_Block_object_assign, d_Block_object_assign), F(_Block_object_dispose, d_Block_object_dispose),
    V(_dispatch_main_q, d_dispatch_main_q), F(dispatch_get_global_queue, d_dispatch_get_global_queue),
    F(dispatch_async, d_dispatch_async),

    // C++ atomic
    F_RAW("__ZN9__gnu_cxx12__atomic_addEPVii", d_atomic_add),
    F_RAW("__ZN9__gnu_cxx18__exchange_and_addEPVii", d_exchange_and_add),

    // C++ throw helpers
    F_RAW("__ZSt17__throw_bad_allocv", d_throw_stub),
    F_RAW("__ZSt19__throw_logic_errorPKc", d_throw_stub),
    F_RAW("__ZSt20__throw_length_errorPKc", d_throw_stub),
    F_RAW("__ZSt20__throw_out_of_rangePKc", d_throw_stub),

    // C++ STL string
    V_RAW("__ZNSs4_Rep20_S_empty_rep_storageE", s_empty_rep),
    F_RAW("__ZNSsC2Ev", str_init_empty),
    F_RAW("__ZNSsC1EPKcRKSaIcE", str_init_cstr),
    F_RAW("__ZNSsC1EPKcmRKSaIcE", str_init_cstr_n),
    F_RAW("__ZNSsC1ERKSs", str_init_copy),
    F_RAW("__ZNSsC1ERKSsmm", str_init_sub),
    F_RAW("__ZNSsD2Ev", str_destruct),
    F_RAW("__ZNSs6assignEPKcm", str_assign_cstr),
    F_RAW("__ZNSs6assignERKSs", str_assign_str),
    F_RAW("__ZNSs6appendEPKcm", str_append_cstr),
    F_RAW("__ZNSs6appendERKSs", str_append_str),
    F_RAW("__ZNSs6appendEmc", str_append_char),
    F_RAW("__ZNSs9push_backEc", str_push_back),
    F_RAW("__ZNSs5eraseEmm", str_erase),
    F_RAW("__ZNSs6insertEmRKSsmm", str_append_str),
    F_RAW("__ZNSs6resizeEmc", str_resize),
    F_RAW("__ZNSs7reserveEm", str_reserve),
    F_RAW("__ZNSs9_M_mutateEmmm", str_mutate),
    F_RAW("__ZNSs12_M_leak_hardEv", str_leak_hard),
    F_RAW("__ZNSs4_Rep10_M_destroyERKSaIcE", str_rep_destroy),
    F_RAW("__ZNKSs3endEv", str_end),
    F_RAW("__ZNKSs4findEPKcmm", str_find_cstr),
    F_RAW("__ZNKSs4findEcm", str_find_char),
    F_RAW("__ZNKSs7compareEPKc", str_compare_cstr),
    F_RAW("__ZNKSs7compareERKSs", str_compare_str),
    F_RAW("__ZNKSs7compareEmmPKc", str_compare_sub_cstr),

    // C++ STL wstring
    V_RAW("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE", s_wempty_rep),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEEC2Ev", wstr_init_empty),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_", wstr_init_copy),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEED2Ev", wstr_destruct),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE6assignEPKwm", wstr_assign_cstr),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_", wstr_assign_str),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS2_EE", wstr_erase),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE6insertEN9__gnu_cxx17__normal_iteratorIPwS2_EEw", wstr_insert),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE7replaceEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_PKwm", wstr_replace),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEEC1EPKwRKS1_", wstr_init_cstr),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_mm", wstr_init_sub),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEEC1EmwRKS1_", wstr_init_fill),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE12_M_leak_hardEv", wstr_leak_hard),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE12_S_constructEmwRKS1_", wstr_construct),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_destroyERKS1_", wstr_rep_destroy),
    F_RAW("__ZNSbIwSt11char_traitsIwESaIwEE12_Alloc_hiderC1EPwRKS1_", wstr_alloc_hider_init),

    // C++ IO / RTTI stubs
    F_RAW("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", std_ostringstream_init),
    F_RAW("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", std_ostringstream_destruct),
    F_RAW("__ZNSt8ios_base4InitC1Ev", std_ios_base_init),
    F_RAW("__ZNSt8ios_base4InitD1Ev", std_ios_base_destruct),
    F_RAW("__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", std_stringbuf_str),
    F_RAW("__ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc", std_basic_ios_widen),
    F_RAW("__ZNSolsEi", std_ostream_insert_int),
    F_RAW("__ZNSolsEl", std_ostream_insert_long),

    V_RAW("__ZTVN10__cxxabiv117__class_type_infoE", cxx_type_info_vtable),
    V_RAW("__ZTVN10__cxxabiv120__si_class_type_infoE", cxx_type_info_vtable),
    V_RAW("__ZTVN10__cxxabiv121__vmi_class_type_infoE", cxx_type_info_vtable),

    // softfp helpers
    F(__addsf3vfp, f_add), F(__subsf3vfp, f_sub), F(__mulsf3vfp, f_mul), F(__divsf3vfp, f_div),
    F(__adddf3vfp, d_add), F(__subdf3vfp, d_sub), F(__muldf3vfp, d_mul),
    F(__eqsf2vfp, f_eq), F(__nesf2vfp, f_ne), F(__gtsf2vfp, f_gt), F(__gesf2vfp, f_ge),
    F(__ltsf2vfp, f_lt), F(__lesf2vfp, f_le), F(__gtdf2vfp, dd_gt), F(__gedf2vfp, dd_ge),
    F(__ltdf2vfp, dd_lt), F(__extendsfdf2vfp, f_ext), F(__truncdfsf2vfp, d_trunc),
    F(__fixsfsivfp, f_toi), F(__fixdfsivfp, d_toi), F(__fixsfdi, f_toll), F(__fixdfdi, d_toll),
    F(__fixunssfdi, f_toull), F(__floatsisfvfp, i_tof), F(__floatunssisfvfp, u_tof),
    F(__floatsidfvfp, i_tod), F(__floatdidf, ll_tod), F(__floatdisf, ll_tof),
    F(__divsi3, i_div), F(__modsi3, i_mod), F(__udivsi3, u_div), F(__umodsi3, u_mod),
    F(__divdi3, ll_div), F(__moddi3, ll_mod), F(__muldi3, ll_mul), F(__udivdi3, ull_div),
    F(__umoddi3, ull_mod), F(__cmpdi2, ll_cmp),

    // math softfp
    F(acosf, m_acosf), F(cosf, m_cosf), F(sinf, m_sinf), F(tanf, m_tanf), F(expf, m_expf),
    F(logf, m_logf), F(log10f, m_log10f), F(floorf, m_floorf), F(lrintf, m_lrintf), F(cos, m_cos),
    F(sin, m_sin), F(floor, m_floor), F(log10, m_log10), F(atan2, m_atan2), F(fmod, m_fmod),
    F(pow, m_pow), F(ldexp, m_ldexp),
    F(asinf, m_asinf), F(atanf, m_atanf), F(ceilf, m_ceilf), F(roundf, m_roundf),
    F(acos, m_acos), F(asin, m_asin), F(atan, m_atan), F(ceil, m_ceil),
    F(tan, m_tan), F(exp, m_exp), F(log, m_log), F(log2, m_log2),
    F(fmax, m_fmax), F(fmin, m_fmin),
};
const int darwin_imports_count = sizeof(darwin_imports) / sizeof(darwin_imports[0]);

void darwin_init(void) {
    d_stdoutp = stdout;
    d_stderrp = stderr;
    d_stdinp = stdin;
    init_rune_locale();
}
