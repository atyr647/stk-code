// std::chrono clocks for the N64.
//
// libstdc++ on this target has neither CLOCK_MONOTONIC nor CLOCK_REALTIME, so
// both system_clock and steady_clock fall back to gettimeofday() -- which
// libdragon implements on the cartridge RTC: whole seconds, or an error when
// there is no RTC. Time effectively stands still, which hangs
// this_thread::sleep_for and breaks STK's frame timing.
//
// Defining the two now() functions here keeps libstdc++.a's chrono.o (which
// defines nothing else) from being linked at all, so there is no clash. Both
// clocks run off the CPU count register via get_ticks_us(): microsecond
// resolution, monotonic. system_clock adds the RTC's wall time at boot when a
// cartridge RTC exists, else counts from the 1970 epoch.
#include <chrono>
#include <ctime>
#include <sys/time.h>
extern "C" {
#include <n64sys.h>
}

extern "C" int64_t __stk_n64_realtime_us(void);

// libdragon's own gettimeofday() (RTC-based): with -Wl,--wrap=gettimeofday
// in effect, every other reference -- including newlib's time() -- lands in
// __wrap_gettimeofday below, so reading the RTC must bypass the wrap or the
// two call each other forever.
extern "C" int __real_gettimeofday(struct timeval *tv, void *tz);

static int64_t boot_epoch_us()
{
    // Wall time at CPU-counter zero, computed once. Without an RTC the clock
    // simply counts from boot.
    static bool known = false;
    static int64_t epoch = 0;
    if (!known)
    {
        known = true;       // before the RTC read: no re-entry
        struct timeval tv;
        if (__real_gettimeofday(&tv, nullptr) == 0 && tv.tv_sec > 0)
            epoch = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec -
                    (int64_t)get_ticks_us();
    }
    return epoch;
}

extern "C" int64_t __stk_n64_realtime_us(void)
{
    return boot_epoch_us() + (int64_t)get_ticks_us();
}

namespace std { namespace chrono { inline namespace _V2 {

system_clock::time_point system_clock::now() noexcept
{
    return time_point(duration(chrono::microseconds(__stk_n64_realtime_us())));
}

steady_clock::time_point steady_clock::now() noexcept
{
    return time_point(duration(chrono::microseconds((int64_t)get_ticks_us())));
}

} } }

// gettimeofday(): libdragon's reads the cartridge RTC and, on the (common)
// cartridges without one, fails without filling in *tv -- callers such as
// STK's profiler then use an uninitialised struct. Every reference is
// redirected here with -Wl,--wrap=gettimeofday (n64/stk_n64_target.cmake):
// microsecond resolution from the CPU counter, RTC wall time when available.
#include <sys/time.h>
extern "C" int __wrap_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    int64_t us = __stk_n64_realtime_us();
    tv->tv_sec = (time_t)(us / 1000000);
    tv->tv_usec = (suseconds_t)(us % 1000000);
    return 0;
}
