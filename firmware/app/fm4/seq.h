/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4: TQ5-style 8-track song sequencer.
 *
 * Model: 8 songs, each with 8 tracks. A song is one time-sorted event list
 * (tick + track + MIDI message), 96 PPQ, 4/4, 1..99 measures, looped or
 * one-shot. Playback runs in the audio context once per block and emits
 * events through a callback. Recording collects live events in a separate
 * append-only buffer that the control task merges (seq_merge) when a take
 * ends, so the audio context never moves memory. */
#ifndef FM4_SEQ_H
#define FM4_SEQ_H
#include "fm4_port.h"

#define SEQ_TRACKS 8
#define SEQ_SONGS 8
#define SEQ_PPQ 96
#define SEQ_MAX_EVENTS 4096   /* per song (~2000 notes); 8 songs in Flash */
#define SEQ_REC_EVENTS 1024

typedef struct {
    uint32_t tick;      /* bits 0..23 tick, 24..26 track */
    uint8_t status, d1, d2, pad;
} seq_event;

typedef struct {
    char name[10];
    uint16_t tempo;     /* BPM x10, 300..3000 */
    uint8_t measures;   /* 1..99 */
    uint8_t loop;       /* 1 = loop at end */
    uint8_t mute;       /* track mute bit mask */
    uint8_t quantize;   /* 0 off, 1 1/4, 2 1/8, 3 1/8T, 4 1/16, 5 1/16T, 6 1/32 */
    uint8_t track_part[SEQ_TRACKS]; /* destination part 0..7 */
    uint16_t count;
    seq_event ev[SEQ_MAX_EVENTS];
} seq_song;

enum { SEQ_STOPPED, SEQ_PLAYING, SEQ_RECORDING, SEQ_COUNTIN };

typedef void (*seq_emit_fn)(void *ctx, unsigned track, unsigned part,
                            uint8_t status, uint8_t d1, uint8_t d2);

typedef struct {
    seq_song *song;
    uint8_t state, rec_track, metronome, ext_clock;
    uint32_t tick;              /* current position */
    uint32_t frac;              /* sub-tick accumulator (Q16) */
    uint16_t cursor;            /* next event index */
    uint32_t rec_count;
    seq_event rec[SEQ_REC_EVENTS];
    uint8_t held[SEQ_TRACKS][16]; /* notes sounding per track, bit set */
    uint32_t clock_pulses;      /* external MIDI clock F8 received */
    uint8_t beat_flash;
    seq_emit_fn emit;
    void *ctx;
} seq_state;

void seq_init(seq_state *s, seq_song *song, seq_emit_fn emit, void *ctx);
void seq_song_clear(seq_song *song, unsigned number);
uint32_t seq_song_ticks(const seq_song *song);
/* Control task. */
void seq_play(seq_state *s);
void seq_record(seq_state *s, unsigned track);
void seq_stop(seq_state *s);           /* also releases sounding notes */
void seq_locate(seq_state *s, uint32_t tick);
/* Fold the recorded take into the song (quantized). Call from the control
 * task with rendering paused, after seq_stop or at a loop boundary. */
unsigned seq_merge(seq_state *s);
void seq_clear_track(seq_song *song, unsigned track);
/* Audio context: advance by one block, emitting due events. */
FM4_AUDIO_CODE void seq_block(seq_state *s, unsigned frames);
/* Audio context: a live event while recording. */
FM4_AUDIO_CODE void seq_capture(seq_state *s, uint8_t status, uint8_t d1, uint8_t d2);
/* External clock: 24 PPQ F8 pulses. */
FM4_AUDIO_CODE void seq_midi_clock(seq_state *s);
#endif
