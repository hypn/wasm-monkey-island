/* Classic (v3/v4) costume loader, animation and limb renderer. */
#include "engine.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	const u8 *base;       /* costume data after the 6-byte block header */
	int num_anim, format, ncolors;
	const u8 *palette, *anim_cmds, *data_offsets, *anim_offsets;
} CostumeInfo;

static int load_costume(int id, CostumeInfo *c)
{
	u32 sz; const u8 *b = res_costume(id, &sz); if (!b) return 0;
	/* all offsets inside a costume are relative to the start of the block (incl. 6-byte header) */
	c->base = b; c->num_anim = b[6]; c->format = b[7];
	c->ncolors = (c->format & 1) ? 32 : 16;
	c->palette = b + 8;
	c->anim_cmds = b + rd16(b + 8 + c->ncolors);
	c->data_offsets = b + 10 + c->ncolors;
	c->anim_offsets = c->data_offsets + 32;
	return 1;
}

/* decode a costume animation definition for animation `frame` (FUN_1000_c.. costumeDecodeData) */
static void decode_anim(Actor *a, const CostumeInfo *c, int frame, unsigned usemask)
{
	if (frame > c->num_anim) return;
	const u8 *r = c->base + rd16(c->anim_offsets + frame * 2);
	if (r == c->base) return;
	unsigned mask = rd16(r); r += 2;
	int i = 0;
	do {
		if (mask & 0x8000) {
			int j = rd16(r); r += 2;
			if (usemask & 0x8000) {
				if (j == 0xFFFF) { a->curpos[i] = 0xFFFF; a->start[i] = 0; a->frame[i] = frame; }
				else {
					int extra = *r++;
					const u8 *cmd = c->anim_cmds + j;
					if (cmd[0] == 0x7A) a->stopped &= ~(1 << i);
					else if (cmd[0] == 0x79) a->stopped |= (1 << i);
					else {
						a->curpos[i] = a->start[i] = (u16)j; a->end[i] = (u16)(j + (extra & 0x7F));
						if (extra & 0x80) a->curpos[i] |= 0x8000;
						a->frame[i] = frame;
					}
				}
			} else { if (j != 0xFFFF) r++; }
		}
		i++; usemask <<= 1; mask <<= 1;
	} while ((u16)mask);
}

/* ceae: start an animation. anim = frame*4 + direction; frames >= 0xF8 only change direction. */
static void redirect_limbs(Actor *a, const CostumeInfo *c, int dir)     /* d12e: limbs not covered by the new anim follow the new facing */
{
	for (int i = 0; i < 16; i++) {
		unsigned f = a->frame[i];
		if (f == 0xFFFF || (f & 3) == (unsigned)dir) continue;
		decode_anim(a, c, (f & 0xFFFC) | dir, 0x8000u >> i);
	}
}
void act_start_anim(int ai, int anim)
{
	Actor *a = &g_actors[ai]; CostumeInfo c;
	if (a->room != g_room || !a->costume || !load_costume(a->costume, &c)) { a->facing = (u8)(anim & 3); return; }
	a->needs_redraw = 1;
	if (a->init_frame == (anim >> 2)) { for (int i = 0; i < 16; i++) { a->curpos[i] = 0xFFFF; a->frame[i] = 0xFFFF; a->start[i] = 0; a->end[i] = 0; } a->stopped = 0; }
	if ((anim & 0xFC) != 0xF8) { decode_anim(a, &c, anim, 0xFFFF); a->cur_frame = anim >> 2; }
	if (a->facing != (anim & 3)) redirect_limbs(a, &c, anim & 3);
	a->facing = (u8)(anim & 3);
}
void act_start_frame(int ai, int frame) { act_start_anim(ai, frame * 4 + g_actors[ai].facing); }
void act_set_facing(int ai, int dir) { act_start_anim(ai, 0xF8 | dir); }

/* advance animation one step (FUN_1000_c... increaseAnims); returns nonzero if something changed */
static int increase_anim(Actor *a, const CostumeInfo *c, int slot)
{
	if (a->curpos[slot] == 0xFFFF) return 0;
	int highflag = a->curpos[slot] & 0x8000;
	int i = a->curpos[slot] & 0x7FFF;
	int end = a->end[slot];
	u8 code = c->anim_cmds[i] & 0x7F;
	(void)code;
	if (highflag == 0) { if (i++ >= end) i = a->start[slot]; }
	else { if (i != end) i++; }
	/* 0x7C and 0x78 are counter commands (sound/animation counters): consume them and move on (FUN_1000_d272) */
	for (int guard = 0; guard < 64 && (c->anim_cmds[i] == 0x7C || c->anim_cmds[i] == 0x78); guard++) {
		if (a->start[slot] == end && !highflag) break;
		if (highflag == 0) { if (i++ >= end) i = a->start[slot]; } else { if (i != end) i++; else break; }
	}
	a->curpos[slot] = (u16)(i | highflag);
	return 1;
}
void act_step_anim(int ai)
{
	Actor *a = &g_actors[ai]; CostumeInfo c;
	if (!a->costume || !load_costume(a->costume, &c)) return;
	for (int i = 0; i < 16; i++) if (a->curpos[i] != 0xFFFF && !(a->stopped & (1 << i))) increase_anim(a, &c, i);
}

/* ---- limb rendering ---- */
static int pixel_color(const Actor *a, const CostumeInfo *c, int idx)
{
	int col = c->palette[idx];
	if (a->palette[idx] != 0xFF) col = a->palette[idx];
	return g_vga ? (col & 0xFF) : (col & 15);   /* retail: costume palette entries are full VGA indices */
}

extern u8 *g_mask; extern int g_room_w;
static int g_clip_mask;
/* Scaling uses the game's decimation table (DS:0x8CC): a source row/column is kept when table[index] < scale, so
 * scale 255 keeps everything and smaller values drop a pseudo-random subset. Offsets are scaled by counting the
 * kept units they span. (My own drawing routine; the EXE's scaled blitter lives in segment 294d.) */
static int kept(int idx, int scale) { return exe_scale_table[idx & 0xFF] < scale; }
static int scaled_len(int n, int scale)                      /* number of kept units among n units starting at index 128 */
{
	if (scale >= 0xFF) return n;
	int k = 0; for (int i = 0; i < n; i++) k += kept(128 + i, scale); return k;
}
static void draw_picture(u8 *dst, int cam_x, int view_top, const Actor *a, const CostumeInfo *c, const u8 *pic, int *xmove_cur, int *ymove_cur, int mirror)
{
	int w = rd16(pic), h = rd16(pic + 2);
	int ox = *xmove_cur + (s16)rd16(pic + 4), oy = *ymove_cur + (s16)rd16(pic + 6);
	*xmove_cur += (s16)rd16(pic + 8); *ymove_cur -= (s16)rd16(pic + 10);
	const u8 *src = pic + 12;
	int shr = (c->format & 1) ? 3 : 4, runmask = (c->format & 1) ? 7 : 15;
	int sx = a->scalex, sy = a->scaley;
	int ax = a->x - cam_x, ay = a->y - a->elevation + view_top;
	int offx = ox < 0 ? -scaled_len(-ox, sx) : scaled_len(ox, sx);
	int offy = oy < 0 ? -scaled_len(-oy, sy) : scaled_len(oy, sy);
	int x = mirror ? ax - offx : ax + offx, y = ay + offy, step = mirror ? -1 : 1;
	int col = 0, row = 0, outx = x, outy = y, keep_col = kept(128, sx);
	int len, color;
	for (;;) {
		len = *src++; color = len >> shr; len &= runmask; if (!len) len = *src++;
		while (len--) {
			if (keep_col && (sy >= 0xFF || kept(128 + row, sy))) {
				if (color && outx >= 0 && outx < SCREEN_W && outy >= view_top && outy < SCREEN_H) {
					int rx = outx + cam_x, ry = outy - view_top;
					if (!(g_clip_mask && rx >= 0 && rx < g_room_w && ry < 144 && g_mask[ry * g_room_w + rx])) { dst[outy * SCREEN_W + outx] = (u8)pixel_color(a, c, color); Actor *wa = (Actor *)a; if (!wa->hvalid) { wa->hx1 = wa->hx2 = rx; wa->hy1 = wa->hy2 = ry; wa->hvalid = 1; } else { if (rx < wa->hx1) wa->hx1 = rx; if (rx > wa->hx2) wa->hx2 = rx; if (ry < wa->hy1) wa->hy1 = ry; if (ry > wa->hy2) wa->hy2 = ry; } }
				}
				outy++;
			}
			if (++row >= h) {
				row = 0; outy = y;
				if (keep_col) outx += step;
				if (++col >= w) return;
				keep_col = kept(128 + col, sx);
			}
		}
	}
}

extern int g_view_top; extern u8 *g_mask; extern int g_room_w;
void costume_draw_actors(u8 *screen, int cam_x, int view_top)
{
	int order[16], n = 0;
	for (int i = 1; i < 13; i++) if (g_actors[i].visible && g_actors[i].room == g_room && g_actors[i].costume) order[n++] = i;
	for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_actors[order[j]].y < g_actors[order[j - 1]].y; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
	for (int k = 0; k < n; k++) {
		Actor *a = &g_actors[order[k]]; CostumeInfo c;
		if (!load_costume(a->costume, &c)) continue;
		int mirror = (a->facing & 3) == 0;      /* verified visually: west (0) is the mirrored direction */
		if (c.format & 0x80) mirror = !mirror;
		int xm = 0, ym = 0; walk_setup_scale(a); a->hvalid = 0;
		g_clip_mask = eng_get_class(order[k], 20) ? 0 : (eng_get_class(order[k], 21) || (a->walkbox < g_nboxes && g_boxes[a->walkbox].mask));   /* classes 20 NeverClip, 21 AlwaysClip */
		for (int limb = 0; limb < 16; limb++) {
			if (a->curpos[limb] == 0xFFFF || (a->stopped & (1 << limb))) continue;   /* 0x79 = limb stopped (hidden) */
			int code = c.anim_cmds[a->curpos[limb] & 0x7FFF] & 0x7F;
			if (code >= 0x78) continue;
			const u8 *tbl = c.base + rd16(c.data_offsets + limb * 2);
			const u8 *pic = c.base + rd16(tbl + code * 2);
			draw_picture(screen, cam_x, view_top, a, &c, pic, &xm, &ym, mirror);
		}
	}
}
