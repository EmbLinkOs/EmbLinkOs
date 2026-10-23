#ifndef EMBLINK_WIFI_AX200_H
#define EMBLINK_WIFI_AX200_H
#include <stdint.h>
#include "station.h"
enum ax200_state { AX200_ABSENT, AX200_MISSING_FIRMWARE, AX200_BAD_FIRMWARE,
    AX200_STARTING, AX200_FIRMWARE_ALIVE, AX200_NVM_READY, AX200_RADIO_READY, AX200_STOPPED };
struct ax200_status {
    enum ax200_state state;
    uint8_t mac[6], rfkill, tx_ant, rx_ant;
    uint32_t hardware_revision, last_error;
    uint32_t channel_flags[51];
};
/* Bootstraps only the discrete PCIe AX200 (8086:2723). CNVio AX201 and AX210
 * have different boot/power transports and are deliberately not matched. */
void ax200_probe(void);
void ax200_poll(void);
const struct ax200_status *ax200_get_status(void);
int ax200_scan_channel(uint8_t channel);
int ax200_prepare_bss(const struct wifi_bss *);
int ax200_associate_bss(const struct wifi_bss *);
int ax200_tx(const uint8_t *,unsigned);
int ax200_receive(uint8_t *,unsigned,uint8_t *channel);
int ax200_channels(uint8_t *,unsigned);
void ax200_leave(void);
#endif
