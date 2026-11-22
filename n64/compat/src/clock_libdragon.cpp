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
extern "C" {
#include <n64sys.h>
}

extern "C" int64_t __stk_n64_realtime_us(void);

static int64_t boot_epoch_us()
{
    static int64_t epoch = -1;
    if (epoch < 0)
    {
        std::time_t t = std::time(nullptr);   // RTC if present, else -1/0
        epoch = (t > 0 ? (int64_t)t * 1000000 : 0) - (int64_t)get_ticks_us();
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
