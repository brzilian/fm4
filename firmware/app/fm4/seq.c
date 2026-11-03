/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 song sequencer. See seq.h. */
#include "seq.h"
#include <string.h>

#define TICK_MASK 0x00ffffffU
#define EV_TICK(e) ((e)->tick & TICK_MASK)
#define EV_TRACK(e) (((e)->tick >> 24) & 7U)

static const uint8_t quant_grid[7] = { 1, 96, 48, 32, 24, 16, 12 };

void seq_song_clear(seq_song *song, unsigned number)
{
    memset(song, 0, sizeof *song);
    memcpy(song->name, "SONG     ", 10);
    song->name[4] = (char)('1' + (number % 9));
    song->tempo = 1200;
    song->measures = 8;
    song->loop = 1;
    song->quantize = 4;
    for (unsigned t = 0; t < SEQ_TRACKS; ++t) song->track_part[t] = (uint8_t)t;
}

/* Also called by advance() in the audio interrupt: must be SRAM code. */
FM4_AUDIO_CODE uint32_t seq_song_ticks(const seq_song *song)
{
    unsigned m = song->measures ? song->measures : 1;
    return (uint32_t)m * 4U * SEQ_PPQ;
}

void seq_init(seq_state *s, seq_song *song, seq_emit_fn emit, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->song = song; s->emit = emit; s->ctx = ctx;
}

static FM4_AUDIO_CODE void note_track(seq_state *s, unsigned track, uint8_t status, uint8_t note, uint8_t vel)
{
    uint8_t *h = s->held[track];
    unsigned kind = status & 0xf0;
    if (kind == 0x90 && vel) h[note >> 3] |= (uint8_t)(1U << (note & 7));
    else if (kind == 0x80 || kind == 0x90) h[note >> 3] &= (uint8_t)~(1U << (note & 7));
}

static FM4_AUDIO_CODE void release_track(seq_state *s, unsigned track)
{
    unsigned part = s->song->track_part[track] & 7;
    for (unsigned n = 0; n < 128; ++n)
        if (s->held[track][n >> 3] & (1U << (n & 7)))
            s->emit(s->ctx, track, part, (uint8_t)(0x80 | part), (uint8_t)n, 0);
    fm4_zero(s->held[track], sizeof s->held[track]);
}

static FM4_AUDIO_CODE void release_all(seq_state *s)
{
    for (unsigned t = 0; t < SEQ_TRACKS; ++t) release_track(s, t);
}

static FM4_AUDIO_CODE void locate_cursor(seq_state *s)
{
    unsigned lo = 0, hi = s->song->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1;
        if (EV_TICK(&s->song->ev[mid]) < s->tick) lo = mid + 1; else hi = mid;
    }
    s->cursor = (uint16_t)lo;
}

void seq_locate(seq_state *s, uint32_t tick)
{
    s->tick = tick % seq_song_ticks(s->song);
    s->frac = 0;
    locate_cursor(s);
}

void seq_play(seq_state *s)
{
    if (s->state != SEQ_STOPPED) return;
    s->frac = 0;
    locate_cursor(s);
    s->clock_pulses = 0;
    s->state = SEQ_PLAYING;
}

void seq_record(seq_state *s, unsigned track)
{
    s->rec_track = (uint8_t)(track & 7);
    s->rec_count = 0;
    if (s->state == SEQ_STOPPED) { s->frac = 0; locate_cursor(s); }
    s->state = SEQ_RECORDING;
}

void seq_record_countin(seq_state *s, unsigned track, unsigned beats)
{
    if (!beats) { seq_record(s, track); return; }
    s->rec_track = (uint8_t)(track & 7);
    s->rec_count = 0;
    s->frac = 0;
    locate_cursor(s);
    s->clock_pulses = 0;
    s->countin = (uint32_t)beats * SEQ_PPQ;
    s->state = SEQ_COUNTIN;
}

uint32_t seq_step_ticks(const seq_song *song)
{
    unsigned g = quant_grid[song->quantize < 7 ? song->quantize : 0];
    return g > 1 ? g : SEQ_PPQ / 4U;
}

/* index of the first event at tick >= t (lower) or > t (upper) */
static unsigned bound(const seq_song *g, uint32_t t, int upper)
{
    unsigned lo = 0, hi = g->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1, e = EV_TICK(&g->ev[mid]);
        if (upper ? e <= t : e < t) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void insert_at(seq_song *g, unsigned at, uint32_t tick, unsigned track, uint8_t st, uint8_t d1, uint8_t d2)
{
    memmove(&g->ev[at + 1], &g->ev[at], (g->count - at) * sizeof g->ev[0]);
    g->ev[at].tick = (tick & TICK_MASK) | ((uint32_t)(track & 7) << 24);
    g->ev[at].status = st; g->ev[at].d1 = d1; g->ev[at].d2 = d2; g->ev[at].pad = 0;
    ++g->count;
}

int seq_step_insert(seq_state *s, uint32_t tick, unsigned track, uint8_t note, uint8_t vel, uint32_t len)
{
    seq_song *g = s->song;
    uint32_t length = seq_song_ticks(g);
    if (g->count + 2 > SEQ_MAX_EVENTS || !len) return -1;
    tick %= length;
    uint32_t off = tick + len;
    if (off >= length) off = length - 1;          /* the note ends at the song end */
    if (off <= tick) off = tick + 1 < length ? tick + 1 : tick;
    unsigned part = g->track_part[track & 7] & 7;
    /* note-on after anything already at its tick; note-off before, so a note
     * that ends exactly where the next one starts is released first */
    insert_at(g, bound(g, tick, 1), tick, track, (uint8_t)(0x90 | part), note & 127, vel ? vel : 1);
    insert_at(g, bound(g, off, 0), off, track, (uint8_t)(0x80 | part), note & 127, 0);
    locate_cursor(s);
    return 0;
}

void seq_stop(seq_state *s)
{
    if (s->state == SEQ_STOPPED) return;
    s->state = SEQ_STOPPED;
    release_all(s);
}

/* Emit events in [s->tick, end) then set s->tick = end. */
static FM4_AUDIO_CODE void play_range(seq_state *s, uint32_t end)
{
    seq_song *g = s->song;
    while (s->cursor < g->count && EV_TICK(&g->ev[s->cursor]) < end) {
        const seq_event *e = &g->ev[s->cursor++];
        unsigned track = EV_TRACK(e);
        if (g->mute & (1U << track)) continue;
        unsigned part = g->track_part[track] & 7;
        uint8_t status = (uint8_t)((e->status & 0xf0) | part);
        if ((status & 0xf0) <= 0x90) note_track(s, track, status, e->d1, e->d2);
        s->emit(s->ctx, track, part, status, e->d1, e->d2);
    }
    s->tick = end;
}

static FM4_AUDIO_CODE void metronome(seq_state *s, uint32_t from, uint32_t to)
{
    uint32_t next = (from + SEQ_PPQ - 1) / SEQ_PPQ * SEQ_PPQ;
    if (next < to) {
        unsigned down = (next % (4U * SEQ_PPQ)) == 0;
        s->beat_flash = (uint8_t)(down ? 2 : 1);
        if (s->metronome || s->state == SEQ_RECORDING)
            s->emit(s->ctx, SEQ_TRACKS, 0, 0x99, (uint8_t)(down ? 77 : 76), (uint8_t)(down ? 110 : 80));
    }
}

static FM4_AUDIO_CODE void advance(seq_state *s, uint32_t ticks)
{
    uint32_t length = seq_song_ticks(s->song);
    while (ticks && s->state == SEQ_COUNTIN) {
        /* count-in: clicks on every beat, the song position stays put */
        if (s->countin % SEQ_PPQ == 0) {
            unsigned down = s->countin == 4U * SEQ_PPQ;
            s->beat_flash = (uint8_t)(down ? 2 : 1);
            s->emit(s->ctx, SEQ_TRACKS, 0, 0x99, (uint8_t)(down ? 77 : 76), (uint8_t)(down ? 110 : 80));
        }
        uint32_t step = s->countin % SEQ_PPQ ? s->countin % SEQ_PPQ : SEQ_PPQ;
        if (step > ticks) step = ticks;
        s->countin -= step; ticks -= step;
        if (!s->countin) s->state = SEQ_RECORDING;   /* the recording's first beat clicks in metronome() */
    }
    while (ticks && s->state != SEQ_STOPPED) {
        uint32_t room = length - s->tick, step = ticks < room ? ticks : room;
        uint32_t from = s->tick;
        play_range(s, s->tick + step);
        metronome(s, from, s->tick);
        ticks -= step;
        if (s->tick >= length) {
            if (!s->song->loop && s->state == SEQ_PLAYING) {
                s->state = SEQ_STOPPED; s->tick = 0; s->cursor = 0;
                release_all(s);
                return;
            }
            s->tick = 0; s->cursor = 0;
        }
    }
}

FM4_AUDIO_CODE void seq_block(seq_state *s, unsigned frames)
{
    if (s->state == SEQ_STOPPED || s->ext_clock) return;
    /* ticks (Q16) = tempo10 * frames * 96 / (600 * 44100) */
    s->frac += (((uint32_t)s->song->tempo * frames) >> 4) * 15583U >> 12;
    uint32_t whole = s->frac >> 16;
    s->frac &= 0xffff;
    if (whole) advance(s, whole);
}

FM4_AUDIO_CODE void seq_midi_clock(seq_state *s)
{
    if (s->state == SEQ_STOPPED || !s->ext_clock) return;
    ++s->clock_pulses;
    advance(s, SEQ_PPQ / 24U);
}

FM4_AUDIO_CODE void seq_capture(seq_state *s, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (s->state != SEQ_RECORDING || s->rec_count >= SEQ_REC_EVENTS) return;
    unsigned kind = status & 0xf0;
    if (kind < 0x80 || kind > 0xe0) return;
    seq_event *e = &s->rec[s->rec_count++];
    e->tick = (s->tick & TICK_MASK) | ((uint32_t)s->rec_track << 24);
    e->status = status; e->d1 = d1; e->d2 = d2; e->pad = 0;
    note_track(s, s->rec_track, status, d1, d2);
}

static void sort_events(seq_event *ev, unsigned count)
{
    for (unsigned i = 1; i < count; ++i) {
        seq_event x = ev[i];
        unsigned j = i;
        while (j && EV_TICK(&ev[j - 1]) > EV_TICK(&x)) { ev[j] = ev[j - 1]; --j; }
        ev[j] = x;
    }
}

unsigned seq_merge(seq_state *s)
{
    seq_song *g = s->song;
    unsigned n = s->rec_count, grid = quant_grid[g->quantize < 7 ? g->quantize : 0];
    uint32_t length = seq_song_ticks(g);
    int32_t shift[128];
    memset(shift, 0, sizeof shift);
    for (unsigned i = 0; i < n; ++i) {
        seq_event *e = &s->rec[i];
        uint32_t t = EV_TICK(e), track = e->tick & ~TICK_MASK;
        unsigned kind = e->status & 0xf0;
        if (grid > 1 && kind == 0x90 && e->d2) {
            uint32_t q = (t + grid / 2) / grid * grid;
            shift[e->d1 & 127] = (int32_t)q - (int32_t)t;
            t = q;
        } else if (grid > 1 && (kind == 0x80 || kind == 0x90)) {
            int32_t q = (int32_t)t + shift[e->d1 & 127];
            t = q < 0 ? 0 : (uint32_t)q;
        }
        if (t >= length) t -= length;
        e->tick = t | track;
    }
    sort_events(s->rec, n);
    /* Merge from the back so the song array is filled in place. */
    if (g->count + n > SEQ_MAX_EVENTS) n = SEQ_MAX_EVENTS - g->count;
    int i = (int)g->count - 1, j = (int)n - 1, k = (int)(g->count + n) - 1;
    while (j >= 0) {
        if (i >= 0 && EV_TICK(&g->ev[i]) > EV_TICK(&s->rec[j])) g->ev[k--] = g->ev[i--];
        else g->ev[k--] = s->rec[j--];
    }
    g->count = (uint16_t)(g->count + n);
    s->rec_count = 0;
    locate_cursor(s);
    return n;
}

void seq_clear_track(seq_song *song, unsigned track)
{
    unsigned w = 0;
    for (unsigned r = 0; r < song->count; ++r)
        if (EV_TRACK(&song->ev[r]) != (track & 7)) song->ev[w++] = song->ev[r];
    song->count = (uint16_t)w;
}
