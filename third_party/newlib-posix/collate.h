/* EmbLinkOS currently supplies the C locale. These private constants keep the
 * imported BSD/newlib matchers on their documented byte-order fallback. */
#ifndef EMBLINK_POSIX_COLLATE_H
#define EMBLINK_POSIX_COLLATE_H
#define __collate_load_error 1
static inline int __collate_range_cmp(int a,int b){return a-b;}
#endif
