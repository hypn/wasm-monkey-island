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
EMSCRIPTEN_KEEPALIVE void mi_charset(int id, u8 *data, u32 n) { res_add_charset(id, data, n); }
EMSCRIPTEN_KEEPALIVE void mi_start(void) { eng_init(); }

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
EMSCRIPTEN_KEEPALIVE void mi_mouse_move(int x, int y) { g_mouse_x = x; g_mouse_y = y; }
EMSCRIPTEN_KEEPALIVE void mi_mouse_click(int x, int y, int button) { eng_click(x, y, button); }
EMSCRIPTEN_KEEPALIVE void mi_key(int k) { eng_key(k); }
EMSCRIPTEN_KEEPALIVE int mi_room(void) { return g_room; }
extern int ui_cursor_visible(void);
EMSCRIPTEN_KEEPALIVE int mi_cursor_visible(void) { return ui_cursor_visible(); }
