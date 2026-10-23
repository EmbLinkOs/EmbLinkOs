ENTRY(_start)


KERNEL_VIRTUAL_BASE = 0XFFFFFFFF80000000;
KERNEL_PHYSICAL_BASE = 0X100000;


SECTIONS {
    . = KERNEL_VIRTUAL_BASE + KERNEL_PHYSICAL_BASE; /* Start of the kernel in memory at 1MB */

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) {
        *(.text) /* All .text sections from input files */
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) {
        *(.rodata) /* All .rodata sections from input files */

        /* WHAT THE KERNEL LETS A LOADABLE MODULE CALL.
         *
         * Gathered by the linker rather than written out as a list, so adding
         * an export is one EMBK_EXPORT() next to the function it exports and
         * not an edit in a second file that somebody forgets. The start/end
         * symbols are how module.c finds the table; KEEP because nothing
         * REFERENCES these entries and --gc-sections would otherwise be right
         * to remove every one of them. */
        . = ALIGN(8);
        __embk_exports_start = .;
        KEEP(*(.embk_exports))
        __embk_exports_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE){
        *(.data) /* All .data sections from input files */
    }

    .bss ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE){
    *(.bss)
    *(COMMON)
}

    /* Mark the end of the kernel in memory */
    . = ALIGN(4096);
    kernel_end = .;

}