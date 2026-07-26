/* syscalls.c — emlibc's OS-facing rim: the thin retargeting of the standard
 * I/O surface onto EmbLink's int-0x80 ABI (<embk_syscall.h>). Everything here
 * is EmbLink-specific by nature; the layers above (string/stdio/stdlib) are
 * not, and never call the kernel except through these functions.
 *
 * Non-POSIX on purpose (docs/EMLIBC_Requirements.md §3): what the kernel does
 * not provide is ABSENT here, not stubbed to lie. No fork, no exec, no mmap.
 */

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "embk_syscall.h"

#define EM_PATH_MAX 1024

extern int embk_errno_from_kernel(int64_t ret);

/* -EMBK_* -> errno + the caller's error sentinel. */
static long fail_long(int64_t ret) { errno = embk_errno_from_kernel(ret); return -1; }

/* ------------------------------------------------------------------ */
/* environment — published by crt0 from the vector the parent passed  */
/* ------------------------------------------------------------------ */
char **environ = 0;
char  *embk_empty_env[1] = { 0 };   /* crt0 points environ here when envp==NULL:
                                     * "no environment", still walkable. */

/* ------------------------------------------------------------------ */
/* working directory — a LIBC fact; the kernel is absolute-path only. */
/* Nothing is inherited unless a parent NAMES a start dir via PWD.     */
/* ------------------------------------------------------------------ */
static char g_cwd[EM_PATH_MAX] = "/";

/* Make `in` absolute against g_cwd if it is relative. Minimal join (no "."/
 * ".." collapsing yet — phase 1); absolute inputs pass through unchanged. */
static const char *path_abs(const char *in, char *buf, size_t cap)
{
    if (in && in[0] == '/') return in;                 /* already absolute */
    size_t cl = strlen(g_cwd);
    size_t il = in ? strlen(in) : 0;
    if (cl + 1 + il + 1 > cap) return in;              /* too long: let the kernel judge */
    memcpy(buf, g_cwd, cl);
    size_t p = cl;
    if (p == 0 || buf[p - 1] != '/') buf[p++] = '/';
    if (in) { memcpy(buf + p, in, il); p += il; }
    buf[p] = 0;
    return buf;
}

/* Seed g_cwd from PWD if the parent named one. Called by crt0 after environ
 * is published and before any relative path is resolved. */
extern char *getenv(const char *name);
void embk_cwd_init_from_env(void)
{
    const char *pwd = getenv("PWD");
    if (pwd && pwd[0] == '/' && strlen(pwd) < sizeof g_cwd) strcpy(g_cwd, pwd);
}

char *getcwd(char *buf, size_t size)
{
    size_t need = strlen(g_cwd) + 1;
    if (!buf || size < need) { errno = ERANGE; return 0; }
    memcpy(buf, g_cwd, need);
    return buf;
}

int chdir(const char *path)
{
    char tmp[EM_PATH_MAX];
    const char *ap = path_abs(path, tmp, sizeof tmp);
    /* Confirm it exists by asking the kernel to stat it. We only need the
     * return code, so a byte buffer for the kernel's vfs_stat output suffices
     * (no block-scope struct type -- emlibc stays in EmbCC's subset). */
    unsigned char stbuf[128];
    int64_t r = embk_syscall2(EMBK_SYS_stat, (int64_t)ap, (int64_t)stbuf);
    if (embk_is_err(r)) return (int)fail_long(r);
    if (strlen(ap) >= sizeof g_cwd) { errno = ENAMETOOLONG; return -1; }
    strcpy(g_cwd, ap);
    return 0;
}

/* ------------------------------------------------------------------ */
/* I/O — direct wrappers over the kernel                              */
/* ------------------------------------------------------------------ */
ssize_t read(int fd, void *buf, size_t n)
{
    int64_t r = embk_syscall3(EMBK_SYS_read, fd, (int64_t)buf, (int64_t)n);
    return embk_is_err(r) ? fail_long(r) : (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    int64_t r = embk_syscall3(EMBK_SYS_write, fd, (int64_t)buf, (int64_t)n);
    return embk_is_err(r) ? fail_long(r) : (ssize_t)r;
}

int open(const char *path, int flags, ...)
{
    /* mode is accepted for O_CREAT source-compatibility; the kernel applies a
     * default file mode, so we do not thread a mode argument through yet. */
    char tmp[EM_PATH_MAX];
    const char *ap = path_abs(path, tmp, sizeof tmp);
    int64_t r = embk_syscall3(EMBK_SYS_open, (int64_t)ap, (int64_t)flags, 0644);
    return embk_is_err(r) ? (int)fail_long(r) : (int)r;
}

int close(int fd)
{
    int64_t r = embk_syscall1(EMBK_SYS_close, fd);
    return embk_is_err(r) ? (int)fail_long(r) : 0;
}

off_t lseek(int fd, off_t off, int whence)
{
    int64_t r = embk_syscall3(EMBK_SYS_lseek, fd, (int64_t)off, (int64_t)whence);
    return embk_is_err(r) ? fail_long(r) : (off_t)r;
}

int unlink(const char *path)
{
    char tmp[EM_PATH_MAX];
    const char *ap = path_abs(path, tmp, sizeof tmp);
    int64_t r = embk_syscall1(EMBK_SYS_unlink, (int64_t)ap);
    return embk_is_err(r) ? (int)fail_long(r) : 0;
}

void _exit(int code)
{
    embk_syscall1(EMBK_SYS_exit, code);
    for (;;) { }   /* the kernel never returns from exit; defensive backstop */
}

/* ------------------------------------------------------------------ */
/* sbrk — the heap primitive malloc() sits on                         */
/* ------------------------------------------------------------------ */
/* Robust against whether the kernel returns the old or the new break: callers
 * use sbrk(0) for the current break and sbrk(+n) only to grow, so exactly one
 * of the two conventions has to hold and it does not matter which. */
void *emlibc_sbrk(long incr)
{
    int64_t r = embk_syscall1(EMBK_SYS_sbrk, (int64_t)incr);
    if (embk_is_err(r)) { errno = embk_errno_from_kernel(r); return (void *)-1; }
    return (void *)(uintptr_t)r;
}

/* ------------------------------------------------------------------ */
/* getentropy — RDRAND or fail. Never fabricated (§3, non-negotiable). */
/* ------------------------------------------------------------------ */
int getentropy(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        unsigned long long v;
        unsigned char ok = 0;
        for (int tries = 0; tries < 32 && !ok; tries++)
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (!ok) { errno = EIO; return -1; }   /* honest failure, not a fake byte */
        size_t take = n < sizeof v ? n : sizeof v;
        memcpy(p, &v, take);
        p += take; n -= take;
    }
    return 0;
}
