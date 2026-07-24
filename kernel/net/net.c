/* EmbLinkOS network stack -- core.
 *
 * This file owns only the shared interface state (g_netif), the internet
 * checksum, the Ethernet broadcast address, the RX demux that hands a frame up
 * by ethertype, and boot bring-up. Each protocol lives in its own file:
 *
 *   ethernet/eth.c   Ethernet framing (net_tx_eth)
 *   ethernet/arp.c   ARP
 *   ip/ipv4.c        IPv4            (ip/ipv6.c later)
 *   ip/icmp.c        ICMP + ping
 *   udp/udp.c        UDP + the receive capture
 *   dhcp/dhcp.c      DHCP client
 *   dns/dns.c        DNS resolver
 *   virtio_net.c     the NIC driver
 *
 * IPs are HOST order everywhere, converted (htonl/ntohl) only at the wire. The
 * stack is synchronous: ARP/DHCP/DNS/ping drive virtio_net_poll() while waiting,
 * so a caller does not depend on the background RX kthread being scheduled. */

#include "net/net.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"   /* process_create_kthread, schedule */

struct netif g_netif;

const uint8_t ETH_BCAST[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Internet checksum (RFC 1071 / BSD in_cksum): the result is stored directly
 * into a header's checksum field (byte order works out either way). */
uint16_t net_checksum(const void *data, uint32_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Transport checksum shared by UDP and TCP: pseudo-header (src, dst, proto, len)
 * folded with the segment bytes. Returns the raw folded ~sum. */
uint16_t net_l4_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                         const void *seg, uint32_t seg_len) {
    uint32_t sum = 0;
    uint32_t s = htonl(src_ip), d = htonl(dst_ip);
    const uint16_t *ph = (const uint16_t *)&s; sum += ph[0]; sum += ph[1];
    ph = (const uint16_t *)&d;                 sum += ph[0]; sum += ph[1];
    sum += htons(proto);
    sum += htons((uint16_t)seg_len);
    const uint16_t *p = (const uint16_t *)seg;
    uint32_t n = seg_len;
    while (n > 1) { sum += *p++; n -= 2; }
    if (n) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Driver -> stack: one received Ethernet frame, routed up by ethertype. */
void net_rx(const uint8_t *frame, uint32_t len) {
    if (len < ETH_HLEN) return;
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    uint16_t et = ntohs(e->ethertype);
    const uint8_t *payload = frame + ETH_HLEN;
    uint32_t plen = len - ETH_HLEN;
    if (et == ETH_P_ARP)     arp_input(payload, plen);
    else if (et == ETH_P_IP) ip_input(payload, plen);
}

/* ---- bring-up ----------------------------------------------------------- */
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
                IP_OCTETS(g_netif.ip), IP_OCTETS(g_netif.gateway), IP_OCTETS(g_netif.dns));
    } else {
        g_netif.ip      = IPV4(10, 0, 2, 15);
        g_netif.netmask = IPV4(255, 255, 255, 0);
        g_netif.gateway = IPV4(10, 0, 2, 2);
        kprintf("net: DHCP failed -- static 10.0.2.15/24 gw 10.0.2.2\n");
    }
}
