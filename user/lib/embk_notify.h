#ifndef EMBK_NOTIFY_ABI_H
#define EMBK_NOTIFY_ABI_H
#include <stdint.h>
#define EMBK_NOTIFY_VERSION 1
#define EMBK_NOTIFY_MAX 8
struct embk_notice_request {
    uint32_t version,severity; /* 0 information, 1 warning, 2 error */
    char title[48],body[160];
};
struct embk_notice {
    uint64_t sequence,created_ms;
    uint32_t source_pid,severity;
    char title[48],body[160];
};
struct embk_notice_info {
    uint32_t version,count;
    uint64_t latest;
    struct embk_notice entries[EMBK_NOTIFY_MAX]; /* newest first */
};
#endif
