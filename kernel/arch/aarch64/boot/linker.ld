/* aarch64 kernel image layout -- docs/ARM64.md phases A0-A2.
 *
 * The kernel is LINKED at its virtual address and LOADED at its physical one.
 * That is the same split kernel/linker.ld uses on x86 (`AT(ADDR(.x) -
 * KERNEL_VIRTUAL_BASE)`), and the same virtual base, deliberately: the address
 * space layout in kernel/mm/pmm.h -- kernel window, direct map, MMIO window --
 * is architecture-NEUTRAL, and aarch64 adopting it unchanged is what lets
 * kernel/mm/pmm.c compile for both without a line of #ifdef.
 *
 *   KERNEL_VIRTUAL_BASE  0xFFFFFFFF80000000   VA = PA + base
 *   DIRECT_MAP_BASE      0xFFFF800000000000   all of physical memory
 *   MMIO_BASE            0xFFFFC00000000000   device space
 *
 * On aarch64 that split is not a convention but a HARDWARE property: TTBR1_EL1
 * translates the top of the address space and TTBR0_EL1 the bottom, so "is
 * this a kernel address" is decided by the CPU, and switching to a user
 * address space touches only TTBR0.
 *
 * 0x4008_0000 is not arbitrary: QEMU `-M virt` puts DRAM at 0x4000_0000, and
 * the AArch64 Linux boot convention places the image 0x80000 into RAM. Staying
 * on that convention keeps the low 512 KiB free for the boot stub QEMU plants
 * (the one that loads x0 with the DTB pointer -- see boot.S) and for the DTB
 * itself, and it is where every ARM debugging tool expects to find a kernel.
 *
 * CONSEQUENCE, and it is the thing most likely to bite: everything in
 * .text.boot runs BEFORE the MMU, at the physical address, while every symbol
 * it can name is a virtual one. Only PC-relative references (adrp/add, which
 * is what -mcmodel=small emits) are correct there. An absolute address loaded
 * from .rodata is not. boot.S is written to that rule and says so; nothing
 * else runs early enough to care.
 */

ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x40080000;

SECTIONS {
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;

    __kernel_start = .;

    /* .text.boot first and KEEP'd: _start must be at offset 0 of the flat
     * image, because the arm64 Image path has no entry point and simply
     * begins executing at the first byte. */
    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
        __text_start = .;
        KEEP(*(.text.boot))

        /* The exception vector table gets its own explicit 2 KiB alignment
         * rather than trusting `.balign 2048` inside the input section to
         * survive placement. VBAR_EL1 discards the low 11 bits of what it is
         * given, so a misaligned table does not fail -- it dispatches every
         * exception to an address a little way off, which looks like the
         * handler being subtly, randomly wrong. */
        . = ALIGN(2048);
        KEEP(*(.text.vectors))

        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4096);
        __data_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        /* PAGE aligned at both ends, not merely 8. Two callers need it:
         * boot.S zeroes this range with 64-bit stores and does no tail
         * handling, and mm/pagetable.c re-maps it as whole 4 KiB pages when it
         * applies per-section permissions -- an unaligned end there is a hard
         * error rather than a rounding question, deliberately, because
         * "protect a section but not its last few bytes" is not a thing
         * anybody means. */
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }

    . = ALIGN(4096);
    kernel_end = .;
    __kernel_end = .;

    /* Toolchain metadata that would otherwise land in an allocated segment and
     * be loaded into RAM for nothing. */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }

    /* The two things about the vector table that cannot be checked in the
     * assembler (see irq/vectors.S) and must not be discovered at runtime. */
    ASSERT(aarch64_vectors_end - aarch64_vectors == 0x800,
           "aarch64 vector table is not 2 KiB: an EXC_ENTRY stub overflowed its 128-byte slot")
    ASSERT((aarch64_vectors & 0x7FF) == 0,
           "aarch64 vector table is not 2 KiB aligned: VBAR_EL1 would silently truncate it")

    /* The kernel window maps VA = PA + KERNEL_VIRT_BASE, and that base leaves
     * exactly 2 GiB of physical address space below it. A kernel image linked
     * past that would wrap, and the failure would be a wild jump rather than a
     * link error. Checked here so it stays a link error. */
    ASSERT(KERNEL_PHYS_BASE < 0x80000000,
           "KERNEL_PHYS_BASE must be below 2 GiB: the kernel window is PA + 0xFFFFFFFF80000000")
}
