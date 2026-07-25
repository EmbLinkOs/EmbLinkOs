/* ARP (IPv4 over Ethernet): a small IP->MAC cache, answering "who has us", and
 * resolving a next hop by broadcasting a request and polling for the reply. */

#include "net/net.h"
#include "include/kstring.h"
#include "process/process.h"   /* schedule */

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
    net_ip_wr(g_netif.ip, a.spa);
    memcpy(a.tha, target_mac, ETH_ALEN);
    net_ip_wr(target_ip, a.tpa);
    net_tx_eth(op == ARP_OP_REQUEST ? ETH_BCAST : target_mac, ETH_P_ARP, &a, sizeof(a));
}

void arp_input(const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt *)pkt;
    if (ntohs(a->ptype) != ETH_P_IP || a->plen != 4) return;

    uint32_t spa = net_ip_rd(a->spa);
    uint32_t tpa = net_ip_rd(a->tpa);
    arp_cache_put(spa, a->sha);                 /* learn the sender either way */

    if (ntohs(a->oper) == ARP_OP_REQUEST && tpa == g_netif.ip)
        arp_send(ARP_OP_REPLY, spa, a->sha);    /* answer "who has us" */
}

bool arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    net_lock();                                 /* recursive when called mid-send */
    bool ok = arp_cache_get(ip, mac_out);
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        arp_send(ARP_OP_REQUEST, ip, ETH_BCAST);
        for (int i = 0; i < 200000 && !ok; i++) {
            virtio_net_poll();                  /* drive RX while we wait */
            if (arp_cache_get(ip, mac_out)) { ok = true; break; }
            schedule();
        }
    }
    net_unlock();
    return ok;
}
