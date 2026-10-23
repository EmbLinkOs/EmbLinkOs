/* Keep ELF dynamic metadata in separate output sections. Merging .dynstr,
 * .dynsym or .dynamic into .data makes the linker overwrite ordinary data
 * (including compiler jump tables) with generated metadata. The PE conversion
 * carries only the runtime code/data, GOT and relative relocations. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)
SECTIONS
{
    . = 0;
    __image_base = .;
    . = 0x1000; /* reserve PE headers */
    .text : { *(.text) *(.text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata) *(.rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) }
    . = ALIGN(4096);
    .got : { *(.got.plt) *(.got) *(.igot.*) }
    .dynamic : { *(.dynamic) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .gnu.hash : { *(.gnu.hash) }
    .hash : { *(.hash) }
    . = ALIGN(4096);
    .rela : {
        __rela_start = .;
        *(.rela) *(.rela.*)
        __rela_end = .;
    }
    . = ALIGN(4096);
    .bss : { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(4096);
    .reloc : { *(.reloc) }
    /DISCARD/ : {
        *(.comment) *(.note) *(.note.*)
        *(.eh_frame) *(.eh_frame_hdr) *(.interp)
    }
}
