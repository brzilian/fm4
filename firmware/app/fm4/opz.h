/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4: YM2414 (OPZ)-style 4-operator FM voice engine.
 *
 * Chip-level behaviour (log-sine/exp tables, envelope increments, key code to
 * phase step, detune) follows ymfm by Aaron Giles (BSD-3-Clause); see
 * opz_ymfm_tables.c. The engine runs directly at 44.1 kHz: phase steps and the
 * envelope clock are rescaled from the chip's 55.93 kHz so pitch and timing
 * match the original. Integer only; everything called from render is
 * FM4_AUDIO_CODE so it can run from SRAM during Flash writes. */
#ifndef FM4_OPZ_H
#define FM4_OPZ_H
#include "fm4_port.h"

#define OPZ_MAX_VOICES 16
#define OPZ_LFOS 8              /* one per multitimbral part */
#define OPZ_EG_QUIET 0x380

/* Operator parameters in TX81Z/TQ5 terms. Index 0..3 = OP1..OP4. */
typedef struct {
    uint8_t ar, d1r, d2r, rr, d1l;  /* 0-31,0-31,0-31,1-15,0-15 */
    uint8_t ls, rs, ame, kvs, out;  /* 0-99,0-3,0-1,0-7,0-99 */
    uint8_t crs, det;               /* 0-63, 0-6 (3 = centre) */
    uint8_t fixed, fixrg, fine;     /* 0-1, 0-7, 0-15 */
    uint8_t wave, shft;             /* 0-7, 0-3 */
    uint8_t on;                     /* operator enabled */
} opz_op_params;

typedef struct {
    opz_op_params op[4];
    uint8_t alg, fb;                /* 0-7, 0-7 */
    uint8_t lfs, lfd, pmd, amd, sync, lfw, pms, ams; /* LFO */
    uint8_t rev;                    /* 0-7 EG "reverb" rate */
    int8_t transpose;               /* semitones, applied by caller */
} opz_patch;

/* Per-part LFO (the TQ5 keeps one LFO per sounding voice program). */
typedef struct {
    uint32_t counter, delay_count;
    uint8_t rate_reg, wave, pmd, amd, sync;
    uint8_t sh_value, last_index;
    uint32_t noise;
    int16_t pm_raw;   /* -128..127 after depth */
    uint16_t am_raw;  /* 0..255 after depth */
} opz_lfo;

typedef struct {
    uint32_t phase, inc, fixed_inc;
    uint16_t env, sustain, tl;
    uint8_t state, shift, wave, ame, fixed, multiple, det1;
    uint8_t rate[5];
} opz_op;

typedef struct {
    opz_op o[4];            /* chip order: o[0]=OP4 (feedback) .. o[3]=OP1 */
    int32_t fb[2];
    uint8_t alg, fbl, active, keyed, part, note, pms, ams, lfo_delay_done;
    uint16_t pitch64;       /* note pitch in 1/64 semitone from MIDI 0 */
    int16_t bend64;         /* bend + fine tuning in 1/64 semitone */
    int32_t gain_l, gain_r; /* Q15 */
    int32_t send_l, send_r; /* Q15 effect send */
    uint32_t age;
    uint8_t rate_ksr;       /* keycode used for rate scaling */
} opz_voice;

typedef struct {
    opz_voice v[OPZ_MAX_VOICES];
    opz_lfo lfo[OPZ_LFOS];
    unsigned voices;        /* active polyphony limit */
    uint32_t eg_acc, eg_counter, age;
} opz_engine;

void opz_init(opz_engine *e, unsigned polyphony);
/* Start a note on a voice slot. velocity 1..127. */
FM4_AUDIO_CODE void opz_key_on(opz_engine *e, unsigned slot, const opz_patch *p,
                               unsigned part, unsigned note, unsigned velocity,
                               int32_t gain_l, int32_t gain_r, int32_t send);
FM4_AUDIO_CODE void opz_key_off(opz_engine *e, unsigned slot);
FM4_AUDIO_CODE void opz_kill(opz_engine *e, unsigned slot);
/* Release with a very fast (~10 ms, click-free) envelope: voice shedding. */
FM4_AUDIO_CODE void opz_fast_release(opz_engine *e, unsigned slot);
/* Legato pitch change (mono / portamento) without retriggering envelopes. */
FM4_AUDIO_CODE void opz_set_pitch(opz_engine *e, unsigned slot, unsigned pitch64);
FM4_AUDIO_CODE void opz_set_bend(opz_engine *e, unsigned slot, int bend64);
FM4_AUDIO_CODE void opz_lfo_setup(opz_engine *e, unsigned part, const opz_patch *p);
FM4_AUDIO_CODE void opz_lfo_keyon(opz_engine *e, unsigned part);
/* Extra LFO depth (mod wheel etc.) in 0..127 added to PMD/AMD. */
FM4_AUDIO_CODE void opz_lfo_extra(opz_engine *e, unsigned part, unsigned pm, unsigned am);
/* Render FM4_BLOCK frames. dry and send are interleaved stereo, accumulated
 * (caller clears), in 24-bit (Q23) scale. */
FM4_AUDIO_CODE void opz_render(opz_engine *e, int32_t *dry, int32_t *send);

/* Utility for the UI/voice allocator. */
FM4_AUDIO_CODE int opz_voice_busy(const opz_engine *e, unsigned slot);
/* Coarse ratio of CRS value as x100 (for display), e.g. 141 for 1.41. */
unsigned opz_ratio_x100(unsigned crs, unsigned fine);
/* Fixed frequency in Hz for display. */
FM4_AUDIO_CODE unsigned opz_fixed_hz(unsigned crs, unsigned fixrg, unsigned fine);
#endif
