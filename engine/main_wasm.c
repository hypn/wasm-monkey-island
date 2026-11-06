/* WebAssembly platform layer: the page passes the demo's data files (fetched or embedded)
 * and drives the engine one frame at a time. */
#include "engine.h"
#include <stdlib.h>
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
int plat_save_put(int slot, const char *meta, const u8 *data, u32 len) { return js_save_put(slot, meta, data, (int)len); }
int plat_save_meta(int slot, char *out, int max) { return js_save_meta(slot, out, max); }
int plat_save_get(int slot, u8 *out, u32 max) { return js_save_get(slot, out, (int)max); }
void plat_timestamp(char *out, int max) { js_timestamp(out, max); }
