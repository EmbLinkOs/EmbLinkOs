/* httpd.c -- a tiny HTTP server, the M5 witness that EmbLinkOS can HOST a
 * service (not just fetch one). It binds a port, listens, accepts a connection,
 * reads the request, and writes a fixed page -- all through the BSD-sockets shim
 * over the native CAP_NETWORK socket syscalls (now with bind/listen/accept).
 *
 * Serves `count` connections (default 1) then exits, so a witness can spawn it,
 * curl it once from the host over a SLIRP hostfwd, and have it finish cleanly.
 *
 * usage: httpd [port] [count]     (default 8080, 1)
 * exit:  0 = served; 2 socket  3 bind  4 listen  5 accept
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embk_socket.h"

int main(int argc, char **argv)
{
    int port   = (argc > 1) ? atoi(argv[1]) : 8080;
    int count  = (argc > 2) ? atoi(argv[2]) : 1;
    int bodysz = (argc > 3) ? atoi(argv[3]) : 0;   /* >0: serve that many bytes (windowed) */

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { printf("httpd: socket failed (%d)\n", lfd); return 2; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { printf("httpd: bind failed\n"); return 3; }
    if (listen(lfd, 4) != 0) { printf("httpd: listen failed\n"); return 4; }
    printf("httpd: listening on :%d\n", port);

    /* The body: a small page by default, or -- to exercise the windowed sender --
     * `bodysz` bytes of a repeating pattern sent in ONE write(), which net_tcp_send
     * pipelines across the peer's window. */
    static char big[65536];
    const char *body;
    int blen;
    if (bodysz > 0) {
        if (bodysz > (int)sizeof big) bodysz = sizeof big;
        for (int i = 0; i < bodysz; i++) big[i] = (char)('A' + (i % 26));
        body = big; blen = bodysz;
    } else {
        body = "<html><body><h1>Served by EmbLinkOS</h1></body></html>\n";
        blen = (int)strlen(body);
    }

    for (int i = 0; i < count; i++) {
        int cfd = accept(lfd, 0, 0);
        if (cfd < 0) { printf("httpd: accept failed (%d)\n", cfd); return 5; }
        printf("httpd: connection %d accepted (fd %d)\n", i, cfd);

        char req[512];
        long n = recv(cfd, req, sizeof req - 1, 0);
        if (n > 0) {
            req[n] = 0;
            char *nl = strchr(req, '\r'); if (nl) *nl = 0;
            printf("httpd: request: %s\n", req);
        }

        char hdr[160];
        int hl = snprintf(hdr, sizeof hdr,
            "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);
        send(cfd, hdr, hl, 0);
        send(cfd, body, blen, 0);            /* windowed if blen is large */
        close(cfd);
        printf("httpd: served connection %d (%d body bytes)\n", i, blen);
    }
    close(lfd);
    printf("httpd: done\n");
    return 0;
}
