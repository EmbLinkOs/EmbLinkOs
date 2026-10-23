extern syscall_dispatch
global syscall_entry

; Entered via `int 0x80` (legacy) from ring 3 or `syscall` (modern later). CPU has already pushed the return RIP, CS, and RFLAGS onto the stack.
; and switched to RSP0 the (the TSS kernel stack). We push the GPRs to complete a
; struct regs, hand it's pointer to the syscall handler, and then pop the GPRs and return to userland.
syscall_entry:
    push rax                  ; push the syscall number (in rax) onto the stack
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11        
    push r12
    push r13
    push r14
    push r15


    mov rdi, rsp              ; pass pointer to struct regs (on stack) in rdi
    call syscall_dispatch     ; call the syscall handler

    pop r15
    pop r14
    pop r13 
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8  
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax                  ; pop the syscall number (in rax) off the stack

    iretq                     ; return to userland (RIP, CS, RFLAGS popped by CPU).
                              ; MUST be iretq, not a bare `iret`, in long mode: `iret`
                              ; is the 32-bit interrupt return and pops truncated
                              ; 32-bit RIP/CS/RFLAGS off the stack (page-faults on the
                              ; ring-3 return). NASM 3.x silently promotes `iret` ->
                              ; `iretq` in BITS 64, so it only bites on older NASM.

; kernel_ctx_save / kernel_ctx_restore now live in kcontext.asm.






















; ==========================================================================
; THE FAST PATH: `syscall` / `sysretq`, not `int 0x80`.
;
; `int 0x80` is an interrupt: the CPU walks the IDT, does a stack switch
; through the TSS, and pushes a five-word frame -- ~90-110 ns of ceremony
; before a single instruction of the kernel runs, and the same in reverse on
; `iretq`. `syscall` is a register operation: it drops CS/SS to the kernel
; selectors from STAR, saves the return RIP in RCX and RFLAGS in R11, masks
; the flags in FMASK, and jumps to LSTAR. No IDT walk, no TSS, no pushed frame.
;
; NO SWAPGS, AND THAT IS THE INTERESTING PART. `syscall` does NOT switch the
; stack -- RSP is still the user stack on entry -- so the classic answer is a
; per-CPU pointer reached through a swapped GS. But swapgs across a blocking
; syscall (one the scheduler switches away from and resumes on the interrupt
; path) is the single hardest thing to get right in a kernel, because the two
; return paths must agree on the swap state. EmbLink sidesteps it whole: it
; finds its per-CPU data by LAPIC ID, never by GS, so NOTHING in the kernel
; reads GS -- which means GS.base can hold this core's syscall scratch AT ALL
; TIMES, in user mode and kernel mode alike, and this stub needs no swapgs.
; Ring 3 cannot read [gs:0]: GS.base is a kernel address, the page is
; supervisor-only, the access faults. Ring 3 cannot CHANGE GS.base either --
; there is no arch_prctl and CR4.FSGSBASE is off. So the base the kernel wrote
; per core stays put. (syscall_fast.c sets it; see there.)
;
; What `syscall` clobbers -- RCX and R11 -- is exactly what the userland
; wrappers already declare clobbered; every other register the kernel must
; hand back unchanged, so this frame saves and restores rdi/rsi/rdx/r10/r8/r9
; (the args) and rcx/r11 (the return), touches no callee-saved register, and
; lets the C ABI preserve rbx/rbp/r12-r15 across the one call.
; ==========================================================================
extern syscall_fast_dispatch
global syscall_fast_entry
syscall_fast_entry:
    mov [gs:0], rsp           ; stash the USER rsp in this core's scratch (gs:0)
    mov rsp, [gs:8]           ; this core's kernel stack top (gs:8 == tss.rsp0)

    ; AND IMMEDIATELY ONTO THIS THREAD'S OWN STACK. The scratch at gs:0 is
    ; PER CORE, and a syscall is not: this thread can be preempted anywhere
    ; below, another thread on this core can enter its own syscall and
    ; overwrite gs:0, and then this one would return to ring 3 on THAT
    ; thread's stack pointer. (It does, too -- the desktop died on a push to
    ; an unmapped address one word below a stack that was never its own.) The
    ; scratch is live for exactly these two instructions, with interrupts off
    ; because FMASK cleared IF; from here the value lives in the frame, which
    ; is per thread by construction.
    push qword [gs:0]         ; the user rsp -- popped straight into rsp at the end

    ; A frame the C side reads args from and writes the result into. Pushed
    ; high register first, so in memory (low->high) it is r11, rcx, r9, r8,
    ; r10, rdx, rsi, rdi, rax, user_rsp -- struct syscall_fast_frame in
    ; syscall_fast.c.
    push rax                  ; the syscall number, and the result on the way out
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9
    push rcx                  ; user return RIP  -- saved across the C call
    push r11                  ; user RFLAGS       -- saved across the C call

    mov rdi, rsp
    call syscall_fast_dispatch    ; may sti and block; may never return (a kill)

    pop r11                   ; user RFLAGS back into R11 (sysretq restores it)
    pop rcx                   ; user RIP back into RCX     (sysretq jumps to it)
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop rax                   ; the result

    cli                       ; close the window between restoring the user
                              ; stack and the return (harmless here -- no ISR
                              ; reads GS -- but it is the correct shape)
    pop rsp                   ; the USER rsp, off THIS thread's own frame
    ; `o64 sysret`, NOT `sysretq`: NASM has no such mnemonic, and a bare word
    ; it does not recognise is a LABEL -- it assembled `sysretq` into nothing
    ; at all and the stub fell through into the next function in the object
    ; file, on a user stack, which double-faulted. The build now passes
    ; -w+orphan-labels so a label that was meant to be an instruction is an
    ; error rather than a silent hole. The o64 prefix is what makes this the
    ; 64-bit return (plain `sysret` returns to 32-bit compatibility mode).
    o64 sysret                ; -> ring 3 at RCX, RFLAGS from R11
