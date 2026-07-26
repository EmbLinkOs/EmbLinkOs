/* unistd.h — emlibc's thin, honest view of the EmbLink I/O rim
 * (docs/EMLIBC_Requirements.md §3). These are the OS-facing calls that back
 * stdio; each is a direct wrapper over an int-0x80 syscall. Only what the
 * kernel actually implements appears here — no fork, no exec, no mmap. */
#ifndef _EMLIBC_UNISTD_H
#define _EMLIBC_UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef long off_t;

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* open() flags — the subset the kernel's sys_open honors. */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     open(const char *path, int flags, ...);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     unlink(const char *path);
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);
void    _exit(int code);

/* getentropy — RDRAND or fail, never fabricated (§3, non-negotiable). */
int     getentropy(void *buf, size_t n);

#endif /* _EMLIBC_UNISTD_H */
