/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 core. See fm4_core.h.
 *
 * Threading model (matches the FM-1 SDK): the control task owns all state
 * changes; the audio interrupt only renders. They meet through
 *  - two single-producer rings of short MIDI messages (routing task, panel),
 *  - per-part "staged voice" slots handed over with a ready bit,
 *  - an audio->control ring for program changes and MIDI out.
 * Nothing in the audio path calls libc (it lives in Flash). */
#include "fm4_core.h"
#include <stddef.h>
#include <string.h>

/* ---- Storage layout inside the FM-1 user partition --------------------- */
/* Layout 2 (FM-4 1.1): everything inside FM4_FLASH_WINDOW (see fm4_core.h).
 *   0x00000  system + multi setup        1 sector   record v1 (unchanged from layout 1)
 *   0x01000  user voice bank             4 sectors  record v1 (unchanged from layout 1)
 *   0x05000  songs 0..7, 0x2000 each    16 sectors  record v2, only the used events
 *   0x15000  layout marker               1 sector   "FM4L", version 2
 *   0x16000  spare                       1 sector
 * Layout 1 (FM-4 1.0) kept 9-sector songs at 0x05000 + n * 0x09000, so songs
 * 3..8 (n = 2..7) sat at flash 0x93000..0xC8FFF: inside stock V15's settings
 * store, song 3 exactly on its first sector. migrate_layout() moves them into
 * layout 2 on the first boot and erases the old records. */
#define SECTOR 4096U
#define OFF_SYSTEM   0x00000U
#define OFF_VOICES   0x01000U          /* 4 sectors */
#define OFF_SONGS    0x05000U
#define OFF_LAYOUT   0x15000U
#define MAGIC_SYSTEM 0x53344D46U       /* "FM4S" */
#define MAGIC_VOICES 0x56344D46U       /* "FM4V" */
#define MAGIC_SONG   0x47344D46U       /* "FM4G" */
#define MAGIC_LAYOUT 0x4C344D46U       /* "FM4L" */
#define LEGACY_SONGS  0x05000U         /* layout 1 */
#define LEGACY_STRIDE 0x09000U
#define LEGACY_SONG_SECTORS 9U
#define SONG_HEAD ((uint32_t)offsetof(seq_song, ev))
typedef struct { uint32_t magic, version, bytes, crc; } rec_header;
#define SONG_CAPACITY ((FM4_SONG_SLOT_BYTES - (uint32_t)sizeof(rec_header) - SONG_HEAD) / (uint32_t)sizeof(seq_event))
_Static_assert(OFF_SONGS + SEQ_SONGS * FM4_SONG_SLOT_BYTES <= OFF_LAYOUT, "songs overlap the layout marker");
_Static_assert(OFF_LAYOUT + SECTOR <= FM4_FLASH_WINDOW, "FM-4 data must stay below stock's data (flash 0x93000)");
_Static_assert(OFF_VOICES + sizeof(rec_header) + TQ5_BANK_VOICES * TQ5_VMEM <= OFF_SONGS, "voice bank overflows");
_Static_assert(LEGACY_SONGS + LEGACY_STRIDE * 2 == FM4_FLASH_WINDOW, "layout 1 song 3 starts at stock's settings store");

/* ---- State -------------------------------------------------------------- */
static multi_state M;
static fx_unit FX;
static tq5_voice edit_buf[FM4_PARTS];
static fm4_system sys;
static fm4_multi_setup setup;
static uint8_t user_bank[TQ5_BANK_VOICES][TQ5_VMEM];
static seq_song song;
static seq_state seq;
static unsigned active_part;
static const char *notice_text;
static uint8_t settings_dirty;

/* staged voice / effect / config hand-over (control -> audio) */
static multi_voice_data staged_voice[FM4_PARTS];
static fx_params staged_fx;
/* one byte per hand-over item, set by control, cleared by audio:
 * 0..7 voices, 8 fx, 9 part config, 10 all notes off */
enum { ST_FX = 8, ST_CFG = 9, ST_PANIC = 10, ST_ITEMS = 11 };
static volatile uint8_t staged[ST_ITEMS];
static uint32_t restage_mask;               /* control: want to stage again */
static multi_part_cfg staged_cfg[FM4_PARTS];

/* rings */
#define RING 256U
typedef struct { volatile uint32_t r, w; uint32_t d[RING]; } ring_t;
static ring_t ring_midi, ring_local, ring_back;
static volatile uint8_t clock_pulses, clock_cmd;

/* SysEx input queue (routing task -> control) */
#define SX_SLOTS 4
#define SX_MAX 1024
static struct { volatile uint16_t len; uint8_t data[SX_MAX]; } sx_in[SX_SLOTS];
static uint8_t sx_head, sx_tail;
static uint8_t bank_dump_block;            /* 1..4 = next VMEM block to send */

/* MIDI output queue (control task) */
#define OUT_BYTES 4608
static uint8_t out_buf[OUT_BYTES];
static unsigned out_len, out_pos, out_slice;

/* metronome click */
static int32_t click_env, click_phase, click_step;

static uint32_t crc32(const void *p, size_t n)
{
    const uint8_t *b = p; uint32_t c = 0xffffffffU;
    while (n--) { c ^= *b++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U))); }
    return ~c;
}

static FM4_AUDIO_CODE int ring_put(ring_t *q, uint32_t v)
{
    uint32_t w = q->w;
    if (w - q->r >= RING) return -1;
    q->d[w & (RING - 1)] = v;
    __asm__ volatile("" ::: "memory");
    q->w = w + 1;
    return 0;
}
static FM4_AUDIO_CODE int ring_get(ring_t *q, uint32_t *v)
{
    uint32_t r = q->r;
    if (r == q->w) return 0;
    *v = q->d[r & (RING - 1)];
    __asm__ volatile("" ::: "memory");
    q->r = r + 1;
    return 1;
}
static uint32_t pack3(const uint8_t *m, unsigned n)
{
    return m[0] | ((n > 1 ? (uint32_t)m[1] : 0) << 8) | ((n > 2 ? (uint32_t)m[2] : 0) << 16);
}

/* ---- Voices ---------------------------------------------------------------- */
void fm4_voice_get(unsigned bank, unsigned slot, tq5_voice *out)
{
    slot %= TQ5_BANK_VOICES;
    tq5_vmem_unpack(bank == FM4_BANK_USER ? user_bank[slot] : fm4_preset_bank[slot], out);
}

static void stage_voice(unsigned part)
{
    if (staged[part] || (part == 0 && staged[ST_FX])) { restage_mask |= 1U << part; return; }
    tq5_to_engine(&edit_buf[part], &staged_voice[part]);
    if (part == 0) { tq5_effect(&edit_buf[0], &staged_fx); staged[ST_FX] = 1; }
    staged[part] = 1;
}

void fm4_edit_commit(unsigned part)
{
    part &= 7;
    tq5_clamp(&edit_buf[part]);
    stage_voice(part);
}

tq5_voice *fm4_edit(unsigned part) { return &edit_buf[part & 7]; }

void fm4_select(unsigned part, unsigned bank, unsigned program)
{
    part &= 7;
    program %= TQ5_BANK_VOICES;
    setup.bank[part] = (uint8_t)(bank ? FM4_BANK_USER : FM4_BANK_PRESET);
    setup.program[part] = (uint8_t)program;
    setup.cfg[part].voice = (uint8_t)(setup.bank[part] * 100 + program);
    fm4_voice_get(setup.bank[part], program, &edit_buf[part]);
    if (part == 0 && !sys.mode) { sys.single_bank = setup.bank[0]; sys.single_program = (uint8_t)program; }
    settings_dirty = 1;
    stage_voice(part);
}

/* The only writer of FM-4 records. Refuses anything that would end outside
 * FM4_FLASH_WINDOW, so no save can reach stock V15's data (fm4_core.h). */
static int save_record_v(uint32_t off, uint32_t magic, uint32_t version, const void *data, size_t n)
{
    /* erase, write body, then the header last so a torn write never validates */
    rec_header h = { magic, version, (uint32_t)n, crc32(data, n) };
    uint32_t end = off + (uint32_t)(sizeof h + n);
    if (end > FM4_FLASH_WINDOW || end > fm4_hw_flash_size()) return -1;
    for (uint32_t s = off & ~(SECTOR - 1); s < end; s += SECTOR) if (fm4_hw_flash_erase(s)) return -2;
    if (fm4_hw_flash_write(off + sizeof h, data, n)) return -3;
    return fm4_hw_flash_write(off, &h, sizeof h) ? -3 : 0;
}
static int save_record(uint32_t off, uint32_t magic, const void *data, size_t n)
{
    return save_record_v(off, magic, 1, data, n);
}

static int load_record(uint32_t off, uint32_t magic, void *data, size_t n)
{
    rec_header h;
    if (off + sizeof h + n > fm4_hw_flash_size()) return -1;
    if (fm4_hw_flash_read(off, &h, sizeof h) || h.magic != magic || h.version != 1 || h.bytes != n) return -1;
    if (fm4_hw_flash_read(off + sizeof h, data, n)) return -1;
    return crc32(data, n) == h.crc ? 0 : -2;
}

/* ---- Songs in flash (layout 2: header + only the used events) ----------- */
static uint32_t song_offset(unsigned n) { return OFF_SONGS + (n & 7) * FM4_SONG_SLOT_BYTES; }
unsigned fm4_song_capacity(void) { return SONG_CAPACITY; }

static int song_write(unsigned n, const seq_song *g)
{
    if (g->count > SONG_CAPACITY) return -4;
    return save_record_v(song_offset(n), MAGIC_SONG, FM4_LAYOUT_VERSION, g,
                         SONG_HEAD + (size_t)g->count * sizeof(seq_event));
}

static int song_read(unsigned n, seq_song *g)
{
    rec_header h;
    uint32_t off = song_offset(n);
    if (off + FM4_SONG_SLOT_BYTES > fm4_hw_flash_size()) return -1;
    if (fm4_hw_flash_read(off, &h, sizeof h) || h.magic != MAGIC_SONG || h.version != FM4_LAYOUT_VERSION) return -1;
    if (h.bytes < SONG_HEAD || h.bytes > SONG_HEAD + SONG_CAPACITY * sizeof(seq_event) ||
        (h.bytes - SONG_HEAD) % sizeof(seq_event)) return -1;
    if (fm4_hw_flash_read(off + sizeof h, g, h.bytes)) return -1;
    if (crc32(g, h.bytes) != h.crc || g->count != (h.bytes - SONG_HEAD) / sizeof(seq_event)) return -2;
    return 0;
}

/* A layout-1 song record header at legacy slot n (FM-4 1.0). */
static int legacy_song_at(uint32_t off)
{
    rec_header h;
    if (off + LEGACY_SONG_SECTORS * SECTOR > fm4_hw_flash_size()) return 0;
    return !fm4_hw_flash_read(off, &h, sizeof h) && h.magic == MAGIC_SONG && h.version == 1 && h.bytes == sizeof(seq_song);
}

/* Erase layout-1 records outside the window (in stock's settings store) and
 * stale layout-1 headers inside it. Only sectors that carry an FM-4 layout-1
 * song header are touched: whatever else is at those addresses belongs to
 * stock and is left alone. Returns the number of failed erases. */
static unsigned erase_legacy(unsigned *erased)
{
    unsigned failed = 0;
    for (unsigned n = 0; n < SEQ_SONGS; ++n) {
        uint32_t off = LEGACY_SONGS + n * LEGACY_STRIDE;
        if (!legacy_song_at(off)) continue;
        /* outside the window the whole record is FM-4's; inside it only the
         * header sector is stale (the rest may hold layout-2 data) */
        unsigned sectors = off >= FM4_FLASH_WINDOW ? LEGACY_SONG_SECTORS : 1;
        for (unsigned s = 0; s < sectors; ++s) {
            if (fm4_hw_flash_erase(off + s * SECTOR)) ++failed; else ++*erased;
        }
    }
    return failed;
}

/* FM-4 record headers (any magic) at sector starts outside the window, up to
 * the end of layout 1's song area. */
static unsigned fm4_records_outside(void)
{
    unsigned found = 0;
    uint32_t end = LEGACY_SONGS + SEQ_SONGS * LEGACY_STRIDE, size = fm4_hw_flash_size();
    for (uint32_t off = FM4_FLASH_WINDOW; off < end && off + SECTOR <= size; off += SECTOR) {
        rec_header h;
        if (fm4_hw_flash_read(off, &h, sizeof h)) continue;
        if (h.magic == MAGIC_SONG || h.magic == MAGIC_SYSTEM || h.magic == MAGIC_VOICES || h.magic == MAGIC_LAYOUT) ++found;
    }
    return found;
}

int fm4_storage_layout(void)
{
    rec_header h;
    if (fm4_hw_flash_size() < FM4_FLASH_WINDOW) return 0;
    if (fm4_hw_flash_read(OFF_LAYOUT, &h, sizeof h)) return 0;
    return h.magic == MAGIC_LAYOUT && h.version == FM4_LAYOUT_VERSION && h.bytes == 0 ? (int)FM4_LAYOUT_VERSION : 0;
}

/* First boot of layout 2: move layout-1 songs (one RAM buffer suffices: new
 * slot n ends before old slot n+1 starts), erase the old records, then mark
 * the layout. Songs longer than a layout-2 slot keep their first events. */
static void migrate_layout(void)
{
    if (fm4_hw_flash_size() < FM4_FLASH_WINDOW || fm4_storage_layout() == (int)FM4_LAYOUT_VERSION) return;
    unsigned moved = 0, cut = 0, erased = 0;
    for (unsigned n = 0; n < SEQ_SONGS; ++n) {
        uint32_t off = LEGACY_SONGS + n * LEGACY_STRIDE;
        if (!legacy_song_at(off)) continue;
        if (load_record(off, MAGIC_SONG, &song, sizeof song) || song.count > SEQ_MAX_EVENTS) continue;
        if (song.count > SONG_CAPACITY) { song.count = (uint16_t)SONG_CAPACITY; ++cut; }
        if (!song_write(n, &song)) ++moved;
    }
    unsigned failed = erase_legacy(&erased);
    if (!failed) {
        rec_header h = { MAGIC_LAYOUT, FM4_LAYOUT_VERSION, 0, 0 };
        if (!fm4_hw_flash_erase(OFF_LAYOUT)) (void)fm4_hw_flash_write(OFF_LAYOUT, &h, sizeof h);
    }
    if (cut) notice_text = "OLD SONGS SHORTENED";
    else if (moved) notice_text = "SONGS MOVED";
}

int fm4_prepare_stock(unsigned *erased, unsigned *remaining)
{
    unsigned e = 0, r = 0;
    int status;
    if (fm4_hw_flash_size() < FM4_FLASH_WINDOW) status = 2;
    else {
        seq_stop(&seq);
        unsigned failed = erase_legacy(&e);
        r = fm4_records_outside();
        status = failed || r ? 1 : 0;
        if (!status) notice_text = "READY FOR STOCK";
    }
    if (erased) *erased = e;
    if (remaining) *remaining = r;
    return status;
}

int fm4_store(unsigned part, unsigned slot)
{
    if (sys.protect) { notice_text = "MEMORY PROTECTED"; return -1; }
    slot %= TQ5_BANK_VOICES;
    tq5_vmem_pack(&edit_buf[part & 7], user_bank[slot]);
    int r = save_record(OFF_VOICES, MAGIC_VOICES, user_bank, sizeof user_bank);
    notice_text = r ? "STORE FAILED" : "STORED";
    if (!r) { setup.bank[part & 7] = FM4_BANK_USER; setup.program[part & 7] = (uint8_t)slot; settings_dirty = 1; }
    return r;
}

int fm4_save_settings(void)
{
    static struct { fm4_system s; fm4_multi_setup m; } blob;
    blob.s = sys; blob.m = setup;
    settings_dirty = 0;
    return save_record(OFF_SYSTEM, MAGIC_SYSTEM, &blob, sizeof blob);
}

/* ---- Modes & parts -------------------------------------------------------- */
static void defaults(void)
{
    memset(&sys, 0, sizeof sys);
    sys.version = 1; sys.rx_channel = 0; sys.tx_channel = 0; sys.sysex_rx = 1;
    sys.master_tune = 64; sys.polyphony = 8; sys.local = 1; sys.velocity = 100;
    memset(&setup, 0, sizeof setup);
    for (unsigned p = 0; p < FM4_PARTS; ++p) {
        multi_part_cfg *c = &setup.cfg[p];
        c->enabled = 1; c->channel = (uint8_t)p; c->reserve = 1;
        c->note_lo = 0; c->note_hi = 127; c->volume = 85; c->pan = 64; c->fx_send = 40;
        setup.bank[p] = FM4_BANK_PRESET; setup.program[p] = (uint8_t)(p * 12 % 100);
    }
}

void fm4_apply_multi(void)
{
    for (unsigned p = 0; p < FM4_PARTS; ++p) {
        staged_cfg[p] = setup.cfg[p];
        if (!sys.mode) {
            /* SINGLE: part 1 owns the basic channel and all 8 voices */
            staged_cfg[p].enabled = p == 0;
            if (p == 0) { staged_cfg[0].channel = sys.rx_channel; staged_cfg[0].reserve = 16;
                          staged_cfg[0].note_lo = 0; staged_cfg[0].note_hi = 127;
                          staged_cfg[0].volume = 90; staged_cfg[0].pan = 64; staged_cfg[0].fx_send = 60;
                          staged_cfg[0].detune = 0; staged_cfg[0].note_shift = 0; }
        }
    }
    staged[ST_CFG] = 1;
}

void fm4_apply_system(void)
{
    M.master_tune = (int8_t)(sys.master_tune - 64);
    if (sys.polyphony < 4 || sys.polyphony > OPZ_MAX_VOICES) sys.polyphony = 8;
    if (M.eng.voices != sys.polyphony) { staged[ST_PANIC] = 1; M.eng.voices = sys.polyphony; }
}

void fm4_set_mode(unsigned multi)
{
    sys.mode = multi ? 1 : 0;
    if (!sys.mode) { active_part = 0; fm4_select(0, sys.single_bank, sys.single_program); }
    else for (unsigned p = 0; p < FM4_PARTS; ++p) fm4_select(p, setup.bank[p], setup.program[p]);
    fm4_apply_multi();
    staged[ST_PANIC] = 1;         /* all notes off */
    settings_dirty = 1;
}

fm4_system *fm4_sys(void) { return &sys; }
fm4_multi_setup *fm4_multi(void) { return &setup; }
unsigned fm4_active_part(void) { return active_part; }
void fm4_set_active_part(unsigned part) { active_part = sys.mode ? (part & 7) : 0; }
const char *fm4_notice(void) { const char *n = notice_text; notice_text = 0; return n; }

/* ---- Songs ---------------------------------------------------------------- */
static FM4_AUDIO_CODE void seq_emit(void *ctx, unsigned track, unsigned part, uint8_t status, uint8_t d1, uint8_t d2)
{
    (void)ctx;
    if (track >= SEQ_TRACKS) {                    /* metronome */
        click_env = d2 << 16; click_phase = 0; click_step = d1 == 77 ? 0x0B000000 : 0x08000000;
        return;
    }
    (void)part;
    uint8_t m[3] = { status, d1, d2 };
    multi_midi(&M, m, 3);
    ring_put(&ring_back, (uint32_t)status | ((uint32_t)d1 << 8) | ((uint32_t)d2 << 16) | (1U << 31));
}

seq_state *fm4_seq(void) { return &seq; }
static uint8_t record_part;
void fm4_record(unsigned track)
{
    seq.ext_clock = sys.clock_ext;
    record_part = (uint8_t)active_part;
    seq_record(&seq, track & 7);
}

int fm4_song_load(unsigned n)
{
    seq_stop(&seq);
    n &= 7;
    if (song_read(n, &song)) seq_song_clear(&song, n);
    if (song.count > SEQ_MAX_EVENTS) seq_song_clear(&song, n);
    sys.song = (uint8_t)n; settings_dirty = 1;
    seq_locate(&seq, 0);
    return 0;
}

int fm4_song_save(void)
{
    int r = song_write(sys.song & 7, &song);
    notice_text = r == -4 ? "SONG TOO LONG" : r ? "SONG SAVE FAILED" : "SONG SAVED";
    return r;
}

void fm4_transport(unsigned action)
{
    switch (action) {
    case 0:
        if (seq.state == SEQ_RECORDING) { seq_stop(&seq); if (seq_merge(&seq)) notice_text = "TAKE MERGED"; }
        else seq_stop(&seq);
        break;
    case 1: seq.ext_clock = sys.clock_ext; seq_play(&seq); break;
    case 2: fm4_record(active_part); break;
    }
}

/* ---- Init ------------------------------------------------------------------ */
void fm4_core_init(void)
{
    defaults();
    static struct { fm4_system s; fm4_multi_setup m; } blob;
    if (!load_record(OFF_SYSTEM, MAGIC_SYSTEM, &blob, sizeof blob) && blob.s.version == 1) {
        sys = blob.s; setup = blob.m;
    }
    if (sys.polyphony < 4 || sys.polyphony > OPZ_MAX_VOICES) sys.polyphony = 8;
    if (load_record(OFF_VOICES, MAGIC_VOICES, user_bank, sizeof user_bank))
        memcpy(user_bank, fm4_preset_bank, sizeof user_bank);   /* first boot: user = copy of presets */
    multi_init(&M, sys.polyphony);
    fx_init(&FX);
    M.master_tune = (int8_t)(sys.master_tune - 64);
    seq_init(&seq, &song, seq_emit, NULL);
    migrate_layout();                 /* layout 1 -> 2 (uses the song buffer) */
    fm4_song_load(sys.song);
    fm4_set_mode(sys.mode);
    settings_dirty = 0;
}

/* ---- Input ----------------------------------------------------------------- */
int fm4_post(const uint8_t *m, unsigned n)
{
    if (!n || n > 3) return -1;
    if (m[0] == 0xF8 || m[0] == 0xFA || m[0] == 0xFB || m[0] == 0xFC) {
        if (m[0] == 0xF8) ++clock_pulses; else clock_cmd = m[0];
        return 0;
    }
    return ring_put(&ring_midi, pack3(m, n));
}
int fm4_post_local(const uint8_t *m, unsigned n)
{
    if (!n || n > 3) return -1;
    return ring_put(&ring_local, pack3(m, n));
}
void fm4_sysex_in(const uint8_t *m, size_t n)
{
    if (n < 4 || n > SX_MAX) return;
    unsigned slot = sx_head % SX_SLOTS;
    if (sx_in[slot].len) return;              /* queue full: drop */
    memcpy(sx_in[slot].data, m, n);
    __asm__ volatile("" ::: "memory");
    sx_in[slot].len = (uint16_t)n;
    ++sx_head;
}

/* ---- Audio ------------------------------------------------------------------ */
static FM4_AUDIO_CODE void handle_event(uint32_t e, int local)
{
    uint8_t m[3] = { (uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16) };
    unsigned kind = m[0] & 0xF0;
    if (kind == 0xC0 || (kind == 0xB0 && (m[1] == 0 || m[1] == 126 || m[1] == 127))) {
        ring_put(&ring_back, e);             /* program/bank/mode changes: control task */
        if (kind == 0xC0) return;
    }
    if (local) {
        /* panel keys address the active part on its own channel */
        unsigned ch = M.part[active_part].cfg.channel;
        m[0] = (uint8_t)(kind | (ch < 16 ? ch : 0));
        if (kind == 0x80 || kind == 0x90) {
            multi_note(&M, active_part, m[1], kind == 0x90 ? m[2] : 0);
            if (seq.state == SEQ_RECORDING) seq_capture(&seq, (uint8_t)(kind | active_part), m[1], kind == 0x90 ? m[2] : 0);
            return;
        }
    }
    multi_midi(&M, m, 3);
    if (seq.state == SEQ_RECORDING && !local) {
        unsigned parts = multi_parts_for_channel(&M, m[0] & 15);
        if (parts & (1U << active_part)) seq_capture(&seq, (uint8_t)(kind | active_part), m[1], m[2]);
    }
}

FM4_AUDIO_CODE void fm4_midi_clock_in(uint8_t status) { if (status == 0xF8) ++clock_pulses; }

FM4_AUDIO_CODE void fm4_core_render(int32_t *out)
{
    for (unsigned p = 0; p < FM4_PARTS; ++p) if (staged[p]) {
        fm4_copy(&M.part[p].data, &staged_voice[p], sizeof staged_voice[p]);
        opz_lfo_setup(&M.eng, p, &staged_voice[p].patch);
        staged[p] = 0;
    }
    if (staged[ST_FX]) {
        const fx_params *f = &staged_fx;
        if (f->algo == FX.p.algo) {
            /* same algorithm keeps its delay memory: update coefficients only */
            fm4_copy(&FX.p, f, sizeof *f);
            FX.delay_len = 441 + (int32_t)((FX_DELAY_SAMPLES - 500) * f->time / 99U);
            FX.feedback = (int32_t)(f->character * 26000U / 99U);
            FX.rv_feedback = 21000 + (int32_t)(f->time * 9000U / 99U) + (int32_t)(f->character * 1800U / 99U);
            if (FX.rv_feedback > 31800) FX.rv_feedback = 31800;
            FX.wet = (int32_t)(f->balance * 32767U / 99U);
            FX.drive = 1 + (int32_t)(f->character * 15U / 99U);
        } else {
            FX.p.algo = FX_ALGO_OFF;              /* control task clears buffers, then sets */
            ring_put(&ring_back, 0xFFFFFFF0U);
        }
        staged[ST_FX] = 0;
    }
    if (staged[ST_CFG]) {
        for (unsigned p = 0; p < FM4_PARTS; ++p) fm4_copy(&M.part[p].cfg, &staged_cfg[p], sizeof staged_cfg[p]);
        staged[ST_CFG] = 0;
    }
    if (staged[ST_PANIC]) { multi_all_off(&M, 0); staged[ST_PANIC] = 0; }
    uint32_t e;
    for (unsigned n = 0; n < 64 && ring_get(&ring_midi, &e); ++n) handle_event(e, 0);
    for (unsigned n = 0; n < 64 && ring_get(&ring_local, &e); ++n) handle_event(e, 1);
    if (clock_cmd) {
        uint8_t c = clock_cmd; clock_cmd = 0;
        if (seq.ext_clock) {
            if (c == 0xFA) { seq.tick = 0; seq.cursor = 0; seq.state = SEQ_PLAYING; }
            else if (c == 0xFB && seq.state == SEQ_STOPPED) seq.state = SEQ_PLAYING;
            else if (c == 0xFC) ring_put(&ring_back, 0xFFFFFFF1U);   /* stop via control */
        }
    }
    while (clock_pulses) { --clock_pulses; seq_midi_clock(&seq); }
    seq_block(&seq, FM4_BLOCK);

    static int32_t dry[FM4_BLOCK * 2], send[FM4_BLOCK * 2];
    fm4_zero32(dry, FM4_BLOCK * 2); fm4_zero32(send, FM4_BLOCK * 2);   /* not memset: libc is in Flash */
    multi_render(&M, dry, send);
    fx_render(&FX, send, dry);
    if (click_env > 0) {
        for (unsigned s = 0; s < FM4_BLOCK && click_env > 0; ++s) {
            click_phase += click_step;
            int32_t sq = click_phase < 0 ? -1 : 1;
            int32_t v = sq * (click_env >> 4);
            dry[2 * s] += v; dry[2 * s + 1] += v;
            click_env -= click_env >> 9; if (click_env < 4096) click_env = 0;
        }
    }
    fm4_copy32(out, dry, FM4_BLOCK * 2);                                 /* not memcpy */
}

/* ---- Control poll ------------------------------------------------------------ */
static void queue_out(const uint8_t *m, size_t n)
{
    if (out_len + n > OUT_BYTES) return;
    memcpy(out_buf + out_len, m, n); out_len += (unsigned)n;
}

static void flush_out(void)
{
    /* One complete message per SDK call (<=128 bytes). A longer SysEx (VMEM
     * bulk) is streamed in 128-byte slices; out_slice tracks progress. */
    while (out_pos < out_len) {
        unsigned start = out_pos, end = start;
        if (out_buf[start] == 0xF0) { while (end < out_len && out_buf[end] != 0xF7) ++end; ++end; }
        else end = start + ((out_buf[start] & 0xE0) == 0xC0 ? 2 : (out_buf[start] >= 0xF8 ? 1 : 3));
        if (end > out_len) { out_len = out_pos = out_slice = 0; return; }
        unsigned len = end - start;
        if (len <= 128) {
            if (fm4_hw_midi_send(out_buf + start, len) < 0) return;
            out_pos = end;
            continue;
        }
        unsigned chunk = len - out_slice > 128 ? 128 : len - out_slice;
        if (fm4_hw_midi_send(out_buf + start + out_slice, chunk) < 0) return;
        out_slice += chunk;
        if (out_slice >= len) { out_slice = 0; out_pos = end; }
    }
    out_len = out_pos = out_slice = 0;
}

static unsigned sysex_part(unsigned device)
{
    if (!sys.mode) return 0;
    for (unsigned p = 0; p < FM4_PARTS; ++p)
        if (setup.cfg[p].enabled && setup.cfg[p].channel == device) return p;
    return 0xff;
}

static void handle_ext(const uint8_t *m, size_t n)
{
    uint8_t reply[200];
    unsigned cmd = m[4];
    if (cmd == FM4X_STORE && n == 8) {
        int r = fm4_store(m[5] & 7, m[6]);
        uint8_t ack[8] = { 0xF0, FM4X_ID0, FM4X_ID1, FM4X_ID2, FM4X_STORE | FM4X_REPLY, (uint8_t)(r ? 1 : 0), m[6], 0xF7 };
        queue_out(ack, sizeof ack);
    } else if (cmd == FM4X_READ && n == 7) {
        tq5_voice v; fm4_voice_get(FM4_BANK_USER, m[5], &v);
        size_t len = tq5_reply(&v, TQ5_REQ_EFEDS, sys.tx_channel, reply, sizeof reply);
        queue_out(reply, len);
    } else if (cmd == FM4X_SELECT && n == 9) {
        fm4_select(m[5] & 7, m[6], m[7]);
    } else if (cmd == FM4X_INFO && n == 6) {
        static const char info[] = "FM-4 " FM4_VERSION " TQ5";
        size_t k = 0; reply[k++] = 0xF0; reply[k++] = FM4X_ID0; reply[k++] = FM4X_ID1; reply[k++] = FM4X_ID2;
        reply[k++] = FM4X_INFO | FM4X_REPLY;
        for (const char *c = info; *c; ++c) reply[k++] = (uint8_t)*c;
        reply[k++] = 0xF7;
        queue_out(reply, k);
    } else if (cmd == FM4X_PREPARE_STOCK && n == 11 && !memcmp(m + 5, "STOCK", 5)) {
        unsigned erased, remaining;
        int st = fm4_prepare_stock(&erased, &remaining);
        uint8_t ack[9] = { 0xF0, FM4X_ID0, FM4X_ID1, FM4X_ID2, FM4X_PREPARE_STOCK | FM4X_REPLY, (uint8_t)st,
                           (uint8_t)(erased > 127 ? 127 : erased), (uint8_t)(remaining > 127 ? 127 : remaining), 0xF7 };
        queue_out(ack, sizeof ack);
    }
}

static void handle_sysex(const uint8_t *m, size_t n)
{
    if (n >= 6 && m[1] == FM4X_ID0 && m[2] == FM4X_ID1 && m[3] == FM4X_ID2) { handle_ext(m, n); return; }
    if (!sys.sysex_rx || n < 5 || m[1] != 0x43) return;
    unsigned device = m[2] & 15;
    unsigned part = sysex_part(device);
    if (part == 0xff) return;
    unsigned rx = sys.mode ? device : sys.rx_channel;
    tq5_result r = tq5_parse(m, n, rx, &edit_buf[part]);
    static uint8_t vmem_block;
    switch (r.action) {
    case TQ5_VOICE_EDIT: stage_voice(part); break;
    case TQ5_DUMP_REQUEST:
        if (r.request == TQ5_REQ_VMEM) {
            bank_dump_block = 1;              /* streamed block by block in poll */
        } else {
            uint8_t reply[200];
            size_t len = tq5_reply(&edit_buf[part], r.request, sys.tx_channel, reply, sizeof reply);
            queue_out(reply, len);
        }
        break;
    case TQ5_BLOCK_SELECT: vmem_block = r.block; break;
    case TQ5_VMEM_BLOCK:
        if (sys.protect) { notice_text = "MEMORY PROTECTED"; break; }
        for (unsigned i = 0; i < 25; ++i) memcpy(user_bank[25 * vmem_block + i], r.vmem + i * TQ5_VMEM, TQ5_VMEM);
        save_record(OFF_VOICES, MAGIC_VOICES, user_bank, sizeof user_bank);
        notice_text = "BANK RECEIVED";
        vmem_block = 0;
        break;
    case TQ5_REMOTE:
        if (r.value && (r.pp == 116 || r.pp == 117))
            fm4_select(part, r.pp == 117 ? FM4_BANK_USER : FM4_BANK_PRESET, setup.program[part]);
        break;
    case TQ5_MULTI_PARAM: {
        multi_part_cfg *c = &setup.cfg[r.part & 7];
        unsigned v = r.value;
        switch (r.pp) {
        case 0: c->reserve = (uint8_t)(v > 16 ? 16 : v); c->enabled = v != 0; break;
        case 1: break;
        case 2: fm4_select(r.part, setup.bank[r.part & 7], v); break;
        case 3: c->channel = (uint8_t)(v > 16 ? 16 : v); break;
        case 4: c->note_lo = (uint8_t)(v & 127); break;
        case 5: c->note_hi = (uint8_t)(v & 127); break;
        case 6: c->detune = (int8_t)((v > 14 ? 14 : v) - 7); break;
        case 7: c->note_shift = (int8_t)((v > 48 ? 48 : v) - 24); break;
        case 8: c->volume = (uint8_t)(v > 99 ? 99 : v); break;
        case 9: c->pan = (uint8_t)(v == 1 ? 0 : v == 2 ? 127 : 64); break;
        default: break;
        }
        fm4_apply_multi(); settings_dirty = 1;
        break;
    }
    case TQ5_MASTER_TUNE: sys.master_tune = r.value; M.master_tune = (int8_t)(r.value - 64); settings_dirty = 1; break;
    default: break;
    }
}

void fm4_core_poll(void)
{
    /* deferred SysEx */
    for (unsigned k = 0; k < SX_SLOTS; ++k) {
        unsigned slot = sx_tail % SX_SLOTS;
        unsigned len = sx_in[slot].len;
        if (!len) break;
        handle_sysex(sx_in[slot].data, len);
        sx_in[slot].len = 0; ++sx_tail;
    }
    /* audio -> control events */
    uint32_t e;
    while (ring_get(&ring_back, &e)) {
        if (e == 0xFFFFFFF0U) { fx_params f; tq5_effect(&edit_buf[0], &f); fm4_hw_audio_gate(1); fx_set(&FX, &f); fm4_hw_audio_gate(0); continue; }
        if (e == 0xFFFFFFF1U) { fm4_transport(0); continue; }
        uint8_t st = (uint8_t)e, d1 = (uint8_t)(e >> 8), d2 = (uint8_t)(e >> 16);
        if (e & (1U << 31)) {               /* sequencer output: echo to MIDI out */
            uint8_t m[3] = { st, d1, d2 }; queue_out(m, (st & 0xE0) == 0xC0 ? 2 : 3);
            continue;
        }
        unsigned ch = st & 15, kind = st & 0xF0;
        if (kind == 0xC0) {
            if (d1 >= TQ5_BANK_VOICES) continue;
            unsigned parts = multi_parts_for_channel(&M, ch);
            for (unsigned p = 0; p < FM4_PARTS; ++p) if (parts & (1U << p)) fm4_select(p, setup.bank[p], d1);
        } else if (kind == 0xB0 && d1 == 0 && d2 <= 1) {
            unsigned parts = multi_parts_for_channel(&M, ch);
            for (unsigned p = 0; p < FM4_PARTS; ++p) if (parts & (1U << p)) setup.bank[p] = d2;
        } else if (kind == 0xB0 && (d1 == 126 || d1 == 127)) {
            unsigned parts = multi_parts_for_channel(&M, ch);
            for (unsigned p = 0; p < FM4_PARTS; ++p) if (parts & (1U << p)) {
                edit_buf[p].vced[TQ5_POLY] = d1 == 126; stage_voice(p);
            }
        }
    }
    /* voices that changed while a hand-over was pending */
    if (restage_mask) {
        uint32_t r = restage_mask; restage_mask = 0;
        for (unsigned p = 0; p < FM4_PARTS; ++p) if (r & (1U << p)) stage_voice(p);
    }
    flush_out();
    if (bank_dump_block && out_len == 0) {
        static uint8_t recs[32][TQ5_VMEM];
        unsigned b = bank_dump_block - 1;
        uint8_t sel[7]; tq5_msg_param(sys.tx_channel, 0x24, 7, b + 1, sel); queue_out(sel, 7);
        for (unsigned i = 0; i < 32; ++i) memcpy(recs[i], user_bank[25 * b + (i < 25 ? i : 0)], TQ5_VMEM);
        out_len += (unsigned)tq5_msg_vmem((const uint8_t (*)[TQ5_VMEM])recs, sys.tx_channel, out_buf + out_len);
        bank_dump_block = b + 1 < 4 ? (uint8_t)(b + 2) : 0;
        flush_out();
    }
}

void fm4_mark_dirty(void) { settings_dirty = 1; }
void fm4_midi_out(const uint8_t *m, size_t n) { queue_out(m, n); flush_out(); }
void fm4_core_tick(uint32_t now)
{
    /* A Flash erase/write silences audio and holds off interrupts for tens
     * of ms (audible dropout; the panel LED scan freezes, which shows as a
     * brighter flash of the lit LED). Only save once the instrument has been
     * silent for a while: no voice sounding, no song running. */
    static uint32_t dirty_since, quiet_since; static uint8_t was_dirty;
    if (multi_active_voices(&M) || seq.state != SEQ_STOPPED) quiet_since = now;
    if (!settings_dirty) { was_dirty = 0; return; }
    if (!was_dirty) { was_dirty = 1; dirty_since = now; return; }
    if (now - dirty_since >= 2000U && now - quiet_since >= 3000U) { fm4_save_settings(); was_dirty = 0; }
}

unsigned fm4_voices_active(void) { return multi_active_voices(&M); }
unsigned fm4_part_activity(unsigned part)
{
    unsigned n = 0;
    for (unsigned s = 0; s < M.eng.voices; ++s) if (M.slot_part[s] == part && M.eng.v[s].active) ++n;
    return n;
}
