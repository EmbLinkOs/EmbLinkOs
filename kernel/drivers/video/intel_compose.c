/* intel_compose.c -- the desktop composed with the Gen9 blitter. See intel_compose.h. */
#include "drivers/video/intel_compose.h"
#include "gfx/soft_compose.h"
#include "include/kstring.h"

#define PAGE        4096u
#define MAX_IMAGES  96u
#define MAX_FRONTS  8u

struct image {
    int       used;
    uint8_t  *px;
    uint32_t  w, h, stride;
    uint32_t  va, npages;          /* the PPGTT range its pages are mapped at */
    uint64_t  gaddr;               /* the graphics address of its first pixel */
};

static const struct intel_compose_env *E;
static int ready;
static struct image img[MAX_IMAGES];
/* The composition buffer, and the blur's scratch, sized to the front. */
static struct {
    uint8_t *px;
    void    *raw;
    uint32_t w, h, pitch, va, npages;
    uint32_t *scratch;
    void    *scratch_raw;
} T;
static struct { uint32_t ggtt, pages; } fronts[MAX_FRONTS];
static struct gpu_rect damage;
static uint32_t front_ggtt, front_pitch;

static struct intel_blt *blt(void) { return E->blt; }

/* ---- memory the blitter reaches ----------------------------------------------- */
/* Map the pages under [p, p + bytes) at a fresh PPGTT range; returns the
 * range, and through *gaddr the graphics address of p itself. */
static uint32_t map_span(const void *p, uint64_t bytes, uint32_t *npages, uint64_t *gaddr)
{
    uintptr_t start = (uintptr_t)p & ~(uintptr_t)(PAGE - 1), end = (uintptr_t)p + bytes;
    uint32_t n = (uint32_t)((end - start + PAGE - 1) / PAGE);
    uint64_t phys[64];
    uint32_t va = intel_blt_va_alloc(blt(), n);
    if (!va) return 0;
    for (uint32_t done = 0; done < n;) {
        uint32_t k = n - done < 64 ? n - done : 64;
        for (uint32_t i = 0; i < k; i++) {
            phys[i] = E->mem->va_phys(E->mem->ctx, (const void *)(start + (uintptr_t)(done + i) * PAGE));
            if (!phys[i]) { intel_blt_va_free(blt(), va, n); return 0; }
        }
        if (intel_blt_map(blt(), va + done * PAGE, phys, k, BLT_CACHED)) { intel_blt_va_free(blt(), va, n); return 0; }
        done += k;
    }
    *npages = n;
    *gaddr = va + ((uintptr_t)p - start);
    return va;
}

static void drop_target(void)
{
    if (T.va) { intel_blt_sync(blt()); intel_blt_va_free(blt(), T.va, T.npages); }
    if (T.raw) E->free(E->ctx, T.raw);
    if (T.scratch_raw) E->free(E->ctx, T.scratch_raw);
    memset(&T, 0, sizeof T);
}

/* The composition buffer for a w x h front (made again when that changes). */
static int target(uint32_t w, uint32_t h)
{
    if (T.px && T.w == w && T.h == h) return 0;
    drop_target();
    uint32_t pitch = (w * 4 + 63u) & ~63u;
    uint64_t gaddr;
    T.px = (uint8_t *)E->alloc(E->ctx, (uint64_t)pitch * h, &T.raw);
    T.scratch = (uint32_t *)E->alloc(E->ctx, (uint64_t)w * h * 4, &T.scratch_raw);
    if (!T.px || !T.scratch) { drop_target(); return -1; }
    memset(T.px, 0, (uint64_t)pitch * h);
    T.w = w; T.h = h; T.pitch = pitch;
    T.va = map_span(T.px, (uint64_t)pitch * h, &T.npages, &gaddr);
    if (!T.va || gaddr != T.va) { drop_target(); return -1; }
    return 0;
}

/* The front, mirrored uncached at its own GGTT address: the display reads
 * memory, not the LLC. */
static int front(uint32_t *w, uint32_t *h)
{
    uint32_t ggtt, pitch;
    if (E->front(E->ctx, &ggtt, &pitch, w, h)) return -1;
    uint32_t pages = (uint32_t)(((uint64_t)pitch * *h + PAGE - 1) / PAGE);
    for (unsigned i = 0; i < MAX_FRONTS; i++)
        if (fronts[i].pages && fronts[i].ggtt == ggtt && fronts[i].pages == pages) {
            front_ggtt = ggtt; front_pitch = pitch;
            return 0;
        }
    unsigned slot = 0;
    while (slot < MAX_FRONTS && fronts[slot].pages) slot++;
    if (slot == MAX_FRONTS) return -1;
    if (intel_blt_mirror_ggtt(blt(), ggtt, pages, BLT_UNCACHED)) return -1;
    fronts[slot].ggtt = ggtt; fronts[slot].pages = pages;
    front_ggtt = ggtt; front_pitch = pitch;
    return 0;
}

void intel_compose_forget_front(uint32_t ggtt)
{
    if (!E) return;
    for (unsigned i = 0; i < MAX_FRONTS; i++)
        if (fronts[i].pages && fronts[i].ggtt == ggtt) {
            intel_blt_sync(blt());                      /* nothing may still be writing it */
            intel_blt_unmap(blt(), ggtt, fronts[i].pages);
            fronts[i].pages = 0;
        }
}

static struct sc_surface screen(void)
{
    return (struct sc_surface){ T.px, T.pitch, (int32_t)T.w, (int32_t)T.h };
}

/* The CPU is about to touch the composition buffer: what the blitter was
 * given must be on it first. */
static void cpu_turn(void)
{
    intel_blt_sync(blt());
}

/* ---- images ----------------------------------------------------------------- */
static int ic_image_create(void *pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t *id)
{
    if (!ready || intel_blt_dead(blt()) || !w || !h || stride < w * 4 || (stride & 3u) || ((uintptr_t)pixels & 3u))
        return -1;
    unsigned i = 0;
    while (i < MAX_IMAGES && img[i].used) i++;
    if (i == MAX_IMAGES) return -1;
    struct image *m = &img[i];
    uint64_t gaddr;
    uint32_t n;
    uint32_t va = map_span(pixels, (uint64_t)stride * (h - 1) + (uint64_t)w * 4, &n, &gaddr);
    if (!va) return -1;
    *m = (struct image){ 1, (uint8_t *)pixels, w, h, stride, va, n, gaddr };
    *id = i + 1;
    return 0;
}

/* The blitter reads the app's own pages, coherent with the CPU in the LLC:
 * there is nothing to upload. */
static void ic_image_update(uint32_t id, const struct gpu_rect *r)
{
    (void)id; (void)r;
}

static void ic_image_destroy(uint32_t id)
{
    if (!id || id > MAX_IMAGES || !img[id - 1].used) return;
    struct image *m = &img[id - 1];
    intel_blt_sync(blt());                              /* no blit still reads it */
    intel_blt_va_free(blt(), m->va, m->npages);
    m->used = 0;
}

static struct sc_surface image_surface(const struct image *m)
{
    return (struct sc_surface){ m->px, m->stride, (int32_t)m->w, (int32_t)m->h };
}

/* ---- a frame ------------------------------------------------------------------- */
static int ic_begin(const struct gpu_rect *d)
{
    if (!ready || intel_blt_dead(blt())) return -1;
    uint32_t w, h;
    if (front(&w, &h) || target(w, h)) return -1;
    struct gpu_rect all = { 0, 0, (int32_t)w, (int32_t)h };
    damage = *d;
    return sc_intersect(&damage, &all) ? 0 : -1;
}

/* A rectangle of the composition buffer for the blitter: in the damage, or nothing. */
static int blit_area(struct gpu_rect *r)
{
    return sc_intersect(r, &damage);
}

static void ic_fill(const struct gpu_rect *r0, uint32_t rgb)
{
    struct gpu_rect r = *r0;
    if (!blit_area(&r)) return;
    if (intel_blt_fill(blt(), T.va, T.pitch, r.x, r.y, r.w, r.h, 0xff000000u | (rgb & 0xffffffu))) {
        cpu_turn();
        struct sc_surface s = screen();
        sc_fill(&s, &damage, &r, rgb);
    }
}

/* Copy `r` (screen pixels, inside dst) of an unscaled image. */
static void copy_part(const struct image *m, const struct gpu_draw *d, struct gpu_rect r)
{
    if (!sc_intersect(&r, &d->dst) || !blit_area(&r)) return;
    int32_t sx = d->src.x + (r.x - d->dst.x), sy = d->src.y + (r.y - d->dst.y);
    if (sx < 0 || sy < 0 || sx + r.w > (int32_t)m->w || sy + r.h > (int32_t)m->h ||
        intel_blt_copy(blt(), T.va, T.pitch, r.x, r.y, m->gaddr, m->stride, sx, sy, r.w, r.h)) {
        cpu_turn();
        struct sc_surface s = screen(), i = image_surface(m);
        sc_draw(&s, &r, &i, d);
    }
}

static void fill_part(const struct gpu_draw *d, struct gpu_rect r, uint32_t rgb)
{
    if (!sc_intersect(&r, &d->dst) || !blit_area(&r)) return;
    if (intel_blt_fill(blt(), T.va, T.pitch, r.x, r.y, r.w, r.h, 0xff000000u | (rgb & 0xffffffu))) {
        cpu_turn();
        struct sc_surface s = screen();
        sc_fill(&s, &damage, &r, rgb);
    }
}

static void ic_draw(const struct gpu_draw *d)
{
    if (!d->image || d->image > MAX_IMAGES || !img[d->image - 1].used) return;
    const struct image *m = &img[d->image - 1];
    struct sc_surface s = screen(), i = image_surface(m);
    int scaled = d->src.w != d->dst.w || d->src.h != d->dst.h;
    if (scaled || !d->opaque || d->opacity != 255) {        /* it blends: the CPU's */
        cpu_turn();
        sc_draw(&s, &damage, &i, d);
        return;
    }
    struct gpu_rect f = d->frame.w > 0 && d->frame.h > 0 ? d->frame : d->dst;
    int32_t half = (f.w < f.h ? f.w : f.h) / 2;
    int32_t radius = d->radius > half ? half : d->radius, ring = d->ring > half ? half : d->ring;
    int32_t c = radius > ring ? radius : ring;               /* the corner boxes, where the edge curves */
    int32_t x0 = f.x, y0 = f.y, x1 = f.x + f.w, y1 = f.y + f.h;
    /* The inside by the blitter: a middle band and the bands between the corners. */
    copy_part(m, d, (struct gpu_rect){ x0 + ring, y0 + c, f.w - 2 * ring, f.h - 2 * c });
    if (c > ring) {
        copy_part(m, d, (struct gpu_rect){ x0 + c, y0 + ring, f.w - 2 * c, c - ring });
        copy_part(m, d, (struct gpu_rect){ x0 + c, y1 - c, f.w - 2 * c, c - ring });
    }
    /* The straight runs of the border ring, by the blitter. */
    if (ring) {
        fill_part(d, (struct gpu_rect){ x0 + c, y0, f.w - 2 * c, ring }, d->ring_rgb);
        fill_part(d, (struct gpu_rect){ x0 + c, y1 - ring, f.w - 2 * c, ring }, d->ring_rgb);
        fill_part(d, (struct gpu_rect){ x0, y0 + c, ring, f.h - 2 * c }, d->ring_rgb);
        fill_part(d, (struct gpu_rect){ x1 - ring, y0 + c, ring, f.h - 2 * c }, d->ring_rgb);
    }
    /* The four corners, where coverage and the ring curve: the CPU's. */
    if (c) {
        struct gpu_rect corner[4] = { { x0, y0, c, c }, { x1 - c, y0, c, c }, { x0, y1 - c, c, c }, { x1 - c, y1 - c, c, c } };
        int any = 0;
        for (unsigned k = 0; k < 4; k++) {
            struct gpu_rect r = corner[k];
            if (sc_intersect(&r, &d->dst) && sc_intersect(&r, &damage)) {
                if (!any) { cpu_turn(); any = 1; }
                sc_draw(&s, &r, &i, d);
            }
        }
    }
}

static void ic_shadow(const struct gpu_rect *frame, const struct gpu_rect *cover, uint32_t radius, uint32_t width,
                      uint32_t peak)
{
    cpu_turn();
    struct sc_surface s = screen();
    sc_shadow(&s, &damage, frame, cover, radius, width, peak);
}

static void ic_blur(const struct gpu_rect *r, uint32_t radius, const struct gpu_rect *frame, uint32_t corner)
{
    cpu_turn();
    struct sc_surface s = screen();
    sc_blur(&s, &damage, r, radius, frame, corner, T.scratch);
}

/* The damage onto the scanout, and waited for: once end() returns no blit
 * reads a window's pages, so a window that dies next may free them. */
static int ic_end(void)
{
    if (intel_blt_dead(blt())) return -1;
    if (intel_blt_copy(blt(), front_ggtt, front_pitch, damage.x, damage.y, T.va, T.pitch, damage.x, damage.y,
                       damage.w, damage.h))
        return -1;
    return intel_blt_sync(blt());
}

/* The screen is the composition buffer: its pixels go to the backbuffer. */
static int ic_read(const struct gpu_rect *r0)
{
    if (!ready || !T.px) return -1;
    if (intel_blt_sync(blt())) return -1;
    struct gpu_rect r = *r0, all = { 0, 0, (int32_t)T.w, (int32_t)T.h };
    if (!sc_intersect(&r, &all)) return -1;
    uint32_t pitch;
    uint8_t *back = (uint8_t *)E->backbuffer(E->ctx, &pitch);
    if (!back) return -1;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        memcpy(back + (uint64_t)y * pitch + (uint64_t)r.x * 4, T.px + (uint64_t)y * T.pitch + (uint64_t)r.x * 4,
               (uint64_t)r.w * 4);
    return 0;
}

static const struct gpu_compose ops = {
    .name = "Intel blitter",
    .image_create = ic_image_create, .image_update = ic_image_update, .image_destroy = ic_image_destroy,
    .begin = ic_begin, .fill = ic_fill, .shadow = ic_shadow, .blur = ic_blur, .draw = ic_draw,
    .end = ic_end, .read = ic_read,
};

int intel_compose_init(const struct intel_compose_env *env)
{
    E = env;
    memset(img, 0, sizeof img);
    memset(&T, 0, sizeof T);
    memset(fronts, 0, sizeof fronts);
    ready = env && env->blt && !intel_blt_dead(env->blt);
    return ready ? 0 : -1;
}

const struct gpu_compose *intel_compose(void)
{
    return ready && !intel_blt_dead(blt()) ? &ops : 0;
}
