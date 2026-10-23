/* arpa/inet.h -- EmbLink override header. Declarations; syscalls.c defines the
 * string converters for real (pure text work, nothing network about them). */
#ifndef _EMBK_ARPA_INET_H
#define _EMBK_ARPA_INET_H

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);
in_addr_t   inet_addr(const char *cp);        /* dotted-quad -> network order, or INADDR_NONE */

/* The legacy formatter: network order -> dotted-quad, in a STATIC buffer (so
 * two calls in one printf argument list overwrite each other -- that is the
 * historical contract, not a defect here). Implemented in user/lib/syscalls.c
 * and, until now, declared NOWHERE: every caller reached it through an
 * implicit declaration, which older compilers merely warned about. GCC 14
 * makes that an error, so git's connect.c stopped building the moment it met
 * a modern cross compiler. inet_ntop() is the one new code should use. */
char       *inet_ntoa(struct in_addr in);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_ARPA_INET_H */
