#include "lib/random.h"
#include "crypto/hmac.h"
#include "crypto/sha256.h"
#include "include/spinlock.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "drivers/timer/timer.h"

/* ==========================================================================
 * HMAC_DRBG (SP 800-90A §10.1.2), SHA-256.
 *
 * State is two 32-byte values, K and V. Every operation is built from one
 * primitive, HMAC_DRBG_Update, which folds optional input into K and V:
 *
 *   K = HMAC(K, V || 0x00 || data);  V = HMAC(K, V)
 *   if data:  K = HMAC(K, V || 0x01 || data);  V = HMAC(K, V)
 *
 * Generate is then V = HMAC(K, V) repeated, emitting each V, followed by an
 * Update with no data so the emitted values cannot be run backwards to the
 * state (backtracking resistance). Reseed is an Update with the new entropy.
 * That is the entire construction; there is nothing else to get wrong.
 * ========================================================================== */

#define DRBG_OUTLEN   SHA256_DIGEST_SIZE
#define DRBG_RESEED_INTERVAL  (1ull << 20)    /* generates between reseeds */
#define DRBG_MAX_REQUEST      (1u << 16)      /* bytes per generate call    */

struct drbg {
    uint8_t  K[DRBG_OUTLEN];
    uint8_t  V[DRBG_OUTLEN];
    uint64_t reseed_counter;
    bool     instantiated;
    enum random_quality quality;

    uint64_t bytes_out;
    uint64_t reseeds;
    uint64_t hw_words;       /* hardware-generator words that went into seeds */
};
static struct drbg g_drbg;

static spinlock_t g_drbg_lock = SPINLOCK_INIT;

/* --- the one primitive ----------------------------------------------------- */
static void drbg_update_on(struct drbg *d, const uint8_t *data, size_t len);
static void drbg_update(const uint8_t *data, size_t len) { drbg_update_on(&g_drbg, data, len); }

static void drbg_update_on(struct drbg *g, const uint8_t *data, size_t len) {
    /* HMAC(K, V || 0x00 || data): built in a small stack buffer. Seed material
     * is bounded (see drbg_seed) so this never needs a heap allocation, which
     * matters because random_init() runs before there is a heap to allocate
     * from. */
    uint8_t msg[DRBG_OUTLEN + 1 + 256];
    if (len > 256) len = 256;

    memcpy(msg, g->V, DRBG_OUTLEN);
    msg[DRBG_OUTLEN] = 0x00;
    if (len) memcpy(msg + DRBG_OUTLEN + 1, data, len);
    hmac_sha256(g->K, DRBG_OUTLEN, msg, DRBG_OUTLEN + 1 + len, g->K);
    hmac_sha256(g->K, DRBG_OUTLEN, g->V, DRBG_OUTLEN, g->V);

    if (len == 0) return;

    memcpy(msg, g->V, DRBG_OUTLEN);
    msg[DRBG_OUTLEN] = 0x01;
    memcpy(msg + DRBG_OUTLEN + 1, data, len);
    hmac_sha256(g->K, DRBG_OUTLEN, msg, DRBG_OUTLEN + 1 + len, g->K);
    hmac_sha256(g->K, DRBG_OUTLEN, g->V, DRBG_OUTLEN, g->V);
}

/* --- seed gathering -------------------------------------------------------- */

/* Seed material accumulates through SHA-256 rather than being concatenated:
 * the sources are of wildly different quality and length, and hashing them
 * together means no single source can dominate the layout of the seed. The
 * digest is what feeds drbg_update(). */
static struct sha256_ctx g_pool;
static size_t            g_pool_bytes;

static void pool_sink(const void *data, size_t len) {
    sha256_update(&g_pool, data, len);
    g_pool_bytes += len;
}

/* A jitter loop. Not a strong source and not claimed as one: it samples the
 * cycle counter across memory traffic whose exact timing depends on cache
 * state, interrupts and the memory system, all of which are outside this
 * code's control. On real silicon that is worth some bits per sample; under
 * an emulator it is worth fewer, and random_quality() says which world the
 * seed came from. */
static void jitter_sink(void (*sink)(const void *, size_t)) {
    static volatile uint64_t scratch[512];
    uint64_t prev = time_get_ns();
    for (int i = 0; i < 256; i++) {
        /* Touch a stride that defeats the prefetcher, then time it. */
        scratch[(i * 37) & 511] += prev;
        uint64_t now = time_get_ns();
        uint64_t delta = now - prev;
        prev = now;
        sink(&delta, sizeof delta);
    }
}

/* Gather one seed's worth of material into the pool and fold it in. Returns
 * the quality actually reached. Lock held by the caller. */
static enum random_quality drbg_seed_locked(void) {
    sha256_init(&g_pool);
    g_pool_bytes = 0;
    enum random_quality q = RANDOM_JITTER;

    /* 1. Hardware, if any. 32 words -- 256 bytes -- is plenty for a 256-bit
     *    security level with margin, and it is mixed rather than used raw so
     *    a bad generator cannot lower the floor the clocks set. */
    int got = 0;
    for (int i = 0; i < 32; i++) {
        uint64_t w;
        if (arch_hw_random_u64(&w)) { pool_sink(&w, sizeof w); got++; }
    }
    if (got) { q = RANDOM_HARDWARE; g_drbg.hw_words += (uint64_t)got; }

    /* 2. Every clock, several times, with the jitter loop between so the
     *    readings differ by something other than a constant. */
    for (int round = 0; round < 4; round++) {
        arch_random_seed_extra(pool_sink);
        jitter_sink(pool_sink);
    }

    /* 3. Whatever state the DRBG already had, so a reseed never discards the
     *    entropy of the previous seed -- it can only add to it. */
    pool_sink(g_drbg.K, DRBG_OUTLEN);
    pool_sink(g_drbg.V, DRBG_OUTLEN);

    uint8_t seed[SHA256_DIGEST_SIZE];
    sha256_final(&g_pool, seed);
    drbg_update(seed, sizeof seed);
    memset(seed, 0, sizeof seed);

    g_drbg.reseed_counter = 1;
    g_drbg.reseeds++;
    return q;
}

/* --- public API --------------------------------------------------------------- */

void random_init(void) {
    spin_lock(&g_drbg_lock);
    if (!g_drbg.instantiated) {
        /* Instantiate per §10.1.2.3: K = 0x00.., V = 0x01.., then Update with
         * the seed. Done by drbg_seed_locked() folding the seed in. */
        memset(g_drbg.K, 0x00, DRBG_OUTLEN);
        memset(g_drbg.V, 0x01, DRBG_OUTLEN);
        g_drbg.instantiated = true;
    }
    g_drbg.quality = drbg_seed_locked();
    spin_unlock(&g_drbg_lock);

    kprintf("random: seeded from %s (%llu hardware words, %llu bytes of "
            "clock and jitter material)\n",
            g_drbg.quality == RANDOM_HARDWARE ? "a hardware generator + clocks"
                                              : "clocks and jitter ONLY",
            (unsigned long long)g_drbg.hw_words,
            (unsigned long long)g_pool_bytes);
}

/* Generate, on any state: V = HMAC(K, V) repeated, emit each V, then an
 * Update with no data so the emitted values cannot be run back to the state. */
static void drbg_generate_on(struct drbg *g, uint8_t *out, size_t len) {
    while (len) {
        hmac_sha256(g->K, DRBG_OUTLEN, g->V, DRBG_OUTLEN, g->V);
        size_t n = len < DRBG_OUTLEN ? len : DRBG_OUTLEN;
        memcpy(out, g->V, n);
        out += n; len -= n;
    }
    drbg_update_on(g, 0, 0);        /* backtracking resistance */
    g->reseed_counter++;
}

static void drbg_generate_locked(uint8_t *out, size_t len) {
    if (g_drbg.reseed_counter >= DRBG_RESEED_INTERVAL)
        g_drbg.quality = drbg_seed_locked();
    drbg_generate_on(&g_drbg, out, len);
}

void random_bytes(void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        size_t n = len < DRBG_MAX_REQUEST ? len : DRBG_MAX_REQUEST;
        spin_lock(&g_drbg_lock);
        if (!g_drbg.instantiated) {
            /* Called before random_init(): seed now rather than hand out the
             * all-zero instantiation constants as if they were random. */
            memset(g_drbg.K, 0x00, DRBG_OUTLEN);
            memset(g_drbg.V, 0x01, DRBG_OUTLEN);
            g_drbg.instantiated = true;
            g_drbg.quality = drbg_seed_locked();
        }
        drbg_generate_locked(p, n);
        g_drbg.bytes_out += n;
        spin_unlock(&g_drbg_lock);
        p += n; len -= n;
    }
}

uint64_t random_u64(void) {
    uint64_t v;
    random_bytes(&v, sizeof v);
    return v;
}

uint64_t random_below(uint64_t bound) {
    if (bound == 0) return 0;
    /* Rejection sampling: draw again while the draw falls in the partial
     * top bucket that a modulus would fold unevenly. For any bound the
     * expected number of draws is under 2. */
    uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
    uint64_t v;
    do { v = random_u64(); } while (v >= limit);
    return v % bound;
}

void random_add_entropy(const void *data, size_t len) {
    if (!data || !len) return;
    /* Folded straight into the state, not into the pool: the pool is only
     * live during a seed. Update never reduces entropy, so this is safe to do
     * with material of any quality, including none. */
    spin_lock(&g_drbg_lock);
    if (g_drbg.instantiated)
        drbg_update(data, len);
    spin_unlock(&g_drbg_lock);
}

enum random_quality random_quality(void) { return g_drbg.quality; }

void random_stats(uint64_t *bytes_out, uint64_t *reseeds, uint64_t *hw_seed_words) {
    if (bytes_out)     *bytes_out     = g_drbg.bytes_out;
    if (reseeds)       *reseeds       = g_drbg.reseeds;
    if (hw_seed_words) *hw_seed_words = g_drbg.hw_words;
}


/* ==========================================================================
 * THE KNOWN-ANSWER TEST -- SP 800-90A HMAC_DRBG, SHA-256, no prediction
 * resistance, with reseed, no personalisation string, no additional input.
 *
 * Until this existed, "implements HMAC_DRBG" was a claim about the code. The
 * vectors are NIST CAVP's, taken from Botan's test suite (src/tests/data/rng/
 * hmac_drbg.vec) where they are kept in a compressed form: EntropyInput is
 * CAVP's entropy(32) || nonce(16), and Out is the 1024-bit ReturnedBits of
 * the SECOND generate. The CAVP procedure for this file class is
 *
 *     Instantiate(entropy || nonce)      -> K,V
 *     Reseed(entropy_reseed)
 *     Generate(128)                      -> discarded
 *     Generate(128)                      -> compared
 *
 * and it runs on a PRIVATE state so the kernel's live generator is not
 * disturbed. The arrays were emitted from the vector file by a script, not
 * typed, because a wrong transcription would fail a correct implementation
 * and the failure would look like a bug in the code.
 *
 * 128 bytes cannot match by accident, so a match also settles the procedure
 * question the vector file's compressed form leaves open. `which` reports
 * which generate matched when the second does not: 0 = neither (broken),
 * 1 = the FIRST generate (the procedure assumption was wrong, the DRBG is
 * not), 2 = the second (correct).
 * ========================================================================== */
struct drbg_kat_vec {
    uint8_t entropy[48];
    uint8_t reseed[32];
    uint8_t out[128];
};

static const struct drbg_kat_vec KAT[] = {
    { { 0x06, 0x03, 0x2c, 0xd5, 0xee, 0xd3, 0x3f, 0x39, 0x26, 0x5f, 0x49, 0xec, 0xb1, 0x42, 0xc5, 0x11, 0xda, 0x9a, 0xff, 0x2a, 0xf7, 0x12, 0x03, 0xbf, 0xfa, 0xf3, 0x4a, 0x9c, 0xa5, 0xbd, 0x9c, 0x0d, 0x0e, 0x66, 0xf7, 0x1e, 0xdc, 0x43, 0xe4, 0x2a, 0x45, 0xad, 0x3c, 0x6f, 0xc6, 0xcd, 0xc4, 0xdf }, { 0x01, 0x92, 0x0a, 0x4e, 0x66, 0x9e, 0xd3, 0xa8, 0x5a, 0xe8, 0xa3, 0x3b, 0x35, 0xa7, 0x4a, 0xd7, 0xfb, 0x2a, 0x6b, 0xb4, 0xcf, 0x39, 0x5c, 0xe0, 0x03, 0x34, 0xa9, 0xc9, 0xa5, 0xa5, 0xd5, 0x52 }, { 0x76, 0xfc, 0x79, 0xfe, 0x9b, 0x50, 0xbe, 0xcc, 0xc9, 0x91, 0xa1, 0x1b, 0x56, 0x35, 0x78, 0x3a, 0x83, 0x53, 0x6a, 0xdd, 0x03, 0xc1, 0x57, 0xfb, 0x30, 0x64, 0x5e, 0x61, 0x1c, 0x28, 0x98, 0xbb, 0x2b, 0x1b, 0xc2, 0x15, 0x00, 0x02, 0x09, 0x20, 0x8c, 0xd5, 0x06, 0xcb, 0x28, 0xda, 0x2a, 0x51, 0xbd, 0xb0, 0x38, 0x26, 0xaa, 0xf2, 0xbd, 0x23, 0x35, 0xd5, 0x76, 0xd5, 0x19, 0x16, 0x08, 0x42, 0xe7, 0x15, 0x8a, 0xd0, 0x94, 0x9d, 0x1a, 0x9e, 0xc3, 0xe6, 0x6e, 0xa1, 0xb1, 0xa0, 0x64, 0xb0, 0x05, 0xde, 0x91, 0x4e, 0xac, 0x2e, 0x9d, 0x4f, 0x2d, 0x72, 0xa8, 0x61, 0x6a, 0x80, 0x22, 0x54, 0x22, 0x91, 0x82, 0x50, 0xff, 0x66, 0xa4, 0x1b, 0xd2, 0xf8, 0x64, 0xa6, 0xa3, 0x8c, 0xc5, 0xb6, 0x49, 0x9d, 0xc4, 0x3f, 0x7f, 0x2b, 0xd0, 0x9e, 0x1e, 0x0f, 0x8f, 0x58, 0x85, 0x93, 0x51, 0x24 } },
    { { 0xaa, 0xdc, 0xf3, 0x37, 0x78, 0x8b, 0xb8, 0xac, 0x01, 0x97, 0x66, 0x40, 0x72, 0x6b, 0xc5, 0x16, 0x35, 0xd4, 0x17, 0x77, 0x7f, 0xe6, 0x93, 0x9e, 0xde, 0xd9, 0xcc, 0xc8, 0xa3, 0x78, 0xc7, 0x6a, 0x9c, 0xcc, 0x9d, 0x80, 0xc8, 0x9a, 0xc5, 0x5a, 0x8c, 0xfe, 0x0f, 0x99, 0x94, 0x2f, 0x5a, 0x4d }, { 0x03, 0xa5, 0x77, 0x92, 0x54, 0x7e, 0x0c, 0x98, 0xea, 0x17, 0x76, 0xe4, 0xba, 0x80, 0xc0, 0x07, 0x34, 0x62, 0x96, 0xa5, 0x6a, 0x27, 0x0a, 0x35, 0xfd, 0x9e, 0xa2, 0x84, 0x5c, 0x7e, 0x81, 0xe2 }, { 0x17, 0xd0, 0x9f, 0x40, 0xa4, 0x37, 0x71, 0xf4, 0xa2, 0xf0, 0xdb, 0x32, 0x7d, 0xf6, 0x37, 0xde, 0xa9, 0x72, 0xbf, 0xff, 0x30, 0xc9, 0x8e, 0xbc, 0x88, 0x42, 0xdc, 0x7a, 0x9e, 0x3d, 0x68, 0x1c, 0x61, 0x90, 0x2f, 0x71, 0xbf, 0xfa, 0xf5, 0x09, 0x36, 0x07, 0xfb, 0xfb, 0xa9, 0x67, 0x4a, 0x70, 0xd0, 0x48, 0xe5, 0x62, 0xee, 0x88, 0xf0, 0x27, 0xf6, 0x30, 0xa7, 0x85, 0x22, 0xec, 0x6f, 0x70, 0x6b, 0xb4, 0x4a, 0xe1, 0x30, 0xe0, 0x5c, 0x8d, 0x7e, 0xac, 0x66, 0x8b, 0xf6, 0x98, 0x0d, 0x99, 0xb4, 0xc0, 0x24, 0x29, 0x46, 0x45, 0x23, 0x99, 0xcb, 0x03, 0x2c, 0xc6, 0xf9, 0xfd, 0x96, 0x28, 0x47, 0x09, 0xbd, 0x2f, 0xa5, 0x65, 0xb9, 0xeb, 0x9f, 0x20, 0x04, 0xbe, 0x6c, 0x9e, 0xa9, 0xff, 0x91, 0x28, 0xc3, 0xf9, 0x3b, 0x60, 0xdc, 0x30, 0xc5, 0xfc, 0x85, 0x87, 0xa1, 0x0d, 0xe6, 0x8c } },
};

int random_kat(unsigned idx, int *which) {
    if (idx >= sizeof KAT / sizeof KAT[0]) return -1;
    const struct drbg_kat_vec *v = &KAT[idx];
    struct drbg d;
    memset(&d, 0, sizeof d);
    memset(d.K, 0x00, DRBG_OUTLEN);
    memset(d.V, 0x01, DRBG_OUTLEN);
    drbg_update_on(&d, v->entropy, sizeof v->entropy);   /* instantiate  */
    drbg_update_on(&d, v->reseed,  sizeof v->reseed);    /* reseed       */
    uint8_t first[128], second[128];
    drbg_generate_on(&d, first,  sizeof first);
    drbg_generate_on(&d, second, sizeof second);
    int w = 0;
    if (memcmp(second, v->out, sizeof second) == 0)      w = 2;
    else if (memcmp(first, v->out, sizeof first) == 0)   w = 1;
    if (which) *which = w;
    memset(&d, 0, sizeof d);
    return w == 2 ? 0 : 1;
}

unsigned random_kat_count(void) { return sizeof KAT / sizeof KAT[0]; }
