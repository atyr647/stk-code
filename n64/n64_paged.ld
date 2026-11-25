/* n64_paged.ld -- libdragon's n64.ld (preview, see n64/build_libdragon.sh)
 * with code paging (n64/compat/src/pager.c):
 *   - .text holds only what must stay resident: libdragon, newlib, libgcc,
 *     n64/compat, the platform's entry and diagnostics files, and the
 *     libstdc++ exception support libdragon's crash reporting calls;
 *   - every other code input goes to .paged, linked at 0xC0800000 (TLB
 *     mapped) and stored on the cartridge as stk.paged by n64/make_rom.sh,
 *     with its constants (.rodata) and C++ exception tables: read-only
 *     data pages in the same way, through the TLB.
 * Everything else is unchanged from n64.ld.
 */
/* ========================================================================
 *
 * n64.ld -- Libdragon linker script
 *
 * Originally based on n64ld.x, developed by Frank Somers <frank@g0dev.com>,
 * with modifications by hcs (halleyscometsoftware@hotmail.com),
 * released under the Unlicense.
 * ========================================================================
 */

OUTPUT_FORMAT ("elf32-bigmips", "elf32-bigmips", "elf32-littlemips")
OUTPUT_ARCH (mips)
EXTERN (_start)
ENTRY (_start)

PHDRS
{
    irq PT_LOAD AT ( 0x80000000 );
    main PT_LOAD AT ( 0x80000400 );
    paged PT_LOAD;
}

SECTIONS {
    .intvectors 0x80000000 : {
        . = ALIGN(32);
        KEEP(*(.intvectors))
        __intvectors_end = .;
    } :irq

    /* The text section carries the app code and its relocation addr is
    * the first byte of the cart domain in cached, unmapped memory
    */
    .text 0x80000400 : {
        __text_start = .;
        *(.boot)
        . = ALIGN(16);
        *libdragon.a:*(.text .text.* .gnu.linkonce.t.*)
        *libdragonsys.a:*(.text .text.* .gnu.linkonce.t.*)
        *libc.a:*(.text .text.* .gnu.linkonce.t.*)
        *libm.a:*(.text .text.* .gnu.linkonce.t.*)
        *libg.a:*(.text .text.* .gnu.linkonce.t.*)
        *libgcc.a:*(.text .text.* .gnu.linkonce.t.*)
        *libstk_n64_compat.a:*(.text .text.* .gnu.linkonce.t.*)
        *n64_main.cpp.obj(.text .text.* .gnu.linkonce.t.*)
        *n64_diag.cpp.obj(.text .text.* .gnu.linkonce.t.*)
        *crt*.o(.text .text.* .gnu.linkonce.t.*)
        /* libdragon's C++ crash reporting (debugcpp.cpp) calls these */
        *libstdc++.a:eh_*.o(.text .text.* .gnu.linkonce.t.*)
        *libstdc++.a:cp-demangle.o(.text .text.* .gnu.linkonce.t.*)
        *libstdc++.a:vterminate.o(.text .text.* .gnu.linkonce.t.*)
        KEEP(*(keep.text.*))
        *(.init)
        *(.fini)
        . = ALIGN(16);
        __text_end  = .;
    } :main

   .eh_frame_hdr : { *(.eh_frame_hdr) }
   .eh_frame : { 
        __EH_FRAME_BEGIN__ = .; /* Define symbol for accessing eh_frame section */
        KEEP (*(.eh_frame)) 
    }
   .jcr : { KEEP (*(.jcr)) } 

    /* constants of the resident code only: the rest are paged with their
       code (below). What runs with EXL set must not touch paged memory. */
    .rodata : {
        *libdragon.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libdragonsys.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libc.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libm.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libg.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libgcc.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libstk_n64_compat.a:*(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *n64_main.cpp.obj(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *n64_diag.cpp.obj(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *crt*.o(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libstdc++.a:eh_*.o(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libstdc++.a:cp-demangle.o(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *libstdc++.a:vterminate.o(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        KEEP(*(keep.rodata.*))
        . = ALIGN(4);
        __tdata_align = .;
        LONG (ALIGNOF(.tdata));
        . = ALIGN(8);
    }

    /* This is important to keep __CTOR_END__ consistent */
    . = ALIGN(4);

    .ctors : {
        __CTOR_LIST__ = .;
        /* Actually we should place the count here like;

        LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)

        but g++ seem to already do this (though incorrectly placing an 0xFFFFFFFF)
        and that breaks things if we also add it. Ideally this should be parsed
        by the code but we already know the end as well.

        The new build system links everything via g++ by default and should use
        __wrap___do_global_ctors and enables it via the --wrap linker option.
        When linking with ld, we should use __do_global_ctors, which is the default
        if you don't provide the --wrap option. */
        KEEP(*(EXCLUDE_FILE (*crtend.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*crtend.o(.ctors))
         /* Similarly we should have a;

        LONG(0)

        here, and g++ seem to insert it at __CTOR_END__ */
        __CTOR_END__ = .;
    }

    . = ALIGN(8);

    /* Data section has relocation address at start of RAM in cached,
    * unmapped memory, but is loaded just at the end of the text segment,
    * and must be copied to the correct location at startup
    * Gather all initialised data together.  The memory layout
    * will place the global initialised data at the lowest addrs.
    * The lit8, lit4, sdata and sbss sections have to be placed
    * together in that order from low to high addrs with the _gp symbol
    * positioned (aligned) at the start of the sdata section.
    * We then finish off with the standard bss section
    */
    .data : {
        __data_start = .;
        *(.data)
        *(.data.*)
        KEEP(*(keep.data.*))
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
    }
    
    .tdata : {
        __tls_base = .;
        __tdata_start = .;
        *(.tdata)
        *(.tdata.*)
        *(.gnu.linkonce.td.*)
        __tdata_end = .;
    }
    
    .tbss : {
        __tbss_start = .;
        *(.tbss)
        *(.tbss.*)
        *(.gnu.linkonce.tb.*)
        . = ALIGN(8);
        __tbss_end = .;
        __tls_end = .;
    }
    
    /* Fix dot for TBSS Sections */
    . = .+(__tbss_end-__tbss_start);
    
    /* Small data START */

    .sdata : {
        _gp = . + 0x8000;
        *(.sdata)
        *(.sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
    }

    .lit8 : {
        *(.lit8)
        . = ALIGN(8);
    }
    .lit4 : {
        *(.lit4)
        . = ALIGN(8);
    }

    . = ALIGN(8);
    __data_end = .;

    /* Here the ROM is finished. The rest is just in RAM */
    . = ALIGN(8);
    __rom_end = .;

    .sbss : {
         __bss_start = .;
        *(.sbss)
        *(.sbss.*)
        *(.gnu.linkonce.sb.*)
        *(.scommon)
        *(.scommon.*)
    }

    /* Small data END */

    . = ALIGN(8);
    .bss : {
        *(.bss)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        __th_tdata_copy = .;
        . = .+(__tdata_end-__tdata_start);
        . = ALIGN(8);
         __bss_end = .;
    }

    . = ALIGN(8);

    /* Deprecated */
    end = .;

    /* dynamic relocations nothing applies in a static program; left
       unplaced they would land after .paged, in mapped space */
    /DISCARD/ : { *(.rel.dyn) }

    /* paged code: not loaded at boot, see n64/compat/src/pager.c */
    .paged 0xC0800000 : {
        __paged_start = .;
        *(.text .text.* .gnu.linkonce.t.*)
        /* read-only data of paged code: constants, and the C++ exception
           tables, which the (resident) personality routine reads while an
           exception unwinds, in normal context */
        . = ALIGN(16);
        *(.rdata .rodata .rodata.* .gnu.linkonce.r.*)
        *(.gcc_except_table*)
        . = ALIGN(16);
        __paged_end = .;
    } :paged
}

/* Give an assert for a too huge binary. The maximum allowed size here is 8 MiB,
   minus the space for the stack and the initial low memory section. 
   Notice the exact size doesn't matter much: we want to detect mistakes that
   would be much harder to find at runtime when no heap is available... */
ASSERT(end - __text_start <= 8*1024*1024 - 64*1024 - 1024, "ERROR: the generated ELF is too big for N64 memory")
