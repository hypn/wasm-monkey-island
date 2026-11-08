/* WebAssembly platform layer: the page passes the demo's data files (fetched or embedded)
 * and drives the engine one frame at a time. */
#include "engine.h"
#include <stdlib.h>
#include <stdarg.h>
#include <emscripten.h>

extern u32 g_rgb[]; extern void eng_frame(void); extern void eng_init(void);
static u8 rgba[SCREEN_W * SCREEN_H * 4];

EMSCRIPTEN_KEEPALIVE u8 *mi_alloc(u32 n) { return malloc(n); }

EMSCRIPTEN_KEEPALIVE int mi_init(u8 *lec, u32 lec_n, u8 *idx, u32 idx_n)
{
	int r = res_init(lec, lec_n, idx, idx_n);
	return r;
}
/* data is handed over piece by piece by the page (fetched or embedded): disks 0..3, then the index */
EMSCRIPTEN_KEEPALIVE int mi_add_disk(int n, u8 *lec, u32 lec_n) { return res_add_disk(n, lec, lec_n); }
EMSCRIPTEN_KEEPALIVE int mi_set_index(u8 *idx, u32 idx_n) { return res_set_index(idx, idx_n); }
EMSCRIPTEN_KEEPALIVE void mi_charset(int id, u8 *data, u32 n) { res_add_charset(id, data, n); }
EMSCRIPTEN_KEEPALIVE void mi_start(void) { eng_init(); }
/* Developer shortcut (retail): the boot script waits in the copy-protection wheel (room 90, scripts 151/152/155/156).
 * room == 0: just stop the protection scripts so the boot script carries on into the normal intro;
 * room > 0: also stop the boot/start scripts and enter `room` directly (much of the game state is then missing).
 * Call after a few frames so those scripts have started. */
EMSCRIPTEN_KEEPALIVE void mi_start_at(int room)
{
	static const int prot[] = { 151, 152, 155, 156 }, boot[] = { 1, 149 };
	for (unsigned i = 0; i < 4; i++) vm_stop_script(prot[i]);
	if (room > 0) { for (unsigned i = 0; i < 2; i++) vm_stop_script(boot[i]); eng_start_room(room); }
}

/* run one engine frame; returns non-zero if the game requested exit */
EMSCRIPTEN_KEEPALIVE int mi_frame(void)
{
	eng_frame();
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
		u32 c = g_rgb[i];
		rgba[i * 4 + 0] = (c >> 16) & 255; rgba[i * 4 + 1] = (c >> 8) & 255; rgba[i * 4 + 2] = c & 255; rgba[i * 4 + 3] = 255;
	}
	return g_halted;
}
EMSCRIPTEN_KEEPALIVE u8 *mi_framebuffer(void) { return rgba; }
EMSCRIPTEN_KEEPALIVE int mi_ticks_per_frame(void) { return g_vars[VAR_TIMER_NEXT] > 0 ? g_vars[VAR_TIMER_NEXT] : 4; }
/* real duration of one engine frame in ms (game ticks run at 59.16 Hz, see TICKS_PER_SECOND) */
EMSCRIPTEN_KEEPALIVE double mi_frame_ms(void) { return (g_vars[VAR_TIMER_NEXT] > 0 ? g_vars[VAR_TIMER_NEXT] : 4) * 1000.0 / TICKS_PER_SECOND; }
EMSCRIPTEN_KEEPALIVE void mi_mouse_move(int x, int y) { g_mouse_x = x; g_mouse_y = y; }
EMSCRIPTEN_KEEPALIVE void mi_mouse_click(int x, int y, int button) { eng_click(x, y, button); }
EMSCRIPTEN_KEEPALIVE void mi_key(int k) { eng_key(k); }
/* audio: the page's audio callback pulls resampled mono float samples (sound.c keeps the ring buffer) */
EMSCRIPTEN_KEEPALIVE int mi_snd_pull(float *dst, int n, double rate) { return snd_pull(dst, n, rate); }
/* 1 when the save/load menu can be opened right now (input enabled, no cutscene) or is already open */
extern int ui_input_enabled(void); extern int g_cutscene_slot; extern int menu_is_open(void);
EMSCRIPTEN_KEEPALIVE int mi_menu_available(void) { return menu_is_open() || (ui_input_enabled() && g_cutscene_slot == 0xff && g_room); }
EMSCRIPTEN_KEEPALIVE int mi_room(void) { return g_room; }
extern int ui_cursor_visible(void);
EMSCRIPTEN_KEEPALIVE int mi_cursor_visible(void) { return ui_cursor_visible(); }

/* ---- save storage: the browser's localStorage, one key set per game (window.__saveNs = "demo" | "retail" | folder) ---- */
#include "menu.h"
EM_JS(int, js_save_put, (int slot, const char *meta, const unsigned char *data, int len), {
	try {
		const ns = window.__saveNs || "demo"; let m = "", i = 0, c;
		while ((c = HEAPU8[meta + i++])) m += String.fromCharCode(c);
		m = decodeURIComponent(escape(m));
		let s = ""; for (let o = 0; o < len; o += 8192) s += String.fromCharCode.apply(null, HEAPU8.subarray(data + o, data + Math.min(len, o + 8192)));
		localStorage.setItem("mi." + ns + ".slot" + slot + ".data", btoa(s));
		localStorage.setItem("mi." + ns + ".slot" + slot + ".meta", m); return 0;
	} catch (e) { console.warn("save failed", e); return -1; }
});
EM_JS(int, js_save_meta, (int slot, char *out, int max), {
	try {
		const m = localStorage.getItem("mi." + (window.__saveNs || "demo") + ".slot" + slot + ".meta"); if (m === null) return -1;
		const b = unescape(encodeURIComponent(m)); const n = Math.min(b.length, max - 1);
		for (let i = 0; i < n; i++) HEAPU8[out + i] = b.charCodeAt(i); HEAPU8[out + n] = 0; return n;
	} catch (e) { return -1; }
});
EM_JS(int, js_save_get, (int slot, unsigned char *out, int max), {
	try {
		const d = localStorage.getItem("mi." + (window.__saveNs || "demo") + ".slot" + slot + ".data"); if (d === null) return -1;
		const s = atob(d); if (s.length > max) return -1; for (let i = 0; i < s.length; i++) HEAPU8[out + i] = s.charCodeAt(i); return s.length;
	} catch (e) { return -1; }
});
EM_JS(void, js_timestamp, (char *out, int max), {
	const d = new Date(), p = (n) => String(n).padStart(2, "0"), t = d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + p(d.getHours()) + ":" + p(d.getMinutes());
	for (let i = 0; i < t.length && i < max - 1; i++) HEAPU8[out + i] = t.charCodeAt(i); HEAPU8[out + Math.min(t.length, max - 1)] = 0;
});
EM_JS(int, js_prompt_name, (const char *cur, char *out, int max), {
	if (!window.__isMobile) return -1;
	let c = "", i = 0, k; while ((k = HEAPU8[cur + i++])) c += String.fromCharCode(k);
	const r = window.prompt("Name for this saved game:", c); if (r === null) return 0;
	const b = r.replace(/[^ -~]/g, " ").split("|").join(" ").slice(0, 24); const n = Math.min(b.length, max - 1);
	for (let j = 0; j < n; j++) HEAPU8[out + j] = b.charCodeAt(j); HEAPU8[out + n] = 0; return 1;
});
int plat_prompt_name(const char *cur, char *out, int max) { return js_prompt_name(cur, out, max); }
int plat_save_put(int slot, const char *meta, const u8 *data, u32 len) { return js_save_put(slot, meta, data, (int)len); }
int plat_save_meta(int slot, char *out, int max) { return js_save_meta(slot, out, max); }
int plat_save_get(int slot, u8 *out, u32 max) { return js_save_get(slot, out, (int)max); }
void plat_timestamp(char *out, int max) { js_timestamp(out, max); }

/* ---- debug dump: JSON snapshot of the engine state for the page's [debug] dialog ---- */
#include <stdio.h>
#include <string.h>
#include "state.h"
extern char g_room_names[256][10]; extern const char *ui_expanded(const u8 *);
static char *dbg_buf; static size_t dbg_len, dbg_cap;
static void dj(const char *fmt, ...)
{
	char tmp[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
	if (n < 0) return; if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
	if (dbg_len + n + 1 > dbg_cap) { dbg_cap = dbg_cap ? dbg_cap * 2 : 16384; while (dbg_cap < dbg_len + n + 1) dbg_cap *= 2; dbg_buf = realloc(dbg_buf, dbg_cap); }
	memcpy(dbg_buf + dbg_len, tmp, n); dbg_len += n; dbg_buf[dbg_len] = 0;
}
static void djs(const char *s)      /* JSON string, printable ASCII only (names can hold control codes / '@' padding) */
{
	dj("\"");
	for (; s && *s; s++) { unsigned char c = (unsigned char)*s; if (c == '@') continue; if (c == '"' || c == '\\') dj("\\%c", c); else if (c >= 32 && c < 127) dj("%c", c); else dj("\\u%04x", c); }
	dj("\"");
}
EMSCRIPTEN_KEEPALIVE const char *mi_debug_json(void)
{
	dbg_len = 0; if (dbg_buf) dbg_buf[0] = 0;
	extern int eng_get_state(int); extern int ui_input_enabled(void);
	int ego = g_vars[VAR_EGO];
	dj("{\"room\":%d,\"room_name\":", g_room); djs(g_room < 256 ? g_room_names[g_room] : ""); 
	dj(",\"ticks\":%d,\"ego\":%d,\"input_enabled\":%d,\"cutscene_slot\":%d,\"have_msg\":%d,\"talk_actor\":%d",
	   g_vars[VAR_TIMER_TOTAL], ego, ui_input_enabled(), g_cutscene_slot, g_vars[VAR_HAVE_MSG], g_vars[VAR_TALK_ACTOR]);
	dj(",\"camera\":{\"cur\":%d,\"dest\":%d,\"follow\":%d},\"mouse\":{\"screen\":[%d,%d],\"virt\":[%d,%d]},\"room_size\":[%d,%d]",
	   g_cam_cur, g_cam_dest, g_cam_follow, g_mouse_x, g_mouse_y, g_vars[VAR_VIRT_MOUSE_X], g_vars[VAR_VIRT_MOUSE_Y], g_room_w, g_room_h);
	dj(",\"actors\":[");
	for (int a = 0, first = 1; a < 13; a++) { Actor *p = &g_actors[a]; if (!p->used && !p->room && !p->costume) continue;
		dj("%s{\"id\":%d,\"room\":%d,\"x\":%d,\"y\":%d,\"costume\":%d,\"facing\":%d,\"visible\":%d,\"moving\":%d,\"box\":%d,\"name\":", first ? "" : ",", a, p->room, p->x, p->y, p->costume, p->facing, p->visible, p->moving, p->walkbox);
		djs(p->name); dj("}"); first = 0; }
	dj("],\"scripts\":[");
	for (int i = 0, first = 1; i < NUM_SLOTS; i++) { Slot *s = &g_slots[i]; if (s->status == SS_DEAD) continue;
		dj("%s{\"slot\":%d,\"script\":%d,\"where\":%d,\"status\":%d,\"pc\":%u,\"delay\":%d}", first ? "" : ",", i, s->number, s->where, s->status, s->pc, s->delay); first = 0; }
	dj("],\"inventory\":[");
	for (int i = 1, first = 1; i < NUM_INVENTORY; i++) if (g_inventory[i]) { dj("%s{\"id\":%d,\"owner\":%d,\"name\":", first ? "" : ",", g_inventory[i], g_obj_owner[g_inventory[i]] & 15); djs(ui_expanded(eng_obj_name(g_inventory[i]))); dj("}"); first = 0; }
	dj("],\"objects\":[");
	for (int i = 0; i < g_nobjs; i++) { RoomObject *o = &g_objs[i];
		dj("%s{\"id\":%d,\"state\":%d,\"owner\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"walk\":[%d,%d],\"name\":", i ? "," : "", o->id, eng_get_state(o->id), g_obj_owner[o->id] & 15, o->x, o->y, o->w, o->h, o->walk_x, o->walk_y);
		djs(ui_expanded(eng_obj_name(o->id))); dj("}"); }
	dj("],\"vars\":{");
	for (int i = 0, first = 1; i < NUM_VARS; i++) if (g_vars[i]) { dj("%s\"%d\":%d", first ? "" : ",", i, g_vars[i]); first = 0; }
	dj("},\"bits_set\":[");
	for (int i = 0, first = 1; i < NUM_BITVARS; i++) if (g_bitvars[i >> 3] & (1 << (i & 7))) { dj("%s%d", first ? "" : ",", i); first = 0; }
	dj("]}");
	return dbg_buf;
}
/* the complete save state (same bytes as a save slot, zero-run packed) so a bug can be reproduced exactly */
static u8 *dbg_save; static u32 dbg_save_n;
EMSCRIPTEN_KEEPALIVE const u8 *mi_debug_save(void) { free(dbg_save); dbg_save = NULL; dbg_save_n = 0; if (state_save(&dbg_save, &dbg_save_n)) { dbg_save = NULL; dbg_save_n = 0; } return dbg_save; }
EMSCRIPTEN_KEEPALIVE int mi_debug_save_len(void) { return (int)dbg_save_n; }
