/* IEEE completion for the VR4300's FPU.
 *
 * The VR4300 does not compute with NaN or denormal operands: it raises an
 * "unimplemented operation" exception, which cannot be masked. Desktop
 * code (STK, Bullet) relies on IEEE behaviour there -- a zero-length vector
 * normalises to NaN, comparisons with NaN are false, and so on. Like MIPS
 * operating systems, this completes such instructions in software:
 *
 *  - NaN operands: the result is the MIPS default quiet NaN (conversions to
 *    integer give the "invalid" value, compares are unordered), exactly what
 *    the FPU produces with the invalid-operation trap disabled;
 *  - denormal operands: flushed to zero, as FCR31.FS already does for
 *    denormal results, and the operation is then done by the FPU.
 *
 * An instruction in a branch delay slot completes the branch too. Anything
 * else is passed on to libdragon's handler (crash screen).
 *
 * libdragon's exception return reloads $f0-$f19 and FCR31 from the saved
 * frame only while the FPU is enabled, and never reloads callee-saved
 * registers ($f20-$f31, $s0-$s7, $gp, $fp): a C handler preserves those.
 * So results are written to the live registers as well as to the frame,
 * and this file is compiled with $f20-$f31 fixed (see CMakeLists.txt), so
 * that its own epilogue cannot restore stale values over them.
 */
#undef NDEBUG
#include <libdragon.h>
#include <exception.h>
#include <cop0.h>
#include <cop1.h>
#include <stdint.h>
#include <string.h>

#define FMT_S 16
#define FMT_D 17
#define FMT_W 20
#define FMT_L 21

#define FCR31_C (1u << 23)                    /* compare condition */
#define QNAN_S 0x7fbfffffu                    /* MIPS legacy default NaN */
#define QNAN_D 0x7ff7ffffffffffffull
#define INVALID_W 0x7fffffffu
#define INVALID_L 0x7fffffffffffffffull

static exception_handler_t prev_handler;
static volatile uint32_t emulated;

static int nan_s(uint32_t x) { return (x & 0x7f800000u) == 0x7f800000u && (x & 0x007fffffu); }
static int nan_d(uint64_t x) { return (x & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (x & 0x000fffffffffffffull); }
static uint32_t flush_s(uint32_t x) { return (x & 0x7f800000u) ? x : (x & 0x80000000u); }
static uint64_t flush_d(uint64_t x) { return (x & 0x7ff0000000000000ull) ? x : (x & 0x8000000000000000ull); }

static float f_of(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t b_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static double d_of(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static uint64_t b_ofd(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

static uint32_t get_s(reg_block_t *r, int n) { return (uint32_t)r->fpr[n]; }
static uint64_t get_d(reg_block_t *r, int n) { return r->fpr[n]; }
#if !defined(__mips_hard_float) || !defined(STK_N64_FPU_EMU_FIXED_REGS)
#error "build with STK_N64_FPU_EMU_FIXED_REGS and -ffixed-$f20..$f31"
#endif

/* The live register too (see the top of the file). */
static void set_live(int n, uint64_t v)
{
#define L(N) case N: __asm__ volatile("dmtc1 %0, $f" #N :: "r"(v)); break;
    switch (n)
    {
    L(0) L(1) L(2) L(3) L(4) L(5) L(6) L(7) L(8) L(9) L(10) L(11) L(12)
    L(13) L(14) L(15) L(16) L(17) L(18) L(19) L(20) L(21) L(22) L(23)
    L(24) L(25) L(26) L(27) L(28) L(29) L(30) L(31)
    }
#undef L
}
static void set_s(reg_block_t *r, int n, uint32_t v)
{
    r->fpr[n] = (r->fpr[n] & 0xffffffff00000000ull) | v;
    set_live(n, r->fpr[n]);
}
static void set_d(reg_block_t *r, int n, uint64_t v)
{
    r->fpr[n] = v;
    set_live(n, v);
}

/* Round a double to an integer as the given mode does (0 nearest-even,
 * 1 toward zero, 2 up, 3 down). Returns 0 if out of range. */
static int to_int(double v, int mode, int64_t lo, int64_t hi, int64_t *out)
{
    double t;
    switch (mode)
    {
    case 1: t = __builtin_trunc(v); break;
    case 2: t = __builtin_ceil(v); break;
    case 3: t = __builtin_floor(v); break;
    default:
        t = __builtin_floor(v);
        if (v - t > 0.5 || (v - t == 0.5 && __builtin_fmod(t, 2.0) != 0))
            t += 1.0;
        break;
    }
    if (!(t >= (double)lo && t <= (double)hi))
        return 0;
    *out = (int64_t)t;
    return 1;
}

/* Executes the COP1 arithmetic instruction; 0 if it is not one we handle. */
static int emulate(reg_block_t *r, uint32_t insn)
{
    const int fmt = (insn >> 21) & 31, ft = (insn >> 16) & 31;
    const int fs = (insn >> 11) & 31, fd = (insn >> 6) & 31;
    const int func = insn & 63;
    const int rounding = r->fc31 & 3;
    if (fmt != FMT_S && fmt != FMT_D)
        return 0;
    const int dbl = fmt == FMT_D;
    const uint64_t a = dbl ? get_d(r, fs) : get_s(r, fs);
    const uint64_t b = dbl ? get_d(r, ft) : get_s(r, ft);
    const int an = dbl ? nan_d(a) : nan_s((uint32_t)a);
    const int bn = dbl ? nan_d(b) : nan_s((uint32_t)b);
    /* Numeric values only for non-NaN operands: a NaN must never reach the
     * FPU here, it would trap inside this handler (-ftrapping-math, GCC's
     * default, keeps these conversions behind the tests). */
    const double av = an ? 0.0 : dbl ? d_of(flush_d(a))
                                     : (double)f_of(flush_s((uint32_t)a));
    const double bv = bn ? 0.0 : dbl ? d_of(flush_d(b))
                                     : (double)f_of(flush_s((uint32_t)b));

#define RESULT(v) do { if (dbl) set_d(r, fd, b_ofd(v)); \
                       else set_s(r, fd, b_of((float)(v))); } while (0)
#define RESULT_NAN() do { if (dbl) set_d(r, fd, QNAN_D); \
                          else set_s(r, fd, QNAN_S); } while (0)
    switch (func)
    {
    case 0: case 1: case 2: case 3:                 /* add sub mul div */
        if (an || bn) { RESULT_NAN(); return 1; }
        if (dbl)
        {
            const double x = av, y = bv;
            RESULT(func == 0 ? x + y : func == 1 ? x - y :
                   func == 2 ? x * y : x / y);
        }
        else
        {
            const float x = (float)av, y = (float)bv;
            RESULT(func == 0 ? x + y : func == 1 ? x - y :
                   func == 2 ? x * y : x / y);
        }
        return 1;
    case 4:                                         /* sqrt */
        if (an || av < 0) { RESULT_NAN(); return 1; }
        if (dbl) RESULT(__builtin_sqrt(av));
        else RESULT(__builtin_sqrtf((float)av));
        return 1;
    case 5:                                         /* abs */
        if (dbl) set_d(r, fd, a & ~0x8000000000000000ull);
        else set_s(r, fd, (uint32_t)a & ~0x80000000u);
        return 1;
    case 6:                                         /* mov */
        if (dbl) set_d(r, fd, a); else set_s(r, fd, (uint32_t)a);
        return 1;
    case 7:                                         /* neg */
        if (dbl) set_d(r, fd, a ^ 0x8000000000000000ull);
        else set_s(r, fd, (uint32_t)a ^ 0x80000000u);
        return 1;
    case 8: case 9: case 10: case 11:               /* round/trunc/ceil/floor.l */
    case 12: case 13: case 14: case 15:             /* round/trunc/ceil/floor.w */
    case 36: case 37:                               /* cvt.w cvt.l */
    {
        const int to_l = func == 37 || (func >= 8 && func <= 11);
        const int mode = func >= 36 ? rounding : (func & 3);
        int64_t v;
        int ok = !an && to_int(av, mode,
                               to_l ? INT64_MIN : INT32_MIN,
                               to_l ? INT64_MAX : INT32_MAX, &v);
        if (to_l) set_d(r, fd, ok ? (uint64_t)v : INVALID_L);
        else set_s(r, fd, ok ? (uint32_t)(int32_t)v : INVALID_W);
        return 1;
    }
    case 32:                                        /* cvt.s */
        if (an) set_s(r, fd, QNAN_S);
        else set_s(r, fd, b_of((float)av));
        return 1;
    case 33:                                        /* cvt.d */
        if (an) set_d(r, fd, QNAN_D);
        else set_d(r, fd, b_ofd(av));
        return 1;
    default:
        if (func >= 48)                             /* c.cond */
        {
            const int cond = func & 15;
            const int unordered = an || bn;
            const int t = unordered ? (cond & 1)
                        : (((cond & 2) && av == bv) || ((cond & 4) && av < bv));
            if (t) r->fc31 |= FCR31_C;
            else r->fc31 &= ~FCR31_C;
            return 1;
        }
        return 0;
    }
#undef RESULT
#undef RESULT_NAN
}

static int64_t gpr(reg_block_t *r, int n) { return n ? (int64_t)r->gpr[n] : 0; }

/* The branch whose delay slot held the instruction: where it goes, doing
 * its link if any. 0 if it is not a branch we know. */
static int complete_branch(reg_block_t *r, uint32_t pc, uint32_t *next)
{
    const uint32_t br = *(volatile uint32_t*)pc;
    const int op = br >> 26, rs = (br >> 21) & 31, rt = (br >> 16) & 31;
    const uint32_t taken = pc + 4 + ((int32_t)(int16_t)br << 2);
    const uint32_t not_taken = pc + 8;
    int cond;
    switch (op)
    {
    case 0:                                         /* SPECIAL: jr / jalr */
        if ((br & 63) == 8) { *next = (uint32_t)gpr(r, rs); return 1; }
        if ((br & 63) == 9)
        {
            const uint32_t to = (uint32_t)gpr(r, rs);
            const int rd = (br >> 11) & 31;
            // only registers the exception return reloads
            if ((rd >= 16 && rd <= 23) || rd == 28 || rd == 30)
                return 0;
            if (rd) r->gpr[rd] = (int64_t)(int32_t)(pc + 8);
            *next = to;
            return 1;
        }
        return 0;
    case 1:                                         /* REGIMM */
        switch (rt)
        {
        case 0: case 2: cond = gpr(r, rs) < 0; break;           /* bltz(l) */
        case 1: case 3: cond = gpr(r, rs) >= 0; break;          /* bgez(l) */
        case 16: case 18: cond = gpr(r, rs) < 0;                /* bltzal(l) */
            r->gpr[31] = (int64_t)(int32_t)(pc + 8); break;
        case 17: case 19: cond = gpr(r, rs) >= 0;               /* bgezal(l) */
            r->gpr[31] = (int64_t)(int32_t)(pc + 8); break;
        default: return 0;
        }
        *next = cond ? taken : not_taken;
        return 1;
    case 2: case 3:                                 /* j / jal */
        if (op == 3) r->gpr[31] = (int64_t)(int32_t)(pc + 8);
        *next = ((pc + 4) & 0xf0000000u) | ((br & 0x03ffffffu) << 2);
        return 1;
    case 4: case 20: cond = gpr(r, rs) == gpr(r, rt); break;    /* beq(l) */
    case 5: case 21: cond = gpr(r, rs) != gpr(r, rt); break;    /* bne(l) */
    case 6: case 22: cond = gpr(r, rs) <= 0; break;             /* blez(l) */
    case 7: case 23: cond = gpr(r, rs) > 0; break;              /* bgtz(l) */
    case 17:                                        /* COP1: bc1f/t(l) */
        if (rs != 8) return 0;
        cond = ((r->fc31 & FCR31_C) != 0) == ((rt & 1) != 0);
        break;
    default:
        return 0;
    }
    *next = cond ? taken : not_taken;
    return 1;
}

static void fpu_emu_handler(exception_t *ex)
{
    reg_block_t *r = ex->regs;
    if (ex->code == EXCEPTION_CODE_FLOATING_POINT &&
        (r->fc31 & C1_CAUSE_NOT_IMPLEMENTED))
    {
        const int bd = (r->cr & C0_CAUSE_BD) != 0;
        const uint32_t pc = r->epc;
        const uint32_t insn = *(volatile uint32_t*)(pc + (bd ? 4 : 0));
        // our own FPU use below must not see the pending cause
        r->fc31 &= ~C1_CAUSE_MASK;
        C1_WRITE_FCR31(r->fc31);
        uint32_t next = pc + 4;
#ifdef STK_N64_FPU_EMU_TRACE
        static int traced;
        if (traced++ < 40)
            debugf("[fpu] pc %08lx%s insn %08lx fc31 %08lx sr %08lx\n",
                   (unsigned long)pc, bd ? " (bd)" : "", (unsigned long)insn,
                   (unsigned long)r->fc31, (unsigned long)r->sr);
#endif
        if ((insn >> 26) == 17 && ((insn >> 25) & 1) &&       /* COP1 CO */
            (!bd || complete_branch(r, pc, &next)) && emulate(r, insn))
        {
            r->epc = next;
            C1_WRITE_FCR31(r->fc31);       /* condition bit, live */
            emulated++;
            return;
        }
    }
    prev_handler(ex);
}

void stk_n64_fpu_emu_init(void)
{
    prev_handler = register_exception_handler(fpu_emu_handler);
}

uint32_t stk_n64_fpu_emulated(void)
{
    return emulated;
}
