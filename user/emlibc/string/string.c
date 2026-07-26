/* string.c — emlibc. Pure algorithms, no syscalls: identical on any OS
 * (docs/EMLIBC_Requirements.md §4). Built -fno-builtin so the compiler does
 * not rewrite these loops into calls to the very functions they define. The
 * four the compiler can synthesize regardless (memcpy/memmove/memset/memcmp)
 * are here and correct — a static link needs them present. */

#include <string.h>
#include <stddef.h>

extern void *malloc(size_t);

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst; const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else       { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return 0;
}

size_t strlen(const char *s)          { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t m){ size_t i = 0; while (i < m && s[i]) i++; return i; }

char *strcpy(char *restrict d, const char *restrict s)
{ char *r = d; while ((*d++ = *s++)) { } return r; }

char *strncpy(char *restrict d, const char *restrict s, size_t n)
{
    char *r = d;
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *strcat(char *restrict d, const char *restrict s)
{ char *r = d; while (*d) d++; while ((*d++ = *s++)) { } return r; }

char *strncat(char *restrict d, const char *restrict s, size_t n)
{
    char *r = d; while (*d) d++;
    while (n && *s) { *d++ = *s++; n--; }
    *d = 0; return r;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) return (char *)last; }
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}

size_t strspn(const char *s, const char *set)
{
    size_t i = 0;
    for (; s[i]; i++) if (!strchr(set, s[i])) break;
    return i;
}

size_t strcspn(const char *s, const char *set)
{
    size_t i = 0;
    for (; s[i]; i++) if (strchr(set, s[i])) break;
    return i;
}

char *strpbrk(const char *s, const char *set)
{
    for (; *s; s++) if (strchr(set, *s)) return (char *)s;
    return 0;
}

char *strtok(char *restrict s, const char *restrict sep)
{
    static char *save;
    if (!s) s = save;
    if (!s) return 0;
    s += strspn(s, sep);                 /* skip leading separators */
    if (!*s) { save = 0; return 0; }
    char *tok = s;
    s += strcspn(s, sep);                /* run of non-separators */
    if (*s) { *s = 0; save = s + 1; } else save = 0;
    return tok;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* strerror — emlibc's own message table, keyed by emlibc's own errno numbers
 * (<errno.h>). Only codes the kernel can actually return get a real string. */
char *strerror(int e)
{
    switch (e) {
    case 0:   return "Success";
    case 1:   return "Operation not permitted";
    case 2:   return "No such file or directory";
    case 5:   return "I/O error";
    case 9:   return "Bad file descriptor";
    case 12:  return "Out of memory";
    case 13:  return "Permission denied";
    case 14:  return "Bad address";
    case 17:  return "File exists";
    case 20:  return "Not a directory";
    case 21:  return "Is a directory";
    case 22:  return "Invalid argument";
    case 28:  return "No space left on device";
    case 29:  return "Illegal seek";
    case 30:  return "Read-only file system";
    case 34:  return "Numerical result out of range";
    case 36:  return "File name too long";
    case 38:  return "Function not implemented";
    case 39:  return "Directory not empty";
    case 111: return "Connection refused";
    case 125: return "Operation canceled";
    default:  return "Unknown error";
    }
}
