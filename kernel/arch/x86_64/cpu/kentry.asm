; Kernel entry trampoline.
;
; stage2 loads the kernel ELF and jumps to its e_entry with a *temporary* stack
; that lives in low conventional memory (set up by stage2). Our very first job is
; to switch RSP onto the kernel's OWN stack, which lives in .bss and is therefore
; part of the kernel image: it is mapped by the kernel mapping and reserved by the
; PMM automatically, and it grows *with* the kernel instead of sitting at a fixed
; low-memory address the growing kernel can crash into. Then we call kernel_main.

[BITS 64]

section .bss
align 4096
boot_stack_bottom:
    resb 0x20000                 ; 128 KiB kernel boot stack
global boot_stack_top
boot_stack_top:                  ; full-descending stack starts here (page-aligned)

section .text
global _start
extern kernel_main
extern __stack_chk_guard
_start:
    mov r12, rdi                 ; stash the boot protocol pointer
    mov rsp, boot_stack_top      ; switch to the kernel-owned stack
    xor rbp, rbp                 ; terminate stack-trace/frame chain
    ; THE STACK CANARY, seeded here and never again. Every C function compiled
    ; with -fstack-protector-strong reads __stack_chk_guard on entry and checks
    ; it on exit, so the word must be final before the FIRST C frame exists --
    ; kernel_main's frame is live for the whole boot, and rewriting the guard
    ; later (from the CSPRNG, say) would make its epilogue see a mismatch and
    ; halt the machine as if it had been attacked. The TSC at boot is enough:
    ; the canary needs to be unknown to an overflow that cannot READ, not to an
    ; attacker who already can. The low byte is cleared so a string overflow
    ; (which stops at a NUL) cannot overwrite the canary without ending on it.
    ; r12 (the boot record, kernel_main's argument) is not touched.
    rdtsc
    shl rdx, 32
    or  rax, rdx
    mov rcx, 0x9E3779B97F4A7C15
    imul rax, rcx
    and rax, ~0xFF
    mov [rel __stack_chk_guard], rax

    mov rdi, r12                 ; restore as kernel_main's first argument
    call kernel_main
.halt:                           ; kernel_main should never return; park the CPU
    cli
    hlt
    jmp .halt