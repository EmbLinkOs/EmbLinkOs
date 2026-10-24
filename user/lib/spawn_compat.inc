/* POSIX spawn on native explicit actions. No fork or in-place exec emulation.
 * Included once by syscalls.c, alongside its errno/path/flag translations. */
#include <spawn.h>
#include <sched.h>

#define SPAWN_COMPAT_ACTIONS (EMBK_SPAWN_ACTIONS_MAX-3) /* native 64 minus snapshot, cwd and retention */
struct __posix_spawn_file_actions {
    unsigned count;
    struct embk_spawn_file_action actions[SPAWN_COMPAT_ACTIONS];
};
struct __posix_spawnattr {
    short flags;
    pid_t pgroup;
    struct sched_param param;
    int policy;
    sigset_t sigdefault,sigmask;
};
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *out) {
    if(!out)return EINVAL;
    *out=calloc(1,sizeof **out);return *out?0:ENOMEM;
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *out) {
    if(!out || !*out)return EINVAL;
    free(*out);*out=NULL;return 0;
}
static int spawn_action_add(posix_spawn_file_actions_t *out,unsigned kind,int fd,int source,
                            const char *path,int flags,mode_t mode) {
    if(!out || !*out)return EINVAL;
    if(fd<0 || fd>=64 || (kind==EMBK_SPAWN_ACTION_DUP2 && (source<0 || source>=64)))return EBADF;
    if((*out)->count>=SPAWN_COMPAT_ACTIONS)return E2BIG;
    if(path && strlen(path)>=sizeof (*out)->actions[0].path)return ENAMETOOLONG;
    struct embk_spawn_file_action a={0};a.kind=kind;a.target_fd=fd;
    a.src_obj_handle=source;a.flags=flags;a.mode=mode;
    if(path)strcpy(a.path,path);
    (*out)->actions[(*out)->count++]=a;return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *out,int fd,const char *path,int flags,mode_t mode) {
    if(!path)return EINVAL;
    if((flags&O_ACCMODE)==O_ACCMODE)return EINVAL;
    if(flags&~(O_ACCMODE|O_CREAT|O_EXCL|O_TRUNC|O_APPEND|O_NONBLOCK|O_CLOEXEC))return ENOTSUP;
    return spawn_action_add(out,EMBK_SPAWN_ACTION_OPEN,fd,0,path,open_native_flags(flags),mode);
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *out,int oldfd,int newfd) {
    return spawn_action_add(out,EMBK_SPAWN_ACTION_DUP2,newfd,oldfd,NULL,0,0);
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *out,int fd) {
    return spawn_action_add(out,EMBK_SPAWN_ACTION_CLOSE,fd,0,NULL,0,0);
}
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *out,const char *path) {
    if(!path)return EINVAL;
    return spawn_action_add(out,EMBK_SPAWN_ACTION_CWD,0,0,path,0,0);
}
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *out,int fd) {
    if(!out || !*out)return EINVAL;
    if(fd<0 || fd>=64)return EBADF;
    return ENOTSUP; /* vnode-to-path and a kernel cwd object do not exist yet */
}
int posix_spawnattr_init(posix_spawnattr_t *out) {
    if(!out)return EINVAL;
    *out=calloc(1,sizeof **out);return *out?0:ENOMEM;
}
int posix_spawnattr_destroy(posix_spawnattr_t *out) {
    if(!out || !*out)return EINVAL;
    free(*out);*out=NULL;return 0;
}
int posix_spawnattr_setflags(posix_spawnattr_t *out,short flags) {
    if(!out || !*out)return EINVAL;
    const short known=POSIX_SPAWN_RESETIDS|POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSCHEDPARAM|
        POSIX_SPAWN_SETSCHEDULER|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETSIGMASK;
    if(flags&~known)return EINVAL;
    if(flags)return ENOTSUP; /* no silent promises about identities/groups/signals */
    (*out)->flags=flags;return 0;
}
#define SPAWN_ATTR_GET(name,type,field) \
int posix_spawnattr_get##name(const posix_spawnattr_t *a,type *out) { \
    if(!a || !*a || !out){return EINVAL;} *out=(*a)->field;return 0; }
#define SPAWN_ATTR_SET(name,type,field) \
int posix_spawnattr_set##name(posix_spawnattr_t *a,type value) { \
    if(!a || !*a){return EINVAL;} (*a)->field=value;return 0; }
SPAWN_ATTR_GET(flags,short,flags)
SPAWN_ATTR_GET(pgroup,pid_t,pgroup)
SPAWN_ATTR_GET(schedpolicy,int,policy)
SPAWN_ATTR_GET(schedparam,struct sched_param,param)
SPAWN_ATTR_GET(sigdefault,sigset_t,sigdefault)
SPAWN_ATTR_GET(sigmask,sigset_t,sigmask)
SPAWN_ATTR_SET(pgroup,pid_t,pgroup)
SPAWN_ATTR_SET(schedpolicy,int,policy)
int posix_spawnattr_setschedparam(posix_spawnattr_t *a,const struct sched_param *v) {
    if(!a || !*a || !v){return EINVAL;} (*a)->param=*v;return 0;
}
int posix_spawnattr_setsigdefault(posix_spawnattr_t *a,const sigset_t *v) {
    if(!a || !*a || !v){return EINVAL;} (*a)->sigdefault=*v;return 0;
}
int posix_spawnattr_setsigmask(posix_spawnattr_t *a,const sigset_t *v) {
    if(!a || !*a || !v){return EINVAL;} (*a)->sigmask=*v;return 0;
}
#undef SPAWN_ATTR_GET
#undef SPAWN_ATTR_SET

static int spawn_try(pid_t *pid,const char *file,const char *cwd,
                     struct embk_spawn_file_action *actions,unsigned n,
                     char *const argv[],char *const envp[]) {
    char absolute[EMBK_PATH_MAX];
    if(path_abs_from(cwd,file,absolute,sizeof absolute))return errno;
    unsigned argc=0;
    if(!argv)return EINVAL;
    while(argv[argc])if(++argc>=128)return E2BIG;
    int64_t h=embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)absolute,
        (int64_t)(uintptr_t)argv,argc,(int64_t)(uintptr_t)actions,n,(int64_t)(uintptr_t)envp);
    if(h<0)return embk_errno_from_kernel(h);
    if(pid)*pid=(pid_t)h;
    return 0;
}
static int spawn_compat(pid_t *pid,const char *file,const posix_spawn_file_actions_t *fa,
                        const posix_spawnattr_t *attr,char *const argv[],char *const envp[],int search) {
    if(!file)return EINVAL;
    if(!*file)return ENOENT;
    if(attr && (!*attr || (*attr)->flags))return ENOTSUP;
    if(fa && !*fa)return EINVAL;
    struct embk_spawn_file_action *actions=calloc(EMBK_SPAWN_ACTIONS_MAX,sizeof *actions);
    if(!actions)return ENOMEM;
    char cwd[EMBK_PATH_MAX];strcpy(cwd,g_cwd);
    actions[0].kind=EMBK_SPAWN_ACTION_FD_SNAPSHOT;
    actions[1].kind=EMBK_SPAWN_ACTION_CWD;strcpy(actions[1].path,cwd);
    actions[2].kind=EMBK_SPAWN_ACTION_KEEP_STATUS;
    unsigned n=3;int rc=0;
    for(unsigned i=0;fa && i<(*fa)->count;i++) {
        actions[n]=(*fa)->actions[i];
        if(actions[n].kind==EMBK_SPAWN_ACTION_OPEN || actions[n].kind==EMBK_SPAWN_ACTION_CWD) {
            char absolute[EMBK_PATH_MAX];
            if(path_abs_from(cwd,actions[n].path,absolute,sizeof absolute)){rc=errno;goto done;}
            strcpy(actions[n].path,absolute);
            if(actions[n].kind==EMBK_SPAWN_ACTION_CWD)strcpy(cwd,absolute);
        }
        n++;
    }
    if(!search || strchr(file,'/')) {rc=spawn_try(pid,file,cwd,actions,n,argv,envp);goto done;}
    /* Search the caller's PATH, not the child's explicit environment. Empty
     * entries mean the child's final cwd; an unset PATH uses the native bins. */
    const char *path=getenv("PATH");if(!path)path="/system/bin:/bin";
    int denied=0;
    for(const char *part=path;;) {
        const char *end=strchr(part,':');size_t len=end?(size_t)(end-part):strlen(part);
        char candidate[EMBK_PATH_MAX];size_t flen=strlen(file);
        if(len+(len?1:0)+flen>=sizeof candidate){rc=ENAMETOOLONG;goto done;}
        memcpy(candidate,part,len);size_t off=len;if(len)candidate[off++]='/';
        memcpy(candidate+off,file,flen+1);
        rc=spawn_try(pid,candidate,cwd,actions,n,argv,envp);
        if(!rc)break;
        if(rc==EACCES)denied=1;
        else if(rc!=ENOENT && rc!=ENOTDIR)break;
        if(!end){rc=denied?EACCES:ENOENT;break;}part=end+1;
    }
done:
    free(actions);return rc;
}
int posix_spawn(pid_t *pid,const char *file,const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,char *const argv[],char *const envp[]) {
    int saved=errno;int rc=spawn_compat(pid,file,fa,attr,argv,envp,0);errno=saved;return rc;
}
int posix_spawnp(pid_t *pid,const char *file,const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,char *const argv[],char *const envp[]) {
    int saved=errno;int rc=spawn_compat(pid,file,fa,attr,argv,envp,1);errno=saved;return rc;
}
