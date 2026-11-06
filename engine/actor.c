/* Actors: state, actorOps, walking, animation (skeleton; costume rendering is in costume.c) */
#include "engine.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>

Actor g_actors[16];

int new_dir_to_old(int d) { /* facing (degrees 0..359 in v5) is stored as 0..3 here */ return d & 3; }

void act_init_ex(int a, int full)                          /* FUN_1000_0a08(actor, full) */
{
	Actor *ac = &g_actors[a];
	if (full) {                                               /* boot only: also forget room, position and costume */
		memset(ac, 0, sizeof *ac);
		ac->facing = 2; ac->new_facing = 2; ac->walkbox = 0xFF; ac->wd.destbox = 0xFF; ac->wd.destdir = -1; ac->cur_frame = -1;
		for (int i = 0; i < 16; i++) { ac->curpos[i] = 0xFFFF; ac->frame[i] = 0xFFFF; ac->palette[i] = 0xFF; }
	}
	ac->elevation = 0; ac->width = 24; ac->talk_color = 15; ac->scale = 0xFF; ac->scalex = ac->scaley = 0xFF;
	ac->walk_speed_x = 8; ac->walk_speed_y = 2;
	ac->init_frame = 1; ac->walk_frame = 2; ac->stand_frame = 3; ac->talk_start = 4; ac->talk_stop = 5;
	ac->new_facing = 0; ac->moving = 0;                      /* 0a08 clears target facing ([569e]) and the moving flags ([48d2]) */
	if (a >= 0 && a < NUM_OBJECTS) g_obj_class[a] = 0;       /* ... and the actor's three class bytes */
}
void act_init(int a) { act_init_ex(a, 1); }

void act_set_costume(int a, int cost)
{
	Actor *ac = &g_actors[a]; ac->costume = cost;
	for (int i = 0; i < 16; i++) { ac->curpos[i] = 0xFFFF; ac->start[i] = 0; ac->stopped = 0; }
	for (int i = 0; i < 16; i++) ac->palette[i] = 0xFF;
	if (ac->room == g_room && ac->visible) act_start_frame(a, ac->init_frame); else ac->pending_init = 1;   /* b2e8: init anim runs when the actor is shown (b1e8) */
	ac->needs_redraw = 1;
}

void vm_actor_ops(int a, int sub, const s16 *p, const u8 *text, int tlen)
{
	Actor *ac = &g_actors[a]; (void)tlen;
	switch (sub) {
	case 1: act_set_costume(a, p[0]); break;
	case 4: ac->walk_speed_x = p[0]; ac->walk_speed_y = p[1]; break;                      /* setWalkSpeed */
	case 6: ac->walk_frame = (u8)p[0]; break;
	case 7: ac->talk_start = (u8)p[0]; ac->talk_stop = (u8)p[1]; break;
	case 8: ac->stand_frame = (u8)p[0]; break;
	case 10: act_init_ex(a, 0); break;
	case 11: ac->elevation = p[0]; ac->needs_redraw = 1; break;
	case 12: ac->init_frame = 1; ac->walk_frame = 2; ac->stand_frame = 3; ac->talk_start = 4; ac->talk_stop = 5; break;
	case 13: if (getenv("TRACE_PAL")) fprintf(stderr, "actorOps palette1 actor=%d idx=%d val=%d\n", a, p[0], p[1]); if (p[0] >= 0 && p[0] < 16) { ac->palette[p[0]] = (u8)(g_vga ? p[1] : (p[1] * 0x11 & 0xFF))   /* EGA stores a nibble pair; retail (VGA) stores the palette index itself */; ac->needs_redraw = 1; } break;
	case 14: if (getenv("TRACE_TXT")) fprintf(stderr, "talkColor actor=%d color=%d\n", a, p[0]); ac->talk_color = (u8)p[0]; break;
	case 15: strncpy(ac->name, (const char *)text, 23); break;
	case 16: ac->init_frame = (u8)p[0]; break;
	case 17: if (getenv("TRACE_PAL")) { fprintf(stderr, "actorOps palette17 actor=%d:", a); for (int i = 0; i < 16; i++) fprintf(stderr, " %d", p[i]); fprintf(stderr, "\n"); } for (int i = 0; i < 16; i++) if ((u16)p[i] != 0xFF) ac->palette[i] = (u8)p[i]; ac->needs_redraw = 1; break;
	case 18: ac->width = p[0]; break;
	case 19: ac->scale = (u8)p[0]; ac->needs_redraw = 1; break;
	default: break;
	}
}

void act_put(int a, int x, int y, int room)         /* Actor::putActor (FUN_1000_b118) */
{
	Actor *ac = &g_actors[a];
	int was_visible = ac->visible;
	ac->x = x; ac->y = y; ac->room = room; ac->needs_redraw = 1;
	if (room == g_room && room) {
		if (was_visible && ac->moving) { ac->moving = 0; act_start_frame(a, ac->stand_frame); }
		ac->visible = 1;
		walk_place(a);                                   /* snaps into the nearest walkbox */
		if (!was_visible && ac->pending_init) { ac->pending_init = 0; act_start_frame(a, ac->init_frame); }   /* b1e8 */
	} else { ac->visible = 0; ac->moving = 0; }
}
void act_put_at_object(int a, int obj)
{
	int x = 0xF0, y = 0x78;
	if (eng_obj_where(obj) != WIO_NOTFOUND) eng_obj_xy(obj, &x, &y);
	act_put(a, x, y, g_actors[a].room);
}
int act_moving(int a) { return g_actors[a].moving; }
void act_stop_walk(int a) { g_actors[a].moving = 0; }
void act_face_obj(int a, int obj)                        /* Actor::faceToObject (v4): turn gradually */
{
	int x, y; if (!eng_obj_pos(obj, &x, &y)) return;
	Actor *ac = &g_actors[a];
	if (ac->room != g_room) return;
	int dir = ac->x < x ? 1 : 0;
	int width = obj <= g_vars[VAR_NUM_ACTOR] ? g_actors[obj].width : 0;
	if (!width) { for (int i = 0; i < g_nobjs; i++) if (g_objs[i].id == obj) width = g_objs[i].w; }
	if (abs(ac->x - x) < width / 2) dir = ac->y > y ? 3 : 2;
	act_turn_to(a, dir);
}
int g_walk_dir = -1;                                     /* old-style direction (0..3) or -1 */
void act_walk_to(int a, int x, int y)                    /* startWalkActor */
{
	if (getenv("TRACE_ANIM")) fprintf(stderr, "walkTo(%d,%d,%d) from %d,%d\n", a, x, y, g_actors[a].x, g_actors[a].y);
	walk_start(a, x, y, g_walk_dir);
}
void act_walk_to_obj(int a, int obj) { int x, y; if (eng_obj_xy(obj, &x, &y)) { extern int eng_obj_walk_dir(int); g_walk_dir = obj > g_vars[VAR_NUM_ACTOR] ? eng_obj_walk_dir(obj) : -1; act_walk_to(a, x, y); g_walk_dir = -1; } }
void act_walk_to_actor(int a, int b, int dist)           /* handler 79de */
{
	Actor *A = &g_actors[a], *B = &g_actors[b];
	if (A->room != g_room || B->room != g_room) return;
	if (dist == 0xFF) dist = B->width / 2 + A->width;    /* 79de: default stopping distance */
	int x = B->x < A->x ? B->x + dist : B->x - dist;
	walk_start(a, x, B->y, -1);
}
extern void act_step_anim(int a);
void act_animate(int a, int anim)                       /* 8272 */
{
	Actor *ac = &g_actors[a]; int in_room = ac->room == g_room;
	if (getenv("TRACE_ANIM")) fprintf(stderr, "animateActor(%d,%#x) facing=%d stand=%d\n", a, anim, ac->facing, ac->stand_frame);
	switch (anim & 0xFC) {
	case 0xFC: if (in_room) { ac->moving = 0; act_start_frame(a, ac->stand_frame); } break;      /* stop walking, stand */
	case 0xF8: if (in_room) ac->moving &= ~4; act_set_facing(a, anim & 3); break;               /* face a direction now */
	case 0xF4: if (in_room) act_turn_to(a, anim & 3); else ac->facing = anim & 3; break;        /* turn towards a direction */
	default: if (in_room) act_start_anim(a, (anim << 2) | ac->facing); break;                    /* play frame `anim` */
	}
}
int act_in_box(int a, int box) { Actor *ac = &g_actors[a]; return walk_in_box(box, ac->x, ac->y); }
int act_box_of(int x, int y) { for (int i = 0; i < g_nboxes; i++) if (walk_in_box(i, x, y)) return i; return 0xFF; }
int eng_box_in(int box, int x, int y) { return walk_in_box(box, x, y); }
void act_update_all(void)
{
	for (int a = 1; a < 13; a++) {
		Actor *ac = &g_actors[a];
		if (!ac->visible || ac->room != g_room) continue;
		if (a == 1 && ac->moving && getenv("TRACE_EGO")) fprintf(stderr, "ego %d,%d moving=%d dest %d,%d\n", ac->x, ac->y, ac->moving, ac->wd.dest_x, ac->wd.dest_y);
		walk_actor(a);
		act_step_anim(a);
		if (getenv("CHECK_LIMBS") && !ac->moving) for (int i = 0; i < 16; i++) if (ac->frame[i] != 0xFFFF && (ac->frame[i] & 3) != ac->facing) { fprintf(stderr, "LIMB MISMATCH actor %d limb %d frame=%04x facing=%d\n", a, i, ac->frame[i], ac->facing); break; }
	}
}
const u8 *act_name(int a) { return (const u8 *)g_actors[a].name; }

void act_state(StateIO *io) { SIO(io, g_actors); SIO(io, g_walk_dir); }
