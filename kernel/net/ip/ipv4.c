/* IPv4: datagram output (checksum, next-hop selection, broadcast) and input
 * (accept + demux to ICMP/UDP). IPv6 would sit beside this as ip/ipv6.c. */

#include "net/net.h"
#include "include/kstring.h"

/* Core output. `src_ip` may be 0 (unconfigured, e.g. DHCP DISCOVER). A dst of
 * 255.255.255.255 goes out as an Ethernet broadcast with no ARP; otherwise the
 * next hop is the host itself (on-subnet) or the default gateway. */
int ip_output(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
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
        memcpy(mac, ETH_BCAST, ETH_ALEN);
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

void ip_input(const uint8_t *pkt, uint32_t len) {
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
