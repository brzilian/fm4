/* Host tests for the FM-4 OPZ engine: pitch calibration, envelopes, algorithms.
 * Build: make -C tests */
#include "opz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static opz_patch sine_patch(void)
{
    opz_patch p; memset(&p, 0, sizeof p);
    p.alg = 7;
    for (int i = 0; i < 4; ++i) {
        opz_op_params *o = &p.op[i];
        o->ar = 31; o->d1r = 0; o->d2r = 0; o->rr = 7; o->d1l = 15;
        o->out = i == 0 ? 99 : 0; o->crs = 4; o->det = 3; o->on = 1;
    }
    return p;
}

static double measure_hz(opz_engine *e, int seconds)
{
    int32_t dry[FM4_BLOCK * 2];
    int blocks = seconds * FM4_RATE / FM4_BLOCK, crossings = 0, first = -1, last = 0;
    int32_t prev = 0; long n = 0;
    for (int b = 0; b < blocks; ++b) {
        memset(dry, 0, sizeof dry);
        opz_render(e, dry, NULL);
        for (int s = 0; s < FM4_BLOCK; ++s, ++n) {
            int32_t x = dry[2 * s];
            if (n > 2000 && prev < 0 && x >= 0) { if (first < 0) first = (int)n; last = (int)n; ++crossings; }
            prev = x;
        }
    }
    if (crossings < 2) return 0;
    return (double)(crossings - 1) * FM4_RATE / (double)(last - first);
}

static void write_wav(const char *path, const int16_t *pcm, long frames)
{
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t data = (uint32_t)frames * 4, riff = 36 + data, rate = FM4_RATE, br = rate * 4, fmt = 16;
    uint16_t pcmfmt = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(pcm, 4, (size_t)frames, f); fclose(f);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static opz_engine e;
    opz_patch p = sine_patch();

    /* 1. Pitch calibration: MIDI 69 should be 440 Hz. */
    opz_init(&e, 8);
    opz_key_on(&e, 0, &p, 0, 69, 127, 32767, 32767, 0);
    double a4 = measure_hz(&e, 2);
    printf("A4 measured %.3f Hz (suggested OPZ_STEP_SCALE %.1f)\n", a4, 5195.0 * 440.0 / a4);
    CHECK(fabs(a4 - 440.0) < 0.5, "A4 = %.3f", a4);
    for (int note = 24; note <= 108; note += 12) {
        opz_init(&e, 8);
        opz_key_on(&e, 0, &p, 0, (unsigned)note, 127, 32767, 32767, 0);
        double hz = measure_hz(&e, 1), want = 440.0 * pow(2.0, (note - 69) / 12.0);
        double cents = 1200.0 * log2(hz / want);
        printf("note %3d: %9.3f Hz (want %9.3f, %+.2f cents)\n", note, hz, want, cents);
        CHECK(fabs(cents) < 3.0, "note %d off by %.2f cents", note, cents);
    }
    /* Ratio display table spot checks against the TX81Z list. */
    CHECK(opz_ratio_x100(1, 0) == 71, "crs1 %u", opz_ratio_x100(1, 0));
    CHECK(opz_ratio_x100(63, 0) == 2597 || opz_ratio_x100(63, 0) == 2595, "crs63 %u", opz_ratio_x100(63, 0));
    CHECK(opz_fixed_hz(0, 0, 0) == 8, "fixed min");
    CHECK(opz_fixed_hz(63, 7, 15) == 255 << 7, "fixed max");

    /* 2. Fixed frequency op. */
    opz_init(&e, 8);
    p.op[0].fixed = 1; p.op[0].crs = 25 << 2 >> 2 << 2; p.op[0].fine = 0; p.op[0].fixrg = 0;
    p.op[0].crs = 6 << 2; p.op[0].fine = 4;  /* (6<<4|4) = 100 Hz */
    opz_key_on(&e, 0, &p, 0, 60, 127, 32767, 32767, 0);
    double fx = measure_hz(&e, 2);
    printf("fixed 100 Hz measured %.3f\n", fx);
    CHECK(fabs(fx - 100.0) < 0.2, "fixed %.3f", fx);
    p = sine_patch();

    /* 3. Release ends the voice; envelope timing sanity. */
    opz_init(&e, 8);
    p.op[0].rr = 15;
    opz_key_on(&e, 0, &p, 0, 60, 127, 32767, 32767, 0);
    int32_t dry[FM4_BLOCK * 2];
    for (int b = 0; b < 50; ++b) { memset(dry, 0, sizeof dry); opz_render(&e, dry, NULL); }
    opz_key_off(&e, 0);
    int blocks = 0;
    while (opz_voice_busy(&e, 0) && blocks < 2000) { memset(dry, 0, sizeof dry); opz_render(&e, dry, NULL); ++blocks; }
    printf("RR15 release finished after %d blocks (%.0f ms)\n", blocks, blocks * 128000.0 / FM4_RATE);
    CHECK(blocks > 0 && blocks < 200, "release blocks %d", blocks);

    /* 4. Render a demo of every waveform and algorithm to WAV for listening. */
    const long frames = FM4_RATE * 16;
    int16_t *pcm = calloc((size_t)frames * 2, sizeof *pcm);
    opz_init(&e, 8);
    long pos = 0;
    for (int alg = 0; alg < 8 && pos < frames; ++alg) {
        opz_patch q; memset(&q, 0, sizeof q);
        q.alg = (uint8_t)alg; q.fb = 5; q.lfs = 70; q.pmd = 8; q.pms = 3;
        for (int i = 0; i < 4; ++i) {
            opz_op_params *o = &q.op[i];
            o->ar = 31; o->d1r = 8 + i * 2; o->d2r = 3; o->rr = 6; o->d1l = 11;
            o->out = i == 0 ? 99 : 80 - i * 6; o->crs = (uint8_t)(4 + i * 4); o->det = 3; o->on = 1;
            o->wave = (uint8_t)alg; o->kvs = 2;
        }
        static const unsigned chord[3] = { 48, 55, 64 };
        for (int k = 0; k < 3; ++k) opz_key_on(&e, (unsigned)k, &q, 0, chord[k], 110, 12000, 12000, 0);
        for (int b = 0; b < 12 && pos < frames; ++b) {
            memset(dry, 0, sizeof dry); opz_render(&e, dry, NULL);
            for (int s = 0; s < FM4_BLOCK && pos < frames; ++s, ++pos) {
                pcm[2 * pos] = (int16_t)fminf(32767, fmaxf(-32768, dry[2 * s] >> 8));
                pcm[2 * pos + 1] = (int16_t)fminf(32767, fmaxf(-32768, dry[2 * s + 1] >> 8));
            }
        }
        for (int k = 0; k < 3; ++k) opz_key_off(&e, (unsigned)k);
        for (int b = 0; b < 160 && pos < frames; ++b) {
            memset(dry, 0, sizeof dry); opz_render(&e, dry, NULL);
            for (int s = 0; s < FM4_BLOCK && pos < frames; ++s, ++pos) {
                pcm[2 * pos] = (int16_t)fminf(32767, fmaxf(-32768, dry[2 * s] >> 8));
                pcm[2 * pos + 1] = (int16_t)fminf(32767, fmaxf(-32768, dry[2 * s + 1] >> 8));
            }
        }
    }
    write_wav("out/algorithms.wav", pcm, pos);
    free(pcm);

    printf(failures ? "%d FAILURES\n" : "all engine tests passed\n", failures);
    return failures ? 1 : 0;
}
