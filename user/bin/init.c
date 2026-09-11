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
#include "session_policy.h"   /* the clamp on every session profile */

#define DESKTOP      "/system/bin/home.elf"
#define SETUP        "/system/bin/setup.elf"
#define LOGIN        "/system/bin/login.elf"
#define SHADOW       "/etc/shadow"
#define PASSWD       "/etc/passwd"
#define NS_ACTS_MAX  9           /* up to 8 bindings + the NEW_SESSION action */
#define SESSION_FD   9

/* Development shortcut: authentication is already validated, so UI work can
 * boot straight into this disposable session. Set to 0 to restore the normal
 * first-boot setup + password login flow.
 *
 * It opens the session at BOOT and after a CRASH -- a desktop that dies while
 * UI work is under way comes straight back. It does not survive a LOGOUT: a
 * user who logs out has asked for the login screen, and logging them straight
 * back in would make logging out meaningless. So even a development image
 * reaches the real setup and greeter, which is how they are tested.
 *
 * A PRODUCTION image is built with `make AUTOLOGIN=0`: no session opens
 * without a password, from the first boot on. */
#ifndef DEV_AUTOLOGIN
#define DEV_AUTOLOGIN 1
#endif
#define DEV_USER      "yves"

/* init links no libc, and the compiler emits calls to these two for struct
 * copies and zero-initialisers (a spawn action is 280 bytes). The attribute
 * stops GCC recognising each loop as the very function it implements and
 * compiling it into a call to itself. */
#define NO_LOOP_IDIOMS __attribute__((optimize("no-tree-loop-distribute-patterns")))
NO_LOOP_IDIOMS void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d;
}
NO_LOOP_IDIOMS void *memcpy(void *d, const void *src, unsigned long n) {
    unsigned char *p = d; const unsigned char *q = src; while (n--) *p++ = *q++; return d;
}

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

/* Is <user> an administrator? The gid field of its /etc/passwd line == 10
 * (user/lib/auth.h). Read here, not taken from the profile: whether a clamp
 * applies cannot be decided by the file being clamped. */
static int user_is_admin(const char *user) {
    char buf[2048];
    int n = read_small(PASSWD, buf, sizeof buf);
    int ul = 0; while (user[ul]) ul++;
    for (int i = 0; i < n; ) {
        int ls = i;
        while (i < n && buf[i] != '\n') i++;
        int le = i++;
        int match = (le - ls > ul && buf[ls + ul] == ':');
        for (int k = 0; match && k < ul; k++) match = (buf[ls + k] == user[k]);
        if (!match) continue;
        int field = 0, p = ls;
        while (p < le && field < 3) { if (buf[p] == ':') field++; p++; }
        long gid = 0;
        while (p < le && buf[p] >= '0' && buf[p] <= '9') gid = gid * 10 + (buf[p++] - '0');
        return gid == 10;
    }
    return 0;
}

/* The default session: what every account gets without a profile. */
static int default_profile(const char *user, struct embk_spawn_file_action *acts, char *desc, int desc_cap) {
    static char home[48];
    char *p = home;
    p = put_str(p, "/home/"); p = put_str(p, user); *p = 0;
    embk_action_ns_bind(&acts[0], "/system", EMBK_NS_RO);
    embk_action_ns_bind(&acts[1], "/data/apps", EMBK_NS_RO);
    embk_action_ns_bind(&acts[2], home, EMBK_NS_RW);
    embk_action_ns_bind(&acts[3], "/run", EMBK_NS_RW);
    char *d = desc;
    if (desc_cap > 96) {
        d = put_str(d, "ro /system, ro /data/apps, rw "); d = put_str(d, home);
        d = put_str(d, ", rw /run"); *d = 0;
    }
    return 4;
}

/* Load <user>'s SESSION PROFILE -- /etc/sessions/<user>.ns -- into NS_BIND
 * spawn actions (docs/USERSPACE_v2.md UP3). "<ro|rw> <prefix>" per line, '#'
 * comments. Every binding passes through embk_session_grant_ok first (see
 * user/lib/session_policy.h): one that asks for more than the policy allows
 * is dropped and logged, never granted.
 *
 * FROM /etc, AND CLAMPED ANYWAY. It used to be /home/<user>/user.ns -- a file
 * in the user's own writable home, granted unclamped by the process that holds
 * every authority on the machine. One line added to it (`rw /etc`) and the next
 * session could rewrite the account store. /etc is out of every session's
 * reach, and the clamp holds even if that ever stops being true.
 *
 * No profile, or nothing left after the clamp: the default session. */
static int load_user_profile(const char *user, int is_admin,
                             struct embk_spawn_file_action *acts, int max,
                             char *desc, int desc_cap) {
    char path[128], *p = path;
    p = put_str(p, "/etc/sessions/");
    p = put_str(p, user);
    p = put_str(p, ".ns");
    *p = 0;

    char buf[512];
    int n = read_small(path, buf, sizeof buf);
    if (n <= 0) return default_profile(user, acts, desc, desc_cap);

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
        if (!embk_session_grant_ok(user, is_admin, prefix, mode == EMBK_NS_RO)) {
            char b[300], *q = b;
            q = put_str(q, "init: session profile for '"); q = put_str(q, user);
            q = put_str(q, "' asks for "); q = put_str(q, mode == EMBK_NS_RO ? "ro " : "rw ");
            q = put_str(q, prefix); q = put_str(q, " -- not granted (session policy)\n"); *q = 0;
            log_line(b);
            continue;
        }
        embk_action_ns_bind(&acts[na], prefix, mode);
        if (desc_cap && dn + plen + 6 < desc_cap) {
            if (dn) { desc[dn++]=','; desc[dn++]=' '; }
            desc[dn++]='r'; desc[dn++]=(mode==EMBK_NS_RO)?'o':'w'; desc[dn++]=' ';
            for (int k = 0; k < plen; k++) desc[dn++] = prefix[k];
            desc[dn] = 0;
        }
        na++;
    }
    return na > 0 ? na : default_profile(user, acts, desc, desc_cap);
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

/* Spawn a system-session helper (setup, the greeter) with only what it needs:
 * the filesystem (it manages /etc) and the display. Not the network, not
 * audio, not the debugger -- and above all not EMBK_CAP_SESSION, which is
 * init's alone: a greeter bug must not be a way to open sessions. */
static int64_t spawn_narrow(const char *path, struct embk_spawn_file_action *extra, int nextra) {
    struct embk_spawn_file_action acts[3];
    int n = 0;
    for (int i = 0; i < nextra && n < 2; i++) acts[n++] = extra[i];
    embk_action_set_caps(&acts[n++], EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM) | EMBK_CAP_BIT(EMBK_CAP_GPU));
    char *argv_[] = { (char *)path, NULL };
    return embk_spawn(path, argv_, acts, n);
}

void _start(long argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    log_line("init: up -- root of EmbLink userspace authority\n");
    ns_selfcheck();

    int autologin = DEV_AUTOLOGIN;     /* cleared by a logout; see DEV_AUTOLOGIN */

    for (;;) {
        char user[32];
        if (autologin) {
            char *d = user; d = put_str(d, DEV_USER); *d = 0;
            log_line("init: DEV auto-login as '" DEV_USER "'\n");
            /* A fresh COW image has no account directories. */
            (void)embk_mkdir("/home/" DEV_USER);
            (void)embk_mkdir("/home/" DEV_USER "/Desktop");
            (void)embk_mkdir("/home/" DEV_USER "/Documents");
            (void)embk_mkdir("/home/" DEV_USER "/Downloads");
            (void)embk_mkdir("/home/" DEV_USER "/Music");
            (void)embk_mkdir("/home/" DEV_USER "/Pictures");
            (void)embk_mkdir("/home/" DEV_USER "/Videos");
            (void)embk_mkdir("/home/" DEV_USER "/Trash");
        } else {
            struct embk_stat shadow;
            if (embk_stat(SHADOW, &shadow) < 0 || shadow.size == 0) {
                int setup = (int)spawn_narrow(SETUP, NULL, 0);
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
            int login = (int)spawn_narrow(LOGIN, &login_action, 1);
            embk_close_handle(pipe_handles[1]);
            if (login < 0) {
                embk_close_handle(pipe_handles[0]);
                log_line("init: could not launch login\n");
                embk_sleep_ms(1000);
                continue;
            }
            log_line("init: login screen started\n");
            embk_fd_install_obj(pipe_handles[0], SESSION_FD);
            embk_close_handle(pipe_handles[0]);
            int login_code = embk_wait(login);
            int64_t user_len = embk_read(SESSION_FD, user, sizeof user - 1);
            embk_close(SESSION_FD);
            if (login_code != 0 || user_len <= 0 || user_len >= (int64_t)sizeof user) {
                log_line("init: login ended without an authenticated user\n");
                continue;
            }
            user[user_len] = 0;
        }

        /* THE SESSION: its namespace (the profile, clamped) and its identity
         * (NEW_SESSION -- the kernel records whose every process in it is,
         * and ends them all when the desktop goes). */
        struct embk_spawn_file_action sacts[NS_ACTS_MAX];
        char nsdesc[192];
        int is_admin = user_is_admin(user);
        int snacts = load_user_profile(user, is_admin, sacts, NS_ACTS_MAX - 1, nsdesc, sizeof nsdesc);
        embk_action_new_session(&sacts[snacts++], user);

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
            q = put_str(q, is_admin ? "' (administrator)" : "'");
            q = put_str(q, " -> ns["); q = put_str(q, nsdesc); q = put_str(q, "]\n"); *q = 0;
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
         * would leak one of init's 16 handles. The kernel has already stopped
         * whatever the session left running: its leader is gone. */
        int code = embk_wait(h);

        if (code == EMBK_EXIT_LOGOUT) {
            char b[96], *q = b;
            q = put_str(q, "init: '"); q = put_str(q, user);
            q = put_str(q, "' logged out -- returning to the login screen\n"); *q = 0;
            log_line(b);
            autologin = 0;
        } else {
            char b[96], *q = b;
            const char *pre = "init: desktop exited (code ";
            while (*pre) *q++ = *pre++;
            q = put_dec(q, code);
            const char *post = autologin ? ") -- restarting the session\n" : ") -- returning to login\n";
            while (*post) *q++ = *post++;
            *q = 0;
            log_line(b);
        }

        embk_sleep_ms(200);   /* never a hot crash-loop */
    }
}
