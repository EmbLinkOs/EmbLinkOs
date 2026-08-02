/* DEFLATE decompression (RFC 1951). Canonical-Huffman decode in the style of
 * Mark Adler's puff.c: a code is decoded by walking code lengths 1..15 and
 * comparing against the running count per length. Not fast, but small and clearly
 * correct -- fine for one-shot wheel extraction. */
#include "inflate.h"
#include <string.h>

struct state {
    const uint8_t *in; size_t in_len, in_pos;
    int bitbuf, bitcnt;                 /* LSB-first bit reservoir */
    uint8_t *out; size_t out_cap, out_pos;
};

/* Read one bit (LSB first). Returns 0/1, or the low bit of 0 past the end. */
static int getbit(struct state *s) {
    if (s->bitcnt == 0) {
        if (s->in_pos >= s->in_len) return -1;
        s->bitbuf = s->in[s->in_pos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}
/* Read `n` bits LSB-first as an unsigned value. */
static int getbits(struct state *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = getbit(s);
        if (b < 0) return -1;
        v |= b << i;
    }
    return v;
}

struct huff { short count[16]; short symbol[288]; };

static int decode(struct state *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = getbit(s);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[len];
        if (code - first < cnt) return h->symbol[index + (code - first)];
        index += cnt;
        first += cnt; first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void construct(struct huff *h, const short *length, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
}

/* RFC 1951 §3.2.5 length/distance base + extra-bit tables. */
static const short len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_block(struct state *s, const struct huff *lc, const struct huff *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym == 256) return 0;                       /* end of block */
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) return -2;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = len_base[sym] + getbits(s, len_extra[sym]);
            int dsym = decode(s, dc);
            if (dsym < 0 || dsym >= 30) return -1;
            int dist = dist_base[dsym] + getbits(s, dist_extra[dsym]);
            if ((size_t)dist > s->out_pos) return -1;   /* reference before start */
            if (s->out_pos + len > s->out_cap) return -2;
            uint8_t *o = s->out + s->out_pos;
            const uint8_t *from = o - dist;
            for (int i = 0; i < len; i++) o[i] = from[i]; /* may overlap (LZ77) */
            s->out_pos += len;
        }
    }
}

static void fixed_tables(struct huff *lc, struct huff *dc) {
    short ll[288], dl[30];
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0; i < 30; i++) dl[i] = 5;
    construct(lc, ll, 288);
    construct(dc, dl, 30);
}

static int dynamic_tables(struct state *s, struct huff *lc, struct huff *dc) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit  = getbits(s, 5) + 257;
    int hdist = getbits(s, 5) + 1;
    int hclen = getbits(s, 4) + 4;
    if (hlit > 286 || hdist > 30) return -1;

    short cl[19];
    for (int i = 0; i < 19; i++) cl[i] = 0;
    for (int i = 0; i < hclen; i++) { int v = getbits(s, 3); if (v < 0) return -1; cl[order[i]] = (short)v; }
    struct huff clh;
    construct(&clh, cl, 19);

    short lengths[288 + 30];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = decode(s, &clh);
        if (sym < 0) return -1;
        if (sym < 16) { lengths[n++] = (short)sym; }
        else if (sym == 16) { if (n == 0) return -1; int r = getbits(s, 2) + 3; while (r-- && n < total) { lengths[n] = lengths[n-1]; n++; } }
        else if (sym == 17) { int r = getbits(s, 3) + 3;  while (r-- && n < total) lengths[n++] = 0; }
        else                { int r = getbits(s, 7) + 11; while (r-- && n < total) lengths[n++] = 0; }
    }
    construct(lc, lengths, hlit);
    construct(dc, lengths + hlit, hdist);
    return 0;
}

int inflate_raw(const uint8_t *src, size_t src_len,
                uint8_t *dst, size_t dst_cap, size_t *out_len) {
    struct state s = { src, src_len, 0, 0, 0, dst, dst_cap, 0 };

    int final;
    do {
        final = getbit(&s);
        int type = getbits(&s, 2);
        if (final < 0 || type < 0) return -1;

        if (type == 0) {                                /* stored */
            s.bitbuf = 0; s.bitcnt = 0;                 /* align to byte */
            if (s.in_pos + 4 > s.in_len) return -1;
            int len  = s.in[s.in_pos] | (s.in[s.in_pos+1] << 8);
            s.in_pos += 4;                              /* skip LEN + NLEN */
            if (s.in_pos + len > s.in_len) return -1;
            if (s.out_pos + (size_t)len > s.out_cap) return -2;
            memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
            s.in_pos += len; s.out_pos += len;
        } else if (type == 1) {                         /* fixed Huffman */
            struct huff lc, dc;
            fixed_tables(&lc, &dc);
            int r = inflate_block(&s, &lc, &dc);
            if (r < 0) return r;
        } else if (type == 2) {                         /* dynamic Huffman */
            struct huff lc, dc;
            if (dynamic_tables(&s, &lc, &dc) < 0) return -1;
            int r = inflate_block(&s, &lc, &dc);
            if (r < 0) return r;
        } else {
            return -1;                                  /* reserved */
        }
    } while (!final);

    *out_len = s.out_pos;
    return 0;
}
