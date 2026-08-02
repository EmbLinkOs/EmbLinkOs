/* Package manifest parser -- see manifest.h. */
#include "manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EMBX §5.6 capability ids. Index = cap_id; NULL for the invalid id 0. */
static const char *const CAP_NAMES[] = {
    NULL, "filesystem", "network", "gpu", "audio", "camera",
    "usb", "serial", "rawdisk", "kernel-ext", "debug",
};
#define CAP_COUNT ((int)(sizeof CAP_NAMES / sizeof CAP_NAMES[0]))

const char *pkg_cap_name(int id) {
    return (id > 0 && id < CAP_COUNT) ? CAP_NAMES[id] : NULL;
}
int pkg_cap_id(const char *name) {
    for (int i = 1; i < CAP_COUNT; i++)
        if (CAP_NAMES[i] && !strcmp(name, CAP_NAMES[i])) return i;
    return 0;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Parse 64 hex chars -> out[32]. 0 on success. */
static int parse_build_id(const char *s, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hexv(s[2*i]), lo = hexv(s[2*i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (s[64] == 0 || s[64] == ' ' || s[64] == '\t') ? 0 : -1;
}

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
}

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

int pkg_manifest_parse(const char *text, size_t len, struct pkg_manifest *m,
                       char *err, size_t errsz) {
    memset(m, 0, sizeof *m);
    if (errsz) err[0] = 0;

    char line[512];
    size_t i = 0;
    int in_ns = 0;               /* inside the `namespace:` block */
    int have_name = 0;

    while (i < len) {
        size_t j = i; while (j < len && text[j] != '\n') j++;
        size_t ll = j - i; if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, text + i, ll); line[ll] = 0;
        i = (j < len) ? j + 1 : j;
        rstrip(line);

        int indented = (line[0] == ' ' || line[0] == '\t');
        char *p = skip_ws(line);
        if (*p == 0 || *p == '#') continue;             /* blank / comment */

        /* A namespace entry: an indented `ro|rw <prefix>` line. */
        if (in_ns && indented) {
            char mode[8], prefix[300];
            if (sscanf(p, "%7s %299s", mode, prefix) != 2)
                FAIL("bad namespace line: %s", p);
            int ro;
            if      (!strcmp(mode, "ro")) ro = 1;
            else if (!strcmp(mode, "rw")) ro = 0;
            else FAIL("namespace mode must be ro|rw, got '%s'", mode);
            if (prefix[0] != '/') FAIL("namespace prefix must be absolute: %s", prefix);
            if (m->nns >= PKG_MAX_NS) FAIL("too many namespace binds (max %d)", PKG_MAX_NS);
            m->ns[m->nns].ro = ro;
            strncpy(m->ns[m->nns].prefix, prefix, sizeof m->ns[m->nns].prefix - 1);
            m->nns++;
            continue;
        }
        in_ns = 0;

        /* key: value */
        char *colon = strchr(p, ':');
        if (!colon) FAIL("expected 'key: value', got: %s", p);
        *colon = 0;
        char *key = p, *val = skip_ws(colon + 1);
        rstrip(key);

        if (!strcmp(key, "name")) {
            strncpy(m->name, val, sizeof m->name - 1); have_name = 1;
        } else if (!strcmp(key, "version")) {
            strncpy(m->version, val, sizeof m->version - 1);
        } else if (!strcmp(key, "abi")) {
            m->abi = (uint32_t)strtoul(val, NULL, 10);
        } else if (!strcmp(key, "build_id")) {
            if (strlen(val) < 64 || parse_build_id(val, m->build_id) != 0)
                FAIL("build_id must be 64 hex chars");
            m->have_build_id = 1;
        } else if (!strcmp(key, "provides")) {
            strncpy(m->provides, val, sizeof m->provides - 1);
        } else if (!strcmp(key, "caps")) {
            char *tok = strtok(val, " ,\t");
            while (tok) {
                int id = pkg_cap_id(tok);
                if (!id) FAIL("unknown capability '%s'", tok);
                if (m->ncaps >= PKG_MAX_CAPS) FAIL("too many caps");
                m->caps[m->ncaps++] = id;
                tok = strtok(NULL, " ,\t");
            }
        } else if (!strcmp(key, "signature")) {
            if (strlen(val) < 128) FAIL("signature must be 128 hex chars (r||s)");
            for (int i = 0; i < 64; i++) {
                int hi = hexv(val[2*i]), lo = hexv(val[2*i+1]);
                if (hi < 0 || lo < 0) FAIL("signature is not hex");
                m->signature[i] = (uint8_t)((hi << 4) | lo);
            }
            m->have_sig = 1;
        } else if (!strcmp(key, "namespace")) {
            in_ns = 1;                                  /* block follows */
        } else {
            FAIL("unknown manifest key '%s'", key);
        }
    }

    if (!have_name || !m->name[0]) FAIL("manifest has no name");
    if (!m->provides[0])           FAIL("manifest has no `provides`");
    if (!m->have_build_id)         FAIL("manifest has no build_id");
    return 0;
}
