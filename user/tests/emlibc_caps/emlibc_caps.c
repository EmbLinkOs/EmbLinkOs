/* emlibc_caps.c -- exercises the part of EmbLinkOS that a POSIX libc
 * fundamentally cannot express: the capability model + handle-based spawn,
 * through emlibc's <process.h>. Built against emlibc, not newlib.
 *
 * One binary, two roles (chosen by argv):
 *
 *   parent (no arg): reads its OWN capabilities, then SPAWNS itself as a child
 *     with an ATTENUATED set -- FILESYSTEM only, dropping NETWORK -- and waits
 *     on the child by HANDLE (not pid). Its exit code is the child's.
 *
 *   child ("child"): confirms it was born with FEWER capabilities than its
 *     parent (has FILESYSTEM, does NOT have NETWORK), then tries to spawn a
 *     grandchild REQUESTING a capability it does not hold (NETWORK). The kernel
 *     must refuse -- you cannot grant authority you were never given. It writes
 *     a one-line witness to disk (the serial-checkable proof) and exits 42 iff
 *     attenuation AND the invariant both held.
 *
 * None of this -- reading your authority, handing a child a strict subset, the
 * monotonic "no process exceeds its parent" invariant -- exists in newlib.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <process.h>

static int run_child(const char *self)
{
    int fs  = em_have_cap(EM_CAP_FILESYSTEM);
    int net = em_have_cap(EM_CAP_NETWORK);
    printf("child: caps=0x%lx fs=%d net=%d\n", em_getcaps(), fs, net);

    /* The invariant: request a capability we do NOT hold for a grandchild.
     * The grandchild path EXISTS (it is us), so the ONLY reason to fail is the
     * over-request -- proving the kernel refuses to grant what we lack. */
    em_spawn_action over;
    em_action_set_caps(&over, EM_CAP_BIT(EM_CAP_NETWORK));
    char *gargv[] = { (char *)self, (char *)"gc", NULL };
    int gh = em_spawn(self, gargv, NULL, &over, 1);
    int refused = (gh < 0);
    printf("child: over-request NETWORK -> handle=%d errno=%d (%s)\n",
           gh, errno, refused ? "refused" : "GRANTED?!");
    if (!refused) em_kill(gh);   /* should never happen; clean up if it did */

    /* Witness to disk (needs the FILESYSTEM cap we were granted). */
    FILE *f = fopen("/data/tmp/emcaps.out", "w");
    if (f) {
        fprintf(f, "child: fs=%d net=%d overreach=%s\n",
                fs, net, refused ? "refused" : "granted");
        fclose(f);
    }

    int ok = (fs == 1 && net == 0 && refused);
    return ok ? 42 : 1;
}

static int run_parent(const char *self)
{
    unsigned long caps = em_getcaps();
    printf("parent: caps=0x%lx (fs=%d net=%d)\n", caps,
           em_have_cap(EM_CAP_FILESYSTEM), em_have_cap(EM_CAP_NETWORK));

    /* Spawn ourselves as the child with an ATTENUATED set: FILESYSTEM only.
     * We hold NETWORK; the child will not -- the parent chooses. */
    em_spawn_action drop;
    em_action_set_caps(&drop, EM_CAP_BIT(EM_CAP_FILESYSTEM));
    char *cargv[] = { (char *)self, (char *)"child", NULL };
    int h = em_spawn(self, cargv, NULL, &drop, 1);
    if (h < 0) { printf("parent: spawn child failed (errno=%d)\n", errno); return 2; }

    int code = em_wait(h);       /* by HANDLE, not pid */
    printf("parent: child handle=%d exited %d\n", h, code);
    return code;                 /* propagate the child's verdict */
}

int main(int argc, char **argv)
{
    const char *self = argv[0];
    if (argc >= 2 && strcmp(argv[1], "child") == 0) return run_child(self);
    if (argc >= 2 && strcmp(argv[1], "gc") == 0)    return 7;  /* must never run */
    return run_parent(self);
}
