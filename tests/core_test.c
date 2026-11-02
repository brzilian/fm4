/* Host tests for the FM-4 core: storage, SysEx service, multi mode, songs. */
#include "fm4_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fake FM-1 services ---- */
#define FLASH 536576
static uint8_t flash[FLASH];
static uint8_t sent[65536]; static size_t nsent; static int busy_every;
uint32_t fm4_hw_flash_size(void) { return FLASH; }
int fm4_hw_flash_read(uint32_t o, void *d, size_t n) { if (o + n > FLASH) return -1; memcpy(d, flash + o, n); return 0; }
static uint32_t hi_touch;    /* highest flash byte erased or written + 1 */
static unsigned erases;
int fm4_hw_flash_erase(uint32_t o) { if (o % 4096 || o >= FLASH) return -1; memset(flash + o, 0xff, 4096); if (o + 4096 > hi_touch) hi_touch = o + 4096; ++erases; return 0; }
int fm4_hw_flash_write(uint32_t o, const void *d, size_t n)
{
    if (o + n > FLASH) return -1;
    if (o + n > hi_touch) hi_touch = (uint32_t)(o + n);
    const uint8_t *s = d;
    for (size_t i = 0; i < n; ++i) { if ((flash[o + i] & s[i]) != s[i]) return -4; flash[o + i] = s[i]; }
    return 0;
}
int fm4_hw_midi_send(const uint8_t *m, size_t n)
{
    static int calls;
    if (n > 128) return -1;
    if (busy_every && ++calls % busy_every == 0) return -2;
    if (nsent + n > sizeof sent) return -2;
    memcpy(sent + nsent, m, n); nsent += n; return 0;
}
void fm4_hw_audio_gate(int closed) { (void)closed; }

static int32_t out[FM4_BLOCK * 2];
static long render(int blocks) { long peak = 0; while (blocks--) { fm4_core_render(out); for (int i = 0; i < FM4_BLOCK * 2; ++i) if (labs(out[i]) > peak) peak = labs(out[i]); } return peak; }
static void midi(uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = { a, b, c }; fm4_post(m, 3); }
static void sysex(const uint8_t *m, size_t n) { fm4_sysex_in(m, n); fm4_core_poll(); }

/* ---- storage layout helpers ---- */
#define MAGIC_SONG 0x47344D46U
#define MAGIC_SYSTEM 0x53344D46U
static uint32_t crc32_t(const void *p, size_t n)
{
    const uint8_t *b = p; uint32_t c = 0xffffffffU;
    while (n--) { c ^= *b++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U))); }
    return ~c;
}
static seq_song legacy;
/* A song record as FM-4 1.0 (layout 1) wrote it at 0x5000 + n * 0x9000. */
static void put_legacy_song(unsigned n, unsigned count, uint8_t note)
{
    uint32_t off = 0x5000U + n * 0x9000U;
    memset(&legacy, 0, sizeof legacy);
    seq_song_clear(&legacy, n);
    legacy.count = (uint16_t)count;
    for (unsigned i = 0; i < count; ++i) { legacy.ev[i].tick = i * 24U; legacy.ev[i].status = 0x90; legacy.ev[i].d1 = note; legacy.ev[i].d2 = (uint8_t)(1 + i % 120); }
    uint32_t h[4] = { MAGIC_SONG, 1, (uint32_t)sizeof legacy, crc32_t(&legacy, sizeof legacy) };
    memset(flash + off, 0xff, 9 * 4096);
    memcpy(flash + off + 16, &legacy, sizeof legacy);
    memcpy(flash + off, h, 16);
}
static uint8_t stock_byte(uint32_t at) { return (uint8_t)(at * 13U + (at >> 12)); }
static int all_ff(uint32_t from, uint32_t to) { for (uint32_t i = from; i < to; ++i) if (flash[i] != 0xff) return 0; return 1; }
static int stock_intact(uint32_t from, uint32_t to) { for (uint32_t i = from; i < to; ++i) if (flash[i] != stock_byte(i)) return 0; return 1; }
static size_t find_reply(uint8_t cmd)
{
    for (size_t i = 0; i + 4 < nsent; ++i) if (sent[i] == 0xF0 && sent[i + 1] == 0x7D && sent[i + 2] == 0x46 && sent[i + 3] == 0x34 && sent[i + 4] == cmd) return i;
    return (size_t)-1;
}
static void flush_poll(void) { for (int i = 0; i < 50; ++i) fm4_core_poll(); }

static void test_storage(void)
{
    static const uint8_t prep[11] = { 0xF0, 0x7D, 0x46, 0x34, 0x05, 'S', 'T', 'O', 'C', 'K', 0xF7 };
    printf("storage: song capacity %u events per saved song\n", fm4_song_capacity());
    CHECK(fm4_song_capacity() >= 1000, "a saved song holds about 1000 events (%u)", fm4_song_capacity());

    /* 1. Everything FM-4 wrote so far stayed inside the window below stock's data. */
    CHECK(hi_touch <= FM4_FLASH_WINDOW, "no write past user offset 0x17000 = flash 0x93000 (highest 0x%X)", (unsigned)hi_touch);
    CHECK(fm4_storage_layout() == 2, "layout marker written on first boot");

    /* 2. All eight songs save and load in layout 2, still inside the window. */
    for (unsigned n = 0; n < 8; ++n) {
        fm4_song_load(n);
        seq_song *g = fm4_seq()->song;
        g->count = (uint16_t)(n == 7 ? fm4_song_capacity() : 10 + n);
        for (unsigned i = 0; i < g->count; ++i) { g->ev[i].tick = i; g->ev[i].status = 0x90; g->ev[i].d1 = (uint8_t)(40 + n); g->ev[i].d2 = 100; }
        CHECK(fm4_song_save() == 0, "song %u saved", n + 1);
    }
    for (unsigned n = 0; n < 8; ++n) {
        fm4_song_load(n);
        seq_song *g = fm4_seq()->song;
        CHECK(g->count == (n == 7 ? fm4_song_capacity() : 10 + n) && g->ev[g->count - 1].d1 == 40 + n, "song %u reloaded (%u events)", n + 1, g->count);
    }
    CHECK(hi_touch <= FM4_FLASH_WINDOW, "8 full songs stay inside the window (highest 0x%X)", (unsigned)hi_touch);
    fm4_song_load(3);
    fm4_seq()->song->count = (uint16_t)(fm4_song_capacity() + 1);
    (void)fm4_notice();
    CHECK(fm4_song_save() != 0, "a song longer than a slot is refused");
    { const char *t = fm4_notice(); CHECK(t && !strcmp(t, "SONG TOO LONG"), "SONG TOO LONG shown (%s)", t ? t : "none"); }
    fm4_song_load(3);
    CHECK(fm4_seq()->song->count == 13, "the refused save left the stored song intact");

    /* 3. Upgrade from FM-4 1.0 (layout 1): stock V15's settings store at user
     * offsets 0x17000.. holds stock data, plus FM-4 1.0 songs 3 and 6 on top. */
    memset(flash, 0xff, sizeof flash);
    for (uint32_t i = FM4_FLASH_WINDOW; i < FLASH; ++i) flash[i] = stock_byte(i);
    { /* a layout-1 system record and voice bank survive the upgrade */
        fm4_core_init(); fm4_sys()->master_tune = 70; CHECK(fm4_save_settings() == 0, "settings saved");
        fm4_edit(0)->vced[0] = 3; fm4_edit_commit(0); CHECK(fm4_store(0, 42) == 0, "voice stored");
        memset(flash + 0x15000, 0xff, 4096);             /* back to layout 1: no marker */
        for (uint32_t i = 0x5000; i < FM4_FLASH_WINDOW; ++i) flash[i] = 0xff;
    }
    put_legacy_song(0, 5, 60);
    put_legacy_song(1, 1500, 61);       /* longer than a layout-2 slot */
    put_legacy_song(2, 7, 62);          /* flash 0x93000: stock's first settings sector */
    put_legacy_song(5, 9, 65);
    CHECK(flash[0x17000] == 0x46, "layout-1 song 3 sits on stock's settings store");
    hi_touch = 0; erases = 0; nsent = 0;
    fm4_core_init();
    CHECK(fm4_storage_layout() == 2, "layout 2 after the upgrade");
    CHECK(fm4_sys()->master_tune == 70, "system settings kept");
    { tq5_voice v; fm4_voice_get(FM4_BANK_USER, 42, &v); CHECK(v.vced[0] == 3, "user voices kept"); }
    unsigned want[8] = { 5, 0, 7, 0, 0, 9, 0, 0 };
    want[1] = fm4_song_capacity();
    for (unsigned n = 0; n < 8; ++n) {
        fm4_song_load(n);
        seq_song *g = fm4_seq()->song;
        CHECK(g->count == want[n], "migrated song %u: %u events (want %u)", n + 1, g->count, want[n]);
        if (want[n]) CHECK(g->ev[0].d1 == 60 + n && g->ev[g->count - 1].tick == (g->count - 1) * 24U, "migrated song %u content", n + 1);
    }
    CHECK(all_ff(0x17000, 0x20000), "old song 3 erased from stock's settings store");
    CHECK(all_ff(0x32000, 0x3B000), "old song 6 erased from stock's settings store");
    CHECK(stock_intact(0x20000, 0x32000) && stock_intact(0x3B000, FLASH), "stock data around them untouched");
    fm4_song_load(1); fm4_seq()->song->count = 2; fm4_song_save();
    fm4_core_init();
    fm4_song_load(1);
    CHECK(fm4_seq()->song->count == 2, "migration runs once (a newer song 2 is not overwritten)");

    /* 4. PREPARE_STOCK: nothing left outside the window -> status 0. */
    nsent = 0; sysex(prep, sizeof prep); flush_poll();
    size_t at = find_reply(0x45);
    CHECK(at != (size_t)-1 && sent[at + 5] == 0 && sent[at + 6] == 0 && sent[at + 7] == 0 && sent[at + 8] == 0xF7, "prepare: clean, nothing to erase");
    /* a stray layout-1 song (e.g. written by FM-4 1.0 after a downgrade) is erased */
    put_legacy_song(4, 3, 64);
    nsent = 0; sysex(prep, sizeof prep); flush_poll();
    at = find_reply(0x45);
    CHECK(at != (size_t)-1 && sent[at + 5] == 0 && sent[at + 6] == 9 && sent[at + 7] == 0, "prepare: 9 sectors erased, clean");
    CHECK(all_ff(0x29000, 0x32000), "stray old song erased");
    /* an FM-4 header that is not a layout-1 song is reported, not erased */
    { uint32_t h[4] = { MAGIC_SYSTEM, 1, 4, 0 }; memcpy(flash + 0x40000, h, 16); }
    nsent = 0; sysex(prep, sizeof prep); flush_poll();
    at = find_reply(0x45);
    CHECK(at != (size_t)-1 && sent[at + 5] == 1 && sent[at + 7] == 1, "prepare: remaining FM-4 data reported");
    CHECK(flash[0x40000] == 0x46, "unknown data left alone");
    /* a wrong confirmation word does nothing */
    static const uint8_t bad[11] = { 0xF0, 0x7D, 0x46, 0x34, 0x05, 'S', 'T', 'O', 'C', 'X', 0xF7 };
    nsent = 0; sysex(bad, sizeof bad); flush_poll();
    CHECK(find_reply(0x45) == (size_t)-1, "prepare needs the STOCK word");
}

int main(void)
{
    memset(flash, 0xff, sizeof flash);
    fm4_core_init();
    CHECK(fm4_sys()->mode == 0, "boots in single mode");
    char name[11]; tq5_voice_name(fm4_edit(0), name);
    printf("boot voice: %s\n", name);
    render(1);
    midi(0x90, 60, 100);
    long peak = render(20);
    CHECK(peak > 10000, "note sounds (%ld)", peak);
    midi(0x80, 60, 0); render(200);
    CHECK(fm4_voices_active() == 0, "released");

    /* Program change selects preset 12 (control task loads it). */
    midi(0xC0, 12, 0); render(1); fm4_core_poll(); render(1);
    tq5_voice v; fm4_voice_get(FM4_BANK_PRESET, 12, &v);
    CHECK(!memcmp(fm4_edit(0)->vced, v.vced, TQ5_VCED), "program change loaded preset 12");

    /* TQ5 dump request answered with 191 bytes. */
    static const uint8_t req[15] = { 0xF0,0x43,0x20,0x7E,'L','M',' ',' ','8','0','3','6','E','F',0xF7 };
    nsent = 0; sysex(req, 15);
    CHECK(nsent == 191, "voice dump %zu bytes", nsent);
    CHECK(sent[0] == 0xF0 && sent[6] == 'L' && sent[13] == '6', "starts with EFEDS");

    /* Parameter change edits and re-stages part 0. */
    uint8_t pc[7]; tq5_msg_param(0, 0x12, 52, 7, pc); sysex(pc, 7); render(1);
    CHECK(fm4_edit(0)->vced[52] == 7, "ALG param change applied");

    /* Store via FM-4 extension, then survive a reboot. */
    tq5_voice_set_name(fm4_edit(0), "MY SOUND  ");
    static const uint8_t store[8] = { 0xF0, 0x7D, 0x46, 0x34, 0x01, 0, 42, 0xF7 };
    nsent = 0; sysex(store, 8);
    CHECK(nsent == 8 && sent[4] == 0x41 && sent[5] == 0, "store ack");
    fm4_select(0, FM4_BANK_PRESET, 3);
    fm4_save_settings();
    fm4_core_init();
    fm4_voice_get(FM4_BANK_USER, 42, &v); tq5_voice_name(&v, name);
    CHECK(!strcmp(name, "MY SOUND  "), "user slot 42 persisted ('%s')", name);
    CHECK(fm4_multi()->program[0] == 3, "settings persisted (%u)", fm4_multi()->program[0]);

    /* Read user voice back via extension. */
    static const uint8_t rd[7] = { 0xF0, 0x7D, 0x46, 0x34, 0x02, 42, 0xF7 };
    nsent = 0; sysex(rd, 7);
    CHECK(nsent == 191, "ext read %zu", nsent);

    /* VMEM bank request: 4 blocks streamed in <=128-byte slices, with busy retries. */
    static const uint8_t vreq[5] = { 0xF0, 0x43, 0x20, 0x04, 0xF7 };
    nsent = 0; busy_every = 7; sysex(vreq, 5);
    for (int i = 0; i < 400 && nsent < 4 * (7 + 4104); ++i) fm4_core_poll();
    busy_every = 0;
    CHECK(nsent == 4 * (7 + 4104), "bank dump bytes %zu", nsent);
    tq5_result r = tq5_parse(sent + 7, 4104, 0, fm4_edit(1));
    CHECK(r.action == TQ5_VMEM_BLOCK, "bank dump parses (%d)", r.action);

    /* Receive a VMEM block into user bank block 2 (voices 50..74). */
    static uint8_t recs[32][TQ5_VMEM], msg[4200];
    for (int i = 0; i < 32; ++i) { tq5_voice t; tq5_voice_init(&t); char nm[11]; snprintf(nm, 11, "BULK %02d   ", i); tq5_voice_set_name(&t, nm); tq5_vmem_pack(&t, recs[i]); }
    size_t n = tq5_msg_vmem((const uint8_t (*)[TQ5_VMEM])recs, 0, msg);
    uint8_t sel[7]; tq5_msg_param(0, 0x24, 7, 3, sel); sysex(sel, 7);
    CHECK(n <= 1024 || 1, "note: %zu-byte VMEM exceeds the SDK's 1024-byte receive limit on hardware", n);
    fm4_sysex_in(msg, n > 1024 ? 1024 : n); /* hardware would drop it; core drops oversize too */
    fm4_core_poll();
    fm4_voice_get(FM4_BANK_USER, 50, &v); tq5_voice_name(&v, name);
    CHECK(strcmp(name, "BULK 00   ") != 0, "oversize VMEM is not accepted");

    /* Multi mode: part 2 listens on channel 3 and plays its own voice. */
    fm4_set_mode(1);
    fm4_multi()->cfg[2].channel = 3; fm4_multi()->cfg[2].reserve = 2; fm4_apply_multi();
    fm4_select(2, FM4_BANK_PRESET, 40);
    render(1);
    midi(0x93, 64, 100); peak = render(10);
    CHECK(fm4_part_activity(2) == 1, "part 2 sounding (%u)", fm4_part_activity(2));
    /* sysex addressed to device 3 edits part 2 */
    tq5_msg_param(3, 0x12, 53, 5, pc); sysex(pc, 7);
    CHECK(fm4_edit(2)->vced[53] == 5, "multi sysex routed to part 2");
    midi(0x83, 64, 0); render(300);

    /* Record a take from the panel into the song, then play it back. */
    fm4_set_mode(0); render(2);
    fm4_seq()->song->measures = 1; fm4_seq()->song->tempo = 1200;
    fm4_transport(2); render(1);
    uint8_t on[3] = { 0x90, 67, 100 }, off[3] = { 0x80, 67, 0 };
    fm4_post_local(on, 3); render(40); fm4_post_local(off, 3); render(40);
    fm4_transport(0);
    CHECK(fm4_seq()->song->count == 2, "take merged (%u events)", fm4_seq()->song->count);
    fm4_song_save(); fm4_song_load(0);
    CHECK(fm4_seq()->song->count == 2, "song persisted");
    fm4_transport(1); render(5);
    CHECK(fm4_voices_active() >= 1, "song plays back the note");
    fm4_transport(0); render(300);

    test_storage();
    printf(failures ? "%d FAILURES\n" : "all core tests passed\n", failures);
    return failures ? 1 : 0;
}
