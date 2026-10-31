/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 effects: building blocks for the TQ5-style effect programs.
 * Input is the stereo send bus, output is added to the dry mix. Q23 audio,
 * 16-bit delay memory, integer only. */
#ifndef FM4_FX_H
#define FM4_FX_H
#include "fm4_port.h"

#ifndef FX_DELAY_SAMPLES
#define FX_DELAY_SAMPLES 16384   /* per channel, ~371 ms */
#endif
#define FX_DELAY_R_SAMPLES 8192 /* right line: ping-pong only, ~186 ms */
#define FX_REVERB_SAMPLES 8192

enum fx_algo {
    FX_ALGO_OFF,
    FX_ALGO_REVERB,      /* size/decay from time */
    FX_ALGO_DELAY,       /* mono echo with feedback */
    FX_ALGO_PINGPONG,    /* stereo cross-feedback echo */
    FX_ALGO_CHORUS,
    FX_ALGO_DIST,
    FX_ALGO_DIST_REVERB,
    FX_ALGO_DIST_DELAY,
    FX_ALGO_DELAY_LR,    /* one repeat per side, different times */
    FX_ALGO_GATE,        /* gated reverb: flat dense tail cut at gate time */
    FX_ALGO_REVERSE,     /* reverse gate: rising dense tail */
    FX_ALGOS
};

typedef struct {
    uint8_t algo;        /* enum fx_algo */
    uint8_t time;        /* 0..99 meaning depends on algo */
    uint8_t balance;     /* 0..99 wet/dry of the send return */
    uint8_t character;   /* 0..99 secondary: room size / feedback / drive */
} fx_params;

typedef struct {
    fx_params p;
    int16_t dl[FX_DELAY_SAMPLES], dr[FX_DELAY_R_SAMPLES];
    int16_t rv[FX_REVERB_SAMPLES];
    uint32_t dpos, rpos, lfo;
    int32_t lp[8], apos[8];
    int32_t hp_l, hp_r, dc_l, dc_r;
    int32_t delay_len, feedback, wet, rv_feedback, drive;
} fx_unit;

void fx_init(fx_unit *f);
/* Control task; render gate must be held by caller if algo changes. */
void fx_set(fx_unit *f, const fx_params *p);
FM4_AUDIO_CODE void fx_render(fx_unit *f, const int32_t *send, int32_t *mix);
#endif
