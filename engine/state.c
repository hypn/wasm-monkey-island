/* Save / load of the complete game state (see state.h).  Module hooks live next to the data they own. */
#include "engine.h"
#include "state.h"
#include <stdlib.h>

extern u8 g_inv_obj_data[NUM_INVENTORY][INV_OBJ_MAX];
extern void eng_load_room_data(int room);
void vm_state(StateIO *io); void eng_state(StateIO *io); void eng_state_done(void);
void act_state(StateIO *io); void ui_state(StateIO *io); void ui_state_done(void); void snd_state(StateIO *io);

void sio_bytes(StateIO *io, void *p, u32 n)
{
	if (io->err) return;
	if (io->saving) {
		if (io->pos + n > io->cap) { u32 nc = io->cap ? io->cap * 2 : 65536; while (nc < io->pos + n) nc *= 2; u8 *nb = realloc(io->buf, nc); if (!nb) { io->err = 1; return; } io->buf = nb; io->cap = nc; }
		memcpy(io->buf + io->pos, p, n); io->pos += n;
	} else {
		if (io->pos + n > io->cap) { io->err = 1; return; }
		memcpy(p, io->buf + io->pos, n); io->pos += n;
	}
}

/* reference: kind 0 = NULL, 1 = inside disk d at offset o, 2 = inside the inventory object-code table */
void sio_ptr(StateIO *io, const u8 **pp)
{
	u8 kind = 0, disk = 0; u32 off = 0;
	if (io->saving) {
		const u8 *p = *pp;
		if (p) {
			for (int d = 0; d < g_num_disks; d++) if (g_disks[d] && p >= g_disks[d] && p < g_disks[d] + g_disk_sizes[d]) { kind = 1; disk = (u8)d; off = (u32)(p - g_disks[d]); break; }
			if (!kind && p >= &g_inv_obj_data[0][0] && p < &g_inv_obj_data[0][0] + sizeof g_inv_obj_data) { kind = 2; off = (u32)(p - &g_inv_obj_data[0][0]); }
			if (!kind) io->err = 1;                     /* a pointer we cannot describe: refuse to save */
		}
	}
	SIO(io, kind); SIO(io, disk); SIO(io, off);
	if (!io->saving && !io->err) {
		if (kind == 1 && disk < g_num_disks && g_disks[disk]) *pp = g_disks[disk] + off;
		else if (kind == 2) *pp = &g_inv_obj_data[0][0] + off;
		else *pp = NULL;
	}
}

#define MAGIC 0x5653494D            /* "MISV" */
#define VERSION 1
static void header(StateIO *io, u32 *sig, s32 *room)
{
	u32 magic = MAGIC, ver = VERSION, sz[3] = { sizeof(Slot), sizeof(RoomObject), sizeof(Actor) };
	SIO(io, magic); SIO(io, ver); SIO(io, sz); SIO(io, *sig); SIO(io, *room);
	if (!io->saving && (magic != MAGIC || ver != VERSION || sz[0] != sizeof(Slot) || sz[1] != sizeof(RoomObject) || sz[2] != sizeof(Actor))) io->err = 1;
}
static u32 game_sig(void) { return (g_disk_sizes[0] ^ (g_n_script * 65537u) ^ (g_n_cost * 257u)) + (u32)g_num_disks; }   /* demo and retail saves never mix */

static void body(StateIO *io)
{
	vm_state(io); eng_state(io); act_state(io); ui_state(io); snd_state(io);
}

/* zero-run packing: 0x00 n = n zero bytes (state is mostly zeros: 64 KB of object-name overrides alone) */
static u8 *pack(const u8 *in, u32 n, u32 *outn)
{
	u8 *o = malloc(n + n / 128 + 16); u32 j = 0;
	for (u32 i = 0; i < n;) {
		if (in[i]) { o[j++] = in[i++]; continue; }
		u32 r = 0; while (i < n && !in[i] && r < 255) { i++; r++; }
		o[j++] = 0; o[j++] = (u8)r;
	}
	*outn = j; return o;
}
static u8 *unpack(const u8 *in, u32 n, u32 *outn)
{
	u32 cap = 1 << 20, j = 0; u8 *o = malloc(cap);
	for (u32 i = 0; i < n;) {
		if (in[i]) { if (j + 1 > cap) { cap *= 2; o = realloc(o, cap); } o[j++] = in[i++]; continue; }
		if (i + 1 >= n) break;
		u32 r = in[i + 1]; i += 2; while (j + r > cap) { cap *= 2; o = realloc(o, cap); } memset(o + j, 0, r); j += r;
	}
	*outn = j; return o;
}

int state_save(u8 **out, u32 *len)
{
	StateIO io = { 0 }; io.saving = 1; u32 sig = game_sig(); s32 room = g_room;
	header(&io, &sig, &room); body(&io);
	if (io.err) { free(io.buf); return -1; }
	*out = pack(io.buf, io.pos, len); free(io.buf); return 0;
}

static int state_load_raw(const u8 *data, u32 len);
int state_load(const u8 *packed, u32 plen)
{
	u32 len; u8 *data = unpack(packed, plen, &len);
	int r = state_load_raw(data, len); free(data); return r;
}
static int state_load_raw(const u8 *data, u32 len)
{
	StateIO io = { 0 }; io.buf = (u8 *)data; io.cap = len; io.saving = 0;
	u32 sig = 0; s32 room = 0; header(&io, &sig, &room);
	if (io.err || sig != game_sig() || room < 0 || room > 255) return -1;
	g_room = room; if (room) eng_load_room_data(room);      /* resources first; the streamed state then overwrites the room's defaults */
	body(&io);
	if (io.err) return -2;
	eng_state_done(); ui_state_done();
	return 0;
}
