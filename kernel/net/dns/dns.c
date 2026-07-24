/* DNS (RFC 1035): a minimal A-record resolver over the UDP capture. Builds a
 * query, sends it to the DHCP-learned resolver, and parses the first A record,
 * skipping the echoed question and any compressed names. */

#include "net/net.h"
#include "include/kstring.h"

#define DNS_PORT     53
#define DNS_EPH_PORT 0xC000                  /* our source port (49152) */

struct dns_hdr {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

/* Skip a (possibly compressed) name; return the offset just past it, or 0. */
static uint32_t dns_skip_name(const uint8_t *msg, uint32_t len, uint32_t off) {
    while (off < len) {
        uint8_t b = msg[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) return off + 2;   /* a 2-byte pointer ends the name */
        off += 1 + b;
    }
    return 0;
}

bool net_resolve(const char *name, uint32_t *out_ip) {
    if (!g_netif.dns || !name || !out_ip) return false;
    static uint16_t dns_seq = 0x1000;
    uint16_t id = ++dns_seq;

    /* Build the query: header + QNAME(labels) + QTYPE(A=1) + QCLASS(IN=1). */
    uint8_t q[300];
    struct dns_hdr *h = (struct dns_hdr *)q;
    memset(h, 0, sizeof(*h));
    h->id = htons(id);
    h->flags = htons(0x0100);                 /* recursion desired */
    h->qdcount = htons(1);
    uint8_t *p = q + sizeof(*h);
    const char *s = name;
    while (*s) {
        const char *dot = s;
        while (*dot && *dot != '.') dot++;
        uint32_t l = (uint32_t)(dot - s);
        if (l == 0 || l > 63 || p + 1 + l + 6 > q + sizeof(q)) return false;
        *p++ = (uint8_t)l;
        memcpy(p, s, l); p += l;
        s = (*dot == '.') ? dot + 1 : dot;
    }
    *p++ = 0;                                 /* root label */
    *p++ = 0; *p++ = 1;                        /* QTYPE  = A  */
    *p++ = 0; *p++ = 1;                        /* QCLASS = IN */
    uint32_t qlen = (uint32_t)(p - q);

    udp_arm(DNS_EPH_PORT);
    if (net_send_udp(g_netif.ip, DNS_EPH_PORT, g_netif.dns, DNS_PORT, q, qlen) < 0) return false;

    static uint8_t in[1024];
    int r = udp_collect(in, sizeof(in), 0);
    if (r < (int)sizeof(struct dns_hdr)) return false;
    const struct dns_hdr *rh = (const struct dns_hdr *)in;
    if (ntohs(rh->id) != id) return false;
    if ((ntohs(rh->flags) & 0x000F) != 0) return false;   /* RCODE != 0 */
    uint16_t qd = ntohs(rh->qdcount), an = ntohs(rh->ancount);
    if (an == 0) return false;

    uint32_t off = sizeof(*rh);
    for (uint16_t i = 0; i < qd; i++) {         /* skip the echoed question(s) */
        off = dns_skip_name(in, r, off);
        if (!off || off + 4 > (uint32_t)r) return false;
        off += 4;
    }
    for (uint16_t i = 0; i < an; i++) {         /* first A record wins */
        off = dns_skip_name(in, r, off);
        if (!off || off + 10 > (uint32_t)r) return false;
        uint16_t type  = net_rd16be(in + off);
        uint16_t rdlen = net_rd16be(in + off + 8);
        off += 10;
        if (off + rdlen > (uint32_t)r) return false;
        if (type == 1 && rdlen == 4) {
            *out_ip = net_ip_rd(in + off);
            return true;
        }
        off += rdlen;
    }
    return false;
}
