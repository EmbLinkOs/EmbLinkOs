#include "auth.h"
#include "embk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* See auth.h. */

/* THE WORK FACTOR. PBKDF2-HMAC-SHA256 iterations for every hash made from now
 * on; a stored hash records its own count, so raising this never locks anyone
 * out -- their next successful login rehashes them at the new count.
 *
 * It was 4096, which was a reasonable number in 2000. What decides it is
 * time: an attacker with the store tries passwords at (their hardware /
 * this cost), and the owner pays this cost once per login. 210,000 is the
 * floor current guidance gives for PBKDF2-SHA256 at the time of writing.
 * Measured by `test accounts`: 163 ms to verify at native speed (aarch64 under
 * HVF), 1.7 s under full x86 emulation, which is where most of the test suite
 * runs. */
#define AUTH_ITERATIONS 210000u
#define SHA256_BLOCK 64
#define SHA256_SIZE 32

static char g_dir[64] = "/etc";
static uint32_t g_iterations = AUTH_ITERATIONS;

void embk_auth_set_dir(const char *dir) {
    snprintf(g_dir, sizeof g_dir, "%s", dir && *dir ? dir : "/etc");
}
uint32_t embk_auth_iterations(void) { return g_iterations; }
void embk_auth_set_iterations(uint32_t n) { g_iterations = n < 1000u ? 1000u : n; }

static void store_path(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", g_dir, leaf);
}

/* ---- SHA-256 ----------------------------------------------------------- */

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[SHA256_BLOCK];
    uint32_t buf_len;
};

static const uint32_t k256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void transform(struct sha256_ctx *c, const uint8_t b[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) | ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t a = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t z = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + a + w[i-7] + z;
    }
    uint32_t a = c->state[0], b0 = c->state[1], d0 = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + k256[i] + w[i], s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t t2 = s0 + ((a & b0) ^ (a & d0) ^ (b0 & d0));
        h = g; g = f; f = e; e = d + t1; d = d0; d0 = b0; b0 = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b0; c->state[2] += d0; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha_init(struct sha256_ctx *c) {
    static const uint32_t s[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                  0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(c->state, s, sizeof s); c->bitlen = 0; c->buf_len = 0;
}
static void sha_update(struct sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        size_t n = 64 - c->buf_len; if (n > len) n = len;
        memcpy(c->buf + c->buf_len, p, n);
        c->buf_len += (uint32_t)n; c->bitlen += (uint64_t)n * 8; p += n; len -= n;
        if (c->buf_len == 64) { transform(c, c->buf); c->buf_len = 0; }
    }
}
static void sha_final(struct sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->bitlen; uint8_t x = 0x80, z = 0;
    sha_update(c, &x, 1);
    while (c->buf_len != 56) sha_update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i*8));
    sha_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4] = (uint8_t)(c->state[i] >> 24); out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >> 8); out[i*4+3] = (uint8_t)c->state[i];
    }
}

/* ---- HMAC with the pads precomputed ------------------------------------
 *
 * HMAC(k, m) = H(k^opad || H(k^ipad || m)). The two padded-key blocks depend
 * only on the key -- the password -- so they are hashed ONCE per derivation,
 * not once per iteration. The first version recomputed them every time: four
 * compression calls per PBKDF2 iteration where two will do, which is half the
 * work factor thrown away (the attacker precomputes them; the defender paid). */
struct hmac_key { struct sha256_ctx inner, outer; };

static void hmac_key_init(struct hmac_key *hk, const uint8_t *key, size_t kn) {
    uint8_t kb[64] = {0}, pad[64];
    if (kn > 64) { struct sha256_ctx c; sha_init(&c); sha_update(&c, key, kn); sha_final(&c, kb); }
    else memcpy(kb, key, kn);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    sha_init(&hk->inner); sha_update(&hk->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
    sha_init(&hk->outer); sha_update(&hk->outer, pad, 64);
    memset(kb, 0, sizeof kb); memset(pad, 0, sizeof pad);
}

static void hmac_run(const struct hmac_key *hk, const uint8_t *msg, size_t mn, uint8_t out[32]) {
    struct sha256_ctx c = hk->inner;
    uint8_t inner[32];
    sha_update(&c, msg, mn); sha_final(&c, inner);
    c = hk->outer;
    sha_update(&c, inner, 32); sha_final(&c, out);
}

/* PBKDF2-HMAC-SHA256, one 32-byte block. */
static void derive(const char *password, const uint8_t salt[16], uint32_t iterations, uint8_t out[32]) {
    struct hmac_key hk;
    hmac_key_init(&hk, (const uint8_t *)password, strlen(password));
    uint8_t block[20], u[32];
    memcpy(block, salt, 16); block[16] = 0; block[17] = 0; block[18] = 0; block[19] = 1;
    hmac_run(&hk, block, sizeof block, u);
    memcpy(out, u, 32);
    for (uint32_t j = 1; j < iterations; j++) {
        hmac_run(&hk, u, 32, u);
        for (int i = 0; i < 32; i++) out[i] ^= u[i];
    }
    memset(&hk, 0, sizeof hk); memset(u, 0, sizeof u);
}

/* ---- the files --------------------------------------------------------- */

/* The whole file, NUL-terminated, in a buffer sized from the file itself. The
 * first version read into a fixed 8 KiB and silently dropped the rest: the
 * ~40th account could be created and never log in. NULL on error; an absent
 * file is an empty store, not an error. */
static char *read_file(const char *path, size_t *out_len) {
    struct embk_stat st;
    if (embk_stat(path, &st) < 0) {
        char *e = malloc(1); if (e) { e[0] = 0; *out_len = 0; } return e;
    }
    size_t cap = (size_t)st.size + 1;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) { free(buf); return NULL; }
    size_t off = 0;
    while (off + 1 < cap) {
        int64_t n = embk_read(fd, buf + off, cap - 1 - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    embk_close(fd);
    buf[off] = 0;
    *out_len = off;
    return buf;
}

/* Replace `path` with `text`: written to path.new, synced, renamed into place.
 * The rename replaces the old file in the same filesystem commit that moves
 * the name, so a crash leaves one version or the other. 0600: the store is
 * nobody's business -- and no session is given /etc anyway. */
static int write_file_atomic(const char *path, const char *text) {
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = (int)embk_open(tmp, EMBK_O_CREAT | EMBK_O_TRUNC | EMBK_O_WRONLY, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(text), off = 0;
    while (off < len) {
        int64_t n = embk_write(fd, text + off, len - off);
        if (n <= 0) { embk_close(fd); embk_unlink(tmp); return -1; }
        off += (size_t)n;
    }
    if (embk_fsync(fd) != 0) { embk_close(fd); embk_unlink(tmp); return -1; }
    embk_close(fd);
    if (rename(tmp, path) != 0) { embk_unlink(tmp); return -1; }
    return 0;
}

/* The line for `u` in a name:... file: a pointer to its start, and its length
 * including the newline. NULL if absent. */
static const char *find_line(const char *text, const char *u, size_t *line_len) {
    size_t un = strlen(u);
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) + 1 : strlen(p);
        if (ln > un && !memcmp(p, u, un) && p[un] == ':') { *line_len = ln; return p; }
        p += ln;
    }
    return NULL;
}

/* `text` with the line for `u` replaced by `line` (NULL/"" deletes it, and a
 * missing one is appended). A new malloc'd string. */
static char *with_line(const char *text, const char *u, const char *line) {
    size_t ll = 0;
    const char *at = find_line(text, u, &ll);
    size_t tn = strlen(text), nn = line ? strlen(line) : 0;
    char *out = malloc(tn + nn + 2);
    if (!out) return NULL;
    if (at) {
        size_t head = (size_t)(at - text);
        memcpy(out, text, head);
        memcpy(out + head, line ? line : "", nn);
        strcpy(out + head + nn, at + ll);
    } else {
        strcpy(out, text);
        if (tn && text[tn - 1] != '\n') strcat(out, "\n");
        if (nn) strcat(out, line);
    }
    return out;
}

/* ---- hash records ------------------------------------------------------ */

static char hex_digit(unsigned v) { return (char)(v < 10 ? '0' + v : 'a' + v - 10); }
static void to_hex(const uint8_t *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) { out[i*2] = hex_digit(in[i] >> 4); out[i*2+1] = hex_digit(in[i] & 15); }
    out[n*2] = 0;
}
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int from_hex(const char *in, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i++) {
        int a = hex_value(in[i*2]), b = hex_value(in[i*2+1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

/* A shadow line for `u` with a fresh salt, at `iterations`. */
static int make_shadow_line(const char *u, const char *password, uint32_t iterations,
                            char *line, size_t cap) {
    uint8_t salt[16], digest[32];
    if (getentropy(salt, sizeof salt) != 0) return EMBK_AUTH_EENTROPY;
    derive(password, salt, iterations, digest);
    char salt_hex[33], digest_hex[65];
    to_hex(salt, sizeof salt, salt_hex); to_hex(digest, sizeof digest, digest_hex);
    snprintf(line, cap, "%s:$pbkdf2-sha256$%u$%s$%s:0:0:99999:7:::\n",
             u, iterations, salt_hex, digest_hex);
    memset(salt, 0, sizeof salt); memset(digest, 0, sizeof digest);
    return 0;
}

/* Check `password` against the shadow line at `rec` (starting at the name).
 * Returns 1/0, and the stored iteration count in *iters. */
static int check_record(const char *rec, const char *u, const char *password, uint32_t *iters) {
    const char *p = rec + strlen(u) + 1, *prefix = "$pbkdf2-sha256$";
    if (memcmp(p, prefix, 15) != 0) return 0;
    p += 15;
    char *end; uint32_t iterations = (uint32_t)strtoul(p, &end, 10);
    if (!iterations || *end != '$') return 0;
    p = end + 1;
    uint8_t salt[16], expected[32], got[32];
    if (strlen(p) < 33 || p[32] != '$' || from_hex(p, 16, salt) < 0) return 0;
    p += 33;
    if (strlen(p) < 64 || from_hex(p, 32, expected) < 0) return 0;
    derive(password, salt, iterations, got);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= got[i] ^ expected[i];     /* constant time */
    memset(got, 0, sizeof got); memset(expected, 0, sizeof expected);
    *iters = iterations;
    return diff == 0;
}

/* ---- the API ----------------------------------------------------------- */

int embk_auth_valid_username(const char *u) {
    size_t n = strlen(u);
    if (!n || n > EMBK_AUTH_USERNAME_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((u[i] >= 'a' && u[i] <= 'z') || (u[i] >= '0' && u[i] <= '9') || u[i] == '_' || u[i] == '-'))
            return 0;
    return 1;
}

int embk_auth_has_accounts(void) {
    char path[96]; struct embk_stat st;
    store_path(path, sizeof path, "shadow");
    return embk_stat(path, &st) >= 0 && st.size > 0;
}

/* The gid field of `u`'s passwd line, or -1. */
static int passwd_gid(const char *passwd, const char *u) {
    size_t ll;
    const char *l = find_line(passwd, u, &ll);
    if (!l) return -1;
    const char *p = l;
    for (int field = 0; field < 3; field++) { p = memchr(p, ':', ll - (size_t)(p - l)); if (!p) return -1; p++; }
    return (int)strtol(p, NULL, 10);
}

int embk_auth_is_admin(const char *u) {
    char path[96]; size_t n;
    store_path(path, sizeof path, "passwd");
    char *pw = read_file(path, &n);
    if (!pw) return 0;
    int gid = passwd_gid(pw, u);
    free(pw);
    return gid == EMBK_AUTH_GID_ADMIN;
}

static int count_admins(const char *passwd) {
    int n = 0;
    for (const char *p = passwd; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) + 1 : strlen(p);
        const char *c = p;
        for (int field = 0; field < 3 && c; field++) { c = memchr(c, ':', ll - (size_t)(c - p)); if (c) c++; }
        if (c && strtol(c, NULL, 10) == EMBK_AUTH_GID_ADMIN) n++;
        p += ll;
    }
    return n;
}

int embk_auth_create(const char *u, const char *password) {
    return embk_auth_create_as(u, password, 0);
}

int embk_auth_create_as(const char *u, const char *password, int want_admin) {
    if (!embk_auth_valid_username(u)) return EMBK_AUTH_EBADNAME;
    size_t pn = strlen(password);
    if (pn < EMBK_AUTH_PASSWORD_MIN || pn > EMBK_AUTH_PASSWORD_MAX) return EMBK_AUTH_EWEAK;

    char sp[96], pp[96]; size_t sn, ppn;
    store_path(sp, sizeof sp, "shadow"); store_path(pp, sizeof pp, "passwd");
    char *shadow = read_file(sp, &sn), *passwd = read_file(pp, &ppn);
    if (!shadow || !passwd) { free(shadow); free(passwd); return EMBK_AUTH_EIO; }
    size_t ll;
    if (find_line(shadow, u, &ll)) { free(shadow); free(passwd); return EMBK_AUTH_EEXIST; }

    /* The first account on the machine administers it. A uid above every one
     * already used -- counting lines, as the first version did, reissued a
     * removed account's number to the next one created. */
    int admin = (sn == 0) || want_admin;
    int uid = 1000;
    for (const char *p = passwd; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) + 1 : strlen(p);
        const char *c = memchr(p, ':', n);
        if (c) c = memchr(c + 1, ':', n - (size_t)(c + 1 - p));
        if (c) { int v = (int)strtol(c + 1, NULL, 10); if (v >= uid) uid = v + 1; }
        p += n;
    }

    char line[192], pline[192];
    int rc = make_shadow_line(u, password, g_iterations, line, sizeof line);
    if (rc) { free(shadow); free(passwd); return rc; }
    snprintf(pline, sizeof pline, "%s:x:%d:%d:%s:/home/%s:/system/bin/shell.elf\n",
             u, uid, admin ? EMBK_AUTH_GID_ADMIN : EMBK_AUTH_GID_USER, u, u);

    /* passwd FIRST: an account whose shadow line never landed cannot log in,
     * which is the safe way to be half-created. with_line replaces a stale
     * passwd line from such a half-creation instead of duplicating it. */
    char *np = with_line(passwd, u, pline), *ns = with_line(shadow, u, line);
    rc = (np && ns && write_file_atomic(pp, np) == 0 && write_file_atomic(sp, ns) == 0) ? 0 : EMBK_AUTH_EIO;
    memset(line, 0, sizeof line);
    if (ns) memset(ns, 0, strlen(ns));
    memset(shadow, 0, sn);
    free(np); free(ns); free(shadow); free(passwd);
    return rc;
}

/* Replace `u`'s shadow line with one for `password` (fresh salt, today's
 * work factor). */
static int rewrite_hash(const char *u, const char *password) {
    char sp[96]; size_t sn;
    store_path(sp, sizeof sp, "shadow");
    char *shadow = read_file(sp, &sn);
    if (!shadow) return EMBK_AUTH_EIO;
    size_t ll;
    if (!find_line(shadow, u, &ll)) { free(shadow); return EMBK_AUTH_ENOUSER; }
    char line[192];
    int rc = make_shadow_line(u, password, g_iterations, line, sizeof line);
    if (rc) { free(shadow); return rc; }
    char *ns = with_line(shadow, u, line);
    rc = (ns && write_file_atomic(sp, ns) == 0) ? 0 : EMBK_AUTH_EIO;
    memset(line, 0, sizeof line);
    if (ns) { memset(ns, 0, strlen(ns)); free(ns); }
    memset(shadow, 0, sn); free(shadow);
    return rc;
}

int embk_auth_verify(const char *u, const char *password) {
    if (!embk_auth_valid_username(u) || strlen(password) > EMBK_AUTH_PASSWORD_MAX) return 0;
    char sp[96]; size_t sn;
    store_path(sp, sizeof sp, "shadow");
    char *shadow = read_file(sp, &sn);
    if (!shadow) return 0;
    size_t ll;
    const char *rec = find_line(shadow, u, &ll);
    uint32_t iters = 0;
    int ok = rec ? check_record(rec, u, password, &iters) : 0;
    memset(shadow, 0, sn); free(shadow);
    /* Right, but hashed at an older, weaker work factor: rehash now, while
     * the password is in hand -- the only moment it ever is. A failure to
     * rewrite is not a failure to log in. */
    if (ok && iters < g_iterations)
        (void)rewrite_hash(u, password);
    return ok;
}

int embk_auth_change_password(const char *u, const char *old_pw, const char *new_pw) {
    if (!embk_auth_valid_username(u)) return EMBK_AUTH_EBADNAME;
    size_t pn = strlen(new_pw);
    if (pn < EMBK_AUTH_PASSWORD_MIN || pn > EMBK_AUTH_PASSWORD_MAX) return EMBK_AUTH_EWEAK;
    if (!embk_auth_verify(u, old_pw)) return EMBK_AUTH_EDENIED;
    return rewrite_hash(u, new_pw);
}

int embk_auth_set_password(const char *u, const char *new_pw) {
    if (!embk_auth_valid_username(u)) return EMBK_AUTH_EBADNAME;
    size_t pn = strlen(new_pw);
    if (pn < EMBK_AUTH_PASSWORD_MIN || pn > EMBK_AUTH_PASSWORD_MAX) return EMBK_AUTH_EWEAK;
    return rewrite_hash(u, new_pw);
}

int embk_auth_remove(const char *u) {
    if (!embk_auth_valid_username(u)) return EMBK_AUTH_EBADNAME;
    char sp[96], pp[96]; size_t sn, ppn;
    store_path(sp, sizeof sp, "shadow"); store_path(pp, sizeof pp, "passwd");
    char *shadow = read_file(sp, &sn), *passwd = read_file(pp, &ppn);
    if (!shadow || !passwd) { free(shadow); free(passwd); return EMBK_AUTH_EIO; }
    size_t ll;
    if (!find_line(shadow, u, &ll)) { free(shadow); free(passwd); return EMBK_AUTH_ENOUSER; }
    if (passwd_gid(passwd, u) == EMBK_AUTH_GID_ADMIN && count_admins(passwd) <= 1) {
        free(shadow); free(passwd); return EMBK_AUTH_ELASTADMIN;
    }
    /* shadow FIRST: once the hash is gone the account cannot log in, whatever
     * happens to the passwd line after. */
    char *ns = with_line(shadow, u, NULL), *np = with_line(passwd, u, NULL);
    int rc = (ns && np && write_file_atomic(sp, ns) == 0 && write_file_atomic(pp, np) == 0) ? 0 : EMBK_AUTH_EIO;
    if (ns) memset(ns, 0, strlen(ns));
    memset(shadow, 0, sn);
    free(ns); free(np); free(shadow); free(passwd);
    return rc;
}

int embk_auth_list(char names[][EMBK_AUTH_USERNAME_MAX + 1], int max) {
    char sp[96]; size_t sn;
    store_path(sp, sizeof sp, "shadow");
    char *shadow = read_file(sp, &sn);
    if (!shadow) return 0;
    int n = 0;
    for (const char *p = shadow; *p && n < max; ) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) + 1 : strlen(p);
        const char *c = memchr(p, ':', ll);
        if (c && c > p && (size_t)(c - p) <= EMBK_AUTH_USERNAME_MAX) {
            memcpy(names[n], p, (size_t)(c - p)); names[n][c - p] = 0; n++;
        }
        p += ll;
    }
    memset(shadow, 0, sn); free(shadow);
    return n;
}

int embk_auth_provision(const char *u) {
    if (!embk_auth_valid_username(u)) return EMBK_AUTH_EBADNAME;
    static const char *folders[] = { "Desktop", "Documents", "Downloads", "Music",
                                     "Pictures", "Videos", "Trash" };
    char path[160];
    snprintf(path, sizeof path, "/home/%s", u);
    struct embk_stat st;
    if (embk_mkdir(path) < 0 && (embk_stat(path, &st) < 0 || st.type != EMBK_DT_DIR))
        return EMBK_AUTH_EIO;
    for (size_t i = 0; i < sizeof folders / sizeof folders[0]; i++) {
        snprintf(path, sizeof path, "/home/%s/%s", u, folders[i]);
        (void)embk_mkdir(path);
    }
    snprintf(path, sizeof path, "%s/sessions", g_dir);
    (void)embk_mkdir(path);
    snprintf(path, sizeof path, "%s/sessions/%s.ns", g_dir, u);
    char profile[256];
    snprintf(profile, sizeof profile,
             "# Session profile for %s -- the namespace init grants this user's desktop.\n"
             "# init clamps it (user/lib/session_policy.h): /etc and other homes are never granted.\n"
             "ro /system\nro /data/apps\nrw /home/%s\nrw /run\n", u, u);
    return write_file_atomic(path, profile) == 0 ? 0 : EMBK_AUTH_EIO;
}

unsigned embk_auth_throttle_ms(unsigned failures) {
    if (failures < 3) return 0;
    unsigned shift = failures - 3;
    if (shift > 5) shift = 5;                /* 1, 2, 4, 8, 16, then 30 */
    unsigned ms = 1000u << shift;
    return ms > 30000u ? 30000u : ms;
}
