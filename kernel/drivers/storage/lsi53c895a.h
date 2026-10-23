/* The controller that runs a program: SCRIPTS, fetched from host memory. The
 * driver writes the program; the chip executes it. See the .c. */
#ifndef _EMBK_LSI53C895A_H_
#define _EMBK_LSI53C895A_H_
#include <stdint.h>
#include "include/types.h"
bool     lsi53c895a_init(void);
bool     lsi53c895a_present(void);
uint32_t lsi53c895a_target_count(void);
#endif
