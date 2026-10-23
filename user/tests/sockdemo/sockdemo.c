/* sockdemo -- prove the newlib BSD-sockets shim: a plain POSIX HTTP client that
 * uses the standard <sys/socket.h>/<netdb.h> API (getaddrinfo, socket, connect,
 * send, recv, inet_ntop) -- NOT the native embk_socket.h shim. This is exactly
 * the code path Python's _socket / git / curl take, now that syscalls.c routes
 * those symbols to the kernel's CAP_NETWORK sockets. Needs CAP_NETWORK.
 *
 * exit: 0 ok  2 resolve  3 socket  4 connect  5 no data
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "example.com";

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, "80", &hints, &res);
    if (e) { printf("sockdemo: getaddrinfo(%s) failed: %s\n", host, gai_strerror(e)); return 2; }

    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
    printf("sockdemo: %s -> %s:%d (POSIX getaddrinfo/inet_ntop)\n", host, ip, ntohs(sa->sin_port));

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { printf("sockdemo: socket() failed\n"); freeaddrinfo(res); return 3; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { printf("sockdemo: connect() failed\n"); return 4; }
    freeaddrinfo(res);
    printf("sockdemo: connected via POSIX socket()/connect()\n");

    char req[256];
    int rl = snprintf(req, sizeof req,
                      "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    send(fd, req, (size_t)rl, 0);

    char buf[1024]; long total = 0; int printed = 0;
    for (;;) {
        long n = recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0) break;
        total += n;
        if (!printed) {
            buf[n] = 0;
            char *nl = strchr(buf, '\r'); if (nl) *nl = 0;
            printf("sockdemo: <- %s\n", buf);
            printed = 1;
        }
    }
    close(fd);
    printf("sockdemo: read %ld bytes via POSIX recv()\n", total);
    return total > 0 ? 0 : 5;
}
