/* Embeds the stripped kernel ELF into the UEFI loader image so the loader is a
 * single self-contained BOOTX64.EFI (no file-protocol code for the MVP). The
 * path arrives as -DKERNEL_BLOB_PATH='"kernel/kernel.strip.elf"'; compiled with
 * gcc so the C preprocessor expands it before the assembler sees .incbin.
 *
 * .rodata so the linker script folds it into the allocated .data output section
 * -- that is what carries the ~700 KB blob through the ELF->PE objcopy. */
    .section .rodata
    .balign 8
    /* hidden: non-interposable, so the loader references these with a direct
     * RIP-relative lea instead of an absolute address that would be stuck at the
     * link-time (base-0) value once firmware loads the image at a real base. */
    .globl  kernel_elf_start
    .hidden kernel_elf_start
    .globl  kernel_elf_end
    .hidden kernel_elf_end
kernel_elf_start:
    .incbin KERNEL_BLOB_PATH
kernel_elf_end:
