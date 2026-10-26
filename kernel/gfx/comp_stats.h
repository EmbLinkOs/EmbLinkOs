#ifndef __GFX_COMP_STATS_H__
#define __GFX_COMP_STATS_H__

/* kernel/gfx/comp_stats.h -- the compositor's own account of what it costs
 * (the ABI is user/lib/embk_comp_stats.h).
 *
 * The compositor charges every compose to a cause and a duration; this keeps
 * the totals, a ring of the recent ones for percentiles, and the pointer's
 * motion-to-screen latency. It has its own lock, so a reader (the syscall, the
 * serial command) never has to hold the compositor's. */

#include <stdint.h>
#include "../../user/lib/embk_comp_stats.h"

/* One compose of `px` screen pixels charged to `src`, from t0 to t1 (ns). */
void comp_stats_record(unsigned src, uint64_t t0_ns, uint64_t t1_ns, uint64_t px, int full_screen);
/* Window `title` of `pid` presented the w x h rect at (x, y) of its content:
 * who is asking, not only why, and where. Kept for the few windows presenting
 * most since the last reset, with the rect each presented last. */
void comp_stats_present(int pid, const char *title, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* What the display can do and the rate chosen for it (frameclock.h). */
void comp_stats_rate(uint32_t display_mhz, uint32_t vrr_min_mhz, uint32_t vrr_max_mhz,
                     uint32_t rate_mhz, uint32_t frame_cost_us);
/* The frame thread began a beat's frame `late_ns` after it was due. */
void comp_stats_late(uint64_t late_ns);
/* The frame thread finished a frame at `t_ns`, keeping a beat of `period_ns`. */
void comp_stats_frame(uint64_t t_ns, uint64_t period_ns);
/* The pointer moved at some instant and a compose showed it `ns` later. */
void comp_stats_pointer(uint64_t ns);
void comp_stats_snapshot(struct embk_comp_stats *out);
void comp_stats_reset(void);

/* The overlay's text, one line per row, from a fresh snapshot. Returns rows. */
#define COMP_STATS_HUD_ROWS 5
#define COMP_STATS_HUD_COLS 30
int  comp_stats_hud_text(char rows[COMP_STATS_HUD_ROWS][COMP_STATS_HUD_COLS + 1]);

/* The serial console's `perf`, `perf reset`, `perf hud`: 1 if it was one. */
int  comp_stats_command(const char *cmd);

#endif
