/* user/lib/newlib.ld — link script for newlib-linked EmbLink programs (crt0.c +
 * syscalls.c + the program + libc.a), linked at a FIXED address.
 *
 * Same 0x400000 load address and W^X two-segment split as user.ld, but with the
 * sections a full C library actually emits: .rodata (string literals, format
 * strings), the C++/ctor arrays, and a real .bss the kernel's ELF loader
 * zero-fills. The section list itself lives in newlib-body.ld, shared with the
 * position-independent variant — see newlib-pie.ld, and the note at the top of
 * the body about why it is one file and not two. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X, no write */
    data PT_LOAD FLAGS(6);   /* R + W, no execute */
    /* PT_TLS describes the TLS TEMPLATE. It is not separately loaded: it covers
     * the same bytes as the data segment, and each thread gets its own COPY made
     * at runtime (crt0). Declaring it is mandatory here -- once a script has an
     * explicit PHDRS block, ld emits ONLY the program headers it names, so
     * without this line .tdata/.tbss become orphan sections with no PT_TLS and
     * every %fs access faults. */
    tls  PT_TLS  FLAGS(4);   /* R */
}

SECTIONS
{
    . = 0x400000;

INCLUDE newlib-body.ld

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame_hdr) }
}
