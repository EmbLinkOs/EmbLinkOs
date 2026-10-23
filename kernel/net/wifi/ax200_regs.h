#ifndef EMBLINK_WIFI_AX200_REGS_H
#define EMBLINK_WIFI_AX200_REGS_H
#include <stdint.h>
#include <stddef.h>
/* Minimal type/endianness vocabulary for the unchanged OpenBSD wire header.
 * This driver is x86-64 only. The IEEE frame type is a 24-byte wire layout,
 * not an OpenBSD networking object. */
#define IEEE80211_ADDR_LEN 6
#define ETHER_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
struct ieee80211_frame {
    uint8_t fc[2],duration[2],addr1[6],addr2[6],addr3[6],sequence[2];
} __attribute__((packed));
#ifndef __packed
#define __packed __attribute__((packed))
#define EMBLINK_DEFINED_PACKED
#endif
#define le32toh(v) ((uint32_t)(v))
static inline uint32_t le32_to_cpup(const uint32_t *p) {
    const uint8_t *b=(const uint8_t *)p;
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../third_party/iwx/if_iwxreg.h"
#pragma GCC diagnostic pop
#undef le32toh
#ifdef EMBLINK_DEFINED_PACKED
#undef __packed
#undef EMBLINK_DEFINED_PACKED
#endif
#undef IEEE80211_ADDR_LEN
#undef ETHER_ADDR_LEN
#undef IEEE80211_NWID_LEN
#endif
