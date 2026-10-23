/* user/lib/newlib-pie.ld — the same program, linked POSITION-INDEPENDENT.
 *
 * A binary linked with this script is ET_DYN: every address in it is written
 * relative to zero, and the absolute ones the compiler could not fold into a
 * PC-relative instruction are listed in .rela.dyn for the loader to fix up.
 * kernel/loader/elf.c loads it at a bias the process drew from the kernel's
 * CSPRNG, so an app's text is at a different address in every process — the
 * last fixed address in a user address space, and the one a ROP chain is built
 * out of.
 *
 * THREE DIFFERENCES from newlib.ld, and only three:
 *
 * 1. `. = 0x0` instead of 0x400000. The image is linked at zero; the bias is
 *    added at load time.
 *
 * 2. PHDRS declares a PT_DYNAMIC. It has to be declared explicitly: with an
 *    explicit PHDRS block ld emits ONLY the program headers named here and
 *    creates no PT_DYNAMIC of its own — and without PT_DYNAMIC the loader has
 *    no way to find .rela.dyn, so nothing would be relocated and the program
 *    would run with every absolute pointer wrong by the bias. (Declaring it in
 *    newlib.ld too is why the scripts are separate: ld then emits an EMPTY
 *    PT_DYNAMIC into every static binary, which readelf rejects outright.)
 *
 * 3. The dynamic-image sections below. They are placed in the READ-ONLY text
 *    segment, .got and .dynamic included, which is RELRO by construction: the
 *    only writer is the kernel, and the kernel writes relocations through its
 *    own direct map of each frame rather than through the user mapping, so the
 *    user-visible permission can stay read-only. There is no lazy binding here
 *    for the program itself to write a PLT slot back into.
 *
 * The section list is newlib-body.ld, byte-identical to the one the fixed-
 * address link uses. */
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS(5);   /* R + X, no write */
    data    PT_LOAD    FLAGS(6);   /* R + W, no execute */
    tls     PT_TLS     FLAGS(4);   /* the TLS template — see newlib.ld */
    dynamic PT_DYNAMIC FLAGS(4);   /* R: the loader reads it, nobody writes it */
}

SECTIONS
{
    . = 0x0;

    /* --hash-style=sysv: DT_HASH, which is the one hash table the kernel's
     * loader reads (it takes nchain as the dynamic symbol count). */
    .hash     : { *(.hash) }                          :text
    .dynsym   : { *(.dynsym) }                        :text
    .dynstr   : { *(.dynstr) }                        :text
    .rela.dyn : { *(.rela.dyn) }                      :text
    .rela.plt : { *(.rela.plt) }                      :text
    .plt      : { *(.plt) *(.iplt) }                  :text
    .dynamic  : { *(.dynamic) }                       :text :dynamic
    .got      : { *(.got) *(.got.plt) *(.igot) *(.igot.plt) } :text

INCLUDE newlib-body.ld

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame_hdr) }
}
