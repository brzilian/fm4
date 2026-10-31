// SPDX-License-Identifier: GPL-3.0-only
// (b) .syx parsing: VMEM banks, TQ5 100-voice banks, single voices, mixed files, bad checksums.
import test from "node:test";
import assert from "node:assert/strict";
import { codec, randomVoice, read, eq } from "./helpers.mjs";
import { parseSyx, bank100Syx, vmem32Syx, splitSysex, checksum, recordName, BANK100_LEN, VMEM_LEN } from "../js/syx.js";
import { REQUEST } from "../js/codec.js";

const c = await codec();
const records = (n, seed) => Array.from({ length: n }, (_, i) => c.voiceToRecord(randomVoice(c, seed + i)));
const cat = (...parts) => { const o = new Uint8Array(parts.reduce((a, p) => a + p.length, 0)); let at = 0; for (const p of parts) { o.set(p, at); at += p.length; } return o; };

test("32-voice VMEM bank (TX81Z / DX11 / DX21 / DX27 / DX100 / TQ5)", () => {
  const recs = records(32, 10);
  const file = vmem32Syx(recs, c, 3);
  assert.equal(file.length, VMEM_LEN);
  assert.deepEqual(Array.from(file.subarray(0, 6)), [0xF0, 0x43, 0x03, 0x04, 0x20, 0x00]);
  assert.equal(checksum(file.subarray(6, 6 + 4096)), file[4102]);
  const p = parseSyx(file, c);
  assert.equal(p.format, "vmem-32"); assert.equal(p.voices.length, 32); assert.deepEqual(p.warnings, []);
  p.voices.forEach((v, i) => assert.ok(eq(v.record, recs[i])));
});

test("TQ5 100-voice bank we generate: layout, padding, round trip", () => {
  const recs = records(100, 200);
  const file = bank100Syx(recs, c, 0);
  assert.equal(file.length, BANK100_LEN);
  const msgs = splitSysex(file).messages;
  assert.equal(msgs.length, 8);
  for (let b = 0; b < 4; b++) {
    assert.deepEqual(Array.from(msgs[2 * b]), [0xF0, 0x43, 0x10, 0x24, 0x07, b + 1, 0xF7]);
    const data = msgs[2 * b + 1].subarray(6, 6 + 4096);
    for (let s = 25; s < 32; s++) assert.ok(eq(data.subarray(s * 128, s * 128 + 128), data.subarray(0, 128)), "padding repeats slot 0");
  }
  const p = parseSyx(file, c);
  assert.equal(p.format, "tq5-100"); assert.equal(p.voices.length, 100); assert.deepEqual(p.warnings, []);
  p.voices.forEach((v, i) => { assert.equal(v.slot, i); assert.ok(eq(v.record, recs[i]), `voice ${i}`); });
  assert.ok(eq(bank100Syx(p.voices.map((v) => v.record), c, 0), file), "export of the parsed bank is byte-identical");
});

test("the shipped FM-4 preset bank", () => {
  const file = read("presets/FM-4_PRESETS.syx");
  const p = parseSyx(file, c);
  assert.equal(p.format, "tq5-100"); assert.equal(p.voices.length, 100); assert.deepEqual(p.warnings, []);
  assert.equal(p.voices[0].name.trim(), "Glass Tine");
  assert.ok(eq(bank100Syx(p.voices.map((v) => v.record), c, 0), file), "re-export is byte-identical");
});

test("single voices: VCED alone, ACED+VCED, the full 191-byte set, several in one file", () => {
  const v1 = randomVoice(c, 300), v2 = randomVoice(c, 301), v3 = randomVoice(c, 302);
  c.setVoice(1, v1); const full = c.dump(1, REQUEST.EFEDS, 0);
  c.setVoice(1, v2); const vcedOnly = c.dump(1, REQUEST.VCED, 0), acedVced = c.dump(1, REQUEST.ACED, 0);
  c.setVoice(1, v3); const second = c.dump(1, REQUEST.EFEDS, 2);
  assert.equal(full.length, 191); assert.equal(vcedOnly.length, 101); assert.equal(acedVced.length, 142);
  let p = parseSyx(vcedOnly, c);
  assert.equal(p.format, "single"); assert.equal(p.voices.length, 1);
  assert.equal(recordName(p.voices[0].record), recordName(c.voiceToRecord(v2)));
  p = parseSyx(cat(full, acedVced, second), c);
  assert.equal(p.voices.length, 3); assert.deepEqual(p.warnings, []);
  assert.ok(eq(p.voices[0].record, c.voiceToRecord(v1)));
  assert.ok(eq(p.voices[2].record, c.voiceToRecord(v3)));
  // ACED + VCED: the operator ACED bytes travel, EFEDS falls back to INIT defaults
  const r = p.voices[1].record, want = c.voiceToRecord(v2);
  assert.ok(eq(r.subarray(0, 84), want.subarray(0, 84)));
});

test("bad checksums are loaded with a warning", () => {
  const file = vmem32Syx(records(32, 400), c, 0);
  file[4102] ^= 0x11;
  c.setVoice(1, randomVoice(c, 401));
  const v = c.dump(1, REQUEST.VCED, 0); v[99] ^= 0x22;
  const p = parseSyx(cat(file, v), c);
  assert.equal(p.voices.length, 33);
  assert.ok(p.warnings.some((w) => /2 messages have a wrong checksum/.test(w)), p.warnings.join("|"));
  assert.equal(recordName(p.voices[32].record), recordName(c.voiceToRecord(c.getVoice(1))));
});

test("other messages, stray bytes and truncated data are skipped with warnings", () => {
  const dx7 = Uint8Array.of(0xF0, 0x43, 0x00, 0x09, 0x20, 0x00, 0xF7);
  const pcm = Uint8Array.of(0xF0, 0x43, 0x00, 0x7E, 0x00, 0x0B, ...new TextEncoder().encode("LM  8976PE"), 0, 0, 0xF7);
  const bank = vmem32Syx(records(32, 500), c, 0);
  const p = parseSyx(cat(Uint8Array.of(1, 2, 3), dx7, pcm, bank, Uint8Array.of(0xF0, 0x43, 0x00, 0x03)), c);
  assert.equal(p.voices.length, 32);
  assert.ok(p.warnings.some((w) => /bytes outside SysEx/.test(w)));
  assert.ok(p.warnings.some((w) => /LM  8976PE/.test(w)));
  assert.ok(p.warnings.some((w) => /Unterminated/.test(w)));
});

test("a 100-voice bank with a missing block and a lone VMEM after it", () => {
  const recs = records(100, 600);
  const msgs = splitSysex(bank100Syx(recs, c, 0)).messages;
  const extra = vmem32Syx(records(32, 700), c, 0);
  const p = parseSyx(cat(...msgs.slice(0, 4), ...msgs.slice(6), extra), c);
  assert.equal(p.format, "mixed");
  assert.equal(p.voices.filter((v) => v.slot !== undefined).length, 75);
  assert.ok(!p.voices.some((v) => v.slot >= 50 && v.slot < 75));
  assert.equal(p.voices.length, 75 + 32);
});

test("an empty or foreign file reports no voices", () => {
  const p = parseSyx(Uint8Array.of(0xF0, 0x41, 0x10, 0x42, 0xF7), c);
  assert.equal(p.voices.length, 0);
  assert.ok(p.warnings.some((w) => /No voices/.test(w)));
});
