/* UI: charset text, talk messages, verbs, strings, cursor, input routing and the compositor. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>

#define VIEW_TOP 16
int g_view_top = VIEW_TOP, g_view_h = 144;

u8 g_ui[SCREEN_W * SCREEN_H];          /* persistent bitmap for the non-room rows (text line, verbs) */
u8 g_txt[SCREEN_W * SCREEN_H];         /* transient text overlay (0xFF = transparent) */
u32 g_rgb[SCREEN_W * SCREEN_H];
extern u8 g_room_pal[16];
extern int vm_pop_sentence(int *, int *, int *);
int g_charset_id = 1;

/* ---------- charset ---------- */
static const u8 *cs_glyph(int cs, int ch, int *w, int *h, int *xo, int *yo)
{
	const u8 *d = g_charset[cs]; if (!d) return NULL;
	const u8 *base = d + 21;
	u32 off = rd32(base + 4 + 4 * ch);
	if (!off) return NULL;
	const u8 *g = base + off;
	*w = g[0]; *h = g[1]; *xo = (s8)g[2]; *yo = (s8)g[3];
	return g + 4;
}
static int cs_height(int cs) { return g_charset[cs] ? g_charset[cs][22] : 8; }
static int cs_char_w(int cs, int ch) { int w, h, xo, yo; if (!cs_glyph(cs, ch, &w, &h, &xo, &yo)) return 0; return w + xo; }
static int cs_string_w(int cs, const u8 *s, int len)
{
	int w = 1;
	for (int i = 0; i < len && s[i] && s[i] != 0xD; i++) {
		if (s[i] == 0xFF || s[i] == 0xFE) { u8 c = s[++i]; if (c == 1 || c == 3 || c == 8) return w; continue; }
		if (s[i] == '@') continue;
		w += cs_char_w(cs, s[i]);
	}
	return w;
}
static void cs_draw_char(u8 *dst, int dst_w, int dst_h, int cs, int ch, int x, int y, int color)
{
	int w, h, xo, yo; const u8 *g = cs_glyph(cs, ch, &w, &h, &xo, &yo); if (!g) return;
	x += xo; y += yo;
	for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) {
		int bit = r * w + c;                      /* glyph bits are one continuous stream (no row padding) */
		if (g[bit >> 3] & (0x80 >> (bit & 7))) {
			int px = x + c, py = y + r;
			if (px >= 0 && px < dst_w && py >= 0 && py < dst_h) dst[py * dst_w + px] = (u8)color;
		}
	}
}

/* ---------- text: message state ---------- */
static u8 msg[512]; static int msg_len, msg_pos;
static int msg_state;                  /* 0 idle, 0xFF pending, 1 displayed */
static int msg_timer, msg_actor, msg_color, msg_x, msg_y, msg_center, msg_keep;
static int msg_right = 319;
static u8 msg_buf_layer_dirty;

static void expand_codes(const u8 *in, int len, u8 *out, int *olen)
{
	int o = 0;
	for (int i = 0; i < len && o < 500; i++) {
		u8 c = in[i];
		if (c == 0xFF) {
			u8 code = in[++i];
			if (code >= 4 && code <= 7) {
				int var = g_vars[((s16)rd16(in + i + 1)) & 0x3FF]; i += 2;
				char tmp[16];
				if (code == 4) { snprintf(tmp, sizeof tmp, "%d", var); for (char *p = tmp; *p; p++) out[o++] = (u8)*p; }
				else if (code == 6) { const u8 *n = eng_obj_name(var); for (; *n && o < 500; n++) if (*n != '@') out[o++] = *n; }
				else if (code == 5) { extern const u8 *eng_verb_name(int); const u8 *n = eng_verb_name(var); if (n) for (; *n && o < 500; n++) out[o++] = *n; }
				else { extern const u8 *eng_string_ptr(int); const u8 *n = eng_string_ptr(var); if (n) for (; *n && o < 500; n++) out[o++] = *n; }
			} else { out[o++] = 0xFF; out[o++] = code; }
			continue;
		}
		if (c == '@') continue;
		if (c == 0) break;
		out[o++] = c;
	}
	out[o] = 0; *olen = o;
}

void txt_clear(void)
{
	if (msg_state || msg_timer) {
		memset(g_txt, 0xFF, sizeof g_txt);
		if (g_vars[VAR_TALK_ACTOR] && g_vars[VAR_TALK_ACTOR] < 13) {
			Actor *a = &g_actors[g_vars[VAR_TALK_ACTOR]];
			if (a->room == g_room) act_start_anim(g_vars[VAR_TALK_ACTOR], a->talk_stop * 4 + a->facing);
		}
	}
	msg_state = 0; msg_timer = 0; msg_keep = 0; g_vars[VAR_HAVE_MSG] = 0; g_vars[VAR_TALK_ACTOR] = 0;
}

/* draw a (possibly multi-line) string into g_txt (or g_ui); returns */
static void draw_lines(u8 *dst, int dst_h, const u8 *s, int x, int y, int color, int center, int right, int cs, int y_base)
{
	int h = cs_height(cs); (void)y_base;
	/* word wrap */
	u8 line[512]; int ll = 0;
	int maxw = right - x; if (center) { int half = x < (320 - x) ? x : (320 - x); maxw = half * 2; }
	if (maxw < 40) maxw = 40;
	int cx = x, cy = y;
	const u8 *p = s;
	while (*p) {
		/* take next line: up to explicit newline or width */
		ll = 0; int lastsp = -1; int w = 1; const u8 *q = p;
		while (*q) {
			if (*q == 0xFF) { u8 c = q[1]; if (c == 1 || c == 8 || c == 3 || c == 2) { q += 2; break; } q += 2; continue; }
			int cw = cs_char_w(cs, *q);
			if (w + cw > maxw && lastsp >= 0) { ll = lastsp; q = p + lastsp + 1; goto emit; }
			if (*q == ' ') lastsp = ll;
			line[ll++] = *q; w += cw; q++;
		}
		/* fallthrough: line ended by newline/end */
	emit:
		line[ll] = 0;
		{
			int lw = cs_string_w(cs, line, ll);
			int lx = center ? x - lw / 2 : x;
			if (lx < 0) lx = 0; if (lx + lw > 320) lx = 320 - lw; if (lx < 0) lx = 0;
			int px = lx;
			for (int i = 0; i < ll; i++) { cs_draw_char(dst, SCREEN_W, dst_h, cs, line[i], px, cy, color); px += cs_char_w(cs, line[i]); }
			cy += h; (void)cx;
		}
		p = q;
	}
}

void txt_print(int slot, int actor, const u8 *text, int len, int x, int y, int color, int center, int overhead, int left, int clip, int have_pos)
{
	(void)left; (void)have_pos;
	u8 buf[512]; int n; expand_codes(text, len, buf, &n);
	int cs = g_charset_id;
	if (slot == 0) {
		if (msg_state == 0xFF || msg_state == 1) txt_clear();
		if (actor == 0xFF) { g_vars[VAR_TALK_ACTOR] = 0xFF; }
		else if (actor < 13 && g_actors[actor].room == g_room) {
			Actor *a = &g_actors[actor]; g_vars[VAR_TALK_ACTOR] = actor;
			act_start_anim(actor, a->talk_start * 4 + a->facing);
		} else g_vars[VAR_TALK_ACTOR] = 0xFF;
		int sx = x, sy = y;
		if (overhead && actor < 13) {
			Actor *a = &g_actors[actor];
			sx = a->x - g_cam_cur + 160; if (sx < 80) sx = 80; if (sx > 240) sx = 240;
			sy = a->y + a->elevation - 80; if (sy < 1) sy = 1; sy += g_view_top;
		}
		memcpy(msg, buf, n + 1); msg_len = n; msg_pos = 0;
		msg_x = sx; msg_y = sy; msg_color = color; msg_center = center; msg_actor = actor; msg_right = clip ? clip : 319;
		msg_state = 0xFF; msg_timer = 0; g_vars[VAR_HAVE_MSG] = 0xFF;
		if (actor < 13 && actor != 0xFF && g_actors[actor].talk_color && color == 0) {}
		if (!color && actor < 13) msg_color = g_actors[actor].talk_color;
		return;
	}
	if (g_trace || getenv("TRACE_TXT")) fprintf(stderr, "   txt_print slot=%d actor=%d x=%d y=%d col=%d '%s'\n", slot, actor, x, y, color, buf);
	if (slot == 1) {
		int ty = g_view_top + y;
		if (y >= 0 && y < 200) {
			/* text on the UI area is persistent; elsewhere on the transient overlay */
			if (ty >= g_view_top + g_view_h || y < g_view_top) draw_lines(g_ui, SCREEN_H, buf, x, y, color, center, clip ? clip : 319, cs, 0);
			else draw_lines(g_txt, SCREEN_H, buf, x, ty, color, center, clip ? clip : 319, cs, 0);
		}
	}
}

void txt_update(void)                        /* FUN_1000_9b64 */
{
	if (msg_state == 0xFF && msg_timer == 0) {
		/* display */
		u8 stripped[512]; int sl = 0, i = 0; int keep_after = 0; int wait_after = 0;
		for (i = 0; i < msg_len; i++) {
			if (msg[i] == 0xFF) { u8 c = msg[++i]; if (c == 3) { wait_after = 1; break; } if (c == 2) { keep_after = 1; break; } if (c == 1 || c == 8) { stripped[sl++] = 0xFF; stripped[sl++] = 1; } continue; }
			stripped[sl++] = msg[i];
		}
		stripped[sl] = 0;
		memset(g_txt, 0xFF, sizeof g_txt);
		draw_lines(g_txt, SCREEN_H, stripped, msg_x, msg_y, msg_color, msg_center, msg_right, g_charset_id, 0);
		msg_timer = 60 + 4 * (g_vars[VAR_CHARINC] ? g_vars[VAR_CHARINC] : 4) / 4 * sl; (void)msg_timer;
		msg_timer = 60 + (g_vars[VAR_CHARINC] ? g_vars[VAR_CHARINC] : 4) * sl;
		if (wait_after) { memmove(msg, msg + i + 1, msg_len - i); msg_len -= i + 1; msg_state = 0xFF; g_vars[VAR_HAVE_MSG] = 0xFF; }
		else { msg_state = 1; g_vars[VAR_HAVE_MSG] = 1; msg_keep = keep_after; }
	}
}
void ui_msg_timer(int ticks)
{
	if (msg_timer > 0) { msg_timer -= ticks; if (msg_timer < 0) msg_timer = 0; }
	if (msg_state == 1 && msg_timer == 0) {
		if (msg_keep) { msg_state = 0; g_vars[VAR_HAVE_MSG] = 0; msg_keep = 0; }
		else txt_clear();
	}
}
void ui_skip_msg(void) { if (msg_state == 1) { msg_timer = 0; ui_msg_timer(0); } }

/* ---------- strings ---------- */
static u8 g_strings[NUM_STRINGS][256];
void eng_string_op(int sub, int a, int b, int c, const u8 *text)
{
	if (a < 0 || a >= NUM_STRINGS) return;
	if (sub == 1) { u8 buf[256]; int n; expand_codes(text, (int)strlen((const char *)text), buf, &n); (void)n; strncpy((char *)g_strings[a], (const char *)text, 255); }
	else if (sub == 2) { if (b >= 0 && b < NUM_STRINGS) memcpy(g_strings[a], g_strings[b], 256); }
	else if (sub == 3) { if (b >= 0 && b < 255) g_strings[a][b] = (u8)c; }
	else if (sub == 5) { memset(g_strings[a], 0, 256); if (b > 0 && b < 255) memset(g_strings[a], ' ', b); }
}
int eng_string_get_char(int s, int i) { if (s < 0 || s >= NUM_STRINGS || i < 0 || i > 255) return 0; return g_strings[s][i]; }
const u8 *eng_string_ptr(int s) { if (s < 0 || s >= NUM_STRINGS) return NULL; return g_strings[s]; }
int eng_get_string_len(int s) { if (s < 0 || s >= NUM_STRINGS) return 0; return cs_string_w(g_charset_id, g_strings[s], (int)strlen((char *)g_strings[s])); }

/* ---------- verbs ---------- */
typedef struct { u8 used, id, on, color, hicolor, dimcolor, key, center; s16 x, y; u8 name[128]; u16 image; u8 saveid; u8 back; s16 lx, lw; u8 lvalid; } VerbSlot;
static VerbSlot verbs[NUM_VERBS + 1];
static int find_verb(int id, int create)
{
	for (int i = 1; i < NUM_VERBS; i++) if (verbs[i].used && verbs[i].id == id && !verbs[i].saveid) return i;   /* saved verbs are invisible to lookups */
	if (!create) return 0;
	for (int i = 1; i < NUM_VERBS; i++) if (!verbs[i].used) return i;
	return 0;
}
static void verb_text(const VerbSlot *v, u8 *out, int *n);
static u8 vn_tmp[512];
const u8 *eng_verb_name(int id) { int v = find_verb(id, 0); if (!v) return NULL; int n; verb_text(&verbs[v], vn_tmp, &n); return vn_tmp; }
static void verb_text(const VerbSlot *v, u8 *out, int *n)
{
	int l = 0; while (l < 127 && v->name[l]) { if (v->name[l] == 0xFF) l += (v->name[l + 1] >= 4 && v->name[l + 1] <= 7) ? 4 : 2; else l++; }
	expand_codes(v->name, l, out, n);
}
static void verb_erase_rect(VerbSlot *v)
{
	u8 t[512]; int tn; verb_text(v, t, &tn);
	int w = cs_string_w(g_charset_id, t, tn); int h = cs_height(g_charset_id);
	int x = v->center ? v->x - w / 2 : v->x;
	if (v->lvalid) { if (v->lx < x) { w += x - v->lx; x = v->lx; } if (v->lx + v->lw > x + w) w = v->lx + v->lw - x; }
	for (int yy = v->y; yy < v->y + h && yy < SCREEN_H; yy++) for (int xx = x; xx < x + w && xx < SCREEN_W; xx++) if (xx >= 0 && yy >= 0) g_ui[yy * SCREEN_W + xx] = 0;
	v->lvalid = 0;
}
static void verb_erase_rect(VerbSlot *v);
static void verb_draw(VerbSlot *v, int hover)
{
	if (!v->used || !v->on || v->saveid || !v->name[0]) return;
	int color = v->on == 2 ? v->dimcolor : (hover ? v->hicolor : v->color);
	u8 t[512]; int tn; verb_text(v, t, &tn);
	int w = cs_string_w(g_charset_id, t, tn);
	int x = v->center ? v->x - w / 2 : v->x;
	verb_erase_rect(v);
	v->lx = (s16)x; v->lw = (s16)w; v->lvalid = 1;
	int px = x;
	for (const u8 *p = t; *p; p++) {
		if (*p == 0xFF) { p++; continue; }
		cs_draw_char(g_ui, SCREEN_W, SCREEN_H, g_charset_id, *p, px, v->y, color); px += cs_char_w(g_charset_id, *p);
	}
}
void eng_verb_op(int verb, int sub, int a, int b, const u8 *text, int len, int color)
{
	(void)color; (void)len;
	if (sub == 0xFF) return;
	if (sub == 9) { int s = find_verb(verb, 1); if (!s) return; VerbSlot *v = &verbs[s]; memset(v, 0, sizeof *v); v->used = 1; v->id = verb; v->color = 2; v->dimcolor = 8; return; }
	int s = find_verb(verb, 0); if (!s) return; VerbSlot *v = &verbs[s];
	switch (sub) {
	case 1: v->image = a; break;
	case 2: { verb_erase_rect(v); if (len > 127) len = 127; memcpy(v->name, text, len); v->name[len] = 0; break; }
	case 3: v->color = a; break;
	case 4: v->hicolor = a; break;
	case 5: verb_erase_rect(v); v->x = a; v->y = b; break;
	case 6: v->on = 1; break;
	case 7: verb_erase_rect(v); v->on = 0; break;
	case 8: verb_erase_rect(v); memset(v, 0, sizeof *v); break;
	case 0x10: v->dimcolor = a; break;
	case 0x11: v->on = 2; break;
	case 0x12: v->key = a; break;
	case 0x13: v->center = 1; break;
	case 0x14: { const u8 *sp = eng_string_ptr(a); verb_erase_rect(v); if (sp) strncpy((char *)v->name, (const char *)sp, 127); break; }
	default: break;
	}
}
void ui_save_restore_verbs(int sub, int start, int end, int mode)     /* handler 8b90 */
{
	for (int id = start; id <= end; id++) {
		if (sub == 1) {                       /* save: hide the live verb */
			int s = find_verb(id, 0);
			if (s) { verb_erase_rect(&verbs[s]); verbs[s].saveid = (u8)mode; }
		} else if (sub == 2) {                /* restore: drop any live verb with this id, bring the saved one back */
			for (int i = 1; i < NUM_VERBS; i++) if (verbs[i].used && verbs[i].id == id && verbs[i].saveid == mode) {
				int live = find_verb(id, 0); if (live) { verb_erase_rect(&verbs[live]); memset(&verbs[live], 0, sizeof verbs[live]); }
				verbs[i].saveid = 0; verbs[i].lvalid = 0; break;
			}
		} else if (sub == 3) {                /* delete saved */
			for (int i = 1; i < NUM_VERBS; i++) if (verbs[i].used && verbs[i].id == id && verbs[i].saveid == mode) memset(&verbs[i], 0, sizeof verbs[i]);
		}
	}
}
static int verb_at(int x, int y)
{
	for (int i = 1; i < NUM_VERBS; i++) {
		VerbSlot *v = &verbs[i]; if (!v->used || !v->on || v->saveid || !v->name[0]) continue;
		u8 t[512]; int tn; verb_text(v, t, &tn);
		int w = cs_string_w(g_charset_id, t, tn); int h = cs_height(g_charset_id);
		int vx = v->center ? v->x - w / 2 : v->x;
		if (x >= vx && x < vx + w && y >= v->y && y < v->y + h) return v->id;
	}
	return 0;
}

/* ---------- cursor / input ---------- */
static int cursor_on = 1, userput = 0;
int g_cursor_state;
void ui_cursor_cmd(int sub, int a, int b, int c)
{
	switch (sub) {
	case 1: cursor_on = 1; break; case 2: cursor_on = 0; break;
	case 3: userput = 1; break; case 4: userput = 0; break;
	case 5: cursor_on++; break; case 6: cursor_on--; break;
	case 7: userput++; break; case 8: userput--; break;
	case 12: if (a >= 0 && a < 10 && g_charset[a]) g_charset_id = a; break;
	default: break;
	}
	g_vars[VAR_CURSORSTATE] = (s16)cursor_on; g_vars[VAR_USERPUT] = (s16)userput;
	(void)b; (void)c;
}
void ui_set_screen(int top, int height) { g_view_top = top; g_view_h = height; if (g_view_h > 200 - top) g_view_h = 200 - top; if (g_view_h <= 0) g_view_h = 144; }

static int pending_click, click_x, click_y, click_btn, pending_key;
void eng_click(int x, int y, int btn) { pending_click = 1; click_x = x; click_y = y; click_btn = btn; g_mouse_x = x; g_mouse_y = y; }
void eng_key(int key) { pending_key = key; g_key_pressed = key; }

static void run_verb_script(int area, int val, int mode)
{
	if (!g_vars[VAR_VERB_SCRIPT]) return;
	s16 a[16] = { (s16)area, (s16)val, (s16)mode };
	vm_start_script(g_vars[VAR_VERB_SCRIPT], 0, 0, a);
}
static int sentence_step_pending;

int ui_tick(void)
{
	ui_msg_timer(g_vars[VAR_TIMER_NEXT] > 0 ? g_vars[VAR_TIMER_NEXT] : 4);
	if (pending_key) {
		int k = pending_key; pending_key = 0;
		if (k == g_vars[VAR_CUTSCENEEXIT_KEY] && g_vars[VAR_CUTSCENEEXIT_KEY]) { extern void vm_do_override(void); vm_do_override(); g_vars[VAR_OVERRIDE] = 1; txt_clear(); }
		else if (k == '.' || k == g_vars[VAR_TALKSTOP_KEY]) ui_skip_msg();
		else if (cursor_on > 0 || userput > 0) {
			int v = 0; for (int i = 1; i < NUM_VERBS; i++) if (verbs[i].used && verbs[i].on == 1 && verbs[i].key == k) v = verbs[i].id;
			if (v) run_verb_script(1, v, 1); else run_verb_script(4, k, 1);
		}
	}
	if (pending_click) {
		pending_click = 0;
		int mode = click_btn == 2 ? 2 : 1;
		if (cursor_on > 0 || userput > 0) {
			int vx = click_x, vy = click_y;
			if (vy < g_view_top || vy >= g_view_top + g_view_h) { int v = verb_at(vx, vy); run_verb_script(1, v, mode); }
			else {
				int v = verb_at(vx, vy);
				if (v) run_verb_script(1, v, mode); else run_verb_script(2, 0, mode);
			}
		}
		if (g_vars[VAR_TALK_ACTOR]) ui_skip_msg();
	}
	(void)sentence_step_pending;
	/* sentence dispatcher (FUN_1000_a3a0) */
	{
		int run = 0; for (int i = 0; i < NUM_SLOTS; i++) if (g_slots[i].status && g_slots[i].where == WIO_GLOBAL && g_slots[i].number == g_vars[VAR_SENTENCE_SCRIPT]) run = 1;
		if (!run && g_vars[VAR_SENTENCE_SCRIPT]) {
			int verb, a, b;
			if (vm_pop_sentence(&verb, &a, &b)) { s16 args[16] = { (s16)verb, (s16)a, (s16)b }; vm_start_script(g_vars[VAR_SENTENCE_SCRIPT], 0, 0, args); }
		}
	}
	return 0;
}
int eng_sentence_idle(void) { extern int vm_sentence_pending(void); return !vm_sentence_pending(); }

/* ---------- compositor ---------- */
extern void costume_draw_actors(u8 *screen, int cam_x, int view_top);
static int hover_x, hover_y;
void render_frame(void)
{
	extern int g_room_w;
	u8 *scr = g_screen;
	memset(scr, 0, sizeof g_screen);
	/* room view */
	if (g_room && g_bg) {
		int camx = g_cam_cur - 160; if (camx < 0) camx = 0; if (camx > g_room_w - 320) camx = g_room_w - 320; if (camx < 0) camx = 0;
		for (int y = 0; y < g_view_h && y < g_room_h; y++) {
			int sy = g_view_top + y; if (sy >= SCREEN_H) break;
			int w = g_room_w - camx; if (w > 320) w = 320;
			memcpy(scr + sy * SCREEN_W, g_bg + y * g_room_w + camx, w);
		}
		costume_draw_actors(scr, camx, g_view_top);
	}
	/* persistent UI rows */
	for (int y = 0; y < SCREEN_H; y++) {
		int in_view = y >= g_view_top && y < g_view_top + g_view_h;
		if (in_view && g_room) continue;
		for (int x = 0; x < SCREEN_W; x++) { u8 p = g_ui[y * SCREEN_W + x]; if (p) scr[y * SCREEN_W + x] = p; }
	}
	/* verbs (hover colour) */
	{ int hv = verb_at(g_mouse_x, g_mouse_y); for (int i = 1; i < NUM_VERBS; i++) if (verbs[i].used && verbs[i].on) verb_draw(&verbs[i], verbs[i].id == hv); }
	/* text overlay */
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) if (g_txt[i] != 0xFF) scr[i] = g_txt[i];
#ifndef JS_CURSOR
	/* cursor: simple cross (the web build uses a CSS cursor so it tracks the mouse at display rate) */
	if (cursor_on > 0) {
		int cx = g_mouse_x, cy = g_mouse_y;
		for (int d = -4; d <= 4; d++) {
			if (cx + d >= 0 && cx + d < SCREEN_W && cy >= 0 && cy < SCREEN_H) scr[cy * SCREEN_W + cx + d] = 15;
			if (cy + d >= 0 && cy + d < SCREEN_H && cx >= 0 && cx < SCREEN_W) scr[(cy + d) * SCREEN_W + cx] = 15;
		}
	}
#endif
	(void)hover_x; (void)hover_y;
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
		const u8 *c = g_ega_palette[g_room_pal[scr[i] & 15] & 15];
		g_rgb[i] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
	}
}
void ui_init(void) { memset(g_txt, 0xFF, sizeof g_txt); }

int ui_cursor_visible(void) { return cursor_on > 0; }
