/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4: Yamaha TQ5 voice data and SysEx protocol (see docs/tq5_spec.md).
 *
 * A voice is kept in its native SysEx blocks (VCED 93, ACED 23, ACED2 10,
 * EFEDS 3 bytes) so dumps round-trip byte for byte. Operator order inside
 * every block is OP4, OP2, OP3, OP1. */
#ifndef FM4_TQ5_H
#define FM4_TQ5_H
#include "multi.h"
#include "fx.h"

#define TQ5_VCED 93
#define TQ5_ACED 23
#define TQ5_ACED2 10
#define TQ5_EFEDS 3
#define TQ5_VMEM 128
#define TQ5_BANK_VOICES 100
#define TQ5_FULL_VOICE_BYTES 191

typedef struct {
    uint8_t vced[TQ5_VCED];
    uint8_t aced[TQ5_ACED];
    uint8_t aced2[TQ5_ACED2];
    uint8_t efeds[TQ5_EFEDS];
    uint8_t opmask;            /* VCED pp 93: bit3 OP4 .. bit0 OP1 */
} tq5_voice;

/* VCED offsets used outside the codec */
enum {
    TQ5_ALG = 52, TQ5_FBL, TQ5_LFS, TQ5_LFD, TQ5_PMD, TQ5_AMD, TQ5_SYNC, TQ5_LFW,
    TQ5_PMS, TQ5_AMS, TQ5_TRPS, TQ5_POLY, TQ5_PBR, TQ5_PORTMODE, TQ5_PORTTIME,
    TQ5_FCVOL, TQ5_SUS, TQ5_PORTSW, TQ5_CHORUS, TQ5_MWPITCH, TQ5_MWAMP,
    TQ5_BCPITCH, TQ5_BCAMP, TQ5_BCPBIAS, TQ5_BCEBIAS, TQ5_NAME = 77
};
/* Per-operator VCED fields (offset inside a 13-byte operator block) */
enum { TQ5_OP_AR, TQ5_OP_D1R, TQ5_OP_D2R, TQ5_OP_RR, TQ5_OP_D1L, TQ5_OP_LS, TQ5_OP_RS,
       TQ5_OP_EBS, TQ5_OP_AME, TQ5_OP_KVS, TQ5_OP_OUT, TQ5_OP_CRS, TQ5_OP_DET };
/* Per-operator ACED fields (5-byte block) */
enum { TQ5_OP_FIX, TQ5_OP_FIXRG, TQ5_OP_FINE, TQ5_OP_OSW, TQ5_OP_SHFT };
/* Block index (0..3) of TX81Z operator n (1..4) inside VCED/ACED/VMEM. */
static inline unsigned tq5_op_block(unsigned op) { static const uint8_t b[5] = { 0, 3, 1, 2, 0 }; return op <= 4 ? b[op] : 0; }

/* Parameter ranges, for validation and the editor (index = VCED offset). */
extern const uint8_t tq5_vced_max[TQ5_VCED];
extern const uint8_t tq5_aced_max[TQ5_ACED];
extern const uint8_t tq5_aced2_max[TQ5_ACED2];

void tq5_voice_init(tq5_voice *v);                 /* TX81Z-style INIT VOICE */
void tq5_voice_name(const tq5_voice *v, char out[11]);
void tq5_voice_set_name(tq5_voice *v, const char *name);
void tq5_clamp(tq5_voice *v);                      /* force every field into range */
void tq5_to_engine(const tq5_voice *v, multi_voice_data *out);
void tq5_effect(const tq5_voice *v, fx_params *out); /* EFEDS -> effect unit */

/* VMEM 128-byte packed record (VCED + ACED + ACED2 + EFEDS). */
void tq5_vmem_pack(const tq5_voice *v, uint8_t out[TQ5_VMEM]);
void tq5_vmem_unpack(const uint8_t in[TQ5_VMEM], tq5_voice *v);

/* ---- SysEx ------------------------------------------------------------- */
uint8_t tq5_checksum(const uint8_t *data, size_t n);
/* Message builders; return the message length. ch = device number 0..15. */
size_t tq5_msg_vced(const tq5_voice *v, unsigned ch, uint8_t out[101]);
size_t tq5_msg_aced(const tq5_voice *v, unsigned ch, uint8_t out[41]);
size_t tq5_msg_aced2(const tq5_voice *v, unsigned ch, uint8_t out[28]);
size_t tq5_msg_efeds(const tq5_voice *v, unsigned ch, uint8_t out[21]);
/* VMEM bulk: 32 records already packed. out must hold 4104 bytes. */
size_t tq5_msg_vmem(const uint8_t records[32][TQ5_VMEM], unsigned ch, uint8_t *out);
size_t tq5_msg_param(unsigned ch, unsigned group, unsigned pp, unsigned value, uint8_t out[7]);

/* What a parsed message asks the host to do. */
enum tq5_action {
    TQ5_NONE,          /* not a TQ5 message (or wrong device number) */
    TQ5_ERROR,         /* malformed / bad checksum */
    TQ5_VOICE_EDIT,    /* edit buffer changed: field 'changed' says which blocks */
    TQ5_VMEM_BLOCK,    /* 32 records received for bank block 'block' (0..3) */
    TQ5_DUMP_REQUEST,  /* 'request' says what to send back */
    TQ5_REMOTE,        /* remote switch 'pp' pressed ('value' 0x7F) or released */
    TQ5_MULTI_PARAM,   /* multi part 'part', index 'pp', 'value' */
    TQ5_SYS_PARAM,     /* group 24 pp 0..6 */
    TQ5_BLOCK_SELECT,  /* 24/07: next VMEM goes to block 'block' */
    TQ5_MASTER_TUNE,   /* value 0..127 */
};
enum { TQ5_CHANGED_VCED = 1, TQ5_CHANGED_ACED = 2, TQ5_CHANGED_ACED2 = 4, TQ5_CHANGED_EFEDS = 8 };
enum tq5_request { TQ5_REQ_VCED, TQ5_REQ_VMEM, TQ5_REQ_ACED, TQ5_REQ_ACED2, TQ5_REQ_EFEDS, TQ5_REQ_EFEDS_ONLY };

typedef struct {
    enum tq5_action action;
    uint8_t changed, block, request, part, pp, value, param_group;
    const uint8_t *vmem;      /* TQ5_VMEM_BLOCK: points into the message */
} tq5_result;

/* Parse one complete SysEx message (F0..F7). Voice blocks and parameter
 * changes are applied to 'edit'. rx_channel: 0..15 or 16 = omni. */
tq5_result tq5_parse(const uint8_t *msg, size_t len, unsigned rx_channel, tq5_voice *edit);

/* Builds the reply to a dump request into out (up to 191 bytes for voice
 * requests). VMEM requests are answered by the host. Returns length. */
size_t tq5_reply(const tq5_voice *edit, unsigned request, unsigned ch, uint8_t *out, size_t cap);
#endif
