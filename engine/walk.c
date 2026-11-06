/* Walkboxes and the actor walker.
 *
 * Written from the behaviour of MIDEMO.EXE (FUN_1000_b4a6 startWalk, b600 walk state machine, b842/b9b2 leg set-up and
 * stepping, c0ee facing, c262/c8c8 box snapping, d430/cad6 scale) and the data it uses (turn table DS:0x892).
 * The walk path itself (which waypoints to aim for between boxes) is my own design: the EXE walks box by box using the
 * room's box-connection matrix, aiming for a point on the border shared with the next box; here that "gate" point is
 * found by sampling the border instead of replicating the original's collinear-edge special cases.
 * ScummVM's engines/scumm was only used afterwards to sanity-check behaviour.
 */
#include "engine.h"
#include "tables.h"
#include <stdlib.h>
#include <math.h>

#define NO_BOX 0xFF
enum { M_NEWLEG = 1, M_INLEG = 2, M_TURN = 4, M_LAST = 8 };         /* bit meaning taken from b600 */
enum { BF_XFLIP = 0x08, BF_YFLIP = 0x10, BF_PLAYERONLY = 0x20, BF_INVISIBLE = 0x80 };

/* ------------------------------------------------------------------ geometry */
typedef struct { double x, y; } V2;
static void quad_pts(const Box *b, V2 p[4])
{
	p[0] = (V2){ b->ulx, b->uly }; p[1] = (V2){ b->urx, b->ury }; p[2] = (V2){ b->lrx, b->lry }; p[3] = (V2){ b->llx, b->lly };
}
/* convex quadrilateral (possibly degenerate) containment: the point must not be strictly outside any edge */
int walk_in_box(int n, int x, int y)
{
	if (n < 0 || n >= g_nboxes) return 0;
	V2 p[4]; quad_pts(&g_boxes[n], p);
	double minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
	for (int i = 1; i < 4; i++) { if (p[i].x < minx) minx = p[i].x; if (p[i].x > maxx) maxx = p[i].x; if (p[i].y < miny) miny = p[i].y; if (p[i].y > maxy) maxy = p[i].y; }
	if (x < minx || x > maxx || y < miny || y > maxy) return 0;
	int pos = 0, neg = 0;
	for (int i = 0; i < 4; i++) {
		V2 a = p[i], b = p[(i + 1) & 3];
		double cr = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
		if (cr > 0) pos = 1; else if (cr < 0) neg = 1;
	}
	return !(pos && neg);
}
static double nearest_on_segment(V2 a, V2 b, double x, double y, double *ox, double *oy)
{
	double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / l2 : 0;
	if (t < 0) t = 0; if (t > 1) t = 1;
	*ox = a.x + t * dx; *oy = a.y + t * dy;
	return (*ox - x) * (*ox - x) + (*oy - y) * (*oy - y);
}
static double nearest_in_box(int n, int x, int y, int *ox, int *oy)
{
	if (walk_in_box(n, x, y)) { *ox = x; *oy = y; return 0; }
	V2 p[4]; quad_pts(&g_boxes[n], p); double best = 1e18, bx = x, by = y;
	for (int i = 0; i < 4; i++) { double cx, cy, d = nearest_on_segment(p[i], p[(i + 1) & 3], x, y, &cx, &cy); if (d < best) { best = d; bx = cx; by = cy; } }
	*ox = (int)floor(bx + 0.5); *oy = (int)floor(by + 0.5);
	if (!walk_in_box(n, *ox, *oy)) {                      /* rounding pushed us out: try the neighbouring integers */
		for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (walk_in_box(n, *ox + dx, *oy + dy)) { *ox += dx; *oy += dy; return best; }
	}
	return best;
}
static int box_usable(int b, int actor)
{
	int f = g_boxes[b].flags;
	return !((f & BF_INVISIBLE) && !((f & BF_PLAYERONLY) && actor != g_vars[VAR_EGO]));
}
/* nearest walkable point; returns the box, or NO_BOX when the actor ignores boxes / the room has none */
int walk_snap(int actor, int *x, int *y)
{
	if (eng_get_class(actor, 22) || g_nboxes == 0) return NO_BOX;       /* class 22: ignore boxes (c0ee/cad6 check bit 0x20 of class byte 3) */
	int best = NO_BOX, bx = *x, by = *y; double bd = 1e18;
	for (int b = 0; b < g_nboxes; b++) {
		if (!box_usable(b, actor)) continue;
		int px, py; double d = nearest_in_box(b, *x, *y, &px, &py);
		if (d < bd) { bd = d; best = b; bx = px; by = py; if (d == 0) break; }
	}
	*x = bx; *y = by; return best;
}

/* ------------------------------------------------------------------ box graph */
static s8 hop[MAX_BOXES][MAX_BOXES];
void walk_load_matrix(void)
{
	memset(hop, -1, sizeof hop);
	const u8 *m = g_box_matrix, *end = g_box_matrix_end;
	if (!m) return;
	if (m < end && *m == 0xFF) m++;                       /* the EXE's lookup (cbb6) starts after an optional 0xFF */
	for (int from = 0; from < g_nboxes && m < end; from++) {
		while (m + 2 < end && m[0] != 0xFF) {
			for (int to = m[0]; to <= m[1] && to < MAX_BOXES; to++) hop[from][to] = (s8)m[2];
			m += 3;
		}
		m++;
	}
	for (int i = 0; i < g_nboxes; i++) hop[i][i] = (s8)i;
}
static int next_box(int from, int to)
{
	if (from == to) return to;
	if (from == NO_BOX) return to;
	if (to == NO_BOX || from >= g_nboxes || to >= g_nboxes) return -1;
	return hop[from][to];
}
/* Pick the point on the border of box A that also lies in box B which gives the shortest detour pos -> gate -> dest. */
static void pick_gate(int A, int B, int px, int py, int dx, int dy, int *gx, int *gy)
{
	V2 p[4]; quad_pts(&g_boxes[A], p); double best = 1e18; int bx = px, by = py, found = 0;
	for (int e = 0; e < 4; e++) {
		V2 a = p[e], b = p[(e + 1) & 3]; double len = hypot(b.x - a.x, b.y - a.y); int n = (int)len + 1;
		for (int i = 0; i <= n; i++) {
			double t = n ? (double)i / n : 0; int cx = (int)floor(a.x + (b.x - a.x) * t + 0.5), cy = (int)floor(a.y + (b.y - a.y) * t + 0.5);
			if (!walk_in_box(B, cx, cy) || !walk_in_box(A, cx, cy)) continue;
			double d = hypot(cx - px, cy - py) + hypot(dx - cx, dy - cy);
			if (d < best) { best = d; bx = cx; by = cy; found = 1; }
		}
	}
	if (!found) nearest_in_box(B, px, py, &bx, &by);
	*gx = bx; *gy = by;
}

/* ------------------------------------------------------------------ scale */
/* d430 builds, for every y in 0..199, scale = clamp((s2-s1)*(y-y1)/(y2-y1)+s1, 0, 255) for each room scale slot */
ScaleSlot g_scale_slots[20];
static int slot_scale(int slot, int y)
{
	ScaleSlot *s = &g_scale_slots[slot];
	if (s->y1 == s->y2) return 255;
	if (y < 0) y = 0; if (y > 199) y = 199;
	int v = (s->s2 - s->s1) * (y - s->y1) / (s->y2 - s->y1) + s->s1;
	return v < 0 ? 0 : v > 255 ? 255 : v;
}
static int actor_scale_word(const Actor *a)                    /* cad6: box scale, or the actor's own value for class 22 */
{
	if (eng_get_class((int)(a - g_actors), 22)) return a->scale;
	if (a->walkbox >= g_nboxes) return 255;
	return g_boxes[a->walkbox].scale;
}
void walk_setup_scale(Actor *a)
{
	int w = actor_scale_word(a), s;
	if (w & 0x8000) { int slot = w & 0x7FFF; s = slot < 20 ? slot_scale(slot, a->y) : 255; } else s = w;
	if (s < 1) s = 1; if (s > 255) s = 255;
	a->scalex = a->scaley = (u8)s;
}
static void set_box(Actor *a, int box) { a->walkbox = (u8)box; walk_setup_scale(a); }

/* ------------------------------------------------------------------ facing (c0ee) */
static int box_flags_of(const Actor *a) { return a->walkbox < g_nboxes ? g_boxes[a->walkbox].flags : 0; }
/* the direction the actor should be facing next, given where it wants to look: one step of the EXE's turn table,
 * adjusted by the box's flip/forced-direction flags */
static int next_facing(Actor *a, int target, int walking)
{
	int ai = (int)(a - g_actors), fl = box_flags_of(a);
	int right = a->wd.dxf > 0, down = a->wd.dyf > 0;
	if ((fl & BF_XFLIP) || eng_get_class(ai, 19)) { if (!(target & 2)) target ^= 1; right = !right; }
	if ((fl & BF_YFLIP) || eng_get_class(ai, 18)) { if (target & 2) target ^= 1; down = !down; }
	int r = exe_turn_table[target + a->facing * 4];
	switch (fl & 7) {
	case 0: break;
	case 1: r = walking ? right : (target != 1 ? 0 : target); break;
	case 2: r = walking ? 3 - down : (target != 3 ? 2 : target); break;
	case 3: r = 0; break; case 4: r = 1; break; case 6: r = 2; break; case 5: r = 3; break;
	default: break;
	}
	return r;
}
static void face(Actor *a, int dir)                             /* instant direction change (animate 0xF8+dir) */
{
	if (a->facing == dir) return;
	extern void act_set_facing(int ai, int dir);
	act_set_facing((int)(a - g_actors), dir);
}
void act_turn_to(int ai, int dir)                              /* start turning in place */
{
	Actor *a = &g_actors[ai]; if (dir < 0) return;
	a->new_facing = (u8)dir; a->moving = M_TURN;
}

/* ------------------------------------------------------------------ legs and steps */
static int target_dir(int dx, int dy)                          /* b9b2: vertical when |dx| < 3|dy| */
{
	if (abs(dx) < 3 * abs(dy)) return dy > 0 ? 2 : 3;
	return dx > 0 ? 1 : 0;
}
static void begin_leg(Actor *a, int tx, int ty)                 /* b842: movement factors for a straight leg */
{
	int dx = tx - a->x, dy = ty - a->y;
	int fdx = dx ? dx : 1, fdy = dy ? dy : 1;                     /* b842 treats a zero delta as 1 so the ratio below stays defined */
	long long sx = a->walk_speed_x > 0 ? a->walk_speed_x : 1, sy = a->walk_speed_y > 0 ? a->walk_speed_y : 1;
	long long dyf = sy << 16; if (fdy < 0) dyf = -dyf;
	long long dxf = dyf * fdx / fdy;
	if ((dxf < 0 ? -dxf : dxf) >> 16 > sx) { dxf = sx << 16; if (fdx < 0) dxf = -dxf; dyf = dxf * fdy / fdx; }
	a->wd.start_x = a->x; a->wd.start_y = a->y; a->wd.wp_x = tx; a->wd.wp_y = ty;
	a->wd.dxf = (int)dxf; a->wd.dyf = (int)dyf; a->wd.xfrac = a->wd.yfrac = 0;
	a->new_facing = (u8)target_dir(dx, dy);
}
static int step_leg(Actor *a)                                   /* b9b2: returns 1 while the leg continues */
{
	a->needs_redraw = 1;
	int want = next_facing(a, a->new_facing, 1);
	if (!(a->moving & M_INLEG) || a->facing != want) {
		if (a->cur_frame != a->walk_frame || a->facing != want) { face(a, want); extern void act_start_frame(int, int); act_start_frame((int)(a - g_actors), a->walk_frame); }
		a->moving |= M_INLEG;
	}
	if (a->walkbox != a->wd.curbox && walk_in_box(a->wd.curbox, a->x, a->y)) set_box(a, a->wd.curbox);
	int remx = abs(a->wd.wp_x - a->wd.start_x), remy = abs(a->wd.wp_y - a->wd.start_y);
	if (abs(a->x - a->wd.start_x) >= remx && abs(a->y - a->wd.start_y) >= remy) { a->moving &= ~M_INLEG; return 0; }
	walk_setup_scale(a);
	long long nx = (long long)a->x * 0x10000 + a->wd.xfrac + (long long)(a->wd.dxf >> 8) * a->scalex;
	long long ny = (long long)a->y * 0x10000 + a->wd.yfrac + (long long)(a->wd.dyf >> 8) * a->scaley;
	a->wd.xfrac = (int)(nx & 0xFFFF); a->x = (int)(nx >> 16);
	a->wd.yfrac = (int)(ny & 0xFFFF); a->y = (int)(ny >> 16);
	if (abs(a->x - a->wd.start_x) > remx) a->x = a->wd.wp_x;
	if (abs(a->y - a->wd.start_y) > remy) a->y = a->wd.wp_y;
	if (a->x == a->wd.wp_x && a->y == a->wd.wp_y) { a->moving &= ~M_INLEG; return 0; }
	return 1;
}

void walk_start(int ai, int x, int y, int dir)                  /* b4a6; dir = facing on arrival (0..3) or -1 */
{
	Actor *a = &g_actors[ai];
	if (a->room != g_room) { a->x = x; a->y = y; if (dir >= 0) a->facing = a->new_facing = (u8)dir; return; }
	int box = NO_BOX;
	if (!eng_get_class(ai, 22)) {
		if (a->wd.destbox != NO_BOX && walk_in_box(a->wd.destbox, x, y)) box = a->wd.destbox; else box = walk_snap(ai, &x, &y);
		if (a->moving && a->wd.destdir == dir && a->wd.dest_x == x && a->wd.dest_y == y) return;
	} else a->walkbox = NO_BOX;
	if (a->x == x && a->y == y) { if (dir >= 0 && a->facing != dir) act_turn_to(ai, dir); return; }
	a->wd.dest_x = x; a->wd.dest_y = y; a->wd.destbox = box; a->wd.destdir = dir; a->wd.curbox = a->walkbox;
	a->moving = (a->moving & M_INLEG) | M_NEWLEG;
}

void walk_actor(int ai)                                          /* b600: advance the walk by one frame */
{
	Actor *a = &g_actors[ai];
	if (!a->moving) return;
	if (!(a->moving & M_NEWLEG)) {
		if ((a->moving & M_INLEG) && step_leg(a)) return;
		if (a->moving & M_LAST) {                                 /* arrived: stand, then take the requested facing */
			a->moving = 0; set_box(a, a->wd.destbox);
			extern void act_start_frame(int, int); act_start_frame(ai, a->stand_frame);
			if (a->wd.destdir >= 0 && a->facing != a->wd.destdir) act_turn_to(ai, a->wd.destdir);
			return;
		}
		if (a->moving & M_TURN) {
			int nf = next_facing(a, a->new_facing, 0);
			if (a->facing != nf) face(a, nf); else a->moving = 0;
			return;
		}
		set_box(a, a->wd.curbox); a->moving &= M_INLEG;
	}
	a->moving &= ~M_NEWLEG;
	for (int guard = 0; guard < 2 * MAX_BOXES; guard++) {          /* pick the next leg */
		if (a->walkbox == NO_BOX) { set_box(a, a->wd.destbox); a->wd.curbox = a->wd.destbox; break; }
		if (a->walkbox == a->wd.destbox) break;
		int nb = next_box(a->walkbox, a->wd.destbox);
		if (nb < 0 || nb >= g_nboxes) { a->wd.destbox = a->walkbox; a->moving |= M_LAST; return; }   /* no route: stop in this box */
		a->wd.curbox = (u8)nb;
		int gx, gy; pick_gate(a->walkbox, nb, a->x, a->y, a->wd.dest_x, a->wd.dest_y, &gx, &gy);
		if (gx == a->x && gy == a->y) { set_box(a, nb); continue; }       /* already standing on the border */
		begin_leg(a, gx, gy);
		if (step_leg(a)) return;
		set_box(a, nb);
	}
	a->moving |= M_LAST;
	begin_leg(a, a->wd.dest_x, a->wd.dest_y);
	step_leg(a);
}

void walk_place(int ai)                                          /* actor put into the room: snap to the walkable area */
{
	Actor *a = &g_actors[ai]; int x = a->x, y = a->y;
	int b = walk_snap(ai, &x, &y);
	if (b != NO_BOX) { a->x = x; a->y = y; }
	a->wd.destbox = (u8)b; set_box(a, b); a->wd.dest_x = -1; a->moving = 0;
}
