/* Error records that survive the reboot, written without touching the
 * operating system's own storage stack -- which may be what panicked.
 *
 * The unusual part: the ACPI table does not describe a device, it contains a
 * PROGRAM, and the OS is the interpreter. See the .c. */
#ifndef _EMBK_ERST_H_
#define _EMBK_ERST_H_
#include <stdint.h>
#include "include/types.h"
bool     erst_init(void);
bool     erst_present(void);
uint64_t erst_buffer_size(void);
uint64_t erst_record_count(void);
uint64_t erst_first_record(void);
uint64_t erst_writes(void);
int      erst_write(uint64_t record_id, const void *data, uint32_t len);
int      erst_read(uint64_t record_id, void *out, uint32_t cap);
int      erst_clear(uint64_t record_id);
#endif
