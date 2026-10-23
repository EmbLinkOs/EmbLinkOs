#ifndef EMBLINK_NOTIFY_H
#define EMBLINK_NOTIFY_H
#include "../../user/lib/embk_notify.h"
struct process;
int notify_post(struct process *,const struct embk_notice_request *);
int notify_snapshot(struct process *,struct embk_notice_info *,int clear);
void notify_system(unsigned,const char *,const char *);
void notify_clear(void);
#endif
