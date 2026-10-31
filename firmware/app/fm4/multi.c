/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 multitimbral layer. See multi.h. */
#include "multi.h"
#include <string.h>

#define NO_PART 0xff

void multi_init(multi_state *m, unsigned polyphony)
{
    memset(m, 0, sizeof *m);
    opz_init(&m->eng, polyphony);
    m->master_volume = 100;
    for (unsigned p = 0; p < MULTI_PARTS; ++p) {
        multi_part *pt = &m->part[p];
        pt->cfg.enabled = p == 0;
        pt->cfg.channel = (uint8_t)p;
        pt->cfg.reserve = p == 0 ? 8 : 0;
        pt->cfg.note_lo = 0; pt->cfg.note_hi = 127;
        pt->cfg.volume = 90; pt->cfg.pan = 64; pt->cfg.fx_send = 40;
        pt->cc_volume = 100; pt->expression = 127;
        pt->data.perf.pb_range = 2; pt->data.perf.transpose = 24;
    }
    memset(m->slot_part, NO_PART, sizeof m->slot_part);
}

void multi_set_voice(multi_state *m, unsigned part, const multi_voice_data *v)
{
    if (part >= MULTI_PARTS || !v) return;
    m->part[part].data = *v;
    opz_lfo_setup(&m->eng, part, &v->patch);
}

static FM4_AUDIO_CODE void part_gain(const multi_state *m, const multi_part *pt, int32_t *gl, int32_t *gr)
{
    /* volume 0..99 x CC7 x CC11 x master, roughly exponential via squares */
    uint32_t v = (uint32_t)pt->cfg.volume * 331U;              /* 0..32769 */
    v = (v * v) >> 15;
    v = (v * pt->cc_volume) / 127U;
    v = (v * pt->expression) / 127U;
    v = (v * m->master_volume) / 127U;
    if (pt->data.perf.fc_volume && pt->foot < 127)
        v = v - ((v * pt->data.perf.fc_volume * (127U - pt->foot)) / (99U * 127U));
    uint32_t pan = pt->cfg.pan > 127 ? 127 : pt->cfg.pan;
    /* simple sine-ish pan law: 2x linear, clipped */
    uint32_t l = pan <= 64 ? 128 : (127U - pan) * 2U;
    uint32_t r = pan >= 64 ? 128 : pan * 2U;
    *gl = (int32_t)((v * l) >> 7);
    *gr = (int32_t)((v * r) >> 7);
}

static FM4_AUDIO_CODE int part_bend64(const multi_state *m, const multi_part *pt)
{
    int range = pt->data.perf.pb_range > 12 ? 12 : pt->data.perf.pb_range;
    int bend = (pt->bend * range * 64) / 8192;
    return bend + pt->cfg.detune * 2 + m->master_tune;
}

static FM4_AUDIO_CODE void part_mod(multi_state *m, unsigned p)
{
    const multi_part *pt = &m->part[p];
    const multi_perf *f = &pt->data.perf;
    unsigned pm = (f->mw_pitch * pt->modwheel) / 99U + (f->bc_pitch * pt->breath) / 99U;
    unsigned am = (f->mw_amp * pt->modwheel) / 99U + (f->bc_amp * pt->breath) / 99U;
    pm += (f->at_pitch * pt->aftertouch) / 99U;
    am += (f->at_amp * pt->aftertouch) / 99U;
    if (f->fc_pitch) pm += (f->fc_pitch * pt->foot) / 99U;
    if (f->fc_amp) am += (f->fc_amp * pt->foot) / 99U;
    opz_lfo_extra(&m->eng, p, pm, am);
}

static FM4_AUDIO_CODE void refresh_part(multi_state *m, unsigned p)
{
    int32_t gl, gr; part_gain(m, &m->part[p], &gl, &gr);
    int bend = part_bend64(m, &m->part[p]);
    for (unsigned s = 0; s < m->eng.voices; ++s) {
        if (m->slot_part[s] != p || !m->eng.v[s].active) continue;
        m->eng.v[s].gain_l = gl; m->eng.v[s].gain_r = gr;
        opz_set_bend(&m->eng, s, bend);
    }
}

static FM4_AUDIO_CODE unsigned part_voices(const multi_state *m, unsigned p)
{
    unsigned n = 0;
    for (unsigned s = 0; s < m->eng.voices; ++s)
        if (m->slot_part[s] == p && m->eng.v[s].active) ++n;
    return n;
}

/* Choose a slot for a new note on part p. Steal order: released voices of
 * parts over their reserve, own released voices, held voices of parts over
 * their reserve, own held voices. Voices inside another part's reserve are
 * never taken. Oldest first within a class. */
static FM4_AUDIO_CODE int alloc_slot(multi_state *m, unsigned p)
{
    int best = -1, best_rank = 99; uint32_t best_age = 0xffffffffU;
    for (unsigned s = 0; s < m->eng.voices; ++s) {
        if (!m->eng.v[s].active) return (int)s;
        unsigned owner = m->slot_part[s];
        int over = owner >= MULTI_PARTS || part_voices(m, owner) > m->part[owner].cfg.reserve;
        int held = m->eng.v[s].keyed;
        int rank;
        if (owner != p && !over) continue;
        if (owner == p) rank = held ? 3 : 1;
        else rank = held ? 2 : 0;
        if (rank < best_rank || (rank == best_rank && m->eng.v[s].age < best_age)) {
            best = (int)s; best_rank = rank; best_age = m->eng.v[s].age;
        }
    }
    return best;
}

static FM4_AUDIO_CODE unsigned porta_rate(unsigned time)
{
    /* 1/64-semitone per block; time 0 = instant, 99 = very slow */
    if (!time) return 0xffff;
    unsigned inv = 100U - (time > 99 ? 99 : time);
    return 1U + (inv * inv) / 24U;
}

static FM4_AUDIO_CODE int effective_note(const multi_part *pt, unsigned note)
{
    int n = (int)note + pt->cfg.note_shift + ((int)pt->data.perf.transpose - 24);
    while (n < 0) n += 12;
    while (n > 127) n -= 12;
    return n;
}

static FM4_AUDIO_CODE void note_on(multi_state *m, unsigned p, unsigned note, unsigned vel)
{
    multi_part *pt = &m->part[p];
    if (note < pt->cfg.note_lo || note > pt->cfg.note_hi) return;
    pt->held[note >> 3] |= (uint8_t)(1U << (note & 7));
    int n = effective_note(pt, note);
    int32_t gl, gr; part_gain(m, pt, &gl, &gr);
    int32_t send = (int32_t)pt->cfg.fx_send * 331;
    const multi_perf *f = &pt->data.perf;
    if (f->poly_mono) {
        /* mono: keep a small key stack for last-note priority */
        if (pt->mono_count < sizeof pt->mono_stack) pt->mono_stack[pt->mono_count++] = (uint8_t)note;
        for (unsigned s = 0; s < m->eng.voices; ++s) {
            if (m->slot_part[s] != p || !m->eng.v[s].active) continue;
            if (m->eng.v[s].keyed && f->porta_mode == 1 && f->porta_time) {
                /* fingered legato: glide without retrigger */
                pt->porta_from64 = m->eng.v[s].pitch64;
                pt->porta_to64 = (uint16_t)(n * 64);
                pt->porta_active = 1;
                m->slot_note[s] = (uint8_t)note;
                pt->last_note = (uint8_t)n;
                return;
            }
            opz_kill(&m->eng, s);
        }
    }
    int slot = alloc_slot(m, p);
    if (slot < 0) return;
    if (m->eng.v[slot].active) opz_kill(&m->eng, (unsigned)slot);
    opz_lfo_keyon(&m->eng, p);
    opz_key_on(&m->eng, (unsigned)slot, &pt->data.patch, p, (unsigned)n, vel, gl, gr, send);
    opz_set_bend(&m->eng, (unsigned)slot, part_bend64(m, pt));
    m->slot_part[slot] = (uint8_t)p;
    m->slot_note[slot] = (uint8_t)note;
    m->slot_sustained[slot] = 0;
    if (f->porta_time && (f->poly_mono || f->porta_mode == 0) && pt->last_note) {
        pt->porta_from64 = (uint16_t)(pt->last_note * 64);
        pt->porta_to64 = (uint16_t)(n * 64);
        pt->porta_active = 1;
        opz_set_pitch(&m->eng, (unsigned)slot, pt->porta_from64);
    }
    pt->last_note = (uint8_t)n;
}

static FM4_AUDIO_CODE void note_off(multi_state *m, unsigned p, unsigned note)
{
    multi_part *pt = &m->part[p];
    pt->held[note >> 3] &= (uint8_t)~(1U << (note & 7));
    if (pt->data.perf.poly_mono) {
        unsigned w = 0;
        for (unsigned i = 0; i < pt->mono_count; ++i)
            if (pt->mono_stack[i] != note) pt->mono_stack[w++] = pt->mono_stack[i];
        pt->mono_count = (uint8_t)w;
        if (w && !pt->sustain_pedal) {
            /* return to previous held key */
            unsigned prev = pt->mono_stack[w - 1];
            int n = effective_note(pt, prev);
            for (unsigned s = 0; s < m->eng.voices; ++s) {
                if (m->slot_part[s] != p || !m->eng.v[s].keyed) continue;
                m->slot_note[s] = (uint8_t)prev;
                if (pt->data.perf.porta_time) {
                    pt->porta_from64 = m->eng.v[s].pitch64; pt->porta_to64 = (uint16_t)(n * 64); pt->porta_active = 1;
                } else opz_set_pitch(&m->eng, s, (unsigned)n * 64U);
                pt->last_note = (uint8_t)n;
            }
            return;
        }
    }
    for (unsigned s = 0; s < m->eng.voices; ++s) {
        if (m->slot_part[s] != p || m->slot_note[s] != note || !m->eng.v[s].keyed) continue;
        if (pt->sustain_pedal) m->slot_sustained[s] = 1;
        else opz_key_off(&m->eng, s);
    }
}

FM4_AUDIO_CODE void multi_note(multi_state *m, unsigned part, unsigned note, unsigned velocity)
{
    if (part >= MULTI_PARTS || note > 127) return;
    if (velocity) note_on(m, part, note, velocity);
    else note_off(m, part, note);
}

FM4_AUDIO_CODE void multi_all_off(multi_state *m, int hard)
{
    for (unsigned s = 0; s < OPZ_MAX_VOICES; ++s) {
        if (hard) opz_kill(&m->eng, s); else opz_key_off(&m->eng, s);
        m->slot_sustained[s] = 0;
    }
    for (unsigned p = 0; p < MULTI_PARTS; ++p) {
        fm4_zero(m->part[p].held, sizeof m->part[p].held);
        m->part[p].mono_count = 0; m->part[p].sustain_pedal = 0;
    }
}

FM4_AUDIO_CODE unsigned multi_parts_for_channel(const multi_state *m, unsigned ch)
{
    unsigned mask = 0;
    for (unsigned p = 0; p < MULTI_PARTS; ++p) {
        const multi_part_cfg *c = &m->part[p].cfg;
        if (!c->enabled || c->channel == MULTI_OFF) continue;
        if (c->channel == MULTI_OMNI || c->channel == ch) mask |= 1U << p;
    }
    return mask;
}

static FM4_AUDIO_CODE void controller(multi_state *m, unsigned p, unsigned cc, unsigned v)
{
    multi_part *pt = &m->part[p];
    switch (cc) {
    case 1: pt->modwheel = (uint8_t)v; part_mod(m, p); break;
    case 2: pt->breath = (uint8_t)v; part_mod(m, p); break;
    case 4: pt->foot = (uint8_t)v; part_mod(m, p); refresh_part(m, p); break;
    case 5: pt->data.perf.porta_time = (uint8_t)(v * 99U / 127U); break;
    case 7: pt->cc_volume = (uint8_t)v; refresh_part(m, p); break;
    case 10: pt->cfg.pan = (uint8_t)v; refresh_part(m, p); break;
    case 11: pt->expression = (uint8_t)v; refresh_part(m, p); break;
    case 64:
        pt->sustain_pedal = v >= 64;
        if (!pt->sustain_pedal)
            for (unsigned s = 0; s < m->eng.voices; ++s)
                if (m->slot_part[s] == p && m->slot_sustained[s]) {
                    m->slot_sustained[s] = 0;
                    opz_key_off(&m->eng, s);
                }
        break;
    case 120:
        for (unsigned s = 0; s < m->eng.voices; ++s) if (m->slot_part[s] == p) opz_kill(&m->eng, s);
        break;
    case 121:
        pt->bend = 0; pt->modwheel = 0; pt->breath = 0; pt->aftertouch = 0; pt->expression = 127; pt->sustain_pedal = 0;
        part_mod(m, p); refresh_part(m, p);
        break;
    case 123:
        for (unsigned s = 0; s < m->eng.voices; ++s)
            if (m->slot_part[s] == p) { m->slot_sustained[s] = 0; opz_key_off(&m->eng, s); }
        pt->mono_count = 0; fm4_zero(pt->held, sizeof pt->held);
        break;
    default: break;
    }
}

FM4_AUDIO_CODE void multi_midi(multi_state *m, const uint8_t *msg, unsigned len)
{
    if (!len || msg[0] < 0x80 || msg[0] >= 0xf0) return;
    unsigned kind = msg[0] & 0xf0, ch = msg[0] & 15;
    unsigned d1 = len > 1 ? msg[1] & 127 : 0, d2 = len > 2 ? msg[2] & 127 : 0;
    unsigned parts = multi_parts_for_channel(m, ch);
    for (unsigned p = 0; parts; ++p, parts >>= 1) {
        if (!(parts & 1)) continue;
        switch (kind) {
        case 0x90: if (d2) { note_on(m, p, d1, d2); break; } /* fallthrough */
        case 0x80: note_off(m, p, d1); break;
        case 0xb0: controller(m, p, d1, d2); break;
        case 0xe0: m->part[p].bend = (int16_t)(((int)(d2 << 7 | d1)) - 8192); refresh_part(m, p); break;
        case 0xd0: m->part[p].aftertouch = (uint8_t)d1; part_mod(m, p); break;
        default: break;
        }
    }
}

FM4_AUDIO_CODE void multi_render(multi_state *m, int32_t *dry, int32_t *send)
{
    for (unsigned p = 0; p < MULTI_PARTS; ++p) {
        multi_part *pt = &m->part[p];
        if (!pt->porta_active) continue;
        unsigned rate = porta_rate(pt->data.perf.porta_time);
        int from = pt->porta_from64, to = pt->porta_to64;
        if (from < to) from = from + (int)rate > to ? to : from + (int)rate;
        else from = from - (int)rate < to ? to : from - (int)rate;
        pt->porta_from64 = (uint16_t)from;
        if (from == to) pt->porta_active = 0;
        for (unsigned s = 0; s < m->eng.voices; ++s)
            if (m->slot_part[s] == p && m->eng.v[s].active) opz_set_pitch(&m->eng, s, (unsigned)from);
    }
    opz_render(&m->eng, dry, send);
}

unsigned multi_active_voices(const multi_state *m)
{
    unsigned n = 0;
    for (unsigned s = 0; s < m->eng.voices; ++s) n += m->eng.v[s].active ? 1U : 0U;
    return n;
}
