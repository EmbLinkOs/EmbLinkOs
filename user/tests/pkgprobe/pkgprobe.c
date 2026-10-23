/* pkgprobe -- a tiny installed app that verifies it runs under EXACTLY the
 * authority its package declared. Bundled as an EMBX declaring {filesystem} with
 * a manifest granting `ro /system` + `rw /data/apps/pkgprobe`. When `pkg run`
 * spawns it (SET_CAPS + NS_BIND from that manifest), it must find:
 *   - it HOLDS filesystem, and does NOT hold network (undeclared -> ungranted);
 *   - it can name a GRANTED prefix (/system), and CANNOT name an ungranted one
 *     (/data/users) -- namespace absence is physical, not advisory.
 * Exit 0 only if the grant matches the declaration. This is the proof that
 * install mediates authority the KERNEL enforces, not the installer promises. */
#include <stdio.h>
#include "embk.h"

int main(void) {
    unsigned long caps = embk_getcaps();
    int has_fs  = (caps & EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)) != 0;
    int has_net = (caps & EMBK_CAP_BIT(EMBK_CAP_NETWORK)) != 0;
    printf("PKGPROBE caps=0x%lx (filesystem=%d network=%d)\n", caps, has_fs, has_net);

    struct embk_stat st;
    int granted_ok   = (embk_stat("/system", &st) == 0);       /* granted ro */
    int ungranted_no = (embk_stat("/data/users", &st) != 0);   /* not granted -> unnameable */
    printf("PKGPROBE reach /system=%d  deny /data/users=%d\n", granted_ok, ungranted_no);

    int pass = has_fs && !has_net && granted_ok && ungranted_no;
    printf("PKGPROBE %s\n", pass ? "CONFINED-AS-DECLARED" : "MISMATCH");
    return pass ? 0 : 1;
}
