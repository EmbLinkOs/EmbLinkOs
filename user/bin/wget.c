/* wget.c -- a real HTTP downloader for EmbLinkOS. Resolves a host, opens a TCP
 * connection, sends an HTTP/1.0 GET, splits the response header from the body,
 * and writes the body to a file (or stdout). It is where this OS's networking
 * stack meets its filesystem: "fetch a file from the internet and save it."
 *
 * Built on the BSD-sockets shim (embk_socket.h) over the native CAP_NETWORK
 * socket syscalls, plus newlib file I/O (needs CAP_FILESYSTEM to write a file).
 * http:// only -- there is no TLS yet, so https:// is refused honestly.
 *
 * usage: wget [-O outfile] http://host[:port]/path
 * exit:  0 = 2xx downloaded;  1 usage  2 not-http  3 resolve  4 socket
 *        5 connect  6 open-outfile  7 non-2xx status
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "embk_socket.h"

static int parse_url(const char *url, char *host, int hostsz, int *port,
                     char *path, int pathsz) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) return -2;   /* no TLS */
    if (strncmp(p, "http://", 7) == 0)  p += 7;
    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < hostsz - 1) host[i++] = *p++;
    host[i] = 0;
    if (i == 0) return -1;
    *port = 80;
    if (*p == ':') { p++; *port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') { strncpy(path, p, pathsz - 1); path[pathsz - 1] = 0; }
    else strcpy(path, "/");
    return 0;
}

int main(int argc, char **argv)
{
    const char *outfile = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) outfile = argv[++i];
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: wget [-O file] http://host/path\n"); return 1; }

    char host[128], path[256]; int port;
    int pr = parse_url(url, host, sizeof host, &port, path, sizeof path);
    if (pr == -2) { fprintf(stderr, "wget: https:// not supported (no TLS yet)\n"); return 2; }
    if (pr < 0)   { fprintf(stderr, "wget: bad URL '%s'\n", url); return 2; }

    struct in_addr addr;
    if (emb_resolve(host, &addr) != 0) { fprintf(stderr, "wget: cannot resolve %s\n", host); return 3; }
    unsigned int h = ntohl(addr.s_addr);
    fprintf(stderr, "wget: %s -> %u.%u.%u.%u, GET %s\n", host,
            (h >> 24) & 255, (h >> 16) & 255, (h >> 8) & 255, h & 255, path);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fprintf(stderr, "wget: socket failed (%d)\n", fd); return 4; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port); sa.sin_addr = addr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "wget: connect failed\n"); return 5;
    }

    char req[512];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: EmbLinkOS-wget\r\n"
                      "Connection: close\r\n\r\n", path, host);
    send(fd, req, rl, 0);

    int out = 1;                                   /* default: stdout */
    if (outfile) {
        out = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (out < 0) { fprintf(stderr, "wget: cannot open %s\n", outfile); return 6; }
    }

    /* Read the stream; the header ends at the first CRLFCRLF, everything after
     * is body. The header may straddle recv() boundaries, so accumulate it. */
    static char hdr[4096];
    char buf[2048];
    int hlen = 0, header_done = 0, status = 0, body = 0;
    for (;;) {
        long n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        int off = 0;
        if (!header_done) {
            for (int i = 0; i < n && !header_done; i++) {
                if (hlen < (int)sizeof(hdr) - 1) hdr[hlen++] = buf[i];
                if (hlen >= 4 && hdr[hlen-4]=='\r' && hdr[hlen-3]=='\n' &&
                                 hdr[hlen-2]=='\r' && hdr[hlen-1]=='\n') {
                    header_done = 1; off = i + 1;
                }
            }
            if (!header_done) continue;            /* need more header bytes */
            hdr[hlen] = 0;
            if (!strncmp(hdr, "HTTP/1.", 7)) status = atoi(hdr + 9);
        }
        int blen = (int)n - off;
        if (blen > 0) { write(out, buf + off, blen); body += blen; }
    }
    close(fd);
    if (outfile) close(out);

    fprintf(stderr, "wget: HTTP %d, %d bytes%s%s\n",
            status, body, outfile ? " -> " : "", outfile ? outfile : "");
    return (status / 100 == 2) ? 0 : 7;
}
