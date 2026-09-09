/* aarch64 reset path -- docs/ARM64.md phase A0.
 *
 * QEMU `-M virt -kernel <elf>` does NOT jump straight here. It plants a tiny
 * bootloader stub at the base of RAM which sets x0 to the physical address of
 * the device tree it generated and then branches to our ELF entry point. So
 * x0 is a real argument on arrival and must survive to arch_early_main() --
 * A2 reads the memory map out of that DTB. Every register we touch below is
 * chosen to leave x0 alone.
 *
 * The MMU is OFF here and stays off for the whole of A0/A1; we run at the
 * physical addresses the linker script assigns. The higher half is A2.
 */

/* ---------------------------------------------------------------------------
 * Constants and macros for the physical-mode window.
 * ------------------------------------------------------------------------- */

/* Address-space layout. These are kernel/mm/pmm.h's constants, repeated here
 * because the assembler cannot include a C header -- and asserted against it
 * from mm/pagetable.c, which CAN see both, so the duplication is checked
 * rather than trusted. */
    .set L0_IDX_KERNEL, 511         /* 0xFFFFFFFF80000000 >> 39 & 0x1FF */
    .set L0_IDX_DIRECT, 256         /* 0xFFFF800000000000 >> 39 & 0x1FF */
    .set L0_IDX_MMIO,   384         /* 0xFFFFC00000000000 >> 39 & 0x1FF */

/* Descriptor bits (Arm ARM D8.3). A table descriptor is valid+table; a BLOCK
 * descriptor at level 1 maps 1 GiB and is valid+!table. */
    .set PTE_TABLE_BITS,  3         /* VALID | TABLE                        */

/* AF (bit 10) is the one everybody forgets: with FEAT_HAFDBS off -- and it is
 * off unless enabled -- hardware does NOT set the access flag, it FAULTS. An
 * otherwise perfect descriptor without AF gives an "access flag fault" on
 * first touch, which reads like a permission bug and is not one. */
    .set BLOCK_DEVICE,     0x60000000000401   /* attr0, AF, PXN|UXN, SH=0    */
    .set BLOCK_RAM_EXEC,   0x40000000000705   /* attr1, AF, SH=inner, UXN    */
    .set BLOCK_RAM_NOEXEC, 0x60000000000705   /* ...and PXN                  */

/* MAIR_EL1: attr0 = 0x00 Device-nGnRnE, attr1 = 0xFF Normal write-back RWA,
 * attr2 = 0x44 Normal NON-CACHEABLE -- what vmm_map_mmio_wc() maps a
 * framebuffer aperture with. There is no "write-combining" memory type on
 * aarch64 the way x86's PAT has one; Normal non-cacheable is the equivalent,
 * and unlike Device memory it permits the unaligned and merged writes a
 * framebuffer blit depends on. */
    .set MAIR_VALUE, 0x44FF00

/* TCR_EL1: T0SZ = T1SZ = 16 (48-bit VAs both halves), 4 KiB granule both
 * halves (TG0=0b00, TG1=0b10 -- the two fields are encoded DIFFERENTLY, which
 * is a genuine trap), inner-shareable write-back cacheable walks. IPS is OR'd
 * in at runtime from ID_AA64MMFR0_EL1.PARange. */
    .set TCR_VALUE, 0xb5103510

/* SCTLR_EL1: M (MMU), C (data cache), I (instruction cache). */
    .set SCTLR_ENABLE, 0x1005

/* The physical address of a symbol, while running with the MMU off.
 *
 * adrp+add is PC-RELATIVE: it computes (symbol - instruction) at link time and
 * adds it to the real PC, so it yields wherever the image actually IS. That is
 * the physical address here, and it is why this window can name kernel symbols
 * at all despite them being linked high. `ldr \reg, =symbol` would give the
 * link-time virtual address instead -- which is exactly what is wanted after
 * the MMU is on, and exactly wrong before. */
.macro PHYS_OF reg, sym
    adrp    \reg, \sym
    add     \reg, \reg, :lo12:\sym
.endm

/* Emit one character on the PL011 at its physical address, clobbering only
 * x9/x10. Used solely as a boot beacon in the physical window (see at_el1).
 * ASCII codes rather than character literals: `#'X'` is not portable across
 * assembler syntaxes and this file cannot afford a surprise. */
.macro PUTC code
    mov     x9, #0x09000000
.Lputc_wait\@:
    ldr     w10, [x9, #0x18]            /* UARTFR */
    tbnz    w10, #5, .Lputc_wait\@      /* TXFF: transmit FIFO full */
    mov     w10, #\code
    str     w10, [x9]                   /* UARTDR */
.endm

    .section .text.boot, "ax"
    .globl _start
_start:
    /* --- the arm64 Linux `Image` header (Documentation/arch/arm64/booting.rst)
     *
     * Not decoration, and not Linux-compatibility theatre. It is the only way
     * to get the device tree pointer.
     *
     * MEASURED: booting the ELF directly with `-kernel kernel.elf` works --
     * the console comes up at EL1 -- but x0 arrives as ZERO. QEMU only plants
     * the boot stub that loads x0 with the DTB address when it believes it
     * loaded a Linux kernel, and a bare ELF is not one (hw/arm/boot.c:
     * load_aarch64_image() is what sets is_linux). With this 64-byte header on
     * the front of the flat image, x0 is the DTB. Without it, A2 has no memory
     * map and no way to find one.
     *
     * This is also what a real ARM board's bootloader expects, so paying for
     * it now costs nothing later. Booting the ELF remains useful for gdb
     * symbols; the image is what runs. See kernel/arch/aarch64/arch.mk.
     *
     * code0 must be an executable instruction at offset 0: on the flat-image
     * path there is no ELF entry point, so execution begins here. */
    b       header_end                  /* code0: branch past the header    */
    .long   0                           /* code1: reserved (PE/COFF "MZ")   */
    .quad   0x80000                     /* text_offset: where in RAM we want
                                         * to be. QEMU's default for aarch64
                                         * is the same 0x80000, and the linker
                                         * script's 0x40080000 = virt's RAM
                                         * base + this.                     */
    .quad   0x100000                    /* image_size: how much RAM the image
                                         * needs INCLUDING .bss, so a loader
                                         * knows what not to overwrite. A flat
                                         * 1 MiB, not `kernel_end - _start`:
                                         * that difference spans sections and
                                         * is not guaranteed to be a link-time
                                         * constant. Revisit when the kernel
                                         * approaches it -- and it must, since
                                         * a nonzero value here is what makes
                                         * text_offset above be honoured.    */
    .quad   0                           /* flags: little-endian, page size
                                         * unspecified, 2 MiB-aligned base   */
    .quad   0                           /* res2 */
    .quad   0                           /* res3 */
    .quad   0                           /* res4 */
    .ascii  "ARM\x64"                   /* magic, at offset 56              */
    .long   0                           /* res5: PE/COFF header offset       */

header_end:
    /* Which CPU are we? -M virt starts every -smp core at the same entry.
     * Only core 0 continues; the rest park until PSCI CPU_ON wakes them in
     * A9. MPIDR_EL1.Aff0 is the core number within a cluster, which is all
     * `virt` uses for a single-cluster machine. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, park

    /* Drop to EL1 if firmware left us at EL2.
     *
     * Plain `-M virt` already enters at EL1, so this is dead code TODAY. It is
     * here because the two things we already plan to do -- `-M virt,virtualization=on`
     * and the UEFI boot path of open sub-decision §6.1 -- both enter at EL2,
     * and the failure mode without it is silent: system-register writes go to
     * the EL2 alias, and the first `eret` to user space in A5 lands nowhere.
     * Cheaper to be correct now than to debug that later. (EL3 is not handled:
     * nothing we target hands a kernel EL3.) */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    at_el1

    /* HCR_EL2.RW = 1: the EL1 we are about to enter is AArch64, not AArch32. */
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1

    /* A known-good SCTLR_EL1: MMU off, D-cache off, I-cache off, with the
     * architecture's RES1 bits set. Writing plain zero here is the classic
     * bring-up bug -- it is tolerated by some implementations and not others. */
    ldr     x1, =0x30d00800
    msr     sctlr_el1, x1

    /* Let EL1 see the physical counter/timer (A3 needs CNTVCT_EL0), and zero
     * the virtual offset so virtual and physical time agree. */
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3                  /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr

    /* Return into EL1h (EL1 using SP_EL1) with DAIF masked -- interrupts stay
     * off until A3 has a GIC to route them to. */
    mov     x1, #0x3c5                  /* D,A,I,F masked | M[3:0] = 0b0101 */
    msr     spsr_el2, x1
    adr     x1, at_el1
    msr     elr_el2, x1
    eret

/* ============================================================================
 * EVERYTHING FROM HERE TO `mmu_on` RUNS AT THE PHYSICAL ADDRESS WITH THE MMU
 * OFF, WHILE EVERY SYMBOL IN THE IMAGE IS LINKED AT A VIRTUAL ONE.
 *
 * That means exactly one rule, and breaking it produces a jump into nowhere
 * with no console: only PC-RELATIVE references are valid here. `adrp`+`add`
 * and `ldr` from a literal pool are PC-relative and therefore yield the real
 * physical address; a pointer stored in .rodata or .data is a link-time
 * absolute and is wrong. There is no C in this window for the same reason --
 * the compiler will happily emit an absolute pointer table.
 *
 * `ldr xN, =CONSTANT` is fine: that is a VALUE, not an address.
 * `ldr xN, =symbol` yields the VIRTUAL address -- correct only after mmu_on,
 * which is precisely how the jump to the higher half is made.
 * ==========================================================================*/
at_el1:
    mov     x19, x0                     /* stash the DTB pointer across all of this */

    /* Let FP/SIMD be used at all. CPACR_EL1.FPEN = 0b11 means "do not trap,
     * at either EL". Out of reset it is 0b00, which traps every FP instruction
     * from both EL0 and EL1 -- and the KERNEL wants that for itself (it is
     * built -mgeneral-regs-only precisely so an accidental FP instruction is a
     * loud fault rather than silent state nobody saves). But USER code cannot
     * be built that way: a stock aarch64 compiler emits `str q0` to copy a
     * 16-byte struct, so an EL0 program that never mentions a float still
     * traps on its first memcpy. Opening this is therefore a prerequisite for
     * EL0 existing at all, which is why it lands together with the FP half of
     * kernel_ctx_switch and not before it. */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /* A boot beacon. There is no console yet -- pl011.c is C and runs later --
     * so these single characters are the only evidence available if the MMU
     * bring-up below dies. Each one costs three instructions and has already
     * earned them: "prints A then nothing" and "prints nothing at all" are
     * completely different bugs, and without this they look identical. */
    PUTC    0x41                        /* 'A' -- reached EL1 */

    /* Zero .bss BEFORE installing the stack (the stack LIVES in .bss, so the
     * other order has the zeroing loop walk over the frame we stand on) and
     * before building the page tables (which also live in .bss, and would be
     * erased by it). Both symbols are 8-byte aligned by the linker script, so
     * the 8-bytes-at-a-time loop cannot overrun. */
    PHYS_OF x1, __bss_start
    PHYS_OF x2, __bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b

2:  PHYS_OF x1, __stack_top
    mov     sp, x1
    PUTC    0x42                        /* 'B' -- .bss zeroed, stack up */

    bl      build_page_tables
    PUTC    0x50                        /* 'P' -- page tables built */

    bl      enable_mmu
    /* Still executing physically -- that is what the TTBR0 identity map is
     * for. The very next thing is to stop. */
    PUTC    0x4D                        /* 'M' -- MMU on, still physical */

    /* THE JUMP. `ldr =mmu_on` reads a link-time absolute from the literal
     * pool, which is the VIRTUAL address, which is now mapped. Everything
     * after this line is ordinary code at ordinary addresses. */
    ldr     x0, =mmu_on
    br      x0

mmu_on:
    /* Virtual from here. Re-point the stack at the kernel window's alias of
     * the same memory -- the physical alias still works while TTBR0 holds the
     * identity map, but it stops working the moment A5 gives TTBR0 to user
     * space, and a stack pointer that dies later is far worse than one that
     * dies now. */
    ldr     x1, =__stack_top
    mov     sp, x1

    /* Frame pointer and link register have no meaningful values from the
     * physical world; zero them so a backtrace stops here instead of walking
     * into garbage. */
    mov     x29, xzr
    mov     x30, xzr

    mov     x0, x19                     /* PHYSICAL DTB address; C converts */
    bl      arch_early_main

    /* arch_early_main() is not expected to return. If it does, fall into the
     * same park loop rather than executing whatever bytes follow. */
/* ============================================================================
 * SECONDARY CPU ENTRY -- ARM64.md phase A9.
 *
 * PSCI CPU_ON hands the target core a PHYSICAL entry address and one context
 * value. It arrives here exactly as the primary arrived at _start: MMU off,
 * caches off, executing physically while every symbol in the image is linked
 * at a virtual address -- so the same one rule applies (see the long note
 * above: PHYS_OF for addresses, `ldr =CONST` for values).
 *
 * What is DIFFERENT from the primary, and why each difference exists:
 *
 *   - It does NOT build the page tables. They already exist, in .bss, built by
 *     core 0 before it ever called PSCI. Building them again would be a race
 *     against every core already using them.
 *   - It does NOT zero .bss, for the same reason and a worse one: .bss now
 *     holds the entire running kernel's state.
 *   - It gets its OWN STACK, from the table core 0 filled in before starting
 *     it. Sharing __stack_top would have two cores writing the same frames,
 *     which is not a subtle failure but is a confusing one.
 *   - x0 on entry is the CONTEXT ID PSCI was given: this core's index. It is
 *     the only thing distinguishing one secondary from another this early,
 *     because MPIDR needs decoding and the index is what the C side wants.
 * ==========================================================================*/
    .globl secondary_entry
secondary_entry:
    mov     x20, x0                     /* the CPU index, across all of this */

    /* Same EL2 drop as the primary, and dead code for the same reason: QEMU's
     * PSCI starts a secondary at the EL of the caller, and we call from EL1.
     * It is here so `-M virt,virtualization=on` does not silently start every
     * secondary in a state where system-register writes go to the EL2 alias. */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    sec_at_el1

    mov     x1, #(1 << 31)
    msr     hcr_el2, x1
    ldr     x1, =0x30d00800
    msr     sctlr_el1, x1
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    mov     x1, #0x3c5
    msr     spsr_el2, x1
    adr     x1, sec_at_el1
    msr     elr_el2, x1
    eret

sec_at_el1:
    /* FP/SIMD usable, exactly as the primary sets it: EL0 code on THIS core
     * needs it just as much, and CPACR_EL1 is per-core state. */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /* This core's stack, from the table core 0 filled in before starting it.
     *
     * PHYS_OF reaches the TABLE physically (adrp/add is PC-relative, and the PC
     * is physical here). The VALUE in it is a kernel-window virtual address --
     * that is what the allocator handed out -- so it is converted down by the
     * kernel virtual base, which is the one constant this needs and the same
     * one the linker script and KV2P() use. */
    PHYS_OF x1, smp_stack_top
    ldr     x2, [x1, x20, lsl #3]
    ldr     x3, =KERNEL_VIRT_BASE
    sub     x2, x2, x3
    mov     sp, x2

    bl      enable_mmu                  /* the tables are already built */

    ldr     x0, =sec_mmu_on
    br      x0

sec_mmu_on:
    /* Virtual from here. Re-point the stack at the kernel window's alias of
     * the same memory, for the reason the primary does: the physical alias
     * dies the moment TTBR0 belongs to a user process. */
    ldr     x1, =smp_stack_top
    ldr     x2, [x1, x20, lsl #3]
    mov     sp, x2                      /* already the virtual address */

    mov     x29, xzr
    mov     x30, xzr

    mov     x0, x20
    bl      smp_secondary_main

    /* Not expected to return; park rather than execute whatever follows. */
park:
    wfe
    b       park

/* ---------------------------------------------------------------------------
 * Page tables for the initial address space.
 *
 * 1 GiB blocks at level 1, which is the whole reason this is tractable in
 * assembly: with a 4 KiB granule and 48-bit VAs, a level-1 entry maps 1 GiB
 * directly, so the entire initial address space is a level-0 table plus four
 * level-1 tables and about twenty stores. No level-2, no level-3, no page
 * allocator -- and there could not be one, because the physical memory manager
 * does not exist until the DTB has been read, which cannot happen until there
 * is an address space to read it in.
 *
 * Refinement to 4 KiB pages with real permissions happens later, in C, in
 * mm/pagetable.c, once pmm can hand out page frames.
 *
 * The layout is kernel/mm/pmm.h's, unchanged, because it is not x86-specific:
 *
 *   TTBR0  identity            VA == PA, 0..4 GiB. Required: the instruction
 *                              after the MMU is enabled is still at a physical
 *                              PC, so that PC must translate to itself.
 *   TTBR1  0xFFFFFFFF80000000  kernel window, VA = PA + base, 2 GiB
 *          0xFFFF800000000000  direct map of physical memory, 4 GiB
 *          0xFFFFC00000000000  MMIO window for device space, 1 GiB
 * ------------------------------------------------------------------------- */
build_page_tables:
    stp     x29, x30, [sp, #-16]!

    /* --- TTBR0: identity ------------------------------------------------- */
    PHYS_OF x0, boot_l0_ttbr0
    PHYS_OF x1, boot_l1_ident
    orr     x2, x1, #PTE_TABLE_BITS
    str     x2, [x0]                            /* l0[0] -> l1_ident */

    ldr     x3, =BLOCK_DEVICE
    str     x3, [x1]                            /* PA 0..1G: device space */
    ldr     x4, =BLOCK_RAM_EXEC
    mov     x5, #1
3:  lsl     x6, x5, #30
    orr     x6, x6, x4
    str     x6, [x1, x5, lsl #3]                /* PA 1..4G: RAM, executable */
    add     x5, x5, #1
    cmp     x5, #4
    b.lo    3b

    /* --- TTBR1: kernel window, direct map, MMIO -------------------------- */
    PHYS_OF x0, boot_l0_ttbr1

    PHYS_OF x1, boot_l1_kern
    orr     x2, x1, #PTE_TABLE_BITS
    str     x2, [x0, #(L0_IDX_KERNEL * 8)]
    ldr     x3, =BLOCK_DEVICE
    str     x3, [x1, #(510 * 8)]                /* VA base+0    -> PA 0..1G  */
    ldr     x3, =BLOCK_RAM_EXEC
    ldr     x4, =0x40000000
    orr     x3, x3, x4
    str     x3, [x1, #(511 * 8)]                /* VA base+1G   -> PA 1..2G  */

    PHYS_OF x1, boot_l1_dmap
    orr     x2, x1, #PTE_TABLE_BITS
    str     x2, [x0, #(L0_IDX_DIRECT * 8)]
    ldr     x3, =BLOCK_DEVICE
    str     x3, [x1]
    /* The direct map is data, never instructions: PXN|UXN. A kernel that can
     * execute through its own linear map of all physical memory is a kernel
     * where any write primitive is also a code-injection primitive. */
    ldr     x4, =BLOCK_RAM_NOEXEC
    mov     x5, #1
4:  lsl     x6, x5, #30
    orr     x6, x6, x4
    str     x6, [x1, x5, lsl #3]
    add     x5, x5, #1
    cmp     x5, #4
    b.lo    4b

    PHYS_OF x1, boot_l1_mmio
    orr     x2, x1, #PTE_TABLE_BITS
    str     x2, [x0, #(L0_IDX_MMIO * 8)]
    ldr     x3, =BLOCK_DEVICE
    str     x3, [x1]                            /* PA 0..1G as Device memory */

    /* The tables were built with the MMU and caches OFF, so these stores went
     * straight to memory -- but the page-table walker is a separate observer
     * and there is no guarantee it sees them without a barrier. Skipping this
     * produces a translation fault on the first instruction after the MMU is
     * enabled, i.e. a silent hang. */
    dsb     ishst

    ldp     x29, x30, [sp], #16
    ret

/* ---------------------------------------------------------------------------
 * Turn the MMU on.
 * ------------------------------------------------------------------------- */
enable_mmu:
    /* MAIR_EL1: the attribute table page-table entries index into.
     *   attr0 = 0x00  Device-nGnRnE      (MMIO: no gathering/reordering/early ack)
     *   attr1 = 0xFF  Normal, write-back, read+write allocate, inner+outer     */
    ldr     x0, =MAIR_VALUE
    msr     mair_el1, x0

    /* TCR_EL1. IPS must describe the machine, not a guess: an IPS smaller than
     * the implemented physical address size silently truncates output
     * addresses, which is a fault at an address that looks almost right. */
    mrs     x0, id_aa64mmfr0_el1
    and     x0, x0, #0xf                        /* PARange */
    lsl     x0, x0, #32                         /* TCR_EL1.IPS */
    ldr     x1, =TCR_VALUE
    orr     x0, x0, x1
    msr     tcr_el1, x0

    PHYS_OF x0, boot_l0_ttbr0
    msr     ttbr0_el1, x0
    PHYS_OF x0, boot_l0_ttbr1
    msr     ttbr1_el1, x0

    /* Whatever was in the TLBs describes an address space that no longer
     * exists. `vmalle1` invalidates all stage-1 EL1 entries; the barriers are
     * not optional -- the isb is what makes the new TCR/TTBR visible to
     * instruction fetch before the MMU is switched on. */
    dsb     ishst
    tlbi    vmalle1
    dsb     ish
    isb

    mrs     x0, sctlr_el1
    ldr     x1, =SCTLR_ENABLE
    orr     x0, x0, x1
    msr     sctlr_el1, x0
    isb

    ret

/* Dump the `ldr =` literal pool here, inside .text.boot and within range of
 * every instruction that uses it, rather than wherever the assembler would
 * otherwise put it. */
    .ltorg

/* ---------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */
    .bss
    .balign 16
__stack_bottom:
    /* 64 KiB, not 16.
     *
     * The kernel's own boot path was fine on 16 KiB right up until the real
     * filesystem was linked in: EMBKFS walks a B-tree recursively during
     * mount, and that walk plus kprintf's formatter is far deeper than
     * anything the bring-up code did. The overflow ran off the bottom of the
     * stack into the .bss immediately below it -- which is where the initial
     * PAGE TABLES live -- and the symptom was the first context switch after
     * the mount jumping somewhere unmapped, nowhere near the corruption.
     *
     * There is no guard page here because this stack exists before the MMU
     * does. Threads created later get vmm_alloc_kernel_stack(), which puts an
     * unmapped page below each one and turns exactly this bug into an
     * immediate, named translation fault. */
    .space 65536
    .globl __stack_top
__stack_top:

    /* The initial translation tables. In .bss, so they cost nothing in the
     * image and are already zeroed by the time build_page_tables runs -- a
     * zero entry is an invalid one, which is exactly the right default for
     * every slot we do not fill.
     *
     * Global because mm/pagetable.c adopts them: these ARE the kernel's page
     * tables from here on, not a throwaway bootstrap set. */
    .balign 4096
    .globl boot_l0_ttbr0
boot_l0_ttbr0:  .space 4096
    .globl boot_l0_ttbr1
boot_l0_ttbr1:  .space 4096
    .globl boot_l1_ident
boot_l1_ident:  .space 4096
    .globl boot_l1_kern
boot_l1_kern:   .space 4096
    .globl boot_l1_dmap
boot_l1_dmap:   .space 4096
    .globl boot_l1_mmio
boot_l1_mmio:   .space 4096
