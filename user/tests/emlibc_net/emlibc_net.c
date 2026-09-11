/* emlibc_net -- proves emlibc's NATIVE networking (net.h). An HTTP GET using
 * EmbLink's own vocabulary -- em_resolve + em_tcp_connect + read/write/close --
 * with NO BSD sockets and NO newlib (linked -nostdlib against libemlibc only).
 * The native-world counterpart to sockdemo (which uses the POSIX shim). Needs
 * CAP_NETWORK.  exit: 0 ok  2 resolve  3 connect  5 no data
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "net.h"

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "example.com";

    uint32_t ip;
    if (em_resolve(host, &ip) != 0) { printf("emlibc_net: em_resolve(%s) failed\n", host); return 2; }
    char ips[16];
    printf("emlibc_net: %s -> %s (emlibc em_resolve)\n", host, em_ip_str(ip, ips));

    int fd = em_tcp_connect(host, 80);
    if (fd < 0) { printf("emlibc_net: em_tcp_connect failed\n"); return 3; }
    printf("emlibc_net: connected via emlibc em_tcp_connect (fd %d)\n", fd);

    char req[256];
    int rl = snprintf(req, sizeof req,
                      "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    write(fd, req, (size_t)rl);

    char buf[1024]; long total = 0; int printed = 0;
    for (;;) {
        long n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) break;
        total += n;
        if (!printed) {
            buf[n] = 0;
            char *nl = strchr(buf, '\r'); if (nl) *nl = 0;
            printf("emlibc_net: <- %s\n", buf);
            printed = 1;
        }
    }
    close(fd);
    printf("emlibc_net: read %ld bytes (emlibc-only, native net)\n", total);
    return total > 0 ? 0 : 5;
}
