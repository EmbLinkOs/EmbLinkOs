/* user/lib/newlib-body.ld -- the SECTIONS body shared by newlib.ld (a fixed-
 * address ET_EXEC) and newlib-pie.ld (a position-independent ET_DYN).
 *
 * WHY THIS FILE EXISTS. The two link modes differ in exactly three things: the
 * start address, whether PHDRS declares a PT_DYNAMIC, and the handful of
 * read-only sections a dynamic image carries (.hash, .dynsym, .rela.dyn, ...).
 * Everything else -- where .text goes, how the W^X page boundary is drawn, the
 * TLS template and its geometry symbols, BOTH constructor schemes, the .bss
 * brackets -- is identical and must STAY identical, because an app built one
 * way and an app built the other way are the same program.
 *
 * Two copies of this list would drift, and the drift would be invisible: a new
 * section added to one script and not the other does not fail the link, it
 * silently drops the section (a constructor that never runs, a .rodata that
 * lands in the writable segment). One copy, included twice.
 *
 * INCLUDEd from inside SECTIONS, so it starts at .text and the caller owns the
 * location counter before it and the /DISCARD/ after it. */

    .text : {
        *(.text.start)
        *(.text .text.* .gnu.linkonce.t.*)
    } :text

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    } :text

    . = ALIGN(0x1000);       /* new page: writable data below, W^X boundary */

    /* --- Thread-local storage ------------------------------------------------
     * .tdata is the initialised TLS image (real bytes in the file); .tbss is the
     * zero-filled remainder (NOBITS -- it occupies no file space and, for TLS,
     * no address space either: whatever section follows starts at .tbss's own
     * address). Both belong to :data AND :tls -- the data segment carries the
     * template bytes, PT_TLS describes them.
     *
     * crt0 needs the geometry to build a thread's block, and it cannot read the
     * program headers (no auxv on this OS), so the linker hands it over through
     * these symbols. SIZEOF()/ALIGNOF() are used deliberately instead of
     * differencing the location counter around .tbss: `.` does NOT advance
     * across a NOBITS TLS section, so a `__tls_end - __tls_start` idiom would
     * silently compute a memsz that omits .tbss entirely -- and .tbss is exactly
     * where CPython's _Py_tss_tstate lives. */
    .tdata : {
        __tls_image = .;
        *(.tdata .tdata.* .gnu.linkonce.td.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
    } :data :tls

    /* THE GEOMETRY -- three numbers, stored as DATA rather than as linker
     * symbols, and that is the whole point.
     *
     * They used to be linker numbers: __tls_filesz = SIZEOF(.tdata) and so on,
     * which ld emits as SHN_ABS symbols. Correct for a binary linked at a fixed
     * address, and silently wrong for a position-independent one. PIC code
     * reaches a symbol with a PC-relative `lea`, so at run time the program
     * computes load_bias + value -- right for an ADDRESS, nonsense for a SIZE.
     * The TLS block would be allocated with a memsz of "sixteen terabytes plus
     * eight", malloc would fail, and every __thread variable in the program
     * would quietly resolve against a block that was never built.
     *
     * Making them addresses instead (__tls_image + size, subtract to recover)
     * ALMOST works and is worse than either: two of the three come out SHN_ABS
     * anyway, because an expression mixing ADDR() of two different sections has
     * no single section to belong to -- so the pair would be biased
     * inconsistently and the difference would be garbage, on some links and not
     * others. A rule that holds by accident is not a rule.
     *
     * As data there is nothing to bias. __tls_geom is an address, and an
     * address is exactly what a load bias is FOR; the three quadwords it points
     * at are link-time constants that no relocation touches. Both link modes
     * read the same three numbers the same way. */
    .embk_tls_geom : {
        __tls_geom = .;
        QUAD(SIZEOF(.tdata));                                  /* [0] filesz */
        QUAD(ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata));      /* [1] memsz  */
        QUAD(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));            /* [2] align  */
    } :data

    /* [2] IS PT_TLS's p_align, EXACTLY -- not "at least". crt0 must round memsz
     * up with the same value ld used, because a variable's address is
     * TP - ALIGN(memsz, align) + offset; disagree and every TLS variable lands
     * at the wrong address. MAX() mirrors how ld derives the segment's
     * alignment from its sections, which is where ld gets p_align too.
     *
     * THERE USED TO BE A FLOOR OF 8 HERE, "for the degenerate case where a
     * section is empty and reports align 1", and it was wrong -- not for the
     * degenerate case, for the ordinary one. A single `__thread int` gives
     * ALIGNOF 4, so ld writes p_align 4 and puts the variable at
     * TP - align(4,4) == TP-4, while the floor told crt0 to build the block at
     * TP-8. The program then reads four bytes of the zero tail instead of its
     * own initialiser -- no fault, no diagnostic, the variable is simply always
     * zero. It survived because the only program in the tree with TLS has a
     * 32-byte-aligned variable, where the floor changes nothing. pieprobe.c
     * checks the VALUE of a __thread int rather than only that it is
     * addressable, which is what caught it.
     *
     * The degenerate case needs no floor: two empty sections give memsz 0, and
     * crt0 returns before it looks at the alignment. */

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
    } :data

    /* Static constructors. crt0's run_init_array()/run_ctors() walk these
     * before main -- C++ global constructors and C's
     * __attribute__((constructor)) both land here.
     *
     * BOTH schemes are collected on purpose. GCC picks between them at ITS
     * configure time (--enable-initfini-array): the stock /usr/local/cross
     * C compiler was built --without-headers for bare metal and emits the
     * LEGACY .ctors, while a newlib-aware build emits the modern
     * .init_array. Collecting only .init_array is why a constructor silently
     * never ran -- the entry sat in an uncollected .ctors and the
     * __init_array brackets came out empty (start == end). Taking both means
     * constructors work whichever toolchain compiled the object. */
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data

    /* Legacy GNU scheme. No crtbegin/crtend here (-nostartfiles), so there
     * are no -1/0 sentinels -- just the raw entries; crt0 walks them
     * BACKWARD, which is the order this format expects. */
    .ctors : {
        __ctors_start = .;
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
        __ctors_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        __bss_end = .;
    } :data
