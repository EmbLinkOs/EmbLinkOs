#include "include/usercopy.h"
#include "include/uaccess_guard.h"
#include "arch/aarch64/mm/pagetable.h"
#include "mm/pmm.h"
#include "mm/vma.h"   /* vm_fault: a page not yet touched is not a bad pointer */
#include "process/process.h"   /* current_process_atomic */
#include "include/errno.h"
#include "include/kstring.h"

/* The aarch64 half of kernel/include/usercopy.h -- docs/ARM64.md phase A5.
 *
 * The privilege boundary. Every pointer a ring-3 caller hands the kernel is
 * checked here before it is ever dereferenced, because "user gave us an
 * address" and "that address is safe for the kernel to touch" are different
 * claims, and conflating them is how a syscall becomes an arbitrary-read
 * primitive.
 *
 * ONE THING IS EASIER HERE THAN ON x86, and it is worth saying why rather than
 * leaving it as an unexplained absence. The x86 implementation has to reason
 * about canonical addresses and about the kernel and user halves sharing one
 * page-table root. On aarch64 the split is in the hardware: TTBR0_EL1
 * translates the bottom of the address space and TTBR1_EL1 the top, selected
 * by bit 63. So "is this a user address" is a single bit test that the CPU
 * itself agrees with, and a user pointer that somehow named kernel memory
 * would not merely be rejected here -- it could not be translated by TTBR0 at
 * all.
 *
 * THE CHECK IS STILL NECESSARY. Bit 63 says which half; it does not say the
 * page is mapped, and it does not stop a caller from passing a valid user
 * pointer to a page it does not own -- which is fine, since it owns the whole
 * address space TTBR0 describes, and that is the point of the boundary. */

/* Anything with bit 63 set is translated by TTBR1 and is therefore kernel
 * memory by construction. Also reject the top of the user half as a cheap
 * catch for sign-extension mistakes: no user mapping is created up there. */
#define USER_VA_LIMIT 0x0000800000000000ULL

/* Same public counters the x86 side keeps, for the same reason: "the check is
 * in place" and "the check is passing" are different claims. */
static struct usercopy_stat_pub stats;

bool access_ok(const void *user_ptr, size_t len) {
    uint64_t start = (uint64_t)(uintptr_t)user_ptr;

    stats.calls++;

    if (len == 0)
        return true;

    /* Overflow first: start+len wrapping past the top would otherwise make a
     * wild range look like a small one. */
    if (start >= USER_VA_LIMIT || len > USER_VA_LIMIT ||
        start > USER_VA_LIMIT - len) {
        stats.faults++;
        return false;
    }

    /* Every page in the range must actually translate. Walking the tables is
     * the honest check -- the alternative, "it looks like a user address", is
     * what lets an unmapped pointer through and turns a bad syscall argument
     * into a kernel fault. */
    uint64_t first = start & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last  = (start + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t p = first; p <= last; p += PAGE_SIZE) {
        if (vm_translate(p) == 0) {
            /* NOT MAPPED YET IS NOT THE SAME AS NOT ALLOWED. With demand
             * paging a process's mmap'd buffer has no pages until it touches
             * them -- and handing that buffer straight to read() is the most
             * ordinary thing a program does. Rejecting it here would make
             * every such call EFAULT, so the page is FAULTED IN instead,
             * which is what the process's own first touch would have done.
             *
             * write=false: this only asks the page to exist with whatever
             * permissions its VMA grants. A copy_to_user into a read-only
             * mapping still fails at the store, caught by the uaccess guard,
             * which is the correct place -- only the copy knows its
             * direction. */
            if (!vm_fault(current_process_atomic(), p, false, false)) {
                stats.faults++;
                return false;
            }
        }
    }
    return true;
}

/* access_ok() proves every page is mapped; the GUARD survives it stopping
 * being true between then and the copy. Another thread of the same process, on
 * another core, can unmap underneath us -- and since A9 there ARE other cores.
 * See include/uaccess_guard.h. Without it that is an EL1 data abort inside
 * memcpy, which is a panic a user program can cause on purpose. */
int copy_from_user(void *kernel_dst, const void *user_src, size_t len) {
    if (!access_ok(user_src, len))
        return -EMBK_EFAULT;

    if (!uaccess_arm())
        return -EMBK_EFAULT;          /* raced: the page went away mid-copy */
    memcpy(kernel_dst, user_src, len);
    uaccess_disarm();
    return EMBK_OK;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    if (!access_ok(user_dst, len))
        return -EMBK_EFAULT;

    if (!uaccess_arm())
        return -EMBK_EFAULT;
    memcpy(user_dst, kernel_src, len);
    uaccess_disarm();
    return EMBK_OK;
}

int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    if (max_len == 0)
        return -EMBK_EFAULT;

    /* One byte at a time, because the length is not known until the NUL is
     * found -- a single fixed-length access_ok() would have to guess, and
     * guessing high rejects valid short strings at the end of a mapping. */
    for (size_t i = 0; i < max_len; i++) {
        const char *p = user_src + i;
        if (!access_ok(p, 1))
            return -EMBK_EFAULT;
        kernel_dst[i] = *p;
        if (kernel_dst[i] == '\0')
            return (int)i;
    }
    return -EMBK_ENAMETOOLONG;
}

void usercopy_stat_get(struct usercopy_stat_pub *out) {
    if (out)
        *out = stats;
}

void usercopy_stat_reset(void) {
    struct usercopy_stat_pub zero = {0, 0, 0, 0};
    stats = zero;
}
