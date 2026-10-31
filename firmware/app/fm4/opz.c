/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 OPZ-style engine. See opz.h. */
#include "opz.h"
#include "opz_tables.h"
#include <string.h>

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE, EG_REVERB };

/* 8 OPZ waveforms x 1024 entries: 4.8 log attenuation, bit 15 = sign. */
static uint16_t opz_wave[8][1024];

/* TQ5/TX81Z CRS 0..63 -> chip MUL (0..15) and DT2 (0..3), ascending ratio. */
static const uint8_t crs_map[64][2] FM4_AUDIO_DATA = {
    {0,0},{0,1},{0,2},{0,3},{1,0},{1,1},{1,2},{1,3},{2,0},{2,1},{3,0},{2,2},{2,3},{4,0},{3,1},{3,2},
    {5,0},{3,3},{4,1},{6,0},{4,2},{4,3},{7,0},{5,1},{5,2},{8,0},{6,1},{5,3},{9,0},{6,2},{7,1},{10,0},
    {6,3},{7,2},{11,0},{8,1},{12,0},{7,3},{8,2},{9,1},{13,0},{8,3},{14,0},{9,2},{10,1},{15,0},{11,1},{9,3},
    {10,2},{12,1},{11,2},{10,3},{13,1},{12,2},{11,3},{14,1},{13,2},{12,3},{15,1},{14,2},{13,3},{15,2},{14,3},{15,3}
};
/* DT2 in 1/64 semitone (ymfm: 0, 600, 781, 950 cents). */
static const int16_t dt2_delta[4] FM4_AUDIO_DATA = { 0, 384, 500, 608 };
/* DX/TX output level 0..99 -> attenuation in 0.75 dB steps (0..127). */
static const uint8_t out_low[20] FM4_AUDIO_DATA = {
    127,122,118,114,110,107,104,102,100,98,96,94,92,90,88,86,85,84,82,81
};

/* Chip phase step (0.10 at 55930 Hz) to our 32-bit phase increment at
 * 44.1 kHz. Calibrated in tests/engine_test.c so MIDI 69 = 440.00 Hz. */
#ifndef OPZ_STEP_SCALE
#define OPZ_STEP_SCALE 5196U
#endif
#define OPZ_STEP_MAX 400000U
/* Envelope clock: chip EG ticks at 55930/3 Hz. Q16 increment per sample. */
#define OPZ_EG_INC ((uint32_t)((55930.0 / 3.0 / FM4_RATE) * 65536.0 + 0.5))
/* Chip LFO counter advance per 44.1 kHz block, x1/64 (55930/44100*128*64). */
#define OPZ_LFO_BLOCK_X64 10390U

static uint16_t abs_sin_attenuation(uint32_t input)
{
    if (input & 0x100) input = ~input;
    return opz_sin_table[input & 0xff];
}

void opz_init(opz_engine *e, unsigned polyphony)
{
    memset(e, 0, sizeof *e);
    e->voices = polyphony > OPZ_MAX_VOICES ? OPZ_MAX_VOICES : polyphony ? polyphony : 8;
    for (unsigned i = 0; i < 1024; ++i)
        opz_wave[0][i] = abs_sin_attenuation(i) | (uint16_t)(((i >> 9) & 1) << 15);
    uint16_t zero = opz_wave[0][0];
    for (unsigned i = 0; i < 1024; ++i) {
        uint16_t sq = (uint16_t)(2U * (opz_wave[0][i] & 0x7fffU));
        opz_wave[1][i] = (uint16_t)((sq < zero ? sq : zero) | (((i >> 9) & 1) << 15));
    }
    for (unsigned i = 0; i < 1024; ++i) {
        unsigned hi = (i >> 9) & 1;
        opz_wave[2][i] = hi ? zero : opz_wave[0][i];
        opz_wave[3][i] = hi ? zero : opz_wave[1][i];
        opz_wave[4][i] = hi ? zero : opz_wave[0][(i * 2) & 1023];
        opz_wave[5][i] = hi ? zero : opz_wave[1][(i * 2) & 1023];
        opz_wave[6][i] = hi ? zero : opz_wave[0][(i * 2) & 0x1ff];
        opz_wave[7][i] = hi ? zero : opz_wave[1][(i * 2) & 0x1ff];
    }
    for (unsigned i = 0; i < OPZ_LFOS; ++i) e->lfo[i].noise = 0x1234567U + i;
}

static FM4_AUDIO_CODE unsigned effective_rate(unsigned raw, unsigned ksr)
{
    if (!raw) return 0;
    raw += ksr;
    return raw > 63 ? 63 : raw;
}

/* Pitch (1/64 semitone from MIDI 0) -> chip 0.10 phase step and keycode. */
static FM4_AUDIO_CODE uint32_t pitch_step(int32_t pitch, unsigned *keycode)
{
    int32_t k = pitch - 13 * 64; /* OPM block 0 starts at C#0 (MIDI 13) */
    if (k < 0) k = 0;
    unsigned block = (unsigned)k / 768U, eff = (unsigned)k % 768U;
    if (block > 7) { block = 7; eff = 767; }
    unsigned idx = eff >> 6, code = idx + idx / 3U;
    if (keycode) *keycode = (block << 2) | (code >> 2);
    return opz_phase_step[eff] >> (block ^ 7U);
}

static FM4_AUDIO_CODE void op_pitch(opz_op *o, int32_t pitch, unsigned dt2)
{
    if (o->fixed) { o->inc = o->fixed_inc; return; }
    unsigned kc;
    uint32_t step = pitch_step(pitch + dt2_delta[dt2 & 3], &kc);
    int32_t det = opz_detune1[kc][o->det1 & 3];
    if (o->det1 & 4) det = -det;
    int32_t s = (int32_t)step + det;
    if (s < 0) s = 0;
    step = ((uint32_t)s * o->multiple) >> 4;
    if (step > OPZ_STEP_MAX) step = OPZ_STEP_MAX;
    o->inc = step * OPZ_STEP_SCALE;
}

/* dt2 per op is folded into multiple's high bits storage: keep a table. */
static uint8_t op_dt2[OPZ_MAX_VOICES][4];

static FM4_AUDIO_CODE void voice_pitch(opz_engine *e, unsigned slot)
{
    opz_voice *v = &e->v[slot];
    const opz_lfo *l = &e->lfo[v->part & (OPZ_LFOS - 1)];
    int32_t delta = v->bend64;
    if (v->pms) {
        int32_t pm = l->pm_raw;
        delta += v->pms < 6 ? (pm >> (6 - v->pms)) : (pm << (v->pms - 5));
    }
    int32_t pitch = (int32_t)v->pitch64 + delta;
    for (unsigned i = 0; i < 4; ++i) op_pitch(&v->o[i], pitch, op_dt2[slot][i]);
}

FM4_AUDIO_CODE void opz_key_on(opz_engine *e, unsigned slot, const opz_patch *p,
                               unsigned part, unsigned note, unsigned velocity,
                               int32_t gain_l, int32_t gain_r, int32_t send)
{
    if (slot >= OPZ_MAX_VOICES || !p) return;
    opz_voice *v = &e->v[slot];
    v->alg = p->alg & 7; v->fbl = p->fb & 7;
    v->part = (uint8_t)part; v->note = (uint8_t)note;
    v->pms = p->pms & 7; v->ams = p->ams & 3;
    v->pitch64 = (uint16_t)(note * 64U);
    v->gain_l = gain_l; v->gain_r = gain_r;
    v->send_l = send; v->send_r = send;
    v->fb[0] = v->fb[1] = 0;
    v->age = ++e->age;
    if (velocity < 1) velocity = 1;
    if (velocity > 127) velocity = 127;
    unsigned kc;
    (void)pitch_step((int32_t)note * 64, &kc);
    v->rate_ksr = (uint8_t)kc;
    for (unsigned i = 0; i < 4; ++i) {
        /* chip slot i (0 = OP4) <- TX81Z operator 4 - i */
        const opz_op_params *q = &p->op[3 - i];
        opz_op *o = &v->o[i];
        unsigned mul = crs_map[q->crs & 63][0];
        op_dt2[slot][i] = crs_map[q->crs & 63][1];
        unsigned fine = q->fine & 15;
        if ((q->crs & 63) < 4 && fine > 7) fine = 7;   /* TX81Z: FINE 0-7 when CRS < 4 */
        o->multiple = (uint8_t)(mul ? ((mul << 4) | fine) : (8U + fine));
        o->fixed = q->fixed ? 1 : 0;
        if (o->fixed) o->fixed_inc = opz_fixed_hz(q->crs, q->fixrg, q->fine) * 97392U; /* 2^32/44100 */
        int d = (int)q->det - 3;
        o->det1 = (uint8_t)(d < 0 ? (4 | (unsigned)(-d)) : (unsigned)d);
        o->wave = q->wave & 7;
        o->ame = q->ame ? 1 : 0;
        o->shift = i == 3 ? 0 : (q->shft & 3);  /* OP1 has no EG shift on TX81Z/TQ5 */
        /* Total level: output level + velocity + keyboard level scaling. */
        unsigned out = q->out > 99 ? 99 : q->out;
        unsigned tl = out >= 20 ? 99U - out : out_low[out];
        unsigned att = tl << 3;
        att += ((unsigned)(q->kvs & 7) * (127U - velocity) * 3U) >> 3;
        if (note > 24) att += ((unsigned)q->ls * (note - 24U)) >> 3;  /* LS*(key-24)/64 TL steps */
        if (!q->on) att = 0x3ff;
        o->tl = (uint16_t)(att > 0x3ff ? 0x3ff : att);
        unsigned d1l = 15U - (q->d1l & 15);
        o->sustain = (uint16_t)((d1l | ((d1l + 1) & 0x10)) << 5);
        unsigned ksr = kc >> ((q->rs & 3) ^ 3);
        o->rate[EG_ATTACK] = (uint8_t)effective_rate((q->ar & 31) * 2U, ksr);
        o->rate[EG_DECAY] = (uint8_t)effective_rate((q->d1r & 31) * 2U, ksr);
        o->rate[EG_SUSTAIN] = (uint8_t)effective_rate((q->d2r & 31) * 2U, ksr);
        unsigned rr = q->rr & 15; if (!rr) rr = 1;
        o->rate[EG_RELEASE] = (uint8_t)effective_rate(rr * 4U + 2U, ksr);
        o->rate[EG_REVERB] = o->rate[EG_RELEASE];
        if (p->rev & 7) {
            unsigned r = effective_rate((p->rev & 7) * 4U + 2U, ksr);
            if (r < o->rate[EG_REVERB]) o->rate[EG_REVERB] = (uint8_t)r;
        }
        o->phase = 0;
        o->state = EG_ATTACK;
        if (o->rate[EG_ATTACK] >= 62) o->env = 0;
        else if (!v->active) o->env = 0x3ff;
    }
    v->active = 1; v->keyed = 1;
    voice_pitch(e, slot);
}

FM4_AUDIO_CODE void opz_key_off(opz_engine *e, unsigned slot)
{
    if (slot >= OPZ_MAX_VOICES) return;
    opz_voice *v = &e->v[slot];
    if (!v->keyed) return;
    v->keyed = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (v->o[i].state < EG_RELEASE) v->o[i].state = EG_RELEASE;
}

FM4_AUDIO_CODE void opz_kill(opz_engine *e, unsigned slot)
{
    if (slot >= OPZ_MAX_VOICES) return;
    e->v[slot].active = 0; e->v[slot].keyed = 0;
    for (unsigned i = 0; i < 4; ++i) e->v[slot].o[i].env = 0x3ff;
}

FM4_AUDIO_CODE void opz_set_pitch(opz_engine *e, unsigned slot, unsigned pitch64)
{
    if (slot >= OPZ_MAX_VOICES) return;
    e->v[slot].pitch64 = (uint16_t)pitch64;
}

FM4_AUDIO_CODE void opz_set_bend(opz_engine *e, unsigned slot, int bend64)
{
    if (slot >= OPZ_MAX_VOICES) return;
    e->v[slot].bend64 = (int16_t)bend64;
}

FM4_AUDIO_CODE int opz_voice_busy(const opz_engine *e, unsigned slot)
{
    return slot < OPZ_MAX_VOICES && e->v[slot].active;
}

/* ---- LFO ----------------------------------------------------------------- */
/* TX81Z speed 0..99 -> OPZ LFO rate register (4.4 pseudo-float). */
static FM4_AUDIO_CODE uint8_t lfo_rate_reg(unsigned speed)
{
    if (speed > 99) speed = 99;
    return (uint8_t)(0x30U + (speed * (0xFFU - 0x30U) + 49U) / 99U);
}

FM4_AUDIO_CODE void opz_lfo_setup(opz_engine *e, unsigned part, const opz_patch *p)
{
    opz_lfo *l = &e->lfo[part & (OPZ_LFOS - 1)];
    l->rate_reg = lfo_rate_reg(p->lfs);
    l->wave = p->lfw & 3;
    l->pmd = (uint8_t)((p->pmd > 99 ? 99 : p->pmd) * 127U / 99U);
    l->amd = (uint8_t)((p->amd > 99 ? 99 : p->amd) * 127U / 99U);
    l->sync = p->sync ? 1 : 0;
    /* LFO delay 0..99 -> up to about 5 s, in blocks */
    l->delay_count = (uint32_t)(p->lfd > 99 ? 99 : p->lfd) * 18U;
}

static uint8_t lfo_extra_pm[OPZ_LFOS], lfo_extra_am[OPZ_LFOS];
static uint32_t lfo_delay_left[OPZ_LFOS];

FM4_AUDIO_CODE void opz_lfo_keyon(opz_engine *e, unsigned part)
{
    opz_lfo *l = &e->lfo[part & (OPZ_LFOS - 1)];
    if (l->sync) l->counter = 0;
    lfo_delay_left[part & (OPZ_LFOS - 1)] = l->delay_count;
}

FM4_AUDIO_CODE void opz_lfo_extra(opz_engine *e, unsigned part, unsigned pm, unsigned am)
{
    (void)e;
    lfo_extra_pm[part & (OPZ_LFOS - 1)] = (uint8_t)(pm > 127 ? 127 : pm);
    lfo_extra_am[part & (OPZ_LFOS - 1)] = (uint8_t)(am > 127 ? 127 : am);
}

static FM4_AUDIO_CODE void lfo_block(opz_engine *e)
{
    for (unsigned n = 0; n < OPZ_LFOS; ++n) {
        opz_lfo *l = &e->lfo[n];
        uint32_t step = (0x10U | (l->rate_reg & 15U)) << (l->rate_reg >> 4);
        l->counter += step * (OPZ_LFO_BLOCK_X64 >> 6) + ((step * (OPZ_LFO_BLOCK_X64 & 63U)) >> 6);
        unsigned idx = (l->counter >> 22) & 0xff;
        unsigned am, pm;
        switch (l->wave) {
        case 0: am = idx ^ 0xff; pm = idx; break;                       /* saw up */
        case 1: am = (idx & 0x80) ? 0 : 0xff; pm = am ^ 0x80; break;    /* square */
        case 2: am = ((idx & 0x80) ? idx : idx ^ 0xff) << 1 & 0xff;     /* triangle */
                pm = (idx & 0x40) ? am : (~am & 0xff); break;
        default:                                                         /* S&H */
            if ((idx >> 4) != (l->last_index >> 4)) {
                l->noise = l->noise * 1664525U + 1013904223U;
                l->sh_value = (uint8_t)(l->noise >> 24);
            }
            am = pm = l->sh_value; break;
        }
        l->last_index = (uint8_t)idx;
        unsigned pmd = l->pmd + lfo_extra_pm[n], amd = l->amd + lfo_extra_am[n];
        if (pmd > 127) pmd = 127;
        if (amd > 127) amd = 127;
        if (lfo_delay_left[n]) {
            /* linear fade-in over the delay time */
            unsigned total = l->delay_count ? l->delay_count : 1;
            unsigned k = 256U - (lfo_delay_left[n] * 256U) / total;
            pmd = (pmd * k) >> 8; amd = (amd * k) >> 8;
            --lfo_delay_left[n];
        }
        l->am_raw = (uint16_t)((am * amd) >> 7);
        l->pm_raw = (int16_t)(((int32_t)(int8_t)(uint8_t)pm * (int32_t)pmd) >> 7);
    }
}

/* ---- Rendering ------------------------------------------------------------ */
static FM4_AUDIO_CODE void clock_env(opz_op *o, uint32_t counter)
{
    if (o->state == EG_ATTACK && o->env == 0) o->state = EG_DECAY;
    if (o->state == EG_DECAY && o->env >= o->sustain) o->state = EG_SUSTAIN;
    if (o->state == EG_RELEASE && o->env >= 0xc0) o->state = EG_REVERB;
    unsigned rate = o->rate[o->state];
    unsigned shift = rate >> 2;
    uint32_t c = counter << shift;
    if (c & 0x7ff) return;
    unsigned bits = (c >> (shift <= 11 ? 11 : shift)) & 7;
    unsigned inc = (opz_eg_increment[rate] >> (4 * bits)) & 15;
    int32_t env = o->env;
    if (o->state == EG_ATTACK) {
        if (rate < 62) env += (-(env + 1) * (int32_t)inc) >> 4;
        if (env < 0) env = 0;
    } else {
        env += (int32_t)inc;
        if (env > 0x3ff) env = 0x3ff;
    }
    o->env = (uint16_t)env;
}

static FM4_AUDIO_CODE int32_t op_out(const opz_op *o, uint32_t phase10, uint32_t am)
{
    if (o->env > OPZ_EG_QUIET) return 0;
    uint32_t sa = opz_wave[o->wave][phase10 & 1023];
    uint32_t att = (uint32_t)(o->env >> o->shift) + (o->ame ? am : 0) + o->tl;
    if (att > 0x3ff) att = 0x3ff;
    uint32_t total = (sa & 0x7fff) + (att << 2);
    int32_t vol = (int32_t)(opz_power_table[total & 0xff] >> (total >> 8));
    return (sa & 0x8000) ? -vol : vol;
}

/* Algorithm routing, ymfm order: O1 = OP4 (feedback) .. O4 = OP1.
 * inputs: 0 none, 1 O1, 2 O2, 3 O3, 5 O1+O2, 6 O1+O3, 7 O2+O3
 * fields: O2 in, O3 in, O4 in, O1 out, O2 out, O3 out (O4 always out). */
static const uint8_t alg_table[8][6] FM4_AUDIO_DATA = {
    {1,2,3, 0,0,0}, {0,5,3, 0,0,0}, {0,2,6, 0,0,0}, {1,0,7, 0,0,0},
    {1,0,3, 0,1,0}, {1,1,1, 0,1,1}, {1,0,0, 0,1,1}, {0,0,0, 1,1,1},
};

FM4_AUDIO_CODE void opz_render(opz_engine *e, int32_t *dry, int32_t *send)
{
    /* Envelope clock schedule for this block. */
    uint32_t ticks[FM4_BLOCK];
    for (unsigned s = 0; s < FM4_BLOCK; ++s) {
        e->eg_acc += OPZ_EG_INC;
        if (e->eg_acc >= 65536U) { e->eg_acc -= 65536U; ticks[s] = ++e->eg_counter ? e->eg_counter : ++e->eg_counter; }
        else ticks[s] = 0;
    }
    lfo_block(e);
    for (unsigned slot = 0; slot < e->voices; ++slot) {
        opz_voice *v = &e->v[slot];
        if (!v->active) continue;
        voice_pitch(e, slot);
        const opz_lfo *l = &e->lfo[v->part & (OPZ_LFOS - 1)];
        uint32_t am = v->ams ? ((uint32_t)l->am_raw << (v->ams - 1)) : 0;
        const uint8_t *a = alg_table[v->alg];
        opz_op *o1 = &v->o[0], *o2 = &v->o[1], *o3 = &v->o[2], *o4 = &v->o[3];
        int32_t gl = v->gain_l, gr = v->gain_r, sl = v->send_l;
        for (unsigned s = 0; s < FM4_BLOCK; ++s) {
            if (ticks[s]) {
                clock_env(o1, ticks[s]); clock_env(o2, ticks[s]);
                clock_env(o3, ticks[s]); clock_env(o4, ticks[s]);
            }
            int32_t out[8];
            int32_t mod = v->fbl ? (v->fb[0] + v->fb[1]) >> (10 - v->fbl) : 0;
            out[0] = 0;
            out[1] = op_out(o1, (o1->phase >> 22) + (uint32_t)mod, am);
            v->fb[0] = v->fb[1]; v->fb[1] = out[1];
            out[2] = op_out(o2, (o2->phase >> 22) + (uint32_t)(out[a[0]] >> 1), am);
            out[5] = out[1] + out[2];
            out[3] = op_out(o3, (o3->phase >> 22) + (uint32_t)(out[a[1]] >> 1), am);
            out[6] = out[1] + out[3]; out[7] = out[2] + out[3];
            int32_t r = op_out(o4, (o4->phase >> 22) + (uint32_t)(out[a[2]] >> 1), am);
            if (a[3]) r += out[1];
            if (a[4]) r += out[2];
            if (a[5]) r += out[3];
            o1->phase += o1->inc; o2->phase += o2->inc; o3->phase += o3->inc; o4->phase += o4->inc;
            int32_t L = (r * gl) >> 7, R = (r * gr) >> 7;
            dry[2 * s] += L; dry[2 * s + 1] += R;
            if (sl && send) {
                send[2 * s] += ((L >> 8) * sl) >> 7;
                send[2 * s + 1] += ((R >> 8) * sl) >> 7;
            }
        }
        if (!v->keyed) {
            /* Voice ends when every operator is quiet after release. */
            int alive = 0;
            for (unsigned i = 0; i < 4; ++i)
                if (v->o[i].env <= OPZ_EG_QUIET &&
                    (unsigned)(v->o[i].env >> v->o[i].shift) + v->o[i].tl < OPZ_EG_QUIET) alive = 1;
            if (!alive) v->active = 0;
        }
    }
}

unsigned opz_ratio_x100(unsigned crs, unsigned fine)
{
    static const uint16_t dt2_x1000[4] = { 1000, 1414, 1569, 1730 };
    unsigned mul = crs_map[crs & 63][0], dt2 = crs_map[crs & 63][1];
    if ((crs & 63) < 4 && (fine & 15) > 7) fine = 7;
    unsigned m16 = (mul ? mul << 4 : 8U) + (fine & 15);
    return (m16 * dt2_x1000[dt2] + 80U) / 160U;
}

unsigned opz_fixed_hz(unsigned crs, unsigned fixrg, unsigned fine)
{
    unsigned base = (crs & 63) < 4 ? 8U + ((fine & 15) > 7 ? 7U : (fine & 15))
                                    : 16U * ((crs & 63) >> 2) + (fine & 15);
    return base << (fixrg & 7);
}
