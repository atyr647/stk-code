// N64 entry point: platform setup, then STK's own main() (renamed stk_main
// by n64/stk_n64_target.cmake).
//
// Layout the game sees:
//   rom:/            the cartridge filesystem (DragonFS), read-only.
//                    rom:/data is STK's data directory (stk-code/data plus
//                    the downscaled stk-assets, n64/tools/stk_n64_assets.py)
//   ram:/            n64/compat/src/ramfs.c; $HOME and the XDG directories,
//                    where STK writes config, profiles and its log
//
// The command line comes from rom:/data/n64_args.txt (one argument per line)
// when present, so a ROM can be built for a specific test (e.g. a profiled
// AI race) without recompiling.
// libdragon compiles debugf()/debug_init_*() to nothing under NDEBUG, which
// release (MinSizeRel) builds define; the platform layer's diagnostics must
// survive that.
#undef NDEBUG
#include <libdragon.h>
#include <system.h>     // hook_stdio_calls
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" volatile int __stk_n64_threads_active;
extern "C" void stk_n64_ramfs_init(void);
int stk_main(int argc, char *argv[]);

static std::vector<std::string> read_args()
{
    std::vector<std::string> args{"supertuxkart"};
    FILE *f = fopen("rom:/data/n64_args.txt", "r");
    if (!f)
        return args;
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        std::string a(line);
        while (!a.empty() && (a.back() == '\n' || a.back() == '\r' ||
                              a.back() == ' '))
            a.pop_back();
        if (!a.empty() && a[0] != '#')
            args.push_back(a);
    }
    fclose(f);
    return args;
}

static void print_heap(const char *when)
{
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("[n64] heap %s: used %d KiB of %d KiB (free %d KiB)\n", when,
           hs.used / 1024, hs.total / 1024, hs.free / 1024);
}

#if !defined(STK_N64_CONSOLE) && !defined(STK_N64_SCREEN_LOG)
static int write_debug(char *data, unsigned int len)
{
#ifdef STK_N64_LOG_HEAP
    // Each log line starts with the heap in use: STK's log becomes a
    // memory timeline.
    static bool line_start = true;
    unsigned int from = 0;
    for (unsigned int i = 0; i < len; i++)
    {
        if (line_start)
        {
            heap_stats_t hs;
            sys_get_heap_stats(&hs);
            debugf("[%4d K] ", hs.used / 1024);
            line_start = false;
        }
        if (data[i] == '\n')
        {
            debugf("%.*s", (int)(i + 1 - from), data + from);
            from = i + 1;
            line_start = true;
        }
    }
    if (from < len)
        debugf("%.*s", (int)(len - from), data + from);
#else
    debugf("%.*s", (int)len, data);
#endif
    return (int)len;
}
#endif

extern "C" void stk_n64_fpu_setup(void);
extern "C" void stk_n64_fpu_emu_init(void);
extern "C" uint32_t stk_n64_fpu_emulated(void);

int main()
{
    stk_n64_fpu_setup();
    stk_n64_fpu_emu_init();
    debug_init_isviewer();
    debug_init_usblog();
    debugf("[n64] main reached (static initialisation done)\n");
    print_heap("before platform init");
    timer_init();
    kernel_init();
    print_heap("after kernel_init");
    __stk_n64_threads_active = 1;
    dfs_init(DFS_DEFAULT_LOCATION);
    print_heap("after dfs_init");
    stk_n64_ramfs_init();
    print_heap("after ramfs init");
    for (const char *d : {"ram:/home", "ram:/home/.config", "ram:/home/.local",
                          "ram:/home/.local/share", "ram:/home/.cache"})
        mkdir(d, 0777);
    setenv("HOME", "ram:/home", 1);
    setenv("XDG_CONFIG_HOME", "ram:/home/.config", 1);
    setenv("XDG_DATA_HOME", "ram:/home/.local/share", 1);
    setenv("XDG_CACHE_HOME", "ram:/home/.cache", 1);
    setenv("SUPERTUXKART_DATADIR", "rom:", 1);   // STK appends "/data/"

#if defined(STK_N64_CONSOLE) && !defined(STK_N64_SCREEN_LOG)
    console_init();
    console_set_render_mode(RENDER_AUTOMATIC);
#endif
#if defined(STK_N64_CONSOLE) || defined(STK_N64_SCREEN_LOG)
    console_set_debug(true);
    print_heap("after console init");
#else
    // No console (it costs 600 KB of heap): stdout -- STK's log -- goes
    // straight to the debug channel.
    static stdio_t stdout_to_debug = { NULL, write_debug, NULL };
    hook_stdio_calls(&stdout_to_debug);
#endif

    printf("[n64] SuperTuxKart for Nintendo 64 (%s RDRAM)\n",
           is_memory_expanded() ? "8 MiB" : "4 MiB");
    print_heap("at start");

    std::vector<std::string> args = read_args();
    std::vector<char *> argv;
    for (auto &a : args)
    {
        printf("[n64] arg: %s\n", a.c_str());
        argv.push_back(&a[0]);
    }
    argv.push_back(nullptr);

    int r = stk_main((int)args.size(), argv.data());

    printf("[n64] stk_main returned %d\n", r);
    printf("[n64] FPU instructions completed in software: %u\n",
           (unsigned)stk_n64_fpu_emulated());
    print_heap("at exit");
    while (true)
        kthread_sleep(TICKS_FROM_MS(1000));
}
