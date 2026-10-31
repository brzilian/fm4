/* Host tests for the TQ5 voice codec and SysEx protocol. */
#include "tq5.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void random_voice(tq5_voice *v, unsigned seed)
{
    srand(seed);
    for (unsigned i = 0; i < TQ5_VCED; ++i) v->vced[i] = (uint8_t)(rand() % (tq5_vced_max[i] + 1));
    for (unsigned i = 0; i < TQ5_ACED; ++i) v->aced[i] = (uint8_t)(rand() % (tq5_aced_max[i] + 1));
    for (unsigned i = 0; i < 4; ++i) v->aced2[i] = (uint8_t)(rand() % 100);
    for (unsigned i = 4; i < TQ5_ACED2; ++i) v->aced2[i] = 0;
    v->efeds[0] = (uint8_t)(rand() % 11); v->efeds[1] = (uint8_t)(rand() % 41); v->efeds[2] = (uint8_t)(rand() % 100);
    for (unsigned i = 0; i < 10; ++i) v->vced[TQ5_NAME + i] = (uint8_t)(32 + rand() % 95);
    v->opmask = 15;
}

int main(void)
{
    tq5_voice a, b; uint8_t msg[4200], rec[TQ5_VMEM];

    /* VMEM round trip over many random voices. */
    for (unsigned s = 1; s <= 500; ++s) {
        random_voice(&a, s);
        tq5_vmem_pack(&a, rec);
        for (unsigned i = 0; i < TQ5_VMEM; ++i) CHECK(rec[i] < 128, "vmem byte %u >= 128", i);
        tq5_vmem_unpack(rec, &b);
        if (memcmp(&a, &b, sizeof a)) { CHECK(0, "vmem round trip seed %u", s); break; }
    }

    /* Message sizes from the spec. */
    tq5_voice_init(&a);
    CHECK(tq5_msg_vced(&a, 0, msg) == 101, "vced size");
    CHECK(tq5_msg_aced(&a, 0, msg) == 41, "aced size");
    CHECK(tq5_msg_aced2(&a, 0, msg) == 28, "aced2 size");
    CHECK(tq5_msg_efeds(&a, 0, msg) == 21, "efeds size");
    CHECK(msg[4] == 0 && msg[5] == 0x0D && msg[6] == 'L', "efeds count+header");
    CHECK(tq5_reply(&a, TQ5_REQ_EFEDS, 0, msg, sizeof msg) == 191, "full voice dump 191");

    /* Golden message: INIT ACED, checksum covers "LM  8976AE" + data. */
    size_t n = tq5_msg_aced(&a, 2, msg);
    unsigned sum = 0; for (unsigned i = 6; i < n - 1; ++i) sum += msg[i];
    CHECK((sum & 0x7f) == 0, "aced checksum closes (%u)", sum & 0x7f);
    CHECK(msg[2] == 2, "device number");

    /* Parse every block back into a fresh edit buffer. */
    random_voice(&a, 99);
    tq5_voice_init(&b);
    n = tq5_reply(&a, TQ5_REQ_EFEDS, 0, msg, sizeof msg);
    size_t off = 0; int blocks = 0;
    while (off < n) {
        size_t end = off; while (msg[end] != 0xF7) ++end;
        tq5_result r = tq5_parse(msg + off, end - off + 1, 0, &b);
        CHECK(r.action == TQ5_VOICE_EDIT, "block %d action %d", blocks, r.action);
        off = end + 1; ++blocks;
    }
    CHECK(blocks == 4, "4 blocks");
    CHECK(!memcmp(a.vced, b.vced, TQ5_VCED) && !memcmp(a.aced, b.aced, TQ5_ACED) &&
          !memcmp(a.aced2, b.aced2, TQ5_ACED2) && !memcmp(a.efeds, b.efeds, TQ5_EFEDS), "dump round trip");

    /* Bad checksum and wrong channel. */
    n = tq5_msg_vced(&a, 0, msg); msg[n - 2] ^= 1;
    CHECK(tq5_parse(msg, n, 0, &b).action == TQ5_ERROR, "bad checksum rejected");
    n = tq5_msg_vced(&a, 5, msg);
    CHECK(tq5_parse(msg, n, 0, &b).action == TQ5_NONE, "other device ignored");
    CHECK(tq5_parse(msg, n, 16, &b).action == TQ5_VOICE_EDIT, "omni accepts");

    /* Parameter changes. */
    uint8_t pc[7];
    tq5_msg_param(0, 0x12, 52, 6, pc);
    tq5_result r = tq5_parse(pc, 7, 0, &b);
    CHECK(r.action == TQ5_VOICE_EDIT && b.vced[52] == 6, "ALG via param change");
    tq5_msg_param(0, 0x12, 52, 100, pc); tq5_parse(pc, 7, 0, &b);
    CHECK(b.vced[52] == 7, "clamped ALG %u", b.vced[52]);
    tq5_msg_param(0, 0x13, 3 + 5 * 3, 5, pc); tq5_parse(pc, 7, 0, &b);
    CHECK(b.aced[18] == 5, "OP1 OSW");
    tq5_msg_param(0, 0x13, 24, 42, pc); tq5_parse(pc, 7, 0, &b);
    CHECK(b.aced2[1] == 42, "ACED2 AT AMP");
    tq5_msg_param(0, 0x24, 4, 9, pc); r = tq5_parse(pc, 7, 0, &b);
    CHECK(r.changed == TQ5_CHANGED_EFEDS && b.efeds[0] == 9, "EFEDS preset");
    tq5_msg_param(0, 0x24, 7, 3, pc); r = tq5_parse(pc, 7, 0, &b);
    CHECK(r.action == TQ5_BLOCK_SELECT && r.block == 2, "block select");
    tq5_msg_param(0, 0x24, 117, 0x7f, pc); r = tq5_parse(pc, 7, 0, &b);
    CHECK(r.action == TQ5_REMOTE && r.pp == 117, "remote user bank");
    tq5_msg_param(0, 0x10, 12 * 3 + 3, 5, pc); r = tq5_parse(pc, 7, 0, &b);
    CHECK(r.action == TQ5_MULTI_PARAM && r.part == 3 && r.pp == 3 && r.value == 5, "multi param");
    tq5_msg_param(0, 0x12, 93, 0x5, pc); tq5_parse(pc, 7, 0, &b);
    CHECK(b.opmask == 5, "op on/off");

    /* Dump requests. */
    static const uint8_t req_full[15] = { 0xF0,0x43,0x20,0x7E,'L','M',' ',' ','8','0','3','6','E','F',0xF7 };
    r = tq5_parse(req_full, 15, 0, &b);
    CHECK(r.action == TQ5_DUMP_REQUEST && r.request == TQ5_REQ_EFEDS, "8036EF request");
    static const uint8_t req_vced[5] = { 0xF0,0x43,0x20,0x03,0xF7 };
    CHECK(tq5_parse(req_vced, 5, 0, &b).request == TQ5_REQ_VCED, "VCED request");

    /* VMEM bulk build and parse. */
    static uint8_t recs[32][TQ5_VMEM];
    for (unsigned i = 0; i < 32; ++i) { random_voice(&a, 1000 + i); tq5_vmem_pack(&a, recs[i]); }
    n = tq5_msg_vmem(recs, 0, msg);
    CHECK(n == 4104 && msg[4] == 0x20 && msg[5] == 0, "vmem size %zu", n);
    r = tq5_parse(msg, n, 0, &b);
    CHECK(r.action == TQ5_VMEM_BLOCK && r.vmem && !memcmp(r.vmem, recs, sizeof recs), "vmem parse");

    /* Engine conversion: operator order and fields. */
    tq5_voice_init(&a);
    a.vced[13 * 3 + TQ5_OP_OUT] = 77;           /* block 3 = OP1 */
    a.aced[5 * 0 + TQ5_OP_OSW] = 6;             /* block 0 = OP4 */
    multi_voice_data md; tq5_to_engine(&a, &md);
    CHECK(md.patch.op[0].out == 77, "OP1 out mapped");
    CHECK(md.patch.op[3].wave == 6, "OP4 wave mapped");
    CHECK(!strcmp(md.name, "INIT VOICE"), "name '%s'", md.name);
    fx_params fp; a.efeds[0] = 6; a.efeds[1] = 75; tq5_effect(&a, &fp);
    CHECK(fp.algo == FX_ALGO_PINGPONG && fp.time == 99, "stereo echo");

    printf(failures ? "%d FAILURES\n" : "all tq5 tests passed\n", failures);
    return failures ? 1 : 0;
}
