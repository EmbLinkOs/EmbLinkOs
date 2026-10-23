/* DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK, over the UDP
 * capture (udp_arm/udp_collect). On a lease, configures g_netif and returns. */

#include "net/net.h"
#include "include/kstring.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u
static uint32_t xid=0x454d4231, next_xid=0x454d4231;
static uint64_t lease_until;
static uint32_t lease_seconds;

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NACK     6

#define DHCPOPT_MASK      1
#define DHCPOPT_ROUTER    3
#define DHCPOPT_DNS       6
#define DHCPOPT_REQIP     50
#define DHCPOPT_LEASE     51
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
    h->xid = htonl(xid);
    h->flags = htons(0x8000);                /* ask the server to broadcast the reply */
    memcpy(h->chaddr, g_netif.mac, ETH_ALEN);
    h->magic = htonl(DHCP_MAGIC);
    uint8_t *o = buf + sizeof(*h);
    *o++ = DHCPOPT_MSGTYPE; *o++ = 1; *o++ = type;
    if (type == DHCP_REQUEST) {
        *o++ = DHCPOPT_REQIP;    *o++ = 4; net_ip_wr(req_ip, o);    o += 4;
        *o++ = DHCPOPT_SERVERID; *o++ = 4; net_ip_wr(server_id, o); o += 4;
    }
    *o++ = DHCPOPT_PARAMLIST; *o++ = 5;
    *o++ = DHCPOPT_MASK; *o++ = DHCPOPT_ROUTER; *o++ = DHCPOPT_DNS; *o++ = DHCPOPT_MSGTYPE; *o++=DHCPOPT_LEASE;
    *o++ = DHCPOPT_END;
    return (int)(o - buf);
}

/* Send one DHCP message and capture the reply of the expected type. Returns the
 * reply length, or -1. `in` must hold >= sizeof(dhcp_hdr) + options. */
static int dhcp_xact(uint8_t type, uint32_t req_ip, uint32_t server_id,
                     uint8_t *in, uint32_t incap, uint8_t expect) {
    static uint8_t out[512];
    udp_arm_peer(DHCP_CLIENT_PORT,0,DHCP_SERVER_PORT);
    int n = dhcp_build(out, type, req_ip, server_id);
    if (net_send_udp(0, DHCP_CLIENT_PORT, 0xFFFFFFFFu, DHCP_SERVER_PORT, out, n) < 0) return -1;
    int r = udp_collect(in, incap, 0);
    if (r < (int)sizeof(struct dhcp_hdr)) return -1;
    const struct dhcp_hdr *h = (const struct dhcp_hdr *)in;
    if (h->op != 2 || h->htype!=1 || h->hlen!=6 || ntohl(h->xid)!=xid ||
        ntohl(h->magic)!=DHCP_MAGIC || memcmp(h->chaddr,g_netif.mac,6)) return -1;
    /* Validate the entire options list before any option reader uses a value.
     * In particular zero/short option lengths must not become four-byte IP
     * reads beyond the packet. END is required; zero padding after it is fine. */
    unsigned at=sizeof(*h),seen=0;int ended=0;
    while(at<(unsigned)r) {
        unsigned code=in[at++];if(!code) continue;
        if(code==255) {ended=1;break;}
        if(at==(unsigned)r) return -1;
        unsigned len=in[at++];if(len>(unsigned)r-at) return -1;
        unsigned bit=code==DHCPOPT_MSGTYPE?1:code==DHCPOPT_SERVERID?2:code==DHCPOPT_MASK?4:
            code==DHCPOPT_ROUTER?8:code==DHCPOPT_DNS?16:code==DHCPOPT_LEASE?32:0;
        if(bit && (seen&bit)) return -1;
        seen|=bit;
        if((code==DHCPOPT_MSGTYPE && len!=1) ||
           ((code==DHCPOPT_SERVERID || code==DHCPOPT_MASK || code==DHCPOPT_LEASE) && len!=4) ||
           ((code==DHCPOPT_ROUTER || code==DHCPOPT_DNS) && (!len || (len&3)))) return -1;
        at+=len;
    }
    if(!ended) return -1;
    uint8_t ol;
    const uint8_t *mt = dhcp_opt(in + sizeof(*h), r - sizeof(*h), DHCPOPT_MSGTYPE, &ol);
    if(mt && ol==1 && mt[0]==DHCP_NACK && expect==DHCP_ACK) {
        const uint8_t *sid=dhcp_opt(in+sizeof(*h),r-sizeof(*h),DHCPOPT_SERVERID,&ol);
        return sid && ol==4 && net_ip_rd(sid)==server_id?-2:-1;
    }
    if (!mt || ol!=1 || mt[0] != expect) return -1;
    return r;
}

static int unicast_ip(uint32_t ip) {
    return (ip>>24)!=0 && (ip>>24)!=127 && (ip>>24)<224 && ip!=UINT32_MAX;
}
uint64_t net_dhcp_renew_after_ms(void) {return lease_seconds==UINT32_MAX?UINT64_MAX:(uint64_t)lease_seconds*500;}
uint64_t net_dhcp_lease_remaining_ms(void) {
    uint64_t now=net_ticks()*10;
    return lease_until==UINT64_MAX?UINT64_MAX:lease_until>now?lease_until-now:0;
}
void net_dhcp_forget_lease(void) {lease_until=0;lease_seconds=0;}
bool net_dhcp(void) {
    static uint8_t in[1024];
    net_lock();
    net_service_lock();
    bool ok = false;
    for(unsigned attempt=0;attempt<3 && !ok && g_netif.up;attempt++) {
    xid=++next_xid^(uint32_t)net_ticks();
    int r = dhcp_xact(DHCP_DISCOVER, 0, 0, in, sizeof(in), DHCP_OFFER);
    if (r >= 0) {
        const struct dhcp_hdr *off = (const struct dhcp_hdr *)in;
        uint32_t yiaddr = ntohl(off->yiaddr);
        uint8_t ol;
        const uint8_t *sid = dhcp_opt(in + sizeof(*off), r - sizeof(*off), DHCPOPT_SERVERID, &ol);
        if(!sid || ol!=4 || !unicast_ip(yiaddr)) continue;
        uint32_t server_id=net_ip_rd(sid);if(!unicast_ip(server_id)) continue;

        r = dhcp_xact(DHCP_REQUEST, yiaddr, server_id, in, sizeof(in), DHCP_ACK);
        if(r==-2) {
            g_netif.dhcp=false;g_netif.ip=g_netif.netmask=g_netif.gateway=g_netif.dns=0;
            net_dhcp_forget_lease();arp_reset();continue;
        }
        if (r >= 0) {
            const struct dhcp_hdr *ack = (const struct dhcp_hdr *)in;
            sid=dhcp_opt(in+sizeof(*ack),r-sizeof(*ack),DHCPOPT_SERVERID,&ol);
            if(!g_netif.up || !sid || ol!=4 || net_ip_rd(sid)!=server_id || ntohl(ack->yiaddr)!=yiaddr) continue;
            const uint8_t *mask = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_MASK,   &ol);
            const uint8_t *rtr  = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_ROUTER, &ol);
            const uint8_t *dns  = dhcp_opt(in + sizeof(*ack), r - sizeof(*ack), DHCPOPT_DNS,    &ol);
            const uint8_t *lease=dhcp_opt(in+sizeof(*ack),r-sizeof(*ack),DHCPOPT_LEASE,&ol);
            uint32_t m=mask?net_ip_rd(mask):0,inv=~m,gw=rtr?net_ip_rd(rtr):0,ns=dns?net_ip_rd(dns):0;
            uint32_t seconds=lease?net_ip_rd(lease):3600;
            if(!m || !inv || (inv&(inv+1)) || !(yiaddr&inv) || (yiaddr&inv)==inv || !seconds ||
               (gw && (!unicast_ip(gw) || gw==yiaddr || (gw&m)!=(yiaddr&m))) || (ns && !unicast_ip(ns))) continue;
            if(g_netif.ip!=yiaddr || g_netif.netmask!=m || g_netif.gateway!=gw) arp_reset();
            g_netif.ip=yiaddr;g_netif.netmask=m;g_netif.gateway=gw;g_netif.dns=ns;
            g_netif.dhcp    = true;
            lease_seconds=seconds;lease_until=seconds==UINT32_MAX?UINT64_MAX:net_ticks()*10+(uint64_t)seconds*1000;
            ok = true;
        }
    }
    }
    net_service_unlock();
    net_unlock();
    return ok;
}
