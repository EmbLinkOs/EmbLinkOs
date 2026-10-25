#ifndef EMBLINK_UPDATE_SERVICE_H
#define EMBLINK_UPDATE_SERVICE_H
#include <stdint.h>
#include "../../user/lib/embk_update.h"
int update_service_init(void);
int update_service_control(unsigned,void *,uint64_t);
void update_service_health(int desktop_ready);
#endif
