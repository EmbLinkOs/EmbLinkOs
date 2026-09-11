#include "arch/aarch64/irq/exception.h"
#include "include/uaccess_guard.h"
#include "mm/vma.h"            /* vm_fault: demand paging */
#include "include/arch_irq.h"   /* arch_irq_enable/disable around vm_fault */
#include "arch/aarch64/drivers/pl011.h"
#include "arch/aarch64/irq/gicv3.h"
#include "process/process.h"

void aarch64_syscall(struct aarch64_frame *f);   /* syscall/syscall.c */

/* Fault decoding -- docs/ARM64.md phase A1.
 *
 * The whole value of this file is that it turns a hang into a sentence. The
 * x86 side learned this the expensive way (kernel/lib/ksym.c exists because a
 * panic that prints only hex addresses is a panic you cannot act on); here we
 * get most of the way there for free, because ESR_EL1 is a DECODED syndrome
 * rather than x86's bare error code. */

extern char aarch64_vectors[];
void aarch64_set_vbar(void *vbar);           /* vectors.S */
void aarch64_exception(uint64_t which, struct aarch64_frame *f);

/* The frame is built by hand in assembly; if the struct grows a field or the
 * stub's offsets drift, every register in every dump is silently wrong. */
_Static_assert(sizeof(struct aarch64_frame) == 288,
               "aarch64_frame must match FRAME_SIZE in vectors.S");
_Static_assert(__builtin_offsetof(struct aarch64_frame, esr) == 272,
               "F_ESR in vectors.S disagrees with struct aarch64_frame");

/* Set while exception_probe() is running: see the header. Deliberately a
 * plain global -- there is one CPU running kernel code at A1, and making this
 * per-CPU before per-CPU state exists would be inventing an abstraction from
 * zero implementations. A9 revisits it. */
static volatile int probe_active;
static volatile int probe_verbose;
static volatile unsigned probe_faults;

static const char *vector_name(uint64_t which) {
    static const char *const kind[] = { "sync", "IRQ", "FIQ", "SError" };
    return which > 15 ? "?" : kind[which & 3];
}

static const char *vector_group(uint64_t which) {
    switch (which >> 2) {
    case 0:  return "EL1t (current EL on SP_EL0 -- should be impossible)";
    case 1:  return "EL1h (current EL, kernel)";
    case 2:  return "EL0 AArch64 (from user space)";
    default: return "EL0 AArch32 (should be impossible -- we never run A32)";
    }
}

/* ESR_EL1.EC, Arm ARM D17.2.37. Only the classes that can actually reach this
 * kernel are named; anything else prints its raw EC, which is still enough to
 * look up. Note EC 0x07 in particular: with -mgeneral-regs-only it should be
 * unreachable, so seeing it means a build lost that flag. */
static const char *ec_name(uint64_t ec) {
    switch (ec) {
    case 0x00: return "unknown / undefined instruction";
    case 0x01: return "trapped WFI/WFE";
    case 0x07: return "SIMD/FP access trapped (CPACR_EL1.FPEN -- lost -mgeneral-regs-only?)";
    case 0x0E: return "illegal execution state";
    case 0x15: return "SVC from AArch64";
    case 0x18: return "trapped MSR/MRS/system instruction";
    case 0x20: return "instruction abort, lower EL";
    case 0x21: return "instruction abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort, lower EL";
    case 0x25: return "data abort, same EL";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "trapped floating-point exception";
    case 0x2F: return "SError";
    case 0x30: return "breakpoint, lower EL";
    case 0x31: return "breakpoint, same EL";
    case 0x32: return "software step, lower EL";
    case 0x33: return "software step, same EL";
    case 0x34: return "watchpoint, lower EL";
    case 0x35: return "watchpoint, same EL";
    case 0x3C: return "BRK instruction";
    default:   return "(unnamed exception class)";
    }
}

/* ESR_EL1.ISS.DFSC/IFSC for aborts -- the field x86 simply does not have. It
 * is the difference between "page fault" and "level 2 translation fault on a
 * write", and at A2, when the page tables are new, it is the whole diagnosis. */
static const char *fsc_name(uint64_t fsc) {
    switch (fsc) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return "address size fault";
    case 0x04: case 0x05: case 0x06: case 0x07:
        return "translation fault";
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return "access flag fault";
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        return "permission fault";
    case 0x10: return "synchronous external abort (nothing is mapped there)";
    case 0x11: return "synchronous tag check fault";
    case 0x21: return "alignment fault";
    case 0x30: return "TLB conflict abort";
    default:   return "(unnamed fault status)";
    }
}

static int is_abort(uint64_t ec) {
    return ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25;
}

static void dump_frame(uint64_t which, struct aarch64_frame *f) {
    uint64_t ec  = (f->esr >> 26) & 0x3F;
    uint64_t iss = f->esr & 0x1FFFFFF;

    pl011_puts("\n=== aarch64 exception ===\n");

    pl011_puts("  vector    : ");
    pl011_putdec(which);
    pl011_puts("  ");
    pl011_puts(vector_name(which));
    pl011_puts(", from ");
    pl011_puts(vector_group(which));
    pl011_puts("\n");

    pl011_puts("  ESR_EL1   : ");
    pl011_puthex64(f->esr);
    pl011_puts("\n              EC=");
    pl011_puthex32((uint32_t)ec);
    pl011_puts("  ");
    pl011_puts(ec_name(ec));
    pl011_puts("\n");

    if (is_abort(ec)) {
        pl011_puts("              FSC=");
        pl011_puthex32((uint32_t)(iss & 0x3F));
        pl011_puts("  ");
        pl011_puts(fsc_name(iss & 0x3F));
        /* WnR is meaningful for data aborts only. */
        if (ec == 0x24 || ec == 0x25) {
            pl011_puts(", on a ");
            pl011_puts((iss & (1u << 6)) ? "WRITE" : "READ");
        }
        pl011_puts("\n");
        pl011_puts("  FAR_EL1   : ");
        pl011_puthex64(f->far);
        pl011_puts("   <- the address that faulted\n");
    }

    pl011_puts("  ELR_EL1   : ");
    pl011_puthex64(f->elr);
    pl011_puts("   <- the instruction\n");
    pl011_puts("  SPSR_EL1  : ");
    pl011_puthex64(f->spsr);
    pl011_puts("\n  SP        : ");
    pl011_puthex64(f->sp);
    pl011_puts("\n");

    /* Four per line: 31 registers in a wall of one-per-line is a screen and a
     * half of scrollback, and the thing you are looking for is never on the
     * part you can still see. */
    pl011_puts("\n");
    for (int i = 0; i < 31; i++) {
        if ((i & 3) == 0)
            pl011_puts("  ");
        pl011_puts("x");
        pl011_putdec((uint64_t)i);
        pl011_puts(i < 10 ? " =" : "=");
        pl011_puthex64(f->x[i]);
        pl011_puts((i & 3) == 3 ? "\n" : "  ");
    }
    pl011_puts("\n  (x30 is LR: the address the faulting function returns to)\n");
}

void aarch64_exception(uint64_t which, struct aarch64_frame *f) {
    uint64_t ec = (f->esr >> 26) & 0x3F;

    /* An IRQ is not a fault and must not be decoded like one: ESR_EL1 is not
     * even updated for it. Vectors 1, 5, 9 and 13 are the IRQ slot of each
     * group; the controller knows which line actually fired.
     *
     * This returns normally, and the vector epilogue then restores the frame
     * and erets -- which is how a preempted thread resumes exactly where it
     * was. It may also NOT return: the timer handler can context-switch, in
     * which case some other thread's epilogue runs instead. Both are correct;
     * see gic_dispatch()'s note on why the interrupt is ended first. */
    if ((which & 3) == 1) {
        gic_dispatch();
        return;
    }

    /* A system call. EC 0x15 is `svc` from AArch64, and from a LOWER EL it can
     * only be user space asking for something -- which is why this is checked
     * against the vector group and not just the exception class: an `svc` from
     * EL1 would be the kernel calling itself, which nothing does and which
     * should therefore be a panic rather than a dispatch.
     *
     * ELR_EL1 already points PAST the svc (the architecture treats it as
     * completed), so returning from here erets straight to the instruction
     * after it -- no adjustment, unlike the +4 the fault-recovery path needs.
     * That asymmetry is the same one exception_probe() documents. */
    if (which == EXC_EL0_64_SYNC && ec == 0x15) {
        aarch64_syscall(f);
        return;
    }

    /* --- RESOLVE the fault, if it is resolvable -----------------------------
     *
     * Before the probe and long before the panic: ask the VM whether this
     * address is legitimately the process's and simply has no page yet. That
     * is what demand paging IS -- mmap reserves address space, and the page
     * appears here, on first touch. A handled fault produces NO output; it is
     * the ordinary way memory comes into existence.
     *
     * EITHER EL, but only for a USER address. A fault at EL1 on a user address
     * is not a bug -- it is the kernel touching a process's memory on its
     * behalf, which is what copy_to_user does, and a page not yet faulted in
     * is as legitimate there as it is at EL0. A fault at EL1 on a KERNEL
     * address has no VMA and stays fatal.
     *
     * EC 0x24/0x25 are data aborts (lower EL / same EL) and 0x20/0x21
     * instruction aborts. WnR (bit 6 of ISS) says whether a DATA access was a
     * write; for an instruction abort the access is by definition a fetch,
     * which is what `exec` carries. */
    if ((ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) &&
        current_thread && f->far < USER_VA_LIMIT) {
        bool is_data = (ec == 0x24 || ec == 0x25);
        bool w = is_data && ((f->esr >> 6) & 1);
        bool x = !is_data;

        /* WITH INTERRUPTS ON, if the interrupted context had them on -- as
         * on x86. Taking an exception masks IRQs, and resolving a fault can
         * mean waiting for a disk: the file the page comes from, or the swap
         * store it went to. The saved PSTATE (SPSR_EL1.I clear = unmasked)
         * says whether unmasking is legitimate: EL0 always was; an EL1
         * copy_to_user that faulted was too. Re-masked afterwards so the
         * rest of this handler runs as it always did.
         *
         * THIS HUNG INIT ONCE, every boot, and the cause was not here. The
         * three lines were bisected out and the boot passed; what they had
         * exposed was the VMA lock: a spinlock under which objects were
         * created and pages discarded -- both of which sleep on the page
         * cache's mutex -- so the first EL1 preemption of a thread holding
         * it, made possible by this unmask, left every other core spinning
         * on it with interrupts off. With nothing sleeping under that lock
         * (mm/vma.c), the unmask went back in and seven boots in a row
         * passed. This was also the first path on aarch64 to be preempted at
         * EL1 as a USER thread; kernel threads always were. */
        bool from_user = (which >> 2) == 2;
        if (from_user) current_thread->in_kernel++;   /* a kill waits for this to finish */
        bool irqs_on = (f->spsr & (1ULL << 7)) == 0;
        if (irqs_on) arch_irq_enable();
        bool handled = vm_fault(current_thread->proc, f->far, w, x);
        if (irqs_on) arch_irq_disable();
        if (from_user) {
            current_thread->in_kernel--;
            if (handled && current_thread->killed)
                thread_die_killed();         /* never returns */
        }
        if (handled)
            return;                      /* retry the instruction */
    }

    /* A GUARDED user copy that faulted. The kernel touched user memory that
     * access_ok() had just proved was mapped, and another core unmapped it in
     * between -- see include/uaccess_guard.h. This does not return: it
     * longjmps back into copy_from_user()/copy_to_user(), which then reports
     * -EFAULT to the syscall that asked.
     *
     * Restricted to a SYNCHRONOUS exception FROM EL1: a fault at EL0 is the
     * program's own and belongs to the code below, and an IRQ is not a fault
     * at all. Checked before the probe and before the panic, because it is the
     * only one of the three that can legitimately happen at any moment. */
    if (which == EXC_EL1H_SYNC && (ec == 0x25 || ec == 0x21) &&
        uaccess_fault_recover()) {
        /* not reached */
    }

    /* Recoverable probe: step over the offending instruction and continue.
     * Restricted to SYNCHRONOUS exceptions, because for an IRQ or an SError
     * "the offending instruction" is not a meaningful idea -- ELR points at
     * innocent code that would then be skipped. */
    if (probe_active && (which & 3) == 0) {
        probe_faults++;
        if (probe_verbose)
            dump_frame(which, f);

        /* Step over the offending instruction. Every A64 instruction is 4
         * bytes, which is one of the genuinely pleasant things about this
         * architecture -- the x86 equivalent needs a disassembler to know how
         * far to step.
         *
         * EXCEPT for SVC (and HVC/SMC), where ELR already points PAST the
         * instruction, because a syscall is meant to resume after it. Adding 4
         * there skips an innocent instruction instead, and the damage shows up
         * somewhere unrelated. Same asymmetry as x86 traps vs. faults; ARM
         * just does not name it in the encoding, so it has to be known. */
        if (ec != 0x15)
            f->elr += 4;
        return;
    }

    dump_frame(which, f);

    /* A fault from EL0 is the PROGRAM's fault, not the kernel's, and halting
     * the machine for it would be a denial of service any user program could
     * trigger. Kill it and carry on -- which is exactly what x86 does, through
     * process_exit_self(), and that path is now linked here too. */
    if ((which >> 2) == 2) {
        pl011_puts("\nel0: killing the faulting program; the kernel is fine.\n");
        process_exit_self(-1);
    }

    pl011_puts("\nkernel halted (no recovery path for a fault at EL1).\n");
    for (;;)
        __asm__ volatile("wfi");
}

void exception_init(void) {
    aarch64_set_vbar(aarch64_vectors);
}

unsigned exception_probe(void (*fn)(void), int verbose) {
    unsigned before = probe_faults;

    probe_verbose = verbose;
    probe_active  = 1;
    fn();
    probe_active  = 0;
    probe_verbose = 0;

    return probe_faults - before;
}
