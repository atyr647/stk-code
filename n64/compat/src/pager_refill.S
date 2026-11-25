# TLB refill for paged code (see pager.c). Reached from the 64-bit refill
# vector (0x80000080), with only $26/$27 free and EntryHi already holding the
# faulting address's VPN2. A block that is in a frame is mapped here; any
# other refill goes to libdragon's exception handler, and from there to
# pager_exception() or a crash report.

# CP0 registers: $8 BadVAddr, $2 EntryLo0, $3 EntryLo1

#include "pager.h"

    .set noreorder
    .set noat
    .section .text.__pager_refill, "ax"
    .global __pager_refill
    .p2align 5
__pager_refill:
    mfc0    $26, $8
    lui     $27, PAGED_BASE_HI
    subu    $26, $26, $27
    srl     $26, $26, BLOCK_SHIFT         # block
    lui     $27, %hi(__pager_nblocks)
    lw      $27, %lo(__pager_nblocks)($27)
    sltu    $27, $26, $27
    beqz    $27, 1f
    sll     $26, $26, 2
    lui     $27, %hi(__pager_table)
    lw      $27, %lo(__pager_table)($27)
    addu    $27, $27, $26
    lw      $27, 0($27)                   # EntryLo0, 0 = not in a frame
    beqz    $27, 1f
    nop
    mtc0    $27, $2
    addiu   $27, $27, PAGER_ENTRYLO_STEP  # the frame's second page
    mtc0    $27, $3
    nop
    nop
    tlbwr
    nop
    nop
    eret
1:
    j       inthandler
    nop
