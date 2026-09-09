#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/cpu/fsbase.h"    /* arch_tls_base_set() */
#include "include/syscall_abi.h"
#include <stdint.h>

/* The x86_64 half of the system call path, and now all of it that is x86.
 *
 * Everything that used to be here -- 89 handlers, 2,000 lines -- is in
 * kernel/syscall/syscalls.c, because it was never architecture-specific
 * (docs/ARM64.md §2.4). What is left is the part that genuinely is a machine:
 * where the arguments live, which vector user space traps through, and how the
 * thread pointer is installed.
 */

/* Fill the machine-independent argument block from the interrupt frame.
 *
 * THIS FUNCTION IS THE ENTIRE x86-NESS OF THE SYSTEM CALL INTERFACE. rdi, rsi,
 * rdx, r10, r8, r9 -- the SysV order, with r10 standing in for rcx, which is
 * the convention EmbLink's userspace stubs already use. aarch64 will write the
 * same six slots from x0..x5 and nothing above this line will notice.
 *
 * Note the number comes from rax and the arguments do not: reading rax as the
 * first argument was a real bug once (every exit code became 2, the SYS_exit
 * number), which is why sys_exit still carries a comment about it. */
static void sysargs_from_regs(struct sysargs *a, const struct regs *r) {
    a->nr     = r->rax;
    a->arg[0] = r->rdi;
    a->arg[1] = r->rsi;
    a->arg[2] = r->rdx;
    a->arg[3] = r->r10;
    a->arg[4] = r->r8;
    a->arg[5] = r->r9;
}

/* Called from the asm stub (syscall_entry.asm) with a pointer to the saved
 * register frame. Must have external linkage so the assembler's
 * `extern syscall_dispatch` resolves -- a `static` here would not link.
 * Number in rax; result written back into rax (the stub pops it to the user).*/
void syscall_dispatch(struct regs *r) {
    /* int 0x80 is an INTERRUPT gate (IDT_GATE_USER = 0xEE, type 0xE), which
     * auto-clears IF on entry -- so without this, every syscall handler
     * runs with interrupts disabled for its entire duration. That's fine for
     * sys_write/sys_exit (no blocking work), but sys_open and friends go
     * through vfs_open() -> EMBKFS/FAT32 -> the block layer -> ATA/AHCI, which
     * waits on a disk-completion IRQ to finish a read -- an IRQ that can never
     * be serviced while IF=0. Without this `sti`, any syscall that touches
     * disk I/O hangs forever.
     * Safe to re-enable here: the asm stub's `iret` at the end restores
     * RIP/CS/RFLAGS from the ORIGINAL int 0x80 entry frame regardless of
     * what IF is doing in between, and ring-3 code always runs with IF=1
     * anyway (it can't execute cli/sti itself -- privileged, #GP). */
    __asm__ volatile("sti");

    struct sysargs a;
    sysargs_from_regs(&a, r);
    r->rax = (uint64_t)syscall_invoke(&a);
}

/* syscall_abi.h's one architecture hook. fsbase_set() is a static inline in a
 * header the shared kernel must not include, so it is given a name here rather
 * than an #ifdef there. */
void arch_tls_base_set(uint64_t base) {
    fsbase_set(base);
}

/* type_attr byte for a 64-bit IDT gate: P(0x80) | DPL(bits 5-6) | type(0xE =
 * interrupt gate, which auto-clears IF on entry). The CS selector (0x08) is set
 * by idt_set_entry itself. */
#define IDT_GATE_KERNEL  0x8E   /* present, DPL0, interrupt gate */
#define IDT_GATE_USER    0xEE   /* present, DPL3, interrupt gate (0x8E | 0x60) */

#define IST_DOUBLE_FAULT 1      /* TSS IST slot for #DF; g_tss.ist1 set in gdt_init */

void syscall_init(void) {
    extern void syscall_entry(void); // Defined in syscall_entry.asm
    /* Vector 0x80, interrupt gate (auto-clears IF). DPL=3 so a ring-3
     * `int 0x80` is allowed; IST 0 means use RSP0 from the TSS. */
    idt_set_entry(0x80, (uint64_t)syscall_entry, IDT_GATE_USER);

    /* Upgrade #DF (vector 8) to run on IST1. idt_init installs a baseline isr8
     * on the regular stack; here we re-point vector 8 at the dedicated
     * isr_double_fault entry that switches to g_tss.ist1 (set up in gdt_init).
     * That way a malformed frame yields a handler+dump instead of reusing a bad
     * kernel stack and triple-faulting into a reset. Must run after idt_init. */
    extern void isr_double_fault(void);
    idt_set_entry_ist(8, (uint64_t)isr_double_fault, IDT_GATE_KERNEL, IST_DOUBLE_FAULT);
}
