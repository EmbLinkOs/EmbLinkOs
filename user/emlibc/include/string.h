/* string.h — emlibc. Pure algorithms, no OS in them: the agnostic bulk
 * (docs/EMLIBC_Requirements.md §4). size_t comes from the compiler's
 * freestanding <stddef.h>, so this header stands alone under -nostdinc. */
#ifndef _EMLIBC_STRING_H
#define _EMLIBC_STRING_H

#include <stddef.h>

void  *memcpy(void *restrict dst, const void *restrict src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char  *strcpy(char *restrict dst, const char *restrict src);
char  *strncpy(char *restrict dst, const char *restrict src, size_t n);
char  *strcat(char *restrict dst, const char *restrict src);
char  *strncat(char *restrict dst, const char *restrict src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
size_t strspn(const char *s, const char *set);
size_t strcspn(const char *s, const char *set);
char  *strpbrk(const char *s, const char *set);
char  *strtok(char *restrict s, const char *restrict sep);
char  *strdup(const char *s);
char  *strerror(int e);

#endif /* _EMLIBC_STRING_H */
