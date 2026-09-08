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

at_el1:
    /* Zero .bss BEFORE installing the stack: the stack LIVES in .bss (see
     * below), so doing it the other way round means the zeroing loop walks
     * over the frame we are standing on. Both symbols are 8-byte aligned by
     * the linker script, so the 8-bytes-at-a-time loop cannot overrun. */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b

2:  ldr     x1, =__stack_top
    mov     sp, x1

    /* x0 still holds the DTB pointer QEMU put there. */
    bl      arch_early_main

    /* arch_early_main() is not expected to return. If it does, fall into the
     * same park loop rather than executing whatever bytes follow. */
park:
    wfe
    b       park

    /* The `ldr xN, =symbol` pseudo-instructions above need their literal pool
     * within reach; place it here rather than wherever the assembler decides. */
    .ltorg

    /* 16 KiB of boot stack. In .bss so it costs nothing in the image, and
     * 16-byte aligned because the AArch64 procedure call standard requires
     * SP to be 16-byte aligned at every public interface -- an unaligned SP
     * raises an SP alignment fault, which in A0 (no exception vectors yet)
     * would look exactly like a hang. */
    .bss
    .balign 16
__stack_bottom:
    .space 16384
    .globl __stack_top
__stack_top:
