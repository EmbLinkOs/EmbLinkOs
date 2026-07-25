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
 * EMBKFS big lock (embkfs.c). It MUST be sleeping/sched-backed (a spinlock could
 * not survive the schedule() below) and OWNER-RECURSIVE, because a client
 * holding it drives virtio_net_poll(), which re-enters net_rx() on the same
 * thread. Every path runs under a public entry point, so wrapping just those is
 * enough -- the internal helpers inherit it.
 *
 * FINER HOLD (not a finer lock): the blocking client calls no longer HOLD the
 * lock while they wait. They hold it only for a state check and the atomic RX
 * drain, and net_yield() RELEASES it across schedule() -- so a process blocked
 * in accept()/recv() no longer camps the whole stack, and other connections make
 * progress on other cores. Same one lock, held for finer-grained intervals. The
 * lockstat below measures whether it is actually contended (the EMBKFS rule:
 * measure before splitting further). (net_tcp_abort still does NOT take it: it
 * runs from the fd reap path under g_sched_lock and only flips one bool.) */
static int               g_net_busy = 0;
static struct thread    *g_net_owner = 0;
static int               g_net_depth = 0;
static struct wait_queue g_net_wq;
static struct net_lockstat g_net_stat;

void net_lock(void) {
    sched_lock();
    if (g_net_busy && g_net_owner == current_thread) {
        g_net_depth++;
        g_net_stat.recursive++;
        sched_unlock();
        return;
    }
    g_net_stat.acquires++;
    if (g_net_busy) g_net_stat.contended++;      /* had to block -> real contention */
    while (g_net_busy) {
        sched_block_current_locked(&g_net_wq);   /* returns UNLOCKED */
        sched_lock();
    }
    g_net_busy = 1;
    g_net_owner = current_thread;
    g_net_depth = 1;
    sched_unlock();
}

/* One wait step for a blocking client: drain+deliver the RX ring UNDER the lock
 * (so no packet strands), then release the lock across schedule() so other cores
 * and connections progress, and reacquire before the caller re-checks state.
 * At the outermost hold (depth 1) this genuinely releases; a nested caller
 * (e.g. an ARP resolve inside a connect) stays held, which is brief and rare. */
void net_yield(void) {
    virtio_net_poll();     /* atomic drain under the held lock */
    net_unlock();          /* release across the yield -- the anti-camping move */
    schedule();
    net_lock();            /* reacquire; the caller re-checks state next iteration */
}

void net_lockstat_get(struct net_lockstat *out) { if (out) *out = g_net_stat; }
void net_lockstat_reset(void) {
    g_net_stat.acquires = 0; g_net_stat.recursive = 0; g_net_stat.contended = 0;
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
