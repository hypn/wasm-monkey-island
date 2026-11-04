/* Native test driver: runs the engine headless and writes PPM/PNG frames. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern u32 g_rgb[]; extern void eng_frame(void); extern void ui_init(void);
extern int g_charset_id;
static u8 *rdf(const char *p, u32 *n) { FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(1); } fseek(f, 0, SEEK_END); *n = (u32)ftell(f); rewind(f); u8 *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b; }
static void dump_ppm(const char *path) { FILE *f = fopen(path, "wb"); fprintf(f, "P6\n320 200\n255\n"); for (int i = 0; i < 320 * 200; i++) { u32 c = g_rgb[i]; fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f); } fclose(f); }
int main(int argc, char **argv)
{
	const char *D = argc > 1 ? argv[1] : "/home/user/monkeyislanddemo/mi_demo2/";
	int frames = argc > 2 ? atoi(argv[2]) : 100; const char *out = argc > 3 ? argv[3] : "/tmp/frame";
	int every = argc > 4 ? atoi(argv[4]) : 0;
	char p[512]; u32 a, b, c;
	snprintf(p, sizeof p, "%sDISK01.LEC", D); u8 *lec = rdf(p, &a);
	snprintf(p, sizeof p, "%s000.LFL", D); u8 *idx = rdf(p, &b);
	if (res_init(lec, a, idx, b)) { fprintf(stderr, "res_init failed\n"); return 1; }
	const char *cs[] = { "901.LFL", "902.LFL", "904.LFL" }; int ids[] = { 1, 2, 4 };
	for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s%s", D, cs[i]); u8 *d = rdf(p, &c); res_add_charset(ids[i], d, c); }
	extern void eng_init(void); eng_init();
	if (getenv("TRACE")) g_trace = 1;
	for (int f = 0; f < frames; f++) {
		if (getenv("CLICKS")) { /* frame:x:y:btn,... */
			char *c = strdup(getenv("CLICKS")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) { int ff, x, y, b; if (sscanf(t, "%d:%d:%d:%d", &ff, &x, &y, &b) == 4 && ff == f) { if (b == 0) { g_mouse_x = x; g_mouse_y = y; } else eng_click(x, y, b); } } free(c);
		}
		if (getenv("ENTER") && f == 1300) { extern void eng_load_room_with_ego(int,int,int,int); eng_load_room_with_ego(353, 52, -1, -1); }
		if (getenv("GOTO") && f == 1300) { extern void eng_load_room_with_ego(int,int,int,int); int r = atoi(getenv("GOTO")); g_actors[1].room = r; eng_start_room(r); act_put(1, getenv("EX")?atoi(getenv("EX")):160, getenv("EY")?atoi(getenv("EY")):110, r); eng_follow_actor(1); }
		if (getenv("WALKOBJ") && f == 800) { extern void act_walk_to_obj(int,int); act_walk_to_obj(1, atoi(getenv("WALKOBJ"))); }
		if (getenv("TRACE_FROM") && f == atoi(getenv("TRACE_FROM"))) g_trace = 1;
		eng_frame();
		if (g_halted) { fprintf(stderr, "halted at frame %d\n", f); break; }
		if (every && f % every == 0) { snprintf(p, sizeof p, "%s_%04d.ppm", out, f); dump_ppm(p); }
	}
	snprintf(p, sizeof p, "%s_last.ppm", out); dump_ppm(p);
	fprintf(stderr, "room=%d frames=%d\n", g_room, frames);
	if (getenv("DUMP")) {
		for (int i = 0; i < NUM_SLOTS; i++) if (g_slots[i].status) fprintf(stderr, "slot %d: script %d status %d where %d pc %u delay %d\n", i, g_slots[i].number, g_slots[i].status, g_slots[i].where, g_slots[i].pc, g_slots[i].delay);
		for (int a = 0; a < 13; a++) fprintf(stderr, "actor %d room %d cost %d pos %d,%d vis %d facing %d moving %d tgt %d,%d\n", a, g_actors[a].room, g_actors[a].costume, g_actors[a].x, g_actors[a].y, g_actors[a].visible, g_actors[a].facing, g_actors[a].moving, g_actors[a].tx, g_actors[a].ty);
		fprintf(stderr, "ego=%d cam=%d,%d\n", g_vars[VAR_EGO], g_cam_cur, g_cam_dest);
	}
	return 0;
}
