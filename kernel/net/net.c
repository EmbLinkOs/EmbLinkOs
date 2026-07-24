/* EmbLinkOS network stack core, M1: Ethernet demux + ARP + IPv4 + ICMP echo.
 *
 * Deliberately small and synchronous. ARP/ping wait by driving virtio_net_poll()
 * themselves (in addition to the background RX kthread), so a witness thread does
 * not depend on the poller being scheduled. One NIC, static config; DHCP/DNS are
 * M2, TCP is M3, the ring-3 CAP_NETWORK surface is M4. IPs are HOST order here
 * and converted (htonl/ntohl) only at the header boundary. */

#include "net/net.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"   /* process_create_kthread, schedule */

struct netif g_netif;

/* ---- internet checksum (RFC 1071 / BSD in_cksum): result stored directly --- */
uint16_t net_checksum(const void *data, uint32_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static const uint8_t BCAST[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void ip_to_bytes(uint32_t ip, uint8_t b[4]) {
    b[0] = ip >> 24; b[1] = ip >> 16; b[2] = ip >> 8; b[3] = ip;
}
static uint32_t bytes_to_ip(const uint8_t b[4]) {
    return IPV4(b[0], b[1], b[2], b[3]);
}

/* ---- Ethernet TX -------------------------------------------------------- */
int net_tx_eth(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
               const void *payload, uint32_t len) {
    if (len > ETH_FRAME_MAX - ETH_HLEN) return -1;
    uint8_t frame[ETH_FRAME_MAX];
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst_mac, ETH_ALEN);
    memcpy(e->src, g_netif.mac, ETH_ALEN);
    e->ethertype = htons(ethertype);
    memcpy(frame + ETH_HLEN, payload, len);
    return virtio_net_tx(frame, ETH_HLEN + len);
}

/* ---- ARP ---------------------------------------------------------------- */
#define ARP_CACHE_N 16
static struct { uint32_t ip; uint8_t mac[ETH_ALEN]; bool valid; } arp_cache[ARP_CACHE_N];

static void arp_cache_put(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { slot = i; break; }
        if (slot < 0 && !arp_cache[i].valid) slot = i;
    }
    if (slot < 0) slot = ip % ARP_CACHE_N;      /* evict on full */
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, ETH_ALEN);
    arp_cache[slot].valid = true;
}
static bool arp_cache_get(uint32_t ip, uint8_t mac[ETH_ALEN]) {
    for (int i = 0; i < ARP_CACHE_N; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, ETH_ALEN); return true;
        }
    return false;
}

static void arp_send(uint16_t op, uint32_t target_ip, const uint8_t target_mac[ETH_ALEN]) {
    struct arp_pkt a;
    a.htype = htons(ARP_HTYPE_ETH);
    a.ptype = htons(ETH_P_IP);
    a.hlen = ETH_ALEN; a.plen = 4;
    a.oper = htons(op);
    memcpy(a.sha, g_netif.mac, ETH_ALEN);
    ip_to_bytes(g_netif.ip, a.spa);
    memcpy(a.tha, target_mac, ETH_ALEN);
    ip_to_bytes(target_ip, a.tpa);
    net_tx_eth(op == ARP_OP_REQUEST ? BCAST : target_mac, ETH_P_ARP, &a, sizeof(a));
}

static void arp_input(const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt *)pkt;
    if (ntohs(a->ptype) != ETH_P_IP || a->plen != 4) return;

    uint32_t spa = bytes_to_ip(a->spa);
    uint32_t tpa = bytes_to_ip(a->tpa);
    arp_cache_put(spa, a->sha);                 /* learn the sender either way */

    if (ntohs(a->oper) == ARP_OP_REQUEST && tpa == g_netif.ip)
        arp_send(ARP_OP_REPLY, spa, a->sha);    /* answer "who has us" */
}

bool arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    if (arp_cache_get(ip, mac_out)) return true;
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(ARP_OP_REQUEST, ip, BCAST);
        for (int i = 0; i < 200000; i++) {
            virtio_net_poll();                  /* drive RX while we wait */
            if (arp_cache_get(ip, mac_out)) return true;
            schedule();
        }
    }
    return false;
}

/* ---- ICMP --------------------------------------------------------------- */
static volatile struct { bool waiting; uint16_t id, seq; volatile bool got; } g_ping;

static void icmp_input(uint32_t src_ip, const uint8_t *msg, uint32_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *ih = (const struct icmp_hdr *)msg;

    if (ih->type == ICMP_ECHO_REQUEST) {
        /* Reflect it: same id/seq/data, type -> reply, fresh checksum. */
        uint8_t buf[ETH_FRAME_MAX - ETH_HLEN - sizeof(struct ip_hdr)];
        if (len > sizeof(buf)) return;
        memcpy(buf, msg, len);
        struct icmp_hdr *r = (struct icmp_hdr *)buf;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = net_checksum(buf, len);
        net_send_ip(src_ip, IP_PROTO_ICMP, buf, len);
    } else if (ih->type == ICMP_ECHO_REPLY) {
        if (g_ping.waiting && ntohs(ih->id) == g_ping.id && ntohs(ih->seq) == g_ping.seq)
            g_ping.got = true;
    }
}

/* ---- IPv4 --------------------------------------------------------------- */
/* Core IPv4 output. `src_ip` may be 0 (unconfigured, e.g. DHCP DISCOVER). A dst
 * of 255.255.255.255 goes out as an Ethernet broadcast with no ARP; otherwise
 * next hop is the host itself (on-subnet) or the default gateway. */
static int ip_output(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                     const void *payload, uint32_t len) {
    uint8_t pkt[ETH_FRAME_MAX - ETH_HLEN];
    if (sizeof(struct ip_hdr) + len > sizeof(pkt)) return -1;

    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = htons(sizeof(struct ip_hdr) + len);
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->src       = htonl(src_ip);
    ip->dst       = htonl(dst_ip);
    ip->checksum  = net_checksum(ip, sizeof(*ip));
    memcpy(pkt + sizeof(*ip), payload, len);

    uint8_t mac[ETH_ALEN];
    if (dst_ip == 0xFFFFFFFFu) {
        memcpy(mac, BCAST, ETH_ALEN);
    } else {
        uint32_t nexthop = ((dst_ip & g_netif.netmask) == (g_netif.ip & g_netif.netmask))
                           ? dst_ip : g_netif.gateway;
        if (!arp_resolve(nexthop, mac)) return -1;
    }
    return net_tx_eth(mac, ETH_P_IP, pkt, sizeof(struct ip_hdr) + len);
}

int net_send_ip(uint32_t dst_ip, uint8_t proto, const void *payload, uint32_t len) {
    return ip_output(g_netif.ip, dst_ip, proto, payload, len);
}

/* ---- UDP ---------------------------------------------------------------- */
/* UDP checksum over the pseudo-header + segment (RFC 768). */
static uint16_t udp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t seg_len) {
    uint32_t sum = 0;
    uint32_t s = htonl(src), d = htonl(dst);
    const uint16_t *ph = (const uint16_t *)&s; sum += ph[0]; sum += ph[1];
    ph = (const uint16_t *)&d;                 sum += ph[0]; sum += ph[1];
    sum += htons(IP_PROTO_UDP);
    sum += htons((uint16_t)seg_len);
    const uint16_t *p = (const uint16_t *)seg;
    uint32_t n = seg_len;
    while (n > 1) { sum += *p++; n -= 2; }
    if (n) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return c ? c : 0xFFFF;                      /* 0 means "none"; send 0xFFFF */
}

int net_send_udp(uint32_t src_ip, uint16_t src_port,
                 uint32_t dst_ip, uint16_t dst_port,
                 const void *payload, uint32_t len) {
    uint8_t seg[ETH_FRAME_MAX - ETH_HLEN - sizeof(struct ip_hdr)];
    uint32_t seg_len = sizeof(struct udp_hdr) + len;
    if (seg_len > sizeof(seg)) return -1;
    struct udp_hdr *u = (struct udp_hdr *)seg;
    u->src_port = htons(src_port);
    u->dst_port = htons(dst_port);
    u->len      = htons((uint16_t)seg_len);
    u->checksum = 0;
    memcpy(seg + sizeof(*u), payload, len);
    u->checksum = udp_checksum(src_ip, dst_ip, seg, seg_len);
    return ip_output(src_ip, dst_ip, IP_PROTO_UDP, seg, seg_len);
}

/* Single-slot receive capture -- enough for the sequential request/reply flows
 * of DHCP (and later DNS). Armed on a local port; udp_input fills it. */
#define UDP_CAP_MAX 1024
static volatile struct {
    bool     armed;
    uint16_t port;
    uint32_t from_ip;
    uint8_t  buf[UDP_CAP_MAX];
    uint32_t len;
    volatile bool got;
} g_udp;

static void udp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len) {
    if (seg_len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *u = (const struct udp_hdr *)seg;
    uint32_t dlen = ntohs(u->len);
    if (dlen < sizeof(*u) || dlen > seg_len) dlen = seg_len;
    const uint8_t *data = seg + sizeof(*u);
    uint32_t datalen = dlen - sizeof(*u);
    if (g_udp.armed && !g_udp.got && ntohs(u->dst_port) == g_udp.port) {
        uint32_t k = datalen < UDP_CAP_MAX ? datalen : UDP_CAP_MAX;
        memcpy((void *)g_udp.buf, data, k);
        g_udp.len = k;
        g_udp.from_ip = src_ip;
        g_udp.got = true;
    }
}

static void udp_arm(uint16_t port) {
    g_udp.port = port; g_udp.len = 0; g_udp.from_ip = 0; g_udp.got = false; g_udp.armed = true;
}
/* Poll (bounded) for the armed datagram; returns bytes into `out`, or -1. */
static int udp_collect(uint8_t *out, uint32_t cap, uint32_t *from_ip) {
    for (int i = 0; i < 400000 && !g_udp.got; i++) { virtio_net_poll(); schedule(); }
    int rc = -1;
    if (g_udp.got) {
        uint32_t k = g_udp.len < cap ? g_udp.len : cap;
        memcpy(out, (void *)g_udp.buf, k);
        if (from_ip) *from_ip = g_udp.from_ip;
        rc = (int)k;
    }
    g_udp.armed = false;
    return rc;
}

static void ip_input(const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;
    if ((ip->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(struct ip_hdr) || ihl > len) return;

    /* Accept: unicast to us, the limited broadcast, or -- while we are still
     * unconfigured mid-DHCP (ip == 0) -- anything. No forwarding (M1). */
    uint32_t dst = ntohl(ip->dst);
    if (dst != g_netif.ip && dst != 0xFFFFFFFFu && g_netif.ip != 0) return;

    uint32_t src = ntohl(ip->src);
    const uint8_t *payload = pkt + ihl;
    /* Trust the IP total length over the frame length: a minimum-size Ethernet
     * frame is zero-padded to 60 bytes, so `len - ihl` would include padding. */
    uint32_t plen = len - ihl;
    uint32_t total = ntohs(ip->total_len);
    if (total >= ihl && total <= len) plen = total - ihl;

    if (ip->proto == IP_PROTO_ICMP)      icmp_input(src, payload, plen);
    else if (ip->proto == IP_PROTO_UDP)  udp_input(src, payload, plen);
    /* TCP: M3 */
}

/* ---- Ethernet RX demux (driver -> here) --------------------------------- */
void net_rx(const uint8_t *frame, uint32_t len) {
    if (len < ETH_HLEN) return;
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    uint16_t et = ntohs(e->ethertype);
    const uint8_t *payload = frame + ETH_HLEN;
    uint32_t plen = len - ETH_HLEN;
    if (et == ETH_P_ARP) arp_input(payload, plen);
    else if (et == ETH_P_IP) ip_input(payload, plen);
}

/* ---- ICMP echo witness (M1) --------------------------------------------- */
bool net_ping(uint32_t dst_ip) {
    static uint16_t seq = 0;
    struct { struct icmp_hdr h; uint8_t data[32]; } echo;
    memset(&echo, 0, sizeof(echo));
    echo.h.type = ICMP_ECHO_REQUEST;
    echo.h.id = htons(0x1234);
    echo.h.seq = htons(++seq);
    for (uint32_t i = 0; i < sizeof(echo.data); i++) echo.data[i] = (uint8_t)i;

    g_ping.id = 0x1234; g_ping.seq = seq; g_ping.got = false; g_ping.waiting = true;
    echo.h.checksum = 0;
    echo.h.checksum = net_checksum(&echo, sizeof(echo));

    if (net_send_ip(dst_ip, IP_PROTO_ICMP, &echo, sizeof(echo)) < 0) {
        g_ping.waiting = false; return false;
    }
    for (int i = 0; i < 400000 && !g_ping.got; i++) {
        virtio_net_poll();
        schedule();
    }
    g_ping.waiting = false;
    return g_ping.got;
}

/* ---- DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK --------- */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u
#define DHCP_XID         0x454D4231u        /* "EMB1" -- any fixed value the reply echoes */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define DHCPOPT_MASK      1
#define DHCPOPT_ROUTER    3
#define DHCPOPT_DNS       6
#define DHCPOPT_REQIP     50
#define DHCPOPT_MSGTYPE   53
#define DHCPOPT_SERVERID  54
#define DHCPOPT_PARAMLIST 55
#define DHCPOPT_END       255

struct dhcp_hdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;                          /* network order 0x63825363 */
    /* options follow */
} __attribute__((packed));

static uint32_t rd_be32(const uint8_t *p) { return IPV4(p[0], p[1], p[2], p[3]); }

/* Find option `code` in the TLV blob; returns its value pointer (+ *outlen). */
static const uint8_t *dhcp_opt(const uint8_t *o, uint32_t len, uint8_t code, uint8_t *outlen) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t c = o[i++];
        if (c == 0) continue;                /* pad */
        if (c == DHCPOPT_END || i >= len) break;
        uint8_t l = o[i++];
        if (i + l > len) break;
        if (c == code) { if (outlen) *outlen = l; return &o[i]; }
        i += l;
    }
    return 0;
}

static int dhcp_build(uint8_t *buf, uint8_t type, uint32_t req_ip, uint32_t server_id) {
    struct dhcp_hdr *h = (struct dhcp_hdr *)buf;
    memset(h, 0, sizeof(*h));
    h->op = 1; h->htype = 1; h->hlen = 6;
    h->xid = htonl(DHCP_XID);
    h->flags = htons(0x8000);                /* ask the server to broadcast the reply */
    memcpy(h->chaddr, g_netif.mac, ETH_ALEN);
    h->magic = htonl(DHCP_MAGIC);
    uint8_t *o = buf + sizeof(*h);
    *o++ = DHCPOPT_MSGTYPE; *o++ = 1; *o++ = type;
    if (type == DHCP_REQUEST) {
        *o++ = DHCPOPT_REQIP;    *o++ = 4;
        *o++ = req_ip >> 24; *o++ = req_ip >> 16; *o++ = req_ip >> 8; *o++ = req_ip;
        *o++ = DHCPOPT_SERVERID; *o++ = 4;
        *o++ = server_id >> 24; *o++ = server_id >> 16; *o++ = server_id >> 8; *o++ = server_id;
    }
    *o++ = DHCPOPT_PARAMLIST; *o++ = 4;
    *o++ = DHCPOPT_MASK; *o++ = DHCPOPT_ROUTER; *o++ = DHCPOPT_DNS; *o++ = DHCPOPT_MSGTYPE;
    *o++ = DHCPOPT_END;
    return (int)(o - buf);
}

/* Send one DHCP message and capture the reply of expected type. Returns the
 * reply length, or -1. `in` must hold >= sizeof(dhcp_hdr) + options. */
static int dhcp_xact(uint8_t type, uint32_t req_ip, uint32_t server_id,
                     uint8_t *in, uint32_t incap, uint8_t expect) {
    static uint8_t out[512];
    udp_arm(DHCP_CLIENT_PORT);
    int n = dhcp_build(out, type, req_ip, server_id);
    if (net_send_udp(0, DHCP_CLIENT_PORT, 0xFFFFFFFFu, DHCP_SERVER_PORT, out, n) < 0) return -1;
    int r = udp_collect(in, incap, 0);
    if (r < (int)sizeof(struct dhcp_hdr)) return -1;
    const struct dhcp_hdr *h = (const struct dhcp_hdr *)in;
    if (h->op != 2 || ntohl(h->xid) != DHCP_XID) return -1;
    uint8_t ol;
    const uint8_t *mt = dhcp_opt(in + sizeof(*h), r - sizeof(*h), DHCPOPT_MSGTYPE, &ol);
    if (!mt || mt[0] != expect) return -1;
    return r;
}

bool net_dhcp(void) {
    static uint8_t in[1024];

    int r = dhcp_xact(DHCP_DISCOVER, 0, 0, in, sizeof(in), DHCP_OFFER);
    if (r < 0) return false;
    const struct dhcp_hdr *off = (const struct dhcp_hdr *)in;
    uint32_t yiaddr = ntohl(off->yiaddr);
    uint8_t ol;
    const uint8_t *sid = dhcp_opt(in + sizeof(*off), r - sizeof(*off), DHCPOPT_SERVERID, &ol);
    uint32_t server_id = sid ? rd_be32(sid) : ntohl(off->siaddr);

    r = dhcp_xact(DHCP_REQUEST, yiaddr, server_id, in, sizeof(in), DHCP_ACK);
    if (r < 0) return false;
    const struct dhcp_hdr *ack = (const struct dhcp_hdr *)in;
    const uint8_t *mask = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_MASK,   &ol);
    const uint8_t *rtr  = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_ROUTER, &ol);
    const uint8_t *dns  = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_DNS,    &ol);

    g_netif.ip      = ntohl(ack->yiaddr);
    g_netif.netmask = mask ? rd_be32(mask) : IPV4(255, 255, 255, 0);
    g_netif.gateway = rtr  ? rd_be32(rtr)  : ((g_netif.ip & g_netif.netmask) | 2);
    g_netif.dns     = dns  ? rd_be32(dns)  : 0;
    g_netif.dhcp    = true;
    return true;
}

/* ---- DNS (RFC 1035): a minimal A-record resolver ------------------------ */
#define DNS_PORT     53
#define DNS_EPH_PORT 0xC000                  /* our source port (49152) */

struct dns_hdr {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

static uint16_t rd16be(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

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
        uint16_t type = rd16be(in + off);
        uint16_t rdlen = rd16be(in + off + 8);
        off += 10;
        if (off + rdlen > (uint32_t)r) return false;
        if (type == 1 && rdlen == 4) {
            *out_ip = IPV4(in[off], in[off + 1], in[off + 2], in[off + 3]);
            return true;
        }
        off += rdlen;
    }
    return false;
}

/* ---- bring-up ----------------------------------------------------------- */
#define IPQ(ip) (uint8_t)((ip) >> 24), (uint8_t)((ip) >> 16), (uint8_t)((ip) >> 8), (uint8_t)(ip)

static void net_rx_thread(void) {
    for (;;) { virtio_net_poll(); schedule(); }
}

void net_init(void) {
    memset(&g_netif, 0, sizeof(g_netif));
    if (!virtio_net_init(g_netif.mac)) {
        kprintf("net: no NIC -- networking disabled\n");
        return;
    }
    process_create_kthread(net_rx_thread, 0);   /* background RX poller */
    g_netif.up = true;

    /* Lease an address (M2). Fall back to the M1 static config so the stack is
     * still usable if no DHCP server answers. */
    if (net_dhcp()) {
        kprintf("net: DHCP lease  ip %u.%u.%u.%u  gw %u.%u.%u.%u  dns %u.%u.%u.%u\n",
                IPQ(g_netif.ip), IPQ(g_netif.gateway), IPQ(g_netif.dns));
    } else {
        g_netif.ip      = IPV4(10, 0, 2, 15);
        g_netif.netmask = IPV4(255, 255, 255, 0);
        g_netif.gateway = IPV4(10, 0, 2, 2);
        kprintf("net: DHCP failed -- static 10.0.2.15/24 gw 10.0.2.2\n");
    }
}
