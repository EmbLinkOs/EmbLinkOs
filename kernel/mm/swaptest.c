#include "mm/swaptest.h"
#include "mm/swap.h"
#include "mm/vm_object.h"
#include "mm/vma.h"
#include "mm/pmm.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "process/process.h"
#include "kworker/kworker.h"
#include "drivers/timer/timer.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"

/* See mm/swaptest.h. This is the body `test swap` had in kernel/selftests.c,
 * moved here so the aarch64 boot -- which has no console to type a command
 * into -- can run it too. */
int swap_selftest_run(const char *wp, bool quick) {
    if (!swap_available()) {
        kprintf("  [info] no swap store (attach build/swap.img): not run\n");
        return -1;
    }
    struct vfs_stat wst;
    if (vfs_stat(wp, &wst) != EMBK_OK) { kprintf("  [FAIL] %s not on image\n", wp); return -1; }

    struct swap_stats st;
    swap_stats_get(&st);
    uint64_t free_mib = pmm_free_pages() / 256;
    uint64_t swap_mib = (st.nslots - st.used) / 256;
    uint64_t want = free_mib * 135 / 100;            /* more than fits ... */
    uint64_t cap  = free_mib + swap_mib / 2;         /* ... but not more than fits in RAM + half the store */
    if (want > cap) want = cap;
    if (want < 8)   want = 8;

    kprintf("  [swap] %llu MiB free, %llu MiB of store: the witness will touch %llu MiB, %s\n",
            (unsigned long long)free_mib, (unsigned long long)swap_mib, (unsigned long long)want,
            quick ? "twice" : "four times");

    /* mmap memory, then malloc's (sbrk -- the heap every ordinary program
     * lives in), then a hot quarter re-read every round while cold pages
     * stream past -- first with the reclaimer in pure fault order, then with
     * second chance (the accessed bit). Each run is judged on its own
     * counters; the hot pair's swap-in counts side by side are the claim. */
    static const char *const modes[] = { "mmap", "heap", "hot", "hot" };
    int nmodes = quick ? 2 : 4;
    int ok = 1;
    uint64_t hot_ins[2] = { 0, 0 };
    for (int m = 0; m < nmodes; m++) {
        if (m == 2) vmo_set_second_chance(false);
        if (m == 3) vmo_set_second_chance(true);

        struct swap_stats s0, s1;
        struct vmo_stats  v0, v1;
        struct vm_fault_stats f0, f1;
        uint64_t pb0, pa0, pb1, pa1;                 /* pmm scan bits / allocations */
        swap_stats_get(&s0);
        vmo_stats_get(&v0);
        vm_fault_stats(&f0);
        pmm_scan_stats(&pb0, &pa0);
        vm_fault_reset_max();

        char arg[24];
        snprintf(arg, sizeof arg, "%llu", (unsigned long long)want);
        char *a[] = { (char *)wp, arg, (char *)modes[m], NULL };
        uint64_t t0 = timer_uptime_ms();
        int pid = process_create(wp, a, 3, NULL, 0);
        if (pid < 0) { kprintf("  [FAIL] spawn %d\n", pid); return -1; }
        int rc = process_wait((uint32_t)pid);
        uint64_t dt = timer_uptime_ms() - t0;

        swap_stats_get(&s1);
        vmo_stats_get(&v1);
        vm_fault_stats(&f1);
        pmm_scan_stats(&pb1, &pa1);

        kprintf("  [%s] %s%s: the witness exited %d after %llu ms (0 = every page came back intact)\n",
                rc == 0 ? "ok" : "FAIL", modes[m],
                m == 2 ? " (fault-order LRU)" : m == 3 ? " (second chance)" : "",
                rc, (unsigned long long)dt);
        if (rc != 0) ok = 0;
        if (m >= 2) hot_ins[m - 2] = s1.ins - s0.ins;

        /* WHERE THE TIME WENT. Not asserted; this is the breakdown that
         * decides what to build next, and it is printed so that a claim
         * about it can be checked against the run that made it. */
        uint64_t calls = v1.reclaim_calls - v0.reclaim_calls;
        kprintf("  [info] %llu faults resolved in %llu ms inside vm_fault (%llu us each, disk waits included)\n",
                (unsigned long long)(f1.handled - f0.handled),
                (unsigned long long)((f1.ns - f0.ns) / 1000000),
                (unsigned long long)((f1.handled - f0.handled) ? (f1.ns - f0.ns) / 1000 / (f1.handled - f0.handled) : 0));
        kprintf("  [info]   the worst single fault took %llu ms\n", (unsigned long long)(f1.max_ns / 1000000));
        kprintf("  [info]   of which: in the object (wire, fill, swap-in) %llu ms -- %llu ms of it waiting for the cache lock -- installing PTEs %llu ms\n",
                (unsigned long long)((f1.wire_ns - f0.wire_ns) / 1000000),
                (unsigned long long)((v1.wire_lock_wait_ns - v0.wire_lock_wait_ns) / 1000000),
                (unsigned long long)((f1.map_ns - f0.map_ns) / 1000000));
        kprintf("  [info]   a fill: frame %llu ms, zeroing %llu ms, record %llu ms; pmm scanned %llu bits over %llu allocations\n",
                (unsigned long long)((v1.fill_frame_ns - v0.fill_frame_ns) / 1000000),
                (unsigned long long)((v1.fill_zero_ns - v0.fill_zero_ns) / 1000000),
                (unsigned long long)((v1.fill_record_ns - v0.fill_record_ns) / 1000000),
                (unsigned long long)(pb1 - pb0), (unsigned long long)(pa1 - pa0));
        kprintf("  [info] reclaim ran %llu times, examined %llu LRU nodes (%llu per call)\n",
                (unsigned long long)calls,
                (unsigned long long)(v1.lru_visited - v0.lru_visited),
                (unsigned long long)(calls ? (v1.lru_visited - v0.lru_visited) / calls : 0));
        kprintf("  [info] time: reclaim %llu ms total, of which swap I/O %llu ms and unmapping %llu ms\n",
                (unsigned long long)((v1.reclaim_ns - v0.reclaim_ns) / 1000000),
                (unsigned long long)((v1.swap_ns - v0.swap_ns) / 1000000),
                (unsigned long long)((v1.unmap_ns - v0.unmap_ns) / 1000000));

        uint64_t outs = s1.outs - s0.outs, ins = s1.ins - s0.ins;
        kprintf("  [%s] pages went out to the store: %llu (%llu MiB), %llu of them as %llu cluster writes\n",
                outs ? "ok" : "FAIL", (unsigned long long)outs, (unsigned long long)(outs / 256),
                (unsigned long long)(s1.cluster_pages - s0.cluster_pages),
                (unsigned long long)(s1.clusters - s0.clusters));
        if (!outs) ok = 0;
        kprintf("  [%s] pages came back from it: %llu, of which %llu read ahead in %llu cluster reads (%llu faults swapped in)\n",
                ins ? "ok" : "FAIL", (unsigned long long)ins,
                (unsigned long long)(v1.readahead_pages - v0.readahead_pages),
                (unsigned long long)(v1.readahead_reads - v0.readahead_reads),
                (unsigned long long)((v1.swapins - v0.swapins) - (v1.readahead_pages - v0.readahead_pages)));
        if (!ins) ok = 0;

        /* The witness has exited and is reaped, but its address space is
         * torn down by the kworker: wait for that before counting. */
        for (int spin = 0; spin < 600 && kworker_pending(); spin++) sched_sleep_ms(5);
        swap_stats_get(&s1);
        vmo_stats_get(&v1);

        /* WHAT MUST HOLD AFTERWARDS -- and what must not be asserted. The
         * other processes' heaps and stacks are anonymous objects too, and
         * the witness pushed them out to the store: those slots are in use
         * afterwards and rightly so. So: every slot in use is a swapped page
         * somebody owns (nothing leaked), and the anonymous pages in the
         * system -- resident plus swapped -- are what they were before the
         * witness, within what the desktop allocates in the meantime (a
         * bound of 4 MiB against a witness of 200+ MiB is not a hedge). */
        uint64_t before = v0.anon_pages + v0.swapped_pages;
        uint64_t after  = v1.anon_pages + v1.swapped_pages;
        {
            uint64_t used = 0, dang = 0, dup = 0;
            uint64_t outp = vmo_audit_swap(&used, &dang, &dup);
            bool sound = (used == outp && dang == 0 && dup == 0);
            kprintf("  [%s] every slot has an owner: %llu slots in use, %llu pages out on the store; %llu name a slot not held, %llu share a slot\n",
                    sound ? "ok" : "FAIL", (unsigned long long)used, (unsigned long long)outp,
                    (unsigned long long)dang, (unsigned long long)dup);
            if (!sound) ok = 0;
        }
        kprintf("  [%s] the witness's pages are gone: %llu anonymous pages before (%llu resident, %llu swapped), %llu after (%llu, %llu)\n",
                after <= before + 1024 ? "ok" : "FAIL",
                (unsigned long long)before, (unsigned long long)v0.anon_pages, (unsigned long long)v0.swapped_pages,
                (unsigned long long)after, (unsigned long long)v1.anon_pages, (unsigned long long)v1.swapped_pages);
        if (after > before + 1024) ok = 0;
    }

    if (!quick) {
        uint64_t wpages = want * 256;
        kprintf("  [%s] the hot set stays: %llu pages, swapped in %llu times in fault order, %llu with second chance (bound %llu)\n",
                hot_ins[1] <= wpages * 13 / 10 ? "ok" : "FAIL",
                (unsigned long long)wpages, (unsigned long long)hot_ins[0],
                (unsigned long long)hot_ins[1], (unsigned long long)(wpages * 13 / 10));
        if (hot_ins[1] > wpages * 13 / 10) ok = 0;
    }
    return ok ? 0 : -1;
}
