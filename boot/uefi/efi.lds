/* Linker script for the EmbLink UEFI loader.
 *
 * Lays a PIE (-shared) out so `objcopy -O pei-x86-64` produces a clean EFI
 * application. The .rela relocation entries are placed INSIDE an allocated
 * section (.data) so they survive the ELF->PE conversion -- crt0.S walks them
 * at runtime between __rela_start and __rela_end. .reloc (the dummy PE fixup
 * from crt0.S) is kept so firmware treats the image as relocatable.
 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = 0;
    __image_base = .;       /* RVA 0 -- the image base itself */

    /* Leave RVA 0..0xFFF for the PE/DOS headers objcopy prepends. A section at
     * RVA 0 would overlap them (malformed) and drop the entry point to 0, which
     * firmware rejects with "Unsupported". First real section starts at 0x1000. */
    . = 0x1000;

    .text : {
        *(.text)
        *(.text.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.rodata) *(.rodata.*)
        *(.data) *(.data.*)
        *(.sdata) *(.sdata.*)
        *(.got.plt) *(.got) *(.igot.*)
        *(.dynamic)

        __rela_start = .;
        *(.rela) *(.rela.*)
        __rela_end = .;

        *(.dynsym) *(.dynstr) *(.gnu.hash) *(.hash)

        *(.bss) *(.bss.*) *(COMMON)
    }

    . = ALIGN(4096);
    .reloc : { *(.reloc) }

    /DISCARD/ : {
        *(.comment)
        *(.note) *(.note.*)
        *(.eh_frame) *(.eh_frame_hdr)
        *(.interp)
    }
}
