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
    int port  = (argc > 1) ? atoi(argv[1]) : 8080;
    int count = (argc > 2) ? atoi(argv[2]) : 1;

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

    const char *body =
        "<html><body><h1>Served by EmbLinkOS</h1></body></html>\n";

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

        char resp[256];
        int rl = snprintf(resp, sizeof resp,
            "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(body), body);
        send(cfd, resp, rl, 0);
        close(cfd);
        printf("httpd: served connection %d\n", i);
    }
    close(lfd);
    printf("httpd: done\n");
    return 0;
}
