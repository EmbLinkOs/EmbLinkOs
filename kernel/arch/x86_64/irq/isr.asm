[BITS 64]


extern isr_handler
extern irq_handler
extern lapic_timer_handler
extern lapic_ipi_handler
global lapic_timer_stub
global lapic_spurious_stub
global lapic_ipi_stub_0
global lapic_ipi_stub_1
global lapic_ipi_stub_2


; LAPIC spurious-interrupt handler (the vector programmed into the SVR, 0xFF).
; A spurious interrupt is the local APIC saying "the IRQ that raised this
; de-asserted before I could latch WHICH one" -- there is nothing to service.
; It must NOT be EOI'd: the SDM (Vol.3A 10.9) is explicit that a spurious
; vector does not set an ISR bit, so an EOI here would acknowledge some OTHER,
; genuinely-in-service interrupt. So touch no state, send no EOI, just return.
; No register saves are needed -- a bare iretq restores the interrupted context
; byte-for-byte (we clobbered nothing). Shared across all cores via the one IDT.
lapic_spurious_stub:
    iretq


lapic_timer_stub:
    push qword 0 ; push dummy error code (not used for LAPIC timer, but we want to keep the stack layout consistent)
    push qword 48 ; push vector number for LAPIC timer (we can choose any unused vector, using 255 here)
    jmp irq_common_lapic




%macro ISR_NOERR 1 ; macro for ISRs without error code
global isr%1       
isr%1:
    push qword 0 ; push dummy error code
    push qword %1 ; push ISR number / interrupt number / vector number
    jmp isr_commom
%endmacro


%macro ISR_ERR 1 ; macro for ISRs with error code
global isr%1
isr%1:
    ; error code is already pushed by CPU, so we only need to push the ISR number
    push qword %1 ; push ISR number / interrupt number / vector number
    jmp isr_commom
%endmacro


%macro IRQ_STUB 2
global irq%1
irq%1:
    push qword 0 ; push dummy error code (not used for IRQs, but we want to keep the stack layout consistent)
    push qword %2 ; push IRQ number ( VECTOR NUMBER)
    jmp irq_commom
%endmacro


; Shared GPR save/restore for EVERY common interrupt path (CPU exceptions, PIC
; IRQs, the LAPIC timer). The three paths differ only in which C handler they
; call; this 15-register frame was copy-pasted into all three, so any change to
; its layout -- which the C-side `struct registers` mirrors field-for-field --
; had to be kept in lockstep in three places or the frame would silently
; desync. One definition each now. The push order is the REVERSE of struct
; registers' declaration, so after PUSH_GPRS `rsp` points at a valid
; `struct registers` to hand the C handler in rdi.
%macro PUSH_GPRS 0
    push rax
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
%endmacro

%macro POP_GPRS 0
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
    pop rax
%endmacro

; INTER-PROCESSOR INTERRUPTS. One stub per reason, because the vector IS the
; message -- an IPI carries no payload, so the only way to say WHICH is to send
; a different vector. 0xF0..0xF2 are above every device vector and below the
; spurious vector (0xFF).
;
; Each stub passes its reason to C as an IMMEDIATE rather than handing over the
; register frame. The frame would mean teaching this file about `struct
; registers`, which is declared privately in TWO different .c files already;
; the reason is the only thing the handler wants, and the stub is the one place
; that knows it for certain.
%macro IPI_STUB 2   ; %1 = stub suffix, %2 = reason passed to C
lapic_ipi_stub_%1:
    push qword 0            ; dummy error code, so the frame matches every
    push qword %1           ; other stub's and `add rsp,16` means the same
    PUSH_GPRS
    mov rdi, %2             ; the REASON -- see enum ipi_reason
    call lapic_ipi_handler
    POP_GPRS
    add rsp, 16
    iretq
%endmacro

IPI_STUB 0, 0
IPI_STUB 1, 1
IPI_STUB 2, 2


ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; === IRQ stubs (vectors 32-47 = 0x20-0x2F ) ===
; same pattern as ISR_NOERR - push dummy error code (0), push vector
; jump to common handler


IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43     
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47


; Dedicated double-fault (#DF, vector 8) entry. Installed on IST1 by
; syscall_init, so even a corrupted/overflowed kernel stack lands on a known-
; good stack instead of escalating #DF -> triple-fault -> CPU reset. The CPU
; pushes a #DF error code (always 0), so like ISR_ERR we only push the vector
; before joining the shared dump path (isr_handler prints and halts).
global isr_double_fault
isr_double_fault:
    push qword 8        ; vector number (error code already on the stack)
    jmp isr_commom




isr_commom:
    PUSH_GPRS
    mov rdi, rsp          ; pass pointer to the register frame as first argument
    call isr_handler
    POP_GPRS
    add rsp, 16           ; skip the error code + ISR number pushed by the stub
    iretq


irq_commom:
    PUSH_GPRS
    mov rdi, rsp          ; pass pointer to the register frame as first argument
    call irq_handler
    POP_GPRS
    add rsp, 16           ; skip the error code + vector number pushed by the stub
    iretq


irq_common_lapic:
    PUSH_GPRS
    mov rdi, rsp          ; pass pointer to the register frame as first argument
    call lapic_timer_handler
    POP_GPRS
    add rsp, 16           ; skip the dummy error code + vector number pushed by the stub
    iretq

