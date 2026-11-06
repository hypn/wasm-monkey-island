#ifndef OPL_H
#define OPL_H
#include "scumm.h"
#define OPL_RATE 49716                    /* native YM3812 sample rate (3.579545 MHz / 72) */
void opl_reset(void);
void opl_write(int reg, int val);         /* register write exactly as the game does through ports 0x388/0x389 */
void opl_generate(s16 *out, int n);       /* mono samples at OPL_RATE */
#endif
