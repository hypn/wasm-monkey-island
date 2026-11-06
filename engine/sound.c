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
#include "state.h"
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

static void fx_stop_all(void);
static void music_start(int id, const u8 *b)                                                                  /* 223d + 240c + 2179 */
{
	fx_stop_all(); music_stop();
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


/* ---- sound effects (EXE 1fe8:2963 / 2a4c / 2af6 / 2b4a / 2d8b; tables verified against DS:0x440..0x4cc) ----
 * An effect ('AD' block whose +8 is not 0x80) has: +6 priority, +7 slot (0..2); slot s drives hardware voices 3s..3s+2.
 * From +8: commands  01 = instrument (15 bytes: A0, B0, C0, 2 x 5 operator regs),  02 = note (11 bytes = two 5-byte
 * operator-parameter notes: byte0 bit7 enabled, bits0-2 which register to sweep (0 fnum-lo, 1 C0, 2 carrier TL, 3 carrier
 * mult/flags, 4 modulator TL, 5 modulator mult, 6 none), bit3 loop, bit4 retrigger, bit5 timed, bit6 random sustain;
 * bytes 1-3 = attack/decay/sustain+release envelope (nibbles: target value index, time index), byte 4 = play time),
 * 80 = loop channel, any other byte = start of the next channel, FF = end.  The FX sequencer runs on every 4th ISR (118 Hz)
 * and only while no music is playing; a new effect is ignored while music plays. */
typedef struct { int state, play_time, sustain_timer, inst_val, bias, pre_inc, adjust, step_inc, step, step_cnt, timer; } FxNote;
typedef struct { int state; u32 pc, start; int inst[7]; FxNote note[2]; } FxChan;       /* state 0 off, 1 parse, 2 play */
typedef struct { int id, prio; const u8 *base; FxChan ch[3]; } FxSlot;
static FxSlot fx[3];
static int fx_timer = 4; static unsigned fx_rnd = 1;

static const int fx_steps[16] = { 1, 4, 6, 8, 10, 14, 18, 24, 36, 64, 100, 160, 240, 340, 600, 1200 };        /* DS:0x44a */
static const unsigned fx_adjust[16] = { 0, 4369, 8738, 13107, 17476, 21845, 26214, 30583, 34952, 39321, 43690, 48059, 52428, 56797, 61166, 65535 };
static const int fx_scale[7] = { 255, 7, 63, 15, 63, 15, 63 }, fx_bias[7] = { 0, 0, 0x3F, 0, 0x3F, 0, 0 };
static const int fx_useop[7] = { 0, 0, 1, 1, 1, 1, 0 }, fx_chopoff[7] = { 0, 0, 1, 1, 0, 0, 0 };
static const int fx_base[7] = { 0xA0, 0xC0, 0x40, 0x20, 0x40, 0x20, 0 }, fx_mask[7] = { 0xFF, 0x0E, 0x3F, 0x0F, 0x3F, 0x0F, 0 }, fx_shift[7] = { 0, 1, 0, 0, 0, 0, 0 };

static int fx_rand(void) { if (fx_rnd & 1) { fx_rnd >>= 1; fx_rnd ^= 0xB8; } else fx_rnd >>= 1; return (int)fx_rnd; }

static int fx_regnum(int hw, int which)
{
	int r = fx_useop[which] ? op_off[fx_chopoff[which] + hw * 2] : hw;
	return r + fx_base[which];
}
static int fx_read(int hw, int def, int which)
{
	if (which == 6) return 0;
	int v = def ? def : shadow[fx_regnum(hw, which)];
	return (v & fx_mask[which]) >> fx_shift[which];
}
static void fx_write(int hw, int value, int which)
{
	if (which == 6) return;
	int reg = fx_regnum(hw, which);
	int v = shadow[reg] & ~fx_mask[which]; v |= (value << fx_shift[which]) & 0xFF;
	adl(reg, v & 0xFF);
}
static void fx_note_off_on(int hw) { int b = shadow[0xB0 + hw]; adl(0xB0 + hw, b & 0xDF); adl(0xB0 + hw, b | 0x20); }                       /* 2d09 */
static void fx_setup_env(FxNote *n, int steps, int adjust)
{
	n->pre_inc = 0;
	if (abs(adjust) > steps) { n->pre_inc = 1; n->adjust = adjust / steps; n->step_inc = abs(adjust % steps); }
	else { n->adjust = adjust; n->step_inc = abs(adjust); }
	n->step = steps; n->step_cnt = 0; n->timer = steps;
}
static int fx_env(FxNote *n)                                       /* 2d8b: one envelope tick; non-zero when the segment is over */
{
	if (n->pre_inc) n->inst_val += n->adjust;
	n->step_cnt += n->step_inc;
	if (n->step_cnt >= n->step) { n->step_cnt -= n->step; if (n->adjust < 0) n->inst_val--; else n->inst_val++; }
	return --n->timer == 0;
}
static int fx_process_note(FxNote *n, const FxChan *c, int hw, const u8 *np)   /* 2b4a: advance to the next envelope stage; non-zero = finished */
{
	if (++n->state == 4) return 1;
	int which = np[0] & 7; n->bias = fx_bias[which];
	int v = n->state == 0 ? c->inst[which] : 0;
	int cur = fx_read(hw, v, which); if (n->bias) cur = n->bias - cur;
	n->inst_val = cur;
	if (n->state == 2) {
		n->sustain_timer = fx_steps[np[3] >> 4];
		if (np[0] & 0x40) n->sustain_timer = (((fx_rand() << 8) * n->sustain_timer) >> 16) + 1;
	} else {
		int t1, t2;
		if (n->state == 3) { t1 = np[3] & 15; t2 = 0; } else { t1 = np[n->state + 1] >> 4; t2 = np[n->state + 1] & 15; }
		int adj = (int)(((unsigned long)fx_adjust[t2] * (unsigned)fx_scale[which]) >> 16) - cur;
		fx_setup_env(n, fx_steps[t1], adj);
	}
	return 0;
}
static void fx_parse_note(FxNote *n, const FxChan *c, int hw, const u8 *np)    /* 2af6 */
{
	if (!(np[0] & 0x80)) return;
	n->state = -1; fx_process_note(n, c, hw, np); n->play_time = 0;
	if (np[0] & 0x20) n->play_time = (np[4] >> 4) * 118 + (np[4] & 15) * 8;
}
static void fx_silence(int hw) { adl(0xA0 + hw, 0); adl(0xB0 + hw, 0); }

static void fx_parse(FxSlot *s, FxChan *c, int hw)                 /* 2963 */
{
	const u8 *b = s->base;
	for (;;) {
		int cmd = b[c->pc];
		if (cmd == 1) {
			const u8 *ins = b + c->pc + 1;
			c->inst[0] = ins[0]; c->inst[1] = ins[2]; c->inst[2] = ins[9]; c->inst[3] = ins[8]; c->inst[4] = ins[4]; c->inst[5] = ins[3]; c->inst[6] = 0;
			load_instrument(hw, ins); adl(0xA0 + hw, ins[0]); adl(0xB0 + hw, ins[1] & 0xDF);
			c->pc += 15;
		} else if (cmd == 2) {
			c->state = 2; fx_note_off_on(hw);
			fx_parse_note(&c->note[0], c, hw, b + c->pc + 1); fx_parse_note(&c->note[1], c, hw, b + c->pc + 6);
			return;
		} else if (cmd == 0x80) c->pc = c->start;
		else { fx_silence(hw); c->state = 0; return; }           /* start of another channel = this one has finished */
	}
}
static void fx_play(FxSlot *s, FxChan *c, int hw)                  /* 2a4c */
{
	const u8 *np = s->base + c->pc + 1;
	for (int k = 0; k < 2; k++, np += 5) {
		if (!(np[0] & 0x80)) continue;
		FxNote *n = &c->note[k]; int advance = 0;
		if (n->state == 2) { if (!--n->sustain_timer) advance = 1; }
		else {
			advance = fx_env(n);
			fx_write(hw, n->bias ? n->bias - n->inst_val : n->inst_val, np[0] & 7);
		}
		if (advance && fx_process_note(n, c, hw, np)) {
			if (!(np[0] & 8)) { c->pc += 11; c->state = 1; continue; }
			if (np[0] & 0x10) fx_note_off_on(hw);
			n->state = -1; fx_process_note(n, c, hw, np);
		}
		if ((np[0] & 0x20) && !--n->play_time) { c->pc += 11; c->state = 1; }
	}
}
static void fx_stop_slot(FxSlot *s, int slot)
{
	if (!s->id) return;
	for (int k = 0; k < 3; k++) { if (s->ch[k].state) fx_silence(slot * 3 + k); s->ch[k].state = 0; }
	s->id = 0;
}
static void fx_stop_all(void) { for (int i = 0; i < 3; i++) fx_stop_slot(&fx[i], i); }
static void fx_start(int id, const u8 *ad)                          /* 223d, effect branch */
{
	if (mus.active) return;                                          /* effects are dropped while music plays */
	int slot = ad[7]; if (slot > 2) slot = 2;
	FxSlot *s = &fx[slot];
	if (s->id) { if (ad[6] < s->prio) return; fx_stop_slot(s, slot); }
	memset(s, 0, sizeof *s); s->id = id; s->prio = ad[6]; s->base = ad;
	for (int k = 0; k < 3; k++) silence_voice(slot * 3 + k);
	int k = 0; s->ch[0].pc = s->ch[0].start = 8; s->ch[0].state = 1;
	for (u32 p = 8; ad[p] != 0xFF;) {
		int c = ad[p];
		if (c == 1) p += 15; else if (c == 2) p += 11; else if (c == 0x80) p += 1;
		else { p++; if (k < 2) { k++; s->ch[k].pc = s->ch[k].start = p; s->ch[k].state = 1; } }
	}
}
static void fx_update(void)                                          /* 28fc */
{
	if (--fx_timer) return;
	fx_timer = 4;
	for (int i = 0; i < 3; i++) {
		FxSlot *s = &fx[i]; if (!s->id) continue;
		int active = 0;
		for (int k = 0; k < 3; k++) { FxChan *c = &s->ch[k]; if (!c->state) continue; active = 1; if (c->state == 1) fx_parse(s, c, i * 3 + k); else fx_play(s, c, i * 3 + k); }
		if (!active) s->id = 0;                                      /* every channel finished: the sound is over */
	}
}

/* ISR body (0x2470): tempo accumulator, then either the music step or (future) effect voices */
static void snd_isr(void)
{
	dbg_isr++;
	if (!mus.active) { fx_update(); return; }          /* effects run only while no music plays (EXE 28fc) */
	mus.accum += mus.tempo;
	if (mus.accum <= 0xFF) return;
	mus.accum -= 0x100;
	music_step();
}

/* EXE 1fe8:2465 returns the music beat counter ([0x16a], +1 every 120 steps); the main loop copies it into script var 14 (music timer) - retail's intro waits for it */
int snd_beat(void) { return (int)mus.beat; }

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
	else fx_start(id, ad);
}
void eng_sound_stop(int id) { if (mus.active && mus.id == id) music_stop(); for (int i = 0; i < 3; i++) if (fx[i].id == id) fx_stop_slot(&fx[i], i); }
void eng_music(int id) { if (id) eng_sound_start(id); else { music_stop(); fx_stop_all(); } }          /* stopMusic = stop everything */
int  eng_sound_running(int id) { if (mus.active && mus.id == id) return 1; for (int i = 0; i < 3; i++) if (fx[i].id == id) return 1; return 0; }

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
	/* keep the audio close to the game: the ring may hold up to ~1.3 s (it keeps filling while the browser's audio is not
	 * running yet, e.g. before the first click, and the game clock can briefly run ahead). Without this the backlog would
	 * become a permanent delay, so music would start and stop noticeably late after scene changes. */
	{ unsigned max_lag = OPL_RATE * 15 / 100; if (wr_pos - rd_pos > max_lag) rd_pos = wr_pos - max_lag; }
	for (int i = 0; i < n; i++) {
		if ((int)(wr_pos - rd_pos) < 2) { for (; i < n; i++) dst[i] = 0; return 0; }
		float a = ring[rd_pos & (RING - 1)] / 32768.0f, b = ring[(rd_pos + 1) & (RING - 1)] / 32768.0f;
		dst[i] = a + (b - a) * (float)frac;
		frac += step; while (frac >= 1.0) { frac -= 1.0; rd_pos++; if ((int)(wr_pos - rd_pos) < 2) { frac = 0; break; } }
	}
	return 1;
}

/* ---- save/load: only which music was playing; it restarts from its beginning ---- */
void snd_state(StateIO *io)
{
	int id = mus.active ? mus.id : 0;
	SIO(io, id);
	if (!io->saving) { music_stop(); if (id) eng_sound_start(id); }
}
