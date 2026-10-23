#ifndef EMBLINK_WIFI_STATION_H
#define EMBLINK_WIFI_STATION_H
#include <stdint.h>
#include "wpa2.h"
#include "ccmp.h"
#define WIFI_CHANNELS_MAX 64
enum wifi_station_state {
    WIFI_STA_DOWN, WIFI_STA_SCAN, WIFI_STA_AUTH, WIFI_STA_ASSOC,
    WIFI_STA_HANDSHAKE, WIFI_STA_UP, WIFI_STA_FAILED
};
enum wifi_station_error {
    WIFI_ERR_NONE, WIFI_ERR_RADIO, WIFI_ERR_NOT_FOUND, WIFI_ERR_TIMEOUT,
    WIFI_ERR_AUTH, WIFI_ERR_ASSOC, WIFI_ERR_SECURITY, WIFI_ERR_DISCONNECTED
};
struct wifi_bss {
    uint8_t ap[6], ssid[32], ssid_len, channel;
    uint8_t rates[12], rates_len, rsn[WIFI_RSN_MAX];
    unsigned rsn_len;
    uint16_t capabilities, beacon_interval, aid;
    uint64_t tsf;
    uint8_t dtim_period,dtim_count;
};
/* A radio supplies FCS-checked, complete MPDUs without FCS and performs MAC
 * ACK/retries. tx copies its buffer before returning, zero means queued.
 * channels must come from the adapter's regulatory/NVM data. tune is passive;
 * prepare must verify that transmission on the selected BSS channel is legal.
 * prepare creates the PHY/MAC/broadcast context; associate updates the AID and
 * AP station context. The radio must pass CCMP bytes through unchanged (no
 * hardware crypto). No callback may synchronously deliver RX into station_rx.
 * The caller serializes every entry point, including Ethernet TX, with its
 * network/adapter lock. ether/link callbacks must not wait for RX work. */
struct wifi_radio_ops {
    int (*tune)(void *,uint8_t channel);
    int (*prepare)(void *,const struct wifi_bss *);
    int (*associate)(void *,const struct wifi_bss *);
    int (*tx)(void *,const uint8_t *,unsigned);
    int (*random)(void *,uint8_t *,unsigned);
    void (*ether)(void *,const uint8_t *,unsigned);
    void (*link)(void *,int up);
};
struct wifi_station {
    struct wifi_radio_ops ops;
    void *arg;
    enum wifi_station_state state;
    enum wifi_station_error error;
    uint8_t mac[6], ssid[32], pmk[32], ssid_len;
    uint8_t channels[WIFI_CHANNELS_MAX], channel_count, channel_index, sweeps;
    uint16_t sequence;
    unsigned attempts;
    uint64_t deadline, now, last_rx;
    struct wifi_bss bss;
    struct wifi_wpa2 wpa;
    struct wifi_ccmp_key pair, group[4];
};
int wifi_station_init(struct wifi_station *,const struct wifi_radio_ops *,void *,const uint8_t mac[6]);
/* A join replaces the previous connection. SSIDs are binary, length 1..32.
 * PMK is obtained with wifi_wpa2_psk; the station never stores a passphrase. */
int wifi_station_join(struct wifi_station *,const uint8_t *ssid,unsigned ssid_len,
    const uint8_t pmk[32],const uint8_t *channels,unsigned channel_count,uint64_t now_ms);
void wifi_station_disconnect(struct wifi_station *);
void wifi_station_rx(struct wifi_station *,const uint8_t *,unsigned,uint8_t channel,uint64_t now_ms);
void wifi_station_tick(struct wifi_station *,uint64_t now_ms);
int wifi_station_tx(struct wifi_station *,const uint8_t *ethernet,unsigned len);
#endif
