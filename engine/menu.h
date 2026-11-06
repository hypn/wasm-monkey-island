#ifndef MENU_H
#define MENU_H
#include "scumm.h"
/* In-game save/load menu (our own; the original F5 dialog is a plain EXE-native box). */
int  menu_is_open(void);
void menu_try_open(void);
void menu_key(int key);
void menu_click(int x, int y, int button);
void menu_tick(void);
void menu_draw(u32 *rgb);                       /* dims the game and draws the overlay */
/* platform storage (browser localStorage / files for the native test harness); slots 0..MENU_SLOTS-1 */
#define MENU_SLOTS 10
int  plat_save_put(int slot, const char *meta, const u8 *data, u32 len);     /* 0 = ok */
int  plat_save_meta(int slot, char *out, int max);                             /* length, or <0 if empty */
int  plat_save_get(int slot, u8 *out, u32 max);                                /* length, or <0 */
void plat_timestamp(char *out, int max);
#endif
