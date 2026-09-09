#include "arch/aarch64/syscall/usermode.h"
#include "arch/aarch64/cpu/kcontext.h"
#include "arch/aarch64/mm/pagetable.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"

/* Getting to EL0 and back -- docs/ARM64.md phase A5. See usermode.h. */

/* Where the probe's code and stack are mapped. Both are ordinary user virtual
 * addresses in the TTBR0 half; nothing else lives there, because the boot
 * identity map was removed at the end of A2 and TTBR0 has been empty since. */
#define USER_CODE_VA     0x0000000000400000ULL
#define USER_STACK_TOP   0x0000000000800000ULL
#define USER_STACK_PAGES 4

extern const unsigned char el0_probe_blob[];
extern const unsigned char el0_probe_blob_end[];

/* Where to come back to when the program exits. Same trick x86 uses
 * (g_user_exit_ctx in usermode.c): a setjmp/longjmp pair standing in for
 * "return here", because exit() cannot return through the syscall it made. */
static struct kcontext exit_ctx;
static int64_t exit_code;
static bool in_user;

/* Assembly, because the transition cannot be expressed in C: it ends in `eret`
 * and never returns to its caller. kcontext.S. */
#include "include/arch_thread.h"   /* arch_enter_user_mode() */

/* Called by the bring-up sys_exit. Does not return. */
void el0_exit(int64_t code) {
    exit_code = code;
    in_user = false;
    kernel_ctx_restore(&exit_ctx, 1);
}

bool el0_in_user_mode(void) { return in_user; }

/* Map `pages` fresh zeroed frames at `va` with `flags`. Returns false on any
 * failure, having said which -- a half-built address space that is then
 * entered is a fault at an address with no obvious relation to the cause. */
static bool map_user_pages(uint64_t va, unsigned pages, uint32_t flags,
                           const char *what) {
    for (unsigned i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            kprintf("el0: out of memory mapping %s\n", what);
            return false;
        }
        memset((void *)(uintptr_t)P2V(phys), 0, PAGE_SIZE);

        int rc = vm_map_page(va + (uint64_t)i * PAGE_SIZE, phys, flags);
        if (rc != PT_OK) {
            kprintf("el0: vm_map_page(%p) for %s failed (%d)\n",
                    (void *)(uintptr_t)(va + i * PAGE_SIZE), what, rc);
            return false;
        }
    }
    return true;
}

int64_t el0_probe_run(void) {
    uint64_t size = (uint64_t)(el0_probe_blob_end - el0_probe_blob);
    uint64_t code_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    kprintf("el0: probe is %d bytes; mapping code at %p, stack at %p\n",
            (int)size, (void *)USER_CODE_VA,
            (void *)(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE));

    /* Code: readable and executable by EL0, and NOT writable. The kernel
     * writes it before the mapping is made read-only... which it cannot do
     * through a read-only mapping, so the copy goes through the DIRECT MAP
     * instead -- the kernel's own alias of the same physical pages. That is
     * what a direct map is for, and it is why user text never has to be
     * temporarily writable. */
    if (!map_user_pages(USER_CODE_VA, (unsigned)code_pages,
                        PT_USER | PT_EXEC, "probe text"))
        return -EMBK_ENOMEM;

    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys = vm_translate(USER_CODE_VA + off);
        uint64_t n = size - off < PAGE_SIZE ? size - off : PAGE_SIZE;
        memcpy((void *)(uintptr_t)P2V(phys), el0_probe_blob + off, n);
    }

    /* The instruction cache does not snoop the data cache on aarch64. We just
     * wrote code through a data mapping; without pushing it out to the point
     * of unification and invalidating I-cache, EL0 can fetch stale bytes --
     * which on a cold page means executing whatever was there before. This is
     * a class of bug x86 simply does not have (it has coherent I-caches), and
     * it appears as a random undefined-instruction fault at the entry point. */
    __asm__ volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");

    /* Stack: writable, and never executable. */
    if (!map_user_pages(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                        USER_STACK_PAGES, PT_USER | PT_WRITE, "probe stack"))
        return -EMBK_ENOMEM;

    exit_code = -EMBK_EINVAL;

    /* The return point. kernel_ctx_save returns 0 now and (nonzero) later,
     * when el0_exit longjmps back into it. */
    if (kernel_ctx_save(&exit_ctx) != 0) {
        kprintf("el0: back in EL1, program exited with %d\n", (int)exit_code);
        return exit_code;
    }

    in_user = true;
    kprintf("el0: eret to %p, sp %p\n",
            (void *)USER_CODE_VA, (void *)USER_STACK_TOP);

    arch_enter_user_mode(USER_CODE_VA, USER_STACK_TOP, 0, 0, 0);
    return -EMBK_EINVAL;   /* not reached */
}
