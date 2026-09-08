/* user/bin/init.c -- EmbLink OS init: the first user process, the ROOT of
 * userspace authority.
 *
 * The kernel spawns exactly ONE user process at boot -- this one (see
 * kernel/main.c). init holds the full namespace + all capabilities; every other
 * process's authority is a NARROWING of init's (docs/USERSPACE_v2.md -- "authority
 * IS the namespace"). This is the spine the old design was missing: until now the
 * kernel spawned the desktop (home.elf) DIRECTLY, so there was no root of
 * authority and no supervisor -- a died desktop was a dead session with nobody to
 * notice. Now the desktop is init's CHILD.
 *
 * (Numerically this is not pid 1 -- the per-core idle kthreads take the low pids
 * -- but it is the first *user* process and the root of the user authority tree;
 * "pid 1" throughout the docs names that role, not the integer.)
 *
 * UP1 scope is deliberately small (the per-process namespace mechanism lands in
 * UP2, docs/USERSPACE_v2.md):
 *   1. Bring up the session desktop (/system/bin/home.elf).
 *   2. Supervise it -- if it exits or crashes, reap the zombie and respawn, so
 *      the machine always has a desktop.
 *   3. Never exit: init staying alive is what keeps userspace alive.
 *
 * Orphaned grandchildren (apps left behind if the desktop dies) are reaped by
 * the KERNEL (process.c: a process with no live parent is freed directly), so
 * init only needs to watch its own direct child.
 *
 * Freestanding: own _start, no libc -- init carries no dependency it would then
 * have to keep alive. Built with user/lib/user.ld, like primtest.elf.
 */

#include "embk.h"

#define DESKTOP      "/system/bin/home.elf"
#define SETUP        "/system/bin/setup.elf"
#define LOGIN        "/system/bin/login.elf"
#define SHADOW       "/etc/shadow"
#define NS_ACTS_MAX  8
#define SESSION_FD   9

/* Development shortcut: authentication is already validated, so UI work can
 * boot straight into this disposable session. Set to 0 to restore the normal
 * first-boot setup + password login flow. */
#define DEV_AUTOLOGIN 1
#define DEV_USER      "yves"

static void log_line(const char *s) { embk_puts(1, s); }

/* Append a signed decimal to a small buffer (no libc here). Returns end ptr. */
static char *put_dec(char *p, long v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

/* Append a NUL-terminated string; returns the new end. */
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

/* Read a small file fully into buf (NUL-terminated). Returns length or -1. */
static int read_small(const char *path, char *buf, int cap) {
    int64_t fd = embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = 0;
    while (n < cap - 1) {
        int64_t got = embk_read((int)fd, buf + n, cap - 1 - n);
        if (got <= 0) break;
        n += (int)got;
    }
    embk_close((int)fd);
    buf[n] = 0;
    return n;
}

/* Load user <user>'s SESSION PROFILE -- /home/<user>/user.ns -- into
 * NS_BIND spawn actions (docs/USERSPACE_v2.md UP3). Same "<ro|rw> <prefix>"
 * manifest format as the app manifests (UP4); '#' comments and blanks ignored.
 * Returns the binding count (0 => no profile, so the caller launches the desktop
 * with a plain full-inherit view and it always comes up). `desc` gets a short
 * summary for the log. This is the whole of "instantiate a user's session": a
 * session IS a namespace, and it is read from the user's own home. */
static int load_user_profile(const char *user,
                             struct embk_spawn_file_action *acts, int max,
                             char *desc, int desc_cap) {
    char path[128], *p = path;
    p = put_str(p, "/home/");
    p = put_str(p, user);
    p = put_str(p, "/user.ns");
    *p = 0;

    char buf[512];
    int n = read_small(path, buf, sizeof buf);
    if (n <= 0) return 0;

    int na = 0, dn = 0;
    if (desc_cap) desc[0] = 0;
    for (int i = 0; i < n && na < max; ) {
        while (i < n && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }

        int ms = i;
        while (i < n && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        int mlen = i - ms, mode;
        if      (mlen==2 && buf[ms]=='r' && buf[ms+1]=='o') mode = EMBK_NS_RO;
        else if (mlen==2 && buf[ms]=='r' && buf[ms+1]=='w') mode = EMBK_NS_RW;
        else { while (i < n && buf[i] != '\n') i++; continue; }

        while (i < n && (buf[i]==' '||buf[i]=='\t')) i++;
        int ps = i;
        while (i < n && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        int plen = i - ps;
        if (plen == 0 || buf[ps] != '/' || plen > 200) { while (i<n && buf[i]!='\n') i++; continue; }

        char prefix[208];
        for (int k = 0; k < plen; k++) prefix[k] = buf[ps + k];
        prefix[plen] = 0;
        embk_action_ns_bind(&acts[na], prefix, mode);
        if (desc_cap && dn + plen + 6 < desc_cap) {
            if (dn) { desc[dn++]=','; desc[dn++]=' '; }
            desc[dn++]='r'; desc[dn++]=(mode==EMBK_NS_RO)?'o':'w'; desc[dn++]=' ';
            for (int k = 0; k < plen; k++) desc[dn++] = prefix[k];
            desc[dn] = 0;
        }
        na++;
    }
    return na;
}

/* Live proof of the UP2 namespace: init runs in ring 3 holding the inherited
 * global view, where /system is a READ-ONLY binding. Opening a /system file for
 * write must be refused (-EMBK_EROFS) by the kernel's namespace write-gate,
 * BEFORE the file is even resolved. A pure read of the same file still works.
 * This is the sealed-OS invariant enforced by naming, not by uid/rwx. */
static void ns_selfcheck(void) {
    int64_t w = embk_open("/system/bin/home.elf", EMBK_O_WRONLY, 0);
    if (w < 0) {
        log_line("init: ns: /system is read-only for userspace (write refused) -- OK\n");
    } else {
        embk_close((int)w);
        log_line("init: ns: WARNING -- /system accepted a write (namespace RO not enforced)\n");
    }
    int64_t r = embk_open("/system/bin/home.elf", EMBK_O_RDONLY, 0);
    if (r >= 0) { embk_close((int)r); log_line("init: ns: /system still readable -- OK\n"); }
    else          log_line("init: ns: WARNING -- /system unreadable (over-restricted)\n");
}

void _start(long argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    log_line("init: up -- root of EmbLink userspace authority\n");
    ns_selfcheck();

    for (;;) {
#if DEV_AUTOLOGIN
        char user[32] = DEV_USER;
        log_line("init: DEV auto-login as '" DEV_USER "'\n");
#else
        struct embk_stat shadow;
        if (embk_stat(SHADOW, &shadow) < 0 || shadow.size == 0) {
            char *argv_setup[] = { (char *)SETUP, NULL };
            int setup = (int)embk_spawn(SETUP, argv_setup, NULL, 0);
            if (setup < 0) {
                log_line("init: could not launch first-boot setup; retrying\n");
                embk_sleep_ms(1000);
                continue;
            }
            log_line("init: no account found; first-boot setup started\n");
            if (embk_wait(setup) != 0) {
                log_line("init: setup closed without creating an account\n");
                continue;
            }
        }

        int pipe_handles[2];
        if (embk_pipe(pipe_handles) != 0) {
            log_line("init: could not create login channel\n");
            embk_sleep_ms(1000);
            continue;
        }
        struct embk_spawn_file_action login_action = {0};
        login_action.kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;
        login_action.target_fd = 3;
        login_action.src_obj_handle = pipe_handles[1];
        char *login_argv[] = { (char *)LOGIN, NULL };
        int login = (int)embk_spawn(LOGIN, login_argv, &login_action, 1);
        embk_close_handle(pipe_handles[1]);
        if (login < 0) {
            embk_close_handle(pipe_handles[0]);
            log_line("init: could not launch login\n");
            embk_sleep_ms(1000);
            continue;
        }
        embk_fd_install_obj(pipe_handles[0], SESSION_FD);
        embk_close_handle(pipe_handles[0]);
        int login_code = embk_wait(login);
        char user[32];
        int64_t user_len = embk_read(SESSION_FD, user, sizeof user - 1);
        embk_close(SESSION_FD);
        if (login_code != 0 || user_len <= 0 || user_len >= (int64_t)sizeof user) {
            log_line("init: login ended without an authenticated user\n");
            continue;
        }
        user[user_len] = 0;
#endif

        struct embk_spawn_file_action sacts[NS_ACTS_MAX];
        char nsdesc[192];
#if DEV_AUTOLOGIN
        /* A fresh COW image has no account directories. Create only the home
         * required by the development desktop and give it the same namespace
         * policy as a real session profile. */
        (void)embk_mkdir("/home/" DEV_USER);
        (void)embk_mkdir("/home/" DEV_USER "/Desktop");
        (void)embk_mkdir("/home/" DEV_USER "/Documents");
        (void)embk_mkdir("/home/" DEV_USER "/Downloads");
        (void)embk_mkdir("/home/" DEV_USER "/Music");
        (void)embk_mkdir("/home/" DEV_USER "/Pictures");
        (void)embk_mkdir("/home/" DEV_USER "/Videos");
        (void)embk_mkdir("/home/" DEV_USER "/Trash");
        embk_action_ns_bind(&sacts[0], "/system", EMBK_NS_RO);
        embk_action_ns_bind(&sacts[1], "/data/apps", EMBK_NS_RO);
        embk_action_ns_bind(&sacts[2], "/home/" DEV_USER, EMBK_NS_RW);
        embk_action_ns_bind(&sacts[3], "/run", EMBK_NS_RW);
        int snacts = 4;
        char *nd = nsdesc;
        nd = put_str(nd, "ro /system, ro /data/apps, rw /home/" DEV_USER ", rw /run");
        *nd = 0;
#else
        int snacts = load_user_profile(user, sacts, NS_ACTS_MAX, nsdesc, sizeof nsdesc);
        if (snacts <= 0) {
            log_line("init: authenticated user has no valid session profile\n");
            continue;
        }
#endif

        char home[80], env_user[48], env_home[96], env_pwd[96];
        char *p = home;
        p = put_str(p, "/home/"); p = put_str(p, user); *p = 0;
        p = env_user; p = put_str(p, "USER="); p = put_str(p, user); *p = 0;
        p = env_home; p = put_str(p, "HOME="); p = put_str(p, home); *p = 0;
        p = env_pwd; p = put_str(p, "PWD="); p = put_str(p, home); *p = 0;
        char *session_env[] = { env_user, env_home, env_pwd, NULL };

        {
            char b[256], *q = b;
            q = put_str(q, "init: authenticated session '"); q = put_str(q, user);
            q = put_str(q, "' -> ns["); q = put_str(q, nsdesc); q = put_str(q, "]\n"); *q = 0;
            log_line(b);
        }

        char *dargv[] = { (char *)DESKTOP, NULL };
        int h = (int)embk_spawn_env(DESKTOP, dargv, session_env, sacts, snacts);
        if (h < 0) {
            log_line("init: could not spawn the confined desktop\n");
            embk_sleep_ms(1000);
            continue;
        }
        log_line("init: desktop session started\n");

        /* Block until the desktop exits, then reap it. The wait frees BOTH the
         * zombie process slot and this spawn handle -- without it, every restart
         * would leak one of init's 16 handles. */
        int code = embk_wait(h);

        char b[96], *q = b;
        const char *pre = "init: desktop exited (code ";
        while (*pre) *q++ = *pre++;
        q = put_dec(q, code);
        const char *post = ") -- returning to login\n";
        while (*post) *q++ = *post++;
        *q = 0;
        log_line(b);

        embk_sleep_ms(200);   /* never a hot crash-loop */
    }
}
