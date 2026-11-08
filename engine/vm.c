/* Script VM. Every handler below corresponds to an entry of the 256-entry far-pointer
 * dispatch table at DS:0x048c of MIDEMO.EXE (see RE_NOTES.md for the opcode->handler map).
 * Handler address of the original is given in the comment next to each implementation. */
#include "engine.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>

s16 g_vars[NUM_VARS];
u8 g_bitvars[NUM_BITVARS / 8];
Slot g_slots[NUM_SLOTS];
int g_cur_slot = 0xff;
int g_trace = 0;
int g_halted = 0;

/* interpreter registers (DS:6f42 pc, DS:72a5 opcode, DS:6f4c result position) */
static const u8 *code_base;
static u32 pc;
static u8 opcode;
static u16 result_pos;
static int nest;

/* sentence stack (DS:5696.. in the original) */
typedef struct { u8 verb; u16 a, b; u8 fresh; } Sentence;
static Sentence sentences[6]; static int nsent = -1;

/* cutscene / override state */
int g_cutscene_slot = 0xff; static int cutscene_depth; static s16 cutscene_stack[6]; static u32 override_pc; static int override_slot = 0xff;
static const u8 *override_base; static int override_number;

static void vm_error(const char *msg, int a)
{
	fprintf(stderr, "VM error: %s (%d) slot=%d script=%d pc=%u op=%02x\n", msg, a, g_cur_slot,
	        g_cur_slot < NUM_SLOTS ? g_slots[g_cur_slot].number : -1, pc, opcode);
}

/* ---- low-level fetch ---- */
static inline u8 fetch8(void) { return code_base[pc++]; }
static inline s16 fetch16(void) { s16 v = (s16)(code_base[pc] | (code_base[pc + 1] << 8)); pc += 2; return v; }
static void skip16(void) { pc += 2; }                         /* FUN_1000_5d3a */

/* Inline script text (ScummVM resStrLen): 0-terminated; an 0xFF escape is `FF <code>` followed by a 16-bit operand
 * for every code except 1 (newline), 2 (keep text), 3 (wait) and 8 (verb-line newline). Returns the byte count incl. the 0. */
static int fetch_inline_text(u8 *buf, int max)
{
	int n = 0; u8 c;
	do {
		c = fetch8(); if (n < max - 1) buf[n++] = c;
		if (c == 0xFF) {
			u8 code = fetch8(); if (n < max - 1) buf[n++] = code;
			if (code != 1 && code != 2 && code != 3 && code != 8) for (int i = 0; i < 2; i++) { u8 b = fetch8(); if (n < max - 1) buf[n++] = b; }
		}
	} while (c);
	buf[n - 1 < 0 ? 0 : n - 1] = 0; return n;
}

/* ---- variables (FUN_1000_64d4 / FUN_1000_6596) ---- */
static s16 get_var(u16 idx)
{
	if (idx & 0xF000) {
		if (idx & 0x2000) {
			u16 a = (u16)fetch16();
			if (a & 0x2000) a = (u16)get_var(a & 0xDFFF); else a &= 0xFFF;
			idx = (u16)((idx + a) & 0xDFFF);
		}
		if (idx & 0xF000) {
			if (idx & 0x8000) { int b = idx & 0xFFF; return (g_bitvars[b >> 3] >> (b & 7)) & 1; }
			if (idx & 0x4000) return g_slots[g_cur_slot].vars[idx & 0xFFF];
			vm_error("bad var", idx); return 0;
		}
	}
	if (idx >= NUM_VARS) { vm_error("var oob", idx); return 0; }
	return g_vars[idx];
}
int vm_read_var(u16 idx)                      /* variable operand embedded in text (0xFF 4..7 <u16>): global / bit / local, no indexing */
{
	if (idx & 0x8000) { int b = idx & 0xFFF; return (g_bitvars[b >> 3] >> (b & 7)) & 1; }
	if (idx & 0x4000) return g_slots[g_cur_slot].vars[idx & 0xFFF];
	return idx < NUM_VARS ? g_vars[idx] : 0;
}
static void set_var_at(u16 pos, s16 v)
{
	{ static int tv = -2; if (tv == -2) tv = getenv("TRACE_VAR") ? atoi(getenv("TRACE_VAR")) : -1; if (tv >= 0 && pos == tv) fprintf(stderr, "   var[%d] <- %d (script %d)\n", pos, v, g_cur_slot < NUM_SLOTS ? g_slots[g_cur_slot].number : -1); }
	if ((pos & 0xF000) == 0) { if (pos < NUM_VARS) g_vars[pos] = v; return; }
	if (pos & 0x8000) {
		int b = pos & 0x7FFF; b &= 0xFFF;
		if (v) g_bitvars[b >> 3] |= 1 << (b & 7); else g_bitvars[b >> 3] &= ~(1 << (b & 7));
		return;
	}
	if (pos & 0x4000) g_slots[g_cur_slot].vars[pos & 0xFFF] = v;
}
static void get_result_pos(void)       /* FUN_1000_6624 */
{
	result_pos = (u16)fetch16();
	if (result_pos & 0x2000) {
		u16 a = (u16)fetch16();
		if (a & 0x2000) a = (u16)get_var(a & 0xDFFF); else a &= 0xFFF;
		result_pos = (u16)((result_pos + a) & 0xDFFF);
	}
}
static void set_result(s16 v) { set_var_at(result_pos, v); }

static s16 var_or_word(u8 mask) { s16 w = fetch16(); return (opcode & mask) ? get_var((u16)w) : w; }   /* FUN_1000_6688 */
static s16 var_or_byte(u8 mask)                                                                           /* FUN_1000_665a */
{
	if (opcode & mask) return get_var((u16)fetch16());
	return fetch8();
}
static int get_word_vararg(s16 *args)  /* FUN_1000_66ac */
{
	int n = 0; memset(args, 0, 16 * sizeof(s16));
	while ((opcode = fetch8()) != 0xFF) { s16 v = var_or_word(0x80); if (n < 16) args[n] = v; n++; }
	return n;
}

static void jump_rel(void)             /* FUN_1000_68f6 */
{
	s16 off = fetch16();
	pc = (u32)((s32)pc + off);
}

/* ---- slots ---- */
static int find_free_slot(void)       /* FUN_1000_63d2 */
{
	for (int i = 1; i < NUM_SLOTS; i++) if (g_slots[i].status == SS_DEAD) return i;
	vm_error("no free script slot", 0); return 1;
}

static void slot_kill_by_number(int id, int where_mask)
{
	for (int i = 1; i < NUM_SLOTS; i++) {
		Slot *s = &g_slots[i];
		if (s->number == id && s->status != SS_DEAD && ((1 << s->where) & where_mask)) {
			s->number = 0; s->status = SS_DEAD;
			if (g_cur_slot == i) g_cur_slot = 0xff;
		}
	}
}
void vm_stop_script(int id) { if (id) slot_kill_by_number(id, 1 << WIO_GLOBAL); }          /* FUN_1000_5f2c */
void vm_stop_object_script(int id) { if (id) slot_kill_by_number(id, (1 << WIO_ROOM) | (1 << WIO_INVENTORY)); } /* FUN_1000_5fea */

static void run_slot_until_break(int slot);

/* load code base for a slot (FUN_1000_5d4a) */
static int slot_prepare(int slot)
{
	Slot *s = &g_slots[slot];
	return s->base != NULL;
}

int vm_start_script(int id, int freeze_res, int recursive, const s16 *args)   /* FUN_1000_62e2 */
{
	if (!id) return 0;
	if (!recursive) vm_stop_script(id);
	const u8 *base = NULL; u32 sz = 0;
	if (id < g_n_script) {
		const u8 *b = res_script(id, &sz);
		if (!b) { vm_error("script not found", id); return 0; }
		base = b + 6;
	} else {
		/* local script: LS block in current room, id byte after header */
		for (int n = 0;; n++) {
			const u8 *b = res_room_block(g_room, "LS", n, &sz);
			if (!b) { vm_error("local script not found", id); return 0; }
			if (b[6] == id) { base = b + 7; break; }
		}
	}
	int slot = find_free_slot();
	Slot *s = &g_slots[slot];
	memset(s, 0, sizeof *s);
	s->number = id; s->status = SS_RUNNING; s->where = WIO_GLOBAL; s->base = base; s->pc = 0;
	s->freeze_resistant = freeze_res; s->recursive = recursive;
	if (args) for (int i = 0; i < 16; i++) s->vars[i] = args[i];
	run_slot_until_break(slot);
	return 1;
}

void vm_run_object_script(int obj, int verb, int freeze_res, int recursive, const s16 *args)  /* FUN_1000_61ea */
{
	if (!obj) return;
	if (!recursive) vm_stop_object_script(obj);
	int where = eng_obj_where(obj);
	if (where == WIO_NOTFOUND) return;
	int off = eng_find_verb_entry(obj, verb);
	if (!off) return;
	int slot = find_free_slot();
	Slot *s = &g_slots[slot];
	memset(s, 0, sizeof *s);
	s->number = obj; s->status = SS_RUNNING; s->where = where; s->base = eng_obj_code(obj, where); s->pc = off;
	s->freeze_resistant = freeze_res; s->recursive = recursive;
	if (args) for (int i = 0; i < 16; i++) s->vars[i] = args[i];
	run_slot_until_break(slot);
}

static void run_code_block(int slot, const u8 *base, u32 start_pc, int where, int number)
{
	Slot *s = &g_slots[slot];
	memset(s, 0, sizeof *s);
	s->number = number; s->status = SS_RUNNING; s->where = where; s->base = base; s->pc = start_pc;
	run_slot_until_break(slot);
}
void eng_run_code(const u8 *code, int number, int where, const s16 *args)
{
	int slot = find_free_slot();
	Slot *s = &g_slots[slot];
	memset(s, 0, sizeof *s);
	s->number = number; s->status = SS_RUNNING; s->where = where; s->base = code; s->pc = 0;
	if (args) for (int i = 0; i < 16; i++) s->vars[i] = args[i];
	run_slot_until_break(slot);
}

/* ---- opcode handlers ---- */
typedef void (*Handler)(void);
static Handler optab[256]; static const char *opname[256];
static void exec_opcode(void) { Handler h = optab[opcode]; h(); }

static void op_break_here(void)                                   /* 6a3c: 0x80 */
{
	g_slots[g_cur_slot].pc = pc; g_cur_slot = 0xff;
}
static void op_stop_object_code(void)                             /* 6900/6906: 0x00, 0xa0 */
{
	Slot *s = &g_slots[g_cur_slot];
	s->status = SS_DEAD; s->number = 0; g_cur_slot = 0xff;
}
static void op_jump(void) { jump_rel(); }                         /* 68f6: 0x18 */

/* conditional helpers: handler jumps when the condition is false */
#define COND_JUMP_IF(cond) do { if (cond) jump_rel(); else skip16(); } while (0)
static void op_equal_zero(void)        { s16 v = get_var((u16)fetch16()); COND_JUMP_IF(v != 0); }        /* 6bf2: 0x28 */
static void op_not_equal_zero(void)    { s16 v = get_var((u16)fetch16()); COND_JUMP_IF(v == 0); }        /* 6c10: 0xa8 */
static void op_ge(void)  { s16 v = get_var((u16)fetch16()); s16 b = var_or_word(0x80); COND_JUMP_IF(v < b); }   /* 6c2e: 0x38,0xb8 */
static void op_le(void)  { s16 v = get_var((u16)fetch16()); s16 b = var_or_word(0x80); COND_JUMP_IF(b < v); }   /* 6c62: 0x04,0x84 */
static void op_gt(void)  { s16 v = get_var((u16)fetch16()); s16 b = var_or_word(0x80); COND_JUMP_IF(v <= b); }  /* 6c92: 0x44,0xc4 */
static void op_lt(void)  { s16 v = get_var((u16)fetch16()); s16 b = var_or_word(0x80); COND_JUMP_IF(b <= v); }  /* 6cc2: 0x78,0xf8 */
static void op_ne(void)  { s16 v = get_var((u16)fetch16()); s16 b = var_or_word(0x80); COND_JUMP_IF(b == v); }  /* 6cf6: 0x08,0x88 */
static void op_eq(void)  { s16 v = get_var((u16)fetch16()); s16 b = var_or_word(0x80); COND_JUMP_IF(b != v); }  /* 6d26: 0x48,0xc8 */

static void op_set_var_range(void)                                /* 6a48: 0x26,0xa6 */
{
	get_result_pos();
	int n = fetch8();
	do {
		s16 v;
		if (!(opcode & 0x80)) v = fetch8(); else v = fetch16();
		set_result(v);
		result_pos++;
	} while (--n);
}
static void op_move(void)      { get_result_pos(); set_result(var_or_word(0x80)); }                  /* 6a80: 0x1a,0x9a */
static void op_increment(void) { get_result_pos(); set_result(get_var(result_pos) + 1); }            /* 6a9c: 0x46 */
static void op_decrement(void) { get_result_pos(); set_result(get_var(result_pos) - 1); }            /* 6ab8: 0xc6 */
static void op_divide(void)    { get_result_pos(); s16 b = var_or_word(0x80); s16 a = get_var(result_pos); set_result(b ? a / b : 0); } /* 6ad4 */
static void op_multiply(void)  { get_result_pos(); s16 b = var_or_word(0x80); s16 a = get_var(result_pos); set_result((s16)(a * b)); }  /* 6b0c */
static void op_add(void)       { get_result_pos(); s16 b = var_or_word(0x80); s16 a = get_var(result_pos); set_result((s16)(b + a)); }  /* 6b3a */
static void op_and(void)       { get_result_pos(); s16 b = var_or_word(0x80); s16 a = get_var(result_pos); set_result(b & a); }         /* 6b68 */
static void op_or(void)        { get_result_pos(); s16 b = var_or_word(0x80); s16 a = get_var(result_pos); set_result(b | a); }         /* 6b96 */
static void op_subtract(void)  { get_result_pos(); s16 b = var_or_word(0x80); s16 a = get_var(result_pos); set_result((s16)(a - b)); }  /* 6bc4 */

static void op_stop_object_script(void) { vm_stop_object_script(var_or_word(0x80)); }               /* 6d56: 0x6e */
static void op_stop_script(void)                                  /* 6d6c: 0x62 */
{
	int id = var_or_byte(0x80);
	if (id == 0) id = g_slots[g_cur_slot].number;
	vm_stop_script(id);
}
static void op_start_object(void)                                 /* 6d96: 0x37 */
{
	s16 args[16]; int obj = var_or_word(0x80); int verb = var_or_byte(0x40);
	get_word_vararg(args);
	vm_run_object_script(obj, verb, 0, 0, args);
}
static void op_start_script(void)                                 /* 6dde: 0x0a */
{
	s16 args[16]; int id = var_or_byte(0x80);
	u8 op = opcode;
	get_word_vararg(args);
	vm_start_script(id, (op & 0x20) != 0, (op & 0x40) != 0, args);
}
static void op_chain_script(void)                                 /* 6e40: 0x42,0xc2 */
{
	s16 args[16]; int id = var_or_byte(0x80);
	get_word_vararg(args);
	Slot *s = &g_slots[g_cur_slot];
	int fr = s->freeze_resistant, rec = s->recursive;
	s->status = SS_DEAD; s->number = 0; g_cur_slot = 0xff;
	vm_start_script(id, fr, rec, args);
}

static void op_if_state(void)                                     /* 6ed4: 0x0f */
{
	int obj = var_or_word(0x80); int st = var_or_byte(0x40);
	COND_JUMP_IF(st != eng_get_state(obj));
}
static void op_if_not_state(void)                                 /* 6f48: 0x2f */
{
	int obj = var_or_word(0x80); int st = var_or_byte(0x40);
	COND_JUMP_IF(st == eng_get_state(obj));
}
static void op_actor_in_box(void)                                 /* 6f02: 0x1f */
{
	int a = var_or_byte(0x80); int box = var_or_byte(0x40);
	COND_JUMP_IF(!act_in_box(a, box));
}
static void op_set_state(void)                                    /* 6fd0: 0x07 */
{
	int obj = var_or_word(0x80); int st = var_or_byte(0x40);
	eng_set_owner_state(obj, (st << 4) | (eng_owner_ptr(obj) ? (*eng_owner_ptr(obj) & 0xF) : 0));
	eng_redraw_object(obj);
}
static void op_draw_object(void)                                  /* 7014: 0x05 */
{
	int obj = var_or_word(0x80); int x = var_or_word(0x40); int y = var_or_word(0x20);
	eng_draw_object(obj, x, y);
}
static void op_pickup_object(void)                                /* 713c: 0x50 */
{
	int obj = var_or_word(0x80);
	eng_pickup_object(obj);
}
static void op_load_room(void)                                    /* 7240: 0x72 */
{
	u32 p0 = pc; int room = var_or_byte(0x80);
	if (getenv("TRACE_ROOM")) fprintf(stderr, "loadRoom %d (opcode %02x, operand byte %02x, script %d)\n", room, opcode, g_slots[g_cur_slot].base[p0], g_slots[g_cur_slot].number);
	if (room != g_room) eng_start_room(room);
}
static void op_load_room_with_ego(void)                           /* 726e: 0x24 */
{
	int obj = var_or_word(0x80); int room = var_or_byte(0x40);
	int x = fetch16(); int y = fetch16();
	if (getenv("TRACE_DOOR")) fprintf(stderr, "loadRoomWithEgo obj=%d room=%d x=%d y=%d (from room %d)\n", obj, room, x, y, g_room);
	eng_load_room_with_ego(obj, room, x, y);
}
static void op_get_owner(void)                                    /* 73bc: 0x10 */
{
	get_result_pos(); int obj = var_or_word(0x80);
	u8 *p = eng_owner_ptr(obj); set_result(p ? (*p & 0xF) : 0);
}
static void op_set_owner(void)                                    /* 7480: 0x29 */
{
	int obj = var_or_word(0x80); int owner = var_or_byte(0x40);
	u8 *p = eng_owner_ptr(obj);
	if (p) {
		if (owner == 0) { /* drop from inventory */ extern void eng_remove_inventory(int); eng_remove_inventory(obj); }
		*p = (*p & 0xF0) | (owner & 0xF);
	}
	/* VAR_INVENTORY_SCRIPT */
	if (g_vars[VAR_INVENTORY_SCRIPT]) { s16 a[16] = {0}; vm_start_script(g_vars[VAR_INVENTORY_SCRIPT], 0, 0, a); }
}
static void op_print_ego(void)                                    /* 76ac: 0xd8 */
{
	extern int g_print_actor; extern void vm_print_ops(void);
	g_print_actor = g_vars[VAR_EGO]; vm_print_ops();
}
static void op_print(void)                                        /* 76b8: 0x14 */
{
	extern int g_print_actor; extern void vm_print_ops(void);
	g_print_actor = (u8)var_or_byte(0x80); vm_print_ops();
}
static void op_get_dist(void)                                     /* 76cc: 0x34 */
{
	get_result_pos(); int a = var_or_word(0x80); int b = var_or_word(0x40);
	set_result((s16)eng_dist(a, b));
}
static void op_closest_obj_actor(void)                            /* 7706: 0x66 */
{
	get_result_pos(); int obj = var_or_word(0x80);
	int best = 0xff, bestd = 0xff;
	for (int i = g_vars[VAR_ACTOR_RANGE_MAX]; i >= g_vars[VAR_ACTOR_RANGE_MIN]; i--) {
		int d = eng_dist(obj, i);
		if (d < bestd) { bestd = d; best = i; }
	}
	set_result((s16)best);
}
static void op_get_random(void)                                   /* 7760: 0x16 */
{
	get_result_pos(); int mx = var_or_byte(0x80);
	set_result((s16)(rand() % (mx + 1)));
}
static void op_get_actor_moving(void)                             /* 7784: 0x56 */
{
	get_result_pos(); int a = var_or_byte(0x80); set_result((s16)act_moving(a));
}
static void op_is_sound_running(void)                             /* 77aa: 0x7c */
{
	get_result_pos(); int s = var_or_byte(0x80);
	extern int eng_sound_running(int); set_result(s && eng_sound_running(s));
}
static void op_is_script_running(void)                            /* 77dc: 0x68 */
{
	get_result_pos(); int id = var_or_byte(0x80); int r = 0;
	for (int i = 0; i < 20; i++) if (g_slots[i].number == id && g_slots[i].where == WIO_GLOBAL && g_slots[i].status) r = 1;
	set_result(r);
}
static void op_delay_variable(void)                               /* 7826: 0x2b */
{
	Slot *s = &g_slots[g_cur_slot];
	s->delay = (u16)get_var((u16)fetch16()); s->status = SS_PAUSED;
	op_break_here();
}
static void op_delay(void)                                        /* 785c: 0x2e */
{
	Slot *s = &g_slots[g_cur_slot];
	u32 d = fetch8(); d |= fetch8() << 8; d |= (u32)fetch8() << 16;
	s->delay = (s32)d; s->status = SS_PAUSED;
	op_break_here();
}
static void op_start_music(void)  { extern void eng_music(int); eng_music(var_or_byte(0x80)); }        /* 78be: 0x02 */
static void op_stop_music(void)   { extern void eng_music(int); eng_music(0); }                        /* 78d4: 0x20 */
static void op_start_sound(void)  { eng_sound_start(var_or_byte(0x80)); }                              /* 78da: 0x1c */
static void op_stop_sound(void)   { eng_sound_stop(var_or_byte(0x80)); }                               /* 7918: 0x3c */

static void op_walk_to_obj(void)                                  /* 792e: 0x36 */
{
	int a = var_or_byte(0x80); int obj = var_or_word(0x40);
	act_walk_to_obj(a, obj);
}
static void op_put_actor_at_object(void)                          /* 796c: 0x0e */
{
	int a = var_or_byte(0x80); int obj = var_or_word(0x40);
	act_put_at_object(a, obj);
}
static void op_walk_to_actor(void)                                /* 79de: 0x0d */
{
	int a = var_or_byte(0x80); int b = var_or_byte(0x40); int dist = fetch8();
	act_walk_to_actor(a, b, dist);
}
static void op_walk_to(void)                                      /* 7aaa: 0x1e */
{
	int a = var_or_byte(0x80); int x = var_or_word(0x40); int y = var_or_word(0x20);
	act_walk_to(a, x, y);
}
static void op_put_actor(void)                                    /* 7ae2: 0x01 */
{
	int a = var_or_byte(0x80); int x = var_or_word(0x40); int y = var_or_word(0x20);
	if (g_trace) fprintf(stderr, "   putActor(%d,%d,%d) room=%d\n", a, x, y, g_actors[a].room);
	act_put(a, x, y, g_actors[a].room);
}
static void op_put_actor_in_room(void)                            /* 7b36: 0x2d */
{
	int a = var_or_byte(0x80); int room = var_or_byte(0x40);
	if (g_trace) fprintf(stderr, "   putActorInRoom(%d,%d)\n", a, room);
	if (g_actors[a].room == g_room && room != g_room && a == g_vars[VAR_TALK_ACTOR]) txt_clear();
	g_actors[a].room = room;
	if (room == 0) act_put(a, 0, 0, 0);
}
static void op_get_verb_entry(void)                               /* 7baa: 0x0b */
{
	get_result_pos(); int obj = var_or_word(0x80); int verb = var_or_word(0x40);
	set_result((s16)eng_find_verb_entry(obj, verb));
}
static void op_do_sentence(void)                                  /* 7be4: 0x19 */
{
	nsent++;
	int verb = var_or_byte(0x80);
	if (verb == 0xFE) {
		nsent = -1; vm_stop_script(g_vars[VAR_SENTENCE_SCRIPT]);
		return;
	}
	sentences[nsent].verb = verb; sentences[nsent].a = var_or_word(0x40); sentences[nsent].b = var_or_word(0x20);
	sentences[nsent].fresh = 0;
}
static void op_cutscene(void)                                     /* 7c8a: 0x40 */
{
	s16 args[16]; get_word_vararg(args);
	g_slots[g_cur_slot].cut_count++;                                   /* [0x67c0+slot], separate from the freeze count [0x6798] */
	if (++cutscene_depth > 5) vm_error("cutscene stack overflow", 0);
	cutscene_stack[cutscene_depth] = args[0];
	g_cutscene_slot = g_cur_slot;
	if (g_vars[VAR_CUTSCENE_START_SCRIPT]) { vm_start_script(g_vars[VAR_CUTSCENE_START_SCRIPT], 0, 0, args); }
	g_cutscene_slot = 0xff;
}
static void op_end_cutscene(void)                                 /* 7d0c: 0xc0 */
{
	if (g_slots[g_cur_slot].cut_count) g_slots[g_cur_slot].cut_count--;
	if (cutscene_depth > 0) cutscene_depth--;
	override_slot = 0xff;
	g_vars[VAR_OVERRIDE] = 0;
	{ s16 a[16] = {0}; a[0] = cutscene_stack[cutscene_depth + 1 > 5 ? 5 : cutscene_depth + 1];
	  if (g_vars[VAR_CUTSCENE_END_SCRIPT]) vm_start_script(g_vars[VAR_CUTSCENE_END_SCRIPT], 0, 0, a); }
}
static void op_begin_override(void)                               /* 7d5c: 0x58 */
{
	if (fetch8() != 0) {
		override_pc = pc; override_slot = g_cur_slot; override_base = code_base; override_number = g_slots[g_cur_slot].number;
		/* skip the jump instruction that follows */
		pc += 3;
	} else {
		override_slot = 0xff;
	}
}
static void op_set_camera_at(void)                                /* 7dac: 0x32 */
{
	int x = var_or_word(0x80); if (getenv("TRACE_CAM")) fprintf(stderr, "setCameraAt %d (script %d, room %d)\n", x, g_slots[g_cur_slot].number, g_room); eng_set_camera_at(x);
}
static void op_pan_camera(void)                                   /* 7dd0: 0x12 */
{
	int x = var_or_word(0x80); if (getenv("TRACE_CAM")) fprintf(stderr, "panCamera %d (script %d, room %d)\n", x, g_slots[g_cur_slot].number, g_room); eng_pan_camera(x);
}
static void op_follow_actor(void)                                 /* 7dea: 0x52 */
{
	{ int fa = var_or_byte(0x80); if (getenv("TRACE_CAM")) fprintf(stderr, "followActor %d (script %d, room %d)\n", fa, g_slots[g_cur_slot].number, g_room); eng_follow_actor(fa); }
}
static void op_freeze_scripts(void)                               /* 7ef0: 0x60 */
{
	int v = var_or_byte(0x80);
	eng_freeze_scripts(v);
}
static void op_cursor_command(void)                               /* 7f16: 0x2c */
{
	opcode = fetch8(); int sub = opcode & 0x1f; int a = 0, b = 0, c = 0;
	switch (sub) {
	case 10: a = var_or_byte(0x80); b = var_or_byte(0x40); break;      /* set cursor image */
	case 11: a = var_or_byte(0x80); b = var_or_byte(0x40); c = var_or_byte(0x20); break; /* hotspot */
	case 12: a = var_or_byte(0x80); break;                             /* init charset */
	case 13: a = var_or_byte(0x80); break;
	}
	eng_cursor_cmd(sub, a, b, c);
}
static void op_get_actor_room(void)    { get_result_pos(); int a = var_or_byte(0x80); set_result(g_actors[a].room); }     /* 8060 */
static void op_get_actor_costume(void) { get_result_pos(); int a = var_or_byte(0x80); set_result(g_actors[a].costume); }  /* 8084 */
static void op_get_actor_x(void)                                  /* 80a8: 0x43 */
{
	get_result_pos(); int a = var_or_word(0x80);
	if (a > g_vars[VAR_NUM_ACTOR]) { extern int eng_obj_xy(int, int *, int *); int x, y; set_result(eng_obj_xy(a, &x, &y) ? (s16)x : -1); }
	else set_result(g_actors[a].x);
}
static void op_get_actor_y(void)                                  /* 8106: 0x23 */
{
	get_result_pos(); int a = var_or_word(0x80);
	if (a > g_vars[VAR_NUM_ACTOR]) { extern int eng_obj_xy(int, int *, int *); int x, y; set_result(eng_obj_xy(a, &x, &y) ? (s16)y : -1); }
	else set_result(g_actors[a].y);
}
static void op_get_actor_box(void)     { get_result_pos(); int a = var_or_byte(0x80); set_result(g_actors[a].walkbox); }   /* 8164 */
static void op_get_actor_width(void)   { get_result_pos(); int a = var_or_byte(0x80); set_result(g_actors[a].width); }      /* 818a */
static void op_get_actor_facing(void)  { get_result_pos(); int a = var_or_byte(0x80); extern int new_dir_to_old(int); set_result(new_dir_to_old(g_actors[a].facing)); } /* 81ae */
static void op_get_actor_elevation(void){ get_result_pos(); int a = var_or_byte(0x80); set_result(g_actors[a].elevation); }    /* 93f6 */
static void op_face_actor(void)                                   /* 81d2: 0x09 */
{
	int a = var_or_byte(0x80); int obj = var_or_word(0x40);
	act_face_obj(a, obj);
}
static void op_animate_actor(void)                                /* 8272: 0x11 */
{
	int a = var_or_byte(0x80); int anim = var_or_byte(0x40);
	act_animate(a, anim);
}
static void op_lights(void)                                       /* 836c: 0x70 */
{
	int a = var_or_byte(0x80); int b = fetch8(); int c = fetch8();
	if (c == 0) g_vars[VAR_CURRENT_LIGHTS] = a;
	eng_screen_dirty(); (void)b;
}
static void op_set_object_name(void)                              /* 83ba: 0x54 */
{
	int obj = var_or_word(0x80);
	u8 buf[256]; int n = fetch_inline_text(buf, 250);
	eng_set_object_name(obj, buf, n);
}
static void op_nop(void) { }                                      /* 8430: 0x22 */
static void op_resource_routines(void)                            /* 8432: 0x0c */
{
	opcode = fetch8(); int sub = opcode & 0x1f; int res = 0;
	if (opcode != 0x11) res = var_or_byte(0x80);
	eng_resource_routine(sub, res);
}
static void op_system_ops(void)                                   /* 853e: 0x98 */
{
	int sub = fetch8();
	if (sub == 1) { extern void eng_restart(void); eng_restart(); }
	else if (sub == 3) g_halted = 1;
}
static void op_debug(void)         { var_or_word(0x80); }         /* 8564: 0x6b */
static void op_old_room_effect(void)                              /* 8572: 0x5c */
{
	opcode = fetch8();
	if ((opcode & 0x1f) != 3) return;
	int v = var_or_word(0x80);
	extern void eng_room_effect(int); eng_room_effect(v);
}
static void op_matrix_ops(void)                                   /* 85b4: 0x30 */
{
	opcode = fetch8(); int sub = opcode & 0x1f;
	if (sub == 1) { int a = var_or_byte(0x80), b = var_or_byte(0x40); eng_matrix_ops(1, a, b); }
	else if (sub == 2) { int a = var_or_byte(0x80), b = var_or_byte(0x40); eng_matrix_ops(2, a, b); }
	else if (sub == 3) { int a = var_or_byte(0x80), b = var_or_byte(0x40); eng_matrix_ops(3, a, b); }
	else if (sub == 4) eng_matrix_ops(4, 0, 0);
}
static void op_verb_ops(void)                                     /* 8646: 0x7a */
{
	int verb = var_or_byte(0x80);
	int slot_v = verb;
	for (;;) {
		opcode = fetch8();
		if (opcode == 0xFF) { eng_verb_op(slot_v, 0xFF, 0, 0, NULL, 0, 0); return; }
		int sub = opcode & 0x1f, a = 0, b = 0;
		switch (sub) {
		case 1: a = var_or_word(0x80); break;                      /* image (object) */
		case 2: { /* name: inline text */
			u8 buf[256]; int n = fetch_inline_text(buf, 250);
			eng_verb_op(slot_v, 2, 0, 0, buf, n, 0); continue; }
		case 3: a = var_or_byte(0x80); break;                      /* color */
		case 4: a = var_or_byte(0x80); break;                      /* hicolor */
		case 5: a = var_or_word(0x80); b = var_or_word(0x40); break; /* at x,y */
		case 0x10: a = var_or_byte(0x80); break;                   /* dimcolor */
		case 0x12: a = var_or_byte(0x80); break;                   /* key */
		case 0x15: a = fetch8(); b = fetch8(); fetch8(); fetch8(); break; /* backcolor etc. (4 raw bytes) */
		case 0x14: a = var_or_word(0x80); break;                   /* set name from string */
		case 9: eng_verb_op(slot_v, 9, 0, 0, NULL, 0, 0); continue;
		default: break;
		}
		eng_verb_op(slot_v, sub, a, b, NULL, 0, 0);
	}
}
static void op_get_string_width(void)                             /* 88ce: 0x67 */
{
	get_result_pos(); int s = var_or_byte(0x80);
	set_result((s16)eng_get_string_len(s));
}
static void op_string_ops(void)                                   /* 89be: 0x27 */
{
	opcode = fetch8(); int sub = opcode & 0x1f;
	if (sub == 1) {
		int s = var_or_byte(0x80);
		u8 buf[256]; int n = fetch_inline_text(buf, 250);
		eng_string_op(1, s, 0, 0, buf); (void)n;
	} else if (sub == 2) { int a = var_or_byte(0x80), b = var_or_byte(0x40); eng_string_op(2, a, b, 0, NULL); }
	else if (sub == 3) { int a = var_or_byte(0x80), b = var_or_byte(0x40), c = var_or_byte(0x20); eng_string_op(3, a, b, c, NULL); }
	else if (sub == 4) { get_result_pos(); int a = var_or_byte(0x80), b = var_or_byte(0x40); extern int eng_string_get_char(int, int); set_result((s16)eng_string_get_char(a, b)); }
	else if (sub == 5) { int a = var_or_byte(0x80), b = var_or_byte(0x40); eng_string_op(5, a, b, 0, NULL); }
}
static void op_save_restore_verbs(void)                           /* 8b90: 0xab */
{
	opcode = fetch8(); int sub = opcode & 0x1f;
	int a = var_or_byte(0x80), b = var_or_byte(0x40), c = var_or_byte(0x20);
	extern void eng_save_restore_verbs(int, int, int, int); eng_save_restore_verbs(sub, a, b, c);
}
static void op_room_ops(void)                                     /* 8de6: 0x33 */
{
	opcode = fetch8(); int sub = opcode & 0x1f; int a = 0, b = 0, c = 0, d = 0, e = 0;
	switch (sub) {
	case 1: a = var_or_word(0x80); b = var_or_word(0x40); break;
	case 2: a = var_or_word(0x80); b = var_or_word(0x40); break;
	case 3: a = var_or_word(0x80); b = var_or_word(0x40); break;
	case 4: a = var_or_word(0x80); b = var_or_word(0x40); break;
	case 7: a = var_or_word(0x80); b = var_or_word(0x40); opcode = fetch8(); c = var_or_word(0x80); d = var_or_word(0x40); opcode = fetch8(); e = var_or_byte(0x40); break;
	default: break;
	}
	eng_room_ops(sub, a, b, c, d, e);
}
static void op_actor_ops(void)                                    /* 8f9e: 0x13 */
{
	extern void vm_actor_ops(int a, int sub, const s16 *p, const u8 *text, int tlen);
	int a = var_or_byte(0x80);
	for (;;) {
		opcode = fetch8(); if (opcode == 0xFF) return;
		int sub = opcode & 0x1f; s16 p[16] = {0}; u8 tb[64]; int tl = 0;
		switch (sub) {
		case 1: p[0] = var_or_byte(0x80); break;
		case 2: case 3: p[0] = var_or_byte(0x80); break;
		case 4: p[0] = var_or_byte(0x80); p[1] = var_or_byte(0x40); break;               /* walk speed x,y: BYTES (8f9e), stored as words */
		case 5: case 6: case 8: case 14: case 16: p[0] = var_or_byte(0x80); break;
		case 7: p[0] = var_or_byte(0x80); p[1] = var_or_byte(0x40); break;
		case 9: p[0] = var_or_byte(0x80); p[1] = var_or_byte(0x40); p[2] = var_or_byte(0x20); break;
		case 11: p[0] = var_or_word(0x80); break;
		case 13: p[0] = var_or_byte(0x80); p[1] = var_or_byte(0x40); break;
		case 15: tl = fetch_inline_text(tb, 62); break;   /* actor name: same escape-aware reader as every other inline text */
		case 17: { s16 v[16]; for (int i = 0; i < 16; i++) v[i] = 0xff; int n = 0; s16 tmp[16]; (void)n; get_word_vararg(tmp); for (int i = 0; i < 16; i++) p[i] = tmp[i]; } break;
		case 18: case 19: p[0] = var_or_byte(0x80); break;                               /* width / scale: BYTE operands (8f9e) */
		default: break;
		}
		vm_actor_ops(a, sub, p, tb, tl);
	}
}
static void op_actor_from_pos(void)                               /* 921a: 0x15 */
{
	get_result_pos(); int x = var_or_word(0x80); int y = var_or_word(0x40);
	set_result((s16)eng_actor_from_pos(x, y));
}
static void op_find_object(void)                                  /* 9254: 0x35 */
{
	get_result_pos(); int x = var_or_word(0x80); int y = var_or_word(0x40);
	set_result((s16)eng_find_object(x, y));
}
static void op_set_class(void)                                    /* 928e: 0x5d */
{
	int obj = var_or_word(0x80);
	for (;;) {
		opcode = fetch8(); if (opcode == 0xFF) break;
		int cls = var_or_word(0x80);
		if (cls == 0) { for (int c = 1; c <= 24; c++) eng_set_class(obj, c, 0); }
		else eng_set_class(obj, cls & 0x7f, (cls & 0x80) != 0);
	}
}
static void op_if_class_of(void)                                  /* 932e: 0x1d */
{
	int obj = var_or_word(0x80); int bad = 0, any = 0;
	for (;;) {
		opcode = fetch8(); if (opcode == 0xFF) break;
		int cls = var_or_word(0x80);
		int has = eng_get_class(obj, cls & 0x7f);
		if (((cls & 0x80) && has) || (!(cls & 0x80) && !has)) any = 1;
	}
	/* handler 932e: executes the next instruction if ANY listed class test matched, else jumps */
	(void)bad; COND_JUMP_IF(!any);
}
static void op_pseudo_room(void)                                  /* 93c8: 0xcc */
{
	extern u8 g_pseudo_room[128];
	int val = fetch8();
	for (;;) { int c = fetch8(); if (!c) break; if (c & 0x80) g_pseudo_room[c & 0x7f] = val; }
}
static void op_find_inventory(void)                               /* 9418: 0x3d */
{
	get_result_pos(); int owner = var_or_byte(0x80); int idx = var_or_byte(0x40);
	set_result((s16)eng_find_inventory(owner, idx));
}
static void op_inventory_count(void)                              /* 9488: 0x31 */
{
	get_result_pos(); int owner = var_or_byte(0x80); set_result((s16)eng_inv_count(owner));
}
static void op_draw_box(void)                                     /* 94dc: 0x3f */
{
	int x1 = var_or_word(0x80), y1 = var_or_word(0x40);
	opcode = fetch8();
	int x2 = var_or_word(0x80), y2 = var_or_word(0x40), color = var_or_byte(0x20);
	eng_draw_box(x1, y1, x2, y2, color);
}
static s16 expr_stack[16]; static int expr_sp;
static void op_expression(void)                                   /* 9578: 0xac */
{
	int sp = 0; s16 stack[16];
	get_result_pos();
	u16 dst = result_pos;
	for (;;) {
		opcode = fetch8(); if (opcode == 0xFF) break;
		switch (opcode & 0x1f) {
		case 1: stack[sp++] = var_or_word(0x80); break;
		case 2: { s16 a = stack[--sp]; s16 b = stack[--sp]; stack[sp++] = b + a; } break;
		case 3: { s16 a = stack[--sp]; s16 b = stack[--sp]; stack[sp++] = b - a; } break;
		case 4: { s16 a = stack[--sp]; s16 b = stack[--sp]; stack[sp++] = b * a; } break;
		case 5: { s16 a = stack[--sp]; s16 b = stack[--sp]; stack[sp++] = a ? b / a : 0; } break;
		case 6: { opcode = fetch8(); u16 saved = result_pos; exec_opcode(); result_pos = saved; stack[sp++] = g_vars[0]; } break;
		default: break;
		}
	}
	result_pos = dst; set_result(stack[0]);
}
static void op_unknown(void) { vm_error("unknown opcode", opcode); }          /* 9646/965c */

static void op_wait(void)                                         /* 6982: 0xae */
{
	int sub; u32 save_pc;
	opcode = fetch8(); sub = opcode & 0x1f;
	if (sub == 1) {
		save_pc = pc; int a = var_or_byte(0x80);
		if (!act_moving(a)) return;
		pc = save_pc - 2;
	} else if (sub == 2) {
		if (g_vars[VAR_HAVE_MSG] == 0) return;
		pc -= 2;
	} else if (sub == 3) {
		int d = g_cam_dest, c = g_cam_cur; int ad = d < 0 ? -d : d;
		if ((ad >> 3) == (c >> 3)) return;
		pc -= 2;
	} else if (sub == 4) {
		extern int eng_sentence_idle(void);
		if (eng_sentence_idle()) return;
		pc -= 2;
	} else return;
	op_break_here();
}

static void init_optab(void)
{
	for (int i = 0; i < 256; i++) optab[i] = op_unknown;
#define SET(h, ...) do { static const int l[] = { __VA_ARGS__, -1 }; for (int k = 0; l[k] >= 0; k++) { optab[l[k]] = h; opname[l[k]] = #h; } } while (0)
	SET(op_jump, 0x18); SET(op_stop_object_code, 0x00, 0xa0); SET(op_wait, 0xae); SET(op_break_here, 0x80);
	SET(op_set_var_range, 0x26, 0xa6); SET(op_move, 0x1a, 0x9a); SET(op_increment, 0x46); SET(op_decrement, 0xc6);
	SET(op_divide, 0x5b, 0xdb); SET(op_multiply, 0x1b, 0x9b); SET(op_add, 0x5a, 0xda); SET(op_and, 0x17, 0x97);
	SET(op_or, 0x57, 0xd7); SET(op_subtract, 0x3a, 0xba);
	SET(op_equal_zero, 0x28); SET(op_not_equal_zero, 0xa8); SET(op_ge, 0x38, 0xb8); SET(op_le, 0x04, 0x84);
	SET(op_gt, 0x44, 0xc4); SET(op_lt, 0x78, 0xf8); SET(op_ne, 0x08, 0x88); SET(op_eq, 0x48, 0xc8);
	SET(op_stop_object_script, 0x6e, 0xee); SET(op_stop_script, 0x62, 0xe2); SET(op_start_object, 0x37, 0x77, 0xb7, 0xf7);
	SET(op_start_script, 0x0a, 0x2a, 0x4a, 0x6a, 0x8a, 0xaa, 0xca, 0xea); SET(op_chain_script, 0x42, 0xc2);
	SET(op_if_state, 0x0f, 0x4f, 0x8f, 0xcf); SET(op_actor_in_box, 0x1f, 0x5f, 0x9f, 0xdf);
	SET(op_if_not_state, 0x2f, 0x6f, 0xaf, 0xef); SET(op_set_state, 0x07, 0x47, 0x87, 0xc7);
	SET(op_draw_object, 0x05, 0x25, 0x45, 0x65, 0x85, 0xa5, 0xc5, 0xe5); SET(op_pickup_object, 0x50, 0xd0);
	SET(op_load_room, 0x72, 0xf2); SET(op_load_room_with_ego, 0x24, 0x64, 0xa4, 0xe4);
	SET(op_get_owner, 0x10, 0x90); SET(op_set_owner, 0x29, 0x69, 0xa9, 0xe9);
	SET(op_print_ego, 0xd8); SET(op_print, 0x14, 0x94); SET(op_get_dist, 0x34, 0x74, 0xb4, 0xf4);
	SET(op_closest_obj_actor, 0x66, 0xe6); SET(op_get_random, 0x16, 0x96); SET(op_get_actor_moving, 0x56, 0xd6);
	SET(op_is_sound_running, 0x7c, 0xfc); SET(op_is_script_running, 0x68, 0xe8);
	SET(op_delay_variable, 0x2b); SET(op_delay, 0x2e);
	SET(op_start_music, 0x02, 0x82); SET(op_stop_music, 0x20); SET(op_start_sound, 0x1c, 0x9c); SET(op_stop_sound, 0x3c, 0xbc);
	SET(op_walk_to_obj, 0x36, 0x76, 0xb6, 0xf6); SET(op_put_actor_at_object, 0x0e, 0x4e, 0x8e, 0xce);
	SET(op_walk_to_actor, 0x0d, 0x4d, 0x8d, 0xcd); SET(op_walk_to, 0x1e, 0x3e, 0x5e, 0x7e, 0x9e, 0xbe, 0xde, 0xfe);
	SET(op_put_actor, 0x01, 0x21, 0x41, 0x61, 0x81, 0xa1, 0xc1, 0xe1); SET(op_put_actor_in_room, 0x2d, 0x6d, 0xad, 0xed);
	SET(op_get_verb_entry, 0x0b, 0x4b, 0x8b, 0xcb); SET(op_do_sentence, 0x19, 0x39, 0x59, 0x79, 0x99, 0xb9, 0xd9, 0xf9);
	SET(op_cutscene, 0x40); SET(op_end_cutscene, 0xc0); SET(op_begin_override, 0x58);
	SET(op_set_camera_at, 0x32, 0xb2); SET(op_pan_camera, 0x12, 0x92); SET(op_follow_actor, 0x52, 0xd2);
	SET(op_freeze_scripts, 0x60, 0xe0); SET(op_cursor_command, 0x2c);
	SET(op_get_actor_room, 0x03, 0x83); SET(op_get_actor_costume, 0x71, 0xf1); SET(op_get_actor_x, 0x43, 0xc3);
	SET(op_get_actor_y, 0x23, 0xa3); SET(op_get_actor_box, 0x7b, 0xfb); SET(op_get_actor_width, 0x6c, 0xec);
	SET(op_get_actor_facing, 0x63, 0xe3); SET(op_get_actor_elevation, 0x06, 0x86);
	SET(op_face_actor, 0x09, 0x49, 0x89, 0xc9); SET(op_animate_actor, 0x11, 0x51, 0x91, 0xd1);
	SET(op_lights, 0x70, 0xf0); SET(op_set_object_name, 0x54, 0xd4); SET(op_nop, 0x22, 0xa2);
	SET(op_resource_routines, 0x0c, 0x8c); SET(op_system_ops, 0x98); SET(op_debug, 0x6b, 0xeb);
	SET(op_old_room_effect, 0x5c, 0xdc); SET(op_matrix_ops, 0x30, 0xb0); SET(op_verb_ops, 0x7a, 0xfa);
	SET(op_get_string_width, 0x67, 0xe7); SET(op_string_ops, 0x27); SET(op_save_restore_verbs, 0xab);
	SET(op_room_ops, 0x33, 0x73, 0xb3, 0xf3); SET(op_actor_ops, 0x13, 0x53, 0x93, 0xd3);
	SET(op_actor_from_pos, 0x15, 0x55, 0x95, 0xd5); SET(op_find_object, 0x35, 0x75, 0xb5, 0xf5);
	SET(op_set_class, 0x5d, 0xdd); SET(op_if_class_of, 0x1d, 0x9d); SET(op_pseudo_room, 0xcc);
	SET(op_find_inventory, 0x3d, 0x7d, 0xbd, 0xfd); SET(op_inventory_count, 0x31, 0xb1);
	SET(op_draw_box, 0x3f, 0x7f, 0xbf, 0xff); SET(op_expression, 0xac);
	SET(op_unknown, 0xa7, 0x3b, 0x4c, 0xbb);
}

/* run the current slot until it breaks / stops (FUN_1000_5cde main loop) */
static void run_slot_until_break(int slot)
{
	int saved_cur = g_cur_slot; const u8 *saved_base = code_base; u32 saved_pc = pc;
	Slot *s = &g_slots[slot];
	g_cur_slot = slot; code_base = s->base; pc = s->pc;
	nest++;
	while (g_cur_slot != 0xff) {
		if (g_halted) break;
		u32 at = pc;
		opcode = fetch8();
		if (g_trace) fprintf(stderr, "[%d/%d@%u] %02x %s\n", g_cur_slot < NUM_SLOTS ? g_slots[g_cur_slot].number : -1, g_cur_slot, at, opcode, opname[opcode] ? opname[opcode] : "?");
		exec_opcode();
	}
	nest--;
	/* restore the outer slot (FUN_1000_610e) if still alive */
	if (saved_cur != 0xff && g_slots[saved_cur].status != SS_DEAD && g_slots[saved_cur].base == saved_base) {
		g_cur_slot = saved_cur; code_base = saved_base; pc = saved_pc;
	} else g_cur_slot = 0xff;
}

void vm_init(void)
{
	init_optab();
	memset(g_vars, 0, sizeof g_vars); memset(g_bitvars, 0, sizeof g_bitvars); memset(g_slots, 0, sizeof g_slots);
	g_cur_slot = 0xff; nsent = -1;
}

void vm_run_frame(void)                                           /* FUN_1000_5c50 */
{
	for (int i = 0; i < NUM_SLOTS; i++) g_slots[i].ran_this_frame = 0;
	g_cur_slot = 0xff;
	for (int i = 0; i < NUM_SLOTS; i++) {
		Slot *s = &g_slots[i];
		if (s->status == SS_RUNNING && !s->ran_this_frame) { s->ran_this_frame = 1; run_slot_until_break(i); }
	}
}

void vm_tick_delays(int ticks)                                    /* FUN_1000_682a */
{
	for (int i = 0; i < NUM_SLOTS; i++) {
		Slot *s = &g_slots[i];
		if (s->status == SS_PAUSED) { s->delay -= ticks; if (s->delay < 0) { s->status = SS_RUNNING; s->delay = 0; } }
	}
}

/* sentence queue service (what VAR_SENTENCE_SCRIPT drives: see FUN_1000_a14e in original) */
int vm_sentence_pending(void) { return nsent >= 0; }
int vm_pop_sentence(int *verb, int *a, int *b)
{
	if (nsent < 0) return 0;
	*verb = sentences[0].verb; *a = sentences[0].a; *b = sentences[0].b;
	nsent--;
	for (int i = 0; i <= nsent; i++) sentences[i] = sentences[i + 1];
	return 1;
}

void vm_boot(void)
{
	s16 args[16] = {0};
	vm_start_script(1, 0, 0, args);
}

/* helpers used by other modules to resume override etc. */
/* The override belongs to the script that set it. If that script has ended (or was stopped) its slot may already run
 * something else: jumping that slot to the old pc sent script 149 off to a garbage room (black screen). Forget it then. */
static int override_valid(void)
{
	if (override_slot == 0xff) return 0;
	Slot *s = &g_slots[override_slot];
	if (s->status == SS_DEAD || s->base != override_base || s->number != override_number) { override_slot = 0xff; return 0; }
	return 1;
}
int vm_override_active(void) { return override_valid(); }
void vm_do_override(void)
{
	if (!override_valid()) return;
	Slot *s = &g_slots[override_slot];
	s->pc = override_pc; s->status = SS_RUNNING; s->delay = 0;
	/* the opcode right before pc is the jump that skips the cutscene; run from there */
	/* ScummVM semantics: continue just after the beginOverride, i.e. execute the jump */
	s->pc = override_pc; (void)override_base;
	override_slot = 0xff;
}

/* ---- print (handler 76b8 / 74c8) ---- */
typedef struct { s16 x, y; s16 color, center, overhead, clip, charset; } PrintState;
static PrintState ps_saved[4] = { {0,0,15,0,0,0,0},{0,0,15,0,0,0,0},{0,0,15,0,0,0,0},{0,0,15,0,0,0,0} };
int g_print_actor;
static int read_text(u8 *buf, int max) { return fetch_inline_text(buf, max); }
void vm_print_ops(void)
{
	int slot = g_print_actor == 252 ? 3 : g_print_actor == 253 ? 2 : g_print_actor == 254 ? 1 : 0;
	PrintState w = ps_saved[slot];
	for (;;) {
		opcode = fetch8();
		if (opcode == 0xFF) break;
		switch (opcode & 0x1f) {
		case 0: w.x = var_or_word(0x80); w.y = var_or_word(0x40); w.overhead = 0; break;
		case 1: w.color = var_or_byte(0x80); break;
		case 2: w.clip = var_or_word(0x80); break;
		case 4: w.center = 1; w.overhead = 0; break;
		case 6: w.center = 0; w.overhead = 0; break;
		case 7: w.overhead = 1; break;
		case 0xf: {
			u8 buf[512]; int n = read_text(buf, sizeof buf);
			txt_print(slot, g_print_actor, buf, n, w.x, w.y, w.color, w.center, w.overhead, 0, w.clip, 0);
			return; }
		default: goto done;
		}
	}
done:
	ps_saved[slot] = w;
}

/* ---- save/load (state.c) ---- */
void vm_state(StateIO *io)
{
	SIO(io, g_vars); SIO(io, g_bitvars);
	for (int i = 0; i < NUM_SLOTS; i++) {
		Slot tmp = g_slots[i]; const u8 *base = g_slots[i].base; tmp.base = NULL;
		SIO(io, tmp); sio_ptr(io, &base); tmp.base = base;
		if (!io->saving) g_slots[i] = tmp;
	}
	SIO(io, g_cutscene_slot); SIO(io, cutscene_depth); SIO(io, cutscene_stack); SIO(io, override_pc); SIO(io, override_slot);
	SIO(io, sentences); SIO(io, nsent);
	if (!io->saving) { g_cur_slot = 0xff; code_base = NULL; override_base = NULL; }
}
