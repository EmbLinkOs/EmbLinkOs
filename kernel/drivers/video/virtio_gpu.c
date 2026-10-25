// VirtIO-GPU 2D display driver (QEMU -device virtio-vga / virtio-gpu-pci).
//
// The framebuffer lives in guest RAM (the fb layer's backbuffer) and is
// attached to a host-side GPU resource. Presenting a dirty rectangle issues
// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH on the control virtqueue, and the host
// GPU blits/composites it — that is the acceleration path.
//
// Transport: virtio 1.x "modern" PCI — vendor capabilities point at the
// common/notify/isr/device config windows inside the device's BARs, and the
// control queue is a split virtqueue polled synchronously.

#include "drivers/video/gpu.h"
#include "display/edid.h"
#include "../../../user/lib/embk_monitor.h"
#include "include/kmalloc.h"
#include "drivers/video/framebuffer.h"
#include "drivers/bus/pci.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/char/serial.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/spinlock.h"
#if defined(__x86_64__)
#include "arch/x86_64/boot/fw_cfg.h"
#endif

// ---- virtio PCI capability layout ------------------------------------------
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

// Common config offsets (little-endian MMIO).
#define VC_DEVICE_FEATURE_SELECT 0x00
#define VC_DEVICE_FEATURE        0x04
#define VC_DRIVER_FEATURE_SELECT 0x08
#define VC_DRIVER_FEATURE        0x0C
#define VC_MSIX_CONFIG           0x10
#define VC_NUM_QUEUES            0x12
#define VC_DEVICE_STATUS         0x14
#define VC_CONFIG_GENERATION     0x15
#define VC_QUEUE_SELECT          0x16
#define VC_QUEUE_SIZE            0x18
#define VC_QUEUE_MSIX_VECTOR     0x1A
#define VC_QUEUE_ENABLE          0x1C
#define VC_QUEUE_NOTIFY_OFF      0x1E
#define VC_QUEUE_DESC            0x20
#define VC_QUEUE_DRIVER          0x28
#define VC_QUEUE_DEVICE          0x30

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1_BANK 1        // feature bit 32 lives in bank 1 ...
#define VIRTIO_F_VERSION_1_BIT  (1U << 0) // ... as bit 0

// ---- split virtqueue --------------------------------------------------------
#define VQ_SIZE 64

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* struct vring_desc and struct vring_used_elem now come from virtio_pci.h --
 * they are the spec's layouts and belong with the transport. The two whose
 * ring[] is sized by VQ_SIZE stay here, because a queue depth is this driver's
 * choice. */
struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VQ_SIZE];
} __attribute__((packed));

// ---- virtio-gpu protocol ----------------------------------------------------
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2   // memory bytes B,G,R,X

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

// ---- driver state -----------------------------------------------------------
#define VGPU_SCANOUT_RES_ID 1
// Command staging area: attach_backing for an 8 MB framebuffer needs up to
// 2048 mem entries (32 KB) plus the header.
#define VGPU_CMD_BUF_SIZE (160 * 1024)

struct virtio_gpu_state {
    struct virtio_pci_dev vp;   /* the shared transport: windows + handshake */
    bool present;
    volatile uint8_t *common;         // common config window
    volatile uint8_t *notify_base;    // notify window base
    uint32_t notify_off_multiplier;
    uint16_t queue_notify_off;        // controlq notify offset
    uint16_t vq_size;                 // negotiated queue size
    uint16_t last_used_idx;
    uint16_t free_head;               // next free desc (simple 2-desc usage)

    uint32_t width, height;
    uint32_t active_resource,next_resource;
    bool failed,edid_supported;
    struct display_identity identity;
    bool scanout_ready;

    /* Every head: what the host says (or a headless test forces), its EDID. */
    unsigned num_scanouts, forced_heads;
    bool polled_once;
    struct {
        bool connected, has_identity;
        uint32_t width, height;
        struct display_identity identity;
    } heads[GPU_MAX_OUTPUTS];

    struct vring_desc  desc[VQ_SIZE]  __attribute__((aligned(4096)));
    struct vring_avail avail          __attribute__((aligned(4096)));
    struct vring_used  used           __attribute__((aligned(4096)));

    uint8_t cmd_buf[VGPU_CMD_BUF_SIZE] __attribute__((aligned(64)));
    uint8_t resp_buf[4096]             __attribute__((aligned(64)));
};

static struct virtio_gpu_state g_vgpu;

// ---- MMIO helpers -----------------------------------------------------------
static inline uint8_t  vr8(volatile uint8_t *b, uint32_t o)  { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t vr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void vw8(volatile uint8_t *b, uint32_t o, uint8_t v)   { *(volatile uint8_t  *)(b + o) = v; }
static inline void vw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void vw64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    vw32(b, o, (uint32_t)v);
    vw32(b, o + 4, (uint32_t)(v >> 32));
}

static inline uint64_t vgpu_dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

// ---- synchronous command execution -------------------------------------------
// Places cmd (device-readable) + resp (device-writable) as a two-descriptor
// chain, notifies the device, and busy-waits for the used-ring entry.
/* One submitter at a time: cmd_buf/resp_buf, the descriptor+avail rings and
 * last_used_idx are all SHARED single-slot state. Under SMP, one core painting
 * (compositor -> fb -> flush) while another kprintf'd (console -> fb -> flush)
 * submitted concurrently, desynced last_used_idx from the device and every
 * later command spun its full timeout -- the "OS just hangs" under -smp 4.
 * NOTE the error paths must NOT kprintf while holding this lock: kprintf's
 * console flush re-enters this very function -> instant self-deadlock. */
static spinlock_t vgpu_lock = SPINLOCK_INIT;

static bool vgpu_submit(const void *cmd, uint32_t cmd_len,
                        void *resp, uint32_t resp_len,
                        uint32_t expect_type) {
    struct virtio_gpu_state *s = &g_vgpu;
    int fail = 0;   /* 0 ok, 1 bad response type, 2 timeout */
    uint32_t bad_cmd = 0, bad_resp = 0;

    spin_lock(&vgpu_lock);
    if(s->failed || cmd_len>sizeof s->cmd_buf || resp_len>sizeof s->resp_buf) {spin_unlock(&vgpu_lock);return false;}

    if (cmd != s->cmd_buf) memcpy(s->cmd_buf, cmd, cmd_len);
    memset(s->resp_buf, 0, resp_len < sizeof(s->resp_buf) ? resp_len : sizeof(s->resp_buf));

    s->desc[0].addr  = vgpu_dma(s->cmd_buf);
    s->desc[0].len   = cmd_len;
    s->desc[0].flags = VRING_DESC_F_NEXT;
    s->desc[0].next  = 1;
    s->desc[1].addr  = vgpu_dma(s->resp_buf);
    s->desc[1].len   = resp_len;
    s->desc[1].flags = VRING_DESC_F_WRITE;
    s->desc[1].next  = 0;

    uint16_t slot = (uint16_t)(s->avail.idx % s->vq_size);
    s->avail.ring[slot] = 0;
    __sync_synchronize();
    s->avail.idx++;
    __sync_synchronize();

    // Notify the control queue.
    vw16((volatile uint8_t *)s->notify_base,
         (uint32_t)s->queue_notify_off * s->notify_off_multiplier, 0);

    // Busy-wait for completion (control commands are quick on QEMU).
    fail = 2;
    for (uint32_t spins = 0; spins < 200000000U; spins++) {
        __sync_synchronize();
        if (s->used.idx != s->last_used_idx) {
            s->last_used_idx = s->used.idx;
            struct virtio_gpu_ctrl_hdr *h = (struct virtio_gpu_ctrl_hdr *)s->resp_buf;
            if (h->type != expect_type) {
                fail = 1;
                bad_cmd  = ((const struct virtio_gpu_ctrl_hdr *)s->cmd_buf)->type;
                bad_resp = h->type;
            } else {
                fail = 0;
                if (resp && resp != s->resp_buf) memcpy(resp, s->resp_buf, resp_len);
            }
            break;
        }
    }

    if(fail==2)s->failed=true;
    spin_unlock(&vgpu_lock);

    /* report OUTSIDE the lock (kprintf re-enters this path via the console) */
    if (fail == 1)
        kprintf("virtio-gpu: cmd %x -> unexpected resp %x\n",
                (unsigned int)bad_cmd, (unsigned int)bad_resp);
    else if (fail == 2)
        kprintf("virtio-gpu: command timeout\n");
    return fail == 0;
}

static bool vgpu_simple_cmd(const void *cmd, uint32_t cmd_len) {
    struct virtio_gpu_ctrl_hdr resp;
    return vgpu_submit(cmd, cmd_len, &resp, sizeof(resp),
                       VIRTIO_GPU_RESP_OK_NODATA);
}

// ---- transport bring-up -------------------------------------------------------
static bool vgpu_init_transport(const struct pci_device *dev) {
    struct virtio_gpu_state *s = &g_vgpu;

    /* The capability walk, the config windows, bus mastering and the status
     * handshake are drivers/bus/virtio_pci.c's now -- this driver carried the
     * second of what became four near-identical copies.
     *
     * No bank-0 features are requested: this driver uses the 2D control queue
     * only, and every optional virtio-gpu feature (virgl, EDID, resource UUIDs)
     * would add an obligation it does not meet. */
    uint32_t features=0;
    if (!virtio_pci_attach(&s->vp, dev, "virtio-gpu", 1u<<1, &features))
        return false;

    s->edid_supported=(features&(1u<<1))!=0;
    s->active_resource=1;s->next_resource=2;
    s->common      = s->vp.common;
    s->notify_base = s->vp.notify;
    s->notify_off_multiplier = s->vp.notify_multiplier;

    // Control queue (index 0).
    s->last_used_idx = 0;
    memset(s->desc, 0, sizeof(s->desc));
    memset((void *)&s->avail, 0, sizeof(s->avail));
    memset((void *)&s->used, 0, sizeof(s->used));

    uint16_t qsize = virtio_pci_setup_queue(&s->vp, 0, VQ_SIZE, s->desc,
                                            (void *)&s->avail, (void *)&s->used,
                                            &s->queue_notify_off);
    if (qsize == 0) {
        kprintf("virtio-gpu: controlq missing\n");
        return false;
    }
    s->vq_size = qsize;

    virtio_pci_driver_ok(&s->vp);

    kprintf("virtio-gpu: transport up, controlq size=%u notify_mult=%u\n",
            (unsigned int)qsize, (unsigned int)s->notify_off_multiplier);
    return true;
}

// ---- gpu_driver hooks ----------------------------------------------------------
static bool vgpu_get_mode(struct gpu_mode *out) {
    struct virtio_gpu_state *s = &g_vgpu;

    // Ask the host for the preferred scanout geometry.
    struct virtio_gpu_ctrl_hdr cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    struct virtio_gpu_resp_display_info info;
    s->width = 1024;
    s->height = 768;
    if (vgpu_submit(&cmd, sizeof(cmd), &info, sizeof(info),
                    VIRTIO_GPU_RESP_OK_DISPLAY_INFO)) {
        if (info.pmodes[0].enabled &&
            info.pmodes[0].r.width >= 320 && info.pmodes[0].r.height >= 200) {
            s->width  = info.pmodes[0].r.width;
            s->height = info.pmodes[0].r.height;
        }
    }
#if defined(EMBK_VGPU_CAP_W) && defined(EMBK_VGPU_CAP_H)
    /* Dev cap: some host display backends (e.g. -display gtk) report the
     * window/monitor geometry here and ignore the -device virtio-vga xres/yres
     * hint, so the VM window opens huge. Cap it in-guest. virtio-gpu is a
     * VM-only device (real hardware uses UEFI GOP / bochs / VBE), so this never
     * touches a physical machine. Set via FB_W/FB_H in the Makefile. */
    if (s->width  > (EMBK_VGPU_CAP_W)) s->width  = (EMBK_VGPU_CAP_W);
    if (s->height > (EMBK_VGPU_CAP_H)) s->height = (EMBK_VGPU_CAP_H);
#endif
    kprintf("virtio-gpu: scanout %ux%u\n",
            (unsigned int)s->width, (unsigned int)s->height);

    // Create the 2D resource the backbuffer will be attached to.
    struct virtio_gpu_resource_create_2d c2d;
    memset(&c2d, 0, sizeof(c2d));
    c2d.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    c2d.resource_id = VGPU_SCANOUT_RES_ID;
    c2d.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    c2d.width       = s->width;
    c2d.height      = s->height;
    if (!vgpu_simple_cmd(&c2d, sizeof(c2d))) {
        kprintf("virtio-gpu: RESOURCE_CREATE_2D failed\n");
        return false;
    }

    out->phys_addr = 0;   // guest-memory scan-out: fb layer must attach_backing
    out->width  = s->width;
    out->height = s->height;
    out->pitch  = s->width * 4;
    out->bpp    = 32;
    out->format = FB_FORMAT_BGR;
    return true;
}

static bool vgpu_backing(uint32_t resource,void *buf, uint64_t size) {
    // Build the scatter list: walk the buffer page by page, translating to
    // physical addresses and coalescing contiguous runs.
    uint8_t *command=kmalloc(VGPU_CMD_BUF_SIZE);if(!command)return false;
    struct virtio_gpu_resource_attach_backing *ab =
        (struct virtio_gpu_resource_attach_backing *)command;
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *)(command + sizeof(*ab));
    uint32_t max_entries =
        (VGPU_CMD_BUF_SIZE - sizeof(*ab)) / sizeof(*entries);

    memset(ab, 0, sizeof(*ab));
    ab->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab->resource_id = resource;

    uint32_t n = 0;
    uint64_t va = (uint64_t)(uintptr_t)buf;
    uint64_t end = va + size;
    while (va < end) {
        uint64_t phys = vmm_get_phys(va);
        if (!phys) {
            kprintf("virtio-gpu: backbuffer page unmapped at %p\n",
                    (void *)(uintptr_t)va);
            kfree(command);return false;
        }
        uint64_t chunk = PAGE_SIZE;
        if (va + chunk > end) chunk = end - va;

        if (n > 0 &&
            entries[n - 1].addr + entries[n - 1].length == phys) {
            entries[n - 1].length += (uint32_t)chunk;   // coalesce
        } else {
            if (n >= max_entries) {
                kprintf("virtio-gpu: too many backing entries\n");
                kfree(command);return false;
            }
            entries[n].addr = phys;
            entries[n].length = (uint32_t)chunk;
            entries[n].padding = 0;
            n++;
        }
        va += chunk;
    }
    ab->nr_entries = n;

    uint32_t cmd_len = sizeof(*ab) + n * sizeof(*entries);
    struct virtio_gpu_ctrl_hdr resp;
    if (!vgpu_submit(command, cmd_len, &resp, sizeof(resp),
                     VIRTIO_GPU_RESP_OK_NODATA)) {
        kprintf("virtio-gpu: ATTACH_BACKING failed\n");
        kfree(command);return false;
    }
    kfree(command);
    kprintf("virtio-gpu: backing attached (%u entr%s)\n",
            (unsigned int)n, n == 1 ? "y" : "ies");

    return true;
}

static bool vgpu_attach_backing(void *buf,uint64_t size) {
    struct virtio_gpu_state *s=&g_vgpu;
    if(!vgpu_backing(1,buf,size))return false;
    // Point scanout 0 at the resource.
    struct virtio_gpu_set_scanout sc;
    memset(&sc, 0, sizeof(sc));
    sc.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    sc.r.width     = s->width;
    sc.r.height    = s->height;
    sc.scanout_id  = 0;
    sc.resource_id = VGPU_SCANOUT_RES_ID;
    if (!vgpu_simple_cmd(&sc, sizeof(sc))) {
        kprintf("virtio-gpu: SET_SCANOUT failed\n");
        return false;
    }

    s->scanout_ready = true;
    return true;
}

static void vgpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct virtio_gpu_state *s = &g_vgpu;
    if (!s->scanout_ready || w == 0 || h == 0) return;

    // Host reads the dirty rect out of guest memory ...
    struct virtio_gpu_transfer_to_host_2d t;
    memset(&t, 0, sizeof(t));
    t.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t.r.x = x; t.r.y = y; t.r.width = w; t.r.height = h;
    t.offset      = (uint64_t)y * (s->width * 4) + (uint64_t)x * 4;
    t.resource_id = s->active_resource;
    if (!vgpu_simple_cmd(&t, sizeof(t))) return;

    // ... and flushes it to the display.
    struct virtio_gpu_resource_flush f;
    memset(&f, 0, sizeof(f));
    f.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    f.r.x = x; f.r.y = y; f.r.width = w; f.r.height = h;
    f.resource_id = s->active_resource;
    vgpu_simple_cmd(&f, sizeof(f));
}

static bool vgpu_prepare(struct gpu_target *out,void *buf,uint64_t bytes,uint32_t w,uint32_t h) {
    struct virtio_gpu_state *s=&g_vgpu;if(s->failed || bytes!=(uint64_t)w*h*4 || s->next_resource==UINT32_MAX)return false;
    uint32_t id=s->next_resource++;
    struct virtio_gpu_resource_create_2d create={0};create.hdr.type=VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id=id;create.format=VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;create.width=w;create.height=h;
    if(!vgpu_simple_cmd(&create,sizeof create))return false;
    out->resource_id=id;out->mode=(struct gpu_mode){0,w,h,w*4,32,FB_FORMAT_BGR};
    return vgpu_backing(id,buf,bytes);
}
static bool vgpu_select(const struct gpu_target *target) {
    struct virtio_gpu_set_scanout sc={0};sc.hdr.type=VIRTIO_GPU_CMD_SET_SCANOUT;
    sc.r.width=target->mode.width;sc.r.height=target->mode.height;sc.resource_id=target->resource_id;
    if(!vgpu_simple_cmd(&sc,sizeof sc))return false;
    g_vgpu.width=target->mode.width;g_vgpu.height=target->mode.height;
    g_vgpu.active_resource=target->resource_id;return true;
}
static bool vgpu_release(const struct gpu_target *target) {
    if(!target->resource_id)return true;
    struct {struct virtio_gpu_ctrl_hdr header;uint32_t id,padding;} unref={0};
    unref.header.type=VIRTIO_GPU_CMD_RESOURCE_UNREF;unref.id=target->resource_id;
    return vgpu_simple_cmd(&unref,sizeof unref);
}
static uint32_t vgpu_resource(void) {return g_vgpu.active_resource;}
static void vgpu_describe(struct embk_monitor_info *info) {
    struct virtio_gpu_state *s=&g_vgpu;info->flags|=EMBK_MONITOR_CONNECTED;
    if(!s->failed && s->scanout_ready)info->flags|=EMBK_MONITOR_MODESET;
    display_identity_describe(&s->identity,info);
}
static void vgpu_read_edid(void) {
    struct virtio_gpu_state *s=&g_vgpu;if(!s->edid_supported)return;
    struct {struct virtio_gpu_ctrl_hdr header;uint32_t scanout,padding;} request={0};
    struct {struct virtio_gpu_ctrl_hdr header;uint32_t size,padding;uint8_t bytes[1024];} response;
    request.header.type=0x010a;
    if(vgpu_submit(&request,sizeof request,&response,sizeof response,0x1104) && response.size<=1024)
        (void)display_edid_parse(response.bytes,response.size,&s->identity);
}

/* ---- several heads -------------------------------------------------------
 * virtio-gpu config space (virtio 1.2, 5.7.4): events_read, events_clear,
 * num_scanouts. VIRTIO_GPU_EVENT_DISPLAY in events_read says the host's
 * display configuration changed -- a head was connected or resized. */
#define VGPU_CFG_EVENTS_READ  0x0
#define VGPU_CFG_EVENTS_CLEAR 0x4
#define VGPU_CFG_NUM_SCANOUTS 0x8
#define VGPU_EVENT_DISPLAY    1u

static unsigned vgpu_heads(void) {
    unsigned n = g_vgpu.num_scanouts;
    return n > GPU_MAX_OUTPUTS ? GPU_MAX_OUTPUTS : n;
}

static void vgpu_read_heads(void) {
    struct virtio_gpu_state *s = &g_vgpu;
    struct virtio_gpu_ctrl_hdr cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    struct virtio_gpu_resp_display_info info;
    if (!vgpu_submit(&cmd, sizeof cmd, &info, sizeof info, VIRTIO_GPU_RESP_OK_DISPLAY_INFO)) return;
    for (unsigned i = 0; i < vgpu_heads(); i++) {
        int on = info.pmodes[i].enabled || i < s->forced_heads || i == 0;
        uint32_t w = info.pmodes[i].r.width, h = info.pmodes[i].r.height;
        if (w < 320 || h < 200) { w = 1024; h = 768; }
#if defined(EMBK_VGPU_CAP_W) && defined(EMBK_VGPU_CAP_H)
        if (w > (EMBK_VGPU_CAP_W)) w = (EMBK_VGPU_CAP_W);
        if (h > (EMBK_VGPU_CAP_H)) h = (EMBK_VGPU_CAP_H);
#endif
        s->heads[i].connected = on;
        s->heads[i].width = w;
        s->heads[i].height = h;
        if (s->edid_supported) {
            struct {struct virtio_gpu_ctrl_hdr header;uint32_t scanout,padding;} request={0};
            struct {struct virtio_gpu_ctrl_hdr header;uint32_t size,padding;uint8_t bytes[1024];} response;
            request.header.type=0x010a;request.scanout=i;
            s->heads[i].has_identity = vgpu_submit(&request,sizeof request,&response,sizeof response,0x1104) &&
                response.size<=1024 && !display_edid_parse(response.bytes,response.size,&s->heads[i].identity);
        }
    }
}

static unsigned vgpu_output_count(void) { return vgpu_heads(); }

static bool vgpu_output_info(unsigned i, struct gpu_output *o) {
    struct virtio_gpu_state *s = &g_vgpu;
    if (i >= vgpu_heads()) return false;
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "Virtual-%u", i + 1);
    o->connected = s->heads[i].connected;
    if (s->heads[i].has_identity) {
        o->has_edid = 1;
        strncpy(o->model, s->heads[i].identity.name, sizeof o->model - 1);
        memcpy(o->manufacturer, s->heads[i].identity.manufacturer, 4);
        o->width_mm = s->heads[i].identity.width_mm;
        o->height_mm = s->heads[i].identity.height_mm;
    }
    /* A virtual head shows any size: its preferred one first, then the usual. */
    static const uint16_t std[][2] = { {800,600},{1024,768},{1280,720},{1280,800},{1280,1024},{1440,900},
                                       {1600,900},{1680,1050},{1920,1080},{1920,1200},{2560,1440} };
    o->modes[o->mode_count++] = (struct gpu_output_mode){ (uint16_t)s->heads[i].width, (uint16_t)s->heads[i].height, 60000, 1 };
    for (unsigned k = 0; k < sizeof std / sizeof std[0] && o->mode_count < GPU_OUTPUT_MODES; k++)
        if (std[k][0] != s->heads[i].width || std[k][1] != s->heads[i].height)
            o->modes[o->mode_count++] = (struct gpu_output_mode){ std[k][0], std[k][1], 60000, 0 };
    return true;
}

static bool vgpu_apply_layout(const struct gpu_target *surface, const struct gpu_layout *l) {
    struct virtio_gpu_state *s = &g_vgpu;
    for (unsigned i = 0; i < vgpu_heads(); i++) {
        struct virtio_gpu_set_scanout sc;
        memset(&sc, 0, sizeof sc);
        sc.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        sc.scanout_id = i;
        if (i < l->count && l->out[i].enabled) {
            sc.r.x = (uint32_t)l->out[i].x; sc.r.y = (uint32_t)l->out[i].y;
            sc.r.width = l->out[i].width; sc.r.height = l->out[i].height;
            sc.resource_id = surface->resource_id;
        }                                      /* else resource 0: the head is switched off */
        if (!vgpu_simple_cmd(&sc, sizeof sc)) return false;
    }
    s->width = surface->mode.width;
    s->height = surface->mode.height;
    s->active_resource = surface->resource_id;
    s->scanout_ready = true;
    return true;
}

static int vgpu_poll_outputs(void) {
    struct virtio_gpu_state *s = &g_vgpu;
    bool before[GPU_MAX_OUTPUTS];
    for (unsigned i = 0; i < GPU_MAX_OUTPUTS; i++) before[i] = s->heads[i].connected;
    int event = 0;
    if (s->vp.devcfg && (vr32(s->vp.devcfg, VGPU_CFG_EVENTS_READ) & VGPU_EVENT_DISPLAY)) {
        vw32(s->vp.devcfg, VGPU_CFG_EVENTS_CLEAR, VGPU_EVENT_DISPLAY);
        event = 1;
    }
    if (!event && s->polled_once) return 0;
    s->polled_once = true;
    vgpu_read_heads();
    int changed = !event;                      /* the first poll always reports the heads there are */
    for (unsigned i = 0; i < GPU_MAX_OUTPUTS; i++) if (before[i] != s->heads[i].connected) changed = 1;
    return changed;
}

static const struct gpu_driver g_vgpu_driver = {
    .name = "virtio-gpu",
    .prepare=vgpu_prepare,.select=vgpu_select,.release=vgpu_release,.describe=vgpu_describe,.resource_id=vgpu_resource,
    .get_mode = vgpu_get_mode,
    .flush = vgpu_flush,
    .attach_backing = vgpu_attach_backing,
    .output_count = vgpu_output_count, .output_info = vgpu_output_info,
    .apply_layout = vgpu_apply_layout, .poll_outputs = vgpu_poll_outputs,
};

const struct gpu_driver *virtio_gpu_probe(void) {
    const struct pci_device *gpu = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *dev = pci_get_device(i);
        if (!dev) continue;
        // virtio-gpu modern id: vendor 0x1AF4, device 0x1040 + 16.
        if (dev->vendor_id == 0x1AF4 && dev->device_id == 0x1050) {
            gpu = dev;
            break;
        }
    }
    if (!gpu) return 0;

    kprintf("virtio-gpu: found at %x:%x.%x\n",
            (unsigned int)gpu->bus, (unsigned int)gpu->device,
            (unsigned int)gpu->function);

    memset(&g_vgpu, 0, sizeof(g_vgpu));
    if (!vgpu_init_transport(gpu)) {
        return 0;
    }
    g_vgpu.present = true;
    g_vgpu.num_scanouts = g_vgpu.vp.devcfg ? vr32(g_vgpu.vp.devcfg, VGPU_CFG_NUM_SCANOUTS) : 1;
    if (!g_vgpu.num_scanouts) g_vgpu.num_scanouts = 1;
#if defined(__x86_64__)
    g_vgpu.forced_heads = (unsigned)boot_qemu_outputs();
#endif
    vgpu_read_edid();
    vgpu_read_heads();
    kprintf("virtio-gpu: %u head(s)%s\n", g_vgpu.num_scanouts,
            g_vgpu.forced_heads > 1 ? " (a headless test connects them)" : "");
    return &g_vgpu_driver;
}
