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
 * This header is included by every C++ file that uses <mutex>, so it must not
 * drag libdragon's headers (and their macros -- n64sys.h redefines srand)
 * into STK. The kernel objects are therefore opaque here; their layout is
 * checked against libdragon's kmutex_t/kcond_t in gthr_libdragon.c, which is
 * the only place the real types are visible.
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

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A thread handle is a small control block, not the kthread_t: see
 * gthr_libdragon.c for why joins do not go through kthread_join(). */
typedef struct __stk_n64_thread *__gthread_t;
typedef unsigned int __gthread_key_t;
typedef struct { volatile int done; } __gthread_once_t;
/* == libdragon kmutex_t: 8 bytes, zero-initialisable (kernel.h) */
typedef struct { uint64_t __opaque; } __attribute__((aligned(8))) __gthread_mutex_t;
typedef __gthread_mutex_t __gthread_recursive_mutex_t;
/* == libdragon kcond_t: one pointer */
typedef struct { void *__opaque; } __gthread_cond_t;
typedef struct timespec __gthread_time_t;

/* kmutex_t is { int8 original_pri:8; uint8 flags:1; ... } -- on this
 * big-endian target the flags bit is bit 55 of the 64-bit word.
 * gthr_libdragon.c static-asserts both initialisers. */
#define __STK_N64_KMUTEX_RECURSIVE_BITS (1ull << 55)
#define __GTHREAD_MUTEX_INIT { 0 }
#define __GTHREAD_MUTEX_INIT_FUNCTION __gthread_mutex_init_function
#define __GTHREAD_RECURSIVE_MUTEX_INIT { __STK_N64_KMUTEX_RECURSIVE_BITS }
#define __GTHREAD_RECURSIVE_MUTEX_INIT_FUNCTION __gthread_recursive_mutex_init_function
#define __GTHREAD_COND_INIT { 0 }
#define __GTHREAD_COND_INIT_FUNCTION __gthread_cond_init_function
#define __GTHREAD_ONCE_INIT { 0 }
#define __GTHREAD_TIME_INIT { 0, 0 }

extern volatile int __stk_n64_threads_active;

int __stk_n64_thread_create(__gthread_t *thread, void *(*func)(void *),
                            void *args);
int __stk_n64_thread_join(__gthread_t thread);
int __stk_n64_thread_detach(__gthread_t thread);
__gthread_t __stk_n64_thread_self(void);
void __stk_n64_thread_yield(void);
int __stk_n64_once(__gthread_once_t *once, void (*func)(void));
int __stk_n64_key_create(__gthread_key_t *key, void (*dtor)(void *));
int __stk_n64_key_delete(__gthread_key_t key);
void *__stk_n64_key_get(__gthread_key_t key);
int __stk_n64_key_set(__gthread_key_t key, const void *ptr);
void __stk_n64_mutex_init(__gthread_mutex_t *m, int recursive);
void __stk_n64_mutex_lock(__gthread_mutex_t *m);
int __stk_n64_mutex_trylock(__gthread_mutex_t *m);
void __stk_n64_mutex_unlock(__gthread_mutex_t *m);
void __stk_n64_cond_init(__gthread_cond_t *c);
void __stk_n64_cond_signal(__gthread_cond_t *c);
void __stk_n64_cond_broadcast(__gthread_cond_t *c);
void __stk_n64_cond_wait(__gthread_cond_t *c, __gthread_mutex_t *m);
int __stk_n64_cond_timedwait(__gthread_cond_t *c, __gthread_mutex_t *m,
                             const struct timespec *abs);

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
{ __stk_n64_thread_yield(); return 0; }
static inline int __gthread_once(__gthread_once_t *once, void (*func)(void))
{ return once->done ? 0 : __stk_n64_once(once, func); }

static inline int __gthread_key_create(__gthread_key_t *key, void (*dtor)(void *))
{ return __stk_n64_key_create(key, dtor); }
static inline int __gthread_key_delete(__gthread_key_t key)
{ return __stk_n64_key_delete(key); }
static inline void *__gthread_getspecific(__gthread_key_t key)
{ return __stk_n64_key_get(key); }
static inline int __gthread_setspecific(__gthread_key_t key, const void *ptr)
{ return __stk_n64_key_set(key, ptr); }

static inline void __gthread_mutex_init_function(__gthread_mutex_t *m)
{ __stk_n64_mutex_init(m, 0); }
static inline int __gthread_mutex_destroy(__gthread_mutex_t *m)
{ (void)m; return 0; }
static inline int __gthread_mutex_lock(__gthread_mutex_t *m)
{ __stk_n64_mutex_lock(m); return 0; }
static inline int __gthread_mutex_trylock(__gthread_mutex_t *m)
{ return __stk_n64_mutex_trylock(m); }
static inline int __gthread_mutex_unlock(__gthread_mutex_t *m)
{ __stk_n64_mutex_unlock(m); return 0; }

static inline void __gthread_recursive_mutex_init_function(__gthread_recursive_mutex_t *m)
{ __stk_n64_mutex_init(m, 1); }
static inline int __gthread_recursive_mutex_destroy(__gthread_recursive_mutex_t *m)
{ (void)m; return 0; }
static inline int __gthread_recursive_mutex_lock(__gthread_recursive_mutex_t *m)
{ __stk_n64_mutex_lock(m); return 0; }
static inline int __gthread_recursive_mutex_trylock(__gthread_recursive_mutex_t *m)
{ return __stk_n64_mutex_trylock(m); }
static inline int __gthread_recursive_mutex_unlock(__gthread_recursive_mutex_t *m)
{ __stk_n64_mutex_unlock(m); return 0; }

static inline void __gthread_cond_init_function(__gthread_cond_t *c)
{ __stk_n64_cond_init(c); }
static inline int __gthread_cond_broadcast(__gthread_cond_t *c)
{ __stk_n64_cond_broadcast(c); return 0; }
static inline int __gthread_cond_signal(__gthread_cond_t *c)
{ __stk_n64_cond_signal(c); return 0; }
static inline int __gthread_cond_wait(__gthread_cond_t *c, __gthread_mutex_t *m)
{ __stk_n64_cond_wait(c, m); return 0; }
static inline int __gthread_cond_wait_recursive(__gthread_cond_t *c,
                                                __gthread_recursive_mutex_t *m)
{ __stk_n64_cond_wait(c, m); return 0; }
static inline int __gthread_cond_timedwait(__gthread_cond_t *c,
                                           __gthread_mutex_t *m,
                                           const __gthread_time_t *abs)
{ return __stk_n64_cond_timedwait(c, m, abs); }
static inline int __gthread_cond_destroy(__gthread_cond_t *c)
{ (void)c; return 0; }

#ifdef __cplusplus
}
#endif

#endif /* STK_N64_GTHR_DEFAULT_H */
