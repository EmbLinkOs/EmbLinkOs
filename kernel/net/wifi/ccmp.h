#ifndef EMBLINK_WIFI_CCMP_H
#define EMBLINK_WIFI_CCMP_H
#include <stdint.h>
#define WIFI_MPDU_MAX 1600
#define WIFI_CCMP_PN_MAX UINT64_C(0xffffffffffff)
struct wifi_ccmp_key {
    uint8_t tk[16], id, installed;
    uint64_t tx_pn, rx_pn[17]; /* QoS TIDs 0..15 and a separate non-QoS queue */
};
/* Reinstalling an identical key preserves every PN. A different key starts
 * a fresh TX counter and initializes RX replay counters from the EAPOL RSC. */
int wifi_ccmp_install(struct wifi_ccmp_key *, const uint8_t tk[16], unsigned id, uint64_t rsc);
void wifi_ccmp_clear(struct wifi_ccmp_key *);
/* Input/output are complete 802.11 data MPDUs, without FCS. Output must be
 * separate from input. Supports infrastructure data/QoS data, no fragments,
 * A-MSDU, HT control or four-address WDS. Failure never exposes plaintext or
 * advances RX replay state. Seal consumes a PN even when the radio drops TX. */
int wifi_ccmp_seal(struct wifi_ccmp_key *,const uint8_t *,unsigned,uint8_t *,unsigned);
int wifi_ccmp_open(struct wifi_ccmp_key *,const uint8_t *,unsigned,uint8_t *,unsigned);
#endif
