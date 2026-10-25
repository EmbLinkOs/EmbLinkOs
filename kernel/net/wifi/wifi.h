#ifndef EMBLINK_WIFI_H
#define EMBLINK_WIFI_H
#include <stdint.h>
#include "../../../user/lib/embk_wifi.h"
int wifi_init(uint8_t mac[6]);
void wifi_start_worker(void);
void wifi_poll(void); /* net_lock held */
int wifi_tx(const void *,uint32_t); /* net_lock held */
int wifi_command(const char *);
int wifi_snapshot(struct embk_wifi_info *);
int wifi_request(const struct embk_wifi_request *);
#endif
