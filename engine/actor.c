/* Actors: state, actorOps, walking, animation (skeleton; costume rendering is in costume.c) */
#include "engine.h"
int act_adjust_to_box(int *x, int *y);
#include <stdio.h>
#include <stdlib.h>

Actor g_actors[16];

int new_dir_to_old(int d) { /* facing (degrees 0..359 in v5) is stored as 0..3 here */ return d & 3; }

void act_init(int a)             /* FUN_1000_0a08 */
{
	Actor *ac = &g_actors[a];
	memset(ac, 0, sizeof *ac);
	ac->walk_speed_x = 8; ac->walk_speed_y = 2; ac->width = 24; ac->scale = 0xFF;
	ac->init_frame = 1; ac->walk_frame = 2; ac->stand_frame = 3; ac->talk_start = 4; ac->talk_stop = 5;
	ac->talk_color = 15; ac->facing = 2; ac->new_facing = 2;
	for (int i = 0; i < 16; i++) { ac->curpos[i] = 0xFFFF; ac->palette[i] = 0xFF; }
}
void act_set_costume(int a, int cost)
{
	Actor *ac = &g_actors[a]; ac->costume = cost;
	for (int i = 0; i < 16; i++) { ac->curpos[i] = 0xFFFF; ac->start[i] = 0; ac->stopped = 0; }
	for (int i = 0; i < 16; i++) ac->palette[i] = 0xFF;
	act_start_anim(a, ac->init_frame * 4 + ac->facing);       /* initial frame */
	ac->needs_redraw = 1;
}

void vm_actor_ops(int a, int sub, const s16 *p, const u8 *text, int tlen)
{
	Actor *ac = &g_actors[a]; (void)tlen;
	switch (sub) {
	case 1: act_set_costume(a, p[0]); break;
	case 4: ac->walk_speed_x = p[0]; ac->walk_speed_y = p[1]; break;
	case 6: ac->walk_frame = (u8)p[0]; break;
	case 7: ac->talk_start = (u8)p[0]; ac->talk_stop = (u8)p[1]; break;
	case 8: ac->stand_frame = (u8)p[0]; break;
	case 10: act_init(a); break;
	case 11: ac->elevation = p[0]; ac->needs_redraw = 1; break;
	case 12: ac->init_frame = 1; ac->walk_frame = 2; ac->stand_frame = 3; ac->talk_start = 4; ac->talk_stop = 5; break;
	case 13: if (p[0] >= 0 && p[0] < 16) { ac->palette[p[0]] = (u8)(p[1] * 0x11 & 0xFF); ac->needs_redraw = 1; } break;
	case 14: ac->talk_color = (u8)p[0]; break;
	case 15: strncpy(ac->name, (const char *)text, 23); break;
	case 16: ac->init_frame = (u8)p[0]; break;
	case 17: for (int i = 0; i < 16; i++) if ((u16)p[i] != 0xFF) ac->palette[i] = (u8)p[i]; ac->needs_redraw = 1; break;
	case 18: ac->width = p[0]; break;
	case 19: ac->scale = (u8)p[0]; ac->needs_redraw = 1; break;
	default: break;
	}
}

void act_put(int a, int x, int y, int room)         /* FUN_1000_b118 */
{
	Actor *ac = &g_actors[a];
	ac->x = x; ac->y = y; ac->room = room; ac->moving = 0; ac->needs_redraw = 1;
	if (room == g_room && room) {
		ac->visible = 1;
		if (!eng_get_class(a, 22)) { int ax = ac->x, ay = ac->y, b = act_adjust_to_box(&ax, &ay); if (b != 0xFF) { ac->x = ax; ac->y = ay; } }
		ac->walkbox = (u8)act_box_of(ac->x, ac->y);
		if (!ac->cost_loaded) { act_start_anim(a, ac->stand_frame * 4 + ac->facing); ac->cost_loaded = 1; }
	} else ac->visible = 0;
}
void act_put_at_object(int a, int obj)
{
	int x = 0xF0, y = 0x78;
	if (eng_obj_where(obj) != WIO_NOTFOUND) eng_obj_xy(obj, &x, &y);
	act_put(a, x, y, g_actors[a].room);
}
int act_moving(int a) { return g_actors[a].moving; }
void act_stop_walk(int a) { g_actors[a].moving = 0; }
void act_face_obj(int a, int obj) {                      /* handler 81d2 (faceActor) */
	int x, y; if (!eng_obj_xy(obj, &x, &y)) return;
	Actor *ac = &g_actors[a];
	if (ac->room != g_room) return;
	int dir;
	if (abs(x - ac->x) < (g_actors[a].width ? g_actors[a].width / 2 : 12)) dir = y < ac->y ? 3 : 2;
	else dir = x < ac->x ? 0 : 1;
	ac->facing = ac->new_facing = (u8)dir;
	act_start_anim(a, ac->stand_frame * 4 + dir);
}
int g_walk_dir = -1;
void act_walk_to(int a, int x, int y)                    /* FUN_1000_b4a6 (simplified path: straight line) */
{
	if (getenv("TRACE_ANIM")) fprintf(stderr, "walkTo(%d,%d,%d) from %d,%d\n", a, x, y, g_actors[a].x, g_actors[a].y);
	Actor *ac = &g_actors[a];
	if (ac->room != g_room) { ac->x = x; ac->y = y; return; }
	if (!eng_get_class(a, 22)) { act_adjust_to_box(&x, &y); }
	if (ac->x == x && ac->y == y) { if (g_walk_dir >= 0) { ac->facing = ac->new_facing = (u8)g_walk_dir; act_start_anim(a, ac->stand_frame * 4 + ac->facing); } return; }
	ac->tx = x; ac->ty = y; ac->moving = 1; ac->walk_stage = 0; ac->dest_dir = (s8)g_walk_dir;
}
void act_walk_to_obj(int a, int obj) { int x, y; if (eng_obj_xy(obj, &x, &y)) { extern int eng_obj_walk_dir(int); g_walk_dir = obj > g_vars[VAR_NUM_ACTOR] ? eng_obj_walk_dir(obj) : -1; act_walk_to(a, x, y); g_walk_dir = -1; } }
void act_walk_to_actor(int a, int b, int dist) { (void)dist; act_walk_to(a, g_actors[b].x, g_actors[b].y); }
extern void act_step_anim(int a);
void act_animate(int a, int anim)                       /* handler 8272 */
{
	Actor *ac = &g_actors[a]; int in_room = ac->room == g_room;
	int c = anim & 0xFC;
	if (getenv("TRACE_ANIM")) fprintf(stderr, "animateActor(%d,%#x) facing=%d stand=%d\n", a, anim, ac->facing, ac->stand_frame);
	if (c == 0xFC) { if (!in_room) return; ac->moving = 0; act_start_anim(a, ac->stand_frame * 4 + ac->facing); return; }
	if (c == 0xF8) { if (in_room) { ac->moving &= ~4; act_start_anim(a, anim); } ac->facing = ac->new_facing = anim & 3; if (in_room && !ac->moving) act_start_anim(a, ac->stand_frame * 4 + ac->facing); return; }
	if (c == 0xF4) { if (in_room) { ac->new_facing = anim & 3; ac->facing = anim & 3; act_start_anim(a, ac->stand_frame * 4 + ac->facing); return; } ac->facing = anim & 3; return; }
	if (in_room) act_start_anim(a, anim * 4 + ac->facing);
}
int act_in_box(int a, int box) { Actor *ac = &g_actors[a]; return eng_box_in(box, ac->x, ac->y); }
int act_box_of(int x, int y) { for (int i = 0; i < g_nboxes; i++) if (eng_box_in(i, x, y)) return i; return 0xFF; }
int act_adjust_to_box(int *x, int *y);
void act_update_all(void)
{
	for (int a = 1; a < 13; a++) {
		Actor *ac = &g_actors[a];
		if (!ac->visible || ac->room != g_room) continue;
		if (ac->moving) {
			if (a == 1 && getenv("TRACE_EGO")) fprintf(stderr, "ego %d,%d -> %d,%d\n", ac->x, ac->y, ac->tx, ac->ty);
			int dx = ac->tx - ac->x, dy = ac->ty - ac->y;
			int sx = ac->walk_speed_x ? ac->walk_speed_x : 2, sy = ac->walk_speed_y ? ac->walk_speed_y : 1;
			if (!(ac->walk_stage & 1)) {          /* start of leg: face the direction of travel and start the walk animation */
				int dir = abs(dx) * 2 >= abs(dy) ? (dx < 0 ? 0 : 1) : (dy < 0 ? 3 : 2);
				ac->facing = ac->new_facing = (u8)dir; ac->walk_stage |= 1;
				act_start_anim(a, ac->walk_frame * 4 + dir);
			}
			if (abs(dx) <= sx && abs(dy) <= sy) {
				ac->x = ac->tx; ac->y = ac->ty; ac->moving = 0; ac->walk_stage = 0;
				if (ac->dest_dir >= 0) ac->facing = ac->new_facing = (u8)ac->dest_dir;
				act_start_anim(a, ac->stand_frame * 4 + ac->facing);
			} else {
				if (abs(dx) > sx) ac->x += dx > 0 ? sx : -sx; else ac->x = ac->tx;
				if (abs(dy) > sy) ac->y += dy > 0 ? sy : -sy; else ac->y = ac->ty;
			}
		}
		if (!eng_get_class(a, 22) && ac->moving && g_nboxes && act_box_of(ac->x, ac->y) == 0xFF && act_box_of(ac->tx, ac->ty) != 0xFF) { /* keep walking inside the walkable area */ int ax = ac->x, ay = ac->y; if (act_adjust_to_box(&ax, &ay) != 0xFF) { ac->x = ax; ac->y = ay; } }
		ac->walkbox = (u8)act_box_of(ac->x, ac->y);
		act_step_anim(a);
	}
}
const u8 *act_name(int a) { return (const u8 *)g_actors[a].name; }
/* ---- walkbox geometry (quads ul,ur,lr,ll) ---- */
static int pt_in_quad(const Box *b, int x, int y)
{
	int px[4] = { b->ulx, b->urx, b->lrx, b->llx }, py[4] = { b->uly, b->ury, b->lry, b->lly };
	int in = 0;
	for (int i = 0, j = 3; i < 4; j = i++) {
		if ((py[i] > y) != (py[j] > y) && x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]) in = !in;
	}
	if (in) return 1;
	/* inclusive edges */
	for (int i = 0, j = 3; i < 4; j = i++) {
		long long cr = (long long)(px[j] - px[i]) * (y - py[i]) - (long long)(py[j] - py[i]) * (x - px[i]);
		if (cr == 0 && x >= (px[i] < px[j] ? px[i] : px[j]) && x <= (px[i] > px[j] ? px[i] : px[j]) && y >= (py[i] < py[j] ? py[i] : py[j]) && y <= (py[i] > py[j] ? py[i] : py[j])) return 1;
	}
	return 0;
}
int eng_box_in(int box, int x, int y) { return box >= 0 && box < g_nboxes && pt_in_quad(&g_boxes[box], x, y); }
static void closest_on_box(const Box *b, int x, int y, int *ox, int *oy, long long *d2)
{
	int px[4] = { b->ulx, b->urx, b->lrx, b->llx }, py[4] = { b->uly, b->ury, b->lry, b->lly };
	long long best = 1LL << 40; int bx = x, by = y;
	for (int i = 0, j = 3; i < 4; j = i++) {
		double dx = px[j] - px[i], dy = py[j] - py[i], len2 = dx * dx + dy * dy, t = len2 ? ((x - px[i]) * dx + (y - py[i]) * dy) / len2 : 0;
		if (t < 0) t = 0; if (t > 1) t = 1;
		int cx = (int)(px[i] + t * dx + 0.5), cy = (int)(py[i] + t * dy + 0.5);
		long long d = (long long)(cx - x) * (cx - x) + (long long)(cy - y) * (cy - y);
		if (d < best) { best = d; bx = cx; by = cy; }
	}
	*ox = bx; *oy = by; *d2 = best;
}
/* ScummVM-style adjustXYToBeInBox: nearest point inside any visible box; returns box or 0xFF */
int act_adjust_to_box(int *x, int *y)
{
	if (!g_nboxes) return 0xFF;
	int best = 0xFF; long long bd = 1LL << 40; int bx = *x, by = *y;
	for (int i = 0; i < g_nboxes; i++) {
		if (g_boxes[i].flags & 0x80) continue;
		if (eng_box_in(i, *x, *y)) return i;
		int cx, cy; long long d; closest_on_box(&g_boxes[i], *x, *y, &cx, &cy, &d);
		if (d < bd) { bd = d; best = i; bx = cx; by = cy; }
	}
	*x = bx; *y = by; return best;
}
