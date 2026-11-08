/* Native test driver: runs the engine headless and writes PPM/PNG frames. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern u8 *g_mask, *g_masks[4]; extern int g_room_w, g_room_h;
extern u32 g_rgb[]; extern void eng_frame(void); extern void ui_init(void);
extern int g_charset_id;
static u8 *rdf(const char *p, u32 *n) { FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(1); } fseek(f, 0, SEEK_END); *n = (u32)ftell(f); rewind(f); u8 *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b; }
static void dump_ppm(const char *path) { FILE *f = fopen(path, "wb"); fprintf(f, "P6\n320 200\n255\n"); for (int i = 0; i < 320 * 200; i++) { u32 c = g_rgb[i]; fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f); } fclose(f); }
int main(int argc, char **argv)
{
	const char *D = argc > 1 ? argv[1] : "/home/user/monkeyislanddemo/mi_demo/";
	int frames = argc > 2 ? atoi(argv[2]) : 100; const char *out = argc > 3 ? argv[3] : "/tmp/frame";
	int every = argc > 4 ? atoi(argv[4]) : 0;
	char p[512]; u32 a, b, c;
	snprintf(p, sizeof p, "%s000.LFL", D); u8 *idx = rdf(p, &b);
	int nd = 0;
	for (int dn = 1; dn <= MAX_DISKS; dn++) {          /* DISK01.LEC .. DISK04.LEC, whatever exists */
		snprintf(p, sizeof p, "%sDISK%02d.LEC", D, dn); FILE *t = fopen(p, "rb"); if (!t) continue; fclose(t);
		u8 *lec = rdf(p, &a); int r = res_add_disk(dn - 1, lec, a); if (r) { fprintf(stderr, "disk %d failed (%d)\n", dn, r); return 1; } nd++;
	}
	if (!nd || res_set_index(idx, b)) { fprintf(stderr, "resource init failed\n"); return 1; }
	const char *cs[] = { "901.LFL", "902.LFL", "903.LFL", "904.LFL" }; int ids[] = { 1, 2, 3, 4 };
	for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s%s", D, cs[i]); FILE *t = fopen(p, "rb"); if (!t) continue; fclose(t); u8 *d = rdf(p, &c); res_add_charset(ids[i], d, c); }
	extern void eng_init(void); eng_init();
	if (getenv("TRACE")) g_trace = 1;
	FILE *wav = getenv("SND_WAV") ? fopen(getenv("SND_WAV"), "wb") : NULL; u32 wav_n = 0;   /* mono 16-bit at 49716 Hz; header patched at the end */
	if (wav) { u8 h[44] = {0}; fwrite(h, 1, 44, wav); }
	for (int f = 0; f < frames; f++) {
		if (getenv("CLICKS")) { /* frame:x:y:btn,... */
			char *c = strdup(getenv("CLICKS")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) { int ff, x, y, b; if (sscanf(t, "%d:%d:%d:%d", &ff, &x, &y, &b) == 4 && ff == f) { if (b == 0) { g_mouse_x = x; g_mouse_y = y; } else eng_click(x, y, b); } } free(c);
		}
		if (getenv("ENTER") && f == 1300) { extern void eng_load_room_with_ego(int,int,int,int); eng_load_room_with_ego(353, 52, -1, -1); }
		if (getenv("SENT")) { char *c = strdup(getenv("SENT")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) { int ff, v, o; if (sscanf(t, "%d:%d:%d", &ff, &v, &o) == 3 && ff == f) { s16 av[16] = { (s16)v, (s16)o, 0 }; vm_start_script(g_vars[VAR_SENTENCE_SCRIPT], 0, 0, av); } } free(c); }
		if (getenv("KEYS")) { char *c = strdup(getenv("KEYS")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) { int ff, k; if (sscanf(t, "%d:%d", &ff, &k) == 2 && ff == f) eng_key(k); } free(c); }
		if (getenv("STOPSCR") && f == 20) { char *c = strdup(getenv("STOPSCR")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) vm_stop_script(atoi(t)); free(c); }
		if (getenv("START_ROOM") && f == 25) { vm_stop_script(149); vm_stop_script(1); eng_start_room(atoi(getenv("START_ROOM"))); }
		if (getenv("SNDSTART")) { char *c = strdup(getenv("SNDSTART")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) { int ff, id; if (sscanf(t, "%d:%d", &ff, &id) == 2 && ff == f) eng_sound_start(id); } free(c); }
		if (getenv("SEQ")) { char *c = strdup(getenv("SEQ")); for (char *t = strtok(c, ","); t; t = strtok(NULL, ",")) { int ff, o, r; if (sscanf(t, "%d:%d:%d", &ff, &o, &r) == 3 && ff == f) { extern void eng_load_room_with_ego(int,int,int,int); eng_load_room_with_ego(o, r, -1, -1); } } free(c); }
		if (getenv("GOTO") && f == 1300) { extern void eng_load_room_with_ego(int,int,int,int); int r = atoi(getenv("GOTO")); g_actors[1].room = r; eng_start_room(r); act_put(1, getenv("EX")?atoi(getenv("EX")):160, getenv("EY")?atoi(getenv("EY")):110, r); eng_follow_actor(1); }
		if (getenv("ECOST") && f == 1301) { g_vars[1] = 1; g_actors[1].costume = atoi(getenv("ECOST")); extern void act_start_anim(int,int); act_start_anim(1, g_actors[1].stand_frame * 4 + 0); }
		if (getenv("WALKOBJ") && f == 800) { extern void act_walk_to_obj(int,int); act_walk_to_obj(1, atoi(getenv("WALKOBJ"))); }
		if (getenv("TXTTEST") && f == 2000) { extern void eng_string_op(int,int,int,int,const u8*); extern void txt_print(int,int,const u8*,int,int,int,int,int,int,int,int,int); eng_string_op(1, 24, 0, 0, (const u8 *)"Soon you'll be wearing my sword!"); static const u8 t[] = { 0xFF, 7, 24, 0 }; txt_print(0, 1, t, 4, 160, 40, 15, 1, 1, 0, 0, 0); if (getenv("TXTTEST")[0] == 'f') { static const u8 c[] = "Of course I have a helmet. What sort of idiot do \xfe\x08     you take me for?"; txt_print(0, 0xFF, c, sizeof c, 160, 60, 15, 1, 0, 0, 0, 0); }
		if (getenv("TXTTEST")[0] == 'c') { static const u8 c[] = { 'H','e','l','l','o',' ',0xFF,12,0x0B,0x00,'c','y','a','n',' ',0xFF,12,0x04,0x00,'r','e','d',0xFF,12,0xFF,0x00,' ','w','h','i','t','e',' ','a','n','d',' ','s','o',' ','l','o','n','g',' ','t','h','a','t',' ','i','t',' ','w','r','a','p','s',' ','o','n','t','o',' ','a','n','o','t','h','e','r',' ','l','i','n','e',0 }; txt_print(0, 0xFF, c, sizeof c, 160, 60, 15, 1, 0, 0, 0, 0); } }
		if (getenv("VERBTEST") && f == 2000) { static const u8 n[] = "Of course I have a helmet. What sort of idiot do \xfe\x08     you take me for?"; eng_verb_op(40, 9, 0, 0, 0, 0, 0); eng_verb_op(40, 5, 8, 164, 0, 0, 0); eng_verb_op(40, 3, 15, 0, 0, 0, 0); eng_verb_op(40, 2, 0, 0, n, sizeof n - 1, 0); eng_verb_op(40, 6, 0, 0, 0, 0, 0); }
		if (getenv("TXTTEST") && getenv("TXTTEST")[0] == 'g') { extern void txt_print(int,int,const u8*,int,int,int,int,int,int,int,int,int); extern u8 g_txt[]; if (f == 2000) { static const u8 t[] = "Two-three-four!"; txt_print(1, 0xFF, t, sizeof t, 100, 40, 6, 0, 0, 0, 0, 1); } if (f == 2001 || f == 2006) { int nz = 0; for (int i = 0; i < 320 * 200; i++) if (g_txt[i] != 0xFF) nz++; fprintf(stderr, "frame %d overlay pixels=%d\n", f, nz); } if (f == 2003) eng_start_room(41); }
		if (getenv("OVERLAYLOG") && f % 100 == 0 && f >= 1300) { extern u8 g_txt[]; int nz = 0; for (int i = 0; i < 320 * 200; i++) if (g_txt[i] != 0xFF) nz++; fprintf(stderr, "frame %d room %d overlay=%d ego=%d v32=%d\n", f, g_room, nz, g_vars[1], g_vars[32]); }
		if (getenv("GIVE") && f == 1310) { int id = atoi(getenv("GIVE")); extern const u8 *res_room_block(int, const char *, int, u32 *); for (int r = 1; r < 100; r++) for (int n = 0;; n++) { u32 sz; const u8 *oc = res_room_block(r, "OC", n, &sz); if (!oc) break; if ((int)(oc[6] | (oc[7] << 8)) == id) { g_inventory[1] = id; g_obj_owner[id] = 0x01; memcpy(g_inv_obj_data[1], oc, sz < INV_OBJ_MAX ? sz : INV_OBJ_MAX); r = 100; break; } } }
		if (getenv("STARTSCR")) { int ff, id; if (sscanf(getenv("STARTSCR"), "%d:%d", &ff, &id) == 2 && ff == f) { s16 a[16] = {0}; vm_start_script(id, 0, 0, a); } }
		if (getenv("TXTTEST") && getenv("TXTTEST")[0] == 'n' && f == 2000) { extern void eng_set_object_name(int, const u8 *, int); extern void txt_print(int,int,const u8*,int,int,int,int,int,int,int,int,int); static const u8 nm[] = { 0xFF, 4, 0x08, 0x00, ' ', 'p', 'i', 'e', 'c', 'e', 's', ' ', 'o', 'f', ' ', 'e', 'i', 'g', 'h', 't', 0 }; eng_set_object_name(497, nm, sizeof nm); g_vars[0x1F1] = 497; g_vars[8] = 12; static const u8 t[] = { 'Y', 'o', 'u', ' ', 'h', 'a', 'v', 'e', ' ', 0xFF, 6, 0xF1, 0x01, 0 }; txt_print(0, 0xFF, t, sizeof t, 160, 60, 15, 1, 0, 0, 0, 0); }
		if (getenv("EGOLOG") && f >= 1300 && f % atoi(getenv("EGOLOG")) == 0) { Actor *e = &g_actors[1]; fprintf(stderr, "f%d room %d ego %d,%d box %d scale %d moving %d facing %d tgt %d,%d vis %d\n", f, g_room, e->x, e->y, e->walkbox, e->scalex, e->moving, e->facing, e->tx, e->ty, e->visible); }
		if (getenv("OBJRUN")) { int ff, o, v; if (sscanf(getenv("OBJRUN"), "%d:%d:%d", &ff, &o, &v) == 3 && ff == f) { extern void vm_run_object_script(int,int,int,int,const s16*); vm_run_object_script(o, v, 0, 0, NULL); } }
		if (getenv("CLASSLOG") && f == atoi(getenv("CLASSLOG"))) { extern u32 g_obj_class[]; fprintf(stderr, "class 635=%06x 636=%06x 637=%06x 869=%06x\n", g_obj_class[635], g_obj_class[636], g_obj_class[637], g_obj_class[869]); }
		if (getenv("TRACE_FROM") && f == atoi(getenv("TRACE_FROM"))) g_trace = 1;
		static s16 pv[NUM_VARS]; if (getenv("DIFFVARS") && (f == atoi(getenv("DIFFVARS")) - 1 || f == atoi(getenv("DIFFVARS")) + 60)) { if (f < atoi(getenv("DIFFVARS"))) memcpy(pv, g_vars, sizeof pv); else for (int i = 0; i < NUM_VARS; i++) if (pv[i] != g_vars[i]) fprintf(stderr, "var %d: %d -> %d\n", i, pv[i], g_vars[i]); }
		{ static u8 *sv; static u32 svn; extern int state_save(u8 **, u32 *); extern int state_load(const u8 *, u32);
		  if (getenv("SAVE_AT") && f == atoi(getenv("SAVE_AT"))) { int r = state_save(&sv, &svn); fprintf(stderr, "state_save -> %d, %u bytes (room %d)\n", r, svn, g_room); }
		  if (getenv("PICKUP")) { int ff, o; if (sscanf(getenv("PICKUP"), "%d:%d", &ff, &o) == 2 && ff == f) { extern void eng_pickup_object(int); eng_pickup_object(o); } }
		  if (getenv("SAVE_FILE") && getenv("SAVE_AT") && f == atoi(getenv("SAVE_AT")) && sv) { FILE *sf = fopen(getenv("SAVE_FILE"), "wb"); fwrite(sv, 1, svn, sf); fclose(sf); }
		  if (getenv("LOAD_FILE") && getenv("LOAD_AT") && f == atoi(getenv("LOAD_AT"))) { u32 ln; u8 *ld = rdf(getenv("LOAD_FILE"), &ln); int r = state_load(ld, ln); fprintf(stderr, "state_load(file) -> %d (room %d)\n", r, g_room); for (int i = 1; i < NUM_INVENTORY; i++) if (g_inventory[i]) fprintf(stderr, "inv[%d]=%d verb entry(look at)=%d code[18]=%d sum=%u\n", i, g_inventory[i], eng_find_verb_entry(g_inventory[i], 0xFF), g_inv_obj_data[i][18], ({unsigned s=0; for (int k=0;k<INV_OBJ_MAX;k++) s=s*31+g_inv_obj_data[i][k]; s;})); }
		  if (getenv("LOAD_AT") && f == atoi(getenv("LOAD_AT")) && sv) { int r = state_load(sv, svn); fprintf(stderr, "state_load -> %d (room %d)\n", r, g_room); } }
		eng_frame();
		{ extern int snd_read_raw(s16 *, int); s16 sb[4096]; int n; while ((n = snd_read_raw(sb, 4096)) > 0) { if (wav) { fwrite(sb, 2, n, wav); wav_n += n; } } }
		if (g_halted) { fprintf(stderr, "halted at frame %d\n", f); break; }
		if (every && f % every == 0) { snprintf(p, sizeof p, "%s_%04d.ppm", out, f); dump_ppm(p); }
	}
	snprintf(p, sizeof p, "%s_last.ppm", out); dump_ppm(p);
	if (getenv("DUMPMASK") && g_mask) { /* overlay the foreground mask (red) on the room background */
		FILE *f = fopen("/tmp/mask.ppm", "wb"); fprintf(f, "P6\n%d %d\n255\n", g_room_w, g_room_h);
		for (int y = 0; y < g_room_h; y++) for (int x = 0; x < g_room_w; x++) { int m = g_masks[getenv("MASKPLANE") ? atoi(getenv("MASKPLANE")) : 1][y * g_room_w + x]; const unsigned char *c = g_ega_palette[g_bg[y * g_room_w + x] & 15]; fputc(m ? 255 : c[0] / 2, f); fputc(m ? 0 : c[1] / 2, f); fputc(m ? 0 : c[2] / 2, f); }
		fclose(f);
	}
	if (wav) { u32 rate = 49716, bytes = wav_n * 2; u8 h[44]; memcpy(h, "RIFF", 4); u32 v = 36 + bytes; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4); u16 t = 1; memcpy(h + 20, &t, 2); memcpy(h + 22, &t, 2); memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4); t = 2; memcpy(h + 32, &t, 2); t = 16; memcpy(h + 34, &t, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4); fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav); }
	fprintf(stderr, "room=%d frames=%d\n", g_room, frames);
	if (getenv("DUMP")) {
		for (int i = 0; i < NUM_SLOTS; i++) if (g_slots[i].status) fprintf(stderr, "slot %d: script %d status %d where %d pc %u delay %d\n", i, g_slots[i].number, g_slots[i].status, g_slots[i].where, g_slots[i].pc, g_slots[i].delay);
		for (int a = 0; a < 13; a++) fprintf(stderr, "actor %d room %d cost %d pos %d,%d vis %d facing %d moving %d tgt %d,%d\n", a, g_actors[a].room, g_actors[a].costume, g_actors[a].x, g_actors[a].y, g_actors[a].visible, g_actors[a].facing, g_actors[a].moving, g_actors[a].tx, g_actors[a].ty);
		if (getenv("LIMBS")) { fprintf(stderr, "ego walkbox=%d scale=%d bbox=%d,%d-%d,%d\n", g_actors[1].walkbox, g_actors[1].scalex, g_actors[1].hx1, g_actors[1].hy1, g_actors[1].hx2, g_actors[1].hy2); Actor *e = &g_actors[1]; fprintf(stderr, "ego limbs: facing=%d cur_frame=%d stopped=%04x", e->facing, e->cur_frame, e->stopped); for (int i = 0; i < 3; i++) fprintf(stderr, " [%d: frame=%04x curpos=%04x]", i, e->frame[i], e->curpos[i]); fprintf(stderr, "\n"); }
		if (getenv("OBJS")) for (int i = 0; i < g_nobjs; i++) fprintf(stderr, "obj %d at %d,%d %dx%d walk %d,%d name=%s\n", g_objs[i].id, g_objs[i].x, g_objs[i].y, g_objs[i].w, g_objs[i].h, g_objs[i].walk_x, g_objs[i].walk_y, eng_obj_name(g_objs[i].id) ? (const char *)eng_obj_name(g_objs[i].id) : "");
		if (getenv("BOXES")) for (int i = 0; i < g_nboxes; i++) { Box *b = &g_boxes[i]; fprintf(stderr, "box %d: (%d,%d) (%d,%d) (%d,%d) (%d,%d) mask=%d flags=%#x scale=%#x\n", i, b->ulx, b->uly, b->urx, b->ury, b->lrx, b->lry, b->llx, b->lly, b->mask, b->flags, b->scale); }
		fprintf(stderr, "keys: mainmenu(50)=%d restart(42)=%d pause(43)=%d cutsceneexit(24)=%d talkstop(57)=%d\n", g_vars[50], g_vars[42], g_vars[43], g_vars[24], g_vars[57]);
		fprintf(stderr, "ego=%d cam=%d,%d follow=%d\n", g_vars[VAR_EGO], g_cam_cur, g_cam_dest, g_cam_follow);
	}
	return 0;
}

/* ---- platform storage for the native test harness: ./saves/slotN.{meta,sav} ---- */
#include "menu.h"
#include <time.h>
#include <sys/stat.h>
static void spath(char *p, int n, int slot, const char *ext) { snprintf(p, n, "saves/slot%d.%s", slot, ext); }
int plat_save_put(int slot, const char *meta, const u8 *data, u32 len)
{ char p[64]; mkdir("saves", 0777); spath(p, sizeof p, slot, "meta"); FILE *f = fopen(p, "wb"); if (!f) return -1; fputs(meta, f); fclose(f);
  spath(p, sizeof p, slot, "sav"); f = fopen(p, "wb"); if (!f) return -1; fwrite(data, 1, len, f); fclose(f); return 0; }
int plat_save_meta(int slot, char *out, int max)
{ char p[64]; spath(p, sizeof p, slot, "meta"); FILE *f = fopen(p, "rb"); if (!f) return -1; int n = (int)fread(out, 1, max - 1, f); fclose(f); out[n] = 0; return n; }
int plat_save_get(int slot, u8 *out, u32 max)
{ char p[64]; spath(p, sizeof p, slot, "sav"); FILE *f = fopen(p, "rb"); if (!f) return -1; int n = (int)fread(out, 1, max, f); fclose(f); return n; }
void plat_timestamp(char *out, int max) { time_t t = time(NULL); strftime(out, max, "%Y-%m-%d %H:%M", localtime(&t)); }
int plat_prompt_name(const char *cur, char *out, int max) { (void)cur; (void)out; (void)max; return -1; }
