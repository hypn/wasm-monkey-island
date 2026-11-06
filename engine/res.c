#include "scumm.h"
#include <stdlib.h>

u8 *g_disks[MAX_DISKS]; u32 g_disk_sizes[MAX_DISKS]; u8 g_lf_disk[256];   /* up to 4 disks; each room lives on exactly one */
int g_num_disks;
DirEnt g_dir_room[256], g_dir_script[NUM_GLOBAL_SCRIPTS], g_dir_sound[NUM_GLOBAL_SCRIPTS], g_dir_cost[NUM_GLOBAL_SCRIPTS];
int g_n_script, g_n_sound, g_n_cost;
u8 g_obj_owner[NUM_OBJECTS];
u32 g_obj_class[NUM_OBJECTS];
u32 g_lf_off[256];
char g_room_names[256][12];   /* RN block: room id -> short name ("dock", "bar") */
u8 *g_charset[10]; u32 g_charset_size[10];

/* DISK01.LEC is XOR 0x69 on disk; the caller passes raw bytes and we decode in place (copy). */
static void parse_dir(const u8 *p, DirEnt *d, int *count, int max)
{
	int n = rd16(p); p += 2;
	if (n > max) n = max;
	for (int i = 0; i < n; i++, p += 5) { d[i].room = p[0]; d[i].off = rd32(p + 1); }
	*count = n;
}

/* Add disk `n` (0-based): `lec` is the raw DISKnn.LEC (XOR 0x69), its FO block maps room -> LF offset within this disk. */
int res_add_disk(int n, const u8 *lec, u32 lec_size)
{
	if (n < 0 || n >= MAX_DISKS || lec_size < 16) return -3;
	u8 *d = malloc(lec_size);
	for (u32 i = 0; i < lec_size; i++) d[i] = lec[i] ^ 0x69;
	if (memcmp(d + 4, "LE", 2) != 0) { free(d); return -1; }
	u32 o = 6;
	if (memcmp(d + o + 4, "FO", 2) != 0) { free(d); return -2; }
	free(g_disks[n]); g_disks[n] = d; g_disk_sizes[n] = lec_size; if (n >= g_num_disks) g_num_disks = n + 1;
	int cnt = d[o + 6];
	for (int i = 0; i < cnt; i++) { int room = d[o + 7 + 5 * i]; g_lf_off[room] = rd32(d + o + 8 + 5 * i); g_lf_disk[room] = (u8)n; }
	return 0;
}

int res_init(const u8 *lec, u32 lec_size, const u8 *idx, u32 idx_size)      /* single-disk convenience (the demo) */
{
	int r = res_add_disk(0, lec, lec_size); if (r) return r;
	return res_set_index(idx, idx_size);
}

int res_set_index(const u8 *idx, u32 idx_size)
{
	/* index file: plain block headers, payload plain (only RN names are XOR 0xFF) */
	u32 p = 0;
	int which = 0;
	while (p + 6 <= idx_size) {
		u32 sz = rd32(idx + p); const u8 *tag = idx + p + 4; const u8 *b = idx + p + 6;
		if (sz < 6 || p + sz > idx_size) break;
		if (!memcmp(tag, "RN", 2)) { for (const u8 *e = b; e + 10 <= idx + p + sz && e[0]; e += 10) { int id = e[0]; for (int k = 0; k < 9; k++) g_room_names[id][k] = (char)(e[1 + k] ^ 0xFF); g_room_names[id][9] = 0; for (char *c = g_room_names[id]; *c; c++) if (*c == (char)0 ) break; } }
		else if (!memcmp(tag, "0R", 2)) { int dummy; parse_dir(b, g_dir_room, &dummy, 256); }
		else if (!memcmp(tag, "0S", 2)) parse_dir(b, g_dir_script, &g_n_script, NUM_GLOBAL_SCRIPTS);
		else if (!memcmp(tag, "0N", 2)) parse_dir(b, g_dir_sound, &g_n_sound, NUM_GLOBAL_SCRIPTS);
		else if (!memcmp(tag, "0C", 2)) parse_dir(b, g_dir_cost, &g_n_cost, NUM_GLOBAL_SCRIPTS);
		else if (!memcmp(tag, "0O", 2)) {
			int cnt = rd16(b); if (cnt > NUM_OBJECTS) cnt = NUM_OBJECTS;
			for (int i = 0; i < cnt; i++) {
				u32 v = rd32(b + 2 + 4 * i);
				g_obj_class[i] = v & 0xFFFFFF;
				g_obj_owner[i] = (u8)(v >> 24);
			}
		}
		p += sz; which++;
	}
	return 0;
}

int res_add_charset(int id, const u8 *data, u32 size)
{
	if (id < 0 || id > 9) return -1;
	g_charset[id] = malloc(size); memcpy(g_charset[id], data, size); g_charset_size[id] = size;
	return 0;
}

/* find a block (nth occurrence) of given tag inside RO of `room` */
const u8 *res_room_block(int room, const char *tag, int nth, u32 *size)
{
	u32 lf = g_lf_off[room]; const u8 *g_disk = g_disks[g_lf_disk[room]]; if (!lf || !g_disk) return NULL;
	u32 lfsz = rd32(g_disk + lf), p = lf + 8, end = lf + lfsz;
	/* RO is the first block in LF */
	if (memcmp(g_disk + p + 4, "RO", 2) != 0) return NULL;
	u32 rosz = rd32(g_disk + p); u32 q = p + 6; end = p + rosz;
	while (q + 6 <= end) {
		u32 s = rd32(g_disk + q);
		if (s < 6) break;
		if (!memcmp(g_disk + q + 4, tag, 2)) { if (nth-- == 0) { if (size) *size = s; return g_disk + q; } }
		q += s;
	}
	return NULL;
}

static const u8 *res_lookup(const DirEnt *d, int n, int id, const char *tag, u32 *size)
{
	if (id < 0 || id >= n || !d[id].room) return NULL;
	u32 lf = g_lf_off[d[id].room]; const u8 *g_disk = g_disks[g_lf_disk[d[id].room]]; u32 g_disk_size = g_disk_sizes[g_lf_disk[d[id].room]]; if (!lf || !g_disk) return NULL;
	u32 off = lf + 8 + d[id].off;
	if (off + 6 > g_disk_size) return NULL;
	if (memcmp(g_disk + off + 4, tag, 2) != 0) return NULL;
	if (size) *size = rd32(g_disk + off);
	return g_disk + off;
}
const u8 *res_script(int id, u32 *size)  { return res_lookup(g_dir_script, g_n_script, id, "SC", size); }
const u8 *res_costume(int id, u32 *size) { return res_lookup(g_dir_cost, g_n_cost, id, "CO", size); }
const u8 *res_sound(int id, u32 *size)   { return res_lookup(g_dir_sound, g_n_sound, id, "SO", size); }
