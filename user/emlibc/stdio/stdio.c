/* stdio.c — emlibc. Buffered streams and the printf family, over the
 * read/write/open rim (rim/syscalls.c). The formatting is agnostic; only the
 * FILE→fd plumbing is EmbLink-specific.
 *
 * Phase 1 scope, stated honestly (THE RULE): integer / string / char / hex /
 * pointer conversions with flags, width, and precision. Floating point
 * (%f/%g/%e) is NOT yet implemented — an unhandled conversion is echoed
 * literally rather than printing a fabricated number. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

extern long  write(int fd, const void *buf, size_t n);   /* rim */
extern long  read(int fd, void *buf, size_t n);
extern int   close(int fd);
extern int   open(const char *path, int flags, ...);

/* stream flags */
#define F_READ   0x01
#define F_WRITE  0x02
#define F_LINE   0x04   /* flush on '\n' */
#define F_UNBUF  0x08   /* flush every byte */
#define F_EOF    0x10
#define F_ERR    0x20
#define F_OPEN   0x40   /* fopen'd: close+free on fclose */

struct _EM_FILE {
    int  fd;
    int  flags;
    unsigned char *buf;
    int  len, cap;
};

#define BUFSZ 1024
static unsigned char g_out_buf[BUFSZ], g_err_buf[BUFSZ];

static FILE g_stdin  = { 0, F_READ | F_UNBUF, 0, 0, 0 };
static FILE g_stdout = { 1, F_WRITE | F_LINE, g_out_buf, 0, BUFSZ };
static FILE g_stderr = { 2, F_WRITE | F_UNBUF, g_err_buf, 0, BUFSZ };

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

/* ---- the buffer machinery ---------------------------------------- */
int fflush(FILE *f)
{
    if (!f) return 0;
    if ((f->flags & F_WRITE) && f->len > 0) {
        long n = write(f->fd, f->buf, (size_t)f->len);
        if (n != f->len) { f->flags |= F_ERR; f->len = 0; return EOF; }
        f->len = 0;
    }
    return 0;
}

/* Called by exit(): flush the standard streams so a program that printed and
 * returned still shows its output. Registered implicitly via stdlib's exit. */
void emlibc_stdio_flush_all(void)
{
    fflush(&g_stdout);
    fflush(&g_stderr);
}

static void put_byte(FILE *f, unsigned char c)
{
    if (!(f->flags & F_WRITE) || !f->buf) return;
    f->buf[f->len++] = c;
    if (f->len >= f->cap || (f->flags & F_UNBUF) ||
        ((f->flags & F_LINE) && c == '\n'))
        fflush(f);
}

int fputc(int c, FILE *f) { put_byte(f, (unsigned char)c); return (unsigned char)c; }
int putc(int c, FILE *f)  { return fputc(c, f); }
int putchar(int c)        { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    while (*s) put_byte(f, (unsigned char)*s++);
    return 0;
}

int puts(const char *s)
{
    fputs(s, stdout);
    put_byte(stdout, '\n');
    return 0;
}

size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *f)
{
    const unsigned char *b = p;
    size_t total = size * nmemb;
    for (size_t i = 0; i < total; i++) put_byte(f, b[i]);
    return (f->flags & F_ERR) ? 0 : nmemb;
}

/* ---- input ------------------------------------------------------- */
int fgetc(FILE *f)
{
    unsigned char c;
    long n = read(f->fd, &c, 1);
    if (n <= 0) { f->flags |= (n == 0 ? F_EOF : F_ERR); return EOF; }
    return c;
}

int getchar(void) { return fgetc(stdin); }

char *fgets(char *buf, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    buf[i] = 0;
    return buf;
}

size_t fread(void *p, size_t size, size_t nmemb, FILE *f)
{
    unsigned char *b = p;
    size_t total = size * nmemb, got = 0;
    while (got < total) {
        long n = read(f->fd, b + got, total - got);
        if (n <= 0) { f->flags |= (n == 0 ? F_EOF : F_ERR); break; }
        got += (size_t)n;
    }
    return size ? got / size : 0;
}

int feof(FILE *f)   { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }

/* ---- streams ----------------------------------------------------- */
FILE *fopen(const char *path, const char *mode)
{
    int flags = 0, rd = 0, wr = 0;
    switch (mode[0]) {
    case 'r': flags = 0x0000; rd = 1; if (mode[1] == '+') { flags = 0x0002; wr = 1; } break;
    case 'w': flags = 0x0001 | 0x0040 | 0x0200; wr = 1;   /* O_WRONLY|O_CREAT|O_TRUNC */
              if (mode[1] == '+') { flags = 0x0002 | 0x0040 | 0x0200; rd = 1; } break;
    case 'a': flags = 0x0001 | 0x0040 | 0x0400; wr = 1;   /* O_WRONLY|O_CREAT|O_APPEND */
              if (mode[1] == '+') { flags = 0x0002 | 0x0040 | 0x0400; rd = 1; } break;
    default: return 0;
    }
    int fd = open(path, flags);
    if (fd < 0) return 0;
    FILE *f = malloc(sizeof *f);
    if (!f) { close(fd); return 0; }
    f->fd = fd;
    f->flags = F_OPEN | (rd ? F_READ : 0) | (wr ? F_WRITE : 0);
    f->cap = wr ? BUFSZ : 0;
    f->buf = wr ? malloc(BUFSZ) : 0;
    f->len = 0;
    if (wr && !f->buf) { close(fd); free(f); return 0; }
    return f;
}

int fclose(FILE *f)
{
    if (!f) return EOF;
    fflush(f);
    int rc = close(f->fd);
    if (f->flags & F_OPEN) { free(f->buf); free(f); }
    return rc;
}

/* ================================================================== */
/* The formatter                                                      */
/* ================================================================== */
typedef void (*emit_fn)(void *ctx, const char *s, size_t n);

/* unsigned -> text (base 8/10/16), returned right-aligned in tmp; returns ptr
 * to the first digit and sets *outlen. */
static char *u_to_str(unsigned long long v, int base, int upper, char *tmp, int *outlen)
{
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = tmp + 32; *p = 0;
    if (v == 0) *--p = '0';
    while (v) { *--p = digs[v % base]; v /= base; }
    *outlen = (int)(tmp + 32 - p);
    return p;
}

struct fmt_flags { int minus, zero, plus, space, hash, width, prec, has_prec; };

static void emit_pad(emit_fn emit, void *ctx, char c, int n, int *count)
{
    char b[16];
    for (int i = 0; i < 16; i++) b[i] = c;
    while (n > 0) { int k = n < 16 ? n : 16; emit(ctx, b, (size_t)k); *count += k; n -= k; }
}

/* Floating-point conversions (%f/%e/%g) with field width + flags. Self-
 * contained -- IEEE-754 bit tests for sign/nan/inf, integer+fraction split for
 * the digits -- so pulling printf never drags in libm. The mantissa is built
 * into a buffer, then padded/signed by the caller. Pragmatic for typical
 * magnitudes (< ~1e18); a production formatter would use a correctly-rounded
 * dtoa. Rounding carry into an extra digit at the %e boundary is not handled. */
static int f_uint(char *out, unsigned long long v)
{
    char t[24]; int n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    return n;
}
static int dec_exp10(double v)
{
    int e = 0; if (v == 0.0) return 0;
    while (v >= 10.0) { v /= 10.0; e++; }
    while (v <  1.0)  { v *= 10.0; e--; }
    return e;
}
/* v >= 0 -> "iii.fff" with `prec` fractional digits. */
static int build_f(char *out, double v, int prec)
{
    double scale = 1.0; for (int i = 0; i < prec; i++) scale *= 10.0;
    /* EmbCC now has the unsigned-64<->double conversions, so the integer part
     * spans the full [0, 2^64) a %f can present without a signed-range fixup. */
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    unsigned long long fp = (unsigned long long)(frac * scale + 0.5);
    if (fp >= (unsigned long long)scale) { fp -= (unsigned long long)scale; ip++; }
    int n = f_uint(out, ip);
    if (prec > 0) {
        out[n++] = '.';
        char t[24]; int fl = f_uint(t, fp);
        for (int z = fl; z < prec; z++) out[n++] = '0';
        for (int i = 0; i < fl; i++) out[n++] = t[i];
    }
    return n;
}
/* v >= 0 -> "d.ddde+XX" with `prec` mantissa-fraction digits. */
static int build_e(char *out, double v, int prec, char ec)
{
    int e = 0;
    if (v != 0.0) { while (v >= 10.0) { v /= 10.0; e++; } while (v < 1.0) { v *= 10.0; e--; } }
    int n = build_f(out, v, prec);
    out[n++] = ec;
    out[n++] = (e < 0) ? '-' : '+';
    int ae = e < 0 ? -e : e;
    if (ae >= 100) { out[n++] = (char)('0' + ae / 100); ae %= 100; }
    out[n++] = (char)('0' + (ae / 10) % 10);
    out[n++] = (char)('0' + ae % 10);
    return n;
}
/* Strip trailing fractional zeros (and a bare '.') from a %g mantissa. */
static int strip_zeros(char *s, int n)
{
    int epos = -1;
    for (int i = 0; i < n; i++) if (s[i] == 'e' || s[i] == 'E') { epos = i; break; }
    int mend = (epos < 0) ? n : epos, dot = -1;
    for (int i = 0; i < mend; i++) if (s[i] == '.') { dot = i; break; }
    if (dot < 0) return n;
    int j = mend - 1;
    while (j > dot && s[j] == '0') j--;
    if (j == dot) j--;                       /* drop the '.' too */
    int keep = j + 1;
    if (epos >= 0) { int el = n - epos; for (int k = 0; k < el; k++) s[keep + k] = s[epos + k]; return keep + el; }
    return keep;
}

static void fmt_double(emit_fn emit, void *ctx, double v, char conv,
                       const struct fmt_flags *f, int *count)
{
    union { double d; unsigned long long u; } b; b.d = v;
    int neg = (int)(b.u >> 63);
    if (neg) { v = -v; b.d = v; }
    char sign = neg ? '-' : (f->plus ? '+' : (f->space ? ' ' : 0));

    char body[560]; int n = 0; int is_special = 0;
    if (((b.u >> 52) & 0x7ff) == 0x7ff) {
        int up = (conv < 'a');                          /* upper conv -> NAN/INF */
        const char *s = (b.u & 0xfffffffffffffULL) ? (up ? "NAN" : "nan")
                                                    : (up ? "INF" : "inf");
        while (*s) body[n++] = *s++;
        is_special = 1;
    } else {
        int prec = f->has_prec ? f->prec : 6;
        char c = (conv == 'F') ? 'f' : (conv == 'E') ? 'e' : (conv == 'G') ? 'g' : conv;
        if (c == 'f')      n = build_f(body, v, prec);
        else if (c == 'e') n = build_e(body, v, prec, (conv == 'E') ? 'E' : 'e');
        else {                                          /* %g / %G */
            int P = f->has_prec ? (f->prec ? f->prec : 1) : 6;
            int X = dec_exp10(v);
            if (X >= -4 && X < P) n = build_f(body, v, P - 1 - X);
            else                  n = build_e(body, v, P - 1, (conv == 'G') ? 'E' : 'e');
            if (!f->hash) n = strip_zeros(body, n);
        }
    }

    int total = (sign ? 1 : 0) + n;
    int pad = f->width - total; if (pad < 0) pad = 0;
    if (f->minus) {
        if (sign) { emit(ctx, &sign, 1); (*count)++; }
        emit(ctx, body, (size_t)n); *count += n;
        emit_pad(emit, ctx, ' ', pad, count);
    } else if (f->zero && !is_special) {                /* zero-pad after the sign */
        if (sign) { emit(ctx, &sign, 1); (*count)++; }
        emit_pad(emit, ctx, '0', pad, count);
        emit(ctx, body, (size_t)n); *count += n;
    } else {
        emit_pad(emit, ctx, ' ', pad, count);
        if (sign) { emit(ctx, &sign, 1); (*count)++; }
        emit(ctx, body, (size_t)n); *count += n;
    }
}

static int em_vformat(emit_fn emit, void *ctx, const char *fmt, va_list ap)
{
    int count = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(ctx, p, 1); count++; continue; }
        p++;
        if (*p == '%') { emit(ctx, p, 1); count++; continue; }

        struct fmt_flags f = {0,0,0,0,0,0,0,0};
        for (;; p++) {
            if (*p == '-') f.minus = 1;
            else if (*p == '0') f.zero = 1;
            else if (*p == '+') f.plus = 1;
            else if (*p == ' ') f.space = 1;
            else if (*p == '#') f.hash = 1;
            else break;
        }
        if (*p == '*') { f.width = va_arg(ap, int); p++; if (f.width < 0) { f.minus = 1; f.width = -f.width; } }
        else while (*p >= '0' && *p <= '9') f.width = f.width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++; f.has_prec = 1;
            if (*p == '*') { f.prec = va_arg(ap, int); p++; if (f.prec < 0) f.has_prec = 0; }
            else while (*p >= '0' && *p <= '9') f.prec = f.prec * 10 + (*p++ - '0');
        }
        int lng = 0;   /* 1 = long, 2 = long long, -1 = size_t */
        for (;;) {
            if (*p == 'l') { lng = (lng == 1 ? 2 : 1); p++; }
            else if (*p == 'z') { lng = -1; p++; }
            else if (*p == 'h') { p++; }        /* promoted through va_arg anyway */
            else break;
        }

        char c = *p;
        char tmp[40], prefix[3]; int prefixn = 0, isneg = 0;
        char *digits = 0; int dlen = 0;

        switch (c) {
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad = f.width - 1;
            if (!f.minus) emit_pad(emit, ctx, ' ', pad, &count);
            emit(ctx, &ch, 1); count++;
            if (f.minus) emit_pad(emit, ctx, ' ', pad, &count);
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = (int)(f.has_prec ? strnlen(s, (size_t)f.prec) : strlen(s));
            int pad = f.width - slen;
            if (!f.minus) emit_pad(emit, ctx, ' ', pad, &count);
            emit(ctx, s, (size_t)slen); count += slen;
            if (f.minus) emit_pad(emit, ctx, ' ', pad, &count);
            continue;
        }
        case 'd': case 'i': {
            long long v = (lng == 2) ? va_arg(ap, long long)
                        : (lng == 1) ? va_arg(ap, long)
                        : (lng == -1) ? (long long)va_arg(ap, size_t)
                                      : va_arg(ap, int);
            unsigned long long u = v < 0 ? (isneg = 1, (unsigned long long)(-v)) : (unsigned long long)v;
            digits = u_to_str(u, 10, 0, tmp, &dlen);
            if (isneg) prefix[prefixn++] = '-';
            else if (f.plus) prefix[prefixn++] = '+';
            else if (f.space) prefix[prefixn++] = ' ';
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long u = (lng == 2) ? va_arg(ap, unsigned long long)
                                 : (lng == 1) ? va_arg(ap, unsigned long)
                                 : (lng == -1) ? (unsigned long long)va_arg(ap, size_t)
                                              : va_arg(ap, unsigned int);
            int base = (c == 'o') ? 8 : (c == 'u') ? 10 : 16;
            digits = u_to_str(u, base, c == 'X', tmp, &dlen);
            if (f.hash && u && c == 'x') { prefix[prefixn++] = '0'; prefix[prefixn++] = 'x'; }
            if (f.hash && u && c == 'X') { prefix[prefixn++] = '0'; prefix[prefixn++] = 'X'; }
            break;
        }
        case 'p': {
            unsigned long long u = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            digits = u_to_str(u, 16, 0, tmp, &dlen);
            prefix[prefixn++] = '0'; prefix[prefixn++] = 'x';
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            fmt_double(emit, ctx, va_arg(ap, double), c, &f, &count);
            continue;
        default:
            /* A conversion emlibc does not implement: echo it literally rather
             * than printing a fabricated value. */
            emit(ctx, "%", 1); count++;
            if (c) { emit(ctx, p, 1); count++; }
            continue;
        }

        /* integer common tail: precision (min digits), width, pad */
        int zeros = (f.has_prec && f.prec > dlen) ? f.prec - dlen : 0;
        int bodylen = prefixn + zeros + dlen;
        int pad = f.width - bodylen;
        int zeropad = (f.zero && !f.minus && !f.has_prec) ? (pad > 0 ? pad : 0) : 0;
        if (zeropad) pad = 0;

        if (!f.minus) emit_pad(emit, ctx, ' ', pad, &count);
        if (prefixn) { emit(ctx, prefix, (size_t)prefixn); count += prefixn; }
        if (zeropad) emit_pad(emit, ctx, '0', zeropad, &count);
        if (zeros)   emit_pad(emit, ctx, '0', zeros, &count);
        emit(ctx, digits, (size_t)dlen); count += dlen;
        if (f.minus) emit_pad(emit, ctx, ' ', pad, &count);
    }
    return count;
}

/* ---- sinks ------------------------------------------------------- */
static void file_emit(void *ctx, const char *s, size_t n)
{
    FILE *f = ctx;
    for (size_t i = 0; i < n; i++) put_byte(f, (unsigned char)s[i]);
}

struct buf_sink { char *buf; size_t cap, pos; };
static void buf_emit(void *ctx, const char *s, size_t n)
{
    struct buf_sink *b = ctx;
    for (size_t i = 0; i < n; i++, b->pos++)
        if (b->pos + 1 < b->cap) b->buf[b->pos] = s[i];   /* leave room for NUL */
}

/* ---- public entry points ----------------------------------------- */
int vfprintf(FILE *f, const char *fmt, va_list ap) { return em_vformat(file_emit, f, fmt, ap); }
int vprintf(const char *fmt, va_list ap)           { return em_vformat(file_emit, stdout, fmt, ap); }

int fprintf(FILE *f, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = em_vformat(file_emit, f, fmt, ap); va_end(ap); return r; }

int printf(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = em_vformat(file_emit, stdout, fmt, ap); va_end(ap); return r; }

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct buf_sink b = { buf, cap, 0 };
    int r = em_vformat(buf_emit, &b, fmt, ap);
    if (cap) buf[b.pos < cap ? b.pos : cap - 1] = 0;
    return r;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, cap, fmt, ap); va_end(ap); return r; }

int sprintf(char *buf, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, (size_t)-1, fmt, ap); va_end(ap); return r; }
