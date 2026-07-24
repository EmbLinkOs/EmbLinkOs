/* DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK, over the UDP
 * capture (udp_arm/udp_collect). On a lease, configures g_netif and returns. */

#include "net/net.h"
#include "include/kstring.h"

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
        *o++ = DHCPOPT_REQIP;    *o++ = 4; net_ip_wr(req_ip, o);    o += 4;
        *o++ = DHCPOPT_SERVERID; *o++ = 4; net_ip_wr(server_id, o); o += 4;
    }
    *o++ = DHCPOPT_PARAMLIST; *o++ = 4;
    *o++ = DHCPOPT_MASK; *o++ = DHCPOPT_ROUTER; *o++ = DHCPOPT_DNS; *o++ = DHCPOPT_MSGTYPE;
    *o++ = DHCPOPT_END;
    return (int)(o - buf);
}

/* Send one DHCP message and capture the reply of the expected type. Returns the
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
    uint32_t server_id = sid ? net_ip_rd(sid) : ntohl(off->siaddr);

    r = dhcp_xact(DHCP_REQUEST, yiaddr, server_id, in, sizeof(in), DHCP_ACK);
    if (r < 0) return false;
    const struct dhcp_hdr *ack = (const struct dhcp_hdr *)in;
    const uint8_t *mask = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_MASK,   &ol);
    const uint8_t *rtr  = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_ROUTER, &ol);
    const uint8_t *dns  = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_DNS,    &ol);

    g_netif.ip      = ntohl(ack->yiaddr);
    g_netif.netmask = mask ? net_ip_rd(mask) : IPV4(255, 255, 255, 0);
    g_netif.gateway = rtr  ? net_ip_rd(rtr)  : ((g_netif.ip & g_netif.netmask) | 2);
    g_netif.dns     = dns  ? net_ip_rd(dns)  : 0;
    g_netif.dhcp    = true;
    return true;
}
