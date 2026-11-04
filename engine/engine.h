#ifndef ENGINE_H
#define ENGINE_H
#include "scumm.h"

/* ---------------- script slots ---------------- */
enum { SS_DEAD = 0, SS_PAUSED = 1, SS_RUNNING = 2 };
enum { WIO_INVENTORY = 0, WIO_ROOM = 1, WIO_GLOBAL = 2, WIO_NOTFOUND = 0xff };
typedef struct {
	u8 status;          /* SS_* */
	u8 where;           /* WIO_INVENTORY / WIO_ROOM (object code) / WIO_GLOBAL (global or local script) */
	u16 number;
	const u8 *base;     /* code base; pc is an offset from here */
	u32 pc;             /* saved pc */
	s32 delay;
	u8 freeze_resistant, recursive, freeze_count, cutscene_override, ran_this_frame;
	s16 vars[NUM_LOCALVARS];
} Slot;

/* ---------------- objects ---------------- */
typedef struct {
	u16 id;
	s16 x, y;           /* pixel position of image */
	u16 w, h;
	u8 parent, parentstate;
	s16 walk_x, walk_y;
	u8 actordir;
	const u8 *oc;       /* OC block (code / verbs / name) */
	u32 oc_size;
	const u8 *oi[16];   /* image per state (OI blocks with this id, in order) */
	int n_oi;
	int drawn;          /* currently composed into bg */
} RoomObject;

/* ---------------- actors ---------------- */
typedef struct {
	u8 used;
	u8 room, costume, facing, new_facing;
	s16 x, y, elevation;
	s16 walk_speed_x, walk_speed_y;
	u16 width; u8 scale;
	u8 init_frame, walk_frame, stand_frame, talk_start, talk_stop;
	u8 talk_color; char name[24];
	u8 moving, visible, ignore_boxes, force_clip, layer;
	u8 palette[16]; u8 has_palette;
	u8 walkbox; u8 mask;
	s16 tx, ty;         /* walk target */
	s16 walk_x, walk_y; /* sub-pixel accumulators */
	u8 walk_stage;
	s16 next_x, next_y; u8 next_box;
	u8 anim_speed, anim_counter;
	/* costume playback */
	u16 curpos[16], start[16], end[16], frame[16]; u16 stopped;
	u8 cost_loaded;
	u8 needs_redraw;
	u8 cost_anim; /* current anim frame index */
	s8 dest_dir;  /* facing to take on arrival (-1 = keep) */
} Actor;

typedef struct { s16 ulx, uly, urx, ury, lrx, lry, llx, lly; u8 mask, flags; u16 scale; } Box;

typedef struct { u8 on; s16 x, y; u8 color, hicolor, dimcolor, backcolor, center, key; u8 *name; int name_len; u8 curmode; u16 id; s16 saveid;} Verb;

typedef struct {
	char text[256]; s16 x, y; u8 color, center, overhead, clipped; s16 right;
} TextCtx;

/* ---------------- globals ---------------- */
extern s16 g_vars[NUM_VARS];
extern u8 g_bitvars[NUM_BITVARS / 8];
extern Slot g_slots[NUM_SLOTS];
extern int g_cur_slot;
extern Actor g_actors[16];
extern RoomObject g_objs[MAX_ROOM_OBJECTS]; extern int g_nobjs;
extern int g_room, g_room_w, g_room_h;
extern u8 *g_roomimg;
extern Box g_boxes[MAX_BOXES]; extern int g_nboxes; extern const u8 *g_box_matrix;
extern s16 g_cam_cur, g_cam_dest; extern s16 g_cam_follow;
extern u8 g_inv_obj_data[NUM_INVENTORY][64];
extern u16 g_inventory[NUM_INVENTORY];

/* vm.c */
void vm_init(void);
void vm_boot(void);
void vm_run_frame(void);              /* one scheduler pass over all slots */
int  vm_start_script(int id, int freeze_res, int recursive, const s16 *args);
void vm_run_object_script(int obj, int verb, int freeze_res, int recursive, const s16 *args);
void vm_stop_script(int id);
void vm_stop_object_script(int id);
extern int g_trace;
extern int g_halted;

/* engine services implemented in engine.c */
void eng_start_room(int room);
int  eng_obj_xy(int obj, int *x, int *y);
int  eng_obj_where(int obj);
int  eng_obj_index(int obj);
const u8 *eng_obj_code(int obj, int where);
int  eng_find_verb_entry(int obj, int verb);   /* offset in OC block or 0 */
const u8 *eng_obj_name(int obj);
int  eng_get_state(int obj);
void eng_set_owner_state(int obj, int val);
u8  *eng_owner_ptr(int obj);
void eng_redraw_object(int obj);
void eng_draw_object(int obj, int x, int y);
void eng_pickup_object(int obj);
void eng_set_object_name(int obj, const u8 *text, int len);
void eng_run_entry(void); void eng_run_exit(void);
int  eng_find_object(int x, int y);
int  eng_get_class(int obj, int cls); void eng_set_class(int obj, int cls, int on);
int  eng_inv_count(int owner); int eng_find_inventory(int owner, int idx);
void eng_set_camera_at(int x); void eng_pan_camera(int x); void eng_follow_actor(int a);
void eng_screen_dirty(void);
void eng_cursor_cmd(int sub, int a, int b, int c);
void eng_room_ops(int sub, int a, int b, int c, int d, int e);
void eng_resource_routine(int sub, int id);
void eng_sound_start(int id); void eng_sound_stop(int id);
void eng_music(int id);
void eng_set_screen_clear(int a, int b);
int  eng_dist(int a, int b);
int  eng_actor_from_pos(int x, int y);
int  eng_closest_actor(int x, int y, int lo);
void eng_draw_box(int x1, int y1, int x2, int y2, int color);
void eng_do_sentence(int verb, int obj_a, int obj_b);
void eng_matrix_ops(int sub, int a, int b);
int  eng_string_width(const u8 *s);
void eng_load_room_with_ego(int obj, int room, int x, int y);
void eng_override(int on); void eng_cutscene_begin(const s16 *args); void eng_cutscene_end(void);
void eng_begin_override(void);
void eng_freeze_scripts(int on); void eng_set_object_class(int obj, int cls, int on);
void eng_verb_op(int verb, int sub, int a, int b, const u8 *text, int len, int color);
void eng_string_op(int sub, int a, int b, int c, const u8 *text);
int  eng_get_string_len(int s);
void eng_resource_misc(void);

/* actors */
void act_init(int a);
void act_put(int a, int x, int y, int room);
void act_set_costume(int a, int cost);
void act_walk_to(int a, int x, int y);
void act_walk_to_obj(int a, int obj);
void act_walk_to_actor(int a, int b, int dist);
void act_face_obj(int a, int obj);
void act_start_anim(int a, int anim);
void act_animate(int a, int anim);
int  act_moving(int a);
void act_stop_walk(int a);
void act_update_all(void);
void act_put_at_object(int a, int obj);
int  act_in_box(int a, int box);
int  act_box_of(int x, int y);
void act_talk_begin(int a, const u8 *text, int len);
const u8 *act_name(int a);

/* text */
void txt_print(int slot, int actor, const u8 *text, int len, int x, int y, int color, int center, int overhead, int left, int clip, int have_pos);
void txt_update(void);
void txt_clear(void);

/* render */
void render_frame(void);
void eng_tick_timers(int ticks);
int  eng_get_box_scale(int box);
int  eng_box_in(int box, int x, int y);
extern int g_ticks_per_frame;

/* input (platform) */
extern int g_mouse_x, g_mouse_y, g_mouse_buttons, g_key_pressed;
void eng_click(int x, int y, int button);
void eng_key(int key);

#endif
