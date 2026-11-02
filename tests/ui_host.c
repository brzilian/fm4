/* Host harness for the FM-4 panel UI (app/screen.c): fakes the FM-1 SDK,
 * scripts panel input and writes LCD screenshots (PPM) per page. */
#include "instrument.h"
#include "fm1_engine_control.h"
#include "fm4/fm4_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t lcd[240 * 240];
static struct fm1_controls ctl;
static uint32_t now_ms;
static uint8_t ledbits[41];
static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* SDK fakes */
void fm1_controls_init(void) {}
void fm1_controls_read(struct fm1_controls *s) { *s = ctl; for (int i = 0; i < 7; ++i) ctl.encoders[i] = 0; }
void fm1_led_set(unsigned key, int on) { if (key < 41) ledbits[key] = (uint8_t)on; }
uint32_t fm1_millis(void) { return now_ms; }
void fm1_control_request(void) {}
void fm1_screen_init(void) {}
void fm1_screen_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c)
{ for (unsigned j = y; j < y + h; ++j) for (unsigned i = x; i < x + w; ++i) lcd[j * 240 + i] = c; }
int fm1_screen_submit(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t *p, size_t n)
{
    if (w * h > n || x + w > 240 || y + h > 240) return -1;
    for (unsigned j = 0; j < h; ++j) memcpy(&lcd[(y + j) * 240 + x], p + j * w, w * 2);
    return 0;
}
int fm1_screen_pending(void) { return 0; }
static unsigned master = 100;
int groove_master_update(unsigned adc) { (void)adc; return 0; }
FM1_AUDIO_CODE unsigned fm1_engine_output_level(void) { return master; }
int groove_track_role_set(unsigned t, unsigned r) { (void)t; (void)r; return 0; }
int fm1_ble_set_role(enum fm1_ble_role r) { (void)r; return 0; }
int fm1_ble_scan(void) { return 0; }
int fm1_ble_connect(unsigned i) { (void)i; return 0; }
int fm1_ble_disconnect(void) { return 0; }
int fm1_ble_forget_bonds(void) { return 0; }
void fm1_ble_get_status(struct fm1_ble_status *s) { memset(s, 0, sizeof *s); }
int fm1_ble_get_device(unsigned i, struct fm1_ble_device *d) { (void)i; (void)d; return -1; }
int fm1_bluetooth_configure(unsigned f) { (void)f; return 0; }
void fm1_bluetooth_get_status(struct fm1_bluetooth_status *s) { memset(s, 0, sizeof *s); }
int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS]) { (void)t; return 0; }
int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]) { (void)t; return 0; }
int groove_project_apply_edit(const struct groove_project *p, unsigned o) { (void)p; (void)o; return 0; }
/* FM-4 hw fakes */
#define FLASH 536576
static uint8_t flash[FLASH];
uint32_t fm4_hw_flash_size(void) { return FLASH; }
int fm4_hw_flash_read(uint32_t o, void *d, size_t n) { memcpy(d, flash + o, n); return 0; }
int fm4_hw_flash_erase(uint32_t o) { memset(flash + o, 0xff, 4096); return 0; }
int fm4_hw_flash_write(uint32_t o, const void *d, size_t n) { memcpy(flash + o, d, n); return 0; }
int fm4_hw_midi_send(const uint8_t *m, size_t n) { (void)m; (void)n; return 0; }
void fm4_hw_audio_gate(int c) { (void)c; }

void groove_screen_init(void);
void groove_screen_control(void);

static int32_t audio[256];
static void step(int polls)
{
    while (polls--) {
        now_ms += 10;
        fm4_core_render(audio);
        groove_screen_control();
        for (int i = 0; i < 20; ++i) groove_screen_control();   /* let all bands flush */
    }
}
static void press(unsigned b) { ctl.buttons |= 1U << b; step(1); ctl.buttons &= ~(1U << b); step(1); }
static void turn(unsigned enc, int d) { ctl.encoders[enc] = (int16_t)d; step(1); }
static void dump_json(const char *name);
static void shot(const char *name)
{
    now_ms += 1700; step(1);   /* let transient notices expire */
    char path[128]; snprintf(path, sizeof path, "out/ui_%s.ppm", name);
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < 240 * 240; ++i) {
        uint16_t c = lcd[i];
        unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    dump_json(name);
}
/* mirror of screen.c's item struct */
typedef struct { uint8_t op, scale; uint16_t fg, bg; uint8_t x, y, w, h; char s[22]; } ui_item;
unsigned fm4_ui_items(const void **list, unsigned *item_size);
static void dump_json(const char *name)
{
    const void *l; unsigned sz, n = fm4_ui_items(&l, &sz);
    if (sz != sizeof(ui_item)) { printf("item size mismatch\n"); return; }
    char path[128]; snprintf(path, sizeof path, "out/ui_%s.json", name);
    FILE *f = fopen(path, "w"); fprintf(f, "[");
    const ui_item *it = l;
    for (unsigned i = 0; i < n; ++i) {
        fprintf(f, "%s{\"op\":%u,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"fg\":%u,\"scale\":%u,\"s\":\"", i ? "," : "", it[i].op, it[i].x, it[i].y, it[i].w, it[i].h, it[i].fg, it[i].scale);
        for (const char *c = it[i].s; *c; ++c) { if (*c == '"' || *c == '\\') fputc('\\', f); fputc(*c, f); }
        fprintf(f, "\"}");
    }
    fprintf(f, "]\n"); fclose(f);
}
static unsigned lit(void) { unsigned n = 0; for (int i = 0; i < 240 * 240; ++i) n += lcd[i] != 0; return n; }

int main(void)
{
    memset(flash, 0xff, sizeof flash);
    groove_screen_init();
    step(3); shot("home");
    CHECK(lit() > 2000, "home screen drawn (%u px)", lit());
    turn(6, 5); shot("home_next");
    CHECK(fm4_multi()->program[0] == 5, "PRESETS selects voice 5 (%u)", fm4_multi()->program[0]);
    turn(2, 6); step(1);
    press(6); turn(0, 1); shot("edit_freq");
    press(6); shot("edit_wave");
    press(6); shot("edit_scale");
    turn(5, -10); CHECK(fm4_edit(0)->vced[13 * 1 + TQ5_OP_AME] == 0, "AM off");
    press(4); turn(2, -5); shot("env");
    press(5); shot("lfo");
    press(3); shot("voice");
    press(2); turn(2, 3); shot("fx");
    press(7); shot("system");
    turn(2, 1); step(2); CHECK(fm4_sys()->mode == 1, "switched to MULTI");
    press(10); turn(0, 2); shot("multi");
    CHECK(fm4_active_part() == 2, "SELECT picks part 3 (%u)", fm4_active_part());
    press(8); shot("home_multi");
    press(11); turn(2, 4); shot("song");
    /* play a key, record a take */
    press(13); ctl.keys = 1U << 7; step(20); ctl.keys = 0; step(20); press(13); step(2);
    shot("song_rec");
    CHECK(fm4_seq()->song->count >= 2, "recorded from keys (%u)", fm4_seq()->song->count);
    press(9); turn(0, 2); turn(2, 1); shot("save");
    press(9); step(2); shot("after_save");
    tq5_voice v; fm4_voice_get(FM4_BANK_USER, fm4_multi()->program[fm4_active_part()], &v);
    CHECK(fm4_multi()->bank[fm4_active_part()] == FM4_BANK_USER, "stored into user bank");
    printf(failures ? "%d FAILURES\n" : "all ui tests passed\n", failures);
    return failures ? 1 : 0;
}
uint32_t fm4_audio_block_ticks(void) { return 1000; }
void fm4_audio_stats_reset_peak(void) {}
uint32_t fm4_audio_render_peak(void) { return 300; }
uint32_t fm4_audio_late_blocks(void) { return 0; }
void fm1_audio_stats_read(struct fm1_audio_stats *s) { memset(s, 0, sizeof *s); s->peak_ticks = 420; }
