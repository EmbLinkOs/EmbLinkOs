#ifndef EMBLINK_WIFI_CONTROL_H
#define EMBLINK_WIFI_CONTROL_H
#include "profile.h"
#include "../../../user/lib/embk_wifi.h"
#define WIFI_STORE_BYTES (8+65*EMBK_WIFI_SAVED)
struct wifi_saved_profiles {unsigned count;struct wifi_profile items[EMBK_WIFI_SAVED];};
int wifi_store_decode(struct wifi_saved_profiles *,const uint8_t *,unsigned);
unsigned wifi_store_encode(const struct wifi_saved_profiles *,uint8_t *);
int wifi_request_valid(const struct embk_wifi_request *);
/* Complete FCS-checked beacon/probe-response, returns a bounded public entry. */
int wifi_discovery_parse(struct embk_wifi_network *,const uint8_t *,unsigned,uint8_t);
#endif
