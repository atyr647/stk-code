/* Code paging: most of the program's code stays on the cartridge and is
 * brought into RAM in 32 KB blocks when it is first executed.
 *
 * STK's client is 6.6 MB of code (plus 2 MB of constants and unwind tables)
 * for an 8 MB console. The VR4300 has a TLB, so code can run at mapped
 * addresses: n64/n64_paged.ld links everything that is not part of the
 * platform (STK, Irrlicht, Bullet, the libraries, libstdc++) at
 * PAGED_BASE, and n64/make_rom.sh stores that section as its own file on the
 * cartridge (stk.paged) instead of loading it at boot.
 *
 *   - Each block of that code (8 KB, pager.h) is one TLB entry: an
 *     even/odd pair of pages, backed by one frame of a fixed pool.
 *   - A TLB refill for a block already in a frame is handled in the refill
 *     vector itself (pager_refill.S): a table lookup and tlbwr.
 *   - Otherwise libdragon's exception path calls pager_exception(), which
 *     reads the block from ROM into a frame (the oldest one when the pool is
 *     full, unmapping the block it held), then maps it.
 *
 * What may not be paged: anything that can run with EXL set (exception and
 * interrupt handlers, the kernel's scheduler, timer and interrupt callbacks),
 * because a TLB refill there is not taken through the refill vector. That is
 * libdragon, newlib, libgcc and n64/compat, which stay resident.
 *
 * Calls between the two regions cross a 256 MB boundary, which a MIPS jal
 * cannot: paged code and n64/compat are compiled with -mlong-calls
 * (libstdc++ too: n64/build_libstdcxx_longcalls.sh).
 *
 * The ROM is read by PI DMA with the PI polled: this runs inside an
 * exception, where libdragon's DMA queue (which waits on kernel objects)
 * must not be used. A transfer libdragon started is let finish first, and
 * its completion interrupt is left pending for libdragon to see.
 */
#undef NDEBUG
#include <libdragon.h>
#include <cop0.h>
#include <dma.h>
#include <mi.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>         /* memalign */
#include <string.h>

#include "pager.h"
#define ENTRYLO_FLAGS   ((3u << 3) | C0_ENTRYLO_VALID | C0_ENTRYLO_GLOBAL)
#define TLB_ENTRIES     32
#define PI_CLEAR_INTERRUPT 0x02    /* PI_STATUS write: acknowledge (dma.c) */

#ifndef STK_N64_PAGER_FRAMES
#define STK_N64_PAGER_FRAMES (1024 * 1024 / BLOCK_SIZE)     /* 1 MB */
#endif

extern char __paged_start[], __paged_end[];   /* n64_paged.ld */

/* read by pager_refill.S */
uint32_t __pager_nblocks;
uint32_t *__pager_table;      /* per block: EntryLo0 of its frame, or 0 */

static uint32_t g_rom;        /* PI address of stk.paged */
static uint32_t g_rom_size;
static uint8_t *g_frames;
static uint32_t g_nframes;
static int32_t *g_frame_block;  /* block held by each frame, -1 = free */
static uint32_t g_next;         /* next frame to (re)use: FIFO */
static uint32_t g_faults;

extern void __pager_refill(void);
extern void inthandler(void);

static exception_handler_t g_prev_handler;

/* ------------------------------------------------------------------ TLB -- */
static inline void tlb_write_random(void)
{
    asm volatile("nop; nop; tlbwr; nop; nop" ::: "memory");
}

static inline void set_entryhi(uint64_t v)
{
    asm volatile("dmtc0 %0, $10; nop; nop" :: "r"(v));
}

static inline void set_entrylo(uint32_t lo0, uint32_t lo1)
{
    asm volatile("mtc0 %0, $2; mtc0 %1, $3; nop; nop" :: "r"(lo0), "r"(lo1));
}

/* an EntryHi no access will ever match: in the mapped region below
 * PAGED_BASE, which nothing uses, one VPN2 per TLB index */
static uint64_t dummy_entryhi(int index)
{
    /* (PAGED_BASE - 0xC0000000 = 8 MB: room for 32 blocks of any size) */
    return (uint64_t)(int64_t)(int32_t)(0xC0000000u + (uint32_t)index * BLOCK_SIZE);
}

static void tlb_init(void)
{
    asm volatile("mtc0 %0, $5; mtc0 $0, $6; nop; nop" :: "r"(PAGER_PAGEMASK));
    for (int i = 0; i < TLB_ENTRIES; i++)
    {
        set_entryhi(dummy_entryhi(i));
        set_entrylo(0, 0);
        asm volatile("mtc0 %0, $0; nop; nop; tlbwi; nop; nop" :: "r"(i));
    }
}

/* drop the mapping of a block, if the TLB holds one */
static void tlb_unmap(uint32_t block)
{
    const uint64_t hi = (uint64_t)(int64_t)(int32_t)(PAGED_BASE + block * BLOCK_SIZE);
    set_entryhi(hi);
    uint32_t index;
    asm volatile("nop; nop; tlbp; nop; nop; mfc0 %0, $0" : "=r"(index));
    if (index & C0_INDEX_PROBE_FAILED)
        return;
    set_entryhi(dummy_entryhi(index & 31));
    set_entrylo(0, 0);
    asm volatile("tlbwi; nop; nop" ::: "memory");
}

/* --------------------------------------------------------------- ROM read */
static void rom_read(void *dst, uint32_t pi, uint32_t size)
{
    /* A transfer libdragon started is let finish; its completion interrupt
     * must reach libdragon, or the thread waiting for it waits forever.
     * Whether there is one is decided by the PI being busy when we came
     * in, not only by the interrupt flag: the flag can lag behind the end
     * of the transfer, and a flag cleared below in that window was a hang
     * while files were being read. The flag is also given time to settle
     * for a transfer that ended just before. */
    const int was_busy =
        (*PI_STATUS & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY)) != 0;
    while (*PI_STATUS & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY)) {}
    wait_ticks(32);                 /* < 1 us (46.875 MHz ticks) */
    const int libdragon_pending = was_busy ||
        (*MI_INTERRUPT & MI_INTERRUPT_PI) != 0;
    data_cache_hit_writeback_invalidate(dst, size);
    *PI_DRAM_ADDR = PhysicalAddr(dst);
    *PI_CART_ADDR = pi;
    *PI_WR_LEN = size - 1;
    while (*PI_STATUS & (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY)) {}
    if (!libdragon_pending)
        *PI_STATUS = PI_CLEAR_INTERRUPT;
}

/* --------------------------------------------------------------- faults -- */
static void load_block(uint32_t block)
{
    const uint32_t f = g_next;
    g_next = (g_next + 1) % g_nframes;
    if (g_frame_block[f] >= 0)
    {
        __pager_table[g_frame_block[f]] = 0;
        tlb_unmap((uint32_t)g_frame_block[f]);
    }
    uint8_t *frame = g_frames + f * BLOCK_SIZE;
    const uint32_t offset = block * BLOCK_SIZE;
    uint32_t size = g_rom_size - offset;
    if (size > BLOCK_SIZE)
        size = BLOCK_SIZE;
    size = (size + 7) & ~7u;
    rom_read(frame, g_rom + offset, size);
    /* the instruction cache is indexed by virtual address: whatever it holds
     * for this block's addresses belongs to the block's previous frame */
    inst_cache_invalidate_all();
    g_frame_block[f] = (int32_t)block;
    __pager_table[block] = ((PhysicalAddr(frame) >> 12) << 6) | ENTRYLO_FLAGS;
    g_faults++;
}

static void pager_exception(exception_t *ex)
{
    const uint32_t code = ex->code;
    if (code == EXCEPTION_CODE_TLB_LOAD_I_MISS ||
        code == EXCEPTION_CODE_TLB_STORE_MISS)
    {
        const uint32_t va = (uint32_t)C0_BADVADDR();
        const uint32_t block = (va - PAGED_BASE) >> BLOCK_SHIFT;
        if (va >= PAGED_BASE && block < __pager_nblocks)
        {
            if (!__pager_table[block])
                load_block(block);
            set_entryhi((uint64_t)(int64_t)(int32_t)(PAGED_BASE + block * BLOCK_SIZE));
            set_entrylo(__pager_table[block],
                        __pager_table[block] + PAGER_ENTRYLO_STEP);
            tlb_write_random();
            return;
        }
    }
    g_prev_handler(ex);
}

uint32_t stk_n64_pager_faults(void) { return g_faults; }

/* ------------------------------------------------------------------ init --
 * Runs before every other constructor (constructors of paged code are
 * paged), and before libdragon's DMA setup: the ROM's table of contents is
 * read with uncached CPU reads. Format: rompak.c. */
/* one 32-bit read from the cartridge. The PI bus only answers whole words
 * reliably, and GCC turns "volatile word & 0xFFFF" into a halfword load:
 * the load is written out. */
static inline uint32_t rom_word(uint32_t kseg1_addr)
{
    uint32_t v;
    asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(kseg1_addr) : "memory");
    return v;
}

static int find_rom_file(const char *ext, uint32_t *addr, uint32_t *size)
{
    for (int i = 0; i < 1024; i++)
    {
        const uint32_t toc = 0xB0001000u + i * 16;
        if (rom_word(toc) != 0x544F4330)               /* "TOC0" */
            continue;
        const uint32_t sizes = rom_word(toc + 12);
        const uint32_t entry_size = sizes >> 16, entries = sizes & 0xFFFF;
        for (uint32_t e = 0; e < entries; e++)
        {
            const uint32_t ent = toc + 16 + e * entry_size;
            char name[64];
            uint32_t k = 0;
            for (; k + 8 < entry_size && k + 4 <= sizeof(name) - 1; k += 4)
            {
                const uint32_t w = rom_word(ent + 8 + k);
                name[k] = w >> 24; name[k + 1] = w >> 16;
                name[k + 2] = w >> 8; name[k + 3] = w;
            }
            name[k] = 0;
            const size_t n = strlen(name), x = strlen(ext);
            if (n >= x && strcmp(name + n - x, ext) == 0)
            {
                *addr = 0x10000000u + rom_word(ent);
                *size = rom_word(ent + 4);
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Priority 1: before everything, libstdc++'s own (its iostreams use 90, and
 * libstdc++ is paged). Priorities below 101 are reserved to the
 * implementation, which the pager is part of. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
__attribute__((constructor(1)))
static void pager_init(void)
{
    const uint32_t linked = (uint32_t)(__paged_end - __paged_start);
    if (!linked)
        return;                         /* nothing paged in this build */
    assertf((uint32_t)__paged_start == PAGED_BASE,
            "pager: paged code linked at %p, expected %08lx",
            __paged_start, (unsigned long)PAGED_BASE);
    assertf(find_rom_file(".paged", &g_rom, &g_rom_size),
            "pager: no .paged file on the cartridge (n64/make_rom.sh)");
    assertf(g_rom_size >= linked, "pager: stk.paged is %lu bytes, the "
            "program expects %lu", (unsigned long)g_rom_size,
            (unsigned long)linked);
    g_rom_size = linked;
    __pager_nblocks = (linked + BLOCK_SIZE - 1) >> BLOCK_SHIFT;
    __pager_table = calloc(__pager_nblocks, sizeof(uint32_t));
    g_nframes = STK_N64_PAGER_FRAMES;
    g_frames = memalign(1 << PAGER_PAGE_SHIFT, g_nframes * BLOCK_SIZE);
    g_frame_block = malloc(g_nframes * sizeof(int32_t));
    assertf(__pager_table && g_frames && g_frame_block, "pager: out of memory");
    for (uint32_t f = 0; f < g_nframes; f++)
        g_frame_block[f] = -1;

    tlb_init();
    g_prev_handler = register_exception_handler(pager_exception);

    /* the 64-bit TLB refill vector (libdragon runs with KX set) */
    volatile uint32_t *vec = (volatile uint32_t*)0x80000080;
    vec[0] = 0x08000000u | (((uint32_t)__pager_refill >> 2) & 0x03FFFFFFu);  /* j */
    vec[1] = 0;                                                               /* nop */
    data_cache_hit_writeback((void*)vec, 8);
    inst_cache_hit_invalidate((void*)vec, 8);
}
#pragma GCC diagnostic pop
