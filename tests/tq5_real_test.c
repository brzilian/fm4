/* Regression test: TQ5 banks from real hardware must make sound.
 *
 * Bug: voices loaded from a real TQ5 bank were silent while FM-4's own
 * presets played. Real TQ5/TX81Z voices usually carry VCED 67 FC VOL = 99 (or
 * 40); the engine treated "no CC4 received yet" as foot pedal = 0, so FC VOL
 * 99 attenuated the part to nothing. Our own presets and encoder test data all
 * had FC VOL 0, which is why round-trip tests never saw it.
 *
 * Part 1 (always runs): two hand-written VMEM records laid out like real TQ5
 * records (FC VOL 99/40, PEG bytes "222222", TQ5 bytes 90/91/95/96 filled the
 * way a TQ5 fills them), sent through the same path the webapp uses:
 * 100-voice bank file (24 07 bb + VMEM) -> tq5_parse -> record -> 191-byte
 * dump -> edit buffer -> STORE (vmem pack) -> program select (unpack) ->
 * tq5_to_engine -> multi -> audio. Checks level and pitch.
 *
 * Part 2 (only when tests/data/DSBANK_A.SYX, a user-supplied real TQ5 bank,
 * is present; the file is not redistributed): every non-INIT voice must be
 * audible. Skips cleanly when the file is absent. */
#include "tq5.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static multi_state m;
#define BLOCKS 160                      /* ~0.46 s at 44.1 kHz */
static float wave[BLOCKS * FM4_BLOCK];

/* Render note 60 at velocity 100 on a fresh engine; return RMS (full scale = 1). */
static double render(const tq5_voice *v)
{
    multi_voice_data d; tq5_to_engine(v, &d);
    multi_init(&m, 8);
    multi_set_voice(&m, 0, &d);
    uint8_t on[3] = { 0x90, 60, 100 }; multi_midi(&m, on, 3);
    double e = 0;
    for (unsigned b = 0; b < BLOCKS; ++b) {
        int32_t dry[FM4_BLOCK * 2], send[FM4_BLOCK * 2];
        memset(dry, 0, sizeof dry); memset(send, 0, sizeof send);
        multi_render(&m, dry, send);
        for (unsigned s = 0; s < FM4_BLOCK; ++s) {
            double x = dry[2 * s] / 8388608.0;
            wave[b * FM4_BLOCK + s] = (float)x; e += x * x;
        }
    }
    return sqrt(e / (BLOCKS * FM4_BLOCK));
}

/* Fundamental by normalised autocorrelation over the last 4096 samples. */
static double pitch_hz(void)
{
    const float *x = wave + BLOCKS * FM4_BLOCK - 4096;
    double best = 0; unsigned lag_best = 0;
    for (unsigned lag = 40; lag < 1200; ++lag) {
        double num = 0, a = 0, b = 0;
        for (unsigned i = 0; i + lag < 4096; ++i) { num += x[i] * x[i + lag]; a += x[i] * x[i]; b += x[i + lag] * x[i + lag]; }
        double r = (a > 0 && b > 0) ? num / sqrt(a * b) : 0;
        if (r > best + 1e-3) { best = r; lag_best = lag; }
        if (best > 0.9 && r < best - 0.2) break;   /* first strong peak */
    }
    return lag_best ? (double)FM4_RATE / lag_best : 0;
}

/* A VMEM record in the shape real TQ5 hardware sends. Values are our own. */
static void tq5_like_record(uint8_t r[TQ5_VMEM], unsigned alg, unsigned fc_vol, unsigned trps, const char *name)
{
    memset(r, 0, TQ5_VMEM);
    for (unsigned k = 0; k < 4; ++k) {               /* blocks OP4, OP2, OP3, OP1 */
        uint8_t *o = r + 10 * k;
        o[0] = 31; o[1] = 6; o[2] = 2; o[3] = 6; o[4] = 13;   /* AR D1R D2R RR D1L */
        o[5] = 12;                                    /* level scaling */
        o[6] = 0x02;                                  /* AME 0, EBS 0, KVS 2 */
        o[7] = k == 3 ? 92 : (alg == 7 ? 0 : 60);     /* OUT: OP1 carrier */
        o[8] = 4;                                     /* CRS 4 = ratio 1.00 */
        o[9] = (1 << 3) | 3;                          /* RS 1, DET centre */
    }
    r[40] = (uint8_t)(alg & 7); r[41] = 30; r[45] = 0x02 | (2 << 4);
    r[46] = (uint8_t)trps; r[47] = 2; r[48] = 0x04;   /* PBR 2, SUSTAIN flag */
    r[50] = (uint8_t)fc_vol;                           /* FC VOL: 99 or 40 on most real voices */
    r[51] = 50; r[55] = 50;                            /* MW pitch, BC pitch bias */
    size_t nl = strlen(name);
    for (unsigned i = 0; i < 10; ++i) r[57 + i] = (uint8_t)(i < nl ? name[i] : ' ');
    for (unsigned i = 0; i < 6; ++i) r[67 + i] = '2';  /* PEG all 50, as TQ5 writes */
    r[90] = 3; r[91] = 1; r[95] = 0x32; r[96] = 0x64;  /* TQ5 extension bytes */
}

/* The librarian path, in C: record -> 191-byte dump -> fresh edit buffer ->
 * STORE (pack) -> select (unpack). */
static void via_librarian(const uint8_t rec[TQ5_VMEM], tq5_voice *out)
{
    tq5_voice tmp, edit; uint8_t dump[200], stored[TQ5_VMEM];
    tq5_vmem_unpack(rec, &tmp);
    size_t n = tq5_reply(&tmp, TQ5_REQ_EFEDS, 0, dump, sizeof dump);
    CHECK(n == TQ5_FULL_VOICE_BYTES, "dump size %zu", n);
    tq5_voice_init(&edit);
    for (size_t off = 0; off < n;) {
        size_t end = off; while (dump[end] != 0xF7) ++end;
        tq5_result r = tq5_parse(dump + off, end - off + 1, 0, &edit);
        CHECK(r.action == TQ5_VOICE_EDIT, "librarian block action %d", r.action);
        off = end + 1;
    }
    tq5_vmem_pack(&edit, stored);
    tq5_vmem_unpack(stored, out);
}

static void synthetic(void)
{
    static uint8_t recs[32][TQ5_VMEM], msg[4200];
    tq5_like_record(recs[0], 7, 99, 24, "SineFC99");
    tq5_like_record(recs[1], 0, 40, 12, "StackFC40");
    for (unsigned i = 2; i < 32; ++i) memcpy(recs[i], recs[0], TQ5_VMEM);

    /* As a TQ5 100-voice bank file: block select 1, then the VMEM bulk. */
    tq5_voice scratch; tq5_voice_init(&scratch);
    uint8_t sel[7]; tq5_msg_param(0, 0x24, 7, 1, sel);
    tq5_result r = tq5_parse(sel, 7, 16, &scratch);
    CHECK(r.action == TQ5_BLOCK_SELECT && r.block == 0, "block select");
    size_t n = tq5_msg_vmem((const uint8_t (*)[TQ5_VMEM])recs, 0, msg);
    r = tq5_parse(msg, n, 16, &scratch);
    CHECK(r.action == TQ5_VMEM_BLOCK, "vmem block action %d", r.action);
    if (r.action != TQ5_VMEM_BLOCK) return;

    static const struct { double hz; const char *name; } want[2] = { { 261.63, "SineFC99" }, { 130.81, "StackFC40" } };
    for (unsigned i = 0; i < 2; ++i) {
        tq5_voice v; char name[11];
        via_librarian(r.vmem + i * TQ5_VMEM, &v);
        tq5_voice_name(&v, name);
        CHECK(!strncmp(name, want[i].name, strlen(want[i].name)), "name '%s'", name);
        CHECK(v.vced[TQ5_FCVOL] == (i ? 40 : 99), "FC VOL kept (%u)", v.vced[TQ5_FCVOL]);
        double rms = render(&v), hz = pitch_hz();
        CHECK(rms > 0.01, "%s silent: rms %.5f", name, rms);
        CHECK(fabs(hz / want[i].hz - 1) < 0.02, "%s pitch %.1f Hz, want %.1f", name, hz, want[i].hz);
        printf("  %s rms %.4f pitch %.1f Hz\n", name, rms, hz);
    }

    /* A foot pedal still works: CC4 = 0 mutes an FC VOL 99 voice, as on hardware. */
    tq5_voice v; tq5_vmem_unpack(recs[0], &v);
    multi_voice_data d; tq5_to_engine(&v, &d);
    multi_init(&m, 8); multi_set_voice(&m, 0, &d);
    uint8_t cc[3] = { 0xB0, 4, 0 }, on[3] = { 0x90, 60, 100 };
    multi_midi(&m, cc, 3); multi_midi(&m, on, 3);
    double e = 0;
    for (unsigned b = 0; b < 40; ++b) {
        int32_t dry[FM4_BLOCK * 2], send[FM4_BLOCK * 2];
        memset(dry, 0, sizeof dry); memset(send, 0, sizeof send);
        multi_render(&m, dry, send);
        for (unsigned s = 0; s < FM4_BLOCK * 2; ++s) e += fabs((double)dry[s]);
    }
    CHECK(e == 0, "foot pedal at 0 should mute an FC VOL 99 voice");
}

static void real_bank(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP real TQ5 bank: %s not present\n", path); return; }
    static uint8_t d[32768];
    size_t len = fread(d, 1, sizeof d, f); fclose(f);
    printf("  real TQ5 bank %s (%zu bytes)\n", path, len);
    tq5_voice scratch; tq5_voice_init(&scratch);
    unsigned block = 0, voices = 0, silent = 0, blocks = 0; double quietest = 1;
    for (size_t i = 0; i < len;) {
        if (d[i] != 0xF0) { ++i; continue; }
        size_t j = i + 1; while (j < len && d[j] != 0xF7) ++j;
        if (j >= len) break;
        tq5_result r = tq5_parse(d + i, j - i + 1, 16, &scratch);
        if (r.action == TQ5_BLOCK_SELECT) block = r.block;
        else if (r.action == TQ5_VMEM_BLOCK) {
            ++blocks;
            for (unsigned k = 0; k < 25; ++k) {
                tq5_voice v; char name[11];
                via_librarian(r.vmem + k * TQ5_VMEM, &v);
                tq5_voice_name(&v, name);
                if (!strcmp(name, "INIT VOICE")) continue;
                ++voices;
                double rms = render(&v);
                if (rms < quietest) quietest = rms;
                if (rms < 1e-3) { ++silent; printf("  silent: %u-%02u %s (FC VOL %u)\n", block + 1, k, name, v.vced[TQ5_FCVOL]); }
            }
        }
        i = j + 1;
    }
    CHECK(blocks == 4, "bank blocks %u", blocks);
    CHECK(voices > 0, "no voices decoded");
    CHECK(silent == 0, "%u of %u real TQ5 voices silent", silent, voices);
    printf("  %u real voices audible (quietest rms %.4f)\n", voices - silent, quietest);
}

int main(void)
{
    synthetic();
    const char *path = getenv("FM4_TQ5_BANK");
    real_bank(path ? path : "data/DSBANK_A.SYX");
    printf(failures ? "%d FAILURES\n" : "all tq5 real-data tests passed\n", failures);
    return failures ? 1 : 0;
}
