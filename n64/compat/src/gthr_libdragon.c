/* Runtime half of n64/compat/include/bits/gthr-default.h: thread creation
 * with a trampoline that runs thread-specific-storage destructors, the TSS
 * key table, and the clock conversion for timed waits. */
#include <bits/gthr-default.h>
#include <kernel.h>
#include <cop1.h>
#include <debug.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The opaque types in gthr-default.h must be libdragon's, bit for bit. */
_Static_assert(sizeof(__gthread_mutex_t) == sizeof(kmutex_t), "mutex size");
_Static_assert(_Alignof(__gthread_mutex_t) >= _Alignof(kmutex_t), "mutex align");
_Static_assert(sizeof(__gthread_cond_t) == sizeof(kcond_t), "cond size");
static void __attribute__((constructor)) check_mutex_initialisers(void)
{
    /* the static initialisers have to mean what kmutex_init() would set up */
    kmutex_t std_m, rec_m;
    kmutex_init(&std_m, KMUTEX_STANDARD);
    kmutex_init(&rec_m, KMUTEX_RECURSIVE);
    __gthread_mutex_t a = __GTHREAD_MUTEX_INIT;
    __gthread_recursive_mutex_t b = __GTHREAD_RECURSIVE_MUTEX_INIT;
    assertf(!memcmp(&a, &std_m, sizeof a), "__GTHREAD_MUTEX_INIT mismatch");
    assertf(!memcmp(&b, &rec_m, sizeof b),
            "__GTHREAD_RECURSIVE_MUTEX_INIT mismatch");
}
#define KM(m) ((kmutex_t *)(m))
#define KC(c) ((kcond_t *)(c))

volatile int __stk_n64_threads_active = 0;
static kmutex_t once_mutex = { .flags = KMUTEX_RECURSIVE };

void __stk_n64_mutex_init(__gthread_mutex_t *m, int recursive)
{ kmutex_init(KM(m), recursive ? KMUTEX_RECURSIVE : KMUTEX_STANDARD); }
void __stk_n64_mutex_lock(__gthread_mutex_t *m)
{ if (__stk_n64_threads_active) kmutex_lock(KM(m)); }
int __stk_n64_mutex_trylock(__gthread_mutex_t *m)
{
    if (!__stk_n64_threads_active) return 0;
    return kmutex_try_lock(KM(m), 0) ? 0 : EBUSY;
}
void __stk_n64_mutex_unlock(__gthread_mutex_t *m)
{ if (__stk_n64_threads_active) kmutex_unlock(KM(m)); }
void __stk_n64_cond_init(__gthread_cond_t *c) { kcond_init(KC(c)); }
void __stk_n64_cond_signal(__gthread_cond_t *c)
{ if (__stk_n64_threads_active) kcond_signal(KC(c)); }
void __stk_n64_cond_broadcast(__gthread_cond_t *c)
{ if (__stk_n64_threads_active) kcond_broadcast(KC(c)); }
void __stk_n64_cond_wait(__gthread_cond_t *c, __gthread_mutex_t *m)
{ kcond_wait(KC(c), KM(m)); }
uint32_t __stk_n64_ticks_until(const struct timespec *abs);
int __stk_n64_cond_timedwait(__gthread_cond_t *c, __gthread_mutex_t *m,
                             const struct timespec *abs)
{
    uint32_t ticks = __stk_n64_ticks_until(abs);
    if (ticks == 0) return ETIMEDOUT;
    return kcond_wait_timeout(KC(c), KM(m), ticks) ? 0 : ETIMEDOUT;
}
void __stk_n64_thread_yield(void)
{ if (__stk_n64_threads_active) kthread_yield(); }
int __stk_n64_once(__gthread_once_t *once, void (*func)(void))
{
    if (__stk_n64_threads_active) kmutex_lock(&once_mutex);
    if (!once->done) {
        func();
        once->done = 1;
    }
    if (__stk_n64_threads_active) kmutex_unlock(&once_mutex);
    return 0;
}

#ifndef STK_N64_THREAD_STACK
#define STK_N64_THREAD_STACK (64 * 1024)
#endif
#define MAX_KEYS 64

static void (*key_dtor[MAX_KEYS])(void *);
static unsigned char key_used[MAX_KEYS];
static __thread void *key_value[MAX_KEYS];

struct __stk_n64_thread {
    kmutex_t lock;
    kcond_t finished;
    int done, detached;
    void *(*func)(void *);
    void *args;
};

static struct __stk_n64_thread main_thread;
static __thread struct __stk_n64_thread *self;

static void run_key_dtors(void)
{
    /* POSIX semantics: repeat while destructors keep setting values */
    for (int round = 0; round < 4; round++) {
        int any = 0;
        for (int k = 0; k < MAX_KEYS; k++) {
            void *v = key_value[k];
            if (key_used[k] && key_dtor[k] && v) {
                key_value[k] = NULL;
                key_dtor[k](v);
                any = 1;
            }
        }
        if (!any) break;
    }
}

/* The FPU mode STK expects, as on desktop CPUs: IEEE results (inf, NaN)
 * instead of traps. libdragon's debug builds trap on overflow, division by
 * zero and invalid operations (a zero-area triangle's normal divides by
 * zero; desktop code relies on getting NaN there). Denormals are flushed
 * (FS): the VR4300 has no hardware for them and would raise an unmaskable
 * exception. New kernel threads start with a zeroed FCR31, i.e. without
 * FS, so every thread sets this itself. */
void stk_n64_fpu_setup(void)
{
    uint32_t fcr31 = C1_FCR31();
    fcr31 &= ~(C1_ENABLE_MASK | C1_CAUSE_MASK);
    fcr31 |= C1_FCR31_FS;
    C1_WRITE_FCR31(fcr31);
}

static int trampoline(void *arg)
{
    struct __stk_n64_thread *t = arg;
    stk_n64_fpu_setup();
    /* Detach from the kernel's point of view straight away (we are certainly
     * still running, so this is always legal): the kernel frees the kthread
     * when it returns, and nobody ever calls kthread_join(). */
    kthread_detach(NULL);
    self = t;
    t->func(t->args);
    run_key_dtors();
    kmutex_lock(&t->lock);
    t->done = 1;
    if (t->detached) {
        kmutex_unlock(&t->lock);
        free(t);
        return 0;
    }
    kcond_broadcast(&t->finished);
    kmutex_unlock(&t->lock);
    return 0;
}

void stk_n64_note_thread(int stack_bytes);   /* n64_diag.cpp */

int __stk_n64_thread_create(__gthread_t *thread, void *(*func)(void *),
                            void *args)
{
    if (!__stk_n64_threads_active)
        return EAGAIN;
    struct __stk_n64_thread *t = calloc(1, sizeof(*t));
    if (!t) return EAGAIN;
    kmutex_init(&t->lock, KMUTEX_STANDARD);
    kcond_init(&t->finished);
    t->func = func; t->args = args;
    *thread = t;      /* before the thread can possibly run */
    stk_n64_note_thread(STK_N64_THREAD_STACK);
    if (!kthread_new("stk", STK_N64_THREAD_STACK, 0, trampoline, t)) {
        free(t);
        return EAGAIN;
    }
    return 0;
}

int __stk_n64_thread_join(__gthread_t t)
{
    if (!t || t == &main_thread) return EINVAL;
    kmutex_lock(&t->lock);
    while (!t->done)
        kcond_wait(&t->finished, &t->lock);
    kmutex_unlock(&t->lock);
    free(t);
    return 0;
}

int __stk_n64_thread_detach(__gthread_t t)
{
    if (!t || t == &main_thread) return EINVAL;
    kmutex_lock(&t->lock);
    if (t->done) {
        kmutex_unlock(&t->lock);
        free(t);
        return 0;
    }
    t->detached = 1;
    kmutex_unlock(&t->lock);
    return 0;
}

__gthread_t __stk_n64_thread_self(void)
{
    return self ? self : &main_thread;
}

int __stk_n64_key_create(__gthread_key_t *key, void (*dtor)(void *))
{
    for (int k = 0; k < MAX_KEYS; k++) {
        if (!key_used[k]) {
            key_used[k] = 1; key_dtor[k] = dtor; *key = k;
            return 0;
        }
    }
    return EAGAIN;
}

int __stk_n64_key_delete(__gthread_key_t key)
{
    if (key >= MAX_KEYS) return EINVAL;
    key_used[key] = 0; key_dtor[key] = NULL;
    return 0;
}

void *__stk_n64_key_get(__gthread_key_t key)
{ return key < MAX_KEYS ? key_value[key] : NULL; }

int __stk_n64_key_set(__gthread_key_t key, const void *ptr)
{
    if (key >= MAX_KEYS) return EINVAL;
    key_value[key] = (void *)ptr;
    return 0;
}

/* Ticks until an absolute CLOCK_REALTIME point (what libstdc++ passes),
 * clamped at 0 -- libdragon's own cthreads conversion goes negative, which
 * as an unsigned tick count is a near-infinite wait. */
uint32_t __stk_n64_ticks_until(const struct timespec *abs)
{
    /* same time base as std::chrono::system_clock (clock_libdragon.cpp) */
    extern int64_t __stk_n64_realtime_us(void);
    int64_t us = (int64_t)abs->tv_sec * 1000000 + abs->tv_nsec / 1000 -
                 __stk_n64_realtime_us();
    if (us <= 0) return 0;
    uint64_t t = TICKS_FROM_US((uint64_t)us);
    return t > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)t;
}

/* POSIX sleeps (libstdc++'s this_thread::sleep_for, STK's StkTime::sleep).
 * Sleeping through the kernel lets other threads run meanwhile. */
#include <unistd.h>
#include <timer.h>

int usleep(useconds_t us)
{
    if (__stk_n64_threads_active)
        kthread_sleep(TICKS_FROM_US((uint64_t)us));
    else
        wait_ticks(TICKS_FROM_US((uint64_t)us));
    return 0;
}

unsigned int sleep(unsigned int s)
{
    usleep((useconds_t)s * 1000000u);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    usleep((useconds_t)(req->tv_sec * 1000000 + req->tv_nsec / 1000));
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
