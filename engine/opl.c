/* Software YM3812 (OPL2) FM synthesiser - original implementation written from the chip's documented
 * behaviour (register map, operator/channel layout, envelope times, KSL/tremolo/vibrato, waveforms,
 * feedback and phase-modulation depths).  It is deliberately simple (floating point, no ROM tables):
 * the point is to be driven by the game's own AdLib register writes, see sound.c.
 *
 * Not implemented yet (needed only by sound effects / rhythm instruments): rhythm-mode drum voices
 * (channels 7 and 8 stay silent while BD bit 5 is set; channel 6, the bass drum, is a normal 2-op voice),
 * composite-sine/CSM, timers. */
#include "opl.h"
#include <math.h>
#include <string.h>

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };
typedef struct {
	u8 r20, r40, r60, r80, rE0;          /* raw operator registers */
	double phase;                        /* cycles, 0..1 */
	double att;                          /* envelope attenuation in dB, 0 = loudest, >= 96 = silent */
	int state;
	double o1, o2;                       /* last two outputs (feedback) */
} Op;
typedef struct { int fnum, block, kon, fb, cnt; } Chan;

static Op op[18];
static Chan ch[9];
static u8 reg_01, reg_BD, reg_08;
static double lfo_trem, lfo_vib;         /* phases in cycles */
static double sin_tab[1024];
static int tab_ready;
static const int slot_mod[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
static const double mult_tab[16] = { 0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15 };
static const int ksl_rom[16] = { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };

static int reg_slot(int off)             /* register offset within a 0x20 group -> operator slot, or -1 */
{
	if (off < 0 || off > 0x15 || (off & 7) > 5) return -1;
	return (off >> 3) * 6 + (off & 7);
}

void opl_reset(void)
{
	if (!tab_ready) { for (int i = 0; i < 1024; i++) sin_tab[i] = sin(2 * M_PI * (i + 0.0) / 1024.0); tab_ready = 1; }
	memset(op, 0, sizeof op); memset(ch, 0, sizeof ch);
	for (int i = 0; i < 18; i++) { op[i].att = 96; op[i].state = EG_OFF; }
	reg_01 = reg_BD = reg_08 = 0; lfo_trem = lfo_vib = 0;
}


void opl_write(int reg, int val)
{
	val &= 0xFF;
	if (reg == 0x01) { reg_01 = (u8)val; return; }
	if (reg == 0x08) { reg_08 = (u8)val; return; }
	if (reg == 0xBD) { reg_BD = (u8)val; return; }
	if (reg >= 0x20 && reg <= 0x95) {
		int s = reg_slot(reg & 0x1F); if (s < 0) return;
		switch (reg & 0xE0) {
		case 0x20: op[s].r20 = (u8)val; break;
		case 0x40: op[s].r40 = (u8)val; break;
		case 0x60: op[s].r60 = (u8)val; break;
		case 0x80: op[s].r80 = (u8)val; break;
		}
		return;
	}
	if (reg >= 0xE0 && reg <= 0xF5) { int s = reg_slot(reg & 0x1F); if (s >= 0) op[s].rE0 = (u8)(val & 3); return; }
	if (reg >= 0xA0 && reg <= 0xA8) { Chan *c = &ch[reg - 0xA0]; c->fnum = (c->fnum & 0x300) | val; return; }
	if (reg >= 0xB0 && reg <= 0xB8) {
		Chan *c = &ch[reg - 0xB0]; int kon = (val >> 5) & 1;
		c->fnum = (c->fnum & 0xFF) | ((val & 3) << 8); c->block = (val >> 2) & 7;
		if (kon && !c->kon) { for (int k = 0; k < 2; k++) { Op *o = &op[slot_mod[reg - 0xB0] + 3 * k]; o->phase = 0; o->state = EG_ATTACK; } }
		else if (!kon && c->kon) { for (int k = 0; k < 2; k++) { Op *o = &op[slot_mod[reg - 0xB0] + 3 * k]; if (o->state != EG_OFF) o->state = EG_RELEASE; } }
		c->kon = kon; return;
	}
	if (reg >= 0xC0 && reg <= 0xC8) { Chan *c = &ch[reg - 0xC0]; c->fb = (val >> 1) & 7; c->cnt = val & 1; return; }
}

/* effective envelope rate 0..63 for a 4-bit rate value */
static int eff_rate(int rate4, const Chan *c, int ksr)
{
	if (!rate4) return 0;
	int ksv = (c->block << 1) | ((c->fnum >> 9) & 1);
	int r = rate4 * 4 + (ksr ? ksv : ksv >> 2);
	return r > 63 ? 63 : r;
}
static double ms_to_samples(double ms) { return ms * OPL_RATE / 1000.0; }

static void env_step(Op *o, const Chan *c)
{
	int ksr = (o->r20 >> 4) & 1, egt = (o->r20 >> 5) & 1;
	if (o->state == EG_OFF) return;
	if (o->state == EG_ATTACK) {
		int r = eff_rate(o->r60 >> 4, c, ksr);
		if (r >= 60) o->att = 0;
		else if (r) { double n = ms_to_samples(2826.24 / pow(2.0, (r - 4) / 4.0)); o->att *= exp(-6.24 / (n < 1 ? 1 : n)); if (o->att < 0.2) o->att = 0; }
		if (o->att <= 0) o->state = EG_DECAY;
		return;
	}
	double sl = (o->r80 >> 4) * 3.0; if ((o->r80 >> 4) == 15) sl = 93;
	int rate4 = o->state == EG_DECAY ? (o->r60 & 15) : (o->r80 & 15);
	if (o->state == EG_SUSTAIN && egt) return;                   /* held while the key is down */
	int r = eff_rate(rate4, c, ksr);
	if (r) o->att += 96.0 / ms_to_samples(39280.64 / pow(2.0, (r - 4) / 4.0));
	if (o->state == EG_DECAY && o->att >= sl) { o->att = sl; o->state = EG_SUSTAIN; }
	if (o->att >= 96) { o->att = 96; o->state = EG_OFF; }
}

static double ksl_db(const Op *o, const Chan *c)
{
	int k = o->r40 >> 6; if (!k) return 0;
	double db = (ksl_rom[(c->fnum >> 6) & 15] - 8 * (7 - c->block)) * 0.75; if (db < 0) db = 0;
	return k == 1 ? db / 2 : k == 2 ? db / 4 : db;
}

static double wave(int w, double p)      /* p in cycles */
{
	p -= floor(p); int i = (int)(p * 1024) & 1023; double s = sin_tab[i];
	switch (w) {
	case 1: return s < 0 ? 0 : s;
	case 2: return s < 0 ? -s : s;
	case 3: return (i & 256) ? 0 : (s < 0 ? -s : s);
	default: return s;
	}
}

static double op_out(Op *o, const Chan *c, double mod_cycles, double trem_db, double vib)
{
	env_step(o, c);
	if (o->state == EG_OFF) { o->phase += 0; return 0; }
	double inc = (double)c->fnum * (double)(1 << c->block) * mult_tab[o->r20 & 15] / 1048576.0;
	if (o->r20 & 0x40) inc *= vib;
	o->phase += inc; o->phase -= floor(o->phase);
	double db = o->att + (o->r40 & 63) * 0.75 + ksl_db(o, c) + ((o->r20 & 0x80) ? trem_db : 0);
	if (db >= 96) return 0;
	double w = (reg_01 & 0x20) ? o->rE0 : 0;
	return wave((int)w, o->phase + mod_cycles) * pow(10.0, -db / 20.0);
}

void opl_generate(s16 *out, int n)
{
	for (int i = 0; i < n; i++) {
		lfo_trem += 3.7 / OPL_RATE; lfo_trem -= floor(lfo_trem);
		lfo_vib += 6.1 / OPL_RATE; lfo_vib -= floor(lfo_vib);
		double tri = lfo_trem < 0.5 ? lfo_trem * 2 : 2 - lfo_trem * 2;               /* 0..1 */
		double trem_db = tri * ((reg_BD & 0x80) ? 4.8 : 1.0);
		double vtri = lfo_vib < 0.5 ? lfo_vib * 4 - 1 : 3 - lfo_vib * 4;            /* -1..1 */
		double vib = 1.0 + vtri * ((reg_BD & 0x40) ? 0.0081 : 0.0040);
		double mix = 0;
		for (int c = 0; c < 9; c++) {
			if ((reg_BD & 0x20) && c >= 7) continue;                                  /* rhythm drums: not implemented */
			Chan *cc = &ch[c]; Op *m = &op[slot_mod[c]], *k = &op[slot_mod[c] + 3];
			if (m->state == EG_OFF && k->state == EG_OFF) continue;
			double fb = cc->fb ? (m->o1 + m->o2) * pow(2.0, cc->fb - 7) : 0;
			double mo = op_out(m, cc, fb, trem_db, vib);
			m->o2 = m->o1; m->o1 = mo;
			double ko = op_out(k, cc, cc->cnt ? 0 : mo * 2.0, trem_db, vib);
			mix += cc->cnt ? mo + ko : ko;
		}
		int v = (int)(mix * 4200.0); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
		out[i] = (s16)v;
	}
}
