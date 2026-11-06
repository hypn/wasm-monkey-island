/* Engine services: rooms, objects, inventory, camera, resources, misc ops. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>

int g_room, g_room_w, g_room_h;
u8 *g_roomimg; u8 *g_bg; u8 *g_mask; static u8 *g_roomplane;   /* g_mask: 1 = foreground pixel that hides actors */
Box g_boxes[MAX_BOXES]; int g_nboxes; const u8 *g_box_matrix, *g_box_matrix_end;
RoomObject g_objs[MAX_ROOM_OBJECTS]; int g_nobjs;
s16 g_cam_cur = 160, g_cam_dest = 160, g_cam_follow;
u16 g_inventory[NUM_INVENTORY];
u8 g_inv_obj_data[NUM_INVENTORY][64];
u8 g_pseudo_room[128];
u8 g_room_pal[16];
int g_ticks_per_frame = 4;
int g_mouse_x, g_mouse_y, g_mouse_buttons, g_key_pressed;
static u8 g_obj_names_over[NUM_OBJECTS][64]; static u8 g_obj_has_name_over[NUM_OBJECTS];
static const u8 *g_exit_code, *g_entry_code; static u32 g_exit_size, g_entry_size;
static int g_screen_dirty = 1;
int g_effect;
void eng_screen_dirty(void) { g_screen_dirty = 1; }
void eng_room_effect(int v) { g_effect = v; }

extern void vm_tick_delays(int ticks);
extern void eng_run_code(const u8 *code, int number, int where, const s16 *args);
extern int  new_dir_to_old(int d);

/* ---------- ownership / state ---------- */
u8 *eng_owner_ptr(int obj) { return (obj > 0 && obj < NUM_OBJECTS) ? &g_obj_owner[obj] : NULL; }
int eng_get_state(int obj) { u8 *p = eng_owner_ptr(obj); return p ? (*p >> 4) : 0; }
void eng_set_owner_state(int obj, int v) { u8 *p = eng_owner_ptr(obj); if (getenv("TRACE_OBJ") && obj == atoi(getenv("TRACE_OBJ"))) fprintf(stderr, "   obj %d state<-%d (slot script %d)\n", obj, v >> 4, g_cur_slot < NUM_SLOTS ? g_slots[g_cur_slot].number : -1); if (p) *p = (u8)v; }

/* ---------- objects of the current room ---------- */
static RoomObject *find_room_obj(int obj)
{
	for (int i = 0; i < g_nobjs; i++) if (g_objs[i].id == obj) return &g_objs[i];
	return NULL;
}
static int inv_slot_of(int obj) { for (int i = 0; i < NUM_INVENTORY; i++) if (g_inventory[i] == obj) return i; return -1; }

int eng_obj_where(int obj)             /* FUN_1000_a2e4 */
{
	if (obj <= 0 || obj >= NUM_OBJECTS) return WIO_NOTFOUND;
	if ((g_obj_owner[obj] & 0xF) != 0xF) {
		if (inv_slot_of(obj) >= 0) return WIO_INVENTORY;
		return WIO_NOTFOUND;
	}
	if (find_room_obj(obj)) return WIO_ROOM;
	return WIO_NOTFOUND;
}
int eng_obj_index(int obj) { return eng_obj_where(obj); }

const u8 *eng_obj_code(int obj, int where)
{
	if (where == WIO_INVENTORY) { int s = inv_slot_of(obj); return s >= 0 ? g_inv_obj_data[s] : NULL; }
	RoomObject *o = find_room_obj(obj); return o ? o->oc : NULL;
}

/* FUN_1000_60c4: verb entry point lookup. verbs table at OC+19: {verb u8, off u16}*, 0 terminated; 0xFF = default */
int eng_find_verb_entry(int obj, int verb)            /* FUN_1000_60c4: first table entry for `verb` OR the 0xFF default, in table order */
{
	int where = eng_obj_where(obj); if (where == WIO_NOTFOUND) return 0;
	const u8 *oc = eng_obj_code(obj, where); if (!oc) return 0;
	for (const u8 *p = oc + 19; *p; p += 3) if (*p == verb || *p == 0xFF) return rd16(p + 1);
	return 0;
}

const u8 *eng_obj_name(int obj)
{
	if (obj > 0 && obj < NUM_OBJECTS && g_obj_has_name_over[obj]) return g_obj_names_over[obj];
	int where = eng_obj_where(obj); if (where == WIO_NOTFOUND) return (const u8 *)"";
	const u8 *oc = eng_obj_code(obj, where); if (!oc) return (const u8 *)"";
	return oc + oc[18];
}
void eng_set_object_name(int obj, const u8 *text, int len)
{
	if (obj <= 0 || obj >= NUM_OBJECTS) return;
	if (len > 63) len = 63;
	memcpy(g_obj_names_over[obj], text, len); g_obj_names_over[obj][len] = 0; g_obj_has_name_over[obj] = 1;
}

extern int walk_snap(int actor, int *x, int *y);
int eng_obj_xy(int obj, int *x, int *y)        /* FUN_1000_a4a6 (+ a2e4): walk position of a room object, snapped into the walkboxes */
{
	if (obj <= g_vars[VAR_NUM_ACTOR]) { if (obj < 0) return 0; *x = g_actors[obj].x; *y = g_actors[obj].y; return 1; }
	RoomObject *o = find_room_obj(obj);
	if (!o || eng_obj_where(obj) != WIO_ROOM) return 0;
	int px = o->walk_x, py = o->walk_y;
	walk_snap(g_vars[VAR_EGO], &px, &py);
	*x = px; *y = py; return 1;
}
int eng_obj_pos(int obj, int *x, int *y)       /* FUN_1000_a458: actors, room objects, or (for inventory items) the holder's position */
{
	if (obj <= g_vars[VAR_NUM_ACTOR]) return eng_obj_xy(obj, x, y);
	int w = eng_obj_where(obj);
	if (w == WIO_NOTFOUND) return 0;
	if (w == WIO_INVENTORY) { int holder = g_obj_owner[obj] & 0xF; if (holder > g_vars[VAR_NUM_ACTOR]) return 0; *x = g_actors[holder].x; *y = g_actors[holder].y; return 1; }
	return eng_obj_xy(obj, x, y);
}
int eng_obj_walk_dir(int obj) { RoomObject *o = find_room_obj(obj); return o ? o->actordir : 0; }

/* ---------- drawing the objects into the room image ---------- */
static void put_mask(int x, int y, u8 bits)
{
	if (y < 0 || y >= g_room_h) return;
	for (int b = 0; b < 8; b++) if ((bits & (0x80 >> b)) && x + b >= 0 && x + b < g_room_w) g_mask[y * g_room_w + x + b] = 1;
}
/* An object with a parent is only visible/clickable while the parent's state equals the stored parent state (bit 7 of OC byte +10). */
static int parent_chain_ok(const RoomObject *o)
{
	const RoomObject *cur = o;
	for (int guard = 0; guard <= g_nobjs; guard++) {
		if (cur->parent == 0) return 1;
		int pi = cur->parent - 1; if (pi < 0 || pi >= g_nobjs) return 0;
		int ps = cur->parentstate, pstate = (eng_get_state(g_objs[pi].id) << 4) & 0xF0;
		if (pstate != ps) return 0;
		cur = &g_objs[pi];
	}
	return 1;
}
static void rebuild_bg(void)
{
	if (!g_roomimg) return;
	memcpy(g_bg, g_roomimg, (size_t)g_room_w * g_room_h);
	if (g_roomplane) memcpy(g_mask, g_roomplane, (size_t)g_room_w * g_room_h); else memset(g_mask, 0, (size_t)g_room_w * g_room_h);
	for (int i = 0; i < g_nobjs; i++) {
		RoomObject *o = &g_objs[i];
		int st = eng_get_state(o->id);
		if (st <= 0 || !o->n_oi || !parent_chain_ok(o)) continue;
		int idx = st - 1; if (idx >= o->n_oi) idx = o->n_oi - 1;
		const u8 *oi = o->oi[idx]; if (!oi) continue;
		if (!o->w || !o->h) continue;
		/* OI block: 6 hdr, u16 id, then image (u16 size + strip offsets) */
		const u8 *img = oi + 8;
		gfx_draw_strips(g_bg, g_room_w, g_room_h, img, o->x / 8, o->w / 8, o->y, o->h);
		/* foreground mask stored after the image: u16 size, u16 strip offsets, then per-strip RLE (1 byte = 8 pixels per row) */
		{
			const u8 *mk = img + rd16(img); int ns = o->w / 8;
			if (rd16(mk) > 2 * ns + 2) for (int sI = 0; sI < ns; sI++) {
				const u8 *p = mk + rd16(mk + 2 + 2 * sI); int row = 0;
				while (row < o->h) {
					u8 b = *p++;
					if (b & 0x80) { u8 c = *p++; for (int k = b & 0x7F; k > 0 && row < o->h; k--, row++) put_mask(o->x + sI * 8, o->y + row, c); }
					else for (int k = b; k > 0 && row < o->h; k--, row++) put_mask(o->x + sI * 8, o->y + row, *p++);
				}
			}
		}
	}
}
void eng_redraw_object(int obj) { rebuild_bg(); g_screen_dirty = 1; }

void eng_draw_object(int obj, int x, int y)    /* handler 7014 */
{
	if (g_trace) fprintf(stderr, "   drawObject(%d,%d,%d) where=%d\n", obj, x, y, eng_obj_where(obj));
	if (eng_obj_where(obj) != WIO_ROOM) return;
	RoomObject *o = find_room_obj(obj);
	if (x != 0xFF) {
		o->walk_x += (s16)((x * 8) - o->x); o->walk_y += (s16)((y * 8) - o->y);    /* 7014 moves the walk point with the object */
		o->x = (s16)(x * 8); o->y = (s16)(y * 8);
	}
	/* objects occupying the same rectangle lose their drawn state */
	for (int i = 0; i < g_nobjs; i++) {
		RoomObject *p = &g_objs[i];
		if (p != o && p->x == o->x && p->y == o->y && p->w == o->w && p->h == o->h) {
			int st = eng_get_state(p->id); (void)st;
			g_obj_owner[p->id] &= 0x0F;
		}
	}
	g_obj_owner[obj] = (g_obj_owner[obj] & 0x0F) | 0x10;
	rebuild_bg(); g_screen_dirty = 1;
}

void eng_remove_inventory(int obj) { int s = inv_slot_of(obj); if (s >= 0) g_inventory[s] = 0; }
void eng_pickup_object(int obj)       /* handler 713c */
{
	if (eng_obj_where(obj) != WIO_ROOM) return;
	RoomObject *o = find_room_obj(obj);
	int slot = -1; for (int i = 1; i < NUM_INVENTORY; i++) if (!g_inventory[i]) { slot = i; break; }
	if (slot < 0) return;
	g_inventory[slot] = obj;
	u32 n = o->oc_size; if (n > 64) n = 64;
	memcpy(g_inv_obj_data[slot], o->oc, n);
	/* the code part may be larger than 64 bytes: keep a private copy of the full block */
	{ u8 *copy = malloc(o->oc_size); memcpy(copy, o->oc, o->oc_size); extern const u8 *g_inv_full[NUM_INVENTORY]; g_inv_full[slot] = copy; }
	g_obj_owner[obj] = (u8)((g_vars[VAR_EGO] & 0xF) | 0x10);
	rebuild_bg(); g_screen_dirty = 1;
	if (g_vars[VAR_INVENTORY_SCRIPT]) { s16 a[16] = {1}; vm_start_script(g_vars[VAR_INVENTORY_SCRIPT], 0, 0, a); }
}
const u8 *g_inv_full[NUM_INVENTORY];

int eng_inv_count(int owner)
{
	int n = 0;
	for (int i = 1; i < NUM_INVENTORY; i++) if (g_inventory[i] && (g_obj_owner[g_inventory[i]] & 0xF) == owner) n++;
	return n;
}
int eng_find_inventory(int owner, int idx)
{
	int n = 1;
	for (int i = 1; i < NUM_INVENTORY; i++)
		if (g_inventory[i] && (g_obj_owner[g_inventory[i]] & 0xF) == owner) { if (n++ == idx) return g_inventory[i]; }
	return 0;
}

int eng_get_class(int obj, int cls) { if (obj <= 0 || obj >= NUM_OBJECTS || cls < 1) return 0; return (g_obj_class[obj] >> (cls - 1)) & 1; }
void eng_set_class(int obj, int cls, int on)
{
	if (obj <= 0 || obj >= NUM_OBJECTS || cls < 1) return;
	if (on) g_obj_class[obj] |= 1u << (cls - 1); else g_obj_class[obj] &= ~(1u << (cls - 1));
}

int eng_find_object(int x, int y)       /* FUN_1000_4196: first object in room order whose 8x8 cell range contains the point */
{
	if (g_trace) fprintf(stderr, "   findObject(%d,%d)\n", x, y);
	for (int i = 0; i < g_nobjs; i++) {
		RoomObject *o = &g_objs[i];
		if (!o->id || eng_get_class(o->id, 24)) continue;               /* class 24 = untouchable */
		if (!parent_chain_ok(o)) continue;
		if (o->x / 8 <= (x >> 3) && (x >> 3) < o->x / 8 + (int)(o->w / 8) && o->y / 8 <= (y >> 3) && (y >> 3) < o->y / 8 + (int)(o->h / 8)) return o->id;
	}
	return 0;
}
int eng_dist(int a, int b)              /* FUN_1000_678e: Chebyshev distance of the a458 positions; 0xFF if unknown */
{
	int na = g_vars[VAR_NUM_ACTOR];
	if (a <= na && b <= na && a >= 0 && b >= 0 && g_actors[a].room == g_actors[b].room && g_actors[a].room != 0 && g_actors[a].room != g_room) return 0;
	int ax, ay, bx, by;
	if (!eng_obj_pos(a, &ax, &ay) || !eng_obj_pos(b, &bx, &by)) return 0xFF;
	int dx = abs(ax - bx), dy = abs(ay - by);
	return dx > dy ? dx : dy;
}

/* ---------- room loading ---------- */
void eng_load_room_data(int room)
{
	u32 sz;
	const u8 *hd = res_room_block(room, "HD", 0, &sz);
	g_room_w = rd16(hd + 6); g_room_h = rd16(hd + 8);
	if (g_room_h > 144) g_room_h = 144;
	free(g_roomimg); free(g_bg); free(g_mask); free(g_roomplane); g_roomplane = NULL;
	g_roomimg = calloc((size_t)g_room_w * g_room_h, 1); g_bg = calloc((size_t)g_room_w * g_room_h, 1); g_mask = calloc((size_t)g_room_w * g_room_h, 1);
	const u8 *bm = res_room_block(room, "BM", 0, &sz);
	if (bm) gfx_draw_strips(g_roomimg, g_room_w, g_room_h, bm + 6, 0, g_room_w / 8, 0, g_room_h);
	/* The BM payload is a chain of images, each starting with its own u16 size (FUN_1000_2e02 counts up to 4):
	 * image 0 = background, image 1 = foreground mask plane (per strip: byte-per-row RLE, MSB = leftmost pixel). */
	if (bm) {
		const u8 *img0 = bm + 6; u32 s0 = rd16(img0);
		if (s0 && 6 + s0 + 2 <= sz) {
			const u8 *pl = img0 + s0; int ns = g_room_w / 8;
			if (rd16(pl) > 2 * ns + 2) {
				g_roomplane = calloc((size_t)g_room_w * g_room_h, 1);
				for (int sI = 0; sI < ns; sI++) {
					const u8 *p = pl + rd16(pl + 2 + 2 * sI); int row = 0;
					while (row < g_room_h) {
						u8 b = *p++, rep = (b & 0x80) != 0; int cnt = b & 0x7F, lit = 0; u8 c = 0;
						if (rep) c = *p++;
						for (; cnt > 0 && row < g_room_h; cnt--, row++) {
							u8 bits = rep ? c : p[lit++];
							for (int k = 0; k < 8; k++) if (bits & (0x80 >> k)) g_roomplane[row * g_room_w + sI * 8 + k] = 1;
						}
						if (!rep) p += lit;
					}
				}
			}
		}
	}
	/* boxes */
	const u8 *bx = res_room_block(room, "BX", 0, &sz);
	g_nboxes = 0; g_box_matrix = NULL;
	if (bx) {
		int n = bx[6]; if (n > MAX_BOXES) n = MAX_BOXES;
		const u8 *p = bx + 7;
		for (int i = 0; i < n; i++, p += 20) {
			Box *b = &g_boxes[i];
			b->ulx = (s16)rd16(p); b->uly = (s16)rd16(p + 2); b->urx = (s16)rd16(p + 4); b->ury = (s16)rd16(p + 6);
			b->lrx = (s16)rd16(p + 8); b->lry = (s16)rd16(p + 10); b->llx = (s16)rd16(p + 12); b->lly = (s16)rd16(p + 14);
			b->mask = p[16]; b->flags = p[17]; b->scale = rd16(p + 18);
		}
		g_nboxes = n; g_box_matrix = p; g_box_matrix_end = bx + sz; walk_load_matrix();
	}
	/* objects */
	g_nobjs = 0; memset(g_objs, 0, sizeof g_objs);
	for (int n = 0;; n++) {
		const u8 *oc = res_room_block(room, "OC", n, &sz);
		if (!oc || g_nobjs >= MAX_ROOM_OBJECTS) break;
		RoomObject *o = &g_objs[g_nobjs++];
		o->id = rd16(oc + 6); o->x = oc[9] * 8; o->y = (oc[10] & 0x7F) * 8; o->w = oc[11] * 8;
		o->parent = oc[12]; o->parentstate = (oc[10] & 0x80) ? 0x10 : 0; o->walk_x = (s16)rd16(oc + 13); o->walk_y = (s16)rd16(oc + 15);
		o->h = oc[17] & 0xF8; o->actordir = oc[17] & 7; o->oc = oc; o->oc_size = sz;
	}
	for (int n = 0;; n++) {
		const u8 *oi = res_room_block(room, "OI", n, &sz);
		if (!oi) break;
		int id = rd16(oi + 6);
		RoomObject *o = find_room_obj(id);
		if (o && o->n_oi < 16) o->oi[o->n_oi++] = oi;
	}
	{ memset(g_scale_slots, 0, sizeof g_scale_slots);
	  const u8 *sa = res_room_block(room, "SA", 0, &sz);          /* two slots of (s1, y1, s2, y2) words, read by FUN_1000_2c6c */
	  if (sa) for (int i = 0; i < 2 && (u32)(6 + 8 * i + 8) <= sz; i++) {
		g_scale_slots[i].s1 = rd16(sa + 6 + 8 * i); g_scale_slots[i].y1 = rd16(sa + 8 + 8 * i);
		g_scale_slots[i].s2 = rd16(sa + 10 + 8 * i); g_scale_slots[i].y2 = rd16(sa + 12 + 8 * i);
	  } }
	g_exit_code = g_entry_code = NULL;
	const u8 *ex = res_room_block(room, "EX", 0, &g_exit_size); if (ex) g_exit_code = ex + 6;
	const u8 *en = res_room_block(room, "EN", 0, &g_entry_size); if (en) g_entry_code = en + 6;
	for (int i = 0; i < 16; i++) g_room_pal[i] = (u8)i;
	/* objects that already carry a state are composed */
	rebuild_bg();
}

void eng_run_exit(void)
{
	if (g_vars[VAR_EXIT_SCRIPT]) { s16 a[16] = {0}; vm_start_script(g_vars[VAR_EXIT_SCRIPT], 0, 0, a); }
	if (g_exit_code) eng_run_code(g_exit_code, 999, WIO_ROOM, NULL);
	if (g_vars[VAR_EXIT_SCRIPT2]) { s16 a[16] = {0}; vm_start_script(g_vars[VAR_EXIT_SCRIPT2], 0, 0, a); }
}
void eng_run_entry(void)
{
	if (g_vars[VAR_ENTRY_SCRIPT]) { s16 a[16] = {0}; vm_start_script(g_vars[VAR_ENTRY_SCRIPT], 0, 0, a); }
	if (g_entry_code) eng_run_code(g_entry_code, 998, WIO_ROOM, NULL);
	if (g_vars[VAR_ENTRY_SCRIPT2]) { s16 a[16] = {0}; vm_start_script(g_vars[VAR_ENTRY_SCRIPT2], 0, 0, a); }
}

void eng_kill_local_scripts(void)
{
	for (int i = 1; i < NUM_SLOTS; i++) {
		Slot *s = &g_slots[i];
		if (s->status == SS_DEAD) continue;
		if (s->where == WIO_ROOM || (s->where == WIO_GLOBAL && s->number >= g_n_script)) {
			s->status = SS_DEAD; s->number = 0; if (g_cur_slot == i) g_cur_slot = 0xff;
		}
	}
}

void eng_start_room(int room)           /* FUN_1000_2a6e */
{
	if (getenv("TRACE_ROOM")) fprintf(stderr, "start_room %d (ticks=%d)\n", room, g_vars[VAR_TIMER_TOTAL]);
	/* DEVIATION (unresolved): the logo room's exit script parks actors 1..9 in room 0, which would also remove
	 * ego that script 1 has just placed in the destination room; the original evidently keeps ego, so actors
	 * already assigned to the destination room are restored after the exit scripts ran. */
	u8 pending[16]; s16 px[16], py[16]; for (int a = 0; a < 13; a++) { pending[a] = (g_actors[a].room == room && room); px[a] = g_actors[a].x; py[a] = g_actors[a].y; }
	if (g_room) eng_run_exit();
	for (int a = 0; a < 13; a++) if (pending[a]) { g_actors[a].room = room; g_actors[a].x = px[a]; g_actors[a].y = py[a]; }
	eng_kill_local_scripts();
	for (int a = 1; a < 13; a++) if (g_actors[a].visible) { g_actors[a].visible = 0; }
	if (room & 0x80) room = g_pseudo_room[room & 0x7f];
	g_room = room; g_vars[VAR_ROOM] = room;
	g_cam_cur = g_cam_dest = 160; g_cam_follow = 0;
	if (room) {
		eng_load_room_data(room);
		g_vars[VAR_CAMERA_MIN_X] = 160;
		g_vars[VAR_CAMERA_MAX_X] = (g_room_w / 8 - 20) * 8;
		for (int a = 1; a < 13; a++) if (g_actors[a].room == room) act_put(a, g_actors[a].x, g_actors[a].y, room);
		eng_run_entry();
	} else { g_nobjs = 0; g_nboxes = 0; }
	g_screen_dirty = 1;
}

void eng_load_room_with_ego(int obj, int room, int x, int y)    /* handler 726e */
{
	int ego = g_vars[VAR_EGO];
	int ox = 0, oy = 0;
	act_put(ego, g_actors[ego].x, g_actors[ego].y, room);
	g_vars[VAR_WALKTO_OBJ] = obj;
	if (room != g_room) eng_start_room(room);
	g_vars[VAR_WALKTO_OBJ] = 0;
	if (eng_obj_where(obj) == WIO_NOTFOUND) { /* object not found: ego stays */ }
	else if (eng_obj_xy(obj, &ox, &oy)) {
		act_put(ego, ox, oy, g_room);
		{ extern int eng_obj_walk_dir(int); act_set_facing(ego, (eng_obj_walk_dir(obj) ^ 1) & 3); }   /* 726e: animate 0xF8 + (dir^1): a real direction change, so every limb follows */
		(void)new_dir_to_old;
	}
	g_vars[VAR_CAMERA_POS_X] = g_actors[ego].x;
	eng_set_camera_at(g_actors[ego].x);
	eng_follow_actor(ego);                                   /* 726e ends with FUN_1000_7e22(ego) */
	if (x != -1) act_walk_to(ego, x, y);
}

/* ---------- camera ---------- */
static int clamp_cam(int x)
{
	int lo = g_vars[VAR_CAMERA_MIN_X], hi = g_vars[VAR_CAMERA_MAX_X];
	if (x < lo) x = lo; if (x > hi) x = hi; return x;
}
void eng_set_camera_at(int x) { g_cam_cur = (s16)clamp_cam(x); g_cam_dest = g_cam_cur; g_cam_follow = 0; g_screen_dirty = 1; }
void eng_pan_camera(int x) { g_cam_dest = (s16)clamp_cam(x); g_cam_follow = 0; }
static int cam_outside_window(int x)      /* FUN_1000_7e3c / 151e: follow window is strips 10..30 of the 40-strip view */
{
	int left = (g_cam_cur - 160) >> 3; if (left < 0) left = 0;
	int s = (x >> 3) - left; return s < 10 || s > 30;
}
void eng_follow_actor(int a)
{
	g_cam_follow = a;
	/* FUN_1000_7e3c: following an actor that is in another room switches to that room */
	if (a && g_actors[a].room != g_room) { int r = g_actors[a].room; int ax = g_actors[a].x, ay = g_actors[a].y; eng_start_room(r); act_put(a, ax, ay, r); g_cam_cur = g_cam_dest = (s16)clamp_cam(ax); g_cam_follow = a; g_screen_dirty = 1; return; }   /* 7e3c re-sets follow mode (4db1=2) AFTER 2a6e, which resets it to 3 */
	if (a && cam_outside_window(g_actors[a].x)) { g_cam_cur = g_cam_dest = (s16)clamp_cam(g_actors[a].x); }
	g_screen_dirty = 1;
}
static void camera_update(void)
{
	if (g_cam_follow) {
		Actor *a = &g_actors[g_cam_follow];
		if (a->room == g_room && cam_outside_window(a->x)) g_cam_dest = (s16)clamp_cam(a->x);
	}
	if (g_cam_cur < g_cam_dest) { g_cam_cur += 8; if (g_cam_cur > g_cam_dest) g_cam_cur = g_cam_dest; }
	else if (g_cam_cur > g_cam_dest) { g_cam_cur -= 8; if (g_cam_cur < g_cam_dest) g_cam_cur = g_cam_dest; }
}

/* ---------- misc ops ---------- */
void eng_resource_routine(int sub, int id) { (void)sub; (void)id; }     /* everything is resident */
void eng_sound_start(int id) { (void)id; }
void eng_sound_stop(int id) { (void)id; }
int  eng_sound_running(int id) { (void)id; return 0; }
void eng_music(int id) { (void)id; }
void eng_cursor_cmd(int sub, int a, int b, int c) { extern void ui_cursor_cmd(int, int, int, int); ui_cursor_cmd(sub, a, b, c); }
extern int g_cutscene_slot;
void eng_freeze_scripts(int v)
{
	if (v) {
		for (int i = 1; i < NUM_SLOTS; i++) {
			Slot *s = &g_slots[i];
			if (i != g_cur_slot && s->status && (!s->freeze_resistant || v >= 0x80)) { s->status |= 0x80; s->freeze_count++; }
		}
		if (g_cutscene_slot != 0xff) { g_slots[g_cutscene_slot].status &= 0x7F; g_slots[g_cutscene_slot].freeze_count = 0; }
	} else {
		for (int i = 1; i < NUM_SLOTS; i++) { Slot *s = &g_slots[i]; if (s->status & 0x80) { if (s->freeze_count && --s->freeze_count == 0) s->status &= 0x7F; } }
	}
}
void eng_cutscene_begin(const s16 *a) { (void)a; }
void eng_cutscene_end(void) {}
void eng_restart(void) { g_halted = 1; }
void eng_matrix_ops(int sub, int a, int b)
{
	if (sub == 1 && a < g_nboxes) g_boxes[a].flags = (u8)b;
	else if (sub == 2 && a < g_nboxes) g_boxes[a].scale = (u16)b;
	else if (sub == 3 && a < g_nboxes) g_boxes[a].scale = (u16)((b - 1) | 0x8000);
	else if (sub == 4) { /* createBoxMatrix: the matrix is stored in the room; keep as is */ }
}
void eng_draw_box(int x1, int y1, int x2, int y2, int color)
{
	if (!g_bg) return;
	for (int y = y1; y <= y2 && y < g_room_h; y++) for (int x = x1; x <= x2 && x < g_room_w; x++) if (x >= 0 && y >= 0) g_bg[y * g_room_w + x] = (u8)color;
	g_screen_dirty = 1;
}
void eng_save_restore_verbs(int sub, int a, int b, int c) { extern void ui_save_restore_verbs(int, int, int, int); ui_save_restore_verbs(sub, a, b, c); }

void eng_room_ops(int sub, int a, int b, int c, int d, int e)
{
	switch (sub) {
	case 1: {                                           /* roomScroll: set camera limits */
		int mx = (g_room_w / 8 - 20) * 8;
		if (a < 160) a = 160; if (a > mx) a = mx; if (b < 160) b = 160; if (b > mx) b = mx;
		g_vars[VAR_CAMERA_MIN_X] = a; g_vars[VAR_CAMERA_MAX_X] = b; break; }
	case 2: if (b >= 0 && b < 16) g_room_pal[b] = (u8)a; g_screen_dirty = 1; break;       /* palette remap */
	case 3: { extern void ui_set_screen(int, int); ui_set_screen(a, b); break; }
	case 4: break;
	case 5: case 6: break;                               /* shake */
	case 7: { int sl = e - 1; if (sl >= 0 && sl < 20) { g_scale_slots[sl].y1 = b; g_scale_slots[sl].s1 = a; g_scale_slots[sl].y2 = d; g_scale_slots[sl].s2 = c; } break; }
	default: break;
	}
}
int eng_string_get_char(int s, int i);
int eng_actor_from_pos(int x, int y)          /* FUN_1000_413a: first actor whose last drawn bounds contain the point (room coords) */
{
	for (int a = 1; a < 13; a++) {
		Actor *ac = &g_actors[a];
		if (!ac->visible || ac->room != g_room || !ac->hvalid || eng_get_class(a, 24)) continue;
		if (x >= ac->hx1 && x <= ac->hx2 && y >= ac->hy1 && y <= ac->hy2) return a;
	}
	return 0;
}
int eng_sentence_idle(void);

/* ---------- main per-frame step ---------- */
extern int vm_pop_sentence(int *verb, int *a, int *b);
extern int ui_tick(void);
void eng_frame(void)
{
	int ticks = g_vars[VAR_TIMER_NEXT] > 0 ? g_vars[VAR_TIMER_NEXT] : 4;
	g_vars[VAR_TIMER] = ticks; g_vars[VAR_TIMER_TOTAL] += ticks;
	g_vars[VAR_TMR_1] += ticks; g_vars[VAR_TMR_2] += ticks; g_vars[VAR_TMR_3] += ticks;
	vm_tick_delays(ticks);
	g_vars[VAR_CAMERA_POS_X] = g_cam_cur;
	g_vars[VAR_MOUSE_X] = (s16)g_mouse_x; g_vars[VAR_MOUSE_Y] = (s16)g_mouse_y;
	{ extern int g_view_top, g_view_h; int cx = g_cam_cur - 160; if (cx < 0) cx = 0;
		int vy = g_mouse_y - g_view_top; if (vy < 0 || vy >= g_view_h) vy = -1;
		g_vars[VAR_VIRT_MOUSE_X] = (s16)(g_mouse_x + cx); g_vars[VAR_VIRT_MOUSE_Y] = (s16)vy; }
	ui_tick();                         /* input -> verb/sentence scripts */
	vm_run_frame();
	act_update_all();
	camera_update();
	txt_update();
	render_frame();
}

void ui_init(void);
void eng_init(void)
{
	vm_init(); ui_init();
	for (int i = 0; i < 16; i++) act_init(i);
	g_vars[VAR_TIMER_NEXT] = 4; g_vars[VAR_CHARINC] = 4; g_vars[VAR_CURRENT_LIGHTS] = 11; g_vars[VAR_NUM_ACTOR] = 13;
	g_vars[VAR_ACTOR_RANGE_MIN] = 1; g_vars[VAR_ACTOR_RANGE_MAX] = 12; g_vars[VAR_VIDEOMODE] = 13; g_vars[VAR_SOUNDCARD] = 0;
	for (int i = 0; i < 16; i++) g_room_pal[i] = (u8)i;
	g_mouse_x = 104; g_mouse_y = 56;
	vm_boot();
}
