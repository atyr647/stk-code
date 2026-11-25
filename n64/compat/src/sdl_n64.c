/* SDL2 on libdragon: the part of the SDL2 API that STK, Irrlicht's SDL device
 * (lib/irrlicht/source/Irrlicht/CIrrDeviceSDL.cpp) and MojoAL use.
 *
 * Everything is compiled against SDL2's own headers (n64/build_n64_client_deps.sh
 * installs them); this file is the implementation:
 *
 *   window      the TV picture, 320x240 (16-bit, two framebuffers)
 *   OpenGL      libdragon's OpenGL 1.x on the RDP; Irrlicht's fixed-function
 *               driver draws with it (STK's legacy renderer)
 *   joysticks   the four controller ports (see "Controllers" below)
 *   audio       not yet: opening a device fails cleanly, so MojoAL reports no
 *               device and STK runs without sound
 *   the rest    the N64 has no mouse, touch, sensors, clipboard, haptics,
 *               Vulkan or second display: the calls report that
 *
 * The frame protocol: a frame is always open between swaps (rdpq attached to
 * the next framebuffer with the Z buffer, GL context begun), so GL can be
 * called at any time, including texture uploads while loading.
 */
/* debugf() is compiled out under NDEBUG; this file's messages are wanted */
#undef NDEBUG
#include <libdragon.h>
#include <GL/gl.h>
#include <GL/gl_integration.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "SDL_vulkan.h"

/* ---------------------------------------------------------------- errors -- */
static char g_error[128] = "";

static int set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
    return -1;
}

const char *SDL_GetError(void) { return g_error; }

/* ------------------------------------------------------------ init, misc -- */
static int g_joypad_up;

int SDL_InitSubSystem(Uint32 flags)
{
    if ((flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) && !g_joypad_up)
    {
        joypad_init();
        g_joypad_up = 1;
    }
    return 0;
}

void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
void SDL_Quit(void) {}

SDL_bool SDL_SetHint(const char *name, const char *value)
{
    (void)name; (void)value;
    return SDL_TRUE;
}

void SDL_GetVersion(SDL_version *ver)
{
    SDL_VERSION(ver);
}

void SDL_Delay(Uint32 ms)
{
    kthread_sleep(TICKS_FROM_MS(ms));
}

int SDL_OpenURL(const char *url)
{
    (void)url;
    return set_error("no web browser on the N64");
}

/* ------------------------------------------------- libc and math aliases -- */
void *SDL_malloc(size_t size) { return malloc(size); }
void *SDL_calloc(size_t n, size_t size) { return calloc(n, size); }
void *SDL_realloc(void *p, size_t size) { return realloc(p, size); }
void SDL_free(void *p) { free(p); }
void *SDL_memset(void *d, int c, size_t n) { return memset(d, c, n); }
void *SDL_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *SDL_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
size_t SDL_strlen(const char *s) { return strlen(s); }
int SDL_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
char *SDL_strdup(const char *s) { return strdup(s); }
size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
    const size_t n = strlen(src);
    if (maxlen)
    {
        const size_t c = n < maxlen - 1 ? n : maxlen - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
double SDL_sqrt(double x) { return sqrt(x); }
float SDL_sqrtf(float x) { return sqrtf(x); }
double SDL_sin(double x) { return sin(x); }
float SDL_sinf(float x) { return sinf(x); }
double SDL_cos(double x) { return cos(x); }
float SDL_cosf(float x) { return cosf(x); }
float SDL_acosf(float x) { return acosf(x); }
double SDL_atan2(double y, double x) { return atan2(y, x); }
float SDL_powf(float x, float y) { return powf(x, y); }
double SDL_log(double x) { return log(x); }

/* ------------------------------------------------------- mutex, atomics -- */
SDL_mutex *SDL_CreateMutex(void)
{
    kmutex_t *m = malloc(sizeof(kmutex_t));
    if (!m)
    {
        set_error("out of memory");
        return NULL;
    }
    kmutex_init(m, KMUTEX_RECURSIVE);   /* SDL mutexes are recursive */
    return (SDL_mutex*)m;
}

int SDL_LockMutex(SDL_mutex *m)
{
    if (m) kmutex_lock((kmutex_t*)m);
    return 0;
}

int SDL_UnlockMutex(SDL_mutex *m)
{
    if (m) kmutex_unlock((kmutex_t*)m);
    return 0;
}

void SDL_DestroyMutex(SDL_mutex *m)
{
    if (m)
    {
        kmutex_destroy((kmutex_t*)m);
        free(m);
    }
}

/* one CPU: an atomic operation is one with interrupts (and so thread
 * switches) held off */
SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval)
{
    disable_interrupts();
    const SDL_bool ok = a->value == oldval;
    if (ok) a->value = newval;
    enable_interrupts();
    return ok;
}

int SDL_AtomicSet(SDL_atomic_t *a, int v)
{
    disable_interrupts();
    const int old = a->value;
    a->value = v;
    enable_interrupts();
    return old;
}

int SDL_AtomicGet(SDL_atomic_t *a)
{
    return *(volatile int*)&a->value;
}

int SDL_AtomicAdd(SDL_atomic_t *a, int v)
{
    disable_interrupts();
    const int old = a->value;
    a->value += v;
    enable_interrupts();
    return old;
}

SDL_bool SDL_AtomicCASPtr(void **a, void *oldval, void *newval)
{
    disable_interrupts();
    const SDL_bool ok = *a == oldval;
    if (ok) *a = newval;
    enable_interrupts();
    return ok;
}

void *SDL_AtomicSetPtr(void **a, void *v)
{
    disable_interrupts();
    void *old = *a;
    *a = v;
    enable_interrupts();
    return old;
}

void *SDL_AtomicGetPtr(void **a)
{
    return *(void * volatile *)a;
}

/* --------------------------------------------------------------- window -- */
#define SCREEN_W 320
#define SCREEN_H 240

struct SDL_Window
{
    int w, h;
};

static struct SDL_Window g_window = { SCREEN_W, SCREEN_H };
static surface_t g_zbuffer;
static int g_gl_up;

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h,
                             Uint32 flags)
{
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    return &g_window;      /* the TV: one fixed mode */
}

void SDL_DestroyWindow(SDL_Window *win) { (void)win; }
void SDL_SetWindowTitle(SDL_Window *win, const char *t) { (void)win; (void)t; }
void SDL_SetWindowSize(SDL_Window *win, int w, int h) { (void)win; (void)w; (void)h; }
void SDL_SetWindowPosition(SDL_Window *win, int x, int y) { (void)win; (void)x; (void)y; }
void SDL_SetWindowMinimumSize(SDL_Window *win, int w, int h) { (void)win; (void)w; (void)h; }
void SDL_SetWindowResizable(SDL_Window *win, SDL_bool r) { (void)win; (void)r; }
void SDL_RaiseWindow(SDL_Window *win) { (void)win; }
int SDL_SetWindowFullscreen(SDL_Window *win, Uint32 f) { (void)win; (void)f; return 0; }
int SDL_GetWindowDisplayIndex(SDL_Window *win) { (void)win; return 0; }

void SDL_GetWindowSize(SDL_Window *win, int *w, int *h)
{
    (void)win;
    if (w) *w = SCREEN_W;
    if (h) *h = SCREEN_H;
}

void SDL_GetWindowPosition(SDL_Window *win, int *x, int *y)
{
    (void)win;
    if (x) *x = 0;
    if (y) *y = 0;
}

Uint32 SDL_GetWindowPixelFormat(SDL_Window *win)
{
    (void)win;
    return SDL_PIXELFORMAT_RGBA5551;
}

int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
int SDL_WarpMouseGlobal(int x, int y) { (void)x; (void)y; return 0; }

/* ------------------------------------------------------------- displays -- */
int SDL_GetNumVideoDisplays(void) { return 1; }
int SDL_GetNumDisplayModes(int display) { (void)display; return 1; }

static int fill_mode(SDL_DisplayMode *mode)
{
    if (!mode) return set_error("no mode");
    mode->format = SDL_PIXELFORMAT_RGBA5551;
    mode->w = SCREEN_W;
    mode->h = SCREEN_H;
    mode->refresh_rate = 60;
    mode->driverdata = NULL;
    return 0;
}

int SDL_GetDisplayMode(int d, int i, SDL_DisplayMode *m) { (void)d; (void)i; return fill_mode(m); }
int SDL_GetCurrentDisplayMode(int d, SDL_DisplayMode *m) { (void)d; return fill_mode(m); }
int SDL_GetDesktopDisplayMode(int d, SDL_DisplayMode *m) { (void)d; return fill_mode(m); }

int SDL_GetDisplayDPI(int display, float *ddpi, float *hdpi, float *vdpi)
{
    (void)display;
    /* a 4:3 TV of ~14": 320 pixels across ~11 inches */
    if (ddpi) *ddpi = 30.0f;
    if (hdpi) *hdpi = 30.0f;
    if (vdpi) *vdpi = 30.0f;
    return 0;
}

SDL_DisplayOrientation SDL_GetDisplayOrientation(int display)
{
    (void)display;
    return SDL_ORIENTATION_LANDSCAPE;
}

/* --------------------------------------------------------------- OpenGL -- */
static void frame_begin(void)
{
    rdpq_attach(display_get(), &g_zbuffer);
    gl_context_begin();
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *win)
{
    (void)win;
    if (!g_gl_up)
    {
        display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE,
                     FILTERS_RESAMPLE_ANTIALIAS);
        g_zbuffer = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
        rdpq_init();
        gl_init();
        g_gl_up = 1;
        frame_begin();
    }
    return (SDL_GLContext)&g_window;
}

void SDL_GL_DeleteContext(SDL_GLContext ctx) { (void)ctx; }
int SDL_GL_SetAttribute(SDL_GLattr attr, int value) { (void)attr; (void)value; return 0; }
int SDL_GL_SetSwapInterval(int interval) { (void)interval; return 0; }

void SDL_GL_GetDrawableSize(SDL_Window *win, int *w, int *h)
{
    SDL_GetWindowSize(win, w, h);
}

void SDL_GL_SwapWindow(SDL_Window *win)
{
    (void)win;
    if (!g_gl_up)
        return;
    gl_context_end();
    rdpq_detach_show();
    frame_begin();
}

static const struct { const char *name; void *fn; } g_gl_procs[] =
{
#define GL_PROC(f) { #f, (void*)f },
#include "sdl_n64_gl_procs.h"
#undef GL_PROC
};

/* GL 1.x functions libdragon does not implement: a no-op that says so once.
 * Every caller ignores or tolerates the result of these on this ABI: the
 * arguments are in registers (or the caller's frame) and a returning one
 * reads 0. */
#define GL_UNSUPPORTED_PROC(f) \
    static int stub_##f(void) \
    { \
        static int said; \
        if (!said++) debugf("GL: %s is not supported, ignored\n", #f); \
        return 0; \
    }
#include "sdl_n64_gl_unsupported.h"
#undef GL_UNSUPPORTED_PROC

static const struct { const char *name; void *fn; } g_gl_unsupported[] =
{
#define GL_UNSUPPORTED_PROC(f) { #f, (void*)stub_##f },
#include "sdl_n64_gl_unsupported.h"
#undef GL_UNSUPPORTED_PROC
};

/* ------------------------------------------------------ GL textures --
 * Irrlicht's driver and STK's graphics_engine upload textures the desktop
 * way (32-bit BGRA, BGR, 16-bit packed, single-channel GL_RED for font
 * pages) and update them with glTexSubImage2D. libdragon's GL takes RGBA
 * only, has no glTexSubImage2D, and the RDP samples from TMEM: 4 KB.
 *
 * So glTexImage2D/glTexSubImage2D/glBindTexture/glDeleteTextures are
 * wrapped: each texture's image becomes a libdragon surface owned here,
 *   - intensity+alpha (IA8: 64x64 fits) when it is a single-channel mask,
 *   - RGBA 5551 (RGBA16: 2048 texels fit) otherwise,
 * resampled to power-of-two sides (the RDP wraps only those) that fit TMEM
 * (sub-image updates map through the same ratio), and is given to libdragon
 * with glSurfaceTexImageN64, again after each update. Mip levels above 0
 * are ignored. libdragon's surfaces are uncached: no cache maintenance. */
#define TMEM_BYTES 4096
/* desktop GL enums libdragon's headers do not define (standard values) */
#ifndef GL_BGR
#define GL_BGR                          0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA                         0x80E1
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5         0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV   0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV     0x8367
#endif

typedef struct
{
    GLuint    name;
    surface_t surf;
    uint16_t  src_w, src_h;       /* the image as the program sees it */
    uint8_t   used;
} n64_tex_t;

static n64_tex_t *g_tex;
static unsigned   g_tex_count, g_tex_cap;
static GLuint     g_bound_2d;

static n64_tex_t *tex_find(GLuint name, int create)
{
    for (unsigned i = 0; i < g_tex_count; i++)
        if (g_tex[i].used && g_tex[i].name == name)
            return &g_tex[i];
    if (!create || !name)
        return NULL;
    for (unsigned i = 0; i < g_tex_count; i++)
        if (!g_tex[i].used)
        {
            memset(&g_tex[i], 0, sizeof(g_tex[i]));
            g_tex[i].name = name;
            g_tex[i].used = 1;
            return &g_tex[i];
        }
    if (g_tex_count == g_tex_cap)
    {
        unsigned cap = g_tex_cap ? g_tex_cap * 2 : 64;
        n64_tex_t *t = realloc(g_tex, cap * sizeof(*t));
        if (!t) return NULL;
        g_tex = t;
        g_tex_cap = cap;
    }
    n64_tex_t *t = &g_tex[g_tex_count++];
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->used = 1;
    return t;
}

static void tex_release(n64_tex_t *t)
{
    if (t->surf.buffer)
        surface_free(&t->surf);
    memset(&t->surf, 0, sizeof(t->surf));
}

/* one source pixel as 8-bit RGBA; returns 0 for an unknown format */
static int read_pixel(GLenum format, GLenum type, const uint8_t *row, int x,
                      uint8_t *rgba)
{
    switch (format)
    {
    case GL_RGBA:
        if (type == GL_UNSIGNED_BYTE)
        {
            memcpy(rgba, row + 4 * x, 4);
            return 1;
        }
        break;
    case GL_BGRA:
        if (type == GL_UNSIGNED_BYTE)
        {
            const uint8_t *p = row + 4 * x;
            rgba[0] = p[2]; rgba[1] = p[1]; rgba[2] = p[0]; rgba[3] = p[3];
            return 1;
        }
        if (type == GL_UNSIGNED_INT_8_8_8_8_REV)
        {
            uint32_t v;               /* native order: 0xAARRGGBB */
            memcpy(&v, row + 4 * x, 4);
            rgba[0] = v >> 16; rgba[1] = v >> 8; rgba[2] = v; rgba[3] = v >> 24;
            return 1;
        }
        if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV)
        {
            uint16_t v;               /* A1R5G5B5 */
            memcpy(&v, row + 2 * x, 2);
            rgba[0] = ((v >> 10) & 31) * 255 / 31;
            rgba[1] = ((v >> 5) & 31) * 255 / 31;
            rgba[2] = (v & 31) * 255 / 31;
            rgba[3] = (v & 0x8000) ? 255 : 0;
            return 1;
        }
        break;
    case GL_RGB:
        if (type == GL_UNSIGNED_BYTE)
        {
            memcpy(rgba, row + 3 * x, 3);
            rgba[3] = 255;
            return 1;
        }
        if (type == GL_UNSIGNED_SHORT_5_6_5)
        {
            uint16_t v;
            memcpy(&v, row + 2 * x, 2);
            rgba[0] = (v >> 11) * 255 / 31;
            rgba[1] = ((v >> 5) & 63) * 255 / 63;
            rgba[2] = (v & 31) * 255 / 31;
            rgba[3] = 255;
            return 1;
        }
        break;
    case GL_BGR:
        if (type == GL_UNSIGNED_BYTE)
        {
            const uint8_t *p = row + 3 * x;
            rgba[0] = p[2]; rgba[1] = p[1]; rgba[2] = p[0]; rgba[3] = 255;
            return 1;
        }
        break;
    case GL_RED:              /* font pages: a coverage mask */
    case GL_ALPHA:
        if (type == GL_UNSIGNED_BYTE)
        {
            rgba[0] = rgba[1] = rgba[2] = 255;
            rgba[3] = row[x];
            return 1;
        }
        break;
    case GL_LUMINANCE:
        if (type == GL_UNSIGNED_BYTE)
        {
            rgba[0] = rgba[1] = rgba[2] = row[x];
            rgba[3] = 255;
            return 1;
        }
        break;
    }
    return 0;
}

static int bytes_per_pixel(GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV) return 4;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_1_5_5_5_REV) return 2;
    switch (format)
    {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    default: return 1;
    }
}

/* write source rectangle (x, y, w, h) of data into t->surf: each surface
 * texel the rectangle covers takes the source texel it maps to (nearest) */
static void tex_store(n64_tex_t *t, int x, int y, int w, int h,
                      GLenum format, GLenum type, const void *data)
{
    const tex_format_t fmt = surface_get_format(&t->surf);
    const int bpp = bytes_per_pixel(format, type);
    const int dw = t->surf.width, dh = t->surf.height;
    const int sw = t->src_w, sh = t->src_h;
    uint8_t *dst = t->surf.buffer;
    const int dx0 = x * dw / sw, dy0 = y * dh / sh;
    const int dx1 = ((x + w) * dw + sw - 1) / sw;
    const int dy1 = ((y + h) * dh + sh - 1) / sh;
    for (int dy = dy0; dy < dy1 && dy < dh; dy++)
    {
        int syy = (dy * sh + sh / (2 * dh)) / dh - y;   /* texel centre */
        if (syy < 0) syy = 0;
        if (syy >= h) syy = h - 1;
        const uint8_t *row = (const uint8_t*)data + (size_t)syy * w * bpp;
        for (int dx = dx0; dx < dx1 && dx < dw; dx++)
        {
            int sxx = (dx * sw + sw / (2 * dw)) / dw - x;
            if (sxx < 0) sxx = 0;
            if (sxx >= w) sxx = w - 1;
            uint8_t c[4] = { 0, 0, 0, 0 };
            if (data)
                read_pixel(format, type, row, sxx, c);
            if (fmt == FMT_IA8)
            {
                const uint8_t i = (c[0] + c[1] + c[2]) / 3;
                dst[dy * t->surf.stride + dx] = (i & 0xF0) | (c[3] >> 4);
            }
            else
            {
                const uint16_t v = ((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) |
                                   ((c[2] >> 3) << 1) | (c[3] >= 128);
                ((uint16_t*)(dst + dy * t->surf.stride))[dx] = v;
            }
        }
    }
}

/* the power of two nearest to n (in log terms) */
static int nearest_pot(int n)
{
    int p = 1;
    while (p * 2 <= n)
        p *= 2;
    return (n - p) * 4 >= p * 3 ? p * 2 : p;    /* >= 1.75p: round up */
}

static void n64_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                             GLsizei width, GLsizei height, GLint border,
                             GLenum format, GLenum type, const GLvoid *data)
{
    (void)internalformat; (void)border;
    if (target != GL_TEXTURE_2D || level != 0 || width <= 0 || height <= 0)
        return;                         /* mip levels: not used */
    n64_tex_t *t = tex_find(g_bound_2d, 1);
    if (!t)
        return;
    uint8_t probe[4];
    if (!read_pixel(format, type, (const uint8_t*)"\0\0\0\0", 0, probe))
    {
        static int said;
        if (!said++)
            debugf("GL: texture format %#lx/%#lx not converted\n",
                   (unsigned long)format, (unsigned long)type);
        return;
    }
    const int mask = format == GL_RED || format == GL_ALPHA;
    const tex_format_t fmt = mask ? FMT_IA8 : FMT_RGBA16;
    const int bytes = mask ? 1 : 2;
    int dw = nearest_pot(width), dh = nearest_pot(height);
    while (dw * dh * bytes > TMEM_BYTES)
    {
        if (dw >= dh) dw /= 2;
        else dh /= 2;
    }
    tex_release(t);
    t->surf = surface_alloc(fmt, dw, dh);
    if (!t->surf.buffer)
        return;
    t->src_w = width;
    t->src_h = height;
    memset(t->surf.buffer, 0, t->surf.stride * t->surf.height);
    if (data)
        tex_store(t, 0, 0, width, height, format, type, data);
    glSurfaceTexImageN64(GL_TEXTURE_2D, 0, &t->surf, NULL);
}

static void n64_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                                GLsizei w, GLsizei h, GLenum format,
                                GLenum type, const GLvoid *data)
{
    if (target != GL_TEXTURE_2D || level != 0 || !data || w <= 0 || h <= 0)
        return;
    n64_tex_t *t = tex_find(g_bound_2d, 0);
    if (!t || !t->surf.buffer)
        return;
    tex_store(t, x, y, w, h, format, type, data);
    glSurfaceTexImageN64(GL_TEXTURE_2D, 0, &t->surf, NULL);   /* re-upload */
}

static void n64_glBindTexture(GLenum target, GLuint name)
{
    if (target == GL_TEXTURE_2D)
        g_bound_2d = name;
    glBindTexture(target, name);
}

static void n64_glDeleteTextures(GLsizei n, const GLuint *names)
{
    glDeleteTextures(n, names);         /* libdragon drops its references */
    for (GLsizei i = 0; i < n; i++)
    {
        n64_tex_t *t = tex_find(names[i], 0);
        if (t)
        {
            tex_release(t);
            t->used = 0;
        }
        if (names[i] == g_bound_2d)
            g_bound_2d = 0;
    }
}

static void n64_glGenerateMipmap(GLenum target) { (void)target; }

static const struct { const char *name; void *fn; } g_gl_wrapped[] =
{
    { "glTexImage2D", (void*)n64_glTexImage2D },
    { "glTexSubImage2D", (void*)n64_glTexSubImage2D },
    { "glBindTexture", (void*)n64_glBindTexture },
    { "glDeleteTextures", (void*)n64_glDeleteTextures },
    { "glGenerateMipmap", (void*)n64_glGenerateMipmap },
    { "glGenerateMipmapEXT", (void*)n64_glGenerateMipmap },
};

void *SDL_GL_GetProcAddress(const char *proc)
{
    for (size_t i = 0; i < sizeof(g_gl_wrapped) / sizeof(g_gl_wrapped[0]); i++)
        if (strcmp(g_gl_wrapped[i].name, proc) == 0)
            return g_gl_wrapped[i].fn;
    for (size_t i = 0; i < sizeof(g_gl_procs) / sizeof(g_gl_procs[0]); i++)
        if (strcmp(g_gl_procs[i].name, proc) == 0)
            return g_gl_procs[i].fn;
    for (size_t i = 0; i < sizeof(g_gl_unsupported) / sizeof(g_gl_unsupported[0]); i++)
        if (strcmp(g_gl_unsupported[i].name, proc) == 0)
            return g_gl_unsupported[i].fn;
    return NULL;
}

/* --------------------------------------------------------------- Vulkan -- */
SDL_bool SDL_Vulkan_GetInstanceExtensions(SDL_Window *w, unsigned *n, const char **names)
{
    (void)w; (void)names;
    if (n) *n = 0;
    set_error("no Vulkan on the N64");
    return SDL_FALSE;
}

SDL_bool SDL_Vulkan_CreateSurface(SDL_Window *w, VkInstance inst, VkSurfaceKHR *s)
{
    (void)w; (void)inst; (void)s;
    set_error("no Vulkan on the N64");
    return SDL_FALSE;
}

void *SDL_Vulkan_GetVkGetInstanceProcAddr(void) { return NULL; }

void SDL_Vulkan_GetDrawableSize(SDL_Window *w, int *wd, int *ht)
{
    SDL_GetWindowSize(w, wd, ht);
}

/* ----------------------------------------------------------- Controllers --
 * Each connected port is a joystick, instance id = port number:
 *   axes    0 stick X, 1 stick Y (SDL's sign: down is positive)
 *   hat     0 the D-pad
 *   buttons 0 A, 1 B, 2 Z, 3 Start, 4 L, 5 R, 6 C-up, 7 C-down, 8 C-left,
 *           9 C-right
 * and a game controller mapping names them so STK's defaults
 * (GamepadConfig::initSDLMapping, console layout) come out as: A accelerate,
 * B brake, Z fire, R drift, C-right nitro, C-down look back, C-up rescue,
 * Start pause; stick or D-pad steer; A/B select and cancel in menus. */
#define N64_BUTTONS 10

struct _SDL_Joystick
{
    int port;
    int open;
};

struct _SDL_GameController
{
    SDL_Joystick *joystick;
};

static struct _SDL_Joystick g_joy[JOYPAD_PORT_COUNT];
static struct _SDL_GameController g_ctrl[JOYPAD_PORT_COUNT];

static const char N64_MAPPING[] =
    "00000000000000000000000000000000,N64 Controller,"
    "a:b0,b:b1,x:b2,start:b3,misc1:b4,rightshoulder:b5,leftstick:b6,"
    "y:b7,back:b8,leftshoulder:b9,"
    "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,";

/* device index (0..connected-1) <-> port */
static int port_of_index(int index)
{
    for (int p = 0; p < JOYPAD_PORT_COUNT; p++)
        if (g_joypad_up && joypad_is_connected(p) && index-- == 0)
            return p;
    return -1;
}

int SDL_NumJoysticks(void)
{
    int n = 0;
    for (int p = 0; p < JOYPAD_PORT_COUNT; p++)
        if (g_joypad_up && joypad_is_connected(p))
            n++;
    return n;
}

SDL_bool SDL_IsGameController(int index)
{
    return port_of_index(index) >= 0 ? SDL_TRUE : SDL_FALSE;
}

SDL_Joystick *SDL_JoystickOpen(int index)
{
    const int p = port_of_index(index);
    if (p < 0)
    {
        set_error("no controller %d", index);
        return NULL;
    }
    g_joy[p].port = p;
    g_joy[p].open = 1;
    return &g_joy[p];
}

void SDL_JoystickClose(SDL_Joystick *j) { if (j) j->open = 0; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { return j ? j->port : -1; }
const char *SDL_JoystickName(SDL_Joystick *j) { (void)j; return "N64 Controller"; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 2; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return N64_BUTTONS; }
int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 1; }

SDL_JoystickPowerLevel SDL_JoystickCurrentPowerLevel(SDL_Joystick *j)
{
    (void)j;
    return SDL_JOYSTICK_POWER_WIRED;
}

static Sint16 axis_value(int8_t v, int invert)
{
    /* an N64 stick reaches about +-80 */
    int s = (int)v * 32767 / 80;
    if (invert) s = -s;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (Sint16)s;
}

Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis)
{
    if (!j || !g_joypad_up) return 0;
    const joypad_inputs_t in = joypad_get_inputs(j->port);
    return axis == 0 ? axis_value(in.stick_x, 0) :
           axis == 1 ? axis_value(in.stick_y, 1) : 0;
}

SDL_GameController *SDL_GameControllerOpen(int index)
{
    SDL_Joystick *j = SDL_JoystickOpen(index);
    if (!j) return NULL;
    g_ctrl[j->port].joystick = j;
    return &g_ctrl[j->port];
}

void SDL_GameControllerClose(SDL_GameController *c) { if (c) SDL_JoystickClose(c->joystick); }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { return c ? c->joystick : NULL; }
const char *SDL_GameControllerName(SDL_GameController *c) { (void)c; return "N64 Controller"; }
char *SDL_GameControllerMapping(SDL_GameController *c) { (void)c; return strdup(N64_MAPPING); }

/* rumble: the Rumble Pak is on or off; a request runs until its end tick */
static uint32_t g_rumble_until[JOYPAD_PORT_COUNT];
static uint8_t g_rumbling[JOYPAD_PORT_COUNT];

int SDL_GameControllerRumble(SDL_GameController *c, Uint16 low, Uint16 high,
                             Uint32 duration_ms)
{
    if (!c || !c->joystick) return set_error("no controller");
    const int p = c->joystick->port;
    if (!joypad_get_rumble_supported(p)) return set_error("no Rumble Pak");
    const int on = (low | high) != 0 && duration_ms > 0;
    joypad_set_rumble_active(p, on);
    g_rumbling[p] = on;
    g_rumble_until[p] = TICKS_READ() + TICKS_FROM_MS(duration_ms);
    return 0;
}

/* SDL's haptic API: STK uses it only where the game controller API is
 * missing, which it never is here */
SDL_Haptic *SDL_HapticOpenFromJoystick(SDL_Joystick *j) { (void)j; set_error("use SDL_GameControllerRumble"); return NULL; }
void SDL_HapticClose(SDL_Haptic *h) { (void)h; }
int SDL_HapticRumbleInit(SDL_Haptic *h) { (void)h; return -1; }
int SDL_HapticRumblePlay(SDL_Haptic *h, float s, Uint32 l) { (void)h; (void)s; (void)l; return -1; }
int SDL_HapticSetAutocenter(SDL_Haptic *h, int a) { (void)h; (void)a; return -1; }

/* ---------------------------------------------------------------- events -- */
#define QUEUE 64
static SDL_Event g_queue[QUEUE];
static unsigned g_head, g_tail;
static int g_polled;

static void push(const SDL_Event *e)
{
    if (g_tail - g_head < QUEUE)
        g_queue[g_tail++ % QUEUE] = *e;
}

static void poll_controllers(void)
{
    static uint8_t connected[JOYPAD_PORT_COUNT];
    static joypad_inputs_t last[JOYPAD_PORT_COUNT];
    if (!g_joypad_up)
        return;
    joypad_poll();
    const uint32_t now = TICKS_READ();
    int index = 0;
    for (int p = 0; p < JOYPAD_PORT_COUNT; p++)
    {
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        const int is = joypad_is_connected(p);
        if (g_rumbling[p] && (int32_t)(now - g_rumble_until[p]) >= 0)
        {
            joypad_set_rumble_active(p, false);
            g_rumbling[p] = 0;
        }
        if (is && !connected[p])
        {
            e.type = SDL_JOYDEVICEADDED;
            e.jdevice.which = index;
            push(&e);
            memset(&last[p], 0, sizeof(last[p]));
        }
        else if (!is && connected[p])
        {
            e.type = SDL_JOYDEVICEREMOVED;
            e.jdevice.which = p;
            push(&e);
        }
        connected[p] = is;
        if (!is)
            continue;
        index++;
        const joypad_inputs_t in = joypad_get_inputs(p);
        const joypad_inputs_t old = last[p];
        last[p] = in;
        const unsigned now_b[N64_BUTTONS] = { in.btn.a, in.btn.b, in.btn.z,
            in.btn.start, in.btn.l, in.btn.r, in.btn.c_up, in.btn.c_down,
            in.btn.c_left, in.btn.c_right };
        const unsigned old_b[N64_BUTTONS] = { old.btn.a, old.btn.b, old.btn.z,
            old.btn.start, old.btn.l, old.btn.r, old.btn.c_up, old.btn.c_down,
            old.btn.c_left, old.btn.c_right };
        for (int b = 0; b < N64_BUTTONS; b++)
        {
            if (now_b[b] == old_b[b]) continue;
            memset(&e, 0, sizeof(e));
            e.type = now_b[b] ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
            e.jbutton.which = p;
            e.jbutton.button = b;
            e.jbutton.state = now_b[b] ? SDL_PRESSED : SDL_RELEASED;
            push(&e);
        }
        const int8_t axes[2] = { in.stick_x, in.stick_y };
        const int8_t old_axes[2] = { old.stick_x, old.stick_y };
        for (int a = 0; a < 2; a++)
        {
            if (axes[a] == old_axes[a]) continue;
            memset(&e, 0, sizeof(e));
            e.type = SDL_JOYAXISMOTION;
            e.jaxis.which = p;
            e.jaxis.axis = a;
            e.jaxis.value = axis_value(axes[a], a == 1);
            push(&e);
        }
        const unsigned hat = (in.btn.d_up ? SDL_HAT_UP : 0) |
            (in.btn.d_right ? SDL_HAT_RIGHT : 0) |
            (in.btn.d_down ? SDL_HAT_DOWN : 0) |
            (in.btn.d_left ? SDL_HAT_LEFT : 0);
        const unsigned old_hat = (old.btn.d_up ? SDL_HAT_UP : 0) |
            (old.btn.d_right ? SDL_HAT_RIGHT : 0) |
            (old.btn.d_down ? SDL_HAT_DOWN : 0) |
            (old.btn.d_left ? SDL_HAT_LEFT : 0);
        if (hat != old_hat)
        {
            memset(&e, 0, sizeof(e));
            e.type = SDL_JOYHATMOTION;
            e.jhat.which = p;
            e.jhat.hat = 0;
            e.jhat.value = hat;
            push(&e);
        }
    }
}

/* Irrlicht drains the queue once per frame: the controllers are read once
 * per drain */
int SDL_PollEvent(SDL_Event *event)
{
    if (g_head == g_tail && !g_polled)
    {
        poll_controllers();
        g_polled = 1;
    }
    if (g_head == g_tail)
    {
        g_polled = 0;
        return 0;
    }
    if (event)
        *event = g_queue[g_head % QUEUE];
    g_head++;
    return 1;
}

/* ------------------------------------- text input, touch, sensors, clipboard */
void SDL_StartTextInput(void) {}
void SDL_StopTextInput(void) {}
SDL_bool SDL_IsTextInputActive(void) { return SDL_FALSE; }
void SDL_SetTextInputRect(const SDL_Rect *r) { (void)r; }
SDL_bool SDL_HasScreenKeyboardSupport(void) { return SDL_FALSE; }
int SDL_GetNumTouchDevices(void) { return 0; }
int SDL_NumSensors(void) { return 0; }
SDL_Sensor *SDL_SensorOpen(int i) { (void)i; set_error("no sensors"); return NULL; }
void SDL_SensorClose(SDL_Sensor *s) { (void)s; }
SDL_SensorID SDL_SensorGetInstanceID(SDL_Sensor *s) { (void)s; return -1; }
SDL_SensorType SDL_SensorGetDeviceType(int i) { (void)i; return SDL_SENSOR_INVALID; }
SDL_Sensor *SDL_SensorFromInstanceID(SDL_SensorID id) { (void)id; return NULL; }
char *SDL_GetClipboardText(void) { return strdup(""); }
int SDL_SetClipboardText(const char *t) { (void)t; return 0; }

/* ----------------------------------------------------------------- audio --
 * Not implemented yet: opening a device fails, MojoAL reports no device and
 * STK runs without sound (as on a PC without audio). */
int SDL_GetNumAudioDevices(int iscapture) { (void)iscapture; return 0; }
const char *SDL_GetAudioDeviceName(int i, int c) { (void)i; (void)c; return NULL; }

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int changes)
{
    (void)device; (void)iscapture; (void)desired; (void)obtained; (void)changes;
    set_error("N64 audio output is not implemented yet");
    return 0;
}

SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID d) { (void)d; return SDL_AUDIO_STOPPED; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int p) { (void)d; (void)p; }
void SDL_LockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID d) { (void)d; }

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat sf, Uint8 sc, int sr,
                      SDL_AudioFormat df, Uint8 dc, int dr)
{
    (void)cvt; (void)sf; (void)sc; (void)sr; (void)df; (void)dc; (void)dr;
    return set_error("no audio conversion yet");
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt) { (void)cvt; return set_error("no audio conversion yet"); }

SDL_AudioStream *SDL_NewAudioStream(SDL_AudioFormat sf, Uint8 sc, int sr,
                                    SDL_AudioFormat df, Uint8 dc, int dr)
{
    (void)sf; (void)sc; (void)sr; (void)df; (void)dc; (void)dr;
    set_error("no audio streams yet");
    return NULL;
}

int SDL_AudioStreamPut(SDL_AudioStream *s, const void *b, int l) { (void)s; (void)b; (void)l; return -1; }
int SDL_AudioStreamGet(SDL_AudioStream *s, void *b, int l) { (void)s; (void)b; (void)l; return -1; }
int SDL_AudioStreamAvailable(SDL_AudioStream *s) { (void)s; return 0; }
void SDL_AudioStreamClear(SDL_AudioStream *s) { (void)s; }
void SDL_FreeAudioStream(SDL_AudioStream *s) { (void)s; }
