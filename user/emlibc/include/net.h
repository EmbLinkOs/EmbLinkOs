/* net.h -- emlibc's native network surface (docs/EMLIBC_Requirements.md).
 *
 * NOT BSD sockets. emlibc speaks the OS's own vocabulary: a connection is just
 * an fd (read/write/close it through <unistd.h>), and a host is an (ip, port)
 * pair of integers -- no sockaddr, no getaddrinfo ceremony. This is the native
 * counterpart to the newlib-side POSIX shim; both ride the kernel's CAP_NETWORK
 * sockets, but native EmbLink programs use THIS. Requires CAP_NETWORK. IPv4.
 */
#ifndef _EMLIBC_NET_H
#define _EMLIBC_NET_H

#include <stdint.h>
#include <stddef.h>

/* Resolve a hostname (or a dotted-quad string) to a host-order IPv4 address.
 * Returns 0 on success, -1 with errno set. */
int em_resolve(const char *host, uint32_t *ip);

/* TCP client: resolve `host` and connect to `port`. Returns a connection fd
 * (use read/write/close), or -1. */
int em_tcp_connect(const char *host, uint16_t port);

/* TCP server: a listening fd bound to `port`; em_tcp_accept blocks for a peer
 * and returns a fresh connection fd. */
int em_tcp_listen(uint16_t port, int backlog);
int em_tcp_accept(int listener);

/* UDP: a datagram fd, an optional local bind, and (ip,port)-addressed I/O. */
int  em_udp_open(void);
int  em_udp_bind(int fd, uint16_t port);
long em_udp_sendto(int fd, uint32_t ip, uint16_t port, const void *buf, size_t len);
long em_udp_recvfrom(int fd, void *buf, size_t cap, uint32_t *ip, uint16_t *port);

/* Format a host-order IPv4 into "a.b.c.d" (needs >= 16 bytes). Returns `out`. */
char *em_ip_str(uint32_t ip, char *out);

#endif /* _EMLIBC_NET_H */
