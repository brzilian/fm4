/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 effects. See fx.h. */
#include "fx.h"
#include <string.h>

/* Schroeder-style reverb: 4 combs + 2 allpasses in one 16-bit buffer. */
static const uint16_t comb_len[4] FM4_AUDIO_DATA = { 1116, 1188, 1277, 1356 };
static const uint16_t ap_len[2] FM4_AUDIO_DATA = { 556, 441 };
static const uint16_t region[6] FM4_AUDIO_DATA = { 0, 1116, 2304, 3581, 4937, 5493 };

void fx_init(fx_unit *f)
{
    memset(f, 0, sizeof *f);
    fx_params p = { FX_ALGO_OFF, 50, 40, 50 };
    fx_set(f, &p);
}

void fx_set(fx_unit *f, const fx_params *p)
{
    unsigned t = p->time > 99 ? 99 : p->time, c = p->character > 99 ? 99 : p->character;
    if (p->algo != f->p.algo) {
        memset(f->dl, 0, sizeof f->dl); memset(f->dr, 0, sizeof f->dr);
        memset(f->rv, 0, sizeof f->rv); memset(f->lp, 0, sizeof f->lp);
        memset(f->apos, 0, sizeof f->apos);
    }
    f->p = *p;
    /* delay 10..(FX_DELAY_SAMPLES-1) samples */
    f->delay_len = 441 + (int32_t)((FX_DELAY_SAMPLES - 500) * t / 99U);
    f->feedback = (int32_t)(c * 26000U / 99U);           /* Q15 up to ~0.8 */
    f->rv_feedback = 21000 + (int32_t)(t * 9000U / 99U) + (int32_t)(c * 1800U / 99U); /* Q15 */
    if (f->rv_feedback > 31800) f->rv_feedback = 31800;
    f->wet = (int32_t)((p->balance > 99 ? 99 : p->balance) * 32767U / 99U);
    f->drive = 1 + (int32_t)(c * 15U / 99U);
}

static FM4_AUDIO_CODE int32_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

static FM4_AUDIO_CODE int32_t softclip(int32_t x, int32_t drive)
{
    /* x in Q15-ish; cubic soft clip after drive gain */
    int32_t y = x * drive;
    if (y > 32767) y = 32767; else if (y < -32767) y = -32767;
    return y - (((((y * y) >> 15) * y) >> 15) / 3);
}

static FM4_AUDIO_CODE void reverb(fx_unit *f, int32_t in, int32_t *ol, int32_t *or_)
{
    int32_t sum = 0;
    for (unsigned c = 0; c < 4; ++c) {
        int16_t *buf = f->rv + region[c];
        int32_t pos = f->apos[c];
        int32_t y = buf[pos];
        f->lp[c] += ((y - f->lp[c]) * 20000) >> 15;        /* damping */
        buf[pos] = (int16_t)sat16(in + ((f->lp[c] * f->rv_feedback) >> 15));
        if (++pos >= comb_len[c]) pos = 0;
        f->apos[c] = pos;
        sum += y;
    }
    sum >>= 2;
    for (unsigned a = 0; a < 2; ++a) {
        int16_t *buf = f->rv + region[4 + a];
        int32_t pos = f->apos[4 + a];
        int32_t b = buf[pos];
        int32_t y = b - sum;
        buf[pos] = (int16_t)sat16(sum + (b >> 1));
        if (++pos >= ap_len[a]) pos = 0;
        f->apos[4 + a] = pos;
        sum = y;
    }
    /* stereo spread from the comb taps */
    *ol = sum + (f->lp[0] >> 2) - (f->lp[2] >> 2);
    *or_ = sum - (f->lp[1] >> 2) + (f->lp[3] >> 2);
}

FM4_AUDIO_CODE void fx_render(fx_unit *f, const int32_t *send, int32_t *mix)
{
    unsigned algo = f->p.algo;
    if (algo == FX_ALGO_OFF || !f->wet) return;
    const int32_t wet = f->wet;
    uint32_t mask = FX_DELAY_SAMPLES - 1;
    for (unsigned s = 0; s < FM4_BLOCK; ++s) {
        int32_t l = sat16(send[2 * s] >> 8), r = sat16(send[2 * s + 1] >> 8);
        int32_t ol = 0, or_ = 0;
        if (algo == FX_ALGO_DIST || algo == FX_ALGO_DIST_REVERB || algo == FX_ALGO_DIST_DELAY) {
            l = softclip(l, f->drive); r = softclip(r, f->drive);
            /* DC/rumble blocker after clipping */
            f->dc_l += (l - f->dc_l) >> 7; f->dc_r += (r - f->dc_r) >> 7;
            l -= f->dc_l; r -= f->dc_r;
        }
        switch (algo) {
        case FX_ALGO_DIST:
            ol = l; or_ = r; break;
        case FX_ALGO_REVERB:
        case FX_ALGO_DIST_REVERB: {
            int32_t rl, rr; reverb(f, (l + r) >> 2, &rl, &rr);
            ol = rl; or_ = rr;
            if (algo == FX_ALGO_DIST_REVERB) { ol += l; or_ += r; }
            break;
        }
        case FX_ALGO_DELAY:
        case FX_ALGO_DIST_DELAY: {
            uint32_t rd = (f->dpos - (uint32_t)f->delay_len) & mask;
            int32_t y = f->dl[rd];
            f->dl[f->dpos & mask] = (int16_t)sat16(((l + r) >> 1) + ((y * f->feedback) >> 15));
            ol = or_ = y;
            if (algo == FX_ALGO_DIST_DELAY) { ol += l; or_ += r; }
            break;
        }
        case FX_ALGO_PINGPONG: {
            const uint32_t rmask = FX_DELAY_R_SAMPLES - 1;
            uint32_t len = (uint32_t)f->delay_len > rmask ? rmask : (uint32_t)f->delay_len;
            int32_t yl = f->dl[(f->dpos - len) & mask], yr = f->dr[(f->dpos - len) & rmask];
            f->dl[f->dpos & mask] = (int16_t)sat16(((l + r) >> 1) + ((yr * f->feedback) >> 15));
            f->dr[f->dpos & rmask] = (int16_t)sat16((yl * f->feedback) >> 15);
            ol = yl; or_ = yr;
            break;
        }
        case FX_ALGO_CHORUS: {
            /* two taps, 7..(7+depth) ms, triangle LFO in quadrature */
            f->lfo += 120U + (uint32_t)f->p.time * 4U;
            uint32_t ph = f->lfo >> 8;
            int32_t tri = (int32_t)((ph & 0x8000) ? (0xffff - (ph & 0xffff)) : (ph & 0xffff)) - 0x4000;
            int32_t depth = 40 + (int32_t)f->p.character * 4; /* samples */
            int32_t base = 300;
            f->dl[f->dpos & mask] = (int16_t)((l + r) >> 1);
            for (unsigned k = 0; k < 2; ++k) {
                int32_t t = k ? -tri : tri;
                int32_t d16 = (base << 16) + t * depth * 4;  /* Q16 samples */
                uint32_t di = (uint32_t)(d16 >> 16), fr = (uint32_t)d16 & 0xffff;
                int32_t a = f->dl[(f->dpos - di) & mask], b = f->dl[(f->dpos - di - 1) & mask];
                int32_t y = a + (((b - a) * (int32_t)(fr >> 2)) >> 14);
                if (k) or_ = y; else ol = y;
            }
            break;
        }
        case FX_ALGO_DELAY_LR: {
            f->dl[f->dpos & mask] = (int16_t)((l + r) >> 1);
            ol = f->dl[(f->dpos - (uint32_t)f->delay_len) & mask];
            or_ = f->dl[(f->dpos - (uint32_t)(f->delay_len * 2 / 3)) & mask];
            break;
        }
        case FX_ALGO_GATE:
        case FX_ALGO_REVERSE: {
            /* 12 taps spread over the gate time; flat (gate) or rising (reverse)
             * gains, alternated between the sides, then lightly diffused. */
            f->dl[f->dpos & mask] = (int16_t)((l + r) >> 1);
            int32_t accl = 0, accr = 0;
            for (unsigned k = 1; k <= 12; ++k) {
                uint32_t d = (uint32_t)(f->delay_len * (int32_t)k / 12) + k * 37U;
                int32_t g = algo == FX_ALGO_GATE ? 2600 : (int32_t)(k * 420U);
                int32_t x = (f->dl[(f->dpos - d) & mask] * g) >> 15;
                if (k & 1) accl += x; else accr += x;
            }
            int32_t rl, rr; reverb(f, (accl + accr) >> 2, &rl, &rr);
            ol = accl + (rl >> 1); or_ = accr + (rr >> 1);
            break;
        }
        default: break;
        }
        f->dpos++;
        mix[2 * s] += ((ol * wet) >> 15) << 8;
        mix[2 * s + 1] += ((or_ * wet) >> 15) << 8;
    }
}
