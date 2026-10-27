/* kernel/gfx/comp_stats.c -- what composing the desktop costs (comp_stats.h).
 *
 * Nothing here decides anything. It exists because the desktop's speed had
 * only ever been judged by eye, under TCG, where everything is slow and
 * nothing can be compared: a change that halves a compose and one that does
 * nothing look the same through an emulator's 5 fps. The compositor now says
 * how many composes it did, of how many pixels, how long each took, and why
 * it was asked -- and the pointer's motion is timed from the driver noticing
 * it to the compose that showed it.
 *
 * Integer only (the kernel keeps no FPU state). Percentiles come from a ring
 * of the most recent composes, sorted on demand: the reader pays, never the
 * compositor. */

#include "gfx/comp_stats.h"
#include "gfx/compositor.h"
#include "drivers/timer/timer.h"
#include "drivers/video/framebuffer.h"
#include "include/spinlock.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"     /* sched_precise_lead_ns */
#if defined(__x86_64__)
#include "ipc/notify.h"
#endif

#define RING     512           /* recent composes kept for percentiles */
#define FRAME_RING 256         /* recent frame intervals */
#define PTR_RING 256           /* recent pointer latencies */

struct sample { uint64_t t1; uint32_t us; uint32_t kpx; };
#define PRESENTERS 8
struct presenter { int pid; char title[24]; uint64_t n, px; uint32_t last[4]; };

static spinlock_t g_lock = SPINLOCK_INIT;
static struct {
    uint64_t since;
    uint64_t composes, ns, px, full;
    uint64_t src_n[EMBK_COMP_SRCS], src_px[EMBK_COMP_SRCS], src_ns[EMBK_COMP_SRCS];
    struct sample ring[RING];
    uint32_t head, count;
    uint32_t ptr[PTR_RING];
    uint32_t ptr_head, ptr_count;
    struct presenter pres[PRESENTERS];
    uint64_t frames, last_frame_ns, period_ns;
    uint64_t frame_t[FRAME_RING];          /* when each recent frame finished */
    uint32_t interval[FRAME_RING];         /* and the gap before it, when it had one */
    uint32_t frame_head, frame_count, interval_count, interval_head;
    uint32_t display_mhz, vrr_min_mhz, vrr_max_mhz, rate_mhz, frame_cost_us, rate_peak_mhz;
    uint32_t late[FRAME_RING], late_head, late_count;
    uint32_t effects, effects_mode, effects_steps;
} g;

static uint32_t us_of(uint64_t ns)
{
    uint64_t us = ns / 1000u;
    return us > 0xffffffffu ? 0xffffffffu : (uint32_t)us;
}

void comp_stats_record(unsigned src, uint64_t t0, uint64_t t1, uint64_t px, int full)
{
    if (src >= EMBK_COMP_SRCS) src = EMBK_COMP_SRC_OTHER;
    uint64_t ns = t1 > t0 ? t1 - t0 : 0;
    spin_lock(&g_lock);
    if (!g.since) g.since = t0;
    g.src_n[src]++; g.src_px[src] += px; g.src_ns[src] += ns;
    if (src != EMBK_COMP_SRC_HUD) {
        g.composes++; g.ns += ns; g.px += px;
        if (full) g.full++;
        struct sample *s = &g.ring[g.head];
        s->t1 = t1; s->us = us_of(ns);
        s->kpx = (uint32_t)((px + 999u) / 1000u);
        g.head = (g.head + 1) % RING;
        if (g.count < RING) g.count++;
    }
    spin_unlock(&g_lock);
}

void comp_stats_present(int pid, const char *title, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    char name[sizeof g.pres[0].title];
    unsigned k = 0;
    for (; title && title[k] && k < sizeof name - 1; k++)
        name[k] = (title[k] == ' ' || title[k] == '=') ? '_' : title[k];
    name[k] = 0;
    spin_lock(&g_lock);
    struct presenter *p = 0, *least = &g.pres[0];
    for (int i = 0; i < PRESENTERS; i++) {
        struct presenter *q = &g.pres[i];
        if (q->n && q->pid == pid && !strcmp(q->title, name)) { p = q; break; }
        if (q->n < least->n) least = q;
    }
    if (!p) {                    /* a newcomer displaces the quietest */
        p = least;
        p->pid = pid; p->n = 0; p->px = 0;
        memcpy(p->title, name, sizeof name);
    }
    p->n++; p->px += (uint64_t)w * h;
    p->last[0] = x; p->last[1] = y; p->last[2] = w; p->last[3] = h;
    spin_unlock(&g_lock);
}

void comp_stats_rate(uint32_t display_mhz, uint32_t vrr_min_mhz, uint32_t vrr_max_mhz,
                     uint32_t rate_mhz, uint32_t frame_cost_us)
{
    spin_lock(&g_lock);
    g.display_mhz = display_mhz; g.vrr_min_mhz = vrr_min_mhz; g.vrr_max_mhz = vrr_max_mhz;
    g.rate_mhz = rate_mhz; g.frame_cost_us = frame_cost_us;
    if (rate_mhz > g.rate_peak_mhz) g.rate_peak_mhz = rate_mhz;
    spin_unlock(&g_lock);
}

void comp_stats_effects(uint32_t tier, uint32_t mode)
{
    spin_lock(&g_lock);
    if (tier != g.effects) g.effects_steps++;
    g.effects = tier; g.effects_mode = mode;
    spin_unlock(&g_lock);
}

void comp_stats_late(uint64_t late_ns)
{
    spin_lock(&g_lock);
    g.late[g.late_head] = us_of(late_ns);
    g.late_head = (g.late_head + 1) % FRAME_RING;
    if (g.late_count < FRAME_RING) g.late_count++;
    spin_unlock(&g_lock);
}

void comp_stats_frame(uint64_t t, uint64_t period_ns)
{
    spin_lock(&g_lock);
    if (!g.since) g.since = t;
    g.frames++;
    g.period_ns = period_ns;
    g.frame_t[g.frame_head] = t;
    g.frame_head = (g.frame_head + 1) % FRAME_RING;
    if (g.frame_count < FRAME_RING) g.frame_count++;
    /* a frame within two beats of the last continues a run: an interval */
    if (g.last_frame_ns && t > g.last_frame_ns && t - g.last_frame_ns < 2 * period_ns) {
        g.interval[g.interval_head] = us_of(t - g.last_frame_ns);
        g.interval_head = (g.interval_head + 1) % FRAME_RING;
        if (g.interval_count < FRAME_RING) g.interval_count++;
    }
    g.last_frame_ns = t;
    spin_unlock(&g_lock);
}

void comp_stats_pointer(uint64_t ns)
{
    spin_lock(&g_lock);
    g.ptr[g.ptr_head] = us_of(ns);
    g.ptr_head = (g.ptr_head + 1) % PTR_RING;
    if (g.ptr_count < PTR_RING) g.ptr_count++;
    spin_unlock(&g_lock);
}

void comp_stats_reset(void)
{
    spin_lock(&g_lock);
    uint32_t dm = g.display_mhz, vmin = g.vrr_min_mhz, vmax = g.vrr_max_mhz, rm = g.rate_mhz;
    uint32_t fx = g.effects, fxm = g.effects_mode;
    memset(&g, 0, sizeof g);
    g.display_mhz = dm; g.vrr_min_mhz = vmin; g.vrr_max_mhz = vmax; g.rate_mhz = rm;
    g.effects = fx; g.effects_mode = fxm;
    g.since = time_get_ns();
    spin_unlock(&g_lock);
}

/* Shell sort: a few hundred values, read rarely, no allocation. */
static void sort_u32(uint32_t *v, uint32_t n)
{
    static const uint32_t gaps[] = { 132, 57, 23, 10, 4, 1 };
    for (unsigned k = 0; k < sizeof gaps / sizeof gaps[0]; k++) {
        uint32_t gap = gaps[k];
        for (uint32_t i = gap; i < n; i++) {
            uint32_t x = v[i], j = i;
            while (j >= gap && v[j - gap] > x) { v[j] = v[j - gap]; j -= gap; }
            v[j] = x;
        }
    }
}

/* Nearest-rank percentile of a sorted run. */
static uint32_t rank(const uint32_t *v, uint32_t n, uint32_t pct)
{
    if (!n) return 0;
    uint32_t i = (n * pct + 99u) / 100u;
    return v[i ? i - 1 : 0];
}

void comp_stats_snapshot(struct embk_comp_stats *out)
{
    static uint32_t scratch[RING > FRAME_RING ? RING : FRAME_RING];   /* under g_lock: one reader at a time */
    memset(out, 0, sizeof *out);
    out->version = EMBK_COMP_STATS_VERSION;
    uint64_t now = time_get_ns();
    spin_lock(&g_lock);
    out->now_ns = now;
    out->since_ns = g.since;
    out->composes = g.composes; out->compose_ns = g.ns; out->pixels = g.px; out->full_screen = g.full;
    for (int i = 0; i < EMBK_COMP_SRCS; i++) {
        out->src_composes[i] = g.src_n[i];
        out->src_pixels[i] = g.src_px[i];
        out->src_ns[i] = g.src_ns[i];
    }
    uint64_t kpx = 0;
    for (uint32_t i = 0; i < g.count; i++) {
        const struct sample *s = &g.ring[i];
        scratch[i] = s->us;
        if (s->t1 + 1000000000ull > now) { out->sec_composes++; kpx += s->kpx; }
    }
    out->sec_kpixels = kpx > 0xffffffffu ? 0xffffffffu : (uint32_t)kpx;
    out->recent = g.count;
    sort_u32(scratch, g.count);
    out->p50_us = rank(scratch, g.count, 50);
    out->p95_us = rank(scratch, g.count, 95);
    out->max_us = g.count ? scratch[g.count - 1] : 0;
    for (uint32_t i = 0; i < g.ptr_count; i++) scratch[i] = g.ptr[i];
    out->ptr_samples = g.ptr_count;
    sort_u32(scratch, g.ptr_count);
    out->ptr_p50_us = rank(scratch, g.ptr_count, 50);
    out->ptr_p95_us = rank(scratch, g.ptr_count, 95);
    out->ptr_max_us = g.ptr_count ? scratch[g.ptr_count - 1] : 0;
    out->frames = g.frames;
    out->period_us = us_of(g.period_ns);
    out->display_mhz = g.display_mhz; out->vrr_min_mhz = g.vrr_min_mhz; out->vrr_max_mhz = g.vrr_max_mhz;
    out->rate_mhz = g.rate_mhz; out->frame_cost_us = g.frame_cost_us;
    for (uint32_t i = 0; i < g.frame_count; i++)
        if (g.frame_t[i] + 1000000000ull > now) out->sec_frames++;
    for (uint32_t i = 0; i < g.interval_count; i++) scratch[i] = g.interval[i];
    out->intervals = g.interval_count;
    sort_u32(scratch, g.interval_count);
    out->interval_p50_us = rank(scratch, g.interval_count, 50);
    out->interval_p95_us = rank(scratch, g.interval_count, 95);
    out->interval_max_us = g.interval_count ? scratch[g.interval_count - 1] : 0;
    for (uint32_t i = 0; i < g.late_count; i++) scratch[i] = g.late[i];
    out->lates = g.late_count;
    sort_u32(scratch, g.late_count);
    out->late_p50_us = rank(scratch, g.late_count, 50);
    out->late_p95_us = rank(scratch, g.late_count, 95);
    out->late_max_us = g.late_count ? scratch[g.late_count - 1] : 0;
    out->lead_us = us_of(sched_precise_lead_ns());
    out->rate_peak_mhz = g.rate_peak_mhz;
    out->effects = g.effects; out->effects_mode = g.effects_mode; out->effects_steps = g.effects_steps;
    spin_unlock(&g_lock);

    const fb_info_t *fi = fb_get_info();
    if (fi) { out->screen_w = fi->width; out->screen_h = fi->height; }
    char name[8];
    out->gpu = (uint32_t)compositor_renderer(name, sizeof name);
    out->hud = (uint32_t)compositor_hud_shown();
}

/* "12.34" from microseconds, as milliseconds. */
static void ms_text(char *b, unsigned n, uint32_t us)
{
    snprintf(b, n, "%u.%02u", us / 1000u, (us % 1000u) / 10u);
}

int comp_stats_hud_text(char rows[COMP_STATS_HUD_ROWS][COMP_STATS_HUD_COLS + 1])
{
    struct embk_comp_stats s;
    comp_stats_snapshot(&s);
    char a[16], b[16], c[16];
    uint32_t rate = (s.rate_mhz + 500u) / 1000u;
    uint32_t top = ((s.vrr_max_mhz ? s.vrr_max_mhz : s.display_mhz) + 500u) / 1000u;
    snprintf(rows[0], COMP_STATS_HUD_COLS + 1, "%s %ux%u %u/%uHz%s%s",
             s.gpu ? "GPU" : "CPU", s.screen_w, s.screen_h, rate, top ? top : 60u, s.vrr_max_mhz ? " VRR" : "",
             s.effects ? " -fx" : "");
    ms_text(a, sizeof a, s.interval_p50_us); ms_text(b, sizeof b, s.interval_p95_us);
    snprintf(rows[1], COMP_STATS_HUD_COLS + 1, "%3u fps beat %s p95 %s", s.sec_frames, a, b);
    ms_text(a, sizeof a, s.p50_us); ms_text(b, sizeof b, s.p95_us); ms_text(c, sizeof c, s.max_us);
    snprintf(rows[2], COMP_STATS_HUD_COLS + 1, "ms %s p95 %s max %s", a, b, c);
    ms_text(a, sizeof a, s.ptr_p50_us); ms_text(b, sizeof b, s.ptr_p95_us);
    snprintf(rows[3], COMP_STATS_HUD_COLS + 1, "pointer %s p95 %s ms", a, b);
    snprintf(rows[4], COMP_STATS_HUD_COLS + 1, "P%lu M%lu C%lu A%lu O%lu",
             (unsigned long)s.src_composes[EMBK_COMP_SRC_PRESENT],
             (unsigned long)s.src_composes[EMBK_COMP_SRC_MOVE],
             (unsigned long)s.src_composes[EMBK_COMP_SRC_POINTER],
             (unsigned long)s.src_composes[EMBK_COMP_SRC_ANIM],
             (unsigned long)s.src_composes[EMBK_COMP_SRC_OTHER]);
    return COMP_STATS_HUD_ROWS;
}

static const char *const g_src_name[EMBK_COMP_SRCS] = {
    "present", "move", "pointer", "anim", "other", "hud",
};

/* One line a test can parse: key=value pairs, integers only. */
static void dump(void)
{
    struct embk_comp_stats s;
    comp_stats_snapshot(&s);
    uint64_t span = s.now_ns > s.since_ns && s.since_ns ? s.now_ns - s.since_ns : 0;
    kprintf("perf: span_ms=%lu composes=%lu compose_us=%lu kpx=%lu full=%lu"
            " p50_us=%u p95_us=%u max_us=%u recent=%u"
            " ptr_n=%u ptr_p50_us=%u ptr_p95_us=%u ptr_max_us=%u"
            " screen=%ux%u gpu=%u frames=%lu period_us=%u intervals=%u"
            " interval_p50_us=%u interval_p95_us=%u interval_max_us=%u"
            " display_mhz=%u vrr_min_mhz=%u vrr_max_mhz=%u rate_mhz=%u frame_cost_us=%u"
            " lates=%u late_p50_us=%u late_p95_us=%u late_max_us=%u lead_us=%u rate_peak_mhz=%u"
            " effects=%u effects_mode=%u effects_steps=%u\n",
            (unsigned long)(span / 1000000u), (unsigned long)s.composes,
            (unsigned long)(s.compose_ns / 1000u), (unsigned long)(s.pixels / 1000u),
            (unsigned long)s.full_screen, s.p50_us, s.p95_us, s.max_us, s.recent,
            s.ptr_samples, s.ptr_p50_us, s.ptr_p95_us, s.ptr_max_us,
            s.screen_w, s.screen_h, s.gpu, (unsigned long)s.frames, s.period_us, s.intervals,
            s.interval_p50_us, s.interval_p95_us, s.interval_max_us,
            s.display_mhz, s.vrr_min_mhz, s.vrr_max_mhz, s.rate_mhz, s.frame_cost_us,
            s.lates, s.late_p50_us, s.late_p95_us, s.late_max_us, s.lead_us, s.rate_peak_mhz,
            s.effects, s.effects_mode, s.effects_steps);
    for (int i = 0; i < EMBK_COMP_SRCS; i++)
        kprintf("perf: src=%s composes=%lu compose_us=%lu kpx=%lu\n", g_src_name[i],
                (unsigned long)s.src_composes[i], (unsigned long)(s.src_ns[i] / 1000u),
                (unsigned long)(s.src_pixels[i] / 1000u));
    struct presenter pres[PRESENTERS];
    spin_lock(&g_lock);
    memcpy(pres, g.pres, sizeof pres);
    spin_unlock(&g_lock);
    for (int i = 0; i < PRESENTERS; i++) {          /* busiest first */
        int best = -1;
        for (int j = 0; j < PRESENTERS; j++)
            if (pres[j].n && (best < 0 || pres[j].n > pres[best].n)) best = j;
        if (best < 0) break;
        kprintf("perf: win=%s pid=%d presents=%lu kpx=%lu last_x=%u last_y=%u last_w=%u last_h=%u\n",
                pres[best].title[0] ? pres[best].title : "?", pres[best].pid, (unsigned long)pres[best].n,
                (unsigned long)(pres[best].px / 1000u), pres[best].last[0], pres[best].last[1],
                pres[best].last[2], pres[best].last[3]);
        pres[best].n = 0;
    }
    kprintf("perf: end\n");
}

int comp_stats_command(const char *cmd)
{
    if (!strcmp(cmd, "perf")) { dump(); return 1; }
    if (!strcmp(cmd, "perf reset")) { comp_stats_reset(); kprintf("perf: reset\n"); return 1; }
    if (!strcmp(cmd, "perf windows")) { compositor_print_windows(); return 1; }
    if (!strncmp(cmd, "perf slowmo ", 12)) {
        uint32_t n = 0;
        for (const char *p = cmd + 12; *p >= '0' && *p <= '9'; p++) n = n * 10 + (uint32_t)(*p - '0');
        if (!n || n > 20) { kprintf("perf: slowmo <1-20>\n"); return 1; }
        compositor_slowmo(n);
        kprintf("perf: slowmo %u\n", n);
        return 1;
    }
    if (!strcmp(cmd, "perf effects auto") || !strcmp(cmd, "perf effects full") ||
        !strcmp(cmd, "perf effects reduced")) {
        /* the Effects setting, from the console: tests hold the tier with it */
        compositor_effects_override(cmd[13] == 'a' ? 0 : cmd[13] == 'f' ? 1 : 2);
        kprintf("perf: effects %s\n", cmd + 13);
        return 1;
    }
    if (!strncmp(cmd, "perf notice ", 12)) {
        /* a system notice, as the kernel posts one: the banners' own test */
#if defined(__x86_64__)
        notify_system(0, cmd + 12, "Posted from the serial console.");
        kprintf("perf: notice\n");
#else
        kprintf("perf: notice: no system notices on this architecture\n");   /* sessions are x86-only */
#endif
        return 1;
    }
    if (!strncmp(cmd, "perf beat ", 10)) {
        uint32_t n = 0;
        for (const char *p = cmd + 10; *p >= '0' && *p <= '9'; p++) n = n * 10 + (uint32_t)(*p - '0');
        if (!n || n > 60) { kprintf("perf: beat <seconds, 1-60>\n"); return 1; }
        compositor_beat_test(n);
        kprintf("perf: beat %u s\n", n);
        return 1;
    }
    if (!strcmp(cmd, "perf cursor hw") || !strcmp(cmd, "perf cursor sw")) {
        compositor_cursor_plane(cmd[12] == 'h');
        kprintf("perf: cursor %s\n", cmd + 12);
        return 1;
    }
    if (!strncmp(cmd, "perf display ", 13)) {
        /* 120: a fixed 120 Hz display; 48-120: VRR from 48 to 120; auto: the real one */
        const char *p = cmd + 13;
        uint32_t a = 0, b = 0;
        if (strcmp(p, "auto")) {
            while (*p >= '0' && *p <= '9') a = a * 10 + (uint32_t)(*p++ - '0');
            if (*p == '-') { p++; while (*p >= '0' && *p <= '9') b = b * 10 + (uint32_t)(*p++ - '0'); }
            if (*p || !a || a > 1000 || b > 1000 || (b && b < a)) { kprintf("perf: display <hz> | <min>-<max> | auto\n"); return 1; }
        }
        if (b) compositor_simulate_display(0, a * 1000u, b * 1000u);
        else compositor_simulate_display(a * 1000u, 0, 0);
        kprintf("perf: display %s\n", a ? "simulated" : "auto");
        return 1;
    }
    if (!strcmp(cmd, "perf hud")) {
        int on = compositor_hud_toggle();
        kprintf("perf: hud %s\n", on ? "on" : "off");
        return 1;
    }
    return 0;
}
