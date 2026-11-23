// Failure reporting for the N64 build.
//
// Problems during C++ static initialisation (before main) are otherwise
// invisible: the debug channel is not usable yet, and libdragon's abort()
// asks the emulator to exit, which ares answers by rebooting the ROM. So:
//  * a constructor that runs right after libdragon's own (priorities
//    101-130) starts the on-screen console, which also receives stdout --
//    i.e. STK's log;
//  * uncaught exceptions print their what() before terminating;
//  * abort()/_exit() (wrapped at link time) halt with the message on screen
//    instead of resetting.
// libdragon compiles debugf()/debug_init_*() to nothing under NDEBUG, which
// release (MinSizeRel) builds define; the platform layer's diagnostics must
// survive that.
#undef NDEBUG
#include <libdragon.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <backtrace.h>
#include <typeinfo>
#include <cxxabi.h>

extern "C" void __real_abort(void);
extern "C" void __real__exit(int);

static volatile bool console_up = false;

static void halt(const char *why, int code)
{
    fprintf(stdout, "\n[n64] %s (%d) -- halted\n", why, code);
    if (console_up) console_render();
    debugf("[n64] %s (%d) -- halted\n", why, code);
    while (true) { }
}

extern "C" __attribute__((used)) void __wrap_abort(void) { halt("abort()", 0); }
extern "C" __attribute__((used)) void __wrap__exit(int code) { halt("exit", code); }

static void on_terminate()
{
    if (std::exception_ptr p = std::current_exception())
    {
        try { std::rethrow_exception(p); }
        catch (const std::exception &e)
        {
            printf("[n64] uncaught %s: %s\n", typeid(e).name(), e.what());
        }
        catch (...) { printf("[n64] uncaught non-std exception\n"); }
    }
    halt("std::terminate", 0);
}

// Out of memory: say how much was asked for, how full the heap is, and
// who asked, before std::bad_alloc unwinds all of that away.
static void on_out_of_memory()
{
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        heap_stats_t hs;
        sys_get_heap_stats(&hs);
        debugf("[n64] out of memory: heap %d KiB used of %d KiB (%d free)\n",
               hs.used / 1024, hs.total / 1024, hs.free / 1024);
        debug_backtrace();
    }
    std::set_new_handler(nullptr);
    throw std::bad_alloc();
}

__attribute__((constructor(200))) static void n64_diag_init()
{
    std::set_terminate(on_terminate);
    std::set_new_handler(on_out_of_memory);
#ifdef STK_N64_SCREEN_LOG
    debug_init_isviewer();
    console_init();
    console_set_render_mode(RENDER_AUTOMATIC);
    console_set_debug(true);           // mirror the console to the debug channel
    console_up = true;
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    printf("[n64] heap: %d KiB used of %d KiB\n", hs.used / 1024, hs.total / 1024);
    printf("[n64] static initialisation...\n");
#endif
}

extern "C" void stk_n64_console_started(void) { console_up = true; }

// ---------------------------------------------------------------------------
// Exception pointers on the o64 ABI.
//
// When libgcc's unwinder enters a landing pad it puts the exception object in
// a 64-bit register slot with _Unwind_SetGR(), which zero-extends the 32-bit
// pointer (0x00000000_8xxxxxxx). Pointers on o64 must be sign-extended
// (0xFFFFFFFF_8xxxxxxx), and libdragon runs the CPU with 64-bit addressing,
// so the zero-extended value is an invalid address. Code that spills the
// pointer through a 32-bit stack slot re-sign-extends it by accident; code
// that keeps it in registers (common with -flto) hands it on as is and the
// runtime faults. The pointer reaches the runtime only through these entry
// points, so they canonicalise it first (`sll r, r, 0` sign-extends the low
// word). Linked with -Wl,--wrap=<name> (n64/stk_n64_target.cmake).
#define CANON(p) asm volatile("sll %0, %0, 0" : "+r"(p))

extern "C" void __real__Unwind_Resume(void *);
extern "C" __attribute__((used, noreturn)) void __wrap__Unwind_Resume(void *e)
{ CANON(e); __real__Unwind_Resume(e); __builtin_unreachable(); }

extern "C" void *__real___cxa_begin_catch(void *);
extern "C" __attribute__((used)) void *__wrap___cxa_begin_catch(void *e)
{ CANON(e); return __real___cxa_begin_catch(e); }

extern "C" void *__real___cxa_get_exception_ptr(void *);
extern "C" __attribute__((used)) void *__wrap___cxa_get_exception_ptr(void *e)
{ CANON(e); return __real___cxa_get_exception_ptr(e); }

extern "C" void __real___cxa_call_unexpected(void *);
extern "C" __attribute__((used, noreturn)) void __wrap___cxa_call_unexpected(void *e)
{ CANON(e); __real___cxa_call_unexpected(e); __builtin_unreachable(); }

// Every thread's stack comes out of the heap on the N64 (on desktop systems
// it does not): say who starts one.
extern "C" void stk_n64_note_thread(int stack_bytes)
{
    static int count = 0;
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("[n64] thread %d starting (%d KiB stack; heap %d KiB used)\n",
           ++count, stack_bytes / 1024, hs.used / 1024);
    debug_backtrace();
}
