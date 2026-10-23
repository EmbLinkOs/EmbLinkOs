#ifndef EMBK_POLICY_ABI_H
#define EMBK_POLICY_ABI_H
#include <stdint.h>
#define EMBK_POLICY_VERSION 1
#define EMBK_POLICY_LID_LOCK 1u
#define EMBK_POLICY_DIMMED 2u
#define EMBK_POLICY_HWP 4u
#define EMBK_PROFILE_BALANCED 0u
#define EMBK_PROFILE_EFFICIENCY 1u
#define EMBK_PROFILE_PERFORMANCE 2u
struct embk_policy {
    uint32_t version,flags,idle_dim_seconds,idle_lock_seconds;
    uint32_t profile,hwp_cpus;
    uint64_t last_activity_ms;
};
#endif
