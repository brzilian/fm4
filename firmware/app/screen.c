/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 front panel: display, buttons, encoders, keys and LEDs.
 * Font and colours from M-VAVE fm1_open screen.c (GPL-3.0 w/ linking exc.).
 *
 * Panel map (see docs/PANEL.md):
 *   PRESETS (enc 6)   voice number of the active part, every page
 *   ALGORITHM (enc 1) algorithm of the active voice (song track on SEQ)
 *   SELECT (enc 0)    operator / part / song / cursor, per page
 *   KNOB 1-4 (2..5)   the four parameters shown at the bottom
 *   HOME  play screen with TQ5-style Easy Edit (tone, attack, release, FX)
 *   EDIT  operator: frequency / wave / scaling   ENV  operator envelope
 *   LFO   LFO        SEL  voice common, controllers   FX  effect
 *   GLO   system / MIDI      SAVE  name + store      ARP  MULTI (8 parts)
 *   SEQ   8-track song       PLAY  start/stop        REC record
 *   OCT-/OCT+ octave (both: reset)   HOME+PLAY panic   SEQ+white key: mute
 *   Pressing the lit page button again steps through its sub-pages. */
#include "instrument.h"
#include "version.h"
#include "fm1_engine_control.h"
#include "fm4/fm4_core.h"
#include "fm_patch.h"
#include "fm1_device.h"
#include "drum808.h"
#include "sampledrum.h"
#include <string.h>

enum { BACK = 0x0000, PANEL = 0x18e3, TEXT = 0xef5d, ACCENT = 0x07ff, DIM = 0x738e,
       GREEN = 0x07e0, AMBER = 0xfd20, RED = 0xf8a2, LCDBG = 0x0864 };
static const uint16_t knob_colors[4] = { 0x3c9f, 0x07bf, 0x4eae, 0xfd20 };

static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
static const uint8_t punctuation[][6] = {
    {45,8,8,8,8,8}, {47,0x60,0x18,6,1,0}, {43,8,8,0x3e,8,8},
    {35,0x14,0x7f,0x14,0x7f,0x14}, {37,0x63,0x13,8,0x64,0x63},
    {62,0x41,0x22,0x14,8,0}, {63,2,1,0x51,9,6},
    {33,0,0,0x5f,0,0}, {34,0,7,0,7,0}, {36,0x24,0x2a,0x7f,0x2a,0x12},
    {38,0x36,0x49,0x55,0x22,0x50}, {39,0,5,3,0,0}, {40,0,0x1c,0x22,0x41,0},
    {41,0,0x41,0x22,0x1c,0}, {42,0x14,8,0x3e,8,0x14}, {44,0,0x50,0x30,0,0},
    {46,0,0x60,0x60,0,0}, {58,0,0x36,0x36,0,0}, {59,0,0x56,0x36,0,0},
    {60,8,0x14,0x22,0x41,0}, {61,0x14,0x14,0x14,0x14,0x14},
    {64,0x32,0x49,0x79,0x41,0x3e}, {91,0,0x7f,0x41,0x41,0},
    {93,0,0x41,0x41,0x7f,0}, {94,4,2,1,2,4}, {95,0x40,0x40,0x40,0x40,0x40},
    {124,0,0,0x7f,0,0}, {126,8,4,8,0x10,8}
};

/* ---- Display list and band renderer ----------------------------------- */
enum { OP_FILL, OP_TEXT, OP_LINE, OP_FRAME };
typedef struct {
    uint8_t op, scale; uint16_t fg, bg;
    uint8_t x, y, w, h;          /* LINE: x,y -> w,h are the end point */
    char s[22];
} item;
#define MAX_ITEMS 160
#define BAND 16
#define BANDS (FM1_SCREEN_HEIGHT / BAND)
static item items[MAX_ITEMS];
static unsigned nitems;
static uint32_t band_hash[BANDS], shown_hash[BANDS];
static uint16_t band_px[FM1_SCREEN_WIDTH * BAND];
static unsigned flush_band, band_busy;

static void dl_reset(void) { nitems = 0; }
static item *dl_add(unsigned op, unsigned x, unsigned y, unsigned w, unsigned h, uint16_t fg, uint16_t bg)
{
    if (nitems >= MAX_ITEMS) return 0;
    item *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    it->op = (uint8_t)op; it->x = (uint8_t)(x > 239 ? 239 : x); it->y = (uint8_t)(y > 239 ? 239 : y);
    it->w = (uint8_t)w; it->h = (uint8_t)h; it->fg = fg; it->bg = bg;
    return it;
}
static void fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c) { dl_add(OP_FILL, x, y, w, h, c, 0); }
static void frame(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c) { dl_add(OP_FRAME, x, y, w, h, c, 0); }
static void line(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint16_t c) { dl_add(OP_LINE, x0, y0, x1, y1, c, 0); }
static unsigned text(unsigned x, unsigned y, const char *s, unsigned scale, uint16_t fg)
{
    size_t n = strlen(s); if (n > 21) n = 21;
    item *it = dl_add(OP_TEXT, x, y, (unsigned)(6 * scale * n), 7 * scale, fg, 0);
    if (it) { it->scale = (uint8_t)scale; memcpy(it->s, s, n); it->s[n] = 0; }
    return x + 6 * scale * (unsigned)n;
}
static unsigned text_width(const char *s, unsigned scale) { return 6 * scale * (unsigned)strlen(s); }

static uint32_t hash_item(uint32_t h, const item *it)
{
    const uint8_t *p = (const uint8_t *)it;
    for (unsigned i = 0; i < sizeof *it; ++i) h = (h ^ p[i]) * 16777619U;
    return h;
}
static int item_in_band(const item *it, unsigned top)
{
    unsigned y0 = it->y, y1 = it->op == OP_LINE ? it->h : it->y + it->h;
    if (it->op == OP_LINE && y1 < y0) { unsigned t = y0; y0 = y1; y1 = t; }
    if (it->op == OP_LINE) ++y1;
    return y0 < top + BAND && y1 > top;
}
static void band_hashes(void)
{
    for (unsigned b = 0; b < BANDS; ++b) {
        uint32_t h = 2166136261U;
        for (unsigned i = 0; i < nitems; ++i) if (item_in_band(&items[i], b * BAND)) h = hash_item(h, &items[i]);
        band_hash[b] = h;
    }
}
static void px(unsigned x, unsigned y, unsigned top, uint16_t c)
{
    if (x < FM1_SCREEN_WIDTH && y >= top && y < top + BAND) band_px[(y - top) * FM1_SCREEN_WIDTH + x] = c;
}
static const uint8_t *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch >= '0' && ch <= '9') return font[ch - '0'];
    if (ch >= 'A' && ch <= 'Z') return font[10 + ch - 'A'];
    for (unsigned i = 0; i < sizeof punctuation / sizeof punctuation[0]; ++i)
        if ((uint8_t)ch == punctuation[i][0]) return punctuation[i] + 1;
    return 0;
}
static void render_band(unsigned top)
{
    for (unsigned i = 0; i < FM1_SCREEN_WIDTH * BAND; ++i) band_px[i] = BACK;
    for (unsigned n = 0; n < nitems; ++n) {
        const item *it = &items[n];
        if (!item_in_band(it, top)) continue;
        switch (it->op) {
        case OP_FILL:
            for (unsigned y = it->y; y < (unsigned)it->y + it->h; ++y)
                for (unsigned x = it->x; x < (unsigned)it->x + it->w; ++x) px(x, y, top, it->fg);
            break;
        case OP_FRAME:
            for (unsigned x = it->x; x < (unsigned)it->x + it->w; ++x) { px(x, it->y, top, it->fg); px(x, it->y + it->h - 1, top, it->fg); }
            for (unsigned y = it->y; y < (unsigned)it->y + it->h; ++y) { px(it->x, y, top, it->fg); px(it->x + it->w - 1, y, top, it->fg); }
            break;
        case OP_LINE: {
            int x0 = it->x, y0 = it->y, x1 = it->w, y1 = it->h;
            int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
            for (;;) {
                px((unsigned)x0, (unsigned)y0, top, it->fg);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
            break;
        }
        case OP_TEXT: {
            unsigned x = it->x, sc = it->scale;
            for (const char *c = it->s; *c; ++c, x += 6 * sc) {
                const uint8_t *g = glyph(*c);
                if (!g) continue;
                for (unsigned col = 0; col < 5; ++col)
                    for (unsigned row = 0; row < 7; ++row)
                        if (g[col] & (1U << row))
                            for (unsigned yy = 0; yy < sc; ++yy)
                                for (unsigned xx = 0; xx < sc; ++xx)
                                    px(x + col * sc + xx, it->y + row * sc + yy, top, it->fg);
            }
            break;
        }
        }
    }
}
/* Send changed bands over DMA, bounded work per control poll. */
static void flush(void)
{
    for (unsigned work = 0; work < BANDS; ++work) {
        if (band_busy) {
            int r = fm1_screen_pending();
            if (r > 0) return;
            band_busy = 0;
        }
        unsigned b = flush_band;
        flush_band = (flush_band + 1) % BANDS;
        if (band_hash[b] == shown_hash[b]) continue;
        render_band(b * BAND);
        if (fm1_screen_submit(0, b * BAND, FM1_SCREEN_WIDTH, BAND, band_px, FM1_SCREEN_WIDTH * BAND) == 0) {
            shown_hash[b] = band_hash[b];
            band_busy = 1;
        }
        return;   /* one band per poll keeps the control loop responsive */
    }
}

/* ---- Small formatting helpers ------------------------------------------ */
static void fmt_uint(char *o, unsigned v, unsigned width)
{
    char t[12]; unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n < width) t[n++] = '0';
    while (n) *o++ = t[--n];
    *o = 0;
}
static void fmt_int(char *o, int v) { if (v > 0) *o++ = '+'; else if (v < 0) { *o++ = '-'; v = -v; } fmt_uint(o, (unsigned)v, 1); }
static void fmt_note(char *o, unsigned n)
{
    static const char *names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int oct = (int)(n / 12) - 2;           /* Yamaha: middle C (60) = C3 */
    strcpy(o, names[n % 12]);
    o += strlen(o);
    if (oct < 0) { *o++ = '-'; oct = -oct; }
    fmt_uint(o, (unsigned)oct, 1);
}

/* ---- Pages and parameters ----------------------------------------------- */
enum { PG_HOME, PG_EDIT, PG_ENV, PG_LFO, PG_VOICE, PG_FX, PG_GLO, PG_SAVE, PG_MULTI, PG_SONG, PG_COUNT };
enum { OCT_DOWN, OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT, BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC };
#define BUTTON(b) (1U << (b))
static const uint8_t page_button[PG_COUNT] = { BTN_HOME, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_SEL, BTN_FX, BTN_GLO, BTN_SAVE, BTN_ARP, BTN_SEQ };
static const uint8_t page_subs[PG_COUNT] = { 1, 3, 2, 2, 3, 1, 3, 1, 3, 2 };
static const char *const page_title[PG_COUNT] = { "PLAY", "OPERATOR", "ENVELOPE", "LFO", "VOICE", "EFFECT", "SYSTEM", "STORE", "MULTI", "SONG" };

/* parameter kinds */
enum { K_NONE, K_OPV, K_OPA, K_V, K_A, K_A2, K_EF, K_SYS, K_PART, K_SONG, K_EASY, K_OPON, K_ACT };
typedef struct { uint8_t kind, off, fmt; const char *label; } param;
enum { F_NUM, F_RATIO, F_DET, F_WAVE, F_ONOFF, F_FIXRG, F_SHFT, F_LFW, F_POLY, F_TRPS, F_CH, F_FX, F_MODE,
       F_TUNE, F_PAN, F_SIGNED7, F_NOTE, F_SHIFT24, F_TEMPO, F_QUANT, F_PMODE, F_CLOCK, F_SIGNED, F_ACT };
static const param params[PG_COUNT][3][4] = {
    [PG_HOME] = {{{K_EASY,0,F_SIGNED,"TONE"}, {K_EASY,1,F_SIGNED,"ATTACK"}, {K_EASY,2,F_SIGNED,"RELEASE"}, {K_EF,2,F_NUM,"FX BAL"}}},
    [PG_EDIT] = {{{K_OPV,TQ5_OP_CRS,F_RATIO,"FREQ"}, {K_OPA,TQ5_OP_FINE,F_NUM,"FINE"}, {K_OPV,TQ5_OP_DET,F_DET,"DETUNE"}, {K_OPV,TQ5_OP_OUT,F_NUM,"LEVEL"}},
                 {{K_OPA,TQ5_OP_OSW,F_WAVE,"WAVE"}, {K_OPA,TQ5_OP_FIX,F_ONOFF,"FIXED"}, {K_OPA,TQ5_OP_FIXRG,F_FIXRG,"RANGE"}, {K_OPON,0,F_ONOFF,"OP ON"}},
                 {{K_OPV,TQ5_OP_KVS,F_NUM,"VELO"}, {K_OPV,TQ5_OP_LS,F_NUM,"LVL SCL"}, {K_OPV,TQ5_OP_RS,F_NUM,"RATE SCL"}, {K_OPV,TQ5_OP_AME,F_ONOFF,"AM"}}},
    [PG_ENV] = {{{K_OPV,TQ5_OP_AR,F_NUM,"ATTACK"}, {K_OPV,TQ5_OP_D1R,F_NUM,"DECAY1"}, {K_OPV,TQ5_OP_D1L,F_NUM,"D1 LEVEL"}, {K_OPV,TQ5_OP_D2R,F_NUM,"DECAY2"}},
                {{K_OPV,TQ5_OP_RR,F_NUM,"RELEASE"}, {K_OPA,TQ5_OP_SHFT,F_SHFT,"EG SHIFT"}, {K_A,20,F_NUM,"REVERB"}, {K_OPV,TQ5_OP_EBS,F_NUM,"EG BIAS"}}},
    [PG_LFO] = {{{K_V,TQ5_LFS,F_NUM,"SPEED"}, {K_V,TQ5_LFD,F_NUM,"DELAY"}, {K_V,TQ5_PMD,F_NUM,"PITCH MD"}, {K_V,TQ5_AMD,F_NUM,"AMP MD"}},
                {{K_V,TQ5_LFW,F_LFW,"WAVE"}, {K_V,TQ5_SYNC,F_ONOFF,"KEY SYNC"}, {K_V,TQ5_PMS,F_NUM,"PITCH SN"}, {K_V,TQ5_AMS,F_NUM,"AMP SENS"}}},
    [PG_VOICE] = {{{K_V,TQ5_FBL,F_NUM,"FEEDBACK"}, {K_V,TQ5_TRPS,F_TRPS,"TRANSPOS"}, {K_V,TQ5_PBR,F_NUM,"BEND RNG"}, {K_V,TQ5_POLY,F_POLY,"POLY"}},
                  {{K_V,TQ5_PORTTIME,F_NUM,"PORTA"}, {K_V,TQ5_PORTMODE,F_PMODE,"PORT MOD"}, {K_V,TQ5_MWPITCH,F_NUM,"MW PITCH"}, {K_V,TQ5_MWAMP,F_NUM,"MW AMP"}},
                  {{K_V,TQ5_BCPITCH,F_NUM,"BC PITCH"}, {K_V,TQ5_BCAMP,F_NUM,"BC AMP"}, {K_A2,0,F_NUM,"AT PITCH"}, {K_A2,1,F_NUM,"AT AMP"}}},
    [PG_FX] = {{{K_EF,0,F_FX,"EFFECT"}, {K_EF,1,F_NUM,"TIME"}, {K_EF,2,F_NUM,"BALANCE"}, {K_PART,7,F_NUM,"SEND"}}},
    [PG_GLO] = {{{K_SYS,1,F_MODE,"MODE"}, {K_SYS,2,F_CH,"RX CH"}, {K_SYS,6,F_TUNE,"TUNE"}, {K_SYS,7,F_NUM,"VOICES"}},
                {{K_SYS,4,F_ONOFF,"SYSEX RX"}, {K_SYS,5,F_ONOFF,"PROTECT"}, {K_SYS,10,F_NUM,"KEY VEL"}, {K_SYS,11,F_CLOCK,"CLOCK"}},
                {{K_SYS,8,F_ONOFF,"LOCAL"}, {K_SYS,9,F_ONOFF,"KEY OUT"}, {K_SYS,3,F_CH,"TX CH"}, {K_NONE,0,0,""}}},
    [PG_SAVE] = {{{K_ACT,0,F_ACT,"CHAR"}, {K_NONE,0,0,""}, {K_NONE,0,0,""}, {K_ACT,1,F_NUM,"TO USER"}}},
    [PG_MULTI] = {{{K_PART,0,F_CH,"CHANNEL"}, {K_PART,1,F_NUM,"VOLUME"}, {K_PART,2,F_NUM,"VOICES"}, {K_PART,3,F_PAN,"PAN"}},
                  {{K_PART,4,F_NOTE,"LOW KEY"}, {K_PART,5,F_NOTE,"HIGH KEY"}, {K_PART,6,F_SIGNED7,"DETUNE"}, {K_PART,8,F_SHIFT24,"SHIFT"}},
                  {{K_PART,7,F_NUM,"FX SEND"}, {K_PART,9,F_ONOFF,"ENABLED"}, {K_NONE,0,0,""}, {K_NONE,0,0,""}}},
    [PG_SONG] = {{{K_SONG,0,F_TEMPO,"TEMPO"}, {K_SONG,1,F_NUM,"MEASURES"}, {K_SONG,2,F_QUANT,"QUANTIZE"}, {K_SONG,3,F_ONOFF,"LOOP"}},
                 {{K_SONG,4,F_ONOFF,"CLICK"}, {K_SONG,5,F_NUM,"TRK PART"}, {K_ACT,2,F_ACT,"CLEAR TRK"}, {K_ACT,3,F_ACT,"SAVE SONG"}}},
};

/* ---- UI state ------------------------------------------------------------ */
static unsigned page, sub, op = 1, song_track, save_slot, save_cursor;
static int octave, easy[3];
static uint32_t prev_buttons, prev_keys, frame_ms, notice_until, button_down_ms[14], led_phase;
static const char *notice;
static char notice_buf[24];
static uint8_t key_note[27];
static int chord_used;
static volatile unsigned keepalive_never;
/* CPU meter (board/audio.c); the host harness provides stubs. */
uint32_t fm4_audio_block_ticks(void);
void fm4_audio_stats_reset_peak(void);
static unsigned cpu_peak_pct, cpu_overruns;
static uint32_t cpu_seen_overruns, cpu_at;     /* see keep_sdk_alive() */

static fm4_system *S(void) { return fm4_sys(); }
static unsigned part(void) { return fm4_active_part(); }
static tq5_voice *V(void) { return fm4_edit(part()); }
static uint8_t *opv(void) { return V()->vced + 13 * tq5_op_block(op); }
static uint8_t *opa(void) { return V()->aced + 5 * tq5_op_block(op); }

static void show(const char *msg) { notice = msg; notice_until = fm1_millis() + 1500U; }

/* Value access for a parameter: returns pointer + max, or NULL. */
static uint8_t *param_ref(const param *p, unsigned *max, int *lo)
{
    *lo = 0;
    fm4_multi_setup *m = fm4_multi();
    multi_part_cfg *c = &m->cfg[part()];
    switch (p->kind) {
    case K_OPV: *max = tq5_vced_max[13 * tq5_op_block(op) + p->off]; return opv() + p->off;
    case K_OPA: *max = tq5_aced_max[5 * tq5_op_block(op) + p->off];
        if (p->off == TQ5_OP_FINE && opv()[TQ5_OP_CRS] < 4) *max = 7;
        return opa() + p->off;
    case K_V: *max = tq5_vced_max[p->off]; return V()->vced + p->off;
    case K_A: *max = tq5_aced_max[p->off]; return V()->aced + p->off;
    case K_A2: *max = 99; return V()->aced2 + p->off;
    case K_EF: *max = p->off == 0 ? 10 : p->off == 2 ? 99 : ((V()->efeds[0] == 4 || V()->efeds[0] == 5 || V()->efeds[0] == 6 || V()->efeds[0] == 8) ? 75 : 40);
        return V()->efeds + p->off;
    case K_SYS: {
        static const uint8_t maxes[16] = { 0, 1, 16, 15, 1, 1, 127, 16, 1, 1, 127, 1 };
        *max = maxes[p->off];
        if (p->off == 7) *lo = 4;
        if (p->off == 10) *lo = 1;
        return (uint8_t *)S() + p->off;
    }
    case K_PART:
        switch (p->off) {
        case 0: *max = 16; return &c->channel;
        case 1: *max = 99; return &c->volume;
        case 2: *max = 16; return &c->reserve;
        case 3: *max = 127; return &c->pan;
        case 4: *max = 127; return &c->note_lo;
        case 5: *max = 127; return &c->note_hi;
        case 6: *max = 7; *lo = -7; return (uint8_t *)&c->detune;
        case 7: *max = 99; return &c->fx_send;
        case 8: *max = 24; *lo = -24; return (uint8_t *)&c->note_shift;
        case 9: *max = 1; return &c->enabled;
        }
        return 0;
    case K_SONG: {
        seq_song *g = fm4_seq()->song;
        switch (p->off) {
        case 1: *max = 99; *lo = 1; return &g->measures;
        case 2: *max = 6; return &g->quantize;
        case 3: *max = 1; return &g->loop;
        case 4: *max = 1; return &fm4_seq()->metronome;
        case 5: *max = 7; return &g->track_part[song_track & 7];
        }
        return 0;
    }
    default: return 0;
    }
}

static void param_text(const param *p, char *o)
{
    unsigned max; int lo; o[0] = 0;
    if (p->kind == K_NONE) return;
    if (p->kind == K_EASY) { fmt_int(o, easy[p->off]); return; }
    if (p->kind == K_OPON) { strcpy(o, (V()->opmask >> (op - 1)) & 1 ? "ON" : "OFF"); return; }
    if (p->kind == K_SONG && p->off == 0) { unsigned t = fm4_seq()->song->tempo; fmt_uint(o, t / 10, 1); strcat(o, "."); fmt_uint(o + strlen(o), t % 10, 1); return; }
    if (p->kind == K_ACT) {
        if (p->off == 0) { char ch[2] = { (char)V()->vced[TQ5_NAME + save_cursor], 0 }; strcpy(o, "'"); strcat(o, ch); strcat(o, "'"); }
        else if (p->off == 1) { strcpy(o, "U"); fmt_uint(o + 1, save_slot, 2); }
        else strcpy(o, "TURN");
        return;
    }
    uint8_t *ref = param_ref(p, &max, &lo);
    if (!ref) return;
    int v = lo < 0 ? (int)(int8_t)*ref : (int)*ref;
    switch (p->fmt) {
    case F_RATIO:
        if (opa()[TQ5_OP_FIX]) { fmt_uint(o, opz_fixed_hz(*ref, opa()[TQ5_OP_FIXRG], opa()[TQ5_OP_FINE]), 1); strcat(o, "HZ"); }
        else { unsigned r = opz_ratio_x100(*ref, opa()[TQ5_OP_FINE]); fmt_uint(o, r / 100, 1); strcat(o, "."); fmt_uint(o + strlen(o), r % 100, 2); }
        break;
    case F_DET: fmt_int(o, v - 3); break;
    case F_WAVE: strcpy(o, "W"); fmt_uint(o + 1, (unsigned)v + 1, 1); break;
    case F_ONOFF: strcpy(o, v ? "ON" : "OFF"); break;
    case F_FIXRG: fmt_uint(o, 255U << v, 1); break;
    case F_SHFT: { static const char *s[4] = { "OFF", "48DB", "24DB", "12DB" }; strcpy(o, s[v & 3]); break; }
    case F_LFW: { static const char *s[4] = { "SAW", "SQUARE", "TRI", "S/H" }; strcpy(o, s[v & 3]); break; }
    case F_POLY: strcpy(o, v ? "MONO" : "POLY"); break;
    case F_PMODE: strcpy(o, v ? "FINGER" : "FULL"); break;
    case F_TRPS: fmt_int(o, v - 24); break;
    case F_CH: if (v >= 16) strcpy(o, "OMNI"); else fmt_uint(o, (unsigned)v + 1, 1); break;
    case F_FX: { static const char *s[11] = { "OFF", "HALL", "ROOM", "PLATE", "DELAY", "DLY L/R", "ST ECHO", "DIST+REV", "DIST+ECH", "GATE REV", "REV GATE" }; strcpy(o, s[v > 10 ? 10 : v]); break; }
    case F_MODE: strcpy(o, v ? "MULTI" : "SINGLE"); break;
    case F_TUNE: fmt_int(o, v - 64); break;
    case F_PAN: if (v == 64) strcpy(o, "C"); else { strcpy(o, v < 64 ? "L" : "R"); fmt_uint(o + 1, (unsigned)(v < 64 ? 64 - v : v - 64), 1); } break;
    case F_SIGNED7: case F_SHIFT24: case F_SIGNED: fmt_int(o, v); break;
    case F_NOTE: fmt_note(o, (unsigned)v); break;
    case F_QUANT: { static const char *s[7] = { "OFF", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" }; strcpy(o, s[v > 6 ? 6 : v]); break; }
    case F_CLOCK: strcpy(o, v ? "MIDI" : "INT"); break;
    default: fmt_uint(o, (unsigned)v, 1); break;
    }
}

/* TQ5 Easy Edit: relative macro edits over several operators. */
static int is_carrier(unsigned alg, unsigned n)
{
    static const uint8_t carriers[8] = { 0x1, 0x1, 0x1, 0x1, 0x5, 0x7, 0x7, 0xf }; /* bit0 = OP1 */
    return (carriers[alg & 7] >> (n - 1)) & 1;
}
static void easy_edit(unsigned which, int d)
{
    tq5_voice *v = V(); unsigned alg = v->vced[TQ5_ALG];
    for (unsigned n = 1; n <= 4; ++n) {
        uint8_t *o = v->vced + 13 * tq5_op_block(n);
        if (which == 0 && !is_carrier(alg, n)) o[TQ5_OP_OUT] = (uint8_t)fm4_clamp(o[TQ5_OP_OUT] + d, 0, 99);
        if (which == 1) o[TQ5_OP_AR] = (uint8_t)fm4_clamp(o[TQ5_OP_AR] - d, 1, 31);
        if (which == 2) o[TQ5_OP_RR] = (uint8_t)fm4_clamp(o[TQ5_OP_RR] - d, 1, 15);
    }
    easy[which] += d;
}

static void edit_param(const param *p, int d)
{
    if (!d || p->kind == K_NONE) return;
    if (p->kind == K_EASY) { easy_edit(p->off, d); fm4_edit_commit(part()); return; }
    if (p->kind == K_OPON) { V()->opmask ^= (uint8_t)(1U << (op - 1)); fm4_edit_commit(part()); return; }
    if (p->kind == K_SONG && p->off == 0) {
        seq_song *g = fm4_seq()->song; g->tempo = (uint16_t)fm4_clamp(g->tempo + d * 10, 300, 3000); return;
    }
    if (p->kind == K_ACT) {
        if (p->off == 0) { uint8_t *c = &V()->vced[TQ5_NAME + save_cursor]; *c = (uint8_t)fm4_clamp(*c + d, 32, 126); }
        else if (p->off == 1) save_slot = (unsigned)fm4_clamp((int)save_slot + d, 0, 99);
        else if (p->off == 2 && fm4_seq()->state == SEQ_STOPPED) { seq_clear_track(fm4_seq()->song, song_track); show("TRACK CLEARED"); }
        else if (p->off == 3) fm4_song_save();
        return;
    }
    unsigned max; int lo;
    uint8_t *ref = param_ref(p, &max, &lo);
    if (!ref) return;
    int v = lo < 0 ? (int)(int8_t)*ref : (int)*ref;
    v = fm4_clamp(v + d, lo, (int)max);
    *ref = (uint8_t)v;
    switch (p->kind) {
    case K_OPV: case K_OPA: case K_V: case K_A: case K_A2: case K_EF: fm4_edit_commit(part()); break;
    case K_SYS:
        if (p->off == 1) fm4_set_mode((unsigned)v);
        else if (p->off == 2) fm4_apply_multi();
        else if (p->off == 6 || p->off == 7) fm4_apply_system();
        fm4_mark_dirty();
        break;
    case K_PART: fm4_apply_multi(); fm4_mark_dirty(); break;
    default: break;
    }
}

/* ---- Drawing -------------------------------------------------------------- */
/* Operator boxes for the 8 algorithms: column and level (0 = carrier row,
 * drawn at the bottom) for OP1..OP4, as on Yamaha's algorithm charts. */
static const uint8_t alg_pos[8][4][2] = {
    {{0,0},{0,1},{0,2},{0,3}}, {{0,0},{0,1},{0,2},{1,2}}, {{0,0},{0,1},{0,2},{1,1}}, {{0,0},{1,1},{0,1},{0,2}},
    {{0,0},{0,1},{1,0},{1,1}}, {{0,0},{1,0},{2,0},{1,1}}, {{0,0},{1,0},{2,0},{2,1}}, {{0,0},{1,0},{2,0},{3,0}},
};
static const uint8_t alg_links[8][4] = {   /* modulator -> target (OPn<<4 | OPm), 0 = none */
    {0x43,0x32,0x21,0}, {0x42,0x32,0x21,0}, {0x41,0x32,0x21,0}, {0x43,0x31,0x21,0},
    {0x43,0x21,0,0}, {0x41,0x42,0x43,0}, {0x43,0,0,0}, {0,0,0,0},
};
static void draw_algorithm(unsigned x0, unsigned y0, unsigned alg, unsigned highlight)
{
    alg &= 7;
    unsigned bx[5], by[5], rows = 0;
    for (unsigned n = 0; n < 4; ++n) if (alg_pos[alg][n][1] + 1U > rows) rows = alg_pos[alg][n][1] + 1U;
    for (unsigned n = 1; n <= 4; ++n) {
        bx[n] = x0 + alg_pos[alg][n - 1][0] * 22U;
        by[n] = y0 + (rows - 1U - alg_pos[alg][n - 1][1]) * 22U;
    }
    for (unsigned k = 0; k < 4; ++k) {
        unsigned l = alg_links[alg][k]; if (!l) continue;
        unsigned a = l >> 4, b = l & 15;
        line(bx[a] + 8, by[a] + 16, bx[b] + 8, by[b], DIM);
    }
    /* feedback loop on OP4 */
    line(bx[4] + 16, by[4] + 4, bx[4] + 19, by[4] + 4, DIM); line(bx[4] + 19, by[4] + 4, bx[4] + 19, by[4] - 3, DIM);
    line(bx[4] + 19, by[4] - 3, bx[4] + 8, by[4] - 3, DIM); line(bx[4] + 8, by[4] - 3, bx[4] + 8, by[4], DIM);
    for (unsigned n = 1; n <= 4; ++n) {
        int on = (V()->opmask >> (n - 1)) & 1;
        uint16_t c = n == highlight ? ACCENT : on ? TEXT : DIM;
        if (n == highlight) fill(bx[n], by[n], 17, 16, PANEL);
        frame(bx[n], by[n], 17, 16, c);
        char s[2] = { (char)('0' + n), 0 };
        text(bx[n] + 6, by[n] + 5, s, 1, c);
        if (is_carrier(alg, n)) line(bx[n] + 8, by[n] + 16, bx[n] + 8, by[n] + 19, c);
    }
}

static void draw_envelope(unsigned x0, unsigned y0, unsigned w, unsigned h)
{
    const uint8_t *o = opv();
    unsigned ar = o[TQ5_OP_AR], d1 = o[TQ5_OP_D1R], l1 = o[TQ5_OP_D1L], d2 = o[TQ5_OP_D2R], rr = o[TQ5_OP_RR];
    unsigned xa = x0 + (31 - ar) * (w / 4) / 31, xd = xa + (31 - d1) * (w / 4) / 31;
    unsigned yl = y0 + h - 1 - l1 * (h - 1) / 15;
    unsigned xs = x0 + w * 3 / 4, ys = d2 ? (yl + (y0 + h - yl) * d2 / 62) : yl;
    unsigned xr = xs + (16 - rr) * (w / 4) / 15;
    if (xr > x0 + w) xr = x0 + w;
    frame(x0 - 2, y0 - 2, w + 4, h + 4, PANEL);
    line(x0, y0 + h, xa, y0, AMBER); line(xa, y0, xd, yl, AMBER); line(xd, yl, xs, ys, AMBER); line(xs, ys, xr, y0 + h, AMBER);
}

static void draw_knobs(void)
{
    const param *ps = params[page][sub];
    for (unsigned k = 0; k < 4; ++k) {
        unsigned x = 2 + 60 * k;
        fill(x, 196, 56, 42, PANEL);
        fill(x, 196, 56, 3, knob_colors[k]);
        if (ps[k].kind == K_NONE) continue;
        char v[24];
        text(x + 3, 203, ps[k].label, 1, DIM);
        param_text(&ps[k], v);
        unsigned sc = text_width(v, 2) <= 52 ? 2 : 1;
        text(x + 3, sc == 2 ? 218 : 222, v, sc, TEXT);
    }
}

static void draw_header(void)
{
    char s[24];
    fill(0, 0, 240, 20, PANEL);
    text(4, 6, "FM-4", 1, ACCENT);
    text(34, 6, page_title[page], 1, TEXT);
    if (page_subs[page] > 1) { s[0] = (char)('1' + sub); s[1] = '/'; s[2] = (char)('0' + page_subs[page]); s[3] = 0; text(100, 6, s, 1, DIM); }
    if (S()->mode) { strcpy(s, "P"); fmt_uint(s + 1, part() + 1, 1); text(124, 6, s, 1, AMBER); }
    seq_state *q = fm4_seq();
    unsigned t = q->song->tempo;
    fmt_uint(s, t / 10, 1);
    text(170, 6, s, 1, DIM);
    uint16_t c = q->state == SEQ_RECORDING ? RED : q->state == SEQ_PLAYING ? GREEN : DIM;
    if (q->state == SEQ_STOPPED) fill(222, 5, 9, 9, c);
    else if (q->state == SEQ_RECORDING) { fill(220, 5, 11, 11, c); }
    else for (unsigned i = 0; i < 9; ++i) line(222, 5 + i / 2, 222 + i, 5 + i / 2, c), line(222, 13 - i / 2, 222 + i, 13 - i / 2, c);
    /* voice activity meter */
    unsigned n = fm4_voices_active();
    for (unsigned i = 0; i < 8; ++i) fill(190 + i * 3, 15, 2, 3, i < n ? GREEN : BACK);
}

static void draw_voice_line(unsigned y)
{
    char s[24], name[11];
    fm4_multi_setup *m = fm4_multi();
    unsigned p = part();
    strcpy(s, m->bank[p] ? "U" : "P"); fmt_uint(s + 1, m->program[p], 2);
    text(6, y, s, 2, AMBER);
    tq5_voice_name(V(), name);
    text(48, y, name, 2, TEXT);
}

static void draw(void)
{
    dl_reset();
    fill(0, 0, 240, 240, BACK);
    draw_header();
    char s[24];
    switch (page) {
    case PG_HOME: {
        fm4_multi_setup *m = fm4_multi();
        unsigned p = part();
        strcpy(s, m->bank[p] ? "USER" : "PRESET"); text(8, 30, s, 1, DIM);
        if (S()->mode) { strcpy(s, "PART "); fmt_uint(s + 5, p + 1, 1); strcat(s, " CH "); unsigned ch = m->cfg[p].channel; if (ch >= 16) strcat(s, "OMNI"); else fmt_uint(s + strlen(s), ch + 1, 1); text(120, 30, s, 1, AMBER); }
        else { strcpy(s, "CH "); unsigned ch = S()->rx_channel; if (ch >= 16) strcat(s, "OMNI"); else fmt_uint(s + 3, ch + 1, 1); text(180, 30, s, 1, DIM); }
        fmt_uint(s, m->program[p], 2);
        text(8, 46, s, 6, AMBER);
        char name[11]; tq5_voice_name(V(), name);
        text(8, 100, name, 3, TEXT);
        strcpy(s, "ALG "); fmt_uint(s + 4, V()->vced[TQ5_ALG] + 1U, 1); text(8, 132, s, 1, DIM);
        const param fxp = { K_EF, 0, F_FX, "" }; param_text(&fxp, s); text(60, 132, "FX", 1, DIM); text(78, 132, s, 1, DIM);
        draw_algorithm(128, 140, V()->vced[TQ5_ALG], 0);
        if (S()->mode) {
            for (unsigned k = 0; k < 8; ++k) {
                uint16_t c = k == p ? AMBER : fm4_part_activity(k) ? GREEN : m->cfg[k].enabled ? PANEL : BACK;
                fill(8 + k * 13, 150, 10, 10, c);
            }
        }
        if (octave) { strcpy(s, "OCT "); fmt_int(s + 4, octave); text(8, 170, s, 1, DIM); }
        break;
    }
    case PG_EDIT: case PG_ENV: {
        draw_voice_line(28);
        for (unsigned n = 1; n <= 4; ++n) {
            unsigned x = 8 + (n - 1) * 30;
            int on = (V()->opmask >> (n - 1)) & 1;
            fill(x, 52, 26, 16, n == op ? ACCENT : PANEL);
            strcpy(s, "OP"); s[2] = (char)('0' + n); s[3] = 0;
            text(x + 4, 57, s, 1, n == op ? BACK : on ? TEXT : DIM);
        }
        strcpy(s, "ALG "); fmt_uint(s + 4, V()->vced[TQ5_ALG] + 1U, 1); text(140, 57, s, 1, DIM);
        if (page == PG_ENV) draw_envelope(16, 84, 140, 90);
        else {
            unsigned r = opa()[TQ5_OP_FIX] ? 0 : opz_ratio_x100(opv()[TQ5_OP_CRS], opa()[TQ5_OP_FINE]);
            const param fp = { K_OPV, TQ5_OP_CRS, F_RATIO, "" }; param_text(&fp, s);
            text(16, 90, opa()[TQ5_OP_FIX] ? "FIXED" : "RATIO", 1, DIM);
            text(16, 104, s, 3, TEXT);
            strcpy(s, "W"); fmt_uint(s + 1, opa()[TQ5_OP_OSW] + 1U, 1); text(16, 140, s, 2, AMBER);
            /* level bar */
            fill(16, 166, 99 + 2, 8, PANEL); fill(17, 167, opv()[TQ5_OP_OUT], 6, GREEN);
            (void)r;
        }
        draw_algorithm(168, 84, V()->vced[TQ5_ALG], op);
        break;
    }
    case PG_LFO: case PG_VOICE: case PG_FX:
        draw_voice_line(28);
        if (page == PG_LFO) {
            /* LFO shape preview */
            unsigned w = V()->vced[TQ5_LFW] & 3, px0 = 40, py0 = 100;
            for (unsigned x = 0; x < 160; x += 4) {
                unsigned ph = (x * 3) % 64, ph2 = ((x + 4) * 3) % 64, y, y2;
                if (w == 0) { y = 63 - ph; y2 = ph2 > ph ? 63 - ph2 : 63 - ph - 12; }
                else if (w == 1) { y = ph < 32 ? 0 : 63; y2 = ph2 < 32 ? 0 : 63; }
                else if (w == 2) { y = ph < 32 ? 63 - ph * 2 : (ph - 32) * 2; y2 = ph2 < 32 ? 63 - ph2 * 2 : (ph2 - 32) * 2; }
                else { y = (unsigned)((((x / 16) * 2654435761U) >> 26) & 63); y2 = (unsigned)(((((x + 4) / 16) * 2654435761U) >> 26) & 63); }
                line(px0 + x, py0 + y, px0 + x + 4, py0 + y2, AMBER);
            }
        } else if (page == PG_FX) {
            const param fxp = { K_EF, 0, F_FX, "" }; param_text(&fxp, s); text(16, 90, s, 3, TEXT);
            fill(16, 130, 202, 10, PANEL); fill(17, 131, V()->efeds[2] * 2U, 8, AMBER);
            text(16, 146, "DRY", 1, DIM); text(196, 146, "WET", 1, DIM);
        } else {
            draw_algorithm(150, 70, V()->vced[TQ5_ALG], 0);
            strcpy(s, "FB "); fmt_uint(s + 3, V()->vced[TQ5_FBL], 1); text(16, 80, s, 2, TEXT);
        }
        break;
    case PG_GLO:
        text(16, 40, "FM-4 TQ5 GENERATOR", 1, ACCENT);
        text(16, 56, "FIRMWARE " FM4_VERSION, 1, DIM);
        text(16, 70, FM1_UPDATE_PRODUCT, 1, DIM);
        strcpy(s, "VOICES IN USE "); fmt_uint(s + strlen(s), fm4_voices_active(), 1); text(16, 100, s, 1, TEXT);
        strcpy(s, "CPU PEAK "); fmt_uint(s + strlen(s), cpu_peak_pct, 1); strcat(s, "%"); text(16, 116, s, 1, cpu_peak_pct > 85 ? RED : cpu_peak_pct > 70 ? AMBER : GREEN);
        strcpy(s, "OVERRUNS "); fmt_uint(s + strlen(s), cpu_overruns, 1); text(16, 132, s, 1, cpu_overruns ? RED : DIM);
        fill(16, 146, 102, 8, PANEL); fill(17, 147, cpu_peak_pct > 100 ? 100 : cpu_peak_pct, 6, cpu_peak_pct > 85 ? RED : GREEN);
        break;
    case PG_SAVE: {
        text(16, 34, "STORE VOICE TO USER", 1, DIM);
        char name[11]; tq5_voice_name(V(), name);
        text(16, 60, name, 3, TEXT);
        fill(16 + save_cursor * 18, 84, 15, 3, ACCENT);
        strcpy(s, "U"); fmt_uint(s + 1, save_slot, 2); text(16, 100, s, 4, AMBER);
        tq5_voice dst; fm4_voice_get(FM4_BANK_USER, save_slot, &dst); tq5_voice_name(&dst, name);
        text(110, 108, name, 1, DIM);
        text(16, 150, "SAVE AGAIN TO CONFIRM", 1, GREEN);
        text(16, 164, "SELECT: CURSOR", 1, DIM);
        break;
    }
    case PG_MULTI: {
        fm4_multi_setup *m = fm4_multi();
        for (unsigned k = 0; k < 8; ++k) {
            unsigned y = 26 + k * 20, p = k == part();
            fill(4, y, 232, 18, p ? PANEL : BACK);
            s[0] = (char)('1' + k); s[1] = 0; text(8, y + 5, s, 1, p ? ACCENT : DIM);
            strcpy(s, m->bank[k] ? "U" : "P"); fmt_uint(s + 1, m->program[k], 2); text(22, y + 5, s, 1, AMBER);
            char name[11]; tq5_voice_name(fm4_edit(k), name); text(48, y + 5, name, 1, m->cfg[k].enabled ? TEXT : DIM);
            unsigned ch = m->cfg[k].channel; if (ch >= 16) strcpy(s, "OM"); else fmt_uint(s, ch + 1, 2); text(116, y + 5, s, 1, DIM);
            fmt_uint(s, m->cfg[k].reserve, 1); text(136, y + 5, s, 1, DIM);
            fill(152, y + 6, m->cfg[k].volume * 60U / 99U, 5, fm4_part_activity(k) ? GREEN : DIM);
        }
        break;
    }
    case PG_SONG: {
        seq_state *q = fm4_seq(); seq_song *g = q->song;
        strcpy(s, "SONG "); fmt_uint(s + 5, S()->song + 1U, 1); text(8, 28, s, 2, AMBER);
        strcpy(s, "BAR "); fmt_uint(s + 4, q->tick / (4 * SEQ_PPQ) + 1, 2); strcat(s, "/"); fmt_uint(s + strlen(s), g->measures, 2); text(110, 32, s, 1, TEXT);
        strcpy(s, "EV "); fmt_uint(s + 3, g->count, 1); text(186, 32, s, 1, DIM);
        for (unsigned t = 0; t < SEQ_TRACKS; ++t) {
            unsigned y = 52 + t * 16, used = 0;
            for (unsigned i = 0; i < g->count; ++i) if (((g->ev[i].tick >> 24) & 7) == t) { used = 1; break; }
            fill(4, y, 232, 14, t == song_track ? PANEL : BACK);
            strcpy(s, "TR"); s[2] = (char)('1' + t); s[3] = 0; text(8, y + 4, s, 1, t == song_track ? ACCENT : DIM);
            strcpy(s, "P"); s[1] = (char)('1' + g->track_part[t]); s[2] = 0; text(32, y + 4, s, 1, DIM);
            text(52, y + 4, (g->mute >> t) & 1 ? "MUTE" : used ? "DATA" : "----", 1, (g->mute >> t) & 1 ? RED : used ? GREEN : DIM);
            /* position bar */
            unsigned len = seq_song_ticks(g);
            fill(90, y + 5, 140, 4, BACK);
            if (used) for (unsigned i = 0; i < g->count; i += 1) if (((g->ev[i].tick >> 24) & 7) == t && (g->ev[i].status & 0xf0) == 0x90)
                fill(90 + (g->ev[i].tick & 0xffffff) * 139U / len, y + 4, 1, 6, TEXT);
        }
        unsigned len = seq_song_ticks(g);
        fill(90 + q->tick * 139U / len, 50, 1, 130, q->state == SEQ_RECORDING ? RED : ACCENT);
        break;
    }
    }
    draw_knobs();
    if (notice && (int32_t)(fm1_millis() - notice_until) < 0) {
        fill(10, 176, 220, 16, AMBER);
        text(14, 181, notice, 1, BACK);
    } else notice = 0;
    band_hashes();
}

/* ---- Input ----------------------------------------------------------------- */
static void leds(uint32_t keys)
{
    uint32_t now = fm1_millis();
    seq_state *q = fm4_seq();
    unsigned blink = (now / 250U) & 1U;
    for (unsigned b = 0; b < 14; ++b) {
        int on = 0;
        for (unsigned p = 0; p < PG_COUNT; ++p) if (page_button[p] == b && page == p) on = 1;
        if (b == BTN_PLAY) on = q->state != SEQ_STOPPED;
        if (b == BTN_REC) on = q->state == SEQ_RECORDING ? (int)blink : 0;
        if (b == OCT_DOWN) on = octave < 0;
        if (b == OCT_UP) on = octave > 0;
        fm1_led_set(b, on);
    }
    for (unsigned k = 0; k < 27; ++k) fm1_led_set(14 + k, (keys >> k) & 1U);
    (void)led_phase;
}

static void goto_page(unsigned p)
{
    if (page == p) sub = (sub + 1) % page_subs[p];
    else { page = p; sub = 0; }
    if (p == PG_SAVE) { save_slot = fm4_multi()->program[part()]; save_cursor = 0; }
}

static void keep_sdk_alive(void)
{
    /* Test-Firmware requires these SDK symbols; only the stock UI referenced
     * them. A reachable call guarded by a volatile that is never set keeps
     * them linked without ever running them. */
#ifndef FM1_HOST_TEST
    if (keepalive_never == 0x5EED) {
        /* Every SDK library function the stock fm1_open app called: they keep
         * the library's native DSP and Bluetooth paths linked exactly as in a
         * stock build (Build.ps1 checks those symbols). Never executed. */
        static struct groove_tuning tun[GROOVE_TRACKS];
        (void)groove_tuning_capture(tun); (void)groove_tuning_apply(tun);
        (void)drum808_nparams(0);
        (void)drum808_param(0,0);
        (void)fm1_ble_connect(0);
        (void)fm1_ble_disconnect();
        (void)fm1_ble_forget_bonds();
        (void)fm1_ble_get_device(0,0);
        fm1_ble_get_status(0);
        (void)fm1_ble_scan();
        (void)fm1_ble_set_role(0);
        (void)fm1_bluetooth_configure(0);
        fm1_bluetooth_get_status(0);
        fm1_engine_pause();
        fm1_engine_resume();
        fm1_engine_stats_read(0);
        fm1_engine_stop();
        (void)fm1_flash_invalidate(0,0);
        (void)fm1_flash_map(0,0);
        groove_adsr_edit(0,0,0);
        groove_adsr_enable(0,0);
        groove_arp_edit(0,0,0);
        (void)groove_arp_enabled(0);
        groove_arp_set(0,0);
        (void)groove_arp_setting(0,0);
        (void)groove_arrangement_capture(0);
        (void)groove_arrangement_tuning_apply(0);
        (void)groove_arrangement_tuning_capture(0);
        groove_audio_stats(0);
        (void)groove_clock_mode();
        groove_clock_set(0);
        groove_control_edit(0,0,0,0);
        (void)groove_delay_ticks();
        (void)groove_drum_steps(0,0);
        groove_edit_checkpoint(0);
        (void)groove_edit_clear(0,0,0);
        (void)groove_edit_clipboard_count();
        (void)groove_edit_copy(0,0,0);
        (void)groove_edit_cursor();
        (void)groove_edit_error();
        (void)groove_edit_paste(0,0,0);
        groove_edit_record(0,0,0);
        (void)groove_edit_recording();
        (void)groove_edit_repeat(0,0);
        groove_edit_seek(0,0);
        (void)groove_edit_undo();
        groove_effects_edit(0,0);
        groove_effects_get(0);
        (void)groove_fm_copy(0,0,0);
        groove_fm_edit(0,0,0);
        (void)groove_fm_import(0,0);
        groove_fm_name_edit(0,0,0);
        groove_get_view(0,0,0);
        (void)groove_key_note(0,0,0);
        groove_lfo_copy(0,0);
        groove_lfo_edit(0,0,0);
        (void)groove_lfo_rate(0);
        (void)groove_lock_parameter_name(0,0);
        (void)groove_master_state();
        (void)groove_midi_activity(0);
        (void)groove_midi_input_channel(0);
        (void)groove_midi_input_set(0,0);
        groove_mix_toggle(0,0);
        groove_mono_set(0,0);
        groove_operator_toggle(0,0);
        (void)groove_pad_count(0);
        (void)groove_pad_index(0,0);
        (void)groove_pad_label(0,0);
        (void)groove_pad_name(0,0);
        (void)groove_pad_note(0,0);
        (void)groove_project_apply(0);
        (void)groove_project_apply_edit(0,0);
        groove_project_arp_defaults(0);
        groove_project_clean();
        (void)groove_project_dirty();
        groove_project_touch();
        (void)groove_project_valid(0);
        groove_samples_stop();
        groove_sends_edit(0,0,0);
        groove_sends_get(0,0);
        (void)groove_sequence_chord_name(0);
        (void)groove_sequence_count_in();
        groove_sequence_defaults(0);
        groove_sequence_edit(0,0,0,0);
        (void)groove_sequence_length(0);
        groove_sequence_length_set(0,0);
        (void)groove_sequence_rewound();
        (void)groove_sequence_setting(0);
        groove_sequence_setting_edit(0,0);
        (void)groove_source_set(0,0);
        (void)groove_source_type(0);
        (void)groove_step_setting(0,0,0);
        (void)groove_step_setting_edit(0,0,0,0);
        groove_tempo_edit(0);
        (void)groove_tempo_tap(0);
        (void)groove_timbre_export(0,0,0);
        (void)groove_timbre_import(0,0,0);
        (void)groove_tone_copy(0,0,0);
        (void)groove_tone_count(0,0);
        groove_tone_edit(0,0,0,0);
        (void)groove_tone_parameter(0,0,0);
        (void)groove_track_role(0);
        (void)sampledrum_bind(0,0,0,0,0);
        (void)sampledrum_param(0);
    }
#endif
}

void groove_screen_init(void)
{
    /* The stock instrument stays linked but silent: release all its tracks. */
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) groove_track_role_set(t, GROOVE_TRACK_OFF);
    fm4_core_init();
    fm1_controls_init();
    for (unsigned k = 0; k < 41; ++k) fm1_led_set(k, 0);
    fm1_screen_init();
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, BACK);
    memset(shown_hash, 0, sizeof shown_hash);
    memset(key_note, 0xff, sizeof key_note);
    page = PG_HOME; sub = 0; op = 1;
    show("FM-4 " FM4_VERSION);
    draw();
    keep_sdk_alive();
}

void groove_screen_control(void)
{
    struct fm1_controls c;
    fm1_controls_read(&c);
    uint32_t now = fm1_millis();
    uint32_t pressed = c.buttons & ~prev_buttons, released = prev_buttons & ~c.buttons;
    uint32_t kdown = c.keys & ~prev_keys, kup = prev_keys & ~c.keys;
    int changed = pressed || released || kdown || kup;
    for (unsigned i = 0; i < 7; ++i) changed |= c.encoders[i] != 0;
    if (groove_master_update(c.knob)) {
        unsigned level = fm1_engine_output_level();
        strcpy(notice_buf, "MASTER "); fmt_uint(notice_buf + 7, (level * 100U + 63U) / 127U, 1);
        show(notice_buf); changed = 1;
    }
    for (unsigned b = 0; b < 14; ++b) if (pressed & BUTTON(b)) button_down_ms[b] = now;

    /* chords first */
    if ((pressed & BUTTON(BTN_PLAY)) && (c.buttons & BUTTON(BTN_HOME))) {
        uint8_t m[3] = { 0xB0, 123, 0 };
        for (unsigned ch = 0; ch < 16; ++ch) { m[0] = (uint8_t)(0xB0 | ch); fm4_post_local(m, 3); }
        fm4_transport(0); show("ALL NOTES OFF"); chord_used = 1; pressed &= ~BUTTON(BTN_PLAY);
    }
    if ((c.buttons & BUTTON(OCT_DOWN)) && (c.buttons & BUTTON(OCT_UP)) && (pressed & (BUTTON(OCT_DOWN) | BUTTON(OCT_UP)))) {
        octave = 0; pressed &= ~(BUTTON(OCT_DOWN) | BUTTON(OCT_UP)); show("OCTAVE 0");
    }
    if (pressed & BUTTON(OCT_DOWN)) octave = octave > -3 ? octave - 1 : -3;
    if (pressed & BUTTON(OCT_UP)) octave = octave < 3 ? octave + 1 : 3;

    /* page buttons */
    for (unsigned p = 0; p < PG_COUNT; ++p) {
        if (!(pressed & BUTTON(page_button[p]))) continue;
        if (p == PG_SAVE && page == PG_SAVE) {
            if (!fm4_store(part(), save_slot)) show("STORED"); else show("STORE FAILED");
            page = PG_HOME;
            continue;
        }
        if (p == PG_HOME && (c.buttons & BUTTON(BTN_PLAY))) continue;
        goto_page(p);
    }
    if (pressed & BUTTON(BTN_PLAY)) fm4_transport(fm4_seq()->state == SEQ_STOPPED ? 1 : 0);
    if (pressed & BUTTON(BTN_REC)) {
        if (fm4_seq()->state == SEQ_RECORDING) fm4_transport(0);
        else { fm4_seq()->song->track_part[song_track] = (uint8_t)part(); fm4_record(song_track); show("RECORDING"); }
    }

    /* encoders */
    int16_t *e = c.encoders;
    if (e[6]) {   /* PRESETS: voice number */
        fm4_multi_setup *m = fm4_multi();
        int prog = (int)m->program[part()] + e[6];
        unsigned bank = m->bank[part()];
        while (prog < 0) { prog += 100; bank ^= 1; }
        while (prog > 99) { prog -= 100; bank ^= 1; }
        fm4_select(part(), bank, (unsigned)prog);
        easy[0] = easy[1] = easy[2] = 0;
    }
    if (e[1]) {   /* ALGORITHM */
        if (page == PG_SONG) song_track = (unsigned)fm4_clamp((int)song_track + e[1], 0, 7);
        else { const param a = { K_V, TQ5_ALG, F_NUM, "" }; edit_param(&a, e[1]); }
    }
    if (e[0]) {   /* SELECT */
        switch (page) {
        case PG_EDIT: case PG_ENV: op = (unsigned)fm4_clamp((int)op + e[0], 1, 4); break;
        case PG_SAVE: save_cursor = (unsigned)fm4_clamp((int)save_cursor + e[0], 0, 9); break;
        case PG_SONG:
            if (fm4_seq()->state == SEQ_STOPPED) fm4_song_load((unsigned)fm4_clamp((int)S()->song + e[0], 0, 7));
            break;
        case PG_HOME:
            if (!S()->mode) { fm4_select(0, !fm4_multi()->bank[0], fm4_multi()->program[0]); break; }
            /* fallthrough */
        default:
            if (S()->mode) fm4_set_active_part((unsigned)fm4_clamp((int)part() + e[0], 0, 7));
            break;
        }
    }
    for (unsigned k = 0; k < 4; ++k) if (e[2 + k]) edit_param(&params[page][sub][k], e[2 + k]);

    /* keys */
    static const uint8_t white[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
    for (unsigned k = 0; k < 27; ++k) {
        if (kdown & (1U << k)) {
            if (c.buttons & BUTTON(BTN_SEQ)) {         /* SEQ + white key 1-8: track mute */
                for (unsigned t = 0; t < 8; ++t) if (white[t] == k) fm4_seq()->song->mute ^= (uint8_t)(1U << t);
                chord_used = 1;
                continue;
            }
            int note = 53 + (int)k + 12 * octave;
            if (note < 0 || note > 127) continue;
            key_note[k] = (uint8_t)note;
            uint8_t m[3] = { 0x90, (uint8_t)note, S()->velocity };
            if (S()->local) fm4_post_local(m, 3);
            if (S()->key_out) { unsigned ch = fm4_multi()->cfg[part()].channel; m[0] = (uint8_t)(0x90 | (ch < 16 ? ch : 0)); fm4_midi_out(m, 3); }
        }
        if ((kup & (1U << k)) && key_note[k] != 0xff) {
            uint8_t m[3] = { 0x80, key_note[k], 0 };
            if (S()->local) fm4_post_local(m, 3);
            if (S()->key_out) { unsigned ch = fm4_multi()->cfg[part()].channel; m[0] = (uint8_t)(0x80 | (ch < 16 ? ch : 0)); fm4_midi_out(m, 3); }
            key_note[k] = 0xff;
        }
    }
    prev_buttons = c.buttons; prev_keys = c.keys;

    if (now - cpu_at >= 1000U) {
        struct fm1_audio_stats st; fm1_audio_stats_read(&st);
        uint32_t bt = fm4_audio_block_ticks();
        cpu_peak_pct = bt ? (unsigned)((uint64_t)st.peak_ticks * 100U / bt) : 0;
        fm4_audio_stats_reset_peak();
        if (st.overruns != cpu_seen_overruns) {
            cpu_overruns += st.overruns - cpu_seen_overruns;
            cpu_seen_overruns = st.overruns;
            /* CPU guard: shed polyphony instead of glitching */
            if (S()->polyphony > 4) { --S()->polyphony; fm4_apply_system(); fm4_mark_dirty(); show("CPU GUARD: VOICES -1"); }
        }
        cpu_at = now;
    }
    const char *n = fm4_notice();
    if (n) { show(n); changed = 1; }
    fm4_core_tick(now);
    leds(c.keys);
    if (changed || now - frame_ms >= 66U) { frame_ms = now; draw(); }
    flush();
    keep_sdk_alive();
}

/* stock hooks other code may call */
unsigned groove_screen_preset_position(void) { return fm4_multi()->program[part()]; }
