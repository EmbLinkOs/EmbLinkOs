/* net.c -- emlibc's native networking rim, over EmbLink's int-0x80 net syscalls
 * (EMBK_SYS_net_*). EmbLink-specific by nature, like rim/syscalls.c; the API in
 * <net.h> is the OS's own model (fds + integer addresses), not BSD sockets. */
#include "net.h"
#include <errno.h>
#include <unistd.h>
#include "embk_syscall.h"

extern int embk_errno_from_kernel(int64_t ret);
static int  fail_int(int64_t r)  { errno = embk_errno_from_kernel(r); return -1; }
static long fail_long(int64_t r) { errno = embk_errno_from_kernel(r); return -1; }

/* Parse "a.b.c.d" into a host-order IPv4. Returns 1 on success, 0 otherwise. */
static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t b[4]; const char *p = s;
    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return 0;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p++ - '0'); if (v > 255) return 0; }
        b[i] = v;
        if (i < 3) { if (*p != '.') return 0; p++; }
    }
    if (*p != 0) return 0;
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

int em_resolve(const char *host, uint32_t *ip) {
    if (parse_ipv4(host, ip)) return 0;
    int r = (int)embk_syscall2(EMBK_SYS_net_resolve, (int64_t)(long)host, (int64_t)(long)ip);
    return r < 0 ? fail_int(r) : 0;
}

int em_tcp_connect(const char *host, uint16_t port) {
    uint32_t ip;
    if (em_resolve(host, &ip) != 0) return -1;
    int fd = (int)embk_syscall1(EMBK_SYS_net_socket, 1);    /* 1 = stream/TCP */
    if (fd < 0) return fail_int(fd);
    int r = (int)embk_syscall3(EMBK_SYS_net_connect, fd, (int64_t)ip, (int64_t)port);
    if (r < 0) { close(fd); return fail_int(r); }
    return fd;
}

int em_tcp_listen(uint16_t port, int backlog) {
    int fd = (int)embk_syscall1(EMBK_SYS_net_socket, 1);
    if (fd < 0) return fail_int(fd);
    int r = (int)embk_syscall2(EMBK_SYS_net_bind, fd, (int64_t)port);
    if (r >= 0) r = (int)embk_syscall2(EMBK_SYS_net_listen, fd, backlog);
    if (r < 0) { close(fd); return fail_int(r); }
    return fd;
}

int em_tcp_accept(int listener) {
    int fd = (int)embk_syscall1(EMBK_SYS_net_accept, listener);
    return fd < 0 ? fail_int(fd) : fd;
}

int em_udp_open(void) {
    int fd = (int)embk_syscall1(EMBK_SYS_net_socket, 2);    /* 2 = dgram/UDP */
    return fd < 0 ? fail_int(fd) : fd;
}

int em_udp_bind(int fd, uint16_t port) {
    int r = (int)embk_syscall2(EMBK_SYS_net_bind, fd, (int64_t)port);
    return r < 0 ? fail_int(r) : 0;
}

long em_udp_sendto(int fd, uint32_t ip, uint16_t port, const void *buf, size_t len) {
    long r = (long)embk_syscall5(EMBK_SYS_net_sendto, fd, (int64_t)ip, (int64_t)port,
                                 (int64_t)(long)buf, (int64_t)len);
    return r < 0 ? fail_long(r) : r;
}

long em_udp_recvfrom(int fd, void *buf, size_t cap, uint32_t *ip, uint16_t *port) {
    long r = (long)embk_syscall5(EMBK_SYS_net_recvfrom, fd, (int64_t)(long)buf, (int64_t)cap,
                                 (int64_t)(long)ip, (int64_t)(long)port);
    return r < 0 ? fail_long(r) : r;
}

char *em_ip_str(uint32_t ip, char *out) {
    /* minimal, no snprintf dependency */
    char *p = out;
    for (int s = 24; s >= 0; s -= 8) {
        unsigned b = (ip >> s) & 0xff;
        if (b >= 100) *p++ = '0' + b / 100;
        if (b >= 10)  *p++ = '0' + (b / 10) % 10;
        *p++ = '0' + b % 10;
        if (s) *p++ = '.';
    }
    *p = 0;
    return out;
}
