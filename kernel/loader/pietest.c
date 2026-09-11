/* kernel/loader/pietest.c -- does a position-independent executable MOVE?
 *
 * `test aslr` proves the kernel CHOOSES a different executable base for every
 * process. That is the cheaper half of the claim: a base nothing is loaded at
 * randomises nothing. This loads a real ET_DYN binary twice and asks it where
 * it ended up.
 *
 * SEQUENTIAL, NOT CONCURRENT. Two processes alive at once would also show two
 * addresses, but a serial spawn-and-wait shows they differ ACROSS TIME, which
 * is the property that matters: knowing where yesterday's process put its text
 * must tell you nothing about today's.
 */
#include "loader/pietest.h"
#include "loader/elf.h"
#include "process/process.h"
#include "fs/vfs.h"
#include "include/errno.h"
#include "include/kprintf.h"

/* The window process_layout_roll() draws an executable base from: EXEC_VA_BASE,
 * 64 GiB of it. The probe reports (text >> 12) & 0x7FFFFFFF, and the window's
 * base page number is exactly 2^32, so that mask leaves the page OFFSET into
 * the window and nothing else -- an answer at or above the window's page count
 * is text that landed somewhere it should not have. */
#define PIE_WINDOW_PAGES 0x1000000          /* 64 GiB / 4 KiB */
#define PIE_PROBE_FAILED 0x7FFFFFFF         /* the probe's own "I lied" code */

int pie_selftest_run(const char *witness)
{
    struct vfs_stat st;
    if (!witness || vfs_stat(witness, &st) != EMBK_OK) {
        kprintf("  [ -- ] %s is not on the image.\n", witness ? witness : "(null)");
        kprintf("         A PIE needs a -fPIC libc; build one and rebuild:\n");
        kprintf("           tools/newlib/build-newlib-emblink.sh --pic && make\n");
        return -EMBK_ENOENT;
    }

    int code[2] = { -1, -1 };
    int ok = 1;

    for (int i = 0; i < 2; i++) {
        char *a[] = { (char *)witness, NULL };
        int pid = process_create(witness, a, 1, NULL, 0);
        if (pid < 0) {
            kprintf("  [FAIL] run %d: could not spawn (%d)\n", i, pid);
            ok = 0;
            continue;
        }
        code[i] = process_wait((uint32_t)pid);
        if (code[i] == PIE_PROBE_FAILED) {
            kprintf("  [FAIL] run %d: the probe ran and its OWN checks failed"
                    " (its output is above)\n", i);
            ok = 0;
        } else if (code[i] < 0 || code[i] >= PIE_WINDOW_PAGES) {
            kprintf("  [FAIL] run %d: text page %d is outside the executable window\n",
                    i, code[i]);
            ok = 0;
        } else {
            kprintf("  run %d: text at window page %d (0x%llx)\n", i, code[i],
                    (unsigned long long)(EXEC_VA_BASE + ((uint64_t)code[i] << 12)));
        }
    }

    if (ok) {
        int moved = (code[0] != code[1]);
        kprintf("  [%s] the two runs loaded at %s addresses\n",
                moved ? " ok " : "FAIL", moved ? "different" : "THE SAME");
        if (!moved) ok = 0;
    }
    return ok ? 0 : -EMBK_EINVAL;
}
