/* ==========================================================================
 * p10k_zsh.c -- the bits of zsh the prompt engine speaks: prompt escapes
 * (%F{39}, %B, %{...%}, %~) turned into SGR and measured; the parameter
 * expansions Powerlevel10k's *_EXPANSION parameters are written in; and the
 * git formatter every wizard-written configuration defines.
 * ========================================================================== */
#include "prompt/p10k_int.h"
#include "prompt/colors.h"
#include "edit/wcwidth.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- prompt escapes -------------------------------------------------------- */

/* A colour as zsh takes it in %F{...} -- a number, #rrggbb, a name -- as SGR
 * parameters ("38;5;39", "31"); nothing is 0, which is what %F{} draws. "" for
 * a name zsh does not know: no change. */
static void color_param(const char *c, size_t n, bool bg, char out[24]) {
    char v[24];
    if (n >= sizeof v) n = sizeof v - 1;
    memcpy(v, c, n);
    v[n] = 0;
    out[0] = 0;
    if (v[0] == '#' && n == 7) {
        unsigned long rgb = strtoul(v + 1, NULL, 16);
        snprintf(out, 24, "%d;2;%lu;%lu;%lu", bg ? 48 : 38, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
        return;
    }
    int idx;
    if (!v[0]) idx = 0;
    else if (isdigit((unsigned char)v[0])) idx = atoi(v);
    else idx = p10k_color_named(v);
    if (idx < 0) return;
    if (idx < 8) snprintf(out, 24, "%d", (bg ? 40 : 30) + idx);
    else snprintf(out, 24, "%d;5;%d", bg ? 48 : 38, idx & 255);
}

/* What the text is drawn with: the escapes change `want`, and one SGR brings
 * the terminal (`have`) there just before something is shown -- a prompt
 * string says %b%k%f at every segment, the terminal hears it once. */
struct zout { struct sb *out; struct zpen want, have; };

static void flush(struct zout *z) {
    struct zpen *w = &z->want, *h = &z->have;
    char sgr[96] = "";
    size_t n = 0;
#define ADD(s) n += (size_t)snprintf(sgr + n, sizeof sgr - n, "%s%s", n ? ";" : "", s)
    if (w->bold != h->bold) ADD(w->bold ? "1" : "22");
    if (w->ul != h->ul) ADD(w->ul ? "4" : "24");
    if (w->so != h->so) ADD(w->so ? "7" : "27");
    if (strcmp(w->fg, h->fg)) ADD(w->fg[0] ? w->fg : "39");
    if (strcmp(w->bg, h->bg)) ADD(w->bg[0] ? w->bg : "49");
#undef ADD
    if (n) sb_fmt(z->out, "\033[%sm", sgr);
    *h = *w;
}
static void show(struct zout *z, const char *s, size_t n) { if (n) { flush(z); sb_add(z->out, s, n); } }
static void show_str(struct zout *z, const char *s) { show(z, s, strlen(s)); }

/* %~: the directory, home as ~. */
static void tilde_dir(const struct p10k_state *st, struct sb *out) {
    const char *pwd = st->pwd ? st->pwd : "/", *home = st->home;
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(pwd, home, hl) && (pwd[hl] == '/' || !pwd[hl])) { sb_chr(out, '~'); sb_str(out, pwd + hl); }
    else sb_str(out, pwd);
}

/* %D{fmt}: what the prompt's clock is written with. */
static void strftime_lite(const char *fmt, size_t n, const struct p10k_state *st, struct sb *out) {
    static const char *const day[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (size_t i = 0; i < n; i++) {
        if (fmt[i] != '%' || i + 1 >= n) { sb_chr(out, fmt[i]); continue; }
        char c = fmt[++i];
        int h12 = st->hour % 12 ? st->hour % 12 : 12;
        switch (c) {
        case 'H': sb_fmt(out, "%02d", st->hour); break;
        case 'k': sb_fmt(out, "%2d", st->hour); break;
        case 'I': sb_fmt(out, "%02d", h12); break;
        case 'l': sb_fmt(out, "%2d", h12); break;
        case 'M': sb_fmt(out, "%02d", st->minute); break;
        case 'S': sb_fmt(out, "%02d", st->second); break;
        case 'p': sb_str(out, st->hour < 12 ? "AM" : "PM"); break;
        case 'P': sb_str(out, st->hour < 12 ? "am" : "pm"); break;
        case 'y': sb_fmt(out, "%02d", st->year % 100); break;
        case 'Y': sb_fmt(out, "%d", st->year); break;
        case 'm': sb_fmt(out, "%02d", st->month); break;
        case 'd': sb_fmt(out, "%02d", st->mday); break;
        case 'e': sb_fmt(out, "%2d", st->mday); break;
        case 'a': sb_str(out, day[st->wday % 7]); break;
        case 'b': sb_str(out, mon[(st->month + 11) % 12]); break;
        case 'T': sb_fmt(out, "%02d:%02d:%02d", st->hour, st->minute, st->second); break;
        case 'R': sb_fmt(out, "%02d:%02d", st->hour, st->minute); break;
        case 'D': sb_fmt(out, "%02d/%02d/%02d", st->month, st->mday, st->year % 100); break;
        case '%': sb_chr(out, '%'); break;
        default: sb_chr(out, '%'); sb_chr(out, c); break;
        }
    }
}

/* The end of a %(x.true.false) part: the separator at depth 0. */
static const char *cond_part_end(const char *p, char sep) {
    int depth = 0;
    for (; *p; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '(') depth++;
            p++;
            continue;
        }
        if (*p == ')' && depth) { depth--; continue; }
        if ((*p == sep || *p == ')') && !depth) return p;
    }
    return p;
}

static void zexpand_n(const char *z, size_t len, const struct p10k_state *st, struct zout *o);

void p9k_zexpand(const char *z, const struct p10k_state *st, struct sb *out) {
    struct zout o = { out, { "", "", false, false, false }, { "", "", false, false, false } };
    zexpand_n(z, strlen(z), st, &o);
    flush(&o);                                       /* how the string leaves the terminal */
}

void p9k_zexpand_on(const char *z, const struct p10k_state *st, struct sb *out, struct zpen *pen) {
    struct zout o = { out, *pen, *pen };
    zexpand_n(z, strlen(z), st, &o);
    flush(&o);
    *pen = o.have;
}

static void zexpand_n(const char *z, size_t len, const struct p10k_state *st, struct zout *o) {
    const char *end = z + len;
    struct sb tmp = { 0 };
    for (const char *p = z; p < end;) {
        if (*p != '%') {
            const char *q = p;
            while (q < end && *q != '%') q++;
            show(o, p, (size_t)(q - p));
            p = q;
            continue;
        }
        p++;
        if (p >= end) { show_str(o, "%"); break; }
        /* %238F, %1(j.x.y): a number before the escape */
        int num = -1;
        if (isdigit((unsigned char)*p) || (*p == '-' && p + 1 < end && isdigit((unsigned char)p[1]))) {
            num = (int)strtol(p, (char **)&p, 10);
            if (p >= end) break;
        }
        char c = *p++;
        sb_reset(&tmp);
        switch (c) {
        case '%': show_str(o, "%"); break;
        case '{': {                                  /* raw until %}, after the pen; %% is still % */
            const char *q = p;
            while (q + 1 < end && !(q[0] == '%' && q[1] == '}')) { if (q[0] == '%' && q[1] == '%') q++; q++; }
            flush(o);
            for (const char *r = p; r < q; r++) {
                if (r[0] == '%' && r + 1 < q && r[1] == '%') r++;
                sb_chr(o->out, *r);
            }
            p = q + 1 < end ? q + 2 : end;
            break;
        }
        case 'F': case 'K': {
            char *dst = c == 'K' ? o->want.bg : o->want.fg, v[24];
            if (p < end && *p == '{') {
                const char *q = memchr(p, '}', (size_t)(end - p));
                if (!q) q = end;
                color_param(p + 1, (size_t)(q - p - 1), c == 'K', v);
                p = q < end ? q + 1 : end;
            } else if (num >= 0) {
                char n[16];
                snprintf(n, sizeof n, "%d", num);
                color_param(n, strlen(n), c == 'K', v);
            } else v[0] = 0, dst[0] = 0;
            if (v[0]) memcpy(dst, v, sizeof v);
            break;
        }
        case 'f': o->want.fg[0] = 0; break;
        case 'k': o->want.bg[0] = 0; break;
        case 'B': o->want.bold = true; break;
        case 'b': o->want.bold = false; break;
        case 'U': o->want.ul = true; break;
        case 'u': o->want.ul = false; break;
        case 'S': o->want.so = true; break;
        case 's': o->want.so = false; break;
        case 'n': show_str(o, st->user ? st->user : ""); break;
        case 'm': { const char *h = st->host ? st->host : ""; show(o, h, strcspn(h, ".")); break; }
        case 'M': show_str(o, st->host ? st->host : ""); break;
        case '~': tilde_dir(st, &tmp); show_str(o, sb_text(&tmp)); break;
        case '/': case 'd': show_str(o, st->pwd ? st->pwd : "/"); break;
        case '#': show_str(o, st->root ? "#" : "%"); break;
        case 'j': sb_fmt(&tmp, "%d", st->jobs); show_str(o, sb_text(&tmp)); break;
        case '?': sb_fmt(&tmp, "%d", st->status < 0 ? 0 : st->status); show_str(o, sb_text(&tmp)); break;
        case 'D':
            if (p < end && *p == '{') {
                const char *q = memchr(p, '}', (size_t)(end - p));
                if (!q) q = end;
                strftime_lite(p + 1, (size_t)(q - p - 1), st, &tmp);
                show_str(o, sb_text(&tmp));
                p = q < end ? q + 1 : end;
            }
            break;
        case '(': {                                  /* %(x.true.false) */
            if (p >= end) break;
            char what = *p++;
            if (p >= end) break;
            char sep = *p++;
            int n = num < 0 ? 0 : num;
            bool yes;
            switch (what) {
            case '?': yes = (st->status < 0 ? 0 : st->status) == n; break;
            case '#': yes = st->root; break;
            case 'j': yes = st->jobs >= (num < 0 ? 1 : n); break;
            default: yes = false; break;
            }
            const char *t = p, *te = cond_part_end(t, sep);
            const char *f = te < end && *te == sep ? te + 1 : te, *fe = cond_part_end(f, ')');
            if (yes) zexpand_n(t, (size_t)(te - t), st, o);
            else if (f < fe) zexpand_n(f, (size_t)(fe - f), st, o);
            p = fe < end ? fe + 1 : end;
            break;
        }
        case 'G': case 'E': break;
        default: { char e[2] = { '%', c }; show(o, e, 2); break; }
        }
    }
    sb_free(&tmp);
}

int p10k_width(const char *zprompt, const struct p10k_state *st) {
    struct sb s = { 0 };
    p9k_zexpand(zprompt, st, &s);
    const char *t = sb_text(&s);
    int w = 0;
    size_t n = strlen(t);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)t[i];
        if (c == 0x1b && i + 1 < n && (t[i + 1] == '[' || t[i + 1] == ']')) {
            bool osc = t[i + 1] == ']';
            i += 2;
            while (i < n) {
                unsigned char d = (unsigned char)t[i++];
                if (osc ? (d == 7 || (d == 0x1b && i < n && t[i] == '\\' && ++i)) : (d >= 0x40 && d <= 0x7e)) break;
            }
            continue;
        }
        uint32_t cp;
        int len = utf8_decode(t + i, (int)(n - i), &cp);
        i += (size_t)(len > 0 ? len : 1);
        int cw = wcwidth_cp(cp);
        if (cw > 0) w += cw;
    }
    sb_free(&s);
    return w;
}

/* --- parameter expansion: *_EXPANSION ---------------------------------------- */

/* Does `s` match the zsh pattern `pat` in full: * ? [...] \x, and (a|b)
 * groups (an empty alternative allowed, as in ~/work(|/x)). */
static bool glob_at(const char *pat, size_t pn, const char *s, size_t sn, bool hidden);
bool p9k_glob(const char *pat, size_t pn, const char *s, size_t sn) { return glob_at(pat, pn, s, sn, false); }
bool p9k_glob_name(const char *pat, const char *name) {
    return glob_at(pat, strlen(pat), name, strlen(name), name[0] == '.');
}

/* `hidden`: s starts with a dot that no wildcard may stand for. */
static bool glob_at(const char *pat, size_t pn, const char *s, size_t sn, bool hidden) {
    size_t p = 0, i = 0;
    while (p < pn) {
        char c = pat[p];
        if (c == '*') {
            while (p < pn && pat[p] == '*') p++;
            if (hidden && i == 0) return glob_at(pat + p, pn - p, s, sn, true);  /* the dot is not the star's */
            if (p == pn) return true;
            for (size_t k = i; k <= sn; k++) if (glob_at(pat + p, pn - p, s + k, sn - k, false)) return true;
            return false;
        }
        if (c == '(') {                               /* each alternative, then the rest */
            size_t e = p + 1, from = p + 1;
            int depth = 1;
            size_t alts[33], nalt = 0;
            alts[nalt++] = from;
            for (; e < pn; e++) {
                if (pat[e] == '\\' && e + 1 < pn) { e++; continue; }
                if (pat[e] == '(') depth++;
                else if (pat[e] == ')' && !--depth) break;
                else if (pat[e] == '|' && depth == 1 && nalt < 32) alts[nalt++] = e + 1;
            }
            if (e >= pn) return false;
            alts[nalt] = e + 1;
            for (size_t a = 0; a < nalt; a++) {
                char buf[512];
                size_t al = alts[a + 1] - 1 - alts[a], rest = pn - e - 1;
                if (al + rest > sizeof buf) return false;
                memcpy(buf, pat + alts[a], al);
                memcpy(buf + al, pat + e + 1, rest);
                if (glob_at(buf, al + rest, s + i, sn - i, hidden && i == 0)) return true;
            }
            return false;
        }
        if (i >= sn) return false;
        if (hidden && i == 0 && (c == '?' || c == '[')) return false;
        if (c == '?') { p++; i++; continue; }
        if (c == '[') {                               /* a class: ranges, ! or ^, [:alnum:] */
            size_t q = p + 1;
            bool neg = q < pn && (pat[q] == '!' || pat[q] == '^'), hit = false;
            if (neg) q++;
            size_t first = q;
            for (; q < pn && (pat[q] != ']' || q == first); q++) {
                if (pat[q] == '[' && q + 1 < pn && pat[q + 1] == ':') {
                    const char *end = memchr(pat + q, ']', pn - q);
                    if (end) {
                        size_t len = (size_t)(end - (pat + q));
                        if (len == 8 && !strncmp(pat + q, "[:alnum:", 8)) hit |= isalnum((unsigned char)s[i]) != 0;
                        else if (len == 8 && !strncmp(pat + q, "[:alpha:", 8)) hit |= isalpha((unsigned char)s[i]) != 0;
                        else if (len == 8 && !strncmp(pat + q, "[:digit:", 8)) hit |= isdigit((unsigned char)s[i]) != 0;
                        q += len;
                        continue;
                    }
                }
                if (q + 2 < pn && pat[q + 1] == '-' && pat[q + 2] != ']') {
                    hit |= s[i] >= pat[q] && s[i] <= pat[q + 2];
                    q += 2;
                } else hit |= pat[q] == s[i];
            }
            if (q >= pn || hit == neg) return false;
            p = q + 1;
            i++;
            continue;
        }
        if (c == '\\' && p + 1 < pn) c = pat[++p];
        if (c != s[i]) return false;
        p++;
        i++;
    }
    return i == sn;
}

/* The end of a ${...} that starts at `p` (just past "${"): the matching } */
static const char *brace_end(const char *p) {
    int depth = 1;
    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '$' && p[1] == '{') { depth++; p++; continue; }
        if (*p == '}' && !--depth) return p;
    }
    return p;
}

struct pvars { const char *content, *icon, *gap; };
static void pexpand(const char *e, size_t n, const struct pvars *v, struct sb *out);

static const char *var_value(const char *name, size_t n, const struct pvars *v) {
    if (n == 11 && !strncmp(name, "P9K_CONTENT", 11)) return v->content;
    if (n == 21 && !strncmp(name, "P9K_VISUAL_IDENTIFIER", 21)) return v->icon;
    if (n == 7 && !strncmp(name, "P9K_GAP", 7)) return v->gap;
    return "";
}

/* ${body} with `body` the text between the braces. */
static void brace(const char *b, size_t n, const struct pvars *v, struct sb *out) {
    struct sb val = { 0 };
    size_t i = 0;
    if (n >= 2 && b[0] == '$' && b[1] == '{') {       /* ${${...}ops} */
        const char *e = brace_end(b + 2);
        pexpand(b, (size_t)(e - b + 1), v, &val);
        i = (size_t)(e - b) + 1;
    } else {
        while (i < n && (isalnum((unsigned char)b[i]) || b[i] == '_')) i++;
        sb_str(&val, var_value(b, i, v));
    }
    const char *op = b + i, *end = b + n;
    const char *s = sb_text(&val);
    size_t sl = strlen(s);
    if (op >= end) { sb_str(out, s); sb_free(&val); return; }
    if (*op == '/') {                                 /* /pat/rep or //pat/rep */
        bool all = op + 1 < end && op[1] == '/';
        const char *pat = op + (all ? 2 : 1), *pe = pat;
        while (pe < end && *pe != '/') { if (*pe == '\\' && pe + 1 < end) pe++; pe++; }
        struct sb rep = { 0 };
        if (pe < end) pexpand(pe + 1, (size_t)(end - pe - 1), v, &rep);
        size_t pl = (size_t)(pe - pat), at = 0;
        while (at < sl) {
            size_t k = pl ? sl - at : 0;            /* the longest match starting here */
            while (k && !p9k_glob(pat, pl, s + at, k)) k--;
            if (!k) { sb_chr(out, s[at++]); continue; }
            sb_str(out, sb_text(&rep));
            at += k;
            if (!all) { sb_str(out, s + at); break; }
        }
        sb_free(&rep);
    } else if (op + 1 < end && op[0] == ':' && (op[1] == '+' || op[1] == '-')) {
        bool set = sl > 0;
        if ((op[1] == '+') == set) pexpand(op + 2, (size_t)(end - op - 2), v, out);
        else if (op[1] == '-') sb_str(out, s);
    } else if (*op == '#' || *op == '%') {             /* the shortest prefix / suffix match removed */
        bool front = *op == '#';
        const char *pat = op + 1;
        size_t pl = (size_t)(end - pat), cut = 0;
        bool found = false;
        for (size_t k = 0; k <= sl && !found; k++)
            if (front ? p9k_glob(pat, pl, s, k) : p9k_glob(pat, pl, s + sl - k, k)) { cut = k; found = true; }
        if (front) sb_str(out, s + (found ? cut : 0));
        else sb_add(out, s, sl - (found ? cut : 0));
    } else sb_str(out, s);
    sb_free(&val);
}

static void pexpand(const char *e, size_t n, const struct pvars *v, struct sb *out) {
    for (size_t i = 0; i < n;) {
        if (e[i] == '\\' && i + 1 < n) { sb_chr(out, e[i + 1]); i += 2; continue; }
        if (e[i] == '$' && i + 1 < n && e[i + 1] == '{') {
            const char *end = brace_end(e + i + 2);
            brace(e + i + 2, (size_t)(end - (e + i + 2)), v, out);
            i = (size_t)(end - e) + 1;
            continue;
        }
        if (e[i] == '$' && i + 1 < n && (isalpha((unsigned char)e[i + 1]) || e[i + 1] == '_')) {
            size_t k = i + 1;
            while (k < n && (isalnum((unsigned char)e[k]) || e[k] == '_')) k++;
            sb_str(out, var_value(e + i + 1, k - i - 1, v));
            i = k;
            continue;
        }
        sb_chr(out, e[i++]);
    }
}

void p9k_expand_param(const struct p10k *cfg, const char *exp, const char *content, const char *icon,
                      const struct p10k_state *st, struct sb *out) {
    const char *f = strstr(exp, "my_git_formatter(");
    if (f) {                                          /* the wizard's formatter */
        bool loading = f[17] == '0';
        if (content && content[0]) sb_str(out, content);
        else p9k_git_formatter(cfg, st, loading, out);
        return;
    }
    struct pvars v = { content ? content : "", icon ? icon : "", "" };
    pexpand(exp, strlen(exp), &v, out);
}

void p9k_expand_gap(const char *exp, const char *gap, struct sb *out) {
    struct pvars v = { "", "", gap };
    pexpand(exp, strlen(exp), &v, out);
}

/* --- my_git_formatter -------------------------------------------------------- *
 * The function p10k configure writes into every configuration it generates,
 * here in C. Its colours are the configuration's own: the importer reads its
 * `local meta='%f'` lines into _GIT_FORMAT_* (and _GIT_FORMAT_LOADING_*). */
static const char *fmt_color(const struct p10k *cfg, bool loading, const char *which, const char *def) {
    char name[64];
    snprintf(name, sizeof name, "_GIT_FORMAT_%s%s", loading ? "LOADING_" : "", which);
    const char *v = p10k_get(cfg, name);
    return v ? v : def;
}
static void cut_long(const char *s, char *out, size_t cap) {
    /* more than 32 characters: the first 12, …, the last 12 */
    size_t chars = 0, n = strlen(s);
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xc0) != 0x80) chars++;
    if (chars <= 32) { snprintf(out, cap, "%s", s); return; }
    size_t a = 0, k = 0;
    while (a < n && k < 12) { a++; while (a < n && ((unsigned char)s[a] & 0xc0) == 0x80) a++; k++; }
    size_t b = n; k = 0;
    while (b > 0 && k < 12) { b--; while (b > 0 && ((unsigned char)s[b] & 0xc0) == 0x80) b--; k++; }
    snprintf(out, cap, "%.*s\xe2\x80\xa6%s", (int)a, s, s + b);
}
static void escape_pct(const char *s, struct sb *out) {
    for (; *s; s++) { if (*s == '%') sb_chr(out, '%'); sb_chr(out, *s); }
}
static bool wip(const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + 3 <= n; i++) {
        if (strncmp(s + i, "wip", 3) && strncmp(s + i, "WIP", 3)) continue;
        bool before = !i || !isalnum((unsigned char)s[i - 1]);
        bool after = i + 3 == n || !isalnum((unsigned char)s[i + 3]);
        if (before && after) return true;
    }
    return false;
}

void p9k_git_formatter(const struct p10k *cfg, const struct p10k_state *st, bool loading, struct sb *res) {
    const struct p10k_git *g = &st->git;
    const char *meta = fmt_color(cfg, loading, "META", loading ? "%244F" : "%f");
    const char *clean = fmt_color(cfg, loading, "CLEAN", loading ? "%244F" : "%76F");
    const char *modified = fmt_color(cfg, loading, "MODIFIED", loading ? "%244F" : "%178F");
    const char *untracked = fmt_color(cfg, loading, "UNTRACKED", loading ? "%244F" : "%39F");
    const char *conflicted = fmt_color(cfg, loading, "CONFLICTED", loading ? "%244F" : "%196F");
    char buf[160];
    if (g->branch[0]) {
        cut_long(g->branch, buf, sizeof buf);
        sb_str(res, clean);
        const char *icon = p10k_get(cfg, "VCS_BRANCH_ICON");
        if (icon) p9k_decode_escapes(icon, res);
        escape_pct(buf, res);
    }
    if (g->tag[0] && !g->branch[0]) {
        cut_long(g->tag, buf, sizeof buf);
        sb_str(res, meta); sb_chr(res, '#'); sb_str(res, clean);
        escape_pct(buf, res);
    }
    if (!g->branch[0] && !g->tag[0]) {
        sb_str(res, meta); sb_chr(res, '@'); sb_str(res, clean);
        sb_add(res, g->commit, strlen(g->commit) < 8 ? strlen(g->commit) : 8);
    }
    if (g->remote_branch[0] && strcmp(g->remote_branch, g->branch)) {
        sb_str(res, meta); sb_chr(res, ':'); sb_str(res, clean);
        escape_pct(g->remote_branch, res);
    }
    if (wip(g->summary)) { sb_chr(res, ' '); sb_str(res, modified); sb_str(res, "wip"); }
    if (g->ahead || g->behind) {
        if (g->behind) sb_fmt(res, " %s\xe2\x87\xa3%d", clean, g->behind);
        if (g->ahead && !g->behind) sb_chr(res, ' ');
        if (g->ahead) sb_fmt(res, "%s\xe2\x87\xa1%d", clean, g->ahead);
    }
    if (g->push_behind) sb_fmt(res, " %s\xe2\x87\xa0%d", clean, g->push_behind);
    if (g->push_ahead && !g->push_behind) sb_chr(res, ' ');
    if (g->push_ahead) sb_fmt(res, "%s\xe2\x87\xa2%d", clean, g->push_ahead);
    if (g->stashes) sb_fmt(res, " %s*%d", clean, g->stashes);
    if (g->action[0]) { sb_chr(res, ' '); sb_str(res, conflicted); escape_pct(g->action, res); }
    if (g->conflicted) sb_fmt(res, " %s~%d", conflicted, g->conflicted);
    if (g->staged) sb_fmt(res, " %s+%d", modified, g->staged);
    if (g->unstaged) sb_fmt(res, " %s!%d", modified, g->unstaged);
    if (g->untracked) {
        sb_chr(res, ' '); sb_str(res, untracked);
        const char *icon = p10k_get(cfg, "VCS_UNTRACKED_ICON");
        if (icon) p9k_decode_escapes(icon, res);
        sb_fmt(res, "%d", g->untracked);
    }
    if (g->has_unstaged == -1) { sb_chr(res, ' '); sb_str(res, modified); sb_str(res, "\xe2\x94\x80"); }
}
