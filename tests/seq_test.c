/* Host tests for the FM-4 song sequencer. */
#include "seq.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct { unsigned track, part; uint8_t st, d1, d2; uint32_t at; } got[256];
static unsigned ngot; static uint32_t now_block;
static void emit(void *ctx, unsigned track, unsigned part, uint8_t st, uint8_t d1, uint8_t d2)
{
    (void)ctx;
    if (track >= SEQ_TRACKS) return; /* metronome */
    if (ngot < 256) { got[ngot].track = track; got[ngot].part = part; got[ngot].st = st;
        got[ngot].d1 = d1; got[ngot].d2 = d2; got[ngot].at = now_block; ++ngot; }
}

static seq_song song;
static seq_state s;

int main(void)
{
    seq_song_clear(&song, 0);
    song.measures = 1; song.tempo = 1200; song.quantize = 4;
    seq_init(&s, &song, emit, NULL);

    /* Record two notes on track 2 slightly off the 1/16 grid. */
    seq_record(&s, 2);
    s.tick = 26; seq_capture(&s, 0x90, 60, 100);
    s.tick = 70; seq_capture(&s, 0x80, 60, 0);
    s.tick = 190; seq_capture(&s, 0x90, 64, 90);
    s.tick = 200; seq_capture(&s, 0x80, 64, 0);
    seq_stop(&s);
    unsigned merged = seq_merge(&s);
    CHECK(merged == 4, "merged %u", merged);
    CHECK((song.ev[0].tick & 0xffffff) == 24, "quantized on %u", song.ev[0].tick & 0xffffff);
    CHECK((song.ev[1].tick & 0xffffff) == 68, "shifted off %u", song.ev[1].tick & 0xffffff);
    CHECK((song.ev[2].tick & 0xffffff) == 192, "quantized on2 %u", song.ev[2].tick & 0xffffff);

    /* Play one loop: 1 measure at 120 BPM = 2 s = ~689 blocks. */
    song.track_part[2] = 5;
    ngot = 0; seq_locate(&s, 0); seq_play(&s);
    for (now_block = 0; now_block < 700; ++now_block) seq_block(&s, FM4_BLOCK);
    CHECK(ngot >= 4, "events played %u", ngot);
    CHECK(got[0].part == 5 && got[0].st == 0x95 && got[0].d1 == 60, "routed to part 5: %02x", got[0].st);
    /* tick 24 at 120 BPM = 0.125 s = 43 blocks */
    CHECK(got[0].at >= 42 && got[0].at <= 44, "timing block %u", got[0].at);
    /* Loop wrapped: first note again around block 689+43 */
    seq_stop(&s);

    /* Mute and clear. */
    song.mute = 1 << 2; ngot = 0; seq_locate(&s, 0); seq_play(&s);
    for (now_block = 0; now_block < 400; ++now_block) seq_block(&s, FM4_BLOCK);
    CHECK(ngot == 0, "muted track emitted %u", ngot);
    seq_stop(&s);
    seq_clear_track(&song, 2);
    CHECK(song.count == 0, "cleared count %u", song.count);

    /* Stop releases hanging notes. */
    song.mute = 0; song.ev[0].tick = 0 | (1U << 24); song.ev[0].status = 0x90; song.ev[0].d1 = 50; song.ev[0].d2 = 99;
    song.count = 1; ngot = 0; seq_locate(&s, 0); seq_play(&s);
    for (int b = 0; b < 4; ++b) seq_block(&s, FM4_BLOCK);
    seq_stop(&s);
    CHECK(ngot == 2 && (got[1].st & 0xf0) == 0x80 && got[1].d1 == 50, "note off on stop (%u)", ngot);

    /* External clock: 24 pulses = one beat = 96 ticks. */
    s.ext_clock = 1; seq_locate(&s, 0); seq_play(&s);
    for (int i = 0; i < 24; ++i) seq_midi_clock(&s);
    CHECK(s.tick == 96, "ext clock tick %u", s.tick);

    /* Count-in: 4 beats of clicks, position held, then recording. */
    s.ext_clock = 0; seq_stop(&s);
    seq_song_clear(&song, 0); song.measures = 2;
    seq_locate(&s, 96);
    seq_record_countin(&s, 1, 4);
    CHECK(s.state == SEQ_COUNTIN && s.countin == 4 * SEQ_PPQ, "count-in armed");
    unsigned blocks = 0;
    while (s.state == SEQ_COUNTIN && blocks < 2000) { seq_block(&s, FM4_BLOCK); ++blocks; }
    /* 4 beats at 120 BPM = 2 s = 689 blocks of 128 frames */
    CHECK(blocks >= 685 && blocks <= 692, "count-in lasts 4 beats (%u blocks)", blocks);
    CHECK(s.state == SEQ_RECORDING && s.tick >= 96 && s.tick < 96 + 4, "recording starts at the playhead (%u)", s.tick);
    seq_stop(&s); s.rec_count = 0;

    /* Step entry: sorted, off before on at the same tick, clipped at the song end. */
    seq_song_clear(&song, 0); song.measures = 1; song.quantize = 4;
    CHECK(seq_step_ticks(&song) == 24, "1/16 step");
    song.quantize = 0; CHECK(seq_step_ticks(&song) == 24, "quantize off steps 1/16");
    song.quantize = 2; CHECK(seq_step_ticks(&song) == 48, "1/8 step");
    song.quantize = 4; song.track_part[3] = 5;
    CHECK(!seq_step_insert(&s, 24, 3, 62, 90, 24), "insert 2");
    CHECK(!seq_step_insert(&s, 0, 3, 60, 100, 24), "insert 1");
    CHECK(!seq_step_insert(&s, 24, 3, 60, 80, 24), "insert repeat");
    CHECK(!seq_step_insert(&s, 372, 3, 67, 80, 24), "insert at the end");
    CHECK(song.count == 8, "8 events (%u)", song.count);
    int sorted = 1; for (unsigned i = 1; i < song.count; ++i) sorted &= (song.ev[i - 1].tick & 0xffffff) <= (song.ev[i].tick & 0xffffff);
    CHECK(sorted, "sorted");
    /* at tick 24: the off of note 60 comes before both note-ons */
    CHECK((song.ev[1].tick & 0xffffff) == 24 && (song.ev[1].status & 0xf0) == 0x80 && song.ev[1].d1 == 60, "release first at a shared tick");
    CHECK((song.ev[0].status) == 0x95 && ((song.ev[0].tick >> 24) & 7) == 3, "track 3 to part 6");
    CHECK((song.ev[7].tick & 0xffffff) == 383 && (song.ev[7].status & 0xf0) == 0x80, "last note clipped to the song end (%u)", song.ev[7].tick & 0xffffff);
    /* play it: every note-on has its note-off */
    song.loop = 0; ngot = 0; seq_locate(&s, 0); seq_play(&s);
    for (int b = 0; b < 1000 && s.state == SEQ_PLAYING; ++b) seq_block(&s, FM4_BLOCK);
    CHECK(s.state == SEQ_STOPPED, "one-shot song ended");
    unsigned ons = 0, offs = 0; for (unsigned i = 0; i < ngot; ++i) { if ((got[i].st & 0xf0) == 0x90) ++ons; else ++offs; }
    CHECK(ons == 4 && offs == 4, "step notes play back (%u on, %u off)", ons, offs);

    printf(failures ? "%d FAILURES\n" : "all sequencer tests passed\n", failures);
    return failures ? 1 : 0;
}
