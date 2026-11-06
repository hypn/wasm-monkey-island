/* Sound: the game's own AdLib music driver, recovered from MIDEMO.EXE (segment 1fe8, AdLib mode 3), driving
 * the software OPL2 in opl.c.
 *
 * Structure (kept close to the original so sound effects can be added later without a redesign):
 *   - script ops -> snd_start / snd_stop / snd_running      (EXE 78f0 queue, 56ce/566a stop, 77aa running = refcount)
 *   - "ISR" snd_isr() runs at 473.3 Hz (PIT reload 0x09d9 = 2521); the original chains to the BIOS timer every 26th
 *     call (18.2 Hz) and bumps the game timer every 2nd.  Music and (future) effects both run from it.
 *   - snd_run(ticks) is called once per engine frame: it advances the synthesiser by the same amount of time,
 *     calling the ISR at the right sample positions, and appends the samples to a ring buffer.  The audio device
 *     only *reads* the ring (snd_pull), so game state never depends on the browser's audio clock.
 *
 * AdLib sound resource ('AD' sub-block of an 'SO' block; offsets from the block start; EXE 223d/2179/2470):
 *   +6 priority, +8 0x80 = music (anything else = sound effect: handled by the FX path, not implemented yet),
 *   +9 tempo (added to an 8-bit accumulator each ISR; a step happens on overflow), +10 non-zero = play once (0 = loop),
 *   +11 u16 loop start offset relative to the first event, +0x10 instrument count n, +0x11.. n channel numbers (1-based),
 *   +0x19 n * 16-byte instruments, events start at +0x99:
 *     instrument: [0],[1] = fnum lo / B0 value (percussion only), [2] = C0, [3..7] modulator 20/40/60/80/E0,
 *                 [8..12] carrier 20/40/60/80/E0, [13] 0 = melodic voice, 1..5 = rhythm instrument.
 *     event: 0x90|ch note vel (note on, velocity unused) / 0x80|ch note vel (off) / FF 58 + 5 bytes (skipped) /
 *            FF 2F (end), each followed by a delay: 0 = none, <0x80 = steps, else (b-0x80)*0x80 + next byte; stored >> 1.
 */
#include "engine.h"
#include "opl.h"
#include <stdio.h>
#include <stdlib.h>

#define ISR_NUM 2521                        /* PIT reload; ISR rate = 1193182 / 2521 Hz */
#define RING 65536

static s16 ring[RING]; static unsigned rd_pos, wr_pos;
static double isr_left;                      /* samples until the next ISR */
static double tick_carry;                    /* fractional samples carried between engine frames */
static int snd_ready; static unsigned long dbg_isr, dbg_samples, dbg_start_isr, dbg_start_samples;

/* ---- AdLib chip access: every register write also lands in a shadow, like the original (DS:0x48c) ---- */
static u8 shadow[256];
static void adl(int reg, int val) { shadow[reg & 255] = (u8)val; opl_write(reg, val); }

static const u8 op_off[18] = { 0, 3, 1, 4, 2, 5, 8, 11, 9, 12, 10, 13, 16, 19, 17, 20, 18, 21 };   /* DS:0x3ea */
static const u8 perc_op[6] = { 0, 0, 0x14, 0x12, 0x15, 0x11 }, perc_chan[6] = { 0, 0, 7, 8, 8, 7 }, perc_bd[6] = { 0, 0x10, 8, 4, 2, 1 };
static const u16 fnum_tab[12] = { 0x200, 0x21e, 0x23f, 0x261, 0x285, 0x2ab, 0x2d4, 0x300, 0x32e, 0x35e, 0x390, 0x3c7 };   /* DS:0x3d2 */

/* ---- music player state (EXE DS:0x0000 .. 0x3d1 of the AdLib driver) ---- */
static struct {
	int id, active;
	const u8 *base;                          /* start of the AdLib block: all event/instrument offsets are relative to it */
	u32 pos, loop_pos, delay, accum, tempo, noloop;
	u16 inst[16];                            /* [0x37b] instrument offset per music channel, 0 = unused */
	u8 voice_status[9], voice_note[9], voice_b0[9];     /* [0x3af] [0x3a6] [0x3b8] */
	int nvoices; u8 bd;                      /* [0x3c1] voice count, [0x3c3] BD register image */
	unsigned beat, sub;                      /* [0x16a] [0x16c] */
} mus;

static void key_off(int v) { mus.voice_status[v] = 0; adl(0xB0 + v, mus.voice_b0[v] & 0xDF); }              /* 2742 */
static void silence_voice(int v) { adl(0xA0 + v, 0); adl(0xB0 + v, 0); }                                    /* 275c */
static void op_regs(int chan_op, const u8 *p)                                                                 /* 2705 */
{
	adl(0x20 + chan_op, p[0]); adl(0x40 + chan_op, p[1]); adl(0x60 + chan_op, p[2]); adl(0x80 + chan_op, p[3]); adl(0xE0 + chan_op, p[4]);
}
static void load_instrument(int v, const u8 *ins)                                                             /* 26dc */
{
	adl(0xC0 + v, ins[2]);
	op_regs(op_off[v * 2], ins + 3); op_regs(op_off[v * 2 + 1], ins + 8);
}
static void play_note(int v, int note)                                                                        /* 285c */
{
	int n = note - 0x1F; if (n < 0) n = 0;
	int oct = n / 12, rem = n % 12;
	unsigned f = fnum_tab[rem];
	int hi = (oct << 2) | (f >> 8) | 0x20;
	adl(0xA0 + v, f & 255); mus.voice_b0[v] = (u8)hi; adl(0xB0 + v, hi);
}
static void percussion_on(int type, const u8 *ins)                                                            /* 276f */
{
	if (type == 1) {
		load_instrument(6, ins);
		adl(0xA6, ins[0]); adl(0xB6, ins[1] & 0xDF);
		mus.bd |= 0x10; adl(0xBD, mus.bd);
	} else if (type <= 5) {
		op_regs(perc_op[type], ins + 8);
		adl(0xA0 + perc_chan[type], ins[0]); adl(0xB0 + perc_chan[type], ins[1] & 0xDF); adl(0xC0 + perc_chan[type], ins[2]);
		mus.bd |= perc_bd[type]; adl(0xBD, mus.bd);
	}
}

static void music_stop(void)                                                                                  /* ISR end-of-track / 240c */
{
	if (!mus.active) return;
	if (getenv("TRACE_SND")) fprintf(stderr, "music %d ended after %lu ISR calls = %.3f s at 473.3Hz, %.3f s of audio samples\n", mus.id, dbg_isr - dbg_start_isr, (dbg_isr - dbg_start_isr) / (1193182.0 / 2521), (dbg_samples - dbg_start_samples) / (double)OPL_RATE);
	mus.active = 0; mus.pos = 0;
	for (int v = 0; v < 9; v++) { mus.voice_status[v] = 0; silence_voice(v); }
	mus.bd = 0; adl(0xBD, 0);
}

static void music_start(int id, const u8 *b)                                                                  /* 223d + 240c + 2179 */
{
	music_stop();
	for (int v = 0; v < 9; v++) { mus.voice_status[v] = 0; mus.voice_note[v] = 0; silence_voice(v); }
	memset(mus.inst, 0, sizeof mus.inst);
	mus.base = b; mus.id = id;
	mus.tempo = b[9]; mus.noloop = b[10]; mus.loop_pos = 0x99 + (u32)(b[11] | (b[12] << 8));
	int n = b[0x10], any_perc = 0;
	for (int i = 0; i < n && i < 16; i++) {
		int c = b[0x11 + i] - 1; if (c >= 0 && c < 16) mus.inst[c] = (u16)(0x19 + 16 * i);
		any_perc |= b[0x19 + 16 * i + 13];
	}
	mus.nvoices = any_perc ? 6 : 9; mus.bd = any_perc ? 0x20 : 0;
	mus.pos = 0x99; mus.delay = 0x28; mus.accum = 0; mus.beat = mus.sub = 0;
	adl(0xBD, 0);
	dbg_start_isr = dbg_isr; dbg_start_samples = dbg_samples;
	mus.active = 1;
}

static int alloc_voice(void) { for (int v = 0; v < mus.nvoices; v++) if (!mus.voice_status[v]) return v; return -1; }   /* 28ba */

/* one music step (the part of the 0x2470 ISR after the tempo accumulator overflows) */
static void music_step(void)
{
	const u8 *b = mus.base;
	if (++mus.sub >= 0x78) { mus.sub = 0; mus.beat++; }
	mus.delay = (mus.delay - 1) & 0xFFFF;
	if (mus.delay) return;
	u32 si = mus.pos;
	for (;;) {
		int st = b[si++];
		if (st == 0xFF) {
			int t = b[si];
			if (t == 0x2F) {
				if (mus.noloop) { music_stop(); return; }
				si = mus.loop_pos; mus.delay = 0; continue;
			}
			if (t == 0x58) { si += 6; goto delay; }
			music_stop(); return;                       /* other meta events never occur in this data */
		}
		if (st >= 0x90) {
			int ch = (st - 0x90) & 15; u32 ins = mus.inst[ch];
			if (ins) {
				int type = b[ins + 0xD];
				if (type) percussion_on(type, b + ins);
				else {
					int v = alloc_voice();
					if (v >= 0) {
						key_off(v); load_instrument(v, b + ins);
						mus.voice_status[v] = (u8)st; mus.voice_note[v] = b[si];
						play_note(v, b[si]);
					}
				}
			}
		} else {                                         /* note off */
			int note = b[si], found = 0;
			for (int v = 0; v < mus.nvoices; v++)
				if (mus.voice_note[v] == note && mus.voice_status[v] == st + 0x10) { key_off(v); found = 1; break; }
			if (!found) {
				u32 ins = mus.inst[(st - 0x80) & 15];
				if (ins && b[ins + 0xD] && b[ins + 0xD] <= 5) { mus.bd &= (u8)~perc_bd[b[ins + 0xD]]; adl(0xBD, mus.bd); }
			}
		}
		si += 2;                                         /* note + velocity */
	delay: ;
		int d = b[si++];
		if (!d) continue;
		if (d >= 0x80) d = (d - 0x80) * 0x80 + b[si++];
		d >>= 1; if (!d) d = 1;
		mus.delay = (u32)d; mus.pos = si; return;
	}
}

/* ISR body (0x2470): tempo accumulator, then either the music step or (future) effect voices */
static void snd_isr(void)
{
	dbg_isr++;
	if (!mus.active) { /* TODO: sound-effect sequencer (EXE 28fc), runs only while no music is playing */ return; }
	mus.accum += mus.tempo;
	if (mus.accum <= 0xFF) return;
	mus.accum -= 0x100;
	music_step();
}

/* ---- public API (called from the script ops) ---- */
void snd_init(void) { opl_reset(); memset(&mus, 0, sizeof mus); memset(shadow, 0, sizeof shadow); adl(1, 0); adl(0xBD, 0); adl(8, 0); adl(1, 0x20); rd_pos = wr_pos = 0; isr_left = 0; tick_carry = 0; snd_ready = 1; }

static const u8 *find_sub(const u8 *so, u32 sz, const char *tag)
{
	for (u32 p = 6; p + 6 <= sz;) { u32 s = rd32(so + p); if (s < 6) break; if (!memcmp(so + p + 4, tag, 2)) return so + p; p += s; }
	return NULL;
}

void eng_sound_start(int id)
{
	u32 sz = 0; const u8 *so = res_sound(id, &sz); if (!so || !snd_ready) return;
	const u8 *ad = find_sub(so, sz, "AD"); if (!ad) return;
	if (getenv("TRACE_SND")) fprintf(stderr, "startSound %d (script %d, room %d) type=%02x tempo=%d\n", id, g_slots[g_cur_slot].number, g_room, ad[8], ad[9]);
	if (ad[8] == 0x80) music_start(id, ad);
	else { /* TODO: sound effects (AdLib FX sequencer) */ }
}
void eng_sound_stop(int id) { if (mus.active && mus.id == id) music_stop(); }
void eng_music(int id) { if (id) eng_sound_start(id); else music_stop(); }          /* stopMusic = stop everything */
int  eng_sound_running(int id) { return mus.active && mus.id == id; }

/* ---- time: advance the synthesiser with the game clock ---- */
static void push(const s16 *s, int n)
{
	for (int i = 0; i < n; i++) { ring[wr_pos++ & (RING - 1)] = s[i]; if (wr_pos - rd_pos > RING) rd_pos = wr_pos - RING; }
}
void snd_run(int ticks)
{
	if (!snd_ready) return;
	tick_carry += ticks * (double)OPL_RATE / TICKS_PER_SECOND;
	int todo = (int)tick_carry; tick_carry -= todo;
	s16 buf[256];
	while (todo > 0) {
		if (isr_left <= 0) { snd_isr(); isr_left += (double)OPL_RATE * ISR_NUM / 1193182.0; }
		int n = (int)isr_left; if (n < 1) n = 1; if (n > todo) n = todo; if (n > 256) n = 256;
		opl_generate(buf, n); push(buf, n); dbg_samples += n;
		isr_left -= n; todo -= n;
	}
}

/* raw read at OPL_RATE (native test harness); returns samples actually available */
int snd_read_raw(s16 *dst, int n)
{
	int avail = (int)(wr_pos - rd_pos); if (n > avail) n = avail;
	for (int i = 0; i < n; i++) dst[i] = ring[rd_pos++ & (RING - 1)];
	return n;
}
/* resampling read for the browser's audio callback: fills `n` float samples at `out_rate`; silence on underrun */
int snd_pull(float *dst, int n, double out_rate)
{
	double step = OPL_RATE / out_rate; static double frac;
	for (int i = 0; i < n; i++) {
		if ((int)(wr_pos - rd_pos) < 2) { for (; i < n; i++) dst[i] = 0; return 0; }
		float a = ring[rd_pos & (RING - 1)] / 32768.0f, b = ring[(rd_pos + 1) & (RING - 1)] / 32768.0f;
		dst[i] = a + (b - a) * (float)frac;
		frac += step; while (frac >= 1.0) { frac -= 1.0; rd_pos++; if ((int)(wr_pos - rd_pos) < 2) { frac = 0; break; } }
	}
	return 1;
}
