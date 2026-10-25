#ifndef EMBLINK_POWER_POLICY_H
#define EMBLINK_POWER_POLICY_H
#include "../../user/lib/embk_policy.h"
void power_activity(void);
void power_policy_poll(void);
void power_policy_get(struct embk_policy *);
int power_policy_set(const struct embk_policy *);
void cpu_power_poll(void);
unsigned cpu_power_count(void);
void cpu_power_profile(unsigned);
#endif
