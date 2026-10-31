// SPDX-License-Identifier: GPL-3.0-only
// (a) fm4.wasm round trips: VMEM pack/unpack and the full 191-byte dump for random voices.
import test from "node:test";
import assert from "node:assert/strict";
import { codec, randomVoice, eq } from "./helpers.mjs";
import { VOICE, ACTION, REQUEST, splitMessages, voiceName } from "../js/codec.js";
import { paramFor, opOffset, OPF, V, randomVoice as musical, mulberry32, maxFor } from "../js/model.js";

const c = await codec();

test("exports and layout", () => {
  assert.equal(c.x.fm4_voice_size(), VOICE.SIZE);
  assert.equal(c.ratio(4, 0), 1);
  assert.equal(c.ratio(0, 0), 0.5);
  assert.equal(c.fixedHz(4, 0, 0), 16);
  const init = c.resetVoice(0);
  assert.equal(voiceName(init), "INIT VOICE");
  assert.equal(init[VOICE.OPMASK], 15);
});

test("VMEM pack/unpack round trip, 500 random voices", () => {
  for (let seed = 1; seed <= 500; seed++) {
    const v = randomVoice(c, seed);
    const rec = c.voiceToRecord(v);
    assert.equal(rec.length, 128);
    assert.ok(rec.every((b) => b < 128), "VMEM bytes are 7-bit");
    const back = c.recordToVoice(rec);
    // VMEM has no op on/off, and ACED2 bytes 4-9 are V50-only (not stored in VMEM)
    const mask = (x) => { const y = x.slice(); y[VOICE.OPMASK] = 15; y.fill(0, VOICE.ACED2 + 4, VOICE.EFEDS); return y; };
    assert.ok(eq(mask(back), mask(v)), `seed ${seed}`);
    assert.ok(eq(c.voiceToRecord(back), rec), `record stable, seed ${seed}`);
  }
});

test("191-byte dump parses back into an identical voice, 300 random voices", () => {
  for (let seed = 1000; seed < 1300; seed++) {
    const v = randomVoice(c, seed), ch = seed % 16;
    c.setVoice(1, v);
    const dump = c.dump(1, REQUEST.EFEDS, ch);
    assert.equal(dump.length, 191);
    const msgs = splitMessages(dump);
    assert.deepEqual(msgs.map((m) => m.length), [21, 28, 41, 101]);
    for (const m of msgs) assert.equal(m[2], ch);
    c.resetVoice(2);
    for (const m of msgs) assert.equal(c.parse(2, m, 16).action, ACTION.VOICE_EDIT);
    const back = c.getVoice(2);
    assert.ok(eq(back.subarray(0, VOICE.OPMASK), v.subarray(0, VOICE.OPMASK)), `seed ${seed}`);
  }
});

test("the device-number filter: a dump on channel 5 is ignored on channel 6", () => {
  c.setVoice(1, randomVoice(c, 7));
  const vced = splitMessages(c.dump(1, REQUEST.VCED, 5))[0];
  assert.equal(c.parse(2, vced, 6).action, ACTION.NONE);
  assert.equal(c.parse(2, vced, 5).action, ACTION.VOICE_EDIT);
});

test("a bad checksum is reported as an error", () => {
  c.setVoice(1, randomVoice(c, 8));
  const vced = splitMessages(c.dump(1, REQUEST.VCED, 0))[0];
  vced[99] ^= 1;
  assert.equal(c.parse(2, vced, 16).action, ACTION.ERROR);
});

test("parameter change messages for every voice byte are applied by the engine", () => {
  const v = randomVoice(c, 42);
  c.resetVoice(3);
  for (let i = 0; i <= VOICE.OPMASK; i++) {
    if (i >= VOICE.ACED2 + 4 && i < VOICE.EFEDS) continue;      // V50 extras: not editable
    const [g, pp] = paramFor(i), m = c.paramMsg(0, g, pp, v[i]);
    assert.deepEqual(Array.from(m.subarray(0, 3)), [0xF0, 0x43, 0x10]);
    assert.equal(m.length, 7);
    assert.equal(c.parse(3, m, 0).action, ACTION.VOICE_EDIT, `byte ${i}`);
  }
  const back = c.getVoice(3);
  for (let i = 0; i <= VOICE.OPMASK; i++) if (!(i >= VOICE.ACED2 + 4 && i < VOICE.EFEDS)) assert.equal(back[i], v[i], `byte ${i}`);
});

test("operator order is OP4, OP2, OP3, OP1", () => {
  assert.equal(opOffset(4, OPF.AR), 0);
  assert.equal(opOffset(2, OPF.AR), 13);
  assert.equal(opOffset(3, OPF.AR), 26);
  assert.equal(opOffset(1, OPF.OUT), 39 + 10);
});

test("musical randomize stays in range and keeps carriers audible", () => {
  const r = mulberry32(9), init = c.resetVoice(4);
  for (let k = 0; k < 200; k++) {
    const v = musical(init, r), clamped = c.setVoice(4, v);
    for (let i = 0; i < VOICE.SIZE; i++) assert.ok(v[i] <= maxFor(c, i, v), `byte ${i}=${v[i]}`);
    assert.ok(eq(clamped.subarray(0, 129), v.subarray(0, 129)), "engine did not need to clamp");
    assert.ok(v[V.ALG] <= 7);
  }
});

test("the engine renders sound for a note", () => {
  c.resetVoice(0);
  c.x.fm4_midi(0x90, 60, 100);
  let peak = 0;
  for (let b = 0; b < 40; b++) { c.x.fm4_render(); for (const s of new Float32Array(c.x.memory.buffer, c.x.fm4_out(), 256)) peak = Math.max(peak, Math.abs(s)); }
  c.x.fm4_midi(0x80, 60, 0);
  assert.ok(peak > 0.005, `peak ${peak}`);
});
