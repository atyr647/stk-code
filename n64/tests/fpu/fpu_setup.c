/* Copy of stk_n64_fpu_setup() from n64/compat/src/gthr_libdragon.c. */
#include <libdragon.h>
#include <cop1.h>
#include <stdint.h>

void stk_n64_fpu_setup(void)
{
    uint32_t fcr31 = C1_FCR31();
    fcr31 &= ~(C1_ENABLE_MASK | C1_CAUSE_MASK);
    fcr31 |= C1_FCR31_FS;
    C1_WRITE_FCR31(fcr31);
}
