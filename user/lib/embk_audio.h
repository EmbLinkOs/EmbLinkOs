#ifndef EMBLINK_AUDIO_ABI_H
#define EMBLINK_AUDIO_ABI_H
#include <stdint.h>
#define EMBK_AUDIO_VERSION 1
#define EMBK_AUDIO_AVAILABLE 1u
#define EMBK_AUDIO_PLAYING 2u
#define EMBK_AUDIO_MUTED 4u
struct embk_audio_info {
    uint32_t version,flags,percent,sample_rate;
    char output[48];
};
#endif
