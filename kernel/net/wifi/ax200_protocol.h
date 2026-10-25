#ifndef EMBLINK_WIFI_AX200_PROTOCOL_H
#define EMBLINK_WIFI_AX200_PROTOCOL_H
#include <stdint.h>
#define AX200_FW_MAX (2U*1024*1024)
#define AX200_FW_SECTIONS 64
#define AX200_FW_COMMANDS 256
struct ax200_fw_section {const uint8_t *data;unsigned length;};
struct ax200_fw {
    struct ax200_fw_section sections[3][AX200_FW_SECTIONS];
    unsigned count[3]; /* LMAC, UMAC, paging; payload lifetime belongs to caller */
    uint32_t phy, api[4], capabilities[8];
    uint8_t commands[AX200_FW_COMMANDS][4];
    unsigned command_count;
};
int ax200_fw_parse(struct ax200_fw *,const uint8_t *,unsigned);
unsigned ax200_fw_command_version(const struct ax200_fw *,unsigned group,unsigned command,int notification);
/* Validate and walk one DMA receive buffer. Each record has its four-byte
 * length prefix removed, leaving a command header and bounded payload.
 * Returning -1 means a malformed hardware record; earlier records may already
 * have been delivered. The transport stops the device in that case. */
int ax200_rx_records(const uint8_t *,unsigned,int (*record)(void *,const uint8_t *,unsigned),void *);
#endif
