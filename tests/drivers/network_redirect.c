/* Test-only host interposer. Redirect QEMU/SLIRP's outbound DNS and HTTP to
 * isolated loopback fixtures; the guest still uses ordinary DHCP DNS and TCP.
 * Loaded only by tools/network_smoke.py, never included in the OS image. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
static const struct sockaddr *redirect(const struct sockaddr *in,socklen_t n,struct sockaddr_in *out) {
    if(!in || n<sizeof(*out) || in->sa_family!=AF_INET) return in;
    memcpy(out,in,sizeof(*out));unsigned port=ntohs(out->sin_port);
    const char *setting=port==53?getenv("EMBLINK_TEST_DNS_PORT"):port==80?getenv("EMBLINK_TEST_HTTP_PORT"):0;
    if(!setting) return in;
    char *end;unsigned long target=strtoul(setting,&end,10);
    if(*end || !target || target>65535) return in;
    out->sin_addr.s_addr=htonl(INADDR_LOOPBACK);out->sin_port=htons(target);
    return (const struct sockaddr *)out;
}
int connect(int fd,const struct sockaddr *addr,socklen_t n) {
    int (*real_connect)(int,const struct sockaddr *,socklen_t)=dlsym(RTLD_NEXT,"connect");
    struct sockaddr_in out;const struct sockaddr *target=redirect(addr,n,&out);
    return real_connect(fd,target,target==addr?n:(socklen_t)sizeof out);
}
ssize_t sendto(int fd,const void *buf,size_t size,int flags,const struct sockaddr *addr,socklen_t n) {
    ssize_t (*real_sendto)(int,const void *,size_t,int,const struct sockaddr *,socklen_t)=dlsym(RTLD_NEXT,"sendto");
    struct sockaddr_in out;const struct sockaddr *target=redirect(addr,n,&out);
    return real_sendto(fd,buf,size,flags,target,target==addr?n:(socklen_t)sizeof out);
}
