// Regression test for the N64 threading shim (n64/compat): standard C++
// threads on libdragon's kernel. Paints the screen GREEN if every check
// passes, RED otherwise, and prints each result -- so ares (or hardware)
// gives an unambiguous answer. Run with n64/tools/ares_run.sh.
#include <libdragon.h>
#include <kernel.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

extern "C" volatile int __stk_n64_threads_active;

static int failures = 0;
static void check(const char* what, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static thread_local int tls_value = 7;

int main()
{
    debug_init_isviewer();
    debug_init_usblog();
    timer_init();
    kernel_init();
    __stk_n64_threads_active = 1;
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    // 1. a thread runs and join() waits for it
    int ran = 0;
    std::thread t([&] { ran = 42; });
    t.join();
    check("std::thread runs, join waits", ran == 42);

    // 2. mutex-protected counter across 4 threads that yield while holding
    //    and between iterations (forces interleaving on a cooperative kernel)
    std::mutex m;
    int counter = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 100; k++) {
                std::lock_guard<std::mutex> g(m);
                int c = counter;
                std::this_thread::yield();
                counter = c + 1;
            }
        });
    for (auto& x : ts) x.join();
    check("mutex serialises 4 threads (400)", counter == 400);

    // 3. condition_variable producer/consumer
    std::condition_variable cv;
    bool ready = false; int got = 0;
    std::thread consumer([&] {
        std::unique_lock<std::mutex> ul(m);
        cv.wait(ul, [&] { return ready; });
        got = 1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::lock_guard<std::mutex> g(m); ready = true; }
    cv.notify_one();
    consumer.join();
    check("condition_variable wait/notify", got == 1);

    // 4. wait_for times out (and does not hang on an expired deadline)
    {
        std::unique_lock<std::mutex> ul(m);
        auto t0 = std::chrono::steady_clock::now();
        bool r = cv.wait_for(ul, std::chrono::milliseconds(30),
                             [] { return false; });
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check("wait_for times out (~30 ms)", !r && ms >= 25 && ms < 500);
    }

    // 5. call_once from several threads
    std::once_flag once; std::atomic<int> calls{0};
    std::vector<std::thread> os;
    for (int i = 0; i < 3; i++)
        os.emplace_back([&] { std::call_once(once, [&] { calls++; }); });
    for (auto& x : os) x.join();
    check("call_once runs exactly once", calls == 1);

    // 6. thread_local is per thread
    int other = 0;
    std::thread tl([&] { tls_value = 99; other = tls_value; });
    tl.join();
    check("thread_local is per-thread", other == 99 && tls_value == 7);

    // 7. recursive_mutex
    std::recursive_mutex rm;
    rm.lock(); rm.lock(); rm.unlock(); rm.unlock();
    check("recursive_mutex relocks", rm.try_lock());
    rm.unlock();

    printf("%s: %d failure(s)\n", failures ? "THREADS FAIL" : "THREADS PASS",
           failures);
    console_render();
    console_close();
    display_close();

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE,
                 FILTERS_RESAMPLE);
    while (1) {
        surface_t* fb = display_get();
        graphics_fill_screen(fb, failures ? graphics_make_color(255, 0, 0, 255)
                                          : graphics_make_color(0, 255, 0, 255));
        graphics_draw_text(fb, 20, 20, failures ? "THREADS FAIL" : "THREADS PASS");
        display_show(fb);
    }
}
