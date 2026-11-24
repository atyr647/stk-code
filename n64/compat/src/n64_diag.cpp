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
#include <cstring>
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

#ifdef STK_N64_LOG_DMA
// ----------------------------------------------------------------------------
// ROM read profile (-DSTK_N64_LOG_DMA=ON): every PI DMA from the cartridge is
// counted, timed and binned by 64 KB region of ROM; a summary goes to the log
// every 10 s. ROM regions are matched to files with dfs_rom_addr.
extern "C" void __real_dma_read(void *ram, pi_addr_t pi, unsigned long len);
namespace
{
    struct DmaBin { uint32_t region, calls, bytes, ticks; };
    DmaBin   g_bins[64];
    uint32_t g_calls, g_bytes, g_ticks, g_last_report;
}

extern "C" __attribute__((used))
void __wrap_dma_read(void *ram, pi_addr_t pi, unsigned long len)
{
    const uint32_t t0 = TICKS_READ();
    __real_dma_read(ram, pi, len);
    const uint32_t dt = TICKS_READ() - t0;
    g_calls++; g_bytes += len; g_ticks += dt;
    static uint32_t total;
    if (++total % 4000 == 0)
    {
        debugf("[dma] read %lu bytes from %08lx, called from:\n",
               (unsigned long)len, (unsigned long)pi);
        debug_backtrace();
    }
    const uint32_t region = (uint32_t)pi >> 16;
    for (unsigned i = 0; i < 64; i++)
    {
        DmaBin &b = g_bins[(region + i) & 63];
        if (b.calls == 0 || b.region == region)
        {
            b.region = region; b.calls++; b.bytes += len; b.ticks += dt;
            break;
        }
    }
    if (t0 - g_last_report > TICKS_FROM_MS(10000))
    {
        g_last_report = t0;
        debugf("[dma] %lu reads, %lu KB, %lu ms\n", (unsigned long)g_calls,
               (unsigned long)g_bytes / 1024,
               (unsigned long)TICKS_TO_MS(g_ticks));
        for (unsigned k = 0; k < 4; k++)
        {
            DmaBin *top = NULL;
            for (auto &b : g_bins)
                if (b.calls && (!top || b.ticks > top->ticks)) top = &b;
            if (!top) break;
            debugf("[dma]   rom %08lx: %lu reads, %lu KB, %lu ms\n",
                   (unsigned long)top->region << 16,
                   (unsigned long)top->calls,
                   (unsigned long)top->bytes / 1024,
                   (unsigned long)TICKS_TO_MS(top->ticks));
            top->calls = 0;           // report each region once
        }
        memset(g_bins, 0, sizeof(g_bins));
        g_calls = g_bytes = g_ticks = 0;
    }
}

extern "C" void stk_n64_log_rom_addr(const char *path)
{
    debugf("[dma] %s at rom %08lx\n", path, (unsigned long)dfs_rom_addr(path));
}
#endif

#ifdef STK_N64_PROFILE
// ----------------------------------------------------------------------------
// Sampling profiler (-DSTK_N64_PROFILE=ON). A timer interrupt every 2 ms of
// N64 time logs the interrupted PC, RA and the return addresses found on the
// interrupted stack:   [prof] pc ra a1 a2 ...   (hex)
// [prof] when the main thread was running, [othr] for any other thread
// (idle included), then [wait]: where the main thread is parked.
// n64/tools/n64_profile.py turns the log into flat and inclusive profiles.
// Loading is skipped: sampling starts once MainLoop::renderGUI (loading
// screens only, on a server build) has not been called for 3 s.
extern "C" void *interrupt_exception_frame;
extern "C" char __text_start[], __text_end[];
// libdragon kernel internals (kernel_internal.h): a kthread_t starts with the
// register block it was parked with
// (unsized, so GCC does not assume it is in the small-data area)
extern "C" void *th_cur;
extern "C" char th_main[];
namespace
{
    volatile uint32_t g_last_gui;
    volatile bool     g_gui_seen;

    bool is_return_address(uint32_t a)
    {
        if (a < (uint32_t)__text_start + 8 || a >= (uint32_t)__text_end ||
            (a & 3))
            return false;
        const uint32_t call = *(const uint32_t*)(a - 8);
        const uint32_t op = call >> 26;
        return op == 3 || (op == 0 && (call & 63) == 9);     // jal, jalr
    }

    void log_sample(const char *tag, const reg_block_t *r, const uint32_t *sp)
    {
        char line[16 * 9 + 24];
        int n = sprintf(line, "[%s] %08lx %08lx", tag, (unsigned long)r->epc,
                        (unsigned long)(uint32_t)r->ra);
        int found = 0;
        if ((uint32_t)sp < 0x80000000u)
            sp = (const uint32_t*)0x80800000u;      // no stack to scan
        for (int i = 0; i < 96 && found < 14; i++)
        {
            if ((uint32_t)(sp + i) >= 0x80800000u)
                break;
            const uint32_t a = sp[i];
            if (is_return_address(a))
            {
                n += sprintf(line + n, " %08lx", (unsigned long)a);
                found++;
            }
        }
        line[n++] = '\n';
        line[n] = 0;
        debugf("%s", line);
    }

    void profile_tick(int ovfl)
    {
        if (!g_gui_seen || TICKS_READ() - g_last_gui < TICKS_FROM_MS(3000))
            return;
        uint8_t *frame = (uint8_t*)interrupt_exception_frame;
        if (!frame)
            return;
        // the interrupted thread; if it is not the main thread, also where
        // the main thread is parked (what it waits for)
        log_sample(th_cur == (void*)th_main ? "prof" : "othr",
                   (const reg_block_t*)(frame + 32),
                   (const uint32_t*)(frame + 576));
        const reg_block_t *m = *(reg_block_t* const*)th_main;
        if (th_cur != (void*)th_main && m)
        {
            log_sample("wait", m, (const uint32_t*)(uint32_t)m->sp);
        }
    }
}

extern "C" void stk_n64_profile_gui(void)
{
    g_last_gui = TICKS_READ();
    if (!g_gui_seen)
    {
        g_gui_seen = true;
        new_timer(TICKS_FROM_MS(2), TF_CONTINUOUS, profile_tick);
    }
}
#endif
