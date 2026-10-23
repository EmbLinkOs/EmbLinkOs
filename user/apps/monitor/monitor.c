/* user/apps/monitor/monitor.c -- Activity Monitor: what this machine is doing.
 *
 * WHY THIS EXISTS. EmbLink knows an unusual amount about itself -- every
 * process's accumulated CPU time, its session, its priority, whether it is a
 * kernel thread -- and until now none of that was reachable from the desktop.
 * You could read it from the kernel console over a serial cable, which on the
 * machine this OS is actually meant to run on is a cable nobody has plugged in.
 * A system you cannot inspect from inside itself is a system you debug by
 * rebooting.
 *
 * WHAT IT REFUSES TO DO. There is no "memory used" column per process, because
 * the kernel does not report one and a number invented here would be worse than
 * a blank: every other reading in this window is measured. The machine-wide
 * memory figures ARE real (SYS_meminfo), so those are shown, and the per-process
 * column is simply absent rather than guessed.
 *
 * CPU PERCENTAGE IS A RATE, and rates need two samples. embk_proc_list reports
 * cpu_ns ACCUMULATED since a process started, so the first frame after launch
 * can only show how busy something has been over its whole life -- which for a
 * process that ran hard for a second and then went to sleep is a lie that looks
 * like a reading. So the first sample of any process establishes a baseline and
 * shows nothing; the second one, a second later, is the first honest number.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define MAX_PROC 64

/* One row's worth of what we know, plus the bookkeeping a rate needs. Keyed by
 * pid, and the pid is REUSED by the kernel once a slot is recycled -- so a
 * stale baseline could be charged to a brand-new process and show it at some
 * impossible percentage. `start_ns` guards that: a cpu_ns that went DOWN means
 * this is not the process we were watching. */
struct row {
    uint32_t pid;
    char     name[24];
    int      state;
    uint8_t  priority;
    unsigned char is_kthread;
    uint64_t cpu_ns;        /* this sample */
    uint64_t prev_ns;       /* the one before, for the rate */
    int      pct;           /* tenths of a percent of one core; -1 = no reading yet */
    int      seen;          /* survives this refresh */
};

static struct row      g_rows[MAX_PROC];
static int             g_nrows;
static uint64_t        g_last_ms;
static struct embk_meminfo g_mem;
static int             g_mem_ok;
static bool            g_show_kthreads;
static uint32_t        g_selected;          /* pid, 0 = none */
static char            g_status[96];

/* Total CPU across everything that is not a kernel thread, as a rate. The same
 * arithmetic the menu bar does, kept here too because this window must agree
 * with the bar and reading the bar's answer is not possible. */
static int  g_total_pct;
static uint64_t g_total_prev_ns;

static const char *state_name(int s) {
    switch (s) {
        case 1:  return "Ready";
        case 2:  return "Running";
        case 3:  return "Blocked";
        case 4:  return "Zombie";
        default: return "-";
    }
}

/* Find the row for a pid, or claim a free one. */
static struct row *row_for(uint32_t pid) {
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].pid == pid) return &g_rows[i];
    if (g_nrows >= MAX_PROC) return 0;
    struct row *r = &g_rows[g_nrows++];
    memset(r, 0, sizeof *r);
    r->pid = pid;
    r->pct = -1;                 /* no rate until there are two samples */
    return r;
}

static void sample(void) {
    static struct embk_proc_info ps[MAX_PROC];
    int n = embk_proc_list(ps, MAX_PROC);
    if (n <= 0) return;

    uint64_t now = embk_uptime_ms();
    uint64_t dms = (g_last_ms && now > g_last_ms) ? (now - g_last_ms) : 0;

    for (int i = 0; i < g_nrows; i++) g_rows[i].seen = 0;

    uint64_t busy = 0;
    for (int i = 0; i < n; i++) {
        struct row *r = row_for(ps[i].pid);
        if (!r) continue;
        snprintf(r->name, sizeof r->name, "%s",
                 ps[i].name[0] ? ps[i].name : (ps[i].is_kthread ? "kernel" : "?"));
        r->state      = ps[i].state;
        r->priority   = ps[i].priority;
        r->is_kthread = ps[i].is_kthread;
        r->prev_ns    = r->cpu_ns;
        r->cpu_ns     = ps[i].cpu_ns;
        r->seen       = 1;

        /* A pid whose CPU time went backwards is a REUSED pid: the slot was
         * recycled and this is somebody else. Start its baseline over rather
         * than charge it the difference. */
        if (r->cpu_ns < r->prev_ns) { r->prev_ns = r->cpu_ns; r->pct = -1; }
        else if (dms) {
            uint64_t dns = r->cpu_ns - r->prev_ns;
            /* ns per ms is the fraction of ONE core times 1e6. A percent is
             * therefore dns/(dms*10000) and a TENTH of a percent -- which is
             * what this column shows -- is dns/(dms*1000). The menu bar does
             * the same arithmetic on the same numbers; getting it wrong here
             * gave a desktop that was 24% busy in the corner of the screen and
             * 100% busy in this window, which is how the error announced
             * itself. */
            r->pct = (int)(dns / (dms * 1000ull));
            if (r->pct > 9999) r->pct = 9999;      /* 999.9% -- four cores can */
        }
        if (!ps[i].is_kthread) busy += ps[i].cpu_ns;
    }

    if (dms && g_total_prev_ns && busy >= g_total_prev_ns) {
        g_total_pct = (int)((busy - g_total_prev_ns) / (dms * 10000ull));
        if (g_total_pct > 999) g_total_pct = 999;   /* the bar's own ceiling */
    }
    g_total_prev_ns = busy;

    /* Drop rows for processes that are gone, so the list is what IS running
     * rather than what ever ran. */
    int out = 0;
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].seen) { if (out != i) g_rows[out] = g_rows[i]; out++; }
    g_nrows = out;

    /* Sort by CPU, descending: the reason anybody opens this window is to find
     * out what is eating the machine, and that should be the first line. */
    for (int i = 1; i < g_nrows; i++) {
        struct row t = g_rows[i];
        int j = i - 1;
        while (j >= 0 && g_rows[j].pct < t.pct) { g_rows[j + 1] = g_rows[j]; j--; }
        g_rows[j + 1] = t;
    }

    g_mem_ok = (embk_meminfo(&g_mem) == 0);
    g_last_ms = now;
}

static void pct_text(char *out, size_t cap, int tenths) {
    if (tenths < 0) snprintf(out, cap, "--");
    else            snprintf(out, cap, "%d.%d%%", tenths / 10, tenths % 10);
}

/* A labelled bar. Used for the two machine-wide readings, where a number alone
 * makes you do the comparison yourself and the comparison is the whole point. */
static void meter_row(const char *label, const char *value, float frac, Color fill) {
    const struct ui_theme *t = ui_theme();
    /* Fill, not grow. In a vertical stack `.grow` is the MAIN axis -- it makes
     * the meter taller, not wider -- and a track with no width is a track you
     * cannot see. That is why the first version of this window showed two
     * labels and no bars at all. */
    VStack(.spacing = 6, .grow = 1, .align = Fill) {
        HStack(.spacing = 8, .align = Center, .grow = 1) {
            Text(label).caption().secondary();
            Spacer();
            Text(value).caption();
        }
        HStack(.height = 8, .corner = 4, .background = t->surface_alt, .grow = 1) {
            if (frac > 0.001f) {
                HStack(.height = 8, .corner = 4, .background = fill,
                       .width = 0, .grow = 0) {
                    ui_set_size((struct layout_size){ .mode = SIZE_FLEX,
                                                      .flex_grow = frac },
                                (struct layout_size){ .mode = SIZE_FIXED,
                                                      .fixed_value = 8 });
                }
            }
            /* The remainder, so the filled part is a FRACTION of the track and
             * not a fixed width pretending to be one. */
            if (frac < 0.999f) {
                HStack(.height = 8) {
                    ui_set_size((struct layout_size){ .mode = SIZE_FLEX,
                                                      .flex_grow = 1.0f - frac },
                                (struct layout_size){ .mode = SIZE_FIXED,
                                                      .fixed_value = 8 });
                }
            }
        }
    }
}

static void app(void) {
    const struct ui_theme *t = ui_theme();
    sample();

    Window("Activity Monitor") {
        AppBar("Activity Monitor") {
            Spacer();
            bool kt = g_show_kthreads;
            Toggle("Kernel threads", &kt);
            Sync();
            g_show_kthreads = kt;
        }

        VStack(.padding = 16, .spacing = 14, .grow = 1, .align = Fill) {
            /* --- the machine, in two numbers it can actually vouch for --- */
            HStack(.spacing = 24, .grow = 1) {
                { char v[24]; snprintf(v, sizeof v, "%d%%", g_total_pct);
                  /* The TEXT keeps the true number and the BAR is clamped: on
                   * four cores 240% is a real reading, and a bar that runs off
                   * its track is not a bar. */
                  float f = (float)g_total_pct / 100.0f;
                  if (f > 1.0f) f = 1.0f;
                  meter_row("Processor", v, f, t->accent); }

                if (g_mem_ok && g_mem.total_pages) {
                    uint64_t used_mb = (g_mem.used_pages * g_mem.page_size) >> 20;
                    uint64_t tot_mb  = (g_mem.total_pages * g_mem.page_size) >> 20;
                    char v[40];
                    snprintf(v, sizeof v, "%llu of %llu MB",
                             (unsigned long long)used_mb, (unsigned long long)tot_mb);
                    float f = (float)g_mem.used_pages / (float)g_mem.total_pages;
                    meter_row("Memory", v, f, t->success);
                } else {
                    meter_row("Memory", "unavailable", 0.0f, t->border);
                }
            }

            Divider();

            /* --- the processes --- */
            HStack(.spacing = 12, .py = 2, .grow = 1) {
                Text("Process").caption().tertiary().width(190);
                Text("PID").caption().tertiary().width(60);
                Text("CPU").caption().tertiary().width(70);
                Text("State").caption().tertiary().width(80);
                Text("Pri").caption().tertiary();
                Spacer();
            }

            /* A REAL VIEWPORT HEIGHT. ui_scroll_begin sizes its window from
             * this number, and zero means a viewport zero pixels tall: the
             * rows were built, measured and laid out entirely outside it, which
             * looks exactly like a list that failed to populate. The chrome
             * above and below is measured once here rather than guessed. */
            static float scroll_y;
            float list_h = em_viewport_height() - 250.0f;
            if (list_h < 80.0f) list_h = 80.0f;
            ScrollView(&scroll_y, list_h, .grow = 1, .align = Fill) {
                for (int i = 0; i < g_nrows; i++) {
                    struct row *r = &g_rows[i];
                    if (r->is_kthread && !g_show_kthreads) continue;
                    bool sel = (g_selected == r->pid);
                    char pid[16], cpu[16], pri[8];
                    snprintf(pid, sizeof pid, "%u", r->pid);
                    pct_text(cpu, sizeof cpu, r->pct);
                    snprintf(pri, sizeof pri, "%u", (unsigned)r->priority);

                    HStack(.spacing = 12, .py = 5, .px = 6, .corner = 6, .grow = 1,
                           .background = sel ? t->accent_soft : (Color){0,0,0,0}) {
                        /* The row IS the control. There is no list widget with
                         * five columns, and inventing one to get a click would
                         * be a lot of machinery to answer a question the
                         * container can already answer about itself. */
                        struct instance_handle self = ui_open();
                        Text(r->name).width(190);
                        Text(pid).secondary().width(60);
                        Text(cpu).width(70);
                        Text(state_name(r->state)).secondary().width(80);
                        Text(pri).tertiary();
                        Spacer();
                        em_flush();
                        if (ui_consume_click(self)) { g_selected = r->pid; g_status[0] = 0; }
                    }
                }
            }

            Divider();
            HStack(.spacing = 10, .align = Center, .grow = 1) {
                if (g_status[0]) Text(g_status).caption().secondary();
                Spacer();
                /* COUNT WHAT IS ON SCREEN. It said "12 processes" over a list
                 * of six, because kernel threads were filtered out of the list
                 * and not out of the total -- a footer that disagrees with the
                 * thing it is summarising teaches you to distrust both. */
                { int shown = 0;
                  for (int i = 0; i < g_nrows; i++)
                      if (!g_rows[i].is_kthread || g_show_kthreads) shown++;
                  char n[56];
                  if (shown == g_nrows)
                      snprintf(n, sizeof n, "%d process%s", shown, shown == 1 ? "" : "es");
                  else
                      snprintf(n, sizeof n, "%d of %d processes", shown, g_nrows);
                  Text(n).caption().tertiary(); }
                /* QUIT acts on the SELECTION, and says what happened. The kernel
                 * refuses a kernel thread and anything outside this session, and
                 * that refusal is the answer -- so it is reported rather than
                 * swallowed.
                 *
                 * It is ABSENT rather than greyed out when nothing is selected:
                 * a disabled control is a promise you have to read the rest of
                 * the window to understand, and there is nothing here to
                 * explain it with. */
                if (g_selected && Button("Quit Process").danger().clicked()) {
                    int rc = embk_proc_kill(g_selected);
                    if (rc == 0) snprintf(g_status, sizeof g_status,
                                          "Asked %u to quit", g_selected);
                    else if (rc == -EMBK_EPERM)
                        snprintf(g_status, sizeof g_status,
                                 "%u is not this session's to quit", g_selected);
                    else snprintf(g_status, sizeof g_status,
                                  "Could not quit %u (%d)", g_selected, rc);
                    g_selected = 0;
                }
            }
        }
    }
}

EM_APPLICATION {
    .title      = "Activity Monitor",
    .size       = { 720, 520 },
    .chrome     = Chromeless,
    .resize     = Resizable,
    .refresh_ms = 1000,     /* a rate needs a beat, and one second is the beat */
    .view       = app,
};
