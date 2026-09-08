/* aarch64 kernel image layout -- docs/ARM64.md phase A0.
 *
 * PHYSICAL addresses only, deliberately. The MMU is off through A0/A1 and the
 * higher-half move to TTBR1_EL1 is A2 (§2.7); pretending otherwise now would
 * mean a link-time fiction that nothing checks.
 *
 * 0x4008_0000 is not arbitrary: QEMU `-M virt` puts DRAM at 0x4000_0000, and
 * the AArch64 Linux boot convention places the image 0x80000 into RAM. Staying
 * on that convention keeps the low 512 KiB free for the boot stub QEMU plants
 * (the one that loads x0 with the DTB pointer -- see boot.S) and for the DTB
 * itself, and it is where every ARM debugging tutorial expects to find a kernel.
 */

ENTRY(_start)

KERNEL_PHYS_BASE = 0x40080000;

SECTIONS {
    . = KERNEL_PHYS_BASE;

    /* .text.boot first and KEEP'd: _start must be the LOWEST address in the
     * image. QEMU honours the ELF entry point so this is belt-and-braces
     * today, but the raw-Image path (`objcopy -O binary`) has no entry point
     * at all and enters at offset 0 -- and that is the path a real board takes. */
    .text ALIGN(4096) : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : {
        *(.data .data.*)
    }

    .bss ALIGN(4096) : {
        /* 8-byte aligned at both ends: boot.S zeroes this range with 64-bit
         * stores and does no tail handling. */
        . = ALIGN(8);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(4096);
    kernel_end = .;

    /* Toolchain metadata that would otherwise land in an allocated segment and
     * be loaded into RAM for nothing. */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
