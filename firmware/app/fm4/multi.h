/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 multitimbral layer: 8 parts over the shared OPZ voice pool.
 * Handles MIDI channel messages, voice reserve/stealing, mono/poly,
 * portamento, sustain, pitch bend and modulation sources per part. */
#ifndef FM4_MULTI_H
#define FM4_MULTI_H
#include "opz.h"

#define MULTI_PARTS 8
#define MULTI_OMNI 16
#define MULTI_OFF 17

/* Per-voice performance data that travels with a TQ5 voice (VCED 62..76). */
typedef struct {
    uint8_t poly_mono;     /* 0 poly, 1 mono */
    uint8_t pb_range;      /* 0..12 semitones */
    uint8_t porta_mode;    /* 0 full time, 1 fingered (mono) / retain */
    uint8_t porta_time;    /* 0..99 */
    uint8_t mw_pitch, mw_amp; /* 0..99 */
    uint8_t bc_pitch, bc_amp, bc_pbias, bc_ebias;
    uint8_t fc_volume, fc_pitch, fc_amp;
    uint8_t at_pitch, at_amp;      /* aftertouch (ACED2) */
    uint8_t transpose;     /* 0..48, 24 = no shift */
} multi_perf;

typedef struct {
    opz_patch patch;
    multi_perf perf;
    uint8_t effect[3];     /* EFEDS travelling with this voice */
    char name[11];
} multi_voice_data;

typedef struct {
    uint8_t enabled;
    uint8_t channel;       /* 0..15, 16 omni, 17 off */
    uint8_t reserve;       /* guaranteed voices 0..16 */
    uint8_t voice;         /* program number 0..199 (preset+user) for UI */
    uint8_t note_lo, note_hi;
    int8_t detune;         /* -7..+7 (1/64 semitone x2) */
    int8_t note_shift;     /* -24..+24 */
    uint8_t volume;        /* 0..99 */
    uint8_t pan;           /* 0..127, 64 centre */
    uint8_t fx_send;       /* 0..99 */
    uint8_t lfo_select;    /* reserved */
} multi_part_cfg;

typedef struct {
    multi_voice_data data;           /* current (edit buffer) voice */
    multi_part_cfg cfg;
    int16_t bend;                    /* -8192..8191 */
    uint8_t modwheel, breath, foot, aftertouch, sustain_pedal, expression, cc_volume;
    uint8_t held[16];                /* keys physically down */
    uint8_t sustained[16];           /* released under sustain */
    uint8_t mono_stack[8], mono_count;
    uint16_t porta_from64, porta_to64, porta_pos;
    uint8_t last_note, porta_active;
} multi_part;

typedef struct {
    opz_engine eng;
    multi_part part[MULTI_PARTS];
    uint8_t slot_part[OPZ_MAX_VOICES];
    uint8_t slot_note[OPZ_MAX_VOICES];
    uint8_t slot_sustained[OPZ_MAX_VOICES];
    uint8_t master_volume;           /* 0..127 */
    int8_t master_tune;              /* -64..+63 in ~1.2 cent steps */
    uint8_t single_mode;             /* 1: part 0 only, omni-ish TQ5 single */
} multi_state;

void multi_init(multi_state *m, unsigned polyphony);
/* Control-side: load a voice into a part's edit buffer (render gate held). */
void multi_set_voice(multi_state *m, unsigned part, const multi_voice_data *v);
/* Audio context: route one complete channel message (status + data). */
FM4_AUDIO_CODE void multi_midi(multi_state *m, const uint8_t *msg, unsigned len);
/* Audio context: play a note directly on a part (panel keys, sequencer). */
FM4_AUDIO_CODE void multi_note(multi_state *m, unsigned part, unsigned note, unsigned velocity);
FM4_AUDIO_CODE void multi_all_off(multi_state *m, int hard);
FM4_AUDIO_CODE void multi_render(multi_state *m, int32_t *dry, int32_t *send);
/* Which parts listen on channel ch (bit mask). */
FM4_AUDIO_CODE unsigned multi_parts_for_channel(const multi_state *m, unsigned ch);
unsigned multi_active_voices(const multi_state *m);
#endif
