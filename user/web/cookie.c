/* user/web/cookie.c -- see cookie.h. */
#include <string.h>
#include <stdio.h>

#include "cookie.h"
#include "store.h"

#define COOKIE_MAX      32
#define NAME_MAX        64
#define VALUE_MAX      256
#define DOMAIN_MAX      96
#define PATH_MAX        96

static struct {
    int  used;
    char name[NAME_MAX];
    char value[VALUE_MAX];
    char domain[DOMAIN_MAX];     /* no leading dot; matched by suffix */
    char path[PATH_MAX];
    int  secure, http_only;
    int  host_only;              /* no Domain was stated: EXACT host only */
    /* WALL-CLOCK seconds since the epoch, not uptime. A jar that outlives the
     * machine being on cannot measure expiry from boot -- every persisted
     * cookie would look brand new on the next start. 0 = session cookie. */
    unsigned long long expires_at;
} g_jar[COOKIE_MAX];

static unsigned long long (*g_now_fn)(void);
void cookie_set_clock(unsigned long long (*fn)(void)) { g_now_fn = fn; }

/* Seconds since the epoch, or 0 when nobody has said. 0 reads as "the clock is
 * not set", and every expiry rule below treats that as no opinion. */
static unsigned long long now_unix(void) { return g_now_fn ? g_now_fn() : 0; }

void cookie_reset(void) { memset(g_jar, 0, sizeof g_jar); }

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (ci(*a) != ci(*b)) return 0;
    return !*a && !*b;
}

static int ieq_n(const char *a, size_t an, const char *b) {
    size_t i = 0;
    for (; i < an && b[i]; i++) if (ci(a[i]) != ci(b[i])) return 0;
    return i == an && !b[i];
}

static void trim(const char **s, size_t *n) {
    while (*n && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' ||
                  (*s)[*n - 1] == '\r')) (*n)--;
}

/* Does `host` belong to `domain`? Either exactly, or as a subdomain -- and the
 * suffix must fall on a dot, or "evil-example.com" would match "example.com".
 * That check is the whole of cookie domain security. */
static int domain_matches(const char *host, const char *domain) {
    if (!domain[0]) return 1;
    size_t hl = strlen(host), dl = strlen(domain);
    if (dl > hl) return 0;
    for (size_t i = 0; i < dl; i++)
        if (ci(host[hl - dl + i]) != ci(domain[i])) return 0;
    return hl == dl || host[hl - dl - 1] == '.';
}

/* A cookie's Path matches a request path that is the same, or that continues
 * after a '/'. "/app" must not match "/applesauce". */
static int path_matches(const char *path, const char *cpath) {
    if (!cpath[0] || !strcmp(cpath, "/")) return 1;
    size_t cl = strlen(cpath);
    if (strncmp(path, cpath, cl)) return 0;
    return path[cl] == 0 || path[cl] == '/' || cpath[cl - 1] == '/';
}

/* An HTTP date: "Wed, 09 Jun 2027 10:18:14 GMT" (RFC 1123, the only spelling
 * that matters in practice). See the header. */
unsigned long long cookie_parse_date(const char *s, size_t n) {
    static const char *const MON[12] = { "jan","feb","mar","apr","may","jun",
                                         "jul","aug","sep","oct","nov","dec" };
    size_t i = 0;
    while (i < n && s[i] != ' ') i++;      /* skip the day name and its comma */
    while (i < n && s[i] == ' ') i++;
    int day = 0, k = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && k < 2) { day = day*10 + (s[i]-'0'); i++; k++; }
    while (i < n && (s[i] == ' ' || s[i] == '-')) i++;
    int mon = -1;
    for (int m = 0; m < 12 && i + 3 <= n; m++)
        if (ci(s[i])==MON[m][0] && ci(s[i+1])==MON[m][1] && ci(s[i+2])==MON[m][2]) { mon = m; break; }
    if (mon < 0 || !day) return COOKIE_DATE_BAD;
    i += 3;
    while (i < n && (s[i] == ' ' || s[i] == '-')) i++;
    int year = 0; k = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && k < 4) { year = year*10 + (s[i]-'0'); i++; k++; }
    if (year < 100) year += (year < 70) ? 2000 : 1900;    /* two-digit years */
    if (year < 1970 || year > 3000) return COOKIE_DATE_BAD;
    while (i < n && s[i] == ' ') i++;
    int hh = 0, mm = 0, ss = 0;
    if (i + 8 <= n) {
        hh = (s[i]-'0')*10 + (s[i+1]-'0');
        mm = (s[i+3]-'0')*10 + (s[i+4]-'0');
        ss = (s[i+6]-'0')*10 + (s[i+7]-'0');
        if (hh > 23 || mm > 59 || ss > 60) { hh = mm = ss = 0; }
    }

    /* days since the epoch: whole years, then whole months, then the day.
     * Leap years by the real rule -- 2100 is not one, and a cookie set to
     * expire that far out is rare but a wrong answer here is silent. */
    long days = 0;
    for (int y = 1970; y < year; y++)
        days += ((y%4==0 && y%100!=0) || y%400==0) ? 366 : 365;
    static const int ML[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (int m = 0; m < mon; m++) {
        days += ML[m];
        if (m == 1 && ((year%4==0 && year%100!=0) || year%400==0)) days++;
    }
    days += day - 1;
    return (unsigned long long)days * 86400ull +
           (unsigned long long)hh * 3600ull + (unsigned long long)mm * 60ull + (unsigned long long)ss;
}

static int slot_for(const char *name, const char *domain, const char *path) {
    for (int i = 0; i < COOKIE_MAX; i++)
        if (g_jar[i].used && ieq(g_jar[i].name, name) &&
            ieq(g_jar[i].domain, domain) && !strcmp(g_jar[i].path, path))
            return i;                      /* same identity: REPLACE */
    for (int i = 0; i < COOKIE_MAX; i++) if (!g_jar[i].used) return i;
    return -1;
}

int cookie_set(const char *host, const char *value) {
    if (!host || !value) return -1;
    const char *p = value;
    size_t n = strlen(value);
    trim(&p, &n);

    /* name=value, up to the first ';' */
    size_t seg = 0;
    while (seg < n && p[seg] != ';') seg++;
    size_t eq = 0;
    while (eq < seg && p[eq] != '=') eq++;
    if (eq >= seg) return -1;                  /* no '=': not a cookie */
    const char *np = p; size_t nn = eq;
    const char *vp = p + eq + 1; size_t vn = seg - eq - 1;
    trim(&np, &nn); trim(&vp, &vn);
    if (!nn || nn >= NAME_MAX || vn >= VALUE_MAX) return -1;

    char domain[DOMAIN_MAX] = "", path[PATH_MAX] = "/";
    int secure = 0, http_only = 0, delete_me = 0;
    unsigned long long expires = 0;

    size_t i = seg;
    while (i < n) {
        i++;                                    /* past ';' */
        size_t a = i;
        while (i < n && p[i] != ';') i++;
        const char *ap = p + a; size_t an = i - a;
        trim(&ap, &an);
        size_t ae = 0;
        while (ae < an && ap[ae] != '=') ae++;
        const char *kp = ap; size_t kn = ae;
        const char *wp = (ae < an) ? ap + ae + 1 : 0; size_t wn = (ae < an) ? an - ae - 1 : 0;
        trim(&kp, &kn); if (wp) trim(&wp, &wn);

        if (ieq_n(kp, kn, "secure"))        secure = 1;
        else if (ieq_n(kp, kn, "httponly")) http_only = 1;
        else if (ieq_n(kp, kn, "path") && wp && wn < PATH_MAX) {
            memcpy(path, wp, wn); path[wn] = 0;
        } else if (ieq_n(kp, kn, "domain") && wp && wn < DOMAIN_MAX) {
            if (wn && *wp == '.') { wp++; wn--; }     /* a leading dot is legacy */
            memcpy(domain, wp, wn); domain[wn] = 0;
            /* A server may only scope a cookie to a domain it belongs to --
             * otherwise any site could set one for any other. */
            if (!domain_matches(host, domain)) domain[0] = 0;
        } else if (ieq_n(kp, kn, "max-age") && wp) {
            long v = 0; int neg = 0; size_t k = 0;
            if (wn && wp[0] == '-') { neg = 1; k = 1; }
            for (; k < wn && wp[k] >= '0' && wp[k] <= '9'; k++) v = v * 10 + (wp[k] - '0');
            if (neg || v == 0) delete_me = 1;
            else expires = now_unix() + (unsigned long long)v;
        } else if (ieq_n(kp, kn, "expires") && wp) {
            unsigned long long t = cookie_parse_date(wp, wn);
            if (t != COOKIE_DATE_BAD) {
                unsigned long long now = now_unix();
                /* A date in the PAST is how a server deletes a cookie, and is
                 * what a logout sends. Comparing against a clock that has not
                 * been set would delete everything, so an implausible clock
                 * means the date is simply not trusted -- see below. */
                if (now > 1600000000ull) { if (t <= now) delete_me = 1; else expires = t; }
            }
        }
    }
    /* No Domain attribute means HOST-ONLY: the cookie goes back to exactly
     * this host and to no subdomain of it. Defaulting to a domain-scoped
     * cookie instead quietly widens every cookie a site sets -- a session set
     * on example.com would be handed to any subdomain, including one an
     * attacker controls. Stating a Domain is how a site opts INTO sharing. */
    int host_only = 0;
    if (!domain[0]) { snprintf(domain, sizeof domain, "%s", host); host_only = 1; }

    char nbuf[NAME_MAX];
    memcpy(nbuf, np, nn); nbuf[nn] = 0;

    int s = slot_for(nbuf, domain, path);
    if (s < 0) return -1;                       /* jar full: the tail is lost */
    if (delete_me) { memset(&g_jar[s], 0, sizeof g_jar[s]); return 0; }

    g_jar[s].used = 1;
    snprintf(g_jar[s].name, sizeof g_jar[s].name, "%s", nbuf);
    memcpy(g_jar[s].value, vp, vn); g_jar[s].value[vn] = 0;
    snprintf(g_jar[s].domain, sizeof g_jar[s].domain, "%s", domain);
    snprintf(g_jar[s].path, sizeof g_jar[s].path, "%s", path);
    g_jar[s].secure = secure;
    g_jar[s].http_only = http_only;
    g_jar[s].expires_at = expires;
    g_jar[s].host_only = host_only;
    return 0;
}

int cookie_take_headers(const char *host, const char *headers) {
    if (!host || !headers) return 0;
    int n = 0;
    for (const char *l = headers; l && *l; ) {
        const char *eol = strchr(l, '\n');
        /* Set-Cookie is the one header that must NOT be comma-joined with its
         * siblings: a cookie's Expires attribute contains a comma. So each
         * line is taken on its own. */
        if (ieq_n(l, 11, "set-cookie:")) {
            const char *v = l + 11;
            while (*v == ' ') v++;
            size_t vlen = eol ? (size_t)(eol - v) : strlen(v);
            char buf[VALUE_MAX + 256];
            if (vlen < sizeof buf) {
                memcpy(buf, v, vlen); buf[vlen] = 0;
                if (cookie_set(host, buf) == 0) n++;
            }
        }
        if (!eol) break;
        l = eol + 1;
    }
    return n;
}

static size_t build(const char *host, const char *path, int secure,
                    int for_script, char *out, size_t cap) {
    if (!out || cap < 2) return 0;
    unsigned long long now = now_unix();
    size_t o = 0;
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!g_jar[i].used) continue;
        if (g_jar[i].expires_at && now > 1600000000ull && now >= g_jar[i].expires_at) {
            memset(&g_jar[i], 0, sizeof g_jar[i]);   /* expired: drop it */
            continue;
        }
        if (for_script && g_jar[i].http_only) continue;
        if (g_jar[i].secure && !secure && !for_script) continue;
        if (g_jar[i].host_only) { if (!ieq(host, g_jar[i].domain)) continue; }
        else if (!domain_matches(host, g_jar[i].domain)) continue;
        if (!path_matches(path && path[0] ? path : "/", g_jar[i].path)) continue;
        size_t nl = strlen(g_jar[i].name), vl = strlen(g_jar[i].value);
        if (o + nl + vl + 4 >= cap) break;
        if (o) { out[o++] = ';'; out[o++] = ' '; }
        memcpy(out + o, g_jar[i].name, nl); o += nl;
        out[o++] = '=';
        memcpy(out + o, g_jar[i].value, vl); o += vl;
    }
    out[o] = 0;
    return o;
}

size_t cookie_header(const char *host, const char *path, int secure,
                     char *out, size_t cap) {
    return build(host, path, secure, 0, out, cap);
}

size_t cookie_for_script(const char *host, const char *path,
                         char *out, size_t cap) {
    return build(host, path, 1, 1, out, cap);
}

/* --- persistence ---------------------------------------------------------
 *
 * One line per cookie, tab-separated, values last. Only cookies with an
 * EXPIRY are written: a session cookie is defined to end with the session, and
 * writing it to disk would quietly redefine what the user agreed to.
 */
int cookie_save(void) {
    static char buf[8192];
    size_t n = 0;
    unsigned long long now = now_unix();
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!g_jar[i].used || !g_jar[i].expires_at) continue;
        if (now > 1600000000ull && g_jar[i].expires_at <= now) continue;   /* dead */
        int w = snprintf(buf + n, sizeof buf - n, "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%llu\n",
                         g_jar[i].domain, g_jar[i].path, g_jar[i].name, g_jar[i].value,
                         g_jar[i].secure, g_jar[i].http_only, g_jar[i].host_only,
                         (unsigned long long)g_jar[i].expires_at);
        if (w < 0 || (size_t)w >= sizeof buf - n) break;
        n += (size_t)w;
    }
    return store_put_blob("cookies.jar", buf, n);
}

int cookie_load(void) {
    static char buf[8192];
    long got = store_get_blob("cookies.jar", buf, sizeof buf - 1);
    if (got <= 0) return -1;
    buf[got] = 0;
    unsigned long long now = now_unix();
    for (char *l = buf; l && *l; ) {
        char *eol = strchr(l, '\n');
        if (eol) *eol = 0;
        char *f[8]; int nf = 0;
        for (char *p = l; nf < 8; ) {
            f[nf++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0; p = t + 1;
        }
        if (nf == 8) {
            unsigned long long exp = 0;
            for (const char *e = f[7]; *e >= '0' && *e <= '9'; e++) exp = exp * 10 + (unsigned)(*e - '0');
            /* An expiry already past is not restored. On a machine whose clock
             * was never set, `now` is 0 and nothing is dropped -- which keeps
             * a saved session rather than destroying it over a wrong clock. */
            if (!(now > 1600000000ull && exp <= now)) {
                int s2 = slot_for(f[2], f[0], f[1]);
                if (s2 >= 0) {
                    memset(&g_jar[s2], 0, sizeof g_jar[s2]);
                    g_jar[s2].used = 1;
                    snprintf(g_jar[s2].domain, sizeof g_jar[s2].domain, "%s", f[0]);
                    snprintf(g_jar[s2].path,   sizeof g_jar[s2].path,   "%s", f[1]);
                    snprintf(g_jar[s2].name,   sizeof g_jar[s2].name,   "%s", f[2]);
                    snprintf(g_jar[s2].value,  sizeof g_jar[s2].value,  "%s", f[3]);
                    g_jar[s2].secure    = f[4][0] == '1';
                    g_jar[s2].http_only = f[5][0] == '1';
                    g_jar[s2].host_only = f[6][0] == '1';
                    g_jar[s2].expires_at = exp;
                }
            }
        }
        if (!eol) break;
        l = eol + 1;
    }
    return 0;
}
