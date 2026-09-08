/* user/lib/appauth.c -- reading an application's declared authority.
 *
 * See appauth.h for what the two sidecars mean. This file is deliberately
 * dependency-light: fixed buffers, no malloc, only the raw SDK, so a launcher
 * that is not a full libc program can still ask the question.
 */
#include "appauth.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MANIFEST_MAX 4096      /* a declaration of authority that needs more
                                * than 4KB is not a declaration anyone reads */

/* Read a sidecar beside `elf_path`: "<...>.elf" -> "<...>.<ext>".
 * Returns the byte count, or -1 if there is no such file (which is not an
 * error -- it is the common case, and it means "inherit"). */
static int read_sidecar(const char *elf_path, const char *ext,
                        char *buf, size_t cap) {
    char path[256];
    size_t L = strlen(elf_path);
    size_t el = strlen(ext);
    if (L < 5 || L - 4 + el + 2 > sizeof path) return -1;
    if (strcmp(elf_path + L - 4, ".elf") != 0) return -1;   /* only "<...>.elf" */
    memcpy(path, elf_path, L - 4);
    path[L - 4] = '.';
    memcpy(path + L - 3, ext, el);
    path[L - 3 + el] = 0;

    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t n = 0;
    for (;;) {
        int64_t got = embk_read(fd, buf + n, cap - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
        if (n + 1 >= cap) break;
    }
    embk_close(fd);
    buf[n] = 0;
    return (int)n;
}

/* --- a tiny line/word scanner shared by both parsers ---------------------- */

/* Advance past blanks and comment lines; return 0 at end of input. */
static int next_token(const char *b, int len, int *i, int *ts, int *tl) {
    for (;;) {
        while (*i < len && (b[*i]==' '||b[*i]=='\t'||b[*i]=='\r'||b[*i]=='\n')) (*i)++;
        if (*i >= len) return 0;
        if (b[*i] == '#') { while (*i < len && b[*i] != '\n') (*i)++; continue; }
        break;
    }
    *ts = *i;
    while (*i < len && b[*i]!=' ' && b[*i]!='\t' && b[*i]!='\n' && b[*i]!='\r') (*i)++;
    *tl = *i - *ts;
    return 1;
}

/* Skip the rest of the current line -- used to discard a malformed entry
 * WITHOUT discarding the entries after it. A manifest with one bad line should
 * lose that line, not silently lose everything below it. */
static void skip_line(const char *b, int len, int *i) {
    while (*i < len && b[*i] != '\n') (*i)++;
}

static int tok_is(const char *b, int ts, int tl, const char *w) {
    return (int)strlen(w) == tl && strncmp(b + ts, w, (size_t)tl) == 0;
}

/* --- namespace ----------------------------------------------------------- */

int appauth_load_ns(const char *elf_path, struct embk_spawn_file_action *acts,
                    int max, char *desc, size_t desc_cap) {
    char buf[MANIFEST_MAX];
    int len = read_sidecar(elf_path, "ns", buf, sizeof buf);
    if (len <= 0) return 0;

    int n = 0, i = 0, ts, tl;
    size_t dn = 0;
    if (desc_cap) desc[0] = 0;

    while (n < max && next_token(buf, len, &i, &ts, &tl)) {
        int mode;
        if      (tok_is(buf, ts, tl, "ro")) mode = EMBK_NS_RO;
        else if (tok_is(buf, ts, tl, "rw")) mode = EMBK_NS_RW;
        else { skip_line(buf, len, &i); continue; }

        /* the prefix must be on the SAME line as its mode */
        while (i < len && (buf[i]==' '||buf[i]=='\t')) i++;
        int ps = i;
        while (i < len && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        int plen = i - ps;
        /* A prefix is absolute -- or begins with $HOME, which expands to one
         * just below. The check used to be `buf[ps] != '/'` alone, which threw
         * every $HOME line away BEFORE the expansion could run: the grant
         * simply vanished from the manifest, with no error anywhere, and the
         * app started with one binding fewer than it asked for. */
        if (plen <= 0 || plen > 255) { skip_line(buf, len, &i); continue; }
        if (buf[ps] != '/' && !(plen >= 5 && !strncmp(buf + ps, "$HOME", 5))) {
            skip_line(buf, len, &i); continue;
        }

        char prefix[256];
        memcpy(prefix, buf + ps, (size_t)plen); prefix[plen] = 0;

        /* `$HOME/...` expands to the SESSION USER's home.
         *
         * An app that keeps state for a person cannot name where it goes: the
         * path contains a user name it must not hard-code, and on a machine
         * with two users there are two answers. Without this the only
         * writable place an app could DECLARE was its own install directory --
         * which the session deliberately makes read-only, because an
         * application rewriting its own binary is exactly what a package
         * manager exists to prevent. So the declaration stays static and the
         * launcher supplies the user. */
        if (!strncmp(prefix, "$HOME", 5) && (prefix[5] == '/' || prefix[5] == 0)) {
            const char *h = getenv("HOME");
            if (!h || !h[0]) { skip_line(buf, len, &i); continue; }
            char expanded[256];
            snprintf(expanded, sizeof expanded, "%s%s", h, prefix + 5);
            snprintf(prefix, sizeof prefix, "%s", expanded);
            plen = (int)strlen(prefix);
        }
        embk_action_ns_bind(&acts[n], prefix, mode);

        if (desc_cap && dn + (size_t)plen + 6 < desc_cap) {
            if (dn) { desc[dn++] = ','; desc[dn++] = ' '; }
            desc[dn++] = 'r'; desc[dn++] = (mode == EMBK_NS_RO) ? 'o' : 'w'; desc[dn++] = ' ';
            memcpy(desc + dn, prefix, (size_t)plen); dn += (size_t)plen; desc[dn] = 0;
        }
        n++;
    }
    return n;
}

/* --- capabilities -------------------------------------------------------- */

/* The names an app writes, in id order. Index == EMBK_CAP_* id. Index 0 is
 * unused: cap id 0 is not a capability (EMBK_CAP_ALL masks it out). */
static const char *const CAP_NAMES[] = {
    0, "filesystem", "network", "gpu", "audio", "camera",
    "usb", "serial", "rawdisk", "kernel_ext", "debug",
};
#define CAP_NAMES_N ((int)(sizeof CAP_NAMES / sizeof CAP_NAMES[0]))

const char *appauth_cap_name(int cap_id) {
    if (cap_id > 0 && cap_id < CAP_NAMES_N && CAP_NAMES[cap_id]) return CAP_NAMES[cap_id];
    return "?";
}

int appauth_load_caps(const char *elf_path, unsigned *out_mask,
                      char *desc, size_t desc_cap) {
    char buf[MANIFEST_MAX];
    int len = read_sidecar(elf_path, "caps", buf, sizeof buf);
    if (len < 0) return 0;                    /* no manifest -> inherit */

    unsigned mask = 0;
    int i = 0, ts, tl, said = 0;
    size_t dn = 0;
    if (desc_cap) desc[0] = 0;

    while (next_token(buf, len, &i, &ts, &tl)) {
        if (tok_is(buf, ts, tl, "none")) { said = 1; continue; }   /* explicit empty set */
        int id = 0;
        for (int c = 1; c < CAP_NAMES_N; c++)
            if (CAP_NAMES[c] && tok_is(buf, ts, tl, CAP_NAMES[c])) { id = c; break; }
        if (!id) continue;                    /* unknown name: ignore, never widen */
        said = 1;
        mask |= EMBK_CAP_BIT(id);
        if (desc_cap) {
            const char *nm = CAP_NAMES[id];
            size_t nl = strlen(nm);
            if (dn + nl + 3 < desc_cap) {
                if (dn) { desc[dn++] = ','; desc[dn++] = ' '; }
                memcpy(desc + dn, nm, nl); dn += nl; desc[dn] = 0;
            }
        }
    }
    /* A file that SAYS nothing is not a file that says "nothing". A .caps with
     * only comments in it -- someone documenting why an app is unrestricted --
     * would otherwise declare the empty set and strip the app of everything it
     * had. Saying "I need no capabilities" requires the word `none`; anything
     * else that names nothing is treated as no manifest at all. */
    if (!said) return 0;

    if (desc_cap && !dn && desc_cap > 5) memcpy(desc, "none", 5);
    *out_mask = mask;
    return 1;
}
