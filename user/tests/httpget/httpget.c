/* httpget.c -- ring-3 networking witness (M4).
 *
 * A userspace program that resolves a hostname, opens a TCP socket, connects to
 * :80, sends an HTTP/1.0 GET, and reads the response -- entirely over the OS's
 * own network stack, through the BSD-sockets shim (embk_socket.h) which maps
 * onto the native CAP_NETWORK socket syscalls. It exercises the whole ring-3
 * path AND the shim in one shot.
 *
 * The capability gates it: run with CAP_NETWORK and it exits 0 on a 200; run
 * without and socket()/resolve return -EPERM, so it exits nonzero. `test
 * netuser` spawns it both ways.
 *
 * usage: httpget [host]        (default example.com)
 * exit:  0 = got an HTTP/1.x response;  2 resolve  3 socket  4 connect  5 no-HTTP
 */
#include <stdio.h>
#include <string.h>
#include "embk_socket.h"

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "example.com";

    struct in_addr addr;
    if (emb_resolve(host, &addr) != 0) { printf("httpget: resolve %s failed\n", host); return 2; }
    unsigned int h = ntohl(addr.s_addr);
    printf("httpget: %s -> %u.%u.%u.%u\n", host,
           (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("httpget: socket failed (%d)\n", fd); return 3; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(80);
    sa.sin_addr   = addr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        printf("httpget: connect failed\n"); return 4;
    }
    printf("httpget: connected, sending GET\n");

    char req[160];
    int rl = snprintf(req, sizeof req,
                      "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    send(fd, req, rl, 0);

    char buf[512], status[128] = {0};
    int total = 0, have_status = 0;
    for (;;) {
        long n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;                       /* 0 = peer closed (EOF) */
        if (!have_status) {
            int k = n < 127 ? (int)n : 127;
            memcpy(status, buf, k); status[k] = 0;
            char *c = strchr(status, '\r'); if (c) *c = 0;
            have_status = 1;
        }
        total += (int)n;
    }
    close(fd);

    printf("httpget: status \"%s\", %d bytes\n", status, total);
    return (total >= 12 && strncmp(status, "HTTP/1", 6) == 0) ? 0 : 5;
}
