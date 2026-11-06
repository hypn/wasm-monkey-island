/* MIDEMO engine: clean-room reimplementation of the SCUMM v4 interpreter found in
 * MIDEMO.EXE (The Secret of Monkey Island contest demo). Behaviour was derived from
 * the decompiled EXE (see ../RE_NOTES.md). Portable C99, no OS dependencies. */
#ifndef SCUMM_H
#define SCUMM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;

#define SCREEN_W 320
#define SCREEN_H 200
#define VIEW_H   144            /* room view height; verb area below */
#define NUM_VARS 800
#define NUM_BITVARS 4096
#define NUM_SLOTS 20
#define NUM_GLOBAL_SCRIPTS 256
#define MAX_DISKS 4
#define NUM_OBJECTS 1000
#define NUM_INVENTORY 80
#define NUM_ACTORS 13
#define NUM_VERBS 100
#define NUM_STRINGS 100
#define NUM_LOCALVARS 17
#define MAX_ROOM_OBJECTS 100
#define MAX_BOXES 64

/* well-known variable numbers (verified against DS addresses in the EXE) */
enum {
	VAR_EGO = 1, VAR_CAMERA_POS_X = 2, VAR_HAVE_MSG = 3, VAR_ROOM = 4, VAR_OVERRIDE = 5,
	VAR_MACHINE_SPEED = 6, VAR_ME = 7, VAR_NUM_ACTOR = 8, VAR_CURRENT_LIGHTS = 9,
	VAR_CURRENTDRIVE = 10, VAR_TMR_1 = 11, VAR_TMR_2 = 12, VAR_TMR_3 = 13, VAR_MUSIC_TIMER = 14,
	VAR_ACTOR_RANGE_MIN = 15, VAR_ACTOR_RANGE_MAX = 16, VAR_CAMERA_MIN_X = 17,
	VAR_CAMERA_MAX_X = 18, VAR_TIMER_NEXT = 19, VAR_VIRT_MOUSE_X = 20, VAR_VIRT_MOUSE_Y = 21,
	VAR_ROOM_RESOURCE = 22, VAR_LAST_SOUND = 23, VAR_CUTSCENEEXIT_KEY = 24, VAR_TALK_ACTOR = 25,
	VAR_CAMERA_FAST_X = 26, VAR_SCROLL_SCRIPT = 27, VAR_ENTRY_SCRIPT = 28, VAR_ENTRY_SCRIPT2 = 29,
	VAR_EXIT_SCRIPT = 30, VAR_EXIT_SCRIPT2 = 31, VAR_VERB_SCRIPT = 32, VAR_SENTENCE_SCRIPT = 33,
	VAR_INVENTORY_SCRIPT = 34, VAR_CUTSCENE_START_SCRIPT = 35, VAR_CUTSCENE_END_SCRIPT = 36,
	VAR_CHARINC = 37, VAR_WALKTO_OBJ = 38, VAR_DEBUGMODE = 39, VAR_HEAPSPACE = 40,
	VAR_RESTART_KEY = 42, VAR_PAUSE_KEY = 43, VAR_MOUSE_X = 44, VAR_MOUSE_Y = 45, VAR_TIMER = 46,
	VAR_TIMER_TOTAL = 47, VAR_SOUNDCARD = 48, VAR_VIDEOMODE = 49, VAR_MAINMENU_KEY = 50,
	VAR_FIXEDDISK = 51, VAR_CURSORSTATE = 52, VAR_USERPUT = 53, VAR_SOUNDRESULT = 56,
	VAR_TALKSTOP_KEY = 57, VAR_FADE_DELAY = 59, VAR_NOSUBTITLES = 60, VAR_SOUNDPARAM = 64,
	VAR_SOUNDPARAM2 = 65, VAR_SOUNDPARAM3 = 66, VAR_INPUTMODE = 67, VAR_MEMORY_PERFORMANCE = 68,
	VAR_VIDEO_PERFORMANCE = 69, VAR_ROOM_FLAG = 70, VAR_GAME_LOADED = 71, VAR_NEW_ROOM = 72
};

/* ---- resources ---- */
typedef struct { u8 room; u32 off; } DirEnt;
extern u8 *g_disks[MAX_DISKS]; extern u32 g_disk_sizes[MAX_DISKS]; extern u8 g_lf_disk[256]; extern int g_num_disks;
int res_add_disk(int n, const u8 *lec, u32 size); int res_set_index(const u8 *idx, u32 size);
extern DirEnt g_dir_room[256], g_dir_script[NUM_GLOBAL_SCRIPTS], g_dir_sound[NUM_GLOBAL_SCRIPTS], g_dir_cost[NUM_GLOBAL_SCRIPTS];
extern int g_n_script, g_n_sound, g_n_cost;
extern u8 g_obj_owner[NUM_OBJECTS];      /* low nibble owner, high nibble state */
extern u32 g_obj_class[NUM_OBJECTS];
extern u32 g_lf_off[256];                /* room id -> LF block offset in disk, 0 if absent */
extern u8 *g_charset[10]; extern u32 g_charset_size[10];

static inline u16 rd16(const u8 *p){ return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p){ return p[0] | (p[1]<<8) | (p[2]<<16) | ((u32)p[3]<<24); }

int  res_init(const u8 *lec, u32 lec_size, const u8 *index_lfl, u32 index_size);
int  res_add_charset(int id, const u8 *data, u32 size);
const u8 *res_room_block(int room, const char *tag, int nth, u32 *size); /* block inside RO */
const u8 *res_script(int id, u32 *size);     /* -> code after block header */
const u8 *res_costume(int id, u32 *size);
const u8 *res_sound(int id, u32 *size);

/* ---- graphics ---- */
extern u8 g_screen[SCREEN_W * SCREEN_H];     /* palette indices 0..15 */
extern u8 *g_bg;                             /* composed room background (room + objects) */
extern const u8 g_ega_palette[16][3];
void gfx_decode_strip(u8 *dst, int pitch, const u8 *src, int h);
extern int g_vga; extern u8 g_pal_rgb[256][3];     /* retail VGA rooms: 256-colour palette from the room's PA block */
void gfx_draw_strips(u8 *dst, int pitch, int dst_h, const u8 *img, int xstrip, int nstrips, int y, int h);

#endif
