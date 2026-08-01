/* tlstest -- drive a real TLS 1.3 handshake from the OS (docs/TLS.md T2).
 * Resolves a host, opens a TCP socket to :443, runs our libtls handshake (server
 * Finished verified; certificate NOT verified -- the labelled T2 milestone),
 * sends a minimal HTTPS GET, and reads the encrypted response.
 *
 * Needs CAP_NETWORK. RDRAND (QEMU -cpu max) for the key material.
 *
 * exit: 0 = handshake + encrypted round-trip OK;  2 resolve  3 socket
 *       4 connect  5 oom  6 handshake  7 write  8 no data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "embk_socket.h"
#include "tls.h"

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "cloudflare.com";

    struct in_addr addr;
    if (emb_resolve(host, &addr) != 0) { printf("tlstest: resolve %s failed\n", host); return 2; }
    unsigned h = ntohl(addr.s_addr);
    printf("tlstest: %s -> %u.%u.%u.%u\n", host, (h>>24)&255, (h>>16)&255, (h>>8)&255, h&255);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("tlstest: socket %d\n", fd); return 3; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(443); sa.sin_addr = addr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { printf("tlstest: connect failed\n"); return 4; }
    printf("tlstest: TCP connected :443\n");

    struct tls_conn *c = malloc(sizeof *c);
    if (!c) { printf("tlstest: oom\n"); return 5; }

    int r = tls_connect(c, fd, host);
    if (r != 0) { printf("tlstest: TLS handshake FAILED rc=%d\n", r); free(c); return 6; }
    printf("tlstest: TLS 1.3 handshake OK -- server Finished verified (cert NOT verified: T2)\n");

    char req[256];
    int rq = snprintf(req, sizeof req,
        "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: EmbLinkOS-libtls\r\n\r\n", host);
    if (tls_write(c, req, (size_t)rq) != rq) { printf("tlstest: tls_write failed\n"); tls_close(c); free(c); return 7; }

    char buf[2048];
    long total = 0; int printed = 0;
    for (;;) {
        long n = tls_read(c, buf, sizeof buf - 1);
        if (n < 0) { printf("tlstest: tls_read error\n"); break; }
        if (n == 0) break;                       /* clean close_notify / EOF */
        total += n;
        if (!printed) {
            buf[n] = 0;
            char *nl = strchr(buf, '\r'); if (!nl) nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            printf("tlstest: <- %s\n", buf);      /* the HTTP status line, decrypted */
            printed = 1;
        }
    }
    printf("tlstest: read %ld bytes of decrypted application data\n", total);

    tls_close(c); free(c);
    return (total > 0 && printed) ? 0 : 8;
}
