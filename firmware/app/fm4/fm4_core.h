/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 core: the TQ5 instrument as a whole (8 parts, voice banks, effect,
 * song sequencer, SysEx, persistent storage). Platform glue (panel, screen,
 * SDK hooks) lives in fm4_main.c; this file only needs the small fm4_hw_*
 * interface below, so it runs unchanged in host tests and the simulator. */
#ifndef FM4_CORE_H
#define FM4_CORE_H
#include "tq5.h"
#include "seq.h"
#include "fx.h"
#include "presets_bank.h"

#define FM4_VERSION "1.0"
#define FM4_BANK_PRESET 0
#define FM4_BANK_USER 1
#define FM4_PARTS MULTI_PARTS

/* FM-4 SysEx extension (non-commercial ID 7D, "F4"). Must match webapp/js/fm4ext.js. */
#define FM4X_ID0 0x7D
#define FM4X_ID1 0x46
#define FM4X_ID2 0x34
enum {
    FM4X_STORE = 0x01,      /* F0 7D 46 34 01 part slot F7: store part edit buffer to user slot */
    FM4X_READ = 0x02,       /* F0 7D 46 34 02 slot F7: load user slot into part 0, reply 191-byte dump */
    FM4X_SELECT = 0x03,     /* F0 7D 46 34 03 part bank program F7 */
    FM4X_INFO = 0x04,       /* F0 7D 46 34 04 F7 -> F0 7D 46 34 44 <ascii "FM-4 x.y"> F7 */
    FM4X_REPLY = 0x40       /* reply flag (cmd | 0x40) */
};

typedef struct {
    uint8_t version;
    uint8_t mode;            /* 0 single, 1 multi */
    uint8_t rx_channel;      /* basic receive channel 0..15, 16 omni */
    uint8_t tx_channel;      /* 0..15 */
    uint8_t sysex_rx;        /* accept SysEx */
    uint8_t protect;         /* user memory protect */
    uint8_t master_tune;     /* 0..127, 64 centre */
    uint8_t polyphony;       /* 4..16 */
    uint8_t local;           /* panel keys play the engine */
    uint8_t key_out;         /* panel keys sent to MIDI out */
    uint8_t velocity;        /* panel key velocity 1..127 */
    uint8_t clock_ext;       /* sequencer follows MIDI clock */
    uint8_t song;            /* current song 0..7 */
    uint8_t single_bank, single_program;
    uint8_t pad[17];
} fm4_system;

typedef struct {
    uint8_t bank[FM4_PARTS], program[FM4_PARTS];
    multi_part_cfg cfg[FM4_PARTS];
} fm4_multi_setup;

/* Hardware services the core needs (provided by fm4_main.c or tests). */
uint32_t fm4_hw_flash_size(void);
int fm4_hw_flash_read(uint32_t off, void *data, size_t n);
int fm4_hw_flash_erase(uint32_t off);
int fm4_hw_flash_write(uint32_t off, const void *data, size_t n);
int fm4_hw_midi_send(const uint8_t *msg, size_t n);   /* 0 queued, <0 retry later */
void fm4_hw_audio_gate(int closed);                    /* mute audio around Flash work */

/* Control task. */
void fm4_core_init(void);
void fm4_core_poll(void);                    /* deferred SysEx, replies, saving */
fm4_system *fm4_sys(void);
fm4_multi_setup *fm4_multi(void);
tq5_voice *fm4_edit(unsigned part);          /* edit buffer of a part */
void fm4_edit_commit(unsigned part);         /* after changing fm4_edit(): re-apply */
void fm4_select(unsigned part, unsigned bank, unsigned program);
int fm4_store(unsigned part, unsigned slot); /* edit buffer -> user slot (flash) */
void fm4_voice_get(unsigned bank, unsigned slot, tq5_voice *out);
unsigned fm4_active_part(void);              /* part the panel plays/edits */
void fm4_set_active_part(unsigned part);
void fm4_set_mode(unsigned multi);
void fm4_apply_multi(void);
void fm4_apply_system(void);                 /* master tune, polyphony */                  /* push fm4_multi() cfg to the engine */
int fm4_save_settings(void);
void fm4_mark_dirty(void);                   /* settings changed: saved ~2 s later */
void fm4_core_tick(uint32_t now_ms);         /* control task: debounced saves */
void fm4_midi_out(const uint8_t *msg, size_t n); /* control task */
/* Songs */
seq_state *fm4_seq(void);
int fm4_song_load(unsigned song);
int fm4_song_save(void);
void fm4_transport(unsigned action);         /* 0 stop, 1 play, 2 record active part's track */
void fm4_record(unsigned track);             /* record onto a song track */
/* Input from any context. */
int fm4_post(const uint8_t *msg, unsigned n);        /* MIDI routing task */
int fm4_post_local(const uint8_t *msg, unsigned n);  /* control task (panel) */
void fm4_sysex_in(const uint8_t *msg, size_t n);     /* routing task; copied, handled in poll */
/* Audio context. */
FM4_AUDIO_CODE void fm4_core_render(int32_t *out);   /* FM4_BLOCK stereo Q23 */
FM4_AUDIO_CODE void fm4_midi_clock_in(uint8_t status);
/* State for the UI. */
unsigned fm4_voices_active(void);
unsigned fm4_part_activity(unsigned part);  /* nonzero while notes sound */
const char *fm4_notice(void);
#endif
