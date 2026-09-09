/* The assembled EL0 probe, embedded in the kernel image as data.
 *
 * Same pattern as kernel/arch/x86_64/smp/ap_trampoline_blob.asm: a payload
 * that must exist as raw bytes at a runtime-chosen address cannot be an
 * ordinary function, because it is not linked where it runs. The build
 * assembles el0_probe.S at address 0, flattens it with objcopy, and this
 * wrapper carries the result into .rodata where the kernel can copy it into a
 * user mapping. */

    .section .rodata
    .balign 16
    .globl el0_probe_blob
el0_probe_blob:
    .incbin "build/aarch64/el0_probe.bin"
    .globl el0_probe_blob_end
el0_probe_blob_end:
