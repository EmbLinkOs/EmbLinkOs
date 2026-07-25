/* stdlib.c — emlibc. Allocation, process exit, conversions, qsort, environ.
 * The allocator and conversions are agnostic; exit()/getenv()/setenv() are the
 * OS-facing members (they flush the streams and read the environment crt0
 * published). */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern void  *emlibc_sbrk(long incr);
extern void   emlibc_stdio_flush_all(void);   /* stdio.c */
extern void   _exit(int code);                 /* rim/syscalls.c */
extern char **environ;

/* ------------------------------------------------------------------ */
/* Allocator — bump arena over sbrk + a first-fit free list.          */
/* Each block carries an 8-byte size header, so realloc knows the old */
/* size and free() can recycle. Not a compacting allocator; adequate  */
/* for real programs, and honest about being phase-1 simple.          */
/* ------------------------------------------------------------------ */
struct hdr { size_t size; struct hdr *next_free; };

static struct hdr *g_free;
static char *g_cur, *g_end;

void *malloc(size_t n)
{
    if (n == 0) n = 1;
    n = (n + 15) & ~(size_t)15;                     /* 16-byte align payloads */

    for (struct hdr **pp = &g_free; *pp; pp = &(*pp)->next_free) {
        if ((*pp)->size >= n) { struct hdr *h = *pp; *pp = h->next_free; return h + 1; }
    }

    size_t need = n + sizeof(struct hdr);
    if ((size_t)(g_end - g_cur) < need) {
        char *base = emlibc_sbrk(0);
        if (base == (void *)-1) return 0;
        size_t grow = need > 0x10000 ? need : 0x10000;   /* grow in >=64 KB slabs */
        grow = (grow + 0xFFF) & ~(size_t)0xFFF;
        if (emlibc_sbrk((long)grow) == (void *)-1) { errno = ENOMEM; return 0; }
        if (g_cur && base == g_end) g_end = base + grow; /* contiguous extend */
        else { g_cur = base; g_end = base + grow; }
    }

    struct hdr *h = (struct hdr *)g_cur;
    g_cur += need;
    h->size = n;
    h->next_free = 0;
    return h + 1;
}

void free(void *p)
{
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    h->next_free = g_free;
    g_free = h;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t n = nmemb * size;
    if (size && n / size != nmemb) { errno = ENOMEM; return 0; }   /* overflow */
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    struct hdr *h = (struct hdr *)p - 1;
    if (h->size >= n) return p;
    void *q = malloc(n);
    if (q) { memcpy(q, p, h->size); free(p); }
    return q;
}

/* ------------------------------------------------------------------ */
/* Process exit                                                        */
/* ------------------------------------------------------------------ */
#define ATEXIT_MAX 32
static void (*g_atexit[ATEXIT_MAX])(void);
static int   g_natexit;

int atexit(void (*fn)(void))
{
    if (g_natexit >= ATEXIT_MAX) return -1;
    g_atexit[g_natexit++] = fn;
    return 0;
}

void exit(int code)
{
    for (int i = g_natexit - 1; i >= 0; i--) if (g_atexit[i]) g_atexit[i]();
    emlibc_stdio_flush_all();
    _exit(code);
    for (;;) { }
}

void _Exit(int code) { _exit(code); for (;;) { } }   /* no flush, no handlers */

void abort(void)
{
    emlibc_stdio_flush_all();
    _exit(134);                                       /* 128 + SIGABRT */
    for (;;) { }
}

/* ------------------------------------------------------------------ */
/* Numeric conversions                                                 */
/* ------------------------------------------------------------------ */
int  abs(int x)   { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') { base = 8; }
    else if (base == 0) base = 10;

    long acc = 0; int any = 0;
    for (; ; p++) {
        int d = digit_val((unsigned char)*p);
        if (d >= base) break;
        acc = acc * base + d; any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -acc : acc;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

/* ------------------------------------------------------------------ */
/* qsort — shellsort (compact, non-recursive, no worst-case blowup).   */
/* ------------------------------------------------------------------ */
static void em_swap(char *a, char *b, size_t sz)
{
    while (sz--) { char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    char *a = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            for (size_t j = i; j >= gap; j -= gap) {
                char *x = a + (j - gap) * sz, *y = a + j * sz;
                if (cmp(x, y) <= 0) break;
                em_swap(x, y, sz);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Environment — getenv over crt0's vector; setenv grows a private copy */
/* ------------------------------------------------------------------ */
static char **g_env_own;     /* our growable copy, once setenv is used */
static size_t g_env_n, g_env_cap;

static size_t name_len(const char *entry)   /* length up to '=' */
{
    const char *e = strchr(entry, '=');
    return e ? (size_t)(e - entry) : strlen(entry);
}

char *getenv(const char *name)
{
    if (!environ) return 0;
    size_t nl = strlen(name);
    for (char **e = environ; *e; e++)
        if (name_len(*e) == nl && strncmp(*e, name, nl) == 0 && (*e)[nl] == '=')
            return *e + nl + 1;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t nl = strlen(name), vl = strlen(value);
    char *entry = malloc(nl + 1 + vl + 1);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, nl); entry[nl] = '='; memcpy(entry + nl + 1, value, vl);
    entry[nl + 1 + vl] = 0;

    /* On first mutation, copy the parent's vector into a private growable one. */
    if (g_env_own == 0) {
        size_t cnt = 0;
        if (environ) while (environ[cnt]) cnt++;
        g_env_cap = cnt + 8;
        g_env_own = malloc(g_env_cap * sizeof(char *));
        if (!g_env_own) { free(entry); errno = ENOMEM; return -1; }
        for (size_t i = 0; i < cnt; i++) g_env_own[i] = environ[i];
        g_env_n = cnt; g_env_own[g_env_n] = 0;
        environ = g_env_own;
    }

    for (size_t i = 0; i < g_env_n; i++) {
        if (name_len(g_env_own[i]) == nl && strncmp(g_env_own[i], name, nl) == 0) {
            if (!overwrite) { free(entry); return 0; }
            g_env_own[i] = entry; return 0;
        }
    }
    if (g_env_n + 1 >= g_env_cap) {
        size_t nc = g_env_cap * 2;
        char **ne = realloc(g_env_own, nc * sizeof(char *));
        if (!ne) { free(entry); errno = ENOMEM; return -1; }
        g_env_own = ne; g_env_cap = nc; environ = g_env_own;
    }
    g_env_own[g_env_n++] = entry;
    g_env_own[g_env_n] = 0;
    return 0;
}
