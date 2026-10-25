#ifndef EMBK_GPU_INFO_H
#define EMBK_GPU_INFO_H
#include <stdint.h>
#define EMBK_GPU_VERSION 1
#define EMBK_GPU_SOFTWARE_RENDERING 1u
#define EMBK_GPU_DRIVER_MODESET 2u
struct embk_gpu_info {
    uint32_t version,flags,width,height,pitch,bpp;
    char driver[64];
};
#endif
