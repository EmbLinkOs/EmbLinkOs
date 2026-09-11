#include "include/kstring.h"

/* WORD-WIDE, and it matters more than it looks. These three were byte loops,
 * and a byte loop is what every page zeroed, every page copied to the swap
 * store and every block bounced through the block layer paid for: measured at
 * 69 us per 4 KiB page under TCG -- 4.1 s of a 22 s paging run spent zeroing.
 * Eight bytes at a time once the destination is aligned, a tail of bytes.
 *
 * memcpy/memmove go word-wide only when BOTH sides can: aligning the
 * destination leaves the source aligned exactly when the two started congruent
 * mod 8, which is every page, block and buffer copy in the kernel. Anything
 * else stays a byte loop -- an unaligned word access is fine on x86 and on
 * aarch64 Normal memory, and a fault on aarch64 Device memory, and these
 * functions do not know which they were handed.
 *
 * The attribute keeps GCC from recognising a loop as the very idiom the
 * function implements and turning it into a call to itself. */
#define NO_LOOP_IDIOMS __attribute__((optimize("no-tree-loop-distribute-patterns")))
typedef uint64_t __attribute__((may_alias)) word_t;

NO_LOOP_IDIOMS void *memset(void *dest, int value, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t  b = (uint8_t)value;
    while (n && ((uintptr_t)d & 7)) { *d++ = b; n--; }
    word_t w = 0x0101010101010101ULL * b;
    while (n >= 8) { *(word_t *)d = w; d += 8; n -= 8; }
    while (n) { *d++ = b; n--; }
    return dest;
}

NO_LOOP_IDIOMS void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        while (n >= 8) { *(word_t *)d = *(const word_t *)s; d += 8; s += 8; n -= 8; }
    }
    while (n) { *d++ = *s++; n--; }
    return dest;
}

NO_LOOP_IDIOMS void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dest;
    /* Forward is safe when dest is below source (each store trails the load
     * that fed it) or when the ranges do not overlap at all. */
    if (d < s || d >= s + n)
        return memcpy(dest, src, n);
    /* dest inside [src, src+n): copy backwards so nothing is clobbered before
     * it is read. */
    d += n; s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *--d = *--s; n--; }
        while (n >= 8) { d -= 8; s -= 8; *(word_t *)d = *(const word_t *)s; n -= 8; }
    }
    while (n) { *--d = *--s; n--; }
    return dest;
}




int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)(p1[i]) - (int)(p2[i]);
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;

    }
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
        // Do nothing
    }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    size_t i;
    for (i = 0; i < n && *src != '\0'; i++) {
        *d++ = *src++;
    }
    for (; i < n; i++) {
        *d++ = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d != '\0') {
        d++;
    }
    while ((*d++ = *src++) != '\0') {
        // Do nothing
    }
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d != '\0') {
        d++;
    }
    size_t i;
    for (i = 0; i < n && *src != '\0'; i++) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 == *s2) {
        if (*s1 == '\0') {
            return 0;
        }
        s1++;
        s2++;
    }
    return (int)(uint8_t)*s1 - (int)(uint8_t)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (int)(uint8_t)s1[i] - (int)(uint8_t)s2[i];
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}
