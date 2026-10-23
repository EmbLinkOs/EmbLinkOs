/* Why the machine stopped, readable after it starts again.
 *
 * The panic path is the whole difficulty: no allocation, no locks, no page
 * tables, bounded everywhere. And what goes in the record is not just the
 * registers -- those say WHERE it stopped and almost never why. See the .c. */
#ifndef _EMBK_CRASHLOG_H_
#define _EMBK_CRASHLOG_H_
#include <stdint.h>
#include "include/types.h"

/* Everything worth keeping about a fault, gathered by the caller so this
 * takes no architecture-specific structure. */
struct crash_state {
    uint32_t vector;
    uint64_t error_code;
    uint64_t rip, rsp, rbp, rflags, cr2, cr3, cs, ss;
    uint32_t cpu;
    uint32_t pid;
    uint64_t uptime_ms;
};

/* Called from the panic path, with the panic lock already held. */
void crashlog_capture(const struct crash_state *st);

/* Called once at boot, after erst_init(): prints the previous crash if there
 * was one, and keeps it for anything that asks. */
void crashlog_report(void);
bool crashlog_load_previous(void);
bool crashlog_have_previous(void);
uint32_t    crashlog_previous_vector(void);
const char *crashlog_previous_symbol(void);
const char *crashlog_previous_log(void);
int  crashlog_clear(void);
#endif
