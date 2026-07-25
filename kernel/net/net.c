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

/* ---- the big net lock ---------------------------------------------------
 * One sleeping, owner-recursive lock serialising ALL shared net-stack state
 * (the TCBs, the ARP cache, the UDP/ping capture) across cores. Modelled on the
 * EMBKFS big lock (embkfs.c). It MUST be a sleeping, sched-backed lock, not a
 * spinlock, because the blocking client calls hold it across their poll +
 * schedule() loops; and it MUST be owner-recursive, because a client holding it
 * drives virtio_net_poll(), which re-enters net_rx() on the SAME thread. Every
 * path through the stack runs under a public entry point (net_rx, net_tcp_*,
 * net_ping, net_dhcp, net_resolve, arp_resolve), so wrapping just those is
 * enough -- the internal helpers inherit the lock and touch shared state safely.
 * Coarse on purpose (correct before fast): a client holds it for the whole
 * request, so two cores doing net serialise. A finer split is a later, measured
 * move -- but nothing races now. (net_tcp_abort deliberately does NOT take this:
 * it runs from the fd reap path under g_sched_lock and only flips one bool.) */
static int               g_net_busy = 0;
static struct thread    *g_net_owner = 0;
static int               g_net_depth = 0;
static struct wait_queue g_net_wq;

void net_lock(void) {
    sched_lock();
    if (g_net_busy && g_net_owner == current_thread) {
        g_net_depth++;
        sched_unlock();
        return;
    }
    while (g_net_busy) {
        sched_block_current_locked(&g_net_wq);   /* returns UNLOCKED */
        sched_lock();
    }
    g_net_busy = 1;
    g_net_owner = current_thread;
    g_net_depth = 1;
    sched_unlock();
}

void net_unlock(void) {
    sched_lock();
    if (--g_net_depth == 0) {
        g_net_busy = 0;
        g_net_owner = 0;
        wait_queue_wake_one(&g_net_wq);
    }
    sched_unlock();
}

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
    net_lock();                          /* protects arp cache / tcbs / capture */
    if (et == ETH_P_ARP)     arp_input(payload, plen);
    else if (et == ETH_P_IP) ip_input(payload, plen);
    net_unlock();
}

/* ---- bring-up ----------------------------------------------------------- */
static void net_rx_thread(void) {
    /* Hold net_lock across the drain so a packet is removed from the virtqueue
     * and delivered to the stack ATOMICALLY. Without this the kthread could pull
     * a packet off the ring (under the driver's rx spinlock) and THEN block on
     * net_lock because a client owns it -- stranding that packet while the client
     * polls the now-empty ring forever (a real -smp>1 deadlock). Every other
     * virtio_net_poll() caller is a client wait loop that already holds net_lock,
     * so only ONE thread ever drains at a time and it always delivers what it
     * took. Released before schedule() -- never held across the yield. */
    for (;;) {
        net_lock();
        virtio_net_poll();
        net_unlock();
        schedule();
    }
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
