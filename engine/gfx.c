#include "scumm.h"
#include "engine.h"
#include <stdlib.h>

u8 g_screen[SCREEN_W * SCREEN_H];
const u8 g_ega_palette[16][3] = {
	{0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
	{85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}
};

/* EGA strip: 8 columns, column-major, three run codes (see RE_NOTES.md) */
void gfx_decode_strip(u8 *dst, int pitch, const u8 *src, int h)
{
	int x = 0, y = 0, run, z;
	u8 color;
	while (x < 8) {
		color = *src++;
		if (color & 0x80) {
			run = color & 0x3f;
			if (color & 0x40) {
				color = *src++;
				if (run == 0) run = *src++;
				for (z = 0; z < run && x < 8; z++) {
					dst[y * pitch + x] = (z & 1) ? (color & 0xf) : (color >> 4);
					if (++y >= h) { y = 0; x++; }
				}
			} else {
				if (run == 0) run = *src++;
				for (z = 0; z < run && x < 8; z++) {
					dst[y * pitch + x] = x > 0 ? dst[y * pitch + x - 1] : 0;
					if (++y >= h) { y = 0; x++; }
				}
			}
		} else {
			run = color >> 4;
			if (run == 0) run = *src++;
			color &= 0xf;
			for (z = 0; z < run && x < 8; z++) {
				dst[y * pitch + x] = color;
				if (++y >= h) { y = 0; x++; }
			}
		}
	}
}

/* Draw an image consisting of a u16 total size + per-strip u16 offsets (BM / OI layout)
 * into a buffer. xstrip: first dest strip, nstrips strips, height h. */
void gfx_draw_strips(u8 *dst, int pitch, int dst_h, const u8 *img, int xstrip, int nstrips, int y, int h)
{
	for (int i = 0; i < nstrips; i++) {
		u16 off = rd16(img + 2 + 2 * i);
		u8 tmp[8 * 200];
		gfx_decode_strip(tmp, 8, img + off, h);
		for (int r = 0; r < h; r++) {
			int yy = y + r; if (yy < 0 || yy >= dst_h) continue;
			memcpy(dst + yy * pitch + (xstrip + i) * 8, tmp + r * 8, 8);
		}
	}
}
