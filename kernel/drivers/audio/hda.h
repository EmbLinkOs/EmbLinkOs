#ifndef EMBLINK_HDA_H
#define EMBLINK_HDA_H
#include <stdint.h>
#include "include/types.h"
void hda_init(void);
bool hda_present(void);
uint32_t hda_fill(int,const int16_t *,uint32_t);
uint8_t hda_civ(void);
void hda_play(int);
void hda_set_last(int);
bool hda_done(int);
void hda_stop(void);
const char *hda_output_name(void);
void hda_irq_handler(void);
#endif
