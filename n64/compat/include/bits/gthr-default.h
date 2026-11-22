/* libstdc++ thread model for the N64, on libdragon's multi-threading kernel.
 *
 * The mips64-elf toolchain (atyr647/Pak tools/build_n64_toolchain.sh, i.e.
 * libdragon's own configuration) builds libstdc++ with the "single" thread
 * model, so <thread>, <mutex> and <condition_variable> compile to almost
 * nothing. libstdc++ reaches its thread model only through this header
 * (<bits/gthr.h> includes <bits/gthr-default.h>), and n64/compat/include is
 * searched before the compiler's own directories, so providing it here -- plus
 * -D_GLIBCXX_HAS_GTHREADS -- turns the standard threading library back on for
 * every translation unit, without touching STK, libstdc++ or the toolchain.
 *
 * The handful of out-of-line pieces the prebuilt libstdc++.a lacks
 * (std::thread::_M_start_thread, join, condition_variable, call_once's TLS
 * hooks) are in n64/compat/src/gthr_libdragon.cpp.
 *
 * libdragon's kernel is cooperative plus interrupt-driven: threads switch when
 * one blocks, sleeps or yields, or an interrupt wakes a higher-priority one.
 * There is no time slicing, so a thread that spins without blocking starves
 * the others -- STK's worker threads all block on condition variables.
 */
#ifndef STK_N64_GTHR_DEFAULT_H
#define STK_N64_GTHR_DEFAULT_H

#define __GTHREADS 1
#define __GTHREADS_CXX0X 1
#define __GTHREAD_HAS_COND 1
#define _GTHREAD_USE_MUTEX_TIMEDLOCK 0

#include <kernel.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A thread handle is a small control block, not the kthread_t: libdragon's
 * kthread_join() parks the joiner without putting it on any wait list when the
 * target is still running (an assertion at this revision), so threads detach
 * themselves and joining is done with a mutex + condition variable instead.
 * See n64/compat/src/gthr_libdragon.c. */
typedef struct __stk_n64_thread *__gthread_t;
typedef unsigned int __gthread_key_t;
typedef struct { volatile int done; } __gthread_once_t;
typedef kmutex_t __gthread_mutex_t;
typedef kmutex_t __gthread_recursive_mutex_t;
typedef kcond_t __gthread_cond_t;
typedef struct timespec __gthread_time_t;

/* kmutex_t / kcond_t may be zero-initialised (kernel.h), which is what makes
 * std::mutex's constexpr constructor possible. Field order: original_pri,
 * flags, owner, counter, waiting. */
#define __GTHREAD_MUTEX_INIT { 0, KMUTEX_STANDARD, 0, 0, 0 }
#define __GTHREAD_MUTEX_INIT_FUNCTION __gthread_mutex_init_function
#define __GTHREAD_RECURSIVE_MUTEX_INIT { 0, KMUTEX_RECURSIVE, 0, 0, 0 }
#define __GTHREAD_RECURSIVE_MUTEX_INIT_FUNCTION __gthread_recursive_mutex_init_function
#define __GTHREAD_COND_INIT { 0 }
#define __GTHREAD_COND_INIT_FUNCTION __gthread_cond_init_function
#define __GTHREAD_ONCE_INIT { 0 }
#define __GTHREAD_TIME_INIT { 0, 0 }

/* Set by n64_main once kernel_init() has run. Before that the program is
 * single-threaded by construction (static constructors), so locking is a
 * no-op, exactly as with gthr-posix when libpthread is not linked. */
extern volatile int __stk_n64_threads_active;
extern kmutex_t __stk_n64_once_mutex;

int __stk_n64_thread_create(__gthread_t *thread, void *(*func)(void *),
                            void *args);
int __stk_n64_thread_join(__gthread_t thread);
int __stk_n64_thread_detach(__gthread_t thread);
__gthread_t __stk_n64_thread_self(void);
int __stk_n64_key_create(__gthread_key_t *key, void (*dtor)(void *));
int __stk_n64_key_delete(__gthread_key_t key);
void *__stk_n64_key_get(__gthread_key_t key);
int __stk_n64_key_set(__gthread_key_t key, const void *ptr);
uint32_t __stk_n64_ticks_until(const struct timespec *abs);

static inline int __gthread_active_p(void) { return __stk_n64_threads_active; }

static inline int __gthread_create(__gthread_t *thread, void *(*func)(void *),
                                   void *args)
{ return __stk_n64_thread_create(thread, func, args); }
static inline int __gthread_join(__gthread_t thread, void **value_ptr)
{
    if (value_ptr) *value_ptr = 0;
    return __stk_n64_thread_join(thread);
}
static inline int __gthread_detach(__gthread_t thread)
{ return __stk_n64_thread_detach(thread); }
static inline int __gthread_equal(__gthread_t t1, __gthread_t t2)
{ return t1 == t2; }
static inline __gthread_t __gthread_self(void)
{ return __stk_n64_thread_self(); }
static inline int __gthread_yield(void)
{ if (__stk_n64_threads_active) kthread_yield(); return 0; }

static inline int __gthread_once(__gthread_once_t *once, void (*func)(void))
{
    if (once->done)
        return 0;
    if (__stk_n64_threads_active) kmutex_lock(&__stk_n64_once_mutex);
    if (!once->done) {
        func();
        once->done = 1;
    }
    if (__stk_n64_threads_active) kmutex_unlock(&__stk_n64_once_mutex);
    return 0;
}

static inline int __gthread_key_create(__gthread_key_t *key, void (*dtor)(void *))
{ return __stk_n64_key_create(key, dtor); }
static inline int __gthread_key_delete(__gthread_key_t key)
{ return __stk_n64_key_delete(key); }
static inline void *__gthread_getspecific(__gthread_key_t key)
{ return __stk_n64_key_get(key); }
static inline int __gthread_setspecific(__gthread_key_t key, const void *ptr)
{ return __stk_n64_key_set(key, ptr); }

static inline void __gthread_mutex_init_function(__gthread_mutex_t *m)
{ kmutex_init(m, KMUTEX_STANDARD); }
static inline int __gthread_mutex_destroy(__gthread_mutex_t *m)
{ (void)m; return 0; }
static inline int __gthread_mutex_lock(__gthread_mutex_t *m)
{ if (__stk_n64_threads_active) kmutex_lock(m); return 0; }
static inline int __gthread_mutex_trylock(__gthread_mutex_t *m)
{
    if (!__stk_n64_threads_active) return 0;
    return kmutex_try_lock(m, 0) ? 0 : EBUSY;
}
static inline int __gthread_mutex_unlock(__gthread_mutex_t *m)
{ if (__stk_n64_threads_active) kmutex_unlock(m); return 0; }

static inline void __gthread_recursive_mutex_init_function(__gthread_recursive_mutex_t *m)
{ kmutex_init(m, KMUTEX_RECURSIVE); }
static inline int __gthread_recursive_mutex_destroy(__gthread_recursive_mutex_t *m)
{ (void)m; return 0; }
static inline int __gthread_recursive_mutex_lock(__gthread_recursive_mutex_t *m)
{ return __gthread_mutex_lock(m); }
static inline int __gthread_recursive_mutex_trylock(__gthread_recursive_mutex_t *m)
{ return __gthread_mutex_trylock(m); }
static inline int __gthread_recursive_mutex_unlock(__gthread_recursive_mutex_t *m)
{ return __gthread_mutex_unlock(m); }

static inline void __gthread_cond_init_function(__gthread_cond_t *c)
{ kcond_init(c); }
static inline int __gthread_cond_broadcast(__gthread_cond_t *c)
{ if (__stk_n64_threads_active) kcond_broadcast(c); return 0; }
static inline int __gthread_cond_signal(__gthread_cond_t *c)
{ if (__stk_n64_threads_active) kcond_signal(c); return 0; }
static inline int __gthread_cond_wait(__gthread_cond_t *c, __gthread_mutex_t *m)
{ kcond_wait(c, m); return 0; }
static inline int __gthread_cond_wait_recursive(__gthread_cond_t *c,
                                                __gthread_recursive_mutex_t *m)
{ kcond_wait(c, m); return 0; }
static inline int __gthread_cond_timedwait(__gthread_cond_t *c,
                                           __gthread_mutex_t *m,
                                           const __gthread_time_t *abs)
{
    uint32_t ticks = __stk_n64_ticks_until(abs);
    if (ticks == 0) return ETIMEDOUT;
    return kcond_wait_timeout(c, m, ticks) ? 0 : ETIMEDOUT;
}
static inline int __gthread_cond_destroy(__gthread_cond_t *c)
{ (void)c; return 0; }

#ifdef __cplusplus
}
#endif

#endif /* STK_N64_GTHR_DEFAULT_H */
