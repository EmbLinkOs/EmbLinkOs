#ifndef EMBLINK_WIFI_WPA2_H
#define EMBLINK_WIFI_WPA2_H
#include <stdint.h>

#define WIFI_EAPOL_MAX 512
#define WIFI_RSN_MAX 257
enum wifi_wpa2_state {
    WIFI_WPA2_IDLE, WIFI_WPA2_M1, WIFI_WPA2_M3, WIFI_WPA2_M4,
    WIFI_WPA2_CONNECTED, WIFI_WPA2_FAILED
};
struct wifi_wpa2_keys {
    uint8_t pairwise, group, gtk_id;
    uint8_t tk[16], gtk[16];
    uint64_t group_rsc;
};
/* All callbacks and entry points run under the adapter/stack lock. Callbacks
 * must not synchronously deliver RX back into this state machine. TX returns
 * zero after accepting a copy of the packet; it never retains the pointer.
 * install is atomic and must preserve the other keys when pairwise/group=0.
 * It resets replay/packet numbers ONLY when the corresponding key changes.
 * port(1) is called only after successful key installation and M4 submission. */
struct wifi_wpa2_ops {
    int (*random)(void *, uint8_t *, unsigned);
    int (*send)(void *, const uint8_t *, unsigned, int protected_frame);
    int (*install)(void *, const struct wifi_wpa2_keys *);
    void (*port)(void *, int authorized);
};
struct wifi_wpa2 {
    struct wifi_wpa2_ops ops;
    void *arg;
    enum wifi_wpa2_state state;
    uint8_t sta[6], ap[6], pmk[32], ptk[48], snonce[32], anonce[32];
    uint8_t ap_rsn[WIFI_RSN_MAX];
    unsigned ap_rsn_len;
    uint8_t last_digest[20], reply[WIFI_EAPOL_MAX];
    unsigned reply_len, attempts;
    uint64_t replay, deadline;
    uint8_t have_replay, reply_protected, authorized;
};
extern const uint8_t wifi_wpa2_station_rsn[22];
/* ap_rsn is the complete IE from the selected beacon (ID, length, body).
 * The caller must obtain association before accepting EAPOL from this AP. */
int wifi_wpa2_start(struct wifi_wpa2 *, const struct wifi_wpa2_ops *, void *arg,
    const uint8_t sta[6], const uint8_t ap[6], const uint8_t pmk[32],
    const uint8_t *ap_rsn, unsigned ap_rsn_len, uint64_t now_ms);
/* 1=accepted (including a retransmission), 0=discarded, -1=local failure. */
int wifi_wpa2_rx(struct wifi_wpa2 *, const uint8_t *eapol, unsigned len, uint64_t now_ms);
void wifi_wpa2_tick(struct wifi_wpa2 *, uint64_t now_ms);
void wifi_wpa2_stop(struct wifi_wpa2 *);
#endif
