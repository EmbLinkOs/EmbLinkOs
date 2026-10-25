/* virgl_compose_test.c -- the GPU compositor's command streams run on a real
 * virglrenderer, with the host's OpenGL (Mesa llvmpipe in CI), and every
 * operation checked in the pixels read back: fills, opaque and premultiplied
 * images, scaling, opacity, rounded corners and the border ring, the drop
 * shadow, the glass blur against the CPU compositor's own box blur, partial
 * uploads, and drawing confined to the damage.
 *
 * Every check runs twice: on a screen made exactly as QEMU makes a 2D
 * resource under virgl (target 2D, B8G8R8X8, render target, Y_0_TOP), so
 * what holds here holds on the virtio-gpu device, and on a plain one, whose
 * rows the renderer keeps the other way up.
 *
 * Linux only, with libvirglrenderer and a DRM render node (vgem is enough):
 *   cc -std=c11 -D_GNU_SOURCE -D_TYPES_H -include stdint.h -include stdbool.h -include stddef.h \
 *      -Ikernel tests/drivers/virgl_compose_test.c kernel/drivers/video/virgl.c \
 *      kernel/drivers/video/virgl_compose.c -lvirglrenderer
 * (the kernel's types.h is bypassed: virglrenderer.h brings the C library's bool).
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <virgl/virglrenderer.h>
#include "drivers/video/virgl.h"
#include "drivers/video/virgl_compose.h"

struct virgl_box { uint32_t x, y, z, w, h, d; };

#define CTX 1
#define W 320
#define H 200
static uint32_t screen[W * H];          /* the scanout's backing */
static uint32_t next_id = 10, screen_res;
static int screen_y0_top;
static int failures;

static void write_fence(void *cookie, uint32_t fence) { (void)cookie; (void)fence; }
static void log_line(enum virgl_log_level_flags level, const char *message, void *user)
{
    (void)user;
    if (level >= VIRGL_LOG_LEVEL_WARNING || getenv("VGC_VERBOSE")) fprintf(stderr, "virgl: %s", message);
}
static int drm_fd(void *cookie) { (void)cookie; return open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC); }
static struct virgl_renderer_callbacks cbs_plain = { .version = 1, .write_fence = write_fence };
static struct virgl_renderer_callbacks cbs = { .version = 2, .write_fence = write_fence, .get_drm_fd = drm_fd };

/* ---- the transport, on libvirglrenderer ------------------------------------------ */
static struct { uint32_t id; struct iovec *iov; } backs[64];

static uint32_t t_new_id(void) { return next_id++; }

static int t_create(uint32_t id, uint32_t target, uint32_t format, uint32_t bind, uint32_t w, uint32_t h,
                    uint32_t flags, void *backing, uint64_t bytes)
{
    struct virgl_renderer_resource_create_args a = { id, target, format, bind, w, h, 1, 1, 0, 0, flags };
    if (virgl_renderer_resource_create(&a, NULL, 0)) return -1;
    /* The backing is attached afterwards, as QEMU does for ATTACH_BACKING: an
     * iovec given at creation never reaches the renderer's transfers. It keeps
     * the iovec, not a copy, so it lives as long as the resource. */
    struct iovec *iov = backing ? malloc(sizeof *iov) : NULL;
    if (iov) {
        iov->iov_base = backing; iov->iov_len = bytes;
        if (virgl_renderer_resource_attach_iov((int)id, iov, 1)) { virgl_renderer_resource_unref(id); free(iov); return -1; }
    }
    virgl_renderer_ctx_attach_resource(CTX, (int)id);
    for (unsigned i = 0; i < 64; i++) if (!backs[i].id) { backs[i].id = id; backs[i].iov = iov; break; }
    return 0;
}

static void t_destroy(uint32_t id)
{
    virgl_renderer_resource_unref(id);
    for (unsigned i = 0; i < 64; i++) if (backs[i].id == id) { backs[i].id = 0; free(backs[i].iov); backs[i].iov = NULL; }
}

static int t_to_host(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride)
{
    struct virgl_box b = { x, y, 0, w, h, 1 };
    /* Through the context, as the kernel's TRANSFER_TO_HOST_3D goes: ordered with its drawing. */
    return virgl_renderer_transfer_write_iov(id, CTX, 0, stride, 0, &b, (uint64_t)y * stride + x * 4, NULL, 0);
}

static int t_from_host(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride)
{
    struct virgl_box b = { x, y, 0, w, h, 1 };
    int rc = virgl_renderer_transfer_read_iov(id, CTX, 0, stride, 0, &b, (uint64_t)y * stride + x * 4, NULL, 0);
    if (rc) fprintf(stderr, "transfer from host of %u (%u,%u %ux%u stride %u): %d\n", id, x, y, w, h, stride, rc);
    return rc;
}

static int t_submit(const uint32_t *dw, uint32_t n)
{
    return virgl_renderer_submit_cmd((void *)dw, CTX, (int)n);
}

static void t_target(uint32_t *id, uint32_t *w, uint32_t *h, int *y0_top)
{
    *id = screen_res; *w = W; *h = H; *y0_top = screen_y0_top;
}
static int t_present(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)id; (void)x; (void)y; (void)w; (void)h;
    return 0;
}

static const struct vgc_transport transport = {
    t_new_id, t_create, t_destroy, t_to_host, t_from_host, t_submit, t_target, t_present,
};

/* Image pixels: anywhere -- transfers copy them through the iovec. */
static uint32_t *check_pixels(size_t n) { uint32_t *p = calloc(n, 4); assert(p); return p; }
#include "compose_checks.h"

int main(void)
{
    virgl_set_log_callback(log_line, NULL, NULL);
    /* EGL on Mesa's surfaceless platform first (no device needed), then on the render node. */
    static int cookie;                                 /* virglrenderer refuses a NULL one */
    if (virgl_renderer_init(&cookie, VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_USE_SURFACELESS, &cbs_plain) &&
        virgl_renderer_init(&cookie, VIRGL_RENDERER_USE_EGL, &cbs)) {
        puts("virglrenderer would not start: no EGL here");
        return 2;
    }
    assert(virgl_renderer_context_create(CTX, 4, "test") == 0);
    assert(virgl_compose_init(&transport) == 0);
    const struct gpu_compose *ops = virgl_compose();
    assert(ops);
    for (screen_y0_top = 1; screen_y0_top >= 0; screen_y0_top--) {
        /* 1: QEMU's RESOURCE_CREATE_2D under virgl (Y_0_TOP, render target). */
        screen_res = t_new_id();
        assert(t_create(screen_res, VG_TEXTURE_2D, VG_FORMAT_B8G8R8X8_UNORM, VG_BIND_RENDER_TARGET, W, H,
                        screen_y0_top ? 1u : 0u, screen, sizeof screen) == 0);
        int before = failures;
        compose_checks(ops);
        if (failures != before) printf("  ^ on the %s screen\n", screen_y0_top ? "Y_0_TOP" : "plain");
        t_destroy(screen_res);                         /* the compositor lets go of it at the next begin */
    }
    if (failures) { printf("virgl compose: %d failures\n", failures); return 1; }
    puts("virgl compose: fills, opaque/premultiplied images, opacity, rounded corners and ring, shadow, "
         "blur equal to the CPU box blur, scaling, partial uploads and damage clipping OK on virglrenderer, "
         "on Y_0_TOP and plain screens");
    return 0;
}
