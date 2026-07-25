/* stdlib.h — emlibc. Agnostic bulk (alloc, conversions, qsort) plus the two
 * OS-facing calls that belong here: exit() (flushes stdio, runs nothing else
 * yet) and getenv/setenv over the environment crt0 published. */
#ifndef _EMLIBC_STDLIB_H
#define _EMLIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void  *malloc(size_t n);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);

void   exit(int code);
void   _Exit(int code);
void   abort(void);
int    atexit(void (*fn)(void));

int    atoi(const char *s);
long   atol(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

int    abs(int x);
long   labs(long x);

void   qsort(void *base, size_t nmemb, size_t size,
             int (*cmp)(const void *, const void *));

char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);

#endif /* _EMLIBC_STDLIB_H */
