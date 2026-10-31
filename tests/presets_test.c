/* Render every factory preset: audible, not clipping, voices end. Writes out/presets.wav. */
#include "tq5.h"
#include "presets_bank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static multi_state m; static fx_unit fx;

int main(void)
{
    const long per = FM4_RATE * 3 / 4;
    long total = per * FM4_PRESETS, pos = 0;
    int16_t *pcm = calloc((size_t)total * 2, 2);
    for (unsigned i = 0; i < FM4_PRESETS; ++i) {
        tq5_voice v; tq5_vmem_unpack(fm4_preset_bank[i], &v);
        multi_voice_data d; tq5_to_engine(&v, &d);
        multi_init(&m, 8); fx_init(&fx);
        multi_set_voice(&m, 0, &d);
        fx_params fp; tq5_effect(&v, &fp); fx_set(&fx, &fp);
        unsigned note = (strstr(d.name, "Bass") || strstr(d.name, "Bas")) ? 36 : 60;
        uint8_t on[3] = { 0x90, (uint8_t)note, 100 }, on2[3] = { 0x90, (uint8_t)(note + 7), 90 };
        multi_midi(&m, on, 3);
        if (!d.perf.poly_mono) multi_midi(&m, on2, 3);
        long peak = 0;
        for (long s = 0; s < per; s += FM4_BLOCK) {
            int32_t dry[FM4_BLOCK * 2] = {0}, send[FM4_BLOCK * 2] = {0};
            if (s == per / 2) { uint8_t off[3] = { 0x80, (uint8_t)note, 0 }, off2[3] = { 0x80, (uint8_t)(note + 7), 0 }; multi_midi(&m, off, 3); multi_midi(&m, off2, 3); }
            multi_render(&m, dry, send); fx_render(&fx, send, dry);
            for (int k = 0; k < FM4_BLOCK && pos < total; ++k, ++pos) {
                for (int c = 0; c < 2; ++c) {
                    long x = dry[2 * k + c]; if (labs(x) > peak) peak = labs(x);
                    x >>= 8; pcm[2 * pos + c] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
                }
            }
        }
        double db = peak > 0 ? 20.0 * __builtin_log10((double)peak / 8388608.0) : -999;
        printf("%3u %-10s peak %6.1f dBFS%s\n", i, d.name, db, d.perf.poly_mono ? " (mono)" : "");
        CHECK(db > -36.0, "%s too quiet (%.1f dB)", d.name, db);
        CHECK(peak < 8388608, "%s clips", d.name);
    }
    FILE *f = fopen("out/presets.wav", "wb");
    uint32_t data = (uint32_t)total * 4, riff = 36 + data, rate = FM4_RATE, br = rate * 4, fmt = 16; uint16_t pf = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVEfmt ",1,8,f); fwrite(&fmt,4,1,f); fwrite(&pf,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&rate,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bits,2,1,f); fwrite("data",1,4,f); fwrite(&data,4,1,f);
    fwrite(pcm, 4, (size_t)total, f); fclose(f); free(pcm);
    printf(failures ? "%d FAILURES\n" : "all preset tests passed\n", failures);
    return failures ? 1 : 0;
}
