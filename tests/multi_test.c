/* Host tests for the FM-4 multitimbral layer. */
#include "multi.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static multi_state m;

static multi_voice_data basic(void)
{
    multi_voice_data v; memset(&v, 0, sizeof v);
    v.patch.alg = 7;
    for (int i = 0; i < 4; ++i) {
        opz_op_params *o = &v.patch.op[i];
        o->ar = 31; o->rr = 8; o->d1l = 15; o->out = 90; o->crs = 4; o->det = 3; o->on = 1;
    }
    v.perf.pb_range = 2; v.perf.transpose = 24;
    return v;
}
static void midi3(uint8_t a, uint8_t b, uint8_t c) { uint8_t x[3] = { a, b, c }; multi_midi(&m, x, 3); }
static void run(int blocks) { int32_t d[FM4_BLOCK * 2], s[FM4_BLOCK * 2]; while (blocks--) { memset(d, 0, sizeof d); memset(s, 0, sizeof s); multi_render(&m, d, s); } }

int main(void)
{
    multi_init(&m, 8);
    multi_voice_data v = basic();
    for (unsigned p = 0; p < MULTI_PARTS; ++p) multi_set_voice(&m, p, &v);

    /* Channel routing. */
    m.part[0].cfg.enabled = 1; m.part[0].cfg.channel = 0; m.part[0].cfg.reserve = 4;
    m.part[1].cfg.enabled = 1; m.part[1].cfg.channel = 1; m.part[1].cfg.reserve = 4;
    CHECK(multi_parts_for_channel(&m, 0) == 1, "ch0 mask");
    CHECK(multi_parts_for_channel(&m, 1) == 2, "ch1 mask");
    midi3(0x90, 60, 100); midi3(0x91, 64, 100);
    run(2);
    CHECK(multi_active_voices(&m) == 2, "two voices %u", multi_active_voices(&m));

    /* Reserve: part 0 floods 10 notes, part 1's voice must survive. */
    for (int n = 0; n < 10; ++n) midi3(0x90, (uint8_t)(70 + n), 100);
    int part1_alive = 0;
    for (unsigned s = 0; s < 8; ++s) if (m.slot_part[s] == 1 && m.eng.v[s].keyed) part1_alive = 1;
    CHECK(part1_alive, "part 1 voice stolen despite reserve");
    multi_all_off(&m, 1); run(1);
    CHECK(multi_active_voices(&m) == 0, "all off");

    /* Sustain pedal holds release. */
    midi3(0x90, 60, 100); midi3(0xb0, 64, 127); midi3(0x80, 60, 0); run(20);
    CHECK(multi_active_voices(&m) == 1 && m.eng.v[0].keyed, "sustained");
    midi3(0xb0, 64, 0); run(400);
    CHECK(multi_active_voices(&m) == 0, "released after pedal up (%u)", multi_active_voices(&m));

    /* Mono legato with fingered portamento glides without retrigger. */
    m.part[0].data.perf.poly_mono = 1; m.part[0].data.perf.porta_mode = 1; m.part[0].data.perf.porta_time = 50;
    midi3(0x90, 60, 100); run(5);
    midi3(0x90, 72, 100); run(1);
    CHECK(multi_active_voices(&m) == 1, "mono single voice");
    unsigned mid = 0; for (unsigned s = 0; s < 8; ++s) if (m.eng.v[s].active) mid = m.eng.v[s].pitch64;
    CHECK(mid > 60 * 64 && mid < 72 * 64, "gliding pitch %u", mid);
    run(400);
    for (unsigned s = 0; s < 8; ++s) if (m.eng.v[s].active) mid = m.eng.v[s].pitch64;
    CHECK(mid == 72 * 64, "glide arrives %u", mid);
    midi3(0x80, 72, 0); run(1);
    for (unsigned s = 0; s < 8; ++s) if (m.eng.v[s].active) mid = m.eng.v[s].pitch64;
    CHECK(mid < 72 * 64, "returns toward held 60 (%u)", mid);
    multi_all_off(&m, 1);

    /* Pitch bend range. */
    m.part[0].data.perf.poly_mono = 0; m.part[0].data.perf.porta_time = 0; m.part[0].data.perf.pb_range = 12;
    midi3(0x90, 60, 100); midi3(0xe0, 0x7f, 0x7f); run(1);
    int bend = 0; for (unsigned s = 0; s < 8; ++s) if (m.eng.v[s].active) bend = m.eng.v[s].bend64;
    CHECK(bend > 760 && bend <= 768, "bend +12 st = %d", bend);
    multi_all_off(&m, 1); run(1);

    /* CPU governor: overload sheds the oldest voice with a fast fade (no
     * hard cut, no settings change) and caps new allocations; calm restores. */
    m.part[0].data.perf.pb_range = 2; m.part[0].cfg.reserve = 8;
    for (int n = 0; n < 6; ++n) midi3(0x90, (uint8_t)(60 + n), 100);
    run(4);
    CHECK(multi_active_voices(&m) == 6, "six voices %u", multi_active_voices(&m));
    unsigned oldest = 0; for (unsigned s = 0; s < 8; ++s) if (m.eng.v[s].active && m.eng.v[s].note == 60) oldest = s;
    multi_governor(900, 1000);                    /* 90 % of the block */
    run(1);
    CHECK(!m.eng.v[oldest].keyed && m.eng.v[oldest].active, "oldest voice fading, not cut");
    CHECK(m.gov_shed == 1 && m.eng.voices == 8, "shed once, polyphony setting untouched");
    run(20);                                      /* ~58 ms: fast release done */
    CHECK(!m.eng.v[oldest].active, "shed voice finished");
    CHECK(multi_active_voices(&m) == 5, "five left %u", multi_active_voices(&m));
    for (int n = 0; n < 4; ++n) midi3(0x90, (uint8_t)(80 + n), 100);
    CHECK(multi_active_voices(&m) <= 5, "cap holds new notes to 5 (%u)", multi_active_voices(&m));
    multi_governor(900, 1000); run(1);            /* inside the 16-block hold: no second shed */
    CHECK(m.gov_shed == 1, "rate limited (%u)", (unsigned)m.gov_shed);
    for (int b = 0; b < 3 * 344; ++b) multi_governor(100, 1000);   /* 3 s calm */
    multi_all_off(&m, 1); run(1);
    for (int n = 0; n < 8; ++n) midi3(0x90, (uint8_t)(60 + n), 100);
    CHECK(multi_active_voices(&m) == 8, "polyphony restored (%u)", multi_active_voices(&m));
    multi_all_off(&m, 1);

    printf(failures ? "%d FAILURES\n" : "all multi tests passed\n", failures);
    return failures ? 1 : 0;
}
