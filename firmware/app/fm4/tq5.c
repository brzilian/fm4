/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 TQ5 voice codec and SysEx protocol. See tq5.h, docs/tq5_spec.md. */
#include "tq5.h"
#include <string.h>

const uint8_t tq5_vced_max[TQ5_VCED] = {
#define OPM 31,31,31,15,15,99,3,7,1,7,99,63,6
    OPM, OPM, OPM, OPM,
#undef OPM
    7, 7, 99, 99, 99, 99, 1, 3, 7, 3, 48, 1, 12, 1, 99, 99, 1, 1, 1, 99, 99, 99, 99, 99, 99,
    127,127,127,127,127,127,127,127,127,127, 99, 99, 99, 99, 99, 99
};
const uint8_t tq5_aced_max[TQ5_ACED] = {
    1,7,15,7,3, 1,7,15,7,3, 1,7,15,7,3, 1,7,15,7,3, 7, 99, 99
};
const uint8_t tq5_aced2_max[TQ5_ACED2] = { 99, 99, 100, 99, 1, 1, 1, 1, 15, 0 };
static const char hdr_aced[10] = { 'L','M',' ',' ','8','9','7','6','A','E' };
static const char hdr_aced2[10] = { 'L','M',' ',' ','8','0','2','3','A','E' };
static const char hdr_efeds[10] = { 'L','M',' ',' ','8','0','3','6','E','F' };

void tq5_voice_init(tq5_voice *v)
{
    memset(v, 0, sizeof *v);
    for (unsigned k = 0; k < 4; ++k) {
        uint8_t *o = v->vced + 13 * k;
        o[TQ5_OP_AR] = 31; o[TQ5_OP_D1R] = 31; o[TQ5_OP_D2R] = 0; o[TQ5_OP_RR] = 15;
        o[TQ5_OP_D1L] = 15; o[TQ5_OP_OUT] = k == 3 ? 90 : 0; o[TQ5_OP_CRS] = 4; o[TQ5_OP_DET] = 3;
    }
    v->vced[TQ5_ALG] = 0; v->vced[TQ5_LFS] = 35; v->vced[TQ5_SYNC] = 1; v->vced[TQ5_LFW] = 2;
    v->vced[TQ5_PMS] = 6; v->vced[TQ5_TRPS] = 24; v->vced[TQ5_PBR] = 4;
    v->vced[TQ5_MWPITCH] = 50; v->vced[TQ5_BCPBIAS] = 50;
    tq5_voice_set_name(v, "INIT VOICE");
    v->aced2[2] = 50;
    v->efeds[0] = 1; v->efeds[1] = 20; v->efeds[2] = 0;
    v->opmask = 15;
}

void tq5_voice_name(const tq5_voice *v, char out[11])
{
    for (unsigned i = 0; i < 10; ++i) {
        char c = (char)v->vced[TQ5_NAME + i];
        out[i] = (c < 32 || c > 126) ? ' ' : c;
    }
    out[10] = 0;
}

void tq5_voice_set_name(tq5_voice *v, const char *name)
{
    unsigned i = 0;
    for (; i < 10 && name && name[i]; ++i) v->vced[TQ5_NAME + i] = (uint8_t)name[i] & 127;
    for (; i < 10; ++i) v->vced[TQ5_NAME + i] = ' ';
}

void tq5_clamp(tq5_voice *v)
{
    for (unsigned i = 0; i < TQ5_VCED; ++i) if (v->vced[i] > tq5_vced_max[i]) v->vced[i] = tq5_vced_max[i];
    for (unsigned i = 0; i < TQ5_ACED; ++i) if (v->aced[i] > tq5_aced_max[i]) v->aced[i] = tq5_aced_max[i];
    for (unsigned i = 0; i < TQ5_ACED2; ++i) if (v->aced2[i] > tq5_aced2_max[i]) v->aced2[i] = tq5_aced2_max[i];
    if (v->efeds[0] > 10) v->efeds[0] = 10;
    unsigned tmax = (v->efeds[0] == 4 || v->efeds[0] == 5 || v->efeds[0] == 6 || v->efeds[0] == 8) ? 75 : 40;
    if (v->efeds[1] > tmax) v->efeds[1] = (uint8_t)tmax;
    if (v->efeds[2] > 99) v->efeds[2] = 99;
    v->opmask &= 15;
}

void tq5_to_engine(const tq5_voice *v, multi_voice_data *out)
{
    memset(out, 0, sizeof *out);
    opz_patch *p = &out->patch;
    for (unsigned n = 1; n <= 4; ++n) {
        unsigned k = tq5_op_block(n);
        const uint8_t *o = v->vced + 13 * k, *a = v->aced + 5 * k;
        opz_op_params *q = &p->op[n - 1];
        q->ar = o[TQ5_OP_AR]; q->d1r = o[TQ5_OP_D1R]; q->d2r = o[TQ5_OP_D2R];
        q->rr = o[TQ5_OP_RR]; q->d1l = o[TQ5_OP_D1L]; q->ls = o[TQ5_OP_LS];
        q->rs = o[TQ5_OP_RS]; q->ame = o[TQ5_OP_AME]; q->kvs = o[TQ5_OP_KVS] & 7;
        q->out = o[TQ5_OP_OUT]; q->crs = o[TQ5_OP_CRS]; q->det = o[TQ5_OP_DET];
        q->fixed = a[TQ5_OP_FIX]; q->fixrg = a[TQ5_OP_FIXRG]; q->fine = a[TQ5_OP_FINE];
        q->wave = a[TQ5_OP_OSW]; q->shft = a[TQ5_OP_SHFT];
        q->on = (v->opmask >> (n - 1)) & 1;
    }
    p->alg = v->vced[TQ5_ALG] & 7; p->fb = v->vced[TQ5_FBL] & 7;
    p->lfs = v->vced[TQ5_LFS]; p->lfd = v->vced[TQ5_LFD]; p->pmd = v->vced[TQ5_PMD];
    p->amd = v->vced[TQ5_AMD]; p->sync = v->vced[TQ5_SYNC]; p->lfw = v->vced[TQ5_LFW] & 3;
    p->pms = v->vced[TQ5_PMS] & 7; p->ams = v->vced[TQ5_AMS] & 3; p->rev = v->aced[20] & 7;
    multi_perf *f = &out->perf;
    f->transpose = v->vced[TQ5_TRPS] > 48 ? 48 : v->vced[TQ5_TRPS];
    f->poly_mono = v->vced[TQ5_POLY] & 1; f->pb_range = v->vced[TQ5_PBR];
    f->porta_mode = v->vced[TQ5_PORTMODE] & 1; f->porta_time = v->vced[TQ5_PORTTIME];
    f->fc_volume = v->vced[TQ5_FCVOL]; f->mw_pitch = v->vced[TQ5_MWPITCH]; f->mw_amp = v->vced[TQ5_MWAMP];
    f->bc_pitch = v->vced[TQ5_BCPITCH]; f->bc_amp = v->vced[TQ5_BCAMP];
    f->bc_pbias = v->vced[TQ5_BCPBIAS]; f->bc_ebias = v->vced[TQ5_BCEBIAS];
    f->fc_pitch = v->aced[21]; f->fc_amp = v->aced[22];
    f->at_pitch = v->aced2[0]; f->at_amp = v->aced2[1];
    memcpy(out->effect, v->efeds, 3);
    tq5_voice_name(v, out->name);
}

/* TQ5 effect presets 0..10 -> effect unit settings. */
void tq5_effect(const tq5_voice *v, fx_params *out)
{
    static const uint8_t algo[11] = {
        FX_ALGO_OFF, FX_ALGO_REVERB, FX_ALGO_REVERB, FX_ALGO_REVERB, FX_ALGO_DELAY,
        FX_ALGO_DELAY_LR, FX_ALGO_PINGPONG, FX_ALGO_DIST_REVERB, FX_ALGO_DIST_DELAY,
        FX_ALGO_GATE, FX_ALGO_REVERSE
    };
    /* character: hall large/dark, room small, plate bright and dense */
    static const uint8_t character[11] = { 0, 85, 35, 60, 45, 0, 45, 60, 45, 70, 70 };
    unsigned preset = v->efeds[0] > 10 ? 10 : v->efeds[0];
    unsigned tmax = (preset == 4 || preset == 5 || preset == 6 || preset == 8) ? 75 : 40;
    unsigned t = v->efeds[1] > tmax ? tmax : v->efeds[1];
    out->algo = algo[preset];
    out->time = (uint8_t)(t * 99U / tmax);
    out->balance = v->efeds[2] > 99 ? 99 : v->efeds[2];
    out->character = character[preset];
}

/* ---- VMEM packing ------------------------------------------------------ */
void tq5_vmem_pack(const tq5_voice *v, uint8_t out[TQ5_VMEM])
{
    memset(out, 0, TQ5_VMEM);
    for (unsigned k = 0; k < 4; ++k) {
        const uint8_t *o = v->vced + 13 * k;
        uint8_t *b = out + 10 * k;
        b[0] = o[TQ5_OP_AR] & 31; b[1] = o[TQ5_OP_D1R] & 31; b[2] = o[TQ5_OP_D2R] & 31;
        b[3] = o[TQ5_OP_RR] & 15; b[4] = o[TQ5_OP_D1L] & 15; b[5] = o[TQ5_OP_LS] & 127;
        b[6] = (uint8_t)(((o[TQ5_OP_AME] & 1) << 6) | ((o[TQ5_OP_EBS] & 7) << 3) | (o[TQ5_OP_KVS] & 7));
        b[7] = o[TQ5_OP_OUT] & 127; b[8] = o[TQ5_OP_CRS] & 63;
        b[9] = (uint8_t)(((o[TQ5_OP_RS] & 3) << 3) | (o[TQ5_OP_DET] & 7));
        const uint8_t *a = v->aced + 5 * k;
        out[73 + 2 * k] = (uint8_t)(((a[TQ5_OP_SHFT] & 3) << 4) | ((a[TQ5_OP_FIX] & 1) << 3) | (a[TQ5_OP_FIXRG] & 7));
        out[74 + 2 * k] = (uint8_t)(((a[TQ5_OP_OSW] & 7) << 4) | (a[TQ5_OP_FINE] & 15));
    }
    const uint8_t *c = v->vced;
    out[40] = (uint8_t)(((c[TQ5_SYNC] & 1) << 6) | ((c[TQ5_FBL] & 7) << 3) | (c[TQ5_ALG] & 7));
    out[41] = c[TQ5_LFS]; out[42] = c[TQ5_LFD]; out[43] = c[TQ5_PMD]; out[44] = c[TQ5_AMD];
    out[45] = (uint8_t)(((c[TQ5_PMS] & 7) << 4) | ((c[TQ5_AMS] & 3) << 2) | (c[TQ5_LFW] & 3));
    out[46] = c[TQ5_TRPS]; out[47] = c[TQ5_PBR];
    out[48] = (uint8_t)(((c[TQ5_CHORUS] & 1) << 4) | ((c[TQ5_POLY] & 1) << 3) | ((c[TQ5_SUS] & 1) << 2) |
                        ((c[TQ5_PORTSW] & 1) << 1) | (c[TQ5_PORTMODE] & 1));
    out[49] = c[TQ5_PORTTIME]; out[50] = c[TQ5_FCVOL];
    for (unsigned i = 0; i < 6; ++i) out[51 + i] = c[TQ5_MWPITCH + i];
    for (unsigned i = 0; i < 10; ++i) out[57 + i] = c[TQ5_NAME + i] & 127;
    for (unsigned i = 0; i < 6; ++i) out[67 + i] = c[87 + i];
    out[81] = v->aced[20] & 7; out[82] = v->aced[21]; out[83] = v->aced[22];
    for (unsigned i = 0; i < 4; ++i) out[84 + i] = v->aced2[i] & 127;
    out[91] = v->efeds[0]; out[92] = v->efeds[1]; out[93] = v->efeds[2];
}

void tq5_vmem_unpack(const uint8_t in[TQ5_VMEM], tq5_voice *v)
{
    memset(v, 0, sizeof *v);
    for (unsigned k = 0; k < 4; ++k) {
        uint8_t *o = v->vced + 13 * k;
        const uint8_t *b = in + 10 * k;
        o[TQ5_OP_AR] = b[0] & 31; o[TQ5_OP_D1R] = b[1] & 31; o[TQ5_OP_D2R] = b[2] & 31;
        o[TQ5_OP_RR] = b[3] & 15; o[TQ5_OP_D1L] = b[4] & 15; o[TQ5_OP_LS] = b[5] & 127;
        o[TQ5_OP_AME] = (b[6] >> 6) & 1; o[TQ5_OP_EBS] = (b[6] >> 3) & 7; o[TQ5_OP_KVS] = b[6] & 7;
        o[TQ5_OP_OUT] = b[7] & 127; o[TQ5_OP_CRS] = b[8] & 63;
        o[TQ5_OP_RS] = (b[9] >> 3) & 3; o[TQ5_OP_DET] = b[9] & 7;
        uint8_t *a = v->aced + 5 * k;
        a[TQ5_OP_SHFT] = (in[73 + 2 * k] >> 4) & 3; a[TQ5_OP_FIX] = (in[73 + 2 * k] >> 3) & 1;
        a[TQ5_OP_FIXRG] = in[73 + 2 * k] & 7;
        a[TQ5_OP_OSW] = (in[74 + 2 * k] >> 4) & 7; a[TQ5_OP_FINE] = in[74 + 2 * k] & 15;
    }
    uint8_t *c = v->vced;
    c[TQ5_SYNC] = (in[40] >> 6) & 1; c[TQ5_FBL] = (in[40] >> 3) & 7; c[TQ5_ALG] = in[40] & 7;
    c[TQ5_LFS] = in[41]; c[TQ5_LFD] = in[42]; c[TQ5_PMD] = in[43]; c[TQ5_AMD] = in[44];
    c[TQ5_PMS] = (in[45] >> 4) & 7; c[TQ5_AMS] = (in[45] >> 2) & 3; c[TQ5_LFW] = in[45] & 3;
    c[TQ5_TRPS] = in[46]; c[TQ5_PBR] = in[47];
    c[TQ5_CHORUS] = (in[48] >> 4) & 1; c[TQ5_POLY] = (in[48] >> 3) & 1; c[TQ5_SUS] = (in[48] >> 2) & 1;
    c[TQ5_PORTSW] = (in[48] >> 1) & 1; c[TQ5_PORTMODE] = in[48] & 1;
    c[TQ5_PORTTIME] = in[49]; c[TQ5_FCVOL] = in[50];
    for (unsigned i = 0; i < 6; ++i) c[TQ5_MWPITCH + i] = in[51 + i];
    for (unsigned i = 0; i < 10; ++i) c[TQ5_NAME + i] = in[57 + i] & 127;
    for (unsigned i = 0; i < 6; ++i) c[87 + i] = in[67 + i];
    v->aced[20] = in[81] & 7; v->aced[21] = in[82]; v->aced[22] = in[83];
    for (unsigned i = 0; i < 4; ++i) v->aced2[i] = in[84 + i] & 127;
    v->efeds[0] = in[91]; v->efeds[1] = in[92]; v->efeds[2] = in[93];
    v->opmask = 15;
    tq5_clamp(v);
}

/* ---- SysEx ------------------------------------------------------------- */
uint8_t tq5_checksum(const uint8_t *data, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) sum += data[i];
    return (uint8_t)((0x100U - (sum & 0xffU)) & 0x7fU);
}

static size_t fixed_msg(unsigned ch, unsigned format, const uint8_t *data, size_t n, uint8_t *out)
{
    out[0] = 0xF0; out[1] = 0x43; out[2] = (uint8_t)(ch & 15); out[3] = (uint8_t)format;
    out[4] = (uint8_t)(n >> 7); out[5] = (uint8_t)(n & 127);
    for (size_t i = 0; i < n; ++i) out[6 + i] = data[i] & 127;
    out[6 + n] = tq5_checksum(out + 6, n);
    out[7 + n] = 0xF7;
    return n + 8;
}

static size_t lm_msg(unsigned ch, const char hdr[10], const uint8_t *data, size_t n, uint8_t *out)
{
    size_t count = n + 10;
    out[0] = 0xF0; out[1] = 0x43; out[2] = (uint8_t)(ch & 15); out[3] = 0x7E;
    out[4] = (uint8_t)(count >> 7); out[5] = (uint8_t)(count & 127);
    memcpy(out + 6, hdr, 10);
    for (size_t i = 0; i < n; ++i) out[16 + i] = data[i] & 127;
    out[16 + n] = tq5_checksum(out + 6, count);
    out[17 + n] = 0xF7;
    return n + 18;
}

size_t tq5_msg_vced(const tq5_voice *v, unsigned ch, uint8_t out[101]) { return fixed_msg(ch, 3, v->vced, TQ5_VCED, out); }
size_t tq5_msg_aced(const tq5_voice *v, unsigned ch, uint8_t out[41]) { return lm_msg(ch, hdr_aced, v->aced, TQ5_ACED, out); }
size_t tq5_msg_aced2(const tq5_voice *v, unsigned ch, uint8_t out[28]) { return lm_msg(ch, hdr_aced2, v->aced2, TQ5_ACED2, out); }
size_t tq5_msg_efeds(const tq5_voice *v, unsigned ch, uint8_t out[21]) { return lm_msg(ch, hdr_efeds, v->efeds, TQ5_EFEDS, out); }

size_t tq5_msg_vmem(const uint8_t records[32][TQ5_VMEM], unsigned ch, uint8_t *out)
{
    return fixed_msg(ch, 4, &records[0][0], 32 * TQ5_VMEM, out);
}

size_t tq5_msg_param(unsigned ch, unsigned group, unsigned pp, unsigned value, uint8_t out[7])
{
    out[0] = 0xF0; out[1] = 0x43; out[2] = (uint8_t)(0x10 | (ch & 15));
    out[3] = (uint8_t)(group & 127); out[4] = (uint8_t)(pp & 127); out[5] = (uint8_t)(value & 127);
    out[6] = 0xF7;
    return 7;
}

static int device_ok(uint8_t b, unsigned rx) { return rx >= 16 || (b & 15) == rx; }

static int lm_is(const uint8_t *p, const char hdr[10]) { return memcmp(p, hdr, 10) == 0; }

tq5_result tq5_parse(const uint8_t *m, size_t len, unsigned rx, tq5_voice *edit)
{
    tq5_result r; memset(&r, 0, sizeof r);
    if (len < 5 || m[0] != 0xF0 || m[1] != 0x43 || m[len - 1] != 0xF7) return r;
    unsigned sub = m[2] & 0x70;
    if (!device_ok(m[2], rx)) return r;
    if (sub == 0x00) {                                   /* bulk dump */
        if (len < 8) { r.action = TQ5_ERROR; return r; }
        unsigned format = m[3], count = ((unsigned)m[4] << 7) | m[5];
        if (len != count + 8) { r.action = TQ5_ERROR; return r; }
        const uint8_t *d = m + 6;
        if (((unsigned)tq5_checksum(d, count) ^ m[6 + count]) & 0x7f) { r.action = TQ5_ERROR; return r; }
        if (format == 3 && count == TQ5_VCED) {
            memcpy(edit->vced, d, TQ5_VCED); edit->opmask = 15;
            tq5_clamp(edit);
            r.action = TQ5_VOICE_EDIT; r.changed = TQ5_CHANGED_VCED;
        } else if (format == 4 && count == 32 * TQ5_VMEM) {
            r.action = TQ5_VMEM_BLOCK; r.vmem = d;
        } else if (format == 0x7E && count >= 10) {
            const uint8_t *data = d + 10; unsigned n = count - 10;
            if (lm_is(d, hdr_aced) && n == TQ5_ACED) { memcpy(edit->aced, data, n); r.changed = TQ5_CHANGED_ACED; }
            else if (lm_is(d, hdr_aced2) && n == TQ5_ACED2) { memcpy(edit->aced2, data, n); r.changed = TQ5_CHANGED_ACED2; }
            else if (lm_is(d, hdr_efeds) && n == TQ5_EFEDS) { memcpy(edit->efeds, data, n); r.changed = TQ5_CHANGED_EFEDS; }
            else return r;   /* other LM blocks (PCED, ACED3, ...) are not ours */
            tq5_clamp(edit);
            r.action = TQ5_VOICE_EDIT;
        } else {
            r.action = TQ5_ERROR;
        }
        return r;
    }
    if (sub == 0x10 && len == 7) {                       /* parameter change */
        unsigned g = m[3], pp = m[4], dd = m[5];
        r.param_group = (uint8_t)g; r.pp = (uint8_t)pp; r.value = (uint8_t)dd;
        if (g == 0x12) {
            if (pp < TQ5_VCED) { edit->vced[pp] = (uint8_t)(dd > tq5_vced_max[pp] ? tq5_vced_max[pp] : dd); }
            else if (pp == 93) edit->opmask = (uint8_t)(dd & 15);
            else return r;
            r.action = TQ5_VOICE_EDIT; r.changed = TQ5_CHANGED_VCED;
        } else if (g == 0x13) {
            if (pp < TQ5_ACED) { edit->aced[pp] = (uint8_t)(dd > tq5_aced_max[pp] ? tq5_aced_max[pp] : dd); r.changed = TQ5_CHANGED_ACED; }
            else if (pp < TQ5_ACED + TQ5_ACED2) {
                unsigned i = pp - TQ5_ACED;
                edit->aced2[i] = (uint8_t)(dd > tq5_aced2_max[i] ? tq5_aced2_max[i] : dd); r.changed = TQ5_CHANGED_ACED2;
            } else return r;
            r.action = TQ5_VOICE_EDIT;
        } else if (g == 0x24) {
            if (pp >= 4 && pp <= 6) {
                edit->efeds[pp - 4] = (uint8_t)dd; tq5_clamp(edit);
                r.action = TQ5_VOICE_EDIT; r.changed = TQ5_CHANGED_EFEDS;
            } else if (pp == 7) {
                r.action = TQ5_BLOCK_SELECT; r.block = (uint8_t)((dd >= 1 && dd <= 4) ? dd - 1 : 0);
            } else if (pp >= 91) {
                r.action = TQ5_REMOTE;
            } else {
                r.action = TQ5_SYS_PARAM;
            }
        } else if (g == 0x10) {
            if (pp < 96) { r.action = TQ5_MULTI_PARAM; r.part = (uint8_t)(pp / 12); r.pp = (uint8_t)(pp % 12); }
            else if (pp == 123) r.action = TQ5_MASTER_TUNE;
        }
        return r;
    }
    if (sub == 0x20) {                                   /* dump request */
        if (len == 5) {
            if (m[3] == 3) { r.action = TQ5_DUMP_REQUEST; r.request = TQ5_REQ_VCED; }
            else if (m[3] == 4) { r.action = TQ5_DUMP_REQUEST; r.request = TQ5_REQ_VMEM; }
        } else if (len == 15 && m[3] == 0x7E) {
            const uint8_t *h = m + 4;
            r.action = TQ5_DUMP_REQUEST;
            if (lm_is(h, hdr_aced)) r.request = TQ5_REQ_ACED;
            else if (lm_is(h, hdr_aced2)) r.request = TQ5_REQ_ACED2;
            else if (lm_is(h, hdr_efeds)) r.request = TQ5_REQ_EFEDS;
            else r.action = TQ5_NONE;
        }
        return r;
    }
    return r;
}

size_t tq5_reply(const tq5_voice *edit, unsigned request, unsigned ch, uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (cap < TQ5_FULL_VOICE_BYTES) return 0;
    switch (request) {
    case TQ5_REQ_EFEDS: n += tq5_msg_efeds(edit, ch, out + n); /* fallthrough */
    case TQ5_REQ_ACED2: n += tq5_msg_aced2(edit, ch, out + n); /* fallthrough */
    case TQ5_REQ_ACED:  n += tq5_msg_aced(edit, ch, out + n);  /* fallthrough */
    case TQ5_REQ_VCED:  n += tq5_msg_vced(edit, ch, out + n); break;
    case TQ5_REQ_EFEDS_ONLY: n += tq5_msg_efeds(edit, ch, out + n); break;
    default: break;
    }
    return n;
}
