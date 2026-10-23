/* The NCR/AMD 53C9x "ESP": a sixteen-byte FIFO and a command register, with
 * the SCSI bus phases walked by hand. See the .c. */
#ifndef _EMBK_ESP_H_
#define _EMBK_ESP_H_
#include <stdint.h>
#include "include/types.h"
bool     esp_init(void);
bool     esp_present(void);
uint32_t esp_target_count(void);
#endif
