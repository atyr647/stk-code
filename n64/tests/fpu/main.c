/* Regression ROM for n64/compat/src/fpu_emu.c: IEEE behaviour (NaN,
 * denormals) on the VR4300, as STK and Bullet expect from desktop CPUs.
 * Prints "fpu: <passed> passed, <failed> failed" on the debug channel. */
#undef NDEBUG
#include <libdragon.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

void stk_n64_fpu_setup(void);
void stk_n64_fpu_emu_init(void);
uint32_t stk_n64_fpu_emulated(void);

static int passed, failed;
static void check(const char *what, int ok)
{
    if (ok) passed++;
    else { failed++; debugf("FAIL: %s\n", what); }
}
static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float from(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static int is_nan(float f) { uint32_t b = bits(f); return (b & 0x7f800000) == 0x7f800000 && (b & 0x7fffff); }
static int is_nan_d(double d) { uint64_t b; memcpy(&b, &d, 8); return (b >> 52 & 0x7ff) == 0x7ff && (b & 0xfffffffffffffull); }

/* volatile: keep the compiler from folding anything */
static volatile float vzero = 0.0f, vone = 1.0f, vtwo = 2.0f, vminus = -1.0f;
static volatile double dzero = 0.0, done_ = 1.0;
static volatile uint32_t vdenorm_bits = 0x00000001;   /* smallest denormal */

__attribute__((noinline)) static float normalize_x(float x, float y, float z)
{
    /* what btVector3::normalize does to a zero vector */
    float len = sqrtf(x * x + y * y + z * z);
    return x / len;
}

__attribute__((noinline)) static int count_nan_less(const volatile float *v, int n)
{
    /* compares and arithmetic in a loop: GCC puts FP instructions in branch
     * delay slots here, which exercises the branch completion */
    int c = 0;
    for (int i = 0; i < n; i++)
        if (v[i] * 2.0f < 1.0f) c++;
    return c;
}

static void tests(const char *who)
{
    debugf("-- %s\n", who);
    float nan = vzero / vzero;                         /* 0/0, no trap */
    check("0/0 is NaN", is_nan(nan));
    check("NaN+1 is NaN", is_nan(nan + vone));
    check("NaN*2 is NaN", is_nan(nan * vtwo));
    check("1-NaN is NaN", is_nan(vone - nan));
    check("NaN/NaN is NaN", is_nan(nan / nan));
    check("NaN < 1 is false", !(nan < vone));
    check("NaN > 1 is false", !(nan > vone));
    check("NaN == NaN is false", !(nan == nan));
    check("NaN != NaN is true", nan != nan);
    check("!(NaN >= 1)", !(nan >= vone));
    check("sqrt(-1) is NaN", is_nan(sqrtf(vminus)));
    check("fabs(NaN) is NaN", is_nan(fabsf(nan)));
    check("-NaN is NaN", is_nan(-nan));
    check("(int)NaN is INT_MAX", (int)nan == 0x7fffffff);
    check("normalize(0,0,0).x is NaN", is_nan(normalize_x(vzero, vzero, vzero)));
    check("1/0 is inf", bits(vone / vzero) == 0x7f800000);
    check("acosf(NaN) is NaN", is_nan(acosf(nan)));
    float d = from(vdenorm_bits);
    check("denormal * 2 flushes to 0", d * vtwo == 0.0f);
    check("denormal + 1 is 1", d + vone == 1.0f);
    check("denormal < 1", d < vone);
    double dn = dzero / dzero;
    check("double 0/0 is NaN", is_nan_d(dn));
    check("double NaN+1 is NaN", is_nan_d(dn + done_));
    check("double NaN < 1 is false", !(dn < done_));
    check("(float)double NaN is NaN", is_nan((float)dn));
    check("(double)float NaN is NaN", is_nan_d((double)nan));
    volatile float arr[64];
    for (int i = 0; i < 64; i++) arr[i] = (i % 3 == 0) ? nan : (i % 3 == 1 ? 0.1f : 5.0f);
    check("compares with NaN in a loop", count_nan_less(arr, 64) == 21);
}

static volatile int thread_done;
static int thread_main(void *arg)
{
    stk_n64_fpu_setup();          /* what the gthr layer does for STK threads */
    tests("kernel thread");
    thread_done = 1;
    return 0;
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    stk_n64_fpu_setup();
    stk_n64_fpu_emu_init();
    tests("main thread");
    kernel_init();
    /* (kthread_join is unreliable at this libdragon revision, see
     * gthr_libdragon.c) */
    kthread_new("fpu", 8192, 0, thread_main, NULL);
    while (!thread_done)
        kthread_yield();
    debugf("fpu: %d passed, %d failed (%u instructions completed in software)\n",
           passed, failed, (unsigned)stk_n64_fpu_emulated());
    while (1) {}
}
