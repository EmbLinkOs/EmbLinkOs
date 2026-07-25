#ifndef EMBK_SOCKET_H
#define EMBK_SOCKET_H
/* A thin BSD-sockets shim over the native EmbLink socket surface (embk.h), so
 * software ported from POSIX can use socket()/connect()/send()/recv() largely
 * unchanged. This is the "sockets shim for ports" half of the M4 networking API
 * decision -- native EmbLink programs should prefer embk_net_* directly. TCP
 * client only for now, matching the kernel surface. A socket is a real fd, so
 * read()/write()/close() also work on it. */

#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include "embk.h"

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define INADDR_ANY   0u

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
struct in_addr { in_addr_t s_addr; };            /* NETWORK order, per BSD */
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_in {
    unsigned short sin_family;
    in_port_t      sin_port;                     /* network order */
    struct in_addr sin_addr;                     /* network order */
    char           sin_zero[8];
};

/* Byte-order helpers (host is little-endian x86-64). */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

static inline int socket(int family, int type, int proto) {
    (void)family; (void)proto;                    /* SOCK_STREAM (TCP) or SOCK_DGRAM (UDP) */
    return embk_net_socket(type);
}
static inline int connect(int fd, const struct sockaddr *addr, unsigned len) {
    (void)len;
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    return embk_net_connect(fd, ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
}
static inline long send(int fd, const void *buf, unsigned long n, int flags) {
    (void)flags; return write(fd, buf, (size_t)n);
}
static inline long recv(int fd, void *buf, unsigned long n, int flags) {
    (void)flags; return read(fd, buf, (size_t)n);
}

/* Server side. accept() returns a NEW fd for the connection; the peer address
 * is not reported (M5) -- if `addr` is given, its port/addr are zeroed. */
static inline int bind(int fd, const struct sockaddr *addr, unsigned len) {
    (void)len;
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    return embk_net_bind(fd, ntohs(in->sin_port));
}
static inline int listen(int fd, int backlog) {
    return embk_net_listen(fd, backlog);
}
static inline int accept(int fd, struct sockaddr *addr, unsigned *len) {
    if (addr && len && *len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)addr;
        memset(in, 0, sizeof(*in)); in->sin_family = AF_INET;
        *len = sizeof(*in);
    }
    return embk_net_accept(fd);
}

/* UDP datagrams (SOCK_DGRAM). */
static inline long sendto(int fd, const void *buf, unsigned long len, int flags,
                          const struct sockaddr *dest, unsigned addrlen) {
    (void)flags; (void)addrlen;
    const struct sockaddr_in *in = (const struct sockaddr_in *)dest;
    return embk_net_sendto(fd, ntohl(in->sin_addr.s_addr), ntohs(in->sin_port), buf, len);
}
static inline long recvfrom(int fd, void *buf, unsigned long len, int flags,
                            struct sockaddr *src, unsigned *addrlen) {
    (void)flags;
    unsigned int ip = 0; unsigned short port = 0;
    int n = embk_net_recvfrom(fd, buf, len, &ip, &port);
    if (n >= 0 && src && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)src;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET; in->sin_port = htons(port); in->sin_addr.s_addr = htonl(ip);
        *addrlen = sizeof(*in);
    }
    return n;
}

/* Resolve a hostname to a network-order in_addr (a gethostbyname in miniature). */
static inline int emb_resolve(const char *name, struct in_addr *out) {
    unsigned int ip_host = 0;
    int rc = embk_net_resolve(name, &ip_host);
    if (rc != 0) return rc;
    out->s_addr = htonl(ip_host);
    return 0;
}

#endif /* EMBK_SOCKET_H */
