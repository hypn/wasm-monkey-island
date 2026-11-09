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
/* ---- VGA (256 colour) images, retail game: u32 total size, then u32 offset per 8-pixel strip. A strip starts with a
 * codec byte: code % 10 = bits per absolute colour, 1x = columns top-to-bottom, 2x = rows left-to-right.  Then a first
 * colour byte and an LSB-first bit stream; after each pixel: bit 0 -> same colour; else next bit 0 -> new absolute colour
 * (shr bits); else next bit 0 -> colour += inc; else inc = -inc, colour += inc. */
int g_vga; u8 g_pal_rgb[256][3];
void gfx_decode_strip_vga(u8 *dst, int pitch, const u8 *src, int h)
{
	int code = *src++, shr = code % 10, mask = 0xFF >> (8 - shr), horiz = (code / 10) == 2;
	int color = *src++; unsigned bits = *src++; int cl = 8, inc = -1;
	int outer = horiz ? h : 8, inner = horiz ? 8 : h;
	for (int a = 0; a < outer; a++) for (int b = 0; b < inner; b++) {
		if (cl <= 8) { bits |= (unsigned)(*src++) << cl; cl += 8; }
		int x = horiz ? b : a, y = horiz ? a : b;
		dst[y * pitch + x] = (u8)color;
#define NEXTBIT() (cl--, ((bits & 1) ? (bits >>= 1, 1) : (bits >>= 1, 0)))
		if (!NEXTBIT()) continue;
		if (!NEXTBIT()) {
			if (cl <= 8) { bits |= (unsigned)(*src++) << cl; cl += 8; }
			color = bits & mask; bits >>= shr; cl -= shr; inc = -1;
		} else if (!NEXTBIT()) color += inc;
		else { inc = -inc; color += inc; }
	}
}
void gfx_draw_strips(u8 *dst, int pitch, int dst_h, const u8 *img, int xstrip, int nstrips, int y, int h)
{
	if (h > 200) h = 200;
	for (int i = 0; i < nstrips; i++) {
		if (xstrip + i < 0 || (xstrip + i + 1) * 8 > pitch) continue;      /* column outside the destination image */
		u8 tmp[8 * 200];
		if (g_vga) gfx_decode_strip_vga(tmp, 8, img + rd32(img + 4 + 4 * i), h);
		else gfx_decode_strip(tmp, 8, img + rd16(img + 2 + 2 * i), h);
		for (int r = 0; r < h; r++) {
			int yy = y + r; if (yy < 0 || yy >= dst_h) continue;
			memcpy(dst + yy * pitch + (xstrip + i) * 8, tmp + r * 8, 8);
		}
	}
}
