/* Self-relocating entry for the EmbLink UEFI loader.
 *
 * UEFI loads a PE anywhere in memory, so a position-independent image must fix
 * up its own R_X86_64_RELATIVE relocations before any absolute address is used.
 * We compile -fpic and link -shared, which emits those relocs into .rela.*; the
 * linker script gathers them between __rela_start/__rela_end (inside an
 * allocated section so objcopy carries them into the PE). This runs first,
 * applies them, then calls efi_main.
 *
 * The dummy .reloc section below is load-bearing: without a .reloc, firmware
 * marks the image RELOCS_STRIPPED and will only load it at its ImageBase (0,
 * which is unusable) -- so it would refuse to launch. One no-op fixup is enough
 * to keep the image relocatable; the REAL relocation is what this code does.
 */
    .intel_syntax noprefix
    .text
    .globl _start
_start:
    /* Firmware entry (MS x64 ABI): RCX = ImageHandle, RDX = SystemTable.
     * Stash them in callee-saved regs -- we never return to firmware, so we owe
     * it nothing, and the relocation loop below is free to clobber the rest. */
    mov r12, rcx
    mov r13, rdx

    lea rdi, [rip + __image_base]   /* rdi = actual load base (RIP-relative) */
    lea rsi, [rip + __rela_start]
    lea rax, [rip + __rela_end]
1:  cmp rsi, rax
    jae 2f
    mov rcx, [rsi + 8]              /* r_info */
    and ecx, 0xffffffff            /* low 32 bits = type */
    cmp ecx, 8                      /* R_X86_64_RELATIVE */
    jne 3f
    mov rcx, [rsi + 0]             /* r_offset */
    mov rdx, [rsi + 16]           /* r_addend */
    add rdx, rdi                    /* base + addend */
    mov [rdi + rcx], rdx           /* *(base + r_offset) = base + addend */
3:  add rsi, 24                     /* sizeof(Elf64_Rela) */
    jmp 1b
2:
    /* Call efi_main(ImageHandle, SystemTable) -- MS x64: RCX, RDX, plus a
     * 16-byte-aligned stack and 32 bytes of shadow space. */
    mov rcx, r12
    mov rdx, r13
    and rsp, -16
    sub rsp, 32
    call efi_main
.hang:
    cli
    hlt
    jmp .hang

    /* Minimal, valid, no-op base relocation block. Keeps the PE relocatable. */
    .section .reloc, "a"
    .long 0      /* Page RVA */
    .long 10     /* Block size: 2 longs (8) + one 2-byte entry */
    .word 0      /* type 0 (ABSOLUTE) offset 0 -- ignored by the loader */
