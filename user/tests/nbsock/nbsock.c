/* nbsock -- exercises the NON-BLOCKING socket path end to end, the same shape
 * CPython's socket-with-timeout uses: O_NONBLOCK via fcntl, a connect() that
 * returns EINPROGRESS, select() for writability (connect completion) then for
 * readability, getsockopt(SO_ERROR) to reap the connect, and a recv() that
 * returns EAGAIN before data arrives. Talks plain HTTP to example.com:80 (no
 * TLS) so it isolates the kernel non-blocking machinery. `test nbsock`. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

int main(void) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("example.com", "80", &hints, &res) != 0 || !res) {
        printf("NBSOCK resolve FAIL\n"); return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NBSOCK socket FAIL\n"); return 1; }

    int fl = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) { printf("NBSOCK F_SETFL FAIL errno=%d\n", errno); return 1; }
    fl = fcntl(fd, F_GETFL, 0);
    printf("NBSOCK nonblock=%d\n", (fl & O_NONBLOCK) ? 1 : 0);
    if (!(fl & O_NONBLOCK)) { printf("NBSOCK flag-not-set FAIL\n"); return 1; }

    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    printf("NBSOCK connect r=%d errno=%d einprogress=%d\n", r, errno, EINPROGRESS);
    if (!(r < 0 && errno == EINPROGRESS)) { printf("NBSOCK connect-not-inprogress FAIL\n"); return 1; }

    fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
    struct timeval tv = { 8, 0 };
    r = select(fd + 1, NULL, &wf, NULL, &tv);
    printf("NBSOCK connect-select r=%d writable=%d\n", r, FD_ISSET(fd, &wf));
    if (r <= 0 || !FD_ISSET(fd, &wf)) { printf("NBSOCK connect-select FAIL\n"); return 1; }

    int soerr = 0; socklen_t sl = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    printf("NBSOCK SO_ERROR=%d\n", soerr);
    if (soerr != 0) { printf("NBSOCK connect-failed FAIL\n"); return 1; }

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    if (send(fd, req, strlen(req), 0) < 0) { printf("NBSOCK send FAIL errno=%d\n", errno); return 1; }

    /* Non-blocking read loop: EAGAIN until the reply arrives; select waits. */
    char buf[256];
    int saw_eagain = 0, got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        int n = recv(fd, buf, sizeof buf - 1, 0);
        if (n < 0 && errno == EAGAIN) {
            saw_eagain = 1;
            fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
            struct timeval t2 = { 8, 0 };
            if (select(fd + 1, &rf, NULL, NULL, &t2) <= 0) { printf("NBSOCK read-select timeout\n"); break; }
            continue;
        }
        if (n < 0) { printf("NBSOCK recv FAIL errno=%d\n", errno); break; }
        if (n == 0) { printf("NBSOCK EOF-before-data\n"); break; }
        buf[n] = 0;
        char *nl = strchr(buf, '\r');
        printf("NBSOCK status: %.*s\n", (int)(nl ? nl - buf : n), buf);
        got = 1;
    }
    close(fd);
    freeaddrinfo(res);
    printf("NBSOCK saw_eagain=%d -> %s\n", saw_eagain, got ? "OK" : "FAIL");
    return got ? 0 : 1;
}
