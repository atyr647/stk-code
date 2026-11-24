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

void *SDL_GL_GetProcAddress(const char *proc)
{
    for (size_t i = 0; i < sizeof(g_gl_procs) / sizeof(g_gl_procs[0]); i++)
        if (strcmp(g_gl_procs[i].name, proc) == 0)
            return g_gl_procs[i].fn;
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
