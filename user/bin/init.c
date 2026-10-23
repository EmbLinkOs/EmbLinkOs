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

#define DEV_USER "guest"
#define LIVE_HOME "/home/.live-guest"

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

static int within(const char *path,const char *prefix) {
    int i=0;while(prefix[i] && path[i]==prefix[i]) i++;
    return !prefix[i] && (!path[i] || path[i]=='/');
}
static int profile_path_allowed(const char *path,int mode,const char *home) {
    /* Profiles are user-writable preferences. Init must never turn them into
     * authority outside the authenticated user's fixed session boundary. */
    if(path[0]!='/') return 0;
    for(int i=0;path[i];i++)
        if(path[i]=='/' && (path[i+1]=='/' ||
           (path[i+1]=='.' && (!path[i+2] || path[i+2]=='/' ||
             (path[i+2]=='.' && (!path[i+3] || path[i+3]=='/')))))) return 0;
    if(within(path,home) || within(path,"/run")) return 1;
    return mode==EMBK_NS_RO && (within(path,"/system") || within(path,"/data/apps"));
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

    char home[80], *hp=home;hp=put_str(hp,"/home/");hp=put_str(hp,user);*hp=0;
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
        if(!profile_path_allowed(prefix,mode,home)) return -1;
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

static void unlock_session(const char *user) {
    int handles[2];
    if(embk_pipe(handles)) {embk_sleep_ms(1000);return;}
    struct embk_spawn_file_action a={0};
    a.kind=EMBK_SPAWN_ACTION_INSTALL_OBJ;a.target_fd=3;a.src_obj_handle=handles[1];
    char *av[]={(char *)LOGIN,(char *)"--unlock",(char *)user,NULL};
    int h=(int)embk_spawn(LOGIN,av,&a,1);
    embk_close_handle(handles[1]);
    if(h<0) {embk_close_handle(handles[0]);embk_sleep_ms(1000);return;}
    embk_fd_install_obj(handles[0],SESSION_FD);embk_close_handle(handles[0]);
    embk_session(EMBK_SESSION_GREETER,h);
    int code=embk_wait(h);char proof[32];
    int n=(int)embk_read(SESSION_FD,proof,sizeof proof-1);embk_close(SESSION_FD);
    if(code==0 && n>0 && n<32) {
        proof[n]=0;int i=0;while(user[i] && user[i]==proof[i]) i++;
        if(!user[i] && !proof[i]) {
            embk_session(EMBK_SESSION_UNLOCK,0);
            log_line("init: session unlocked\n");return;
        }
    }
    log_line("init: unlock closed without authentication; session remains locked\n");
    embk_sleep_ms(500);
}

void _start(long argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    log_line("init: up -- root of EmbLink userspace authority\n");
    ns_selfcheck();

    int session_flags;
    while((session_flags=embk_session(EMBK_SESSION_STATUS,0))<0) embk_sleep_ms(10);
    int live=(session_flags&EMBK_SESSION_LIVE)!=0;
    for (;;) {
        char user[32];
        if(live) {
            char *q=put_str(user,DEV_USER);*q=0;
            log_line("init: explicit live session as guest (no password lock)\n");
        } else {
        struct embk_stat shadow;
        if (embk_stat(SHADOW, &shadow) < 0 || shadow.size == 0) {
            char *argv_setup[] = { (char *)SETUP, NULL };
            int setup = (int)embk_spawn(SETUP, argv_setup, NULL, 0);
            if (setup < 0) {
                log_line("init: could not launch first-boot setup; retrying\n");
                embk_sleep_ms(1000);
                continue;
            }
            embk_session(EMBK_SESSION_GREETER,setup);
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
        embk_session(EMBK_SESSION_GREETER,login);
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

        struct embk_spawn_file_action sacts[NS_ACTS_MAX+1];
        char nsdesc[192];
        int snacts;
        if(live) {
        /* A fresh COW image has no account directories. Create only the home
         * required by the explicit live desktop and give it the same namespace
         * policy as a real session profile. */
        (void)embk_mkdir(LIVE_HOME);
        (void)embk_mkdir(LIVE_HOME "/Desktop");
        (void)embk_mkdir(LIVE_HOME "/Documents");
        (void)embk_mkdir(LIVE_HOME "/Downloads");
        (void)embk_mkdir(LIVE_HOME "/Music");
        (void)embk_mkdir(LIVE_HOME "/Pictures");
        (void)embk_mkdir(LIVE_HOME "/Videos");
        (void)embk_mkdir(LIVE_HOME "/Trash");
        embk_action_ns_bind(&sacts[0], "/system", EMBK_NS_RO);
        embk_action_ns_bind(&sacts[1], "/data/apps", EMBK_NS_RO);
        embk_action_ns_bind(&sacts[2], LIVE_HOME, EMBK_NS_RW);
        embk_action_ns_bind(&sacts[3], "/run", EMBK_NS_RW);
        snacts = 4;
        char *nd = nsdesc;
        nd = put_str(nd, "ro /system, ro /data/apps, rw " LIVE_HOME ", rw /run");
        *nd = 0;
        } else {
        snacts = load_user_profile(user, sacts, NS_ACTS_MAX, nsdesc, sizeof nsdesc);
        if (snacts <= 0) {
            log_line("init: authenticated user has no valid session profile\n");
            continue;
        }
        }
        /* Ordinary apps cannot debug init, access raw disks or load extensions. */
        embk_action_set_caps(&sacts[snacts++],
            EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)|EMBK_CAP_BIT(EMBK_CAP_NETWORK)|
            EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_AUDIO)|
            EMBK_CAP_BIT(EMBK_CAP_POWER)|EMBK_CAP_BIT(EMBK_CAP_NETWORK_ADMIN));
        if(embk_session(EMBK_SESSION_BEGIN,0)<0) {embk_sleep_ms(200);continue;}

        char home[80], env_user[48], env_home[96], env_pwd[96];
        char *p = home;
        if(live)p=put_str(p,LIVE_HOME);
        else {p=put_str(p,"/home/");p=put_str(p,user);}
        *p=0;
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
            while(embk_session(EMBK_SESSION_END,0)<0) embk_sleep_ms(20);
            embk_sleep_ms(1000);
            continue;
        }
        embk_session(EMBK_SESSION_UNLOCK,0);
        log_line("init: desktop session started\n");

        /* Block until the desktop exits, then reap it. The wait frees BOTH the
         * zombie process slot and this spawn handle -- without it, every restart
         * would leak one of init's 16 handles. */
        while(embk_proc_alive(h)>0) {
            int state=embk_session(EMBK_SESSION_STATUS,0);
            if(state&EMBK_SESSION_ENDING) break;
            if((state&EMBK_SESSION_LOCKED) && !live) unlock_session(user);
            embk_sleep_ms(50);
        }
        while(embk_session(EMBK_SESSION_END,0)<0) embk_sleep_ms(20);
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
