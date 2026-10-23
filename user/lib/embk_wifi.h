#ifndef EMBLINK_WIFI_ABI_H
#define EMBLINK_WIFI_ABI_H
#include <stdint.h>
#define EMBK_WIFI_VERSION 1
#define EMBK_WIFI_NETWORKS 24
#define EMBK_WIFI_SAVED 8
#define EMBK_WIFI_READY 1u
#define EMBK_WIFI_BLOCKED 2u
#define EMBK_WIFI_ADDRESS 4u
#define EMBK_WIFI_BUSY 8u
#define EMBK_WIFI_SCANNING 16u
#define EMBK_WIFI_AUTOCONNECT 32u
#define EMBK_WIFI_SEC_WPA2 1u
#define EMBK_WIFI_SEC_OTHER 2u
#define EMBK_WIFI_SEC_OPEN 3u
#define EMBK_WIFI_CONNECT 1u
#define EMBK_WIFI_DISCONNECT 2u
#define EMBK_WIFI_SCAN 3u
#define EMBK_WIFI_FORGET 4u
#define EMBK_WIFI_RECONNECT 5u
struct embk_wifi_network {
    uint8_t ssid[32],ssid_len,channel,security,reserved;
};
/* IPv4 fields use the same representation as the socket API. Secrets are
 * never returned. Scan entries describe the last scan; saved entries do not imply visibility. */
struct embk_wifi_info {
    uint32_t version,flags,radio,state,error;
    uint32_t ip,netmask,gateway,dns;
    uint32_t request_id,completed_id;
    int32_t request_error;
    uint64_t scanned_ms;
    uint8_t ssid[32],ssid_len,network_count,saved_count,reserved;
    struct embk_wifi_network networks[EMBK_WIFI_NETWORKS],saved[EMBK_WIFI_SAVED];
};
/* CONNECT: 8..63 printable ASCII passphrase or 64 hex PSK; RECONNECT selects
 * a saved SSID (zero length selects the current preference). FORGET names an
 * SSID. All other actions require zero length fields. Accepted requests are
 * asynchronous; completed_id/request_error report persistence failures too. */
struct embk_wifi_request {
    uint32_t version,action,ssid_len,secret_len;
    uint8_t ssid[32],secret[64];
};
#endif
