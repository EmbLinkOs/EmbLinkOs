#ifndef _GPU_H_
#define _GPU_H_

#include <stdint.h>
#include "include/types.h"

// ---------------------------------------------------------------------------
// GPU abstraction. A display driver (Bochs/QEMU stdvga DISPI, VirtIO-GPU, ...)
// registers itself during gpu_init(); the framebuffer layer then asks it for
// the active video mode and calls the optional hooks below.
//
// Two scan-out models are supported:
//   * Linear framebuffer (VBE / Bochs DISPI): mode.address is the physical
//     VRAM aperture; the fb layer maps it and copies dirty rectangles into it.
//   * Guest-memory scan-out (VirtIO-GPU): mode.address is 0. The fb layer
//     hands its backbuffer to the driver via attach_backing(), and every
//     dirty rectangle is pushed with flush() (TRANSFER_TO_HOST_2D + FLUSH),
//     letting the host GPU do the composition.
// ---------------------------------------------------------------------------

struct gpu_mode {
    uint64_t phys_addr;   // physical LFB address, 0 = guest-memory scan-out
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       // bytes per scanline of the scan-out surface
    uint32_t bpp;
    uint32_t format;      // FB_FORMAT_RGB / FB_FORMAT_BGR
};

struct gpu_target {struct gpu_mode mode;uint32_t resource_id;};

// ---------------------------------------------------------------------------
// Several outputs. A driver that has them reports each one and shows a whole
// layout at once: one surface (the desktop, the bounding box of every enabled
// output) and, for each output, the rectangle of it that output scans out.
// See display/layout.h. Drivers without these hooks have one output, the
// whole surface, through select().
// ---------------------------------------------------------------------------
#define GPU_MAX_OUTPUTS   4
#define GPU_OUTPUT_MODES 16
struct gpu_output_mode { uint16_t width, height; uint32_t refresh_millihz; uint8_t preferred; };
struct gpu_output {
    char name[16];                      // the connector: "eDP-1", "HDMI-1", "Virtual-2"
    char model[32];                     // the monitor's EDID name, or ""
    char manufacturer[4];
    uint32_t width_mm, height_mm;
    uint8_t connected, builtin, has_edid;
    unsigned mode_count;
    struct gpu_output_mode modes[GPU_OUTPUT_MODES];   // the preferred mode first
};
struct gpu_output_cfg { uint8_t enabled; int32_t x, y; uint32_t width, height; };
struct gpu_layout { unsigned count, primary; struct gpu_output_cfg out[GPU_MAX_OUTPUTS]; };   // primary: an index into out
struct embk_monitor_info;

struct gpu_driver {
    const char *name;
    /* A prepared resource is not visible until select succeeds. release must
     * acknowledge that DMA has stopped before its backing can be freed. */
    bool (*prepare)(struct gpu_target *,void *,uint64_t,uint32_t,uint32_t);
    bool (*select)(const struct gpu_target *);
    bool (*release)(const struct gpu_target *);
    void (*describe)(struct embk_monitor_info *);
    uint32_t (*resource_id)(void);

    // Bring the display up (modeset) and describe the resulting mode.
    bool (*get_mode)(struct gpu_mode *out);

    // Optional: change resolution at runtime. Returns the new mode.
    bool (*set_mode)(uint32_t w, uint32_t h, struct gpu_mode *out);

    // Optional: called by fb_present() after the dirty rect reached the
    // scan-out surface. VirtIO uses this to run the accelerated host blit.
    void (*flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // Optional (guest-memory scan-out only): give the driver the backbuffer
    // so it can attach it as the scan-out resource's backing store.
    bool (*attach_backing)(void *buf, uint64_t size);

    // Optional accelerated 2D ops working directly on the scan-out surface.
    // Return false to make the fb layer fall back to software.
    bool (*fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t rgb);
    bool (*copy_rect)(uint32_t dst_x, uint32_t dst_y,
                      uint32_t src_x, uint32_t src_y,
                      uint32_t w, uint32_t h);

    // Optional: a CURSOR PLANE -- the display puts the pointer over the
    // scanout itself, so moving it composes nothing. cursor_image takes a
    // premultiplied 0xAARRGGBB picture (schedulable context); cursor_place
    // shows it on output `scanout` at (x, y) in that output's coordinates,
    // hides it, or with `image` re-reads the picture -- from ANY context,
    // the mouse interrupt included, and without waiting.
    bool (*cursor_image)(const uint32_t *argb, uint32_t w, uint32_t h, uint32_t hot_x, uint32_t hot_y);
    bool (*cursor_place)(unsigned scanout, int32_t x, int32_t y, bool visible, bool image);

    // Optional: the row pitch (bytes) a surface of this width needs. Absent:
    // width * 4. Intel planes need multiples of 64 bytes, which 1366 * 4 is not.
    uint32_t (*pitch)(uint32_t width);

    // Optional: several outputs (see gpu_output above). poll_outputs returns
    // 1 when an output was connected or disconnected since the last call.
    unsigned (*output_count)(void);
    bool (*output_info)(unsigned index, struct gpu_output *out);
    bool (*apply_layout)(const struct gpu_target *surface, const struct gpu_layout *layout);
    int  (*poll_outputs)(void);
    // How many outputs can be lit at once (a display engine's pipes); 0: as
    // many as it has.
    unsigned max_lit;

    // Optional: composing the desktop on the GPU (see gpu_compose below).
    // NULL when this device, or this host, cannot.
    const struct gpu_compose *(*compose)(void);
};

// ---------------------------------------------------------------------------
// GPU composition. The compositor draws a frame with these instead of the CPU
// primitives: images are window surfaces (and its own pictures: background,
// cursor, decorations), each backed by the pages the CPU already has, which
// the GPU re-reads only where told they changed. Drawing goes into the
// current scanout surface, clipped to the damage; end() shows it.
// All pixels are premultiplied BGRA unless said otherwise.
// ---------------------------------------------------------------------------
struct gpu_rect { int32_t x, y, w, h; };
struct gpu_draw {
    uint32_t image;
    struct gpu_rect src;        // in the image, pixels
    struct gpu_rect dst;        // on the screen; scaled when the sizes differ
    uint8_t  opaque;            // the image's alpha is ignored
    uint8_t  opacity;           // 255 = as it is
    uint8_t  radius;            // rounded corners of `frame` (0: square)
    uint8_t  ring;              // a border ring along `frame`, this wide (0: none)
    struct gpu_rect frame;      // the rounded rectangle coverage comes from
    uint32_t ring_rgb;
};
struct gpu_compose {
    const char *name;           // what composes: "host GPU (virgl)", "Intel blitter"
    // An image over `pixels` (w x h, rows `stride` bytes apart), uploaded whole.
    int  (*image_create)(void *pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t *id);
    void (*image_update)(uint32_t id, const struct gpu_rect *r);   // those pixels changed
    void (*image_destroy)(uint32_t id);
    int  (*begin)(const struct gpu_rect *damage);   // 0: drawing; clipped to damage
    void (*fill)(const struct gpu_rect *r, uint32_t rgb);
    // A soft shadow cast by a rounded frame: peak alpha at its edge, gone
    // `width` out, and none under `cover` (the window casting it; NULL: none).
    void (*shadow)(const struct gpu_rect *frame, const struct gpu_rect *cover, uint32_t radius,
                   uint32_t width, uint32_t peak);
    // Box blur of what is drawn in r, edges clamped to r as fb_blur_region's
    // are, laid back only inside `frame` rounded by `corner` (NULL: all of r).
    void (*blur)(const struct gpu_rect *r, uint32_t radius, const struct gpu_rect *frame, uint32_t corner);
    void (*draw)(const struct gpu_draw *d);
    int  (*end)(void);                              // 0: shown; else the GPU failed
    int  (*read)(const struct gpu_rect *r);         // the screen's pixels back into its backing
};

// Probe PCI for a supported display device and select the best driver.
// Must run after pci_init()/vmm_init() and before fb_init().
void gpu_init(void);

// Active driver, or NULL when running on the plain VBE framebuffer.
const struct gpu_driver *gpu_active(void);

// Individual driver probes (called by gpu_init in preference order).
const struct gpu_driver *virtio_gpu_probe(void);
const struct gpu_driver *bochs_gpu_probe(void);

#endif /* _GPU_H_ */
