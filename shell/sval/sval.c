/* ==========================================================================
 * sval.c -- the structured-value SDK. See sval.h.
 * Uses the newlib POSIX layer (read/write/fstat) -- so any program linking
 * this is a newlib program, not a freestanding -nostdlib one. That's fine:
 * the shell, ls, the terminal are all newlib apps.
 * ========================================================================== */
#include "sval.h"
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>       /* snprintf for scalar formatting */
#include <stdint.h>

/* VFS_DT_FIFO / VFS_DT_CHAR as newlib's fstat reports them. The kernel maps
 * its vfs_stat.type into st_mode; a pipe is S_IFIFO. We check S_ISFIFO rather
 * than a raw EmbLink type constant so this rides on the standard stat shape
 * the newlib _fstat stub already fills. */

/* ==========================================================================
 * The mark of a program that speaks the wire.
 *
 * A pipe on fd 0 tells a program that structured input MAY be arriving; it
 * cannot tell the SHELL whether the program will understand it. Most programs
 * on the machine are ordinary text programs -- sh, a port, a script -- which
 * would read a wire frame on stdin as garbage and print to fd 1 where the
 * pipeline never sees it. So a program that links this SDK carries a named
 * section, and the shell reads a program file's section table for it before
 * deciding how to plumb a stage (eval_extern.c). It is referenced from the
 * mode checks below, which every structured program calls, so no linker
 * that keeps code drops it.
 * ========================================================================== */
#if defined(__APPLE__)                         /* the host tests' Mach-O spells sections segment,name */
__attribute__((used, section("__DATA,__embk_sval")))
#else
__attribute__((used, section(SVAL_WIRE_SECTION)))
#endif
const char sval_wire_marker[] = "EMBK-SVAL 1";

/* ==========================================================================
 * Mode detection
 * ========================================================================== */
static bool fd_is_fifo(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return false;   /* EBADF (unopened fd 3) -> not structured */
    return S_ISFIFO(st.st_mode);
}
bool sval_structured_out(void) { (void)*(volatile const char *)sval_wire_marker; return fd_is_fifo(SVAL_FD_OUT); }
bool sval_structured_in(void)  { (void)*(volatile const char *)sval_wire_marker; return fd_is_fifo(SVAL_FD_IN); }

/* ==========================================================================
 * Emit
 * ========================================================================== */
/* Write every byte or fail. A frame is all-or-nothing: a partial one is a
 * protocol error at the far end ("corrupt structured output"), so a write
 * that stops early must never be mistaken for success.
 *
 * Only EPIPE is terminal (the reader hung up -- a normal end of a pipeline).
 * Anything else is treated as transient and retried: the kernel's user-copy
 * can still surface a spurious fault, and giving up on one would truncate an
 * otherwise-fine frame. Bounded so a genuinely broken fd can't spin forever
 * -- a real EPIPE exits immediately, so the cap only bites on the pathological
 * case. (Mirrors the retry/EPIPE split in the shell's extern feed loop.) */
#define WRITE_ALL_RETRIES 64

static int write_all(int fd, const uint8_t *p, size_t n) {
    size_t off = 0;
    int stalls = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            stalls = 0;                      /* progress: reset the budget */
            continue;
        }
        if (w < 0 && errno == EPIPE) {
            return -1;                       /* reader gone: terminal, stop now */
        }
        if (++stalls > WRITE_ALL_RETRIES) {
            return -1;                       /* not making progress; give up */
        }
        /* EINTR, a transient fault, or a 0-byte return: retry this offset. */
    }
    return 0;
}

int sval_emit_fd(int fd, const struct value *v) {
    struct wire_buf frame;
    wire_buf_init(&frame);
    if (wire_serialize(v, &frame)) { wire_buf_free(&frame); return -1; }
    int rc = write_all(fd, frame.data, frame.len);
    wire_buf_free(&frame);
    return rc;
}
int sval_emit(const struct value *v) { return sval_emit_fd(SVAL_FD_OUT, v); }

/* ==========================================================================
 * Reader
 * ========================================================================== */
void sval_reader_init(struct sval_reader *r) { wire_reader_init(&r->wr); }
void sval_reader_free(struct sval_reader *r) { wire_reader_free(&r->wr); }

int sval_reader_feed(struct sval_reader *r, const uint8_t *data, size_t n) {
    return wire_reader_feed(&r->wr, data, n);
}
int sval_reader_next(struct sval_reader *r, struct value *out) {
    return wire_reader_next(&r->wr, out);
}

int sval_read_blocking(struct sval_reader *r, int fd, struct value *out) {
    /* First, maybe a whole value is already buffered from a previous read that
     * delivered more than one frame -- drain that before touching the fd. */
    int rc = sval_reader_next(r, out);
    if (rc != 0) return rc;   /* 1 = value, <0 = protocol error */

    uint8_t tmp[512];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;                       /* read error */
        }
        if (n == 0) return 0;                /* EOF -- pipe's writers all closed */
        if (sval_reader_feed(r, tmp, (size_t)n)) return -1;   /* OOM */
        rc = sval_reader_next(r, out);
        if (rc != 0) return rc;              /* 1 = value, <0 = protocol error */
        /* rc == 0: partial frame, loop and read more */
    }
}

/* ==========================================================================
 * Human rendering
 * ========================================================================== */

/* Render a filesize as a human unit. Binary-ish (1024) since it's disk/memory
 * sizes; picks the largest unit that keeps the number >= 1. */
static size_t format_filesize(int64_t bytes, char *buf, size_t cap) {
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) return (size_t)snprintf(buf, cap, "%lld B", (long long)bytes);
    return (size_t)snprintf(buf, cap, "%.1f %s", v, units[u]);
}

/* Render a DATE (epoch seconds) as a compact readable stamp. No timezone
 * handling and no libc time (freestanding-ish target) -- a plain UTC
 * decomposition. Good enough to READ; the shell isn't a calendar app. */
static size_t format_date(int64_t epoch, char *buf, size_t cap) {
    /* days since 1970-01-01, civil-from-days (Howard Hinnant's algorithm). */
    int64_t days = epoch / 86400;
    int64_t secs = epoch % 86400;
    if (secs < 0) { secs += 86400; days -= 1; }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    int64_t mp = (5*doy + 2) / 153;
    int64_t d = doy - (153*mp + 2)/5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    int hh = (int)(secs / 3600), mm = (int)((secs % 3600) / 60), ss = (int)(secs % 60);
    return (size_t)snprintf(buf, cap, "%04lld-%02lld-%02lld %02d:%02d:%02d",
                            (long long)y, (long long)m, (long long)d, hh, mm, ss);
}

size_t sval_format_scalar(const struct value *v, char *buf, size_t cap) {
    if (cap == 0) return 0;
    switch (v->type) {
    case VAL_NULL:   buf[0] = '\0'; return 0;   /* empty cell (not snprintf(""):
                                                 * -Wformat-zero-length) */
    case VAL_INT:    return (size_t)snprintf(buf, cap, "%lld", (long long)v->u.i);
    case VAL_FLOAT:  return (size_t)snprintf(buf, cap, "%g", v->u.f);
    case VAL_BOOL:   return (size_t)snprintf(buf, cap, "%s", v->u.b ? "true" : "false");
    case VAL_FILESIZE: return format_filesize(v->u.i, buf, cap);
    case VAL_DATE:     return format_date(v->u.i, buf, cap);
    case VAL_STRING:
    case VAL_PATH: {
        size_t n = v->u.s.len < cap - 1 ? v->u.s.len : cap - 1;
        memcpy(buf, v->u.s.bytes, n);
        buf[n] = '\0';
        return n;
    }
    case VAL_LIST:   return (size_t)snprintf(buf, cap, "[list]");
    case VAL_RECORD: return (size_t)snprintf(buf, cap, "{record}");
    case VAL_TABLE:  return (size_t)snprintf(buf, cap, "[table]");
    case VAL_ERROR:  return (size_t)snprintf(buf, cap, "[error: %s]", v->u.s.bytes);
    default:         return (size_t)snprintf(buf, cap, "[unknown]");
    }
    return 0;
}

static int g_colour = 0;
void sval_set_colour(int on) { g_colour = on; }

/* SGR for a table cell, decided from the ROW -- a "name" whose row says dir
 * paints blue+bold, a name that looks executable paints green. This is the
 * presenter styling from data, the way `ls --color` does; the values in the
 * pipeline stay plain so `where`/`save`/pipes are never polluted. */
static const char *cell_colour(const struct record *row, const char *col,
                               const char *cell) {
    if (!g_colour || strcmp(col, "name") != 0) return NULL;
    const struct value *tv = record_field((struct record *)row, "type");
    if (tv && (tv->type == VAL_STRING || tv->type == VAL_PATH) &&
        tv->u.s.len == 3 && memcmp(tv->u.s.bytes, "dir", 3) == 0)
        return "\x1b[1;34m";                    /* directory: bold blue */
    size_t n = strlen(cell);
    if ((n > 4 && !strcmp(cell + n - 4, ".elf")) ||
        (n > 5 && !strcmp(cell + n - 5, ".embx")))
        return "\x1b[1;32m";                    /* executable: bold green */
    return NULL;
}

/* Where text goes: an fd (the screen, a file), or memory (a program's stdin
 * being prepared, sval_render). */
struct text_out { int fd; struct wire_buf *buf; };

static int out_write(struct text_out *o, const void *p, size_t n) {
    if (!o->buf) return write_all(o->fd, (const uint8_t *)p, n);
    if (o->buf->len + n > o->buf->cap) {
        size_t nc = o->buf->cap ? o->buf->cap : 256;
        while (nc < o->buf->len + n) nc *= 2;
        uint8_t *nd = (uint8_t *)realloc(o->buf->data, nc);
        if (!nd) return -1;
        o->buf->data = nd; o->buf->cap = nc;
    }
    memcpy(o->buf->data + o->buf->len, p, n);
    o->buf->len += n;
    return 0;
}

static int puts_fd(struct text_out *o, const char *s) {
    return out_write(o, s, strlen(s));
}

/* Table renderer: two passes over the rows -- pass 1 measures the max text
 * width of each column (across all rows + the header name), pass 2 prints an
 * aligned grid. Columns are the union of the first row's field names (rows are
 * assumed homogeneous for rendering; a missing field prints blank). */
static int print_table(const struct table *t, struct text_out *fd) {
    if (t->count == 0) { return puts_fd(fd, "(empty table)\n"); }

    struct record *hdr = &t->rows[0];
    size_t ncols = hdr->count;
    if (ncols == 0) { return puts_fd(fd, "(no columns)\n"); }

    /* column widths (capped so one pathological cell can't blow the layout) */
    #define CELL_MAX 40
    size_t *w = (size_t *)calloc(ncols, sizeof(size_t));
    if (!w) return -1;
    for (size_t c = 0; c < ncols; c++) w[c] = strlen(hdr->names[c]);

    char cell[CELL_MAX + 1];
    for (size_t r = 0; r < t->count; r++) {
        struct record *row = &t->rows[r];
        for (size_t c = 0; c < ncols; c++) {
            const struct value *cv = record_field(row, hdr->names[c]);
            size_t len = cv ? sval_format_scalar(cv, cell, sizeof cell) : 0;
            if (len > CELL_MAX) len = CELL_MAX;
            if (len > w[c]) w[c] = len;
        }
    }

    /* header */
    for (size_t c = 0; c < ncols; c++) {
        char pad[CELL_MAX + 2];
        int k = snprintf(pad, sizeof pad, "%-*s%s", (int)w[c], hdr->names[c],
                         c + 1 < ncols ? "  " : "");
        (void)k;
        if (puts_fd(fd, pad)) { free(w); return -1; }
    }
    puts_fd(fd, "\n");
    /* underline */
    for (size_t c = 0; c < ncols; c++) {
        for (size_t i = 0; i < w[c]; i++) puts_fd(fd, "-");
        if (c + 1 < ncols) puts_fd(fd, "  ");
    }
    puts_fd(fd, "\n");
    /* rows */
    for (size_t r = 0; r < t->count; r++) {
        struct record *row = &t->rows[r];
        for (size_t c = 0; c < ncols; c++) {
            const struct value *cv = record_field(row, hdr->names[c]);
            if (cv) sval_format_scalar(cv, cell, sizeof cell); else cell[0] = '\0';
            /* colour goes AROUND the text, padding goes after the reset --
             * escape bytes must never count toward the column width */
            const char *col = cell_colour(row, hdr->names[c], cell);
            if (col) { puts_fd(fd, col); }
            if (puts_fd(fd, cell)) { free(w); return -1; }
            if (col) { puts_fd(fd, "\x1b[0m"); }
            size_t len = strlen(cell);
            for (size_t i = len; i < w[c]; i++) puts_fd(fd, " ");
            if (c + 1 < ncols) puts_fd(fd, "  ");
        }
        puts_fd(fd, "\n");
    }
    free(w);
    return 0;
}

/* Text, as a person reads it. A string is written WHOLE -- a program's output
 * or a file's contents can be any length, and a line cut at a cell width is a
 * line that lies -- with a newline added only if it lacks one. */
static int print_text(struct text_out *o, const struct value *v) {
    if (v->u.s.len && out_write(o, v->u.s.bytes, v->u.s.len)) return -1;
    if (v->u.s.len && v->u.s.bytes[v->u.s.len - 1] == '\n') return 0;
    return puts_fd(o, "\n");
}

static int print_value(const struct value *v, struct text_out *fd) {
    char cell[128];
    switch (v->type) {
    case VAL_LIST:
        for (size_t i = 0; i < v->u.list.count; i++) {
            if (print_value(&v->u.list.items[i], fd)) return -1;   /* one per line;
                                                                    * scalars add \n below */
        }
        return 0;
    case VAL_RECORD: {
        struct record *r = v->u.record;
        for (size_t i = 0; i < r->count; i++) {
            if (puts_fd(fd, r->names[i]) || puts_fd(fd, ": ")) return -1;
            const struct value *f = &r->values[i];
            if (f->type == VAL_STRING || f->type == VAL_PATH) {
                if (print_text(fd, f)) return -1;
            } else {
                sval_format_scalar(f, cell, sizeof cell);
                if (puts_fd(fd, cell) || puts_fd(fd, "\n")) return -1;
            }
        }
        return 0;
    }
    case VAL_TABLE:
        return print_table(v->u.table, fd);
    case VAL_STRING:
    case VAL_PATH:
        return print_text(fd, v);
    default:
        sval_format_scalar(v, cell, sizeof cell);
        if (puts_fd(fd, cell)) return -1;
        return puts_fd(fd, "\n");
    }
}

int sval_print(const struct value *v, int fd) {
    struct text_out o = { fd, NULL };
    return print_value(v, &o);
}

int sval_render(const struct value *v, struct wire_buf *out) {
    struct text_out o = { -1, out };
    int colour = g_colour;
    g_colour = 0;                              /* text for a program, never a display */
    int rc = print_value(v, &o);
    g_colour = colour;
    return rc;
}