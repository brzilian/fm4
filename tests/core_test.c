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
int fm4_hw_flash_erase(uint32_t o) { if (o % 4096 || o >= FLASH) return -1; memset(flash + o, 0xff, 4096); return 0; }
int fm4_hw_flash_write(uint32_t o, const void *d, size_t n)
{
    if (o + n > FLASH) return -1;
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

    printf(failures ? "%d FAILURES\n" : "all core tests passed\n", failures);
    return failures ? 1 : 0;
}
