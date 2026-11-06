#include "engine.h"
#include "menu.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>

extern int ui_text_w(int cs, const char *s);
extern void ui_text(u8 *dst, int cs, const char *s, int x, int y, int color, int outline);
extern char g_room_names[256][12];
extern int g_cutscene_slot; extern int ui_input_enabled(void);
extern u32 g_rgb[];

static int open_, sel = -1, fresh, editing, hover_btn, ticks_, status_t;
static char editbuf[28], status[48];
static struct { int used; char name[28], room[12], stamp[24]; } slots[MENU_SLOTS];
static u8 L[SCREEN_W * SCREEN_H];

/* palette for the overlay (index 0 = transparent) */
static const u8 P[16][3] = { {0,0,0}, {0,0,0}, {10,14,34}, {20,28,58}, {50,175,185}, {22,92,104}, {255,255,255}, {160,172,200},
	{255,214,90}, {30,142,152}, {64,196,206}, {18,100,112}, {16,22,46}, {44,66,130}, {225,90,90}, {120,224,144} };
enum { C_BLACK = 1, C_PANEL, C_PANEL2, C_BORDER, C_BORDER2, C_WHITE, C_GREY, C_YELLOW, C_BTN, C_BTNHI, C_BTNDN, C_SLOT, C_SLOTSEL, C_RED, C_GREEN };

#define PX 16
#define PY 12
#define PW 288
#define PH 176
#define LX 26
#define LY 40
#define LW 180
#define ROWH 13
static const struct { const char *label; int y; } btn[3] = { { "Save", 44 }, { "Load", 74 }, { "Resume", 104 } };
extern int plat_prompt_name(const char *cur, char *out, int max);    /* touch devices have no keyboard for the canvas: ask the browser */
#define BX 218
#define BW 72
#define BH 22

int menu_is_open(void) { return open_; }

static void load_meta(void)
{
	char m[96];
	for (int i = 0; i < MENU_SLOTS; i++) {
		slots[i].used = 0;
		if (plat_save_meta(i, m, sizeof m) < 0) continue;
		char *a = strchr(m, '|'), *b = a ? strchr(a + 1, '|') : NULL; if (!a || !b) continue;
		*a = 0; *b = 0; slots[i].used = 1;
		snprintf(slots[i].name, sizeof slots[i].name, "%s", m); snprintf(slots[i].room, sizeof slots[i].room, "%s", a + 1); snprintf(slots[i].stamp, sizeof slots[i].stamp, "%s", b + 1);
	}
}
static void say(const char *s) { snprintf(status, sizeof status, "%s", s); status_t = 150; }

void menu_try_open(void)
{
	extern int ui_input_enabled(void);
	if (open_ || !ui_input_enabled() || g_cutscene_slot != 0xff || !g_room) return;
	open_ = 1; editing = 0; status[0] = 0; load_meta();
	if (sel < 0) { sel = 0; for (int i = 0; i < MENU_SLOTS; i++) if (slots[i].used) { sel = i; break; } }      /* remember the last slot */
}
static void close_menu(void) { open_ = 0; editing = 0; }

static void do_save(void)
{
	u8 *buf = NULL; u32 len = 0; char meta[96], stamp[24];
	if (state_save(&buf, &len)) { say("Could not save here"); return; }
	char name[28]; snprintf(name, sizeof name, "%s", editbuf[0] ? editbuf : "Saved game");
	for (char *c = name; *c; c++) if (*c == '|') *c = '/';
	plat_timestamp(stamp, sizeof stamp);
	snprintf(meta, sizeof meta, "%s|%s|%s", name, g_room_names[g_room & 255][0] ? g_room_names[g_room & 255] : "?", stamp);
	if (plat_save_put(sel, meta, buf, len)) say("Saving failed (storage full?)"); else { say("Game saved"); load_meta(); }
	free(buf);
}
static void do_load(void)
{
	if (!slots[sel].used) { say("That slot is empty"); return; }
	u32 cap = 1 << 20; u8 *buf = malloc(cap); int n = plat_save_get(sel, buf, cap);
	int r = n > 0 ? state_load(buf, (u32)n) : -1; free(buf);
	if (r) { say("Could not load that save"); return; }
	close_menu();
}

static int in_btn(int x, int y) { for (int i = 0; i < 3; i++) if (x >= BX && x < BX + BW && y >= btn[i].y && y < btn[i].y + BH) return i; return -1; }
static int in_slot(int x, int y) { if (x < LX || x >= LX + LW || y < LY || y >= LY + ROWH * MENU_SLOTS) return -1; return (y - LY) / ROWH; }

void menu_click(int x, int y, int button)
{
	if (button == 2) { if (editing) editing = 0; else close_menu(); return; }          /* right click = back */
	int s = in_slot(x, y), b = in_btn(x, y);
	if (s >= 0) { sel = s; editing = 0; return; }
	if (editing && b == 0) { editing = 0; do_save(); return; }                          /* Okay */
	if (editing && b == 1) { editing = 0; return; }                                      /* Cancel */
	if (b == 0) {                                                                       /* Save: ask for a name */
		{ char cur[28], out[28]; snprintf(cur, sizeof cur, "%s", slots[sel].used ? slots[sel].name : (g_room_names[g_room & 255][0] ? g_room_names[g_room & 255] : "Saved game"));
		  int r = plat_prompt_name(cur, out, sizeof out);
		  if (r == 1) { snprintf(editbuf, sizeof editbuf, "%s", out); do_save(); return; } if (r == 0) return; }
		editing = 1; fresh = 1; snprintf(editbuf, sizeof editbuf, "%s", slots[sel].used ? slots[sel].name : (g_room_names[g_room & 255][0] ? g_room_names[g_room & 255] : "Saved game"));
		if (editbuf[0] >= 'a' && editbuf[0] <= 'z') editbuf[0] -= 32;
	} else if (b == 1) { editing = 0; do_load(); }
	else if (b == 2) close_menu();
	else if (editing) editing = 0;
}

void menu_key(int k)
{
	if (editing) {
		if (k == 27) editing = 0;
		else if (k == 13) { editing = 0; do_save(); }
		else if (k == 8) { int n = (int)strlen(editbuf); if (n) editbuf[n - 1] = 0; }
		else if (k >= 32 && k < 127 && (fresh || strlen(editbuf) < 24)) { if (fresh) editbuf[0] = 0; fresh = 0; int n = (int)strlen(editbuf); editbuf[n] = (char)k; editbuf[n + 1] = 0; }
		return;
	}
	if (k == 27 || k == 319) close_menu();
	else if (k == 13) { editing = 1; fresh = 1; snprintf(editbuf, sizeof editbuf, "%s", slots[sel].used ? slots[sel].name : "Saved game"); }
}

void menu_tick(void) { ticks_++; if (status_t > 0) status_t--; hover_btn = in_btn(g_mouse_x, g_mouse_y); }

/* ---- drawing ---- */
static void rect(int x, int y, int w, int h, int c) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (i >= 0 && i < SCREEN_W && j >= 0 && j < SCREEN_H) L[j * SCREEN_W + i] = (u8)c; }
static void frame(int x, int y, int w, int h, int c) { rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c); rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c); }
static void round_corners(int x, int y, int w, int h)         /* knock the corner pixels out of a filled rect */
{
	static const int cut[3][3] = { {1,1,1}, {1,1,0}, {1,0,0} };
	for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) if (cut[j][i]) {
		L[(y + j) * SCREEN_W + x + i] = 0; L[(y + j) * SCREEN_W + x + w - 1 - i] = 0;
		L[(y + h - 1 - j) * SCREEN_W + x + i] = 0; L[(y + h - 1 - j) * SCREEN_W + x + w - 1 - i] = 0;
	}
}
static void ctext(const char *s, int cs, int cx, int y, int col) { ui_text(L, cs, s, cx - ui_text_w(cs, s) / 2, y, col, C_BLACK); }

void menu_draw(u32 *rgb)
{
	if (!open_) return;
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) { u32 c = rgb[i]; rgb[i] = 0xFF000000u | ((((c >> 16) & 255) * 80 >> 8) << 16) | ((((c >> 8) & 255) * 80 >> 8) << 8) | ((c & 255) * 80 >> 8); }
	memset(L, 0, sizeof L);
	rect(PX, PY, PW, PH, C_PANEL); frame(PX, PY, PW, PH, C_BORDER); frame(PX + 2, PY + 2, PW - 4, PH - 4, C_BORDER2);
	round_corners(PX, PY, PW, PH);
	ctext("Game Menu", 2, PX + PW / 2, PY + 8, C_WHITE);
	rect(PX + 10, PY + 22, PW - 20, 1, C_BORDER2);
	/* slots */
	for (int i = 0; i < MENU_SLOTS; i++) {
		int y = LY + i * ROWH, issel = i == sel;
		rect(LX, y, LW, ROWH - 1, issel ? C_SLOTSEL : C_SLOT);
		if (issel) frame(LX, y, LW, ROWH - 1, C_BORDER);
		char num[8]; snprintf(num, sizeof num, "%d", i + 1);
		ui_text(L, 1, num, LX + 4, y + 2, C_GREY, C_BLACK);
		if (editing && issel) {
			char t[40]; snprintf(t, sizeof t, "%s%s", editbuf, (ticks_ / 8) & 1 ? "_" : " ");
			ui_text(L, 1, t, LX + 22, y + 2, C_YELLOW, C_BLACK);
		} else if (slots[i].used) {
			ui_text(L, 1, slots[i].name, LX + 22, y + 2, C_WHITE, C_BLACK);
			int w = ui_text_w(1, slots[i].room); ui_text(L, 1, slots[i].room, LX + LW - 4 - w, y + 2, C_GREY, C_BLACK);
		} else ui_text(L, 1, "- empty -", LX + 22, y + 2, C_GREY, C_BLACK);
	}
	/* buttons */
	for (int i = 0; i < 3; i++) {
		int y = btn[i].y, down = hover_btn == i && (g_mouse_buttons & 1), c = down ? C_BTNDN : hover_btn == i ? C_BTNHI : C_BTN;
		int dis = (i == 1 && !editing && !slots[sel].used);
		rect(BX, y, BW, BH, dis ? C_SLOT : c); if (!dis) rect(BX + 2, y + 1, BW - 4, 1, C_BORDER);
		frame(BX, y, BW, BH, dis ? C_BORDER2 : C_BORDER2); round_corners(BX, y, BW, BH);
		ctext(editing && i == 0 ? "Okay" : editing && i == 1 ? "Cancel" : btn[i].label, 2, BX + BW / 2, y + 7, dis ? C_GREY : C_WHITE);
	}
	/* status / hint line */
	if (status_t > 0) ui_text(L, 1, status, LX, 176, C_GREEN, C_BLACK);
	else ui_text(L, 1, editing ? "Type a name, then Okay (or Enter)" : "F5, Esc or right click: back to the game", LX, 176, C_GREY, C_BLACK);
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) if (L[i]) { const u8 *c = P[L[i]]; rgb[i] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2]; }
}
