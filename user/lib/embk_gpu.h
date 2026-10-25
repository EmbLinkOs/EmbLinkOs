#ifndef EMBK_GPU_INFO_H
#define EMBK_GPU_INFO_H
#include <stdint.h>
#define EMBK_GPU_VERSION 2
#define EMBK_GPU_SOFTWARE_RENDERING 1u   /* the CPU composes the desktop */
#define EMBK_GPU_DRIVER_MODESET 2u
#define EMBK_GPU_COMPOSED 4u             /* a GPU composes it: `renderer` says which */
struct embk_gpu_info {
    uint32_t version,flags,width,height,pitch,bpp;
    char driver[64];
    char renderer[48];                   /* "host GPU (virgl)", or "Software" */
};
#endif
