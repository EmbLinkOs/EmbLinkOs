/* sessprobe.c -- the witness for `test session`: one program, one mode per
 * claim, spawned by the kernel test into sessions it names.
 *
 *   info <user>      I am in a session for <user>, not the system's, without
 *                    the power to open sessions -- and my child is in mine
 *   same <id>        (the child above) my session is <id>
 *   mint             I cannot open a session: not by NEW_SESSION, not by
 *                    asking for EMBK_CAP_SESSION back
 *   kill <p> <k>     I cannot kill <p> (another session's) or <k> (a kernel
 *                    thread) by pid -- and I can kill my own child
 *   clipset <text>   I put <text> on the clipboard, my child sees it, and I
 *                    stay alive so the test can ask another session to look
 *   clipget          exit code = the bytes the clipboard holds for me
 *   linger           I start a child and exit without logging out
 *   logout           I start a child and log out
 *   route            I cannot take the console's ^C from another session
 *   sleep            I wait to be stopped
 *
 * Exit 0 means the claim held; anything else is the step that did not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embk.h"

static const char *self = "/data/apps/sessprobe/sessprobe.elf";

static int spawn_child(const char *mode, const char *arg) {
    char *a[] = { (char *)self, (char *)mode, (char *)arg, NULL };
    return (int)embk_spawn(self, a, NULL, 0);
}

/* The pid of my child, found the way a user finds anything: in `ps`. */
static uint32_t my_child_pid(void) {
    struct embk_proc_info rows[64];
    int n = embk_proc_list(rows, 64);
    uint32_t me = (uint32_t)embk_getpid();
    for (int i = 0; i < n; i++)
        if (rows[i].parent_pid == me && rows[i].state != 4) return rows[i].pid;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 100;
    const char *mode = argv[1];
    struct embk_session_info si;
    if (embk_session_info(&si) != 0) return 101;

    if (!strcmp(mode, "sleep")) {
        for (;;) embk_sleep_ms(1000);
    }
    if (!strcmp(mode, "same")) {
        return (argc > 2 && si.id == (uint32_t)strtoul(argv[2], 0, 10)) ? 0 : 1;
    }
    if (!strcmp(mode, "info")) {
        unsigned long caps = embk_getcaps();
        printf("sessprobe: session %u, user '%s', leader %u, caps %#lx\n",
               si.id, si.user, si.leader_pid, caps);
        if (si.id == 0) return 1;
        if (argc < 3 || strcmp(si.user, argv[2]) != 0) return 2;
        if (caps & EMBK_CAP_BIT(EMBK_CAP_SESSION)) return 3;
        if (si.leader_pid != (uint32_t)embk_getpid()) return 4;
        char id[16]; snprintf(id, sizeof id, "%u", si.id);
        int h = spawn_child("same", id);
        if (h < 0) return 5;
        return embk_wait(h) == 0 ? 0 : 6;
    }
    if (!strcmp(mode, "mint")) {
        struct embk_spawn_file_action act;
        memset(&act, 0, sizeof act);
        embk_action_new_session(&act, "mallory");
        char *a[] = { (char *)self, "sleep", NULL };
        int64_t r1 = embk_spawn(self, a, &act, 1);
        memset(&act, 0, sizeof act);
        embk_action_set_caps(&act, EMBK_CAP_BIT(EMBK_CAP_SESSION) | EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM));
        int64_t r2 = embk_spawn(self, a, &act, 1);
        printf("sessprobe: NEW_SESSION -> %lld, asking for CAP_SESSION -> %lld (both want %d)\n",
               (long long)r1, (long long)r2, -EMBK_EPERM);
        if (r1 >= 0) { embk_kill((int)r1); return 1; }
        if (r2 >= 0) { embk_kill((int)r2); return 2; }
        return (r1 == -EMBK_EPERM && r2 == -EMBK_EPERM) ? 0 : 3;
    }
    if (!strcmp(mode, "kill")) {
        if (argc < 4) return 100;
        int r_other = embk_proc_kill((uint32_t)strtoul(argv[2], 0, 10));
        int r_kthr  = embk_proc_kill((uint32_t)strtoul(argv[3], 0, 10));
        int h = spawn_child("sleep", NULL);
        if (h < 0) return 5;
        uint32_t child = 0;
        for (int i = 0; i < 50 && !child; i++) { child = my_child_pid(); if (!child) embk_sleep_ms(10); }
        int r_own = child ? embk_proc_kill(child) : -1;
        (void)embk_wait(h);
        printf("sessprobe: kill another session's -> %d, a kernel thread -> %d, my own child -> %d\n",
               r_other, r_kthr, r_own);
        if (r_other != -EMBK_EPERM) return 1;
        if (r_kthr != -EMBK_EPERM) return 2;
        if (r_own != 0) return 3;
        return 0;
    }
    if (!strcmp(mode, "clipset")) {
        if (argc < 3) return 100;
        if (embk_clip_set(argv[2], strlen(argv[2])) != 0) return 1;
        int h = spawn_child("clipget", NULL);
        if (h < 0) return 2;
        int seen = (int)embk_wait(h);
        printf("sessprobe: my own session's child sees %d bytes of the clipboard\n", seen);
        if (seen != (int)strlen(argv[2])) return 3;
        for (;;) embk_sleep_ms(1000);            /* stay: the test asks another session now */
    }
    if (!strcmp(mode, "clipget")) {
        char buf[256];
        int64_t n = embk_clip_get(buf, sizeof buf);
        return n < 0 ? 250 : (n > 200 ? 200 : (int)n);
    }
    if (!strcmp(mode, "linger")) {
        if (spawn_child("sleep", NULL) < 0) return 1;
        embk_sleep_ms(100);
        return 0;                                /* no logout: the session must end anyway */
    }
    if (!strcmp(mode, "logout")) {
        if (spawn_child("sleep", NULL) < 0) return 1;
        embk_sleep_ms(100);
        embk_session_end(0);
        return 99;                               /* only if logging out returned */
    }
    if (!strcmp(mode, "route")) {
        int h = spawn_child("sleep", NULL);
        if (h < 0) return 5;
        int64_t r = embk_console_interrupt_route(h);
        embk_kill(h);
        (void)embk_wait(h);
        printf("sessprobe: taking another session's console ^C -> %lld (want %d)\n",
               (long long)r, -EMBK_EPERM);
        return r == -EMBK_EPERM ? 0 : 1;
    }
    return 100;
}
