#ifndef EMBLINK_WIFI_PROFILE_H
#define EMBLINK_WIFI_PROFILE_H
#include <stdint.h>
struct wifi_profile {uint8_t ssid[32],ssid_len,pmk[32];};
/* Exact, bounded two-line format produced by tools/configure_wifi.py. No
 * passphrases are logged or kept in the connection daemon. */
int wifi_profile_parse(struct wifi_profile *,const uint8_t *,unsigned);
#endif
