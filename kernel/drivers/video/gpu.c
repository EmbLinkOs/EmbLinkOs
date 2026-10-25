#include "drivers/video/gpu.h"
#if defined(__x86_64__)
#include "drivers/video/intel_display.h"
#include "drivers/video/intel_capture.h"
#endif
#include "drivers/char/serial.h"

#include "include/kprintf.h"

static const struct gpu_driver *g_gpu = 0;

void gpu_init(void) {
    serial_write_string("\n=== GPU init ===\n");
#if defined(__x86_64__)
    /* Before any driver claims the device: the record has to show the
     * firmware's display exactly as it handed it over. */
    intel_capture_run();
#endif

    // Preference order: VirtIO-GPU (host-accelerated scan-out) first, then
    // the Bochs/QEMU stdvga DISPI interface (runtime modeset on the LFB).
    g_gpu = virtio_gpu_probe();
    if (!g_gpu) {
        g_gpu = bochs_gpu_probe();
    }
#if defined(__x86_64__)
    if (!g_gpu) g_gpu=intel_display_probe();
#endif

    if (g_gpu) {
        kprintf("GPU: using %s driver\n", g_gpu->name);
    } else {
        kprintf("GPU: no accelerated device found, using VBE framebuffer\n");
    }
}

const struct gpu_driver *gpu_active(void) {
    return g_gpu;
}
