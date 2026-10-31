/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 engine for the browser: the same C sources as the firmware, compiled
 * to freestanding WebAssembly. Used by the webapp's AudioWorklet preview and
 * as the single source of truth for TQ5 SysEx encoding/decoding. */
#include "tq5.h"
#include "fx.h"
#include "seq.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static multi_state g_multi;
static fx_unit g_fx;
static tq5_voice g_edit[MULTI_PARTS];
static int32_t g_dry[FM4_BLOCK * 2], g_send[FM4_BLOCK * 2];
static float g_out[FM4_BLOCK * 2];
static uint8_t g_io[8192 + 16];     /* SysEx in/out; fm4_vmem_bulk uses [0,4104) + [4104,8200) */
static tq5_result g_last;

/* freestanding libc pieces the engine needs */
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *p = d; const uint8_t *q = s;
    if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n; --n, ++p, ++q) if (*p != *q) return *p - *q;
    return 0;
}

static void apply(unsigned part)
{
    multi_voice_data d;
    tq5_to_engine(&g_edit[part], &d);
    multi_set_voice(&g_multi, part, &d);
    if (part == 0) { fx_params f; tq5_effect(&g_edit[0], &f); fx_set(&g_fx, &f); }
}

EXPORT(fm4_init) void fm4_init(unsigned polyphony)
{
    multi_init(&g_multi, polyphony);
    fx_init(&g_fx);
    for (unsigned p = 0; p < MULTI_PARTS; ++p) { tq5_voice_init(&g_edit[p]); apply(p); }
}
EXPORT(fm4_io) uint8_t *fm4_io(void) { return g_io; }
EXPORT(fm4_out) float *fm4_out(void) { return g_out; }
EXPORT(fm4_voice_ptr) uint8_t *fm4_voice_ptr(unsigned part) { return (uint8_t *)&g_edit[part & 7]; }
EXPORT(fm4_voice_size) unsigned fm4_voice_size(void) { return sizeof(tq5_voice); }
EXPORT(fm4_voice_commit) void fm4_voice_commit(unsigned part) { tq5_clamp(&g_edit[part & 7]); apply(part & 7); }
EXPORT(fm4_voice_reset) void fm4_voice_reset(unsigned part) { tq5_voice_init(&g_edit[part & 7]); apply(part & 7); }

/* MIDI channel messages from the keyboard / Web MIDI input. */
EXPORT(fm4_midi) void fm4_midi(unsigned status, unsigned d1, unsigned d2)
{
    uint8_t m[3] = { (uint8_t)status, (uint8_t)d1, (uint8_t)d2 };
    multi_midi(&g_multi, m, 3);
}
EXPORT(fm4_all_off) void fm4_all_off(void) { multi_all_off(&g_multi, 1); }
EXPORT(fm4_part_config) void fm4_part_config(unsigned part, unsigned enabled, unsigned channel, unsigned reserve,
                                             unsigned volume, unsigned pan, unsigned send)
{
    multi_part_cfg *c = &g_multi.part[part & 7].cfg;
    c->enabled = (uint8_t)enabled; c->channel = (uint8_t)channel; c->reserve = (uint8_t)reserve;
    c->volume = (uint8_t)volume; c->pan = (uint8_t)pan; c->fx_send = (uint8_t)send;
}

/* Render one 128-frame block of interleaved float stereo into fm4_out(). */
EXPORT(fm4_render) unsigned fm4_render(void)
{
    for (unsigned i = 0; i < FM4_BLOCK * 2; ++i) g_dry[i] = g_send[i] = 0;
    multi_render(&g_multi, g_dry, g_send);
    fx_render(&g_fx, g_send, g_dry);
    for (unsigned i = 0; i < FM4_BLOCK * 2; ++i) g_out[i] = (float)g_dry[i] * (1.0f / 8388608.0f);
    return multi_active_voices(&g_multi);
}

/* ---- SysEx helpers (byte buffer = fm4_io) ---- */
/* Parse a message in g_io[0..len) into part's edit buffer; returns action. */
EXPORT(fm4_sysex_parse) unsigned fm4_sysex_parse(unsigned part, unsigned len, unsigned rx_channel)
{
    g_last = tq5_parse(g_io, len, rx_channel, &g_edit[part & 7]);
    if (g_last.action == TQ5_VOICE_EDIT) apply(part & 7);
    return g_last.action;
}
EXPORT(fm4_sysex_result) unsigned fm4_sysex_result(unsigned field)
{
    switch (field) {
    case 0: return g_last.changed; case 1: return g_last.block; case 2: return g_last.request;
    case 3: return g_last.part; case 4: return g_last.pp; case 5: return g_last.value;
    case 6: return g_last.vmem ? (unsigned)(g_last.vmem - g_io) : 0;
    default: return 0;
    }
}
/* Build the full 191-byte TQ5 voice dump (or request kind) into g_io. */
EXPORT(fm4_sysex_dump) unsigned fm4_sysex_dump(unsigned part, unsigned request, unsigned ch)
{
    return (unsigned)tq5_reply(&g_edit[part & 7], request, ch, g_io, sizeof g_io);
}
/* VMEM record <-> voice; record at g_io[0..128). */
EXPORT(fm4_vmem_pack) void fm4_vmem_pack(unsigned part) { tq5_vmem_pack(&g_edit[part & 7], g_io); }
EXPORT(fm4_vmem_unpack) void fm4_vmem_unpack(unsigned part) { tq5_vmem_unpack(g_io, &g_edit[part & 7]); apply(part & 7); }
/* Build a VMEM bulk from 32 records placed at g_io[4104..8200) into g_io[0..4104). */
EXPORT(fm4_vmem_bulk) unsigned fm4_vmem_bulk(unsigned ch)
{
    static uint8_t recs[32][TQ5_VMEM];
    memcpy(recs, g_io + 4104, sizeof recs);
    return (unsigned)tq5_msg_vmem((const uint8_t (*)[TQ5_VMEM])recs, ch, g_io);
}
EXPORT(fm4_param_msg) unsigned fm4_param_msg(unsigned ch, unsigned group, unsigned pp, unsigned value)
{
    return (unsigned)tq5_msg_param(ch, group, pp, value, g_io);
}
EXPORT(fm4_ratio_x100) unsigned fm4_ratio_x100(unsigned crs, unsigned fine) { return opz_ratio_x100(crs, fine); }
EXPORT(fm4_fixed_hz) unsigned fm4_fixed_hz(unsigned crs, unsigned rg, unsigned fine) { return opz_fixed_hz(crs, rg, fine); }
EXPORT(fm4_vced_max) unsigned fm4_vced_max(unsigned i) { return i < TQ5_VCED ? tq5_vced_max[i] : 0; }
EXPORT(fm4_aced_max) unsigned fm4_aced_max(unsigned i) { return i < TQ5_ACED ? tq5_aced_max[i] : 0; }
