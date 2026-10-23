#ifndef EMBLINK_POWER_ABI_H
#define EMBLINK_POWER_ABI_H
#include <stdint.h>
#define EMBK_POWER_VERSION 1
#define EMBK_POWER_UNKNOWN UINT32_MAX
#define EMBK_POWER_BATTERY 1
#define EMBK_POWER_AC_KNOWN 2
#define EMBK_POWER_AC_ONLINE 4
#define EMBK_POWER_CHARGING 8
#define EMBK_POWER_DISCHARGING 16
#define EMBK_POWER_CRITICAL 32
#define EMBK_POWER_BUSY 64
#define EMBK_POWER_LID_KNOWN 128
#define EMBK_POWER_LID_CLOSED 256
#define EMBK_POWER_OFF 1
#define EMBK_POWER_REBOOT 2
/* Cached firmware snapshot; no AML runs in a UI polling syscall. Units:
 * unit 0=mWh/mW, unit 1=mAh/mA, voltage=mV; UINT32_MAX means unknown. */
struct embk_power_info {
    uint32_t version,flags,batteries,percent;
    uint64_t sampled_ms;
    uint32_t remaining,full,rate,voltage,unit;
    int32_t last_error;
};
#endif
