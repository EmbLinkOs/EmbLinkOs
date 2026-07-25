/* udptest.c -- ring-3 UDP witness (M5). A userspace program that resolves a
 * hostname by speaking DNS *itself* over its own SOCK_DGRAM socket: build the
 * query, sendto() the SLIRP resolver (10.0.2.3:53), recvfrom() the reply, and
 * parse the A record. Exercises the whole ring-3 UDP path (socket/bind-implicit/
 * sendto/recvfrom) through the sockets shim + native syscalls, gated on
 * CAP_NETWORK.
 *
 * usage: udptest [host]      (default example.com)
 * exit:  0 = resolved;  2 socket  3 sendto  4 no-reply  5 id-mismatch  6 no-A
 */
#include <stdio.h>
#include <string.h>
#include "embk_socket.h"

static int dns_query(unsigned char *buf, int id, const char *name) {
    memset(buf, 0, 12);
    buf[0] = id >> 8; buf[1] = id; buf[2] = 0x01; /* recursion desired */ buf[5] = 0x01; /* qd=1 */
    unsigned char *p = buf + 12;
    const char *s = name;
    while (*s) {
        const char *dot = s; while (*dot && *dot != '.') dot++;
        int l = (int)(dot - s);
        *p++ = (unsigned char)l; memcpy(p, s, l); p += l;
        s = (*dot == '.') ? dot + 1 : dot;
    }
    *p++ = 0; *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 1;  /* QTYPE A, QCLASS IN */
    return (int)(p - buf);
}

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "example.com";

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("udptest: socket failed (%d)\n", fd); return 2; }

    struct sockaddr_in dns;
    memset(&dns, 0, sizeof dns);
    dns.sin_family = AF_INET;
    dns.sin_port   = htons(53);
    dns.sin_addr.s_addr = htonl((10u << 24) | (2u << 8) | 3u);   /* 10.0.2.3 = SLIRP DNS */

    unsigned char q[300];
    int id = 0x4242;
    int ql = dns_query(q, id, host);
    if (sendto(fd, q, ql, 0, (struct sockaddr *)&dns, sizeof dns) < 0) {
        printf("udptest: sendto failed\n"); return 3;
    }

    unsigned char in[512];
    unsigned int sip = 0; unsigned short sport = 0;
    int n = embk_net_recvfrom(fd, in, sizeof in, &sip, &sport);
    if (n < 12) { printf("udptest: no reply (%d)\n", n); return 4; }
    if (((in[0] << 8) | in[1]) != id) { printf("udptest: id mismatch\n"); return 5; }
    int an = (in[6] << 8) | in[7];
    printf("udptest: %d-byte reply from %u.%u.%u.%u:%u (%d answers)\n", n,
           (sip >> 24) & 0xff, (sip >> 16) & 0xff, (sip >> 8) & 0xff, sip & 0xff, sport, an);

    /* Skip header(12) + question, then walk answers for the first A record. */
    int off = 12;
    while (off < n && in[off]) { if ((in[off] & 0xC0) == 0xC0) { off += 2; goto q_done; } off += 1 + in[off]; }
    off++;                                         /* the zero label */
q_done:
    off += 4;                                      /* QTYPE + QCLASS */
    for (int i = 0; i < an && off + 12 <= n; i++) {
        if ((in[off] & 0xC0) == 0xC0) off += 2;    /* compressed name */
        else { while (off < n && in[off]) off += 1 + in[off]; off++; }
        int type  = (in[off] << 8) | in[off + 1];
        int rdlen = (in[off + 8] << 8) | in[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4) {
            printf("udptest: %s -> %u.%u.%u.%u\n", host, in[off], in[off+1], in[off+2], in[off+3]);
            close(fd);
            return 0;
        }
        off += rdlen;
    }
    printf("udptest: no A record\n");
    close(fd);
    return 6;
}
