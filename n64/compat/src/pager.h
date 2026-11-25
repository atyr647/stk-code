/* Block geometry of the code pager, shared by pager.c and pager_refill.S.
 *
 * A block is one TLB entry: an even/odd pair of pages, backed by one frame
 * of the pool. Small blocks waste less RAM on the cold code and constants
 * that sit next to hot ones (a menu frame touches a few hundred scattered
 * functions), at the cost of more TLB refills, which the refill vector
 * serves in a few dozen cycles. */
#ifndef STK_N64_PAGER_H
#define STK_N64_PAGER_H

#define PAGED_BASE      0xC0800000
#define PAGED_BASE_HI   0xC080          /* lui of PAGED_BASE */

#ifndef PAGER_PAGE_SHIFT
#define PAGER_PAGE_SHIFT 12             /* 4 KB pages */
#endif

#if PAGER_PAGE_SHIFT == 12
#define PAGER_PAGEMASK  0x00000000
#elif PAGER_PAGE_SHIFT == 14
#define PAGER_PAGEMASK  0x00006000
#else
#error "pager.h: page size must be 4 KB or 16 KB"
#endif

#define BLOCK_SHIFT     (PAGER_PAGE_SHIFT + 1)       /* two pages */
#define BLOCK_SIZE      (1 << BLOCK_SHIFT)
/* EntryLo of the odd page = EntryLo of the even page + this (PFN field) */
#define PAGER_ENTRYLO_STEP ((1 << (PAGER_PAGE_SHIFT - 12)) << 6)

#endif
