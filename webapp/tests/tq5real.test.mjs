// SPDX-License-Identifier: GPL-3.0-only
// Regression: voices from real TQ5 banks were silent. Real TQ5/TX81Z voices usually have VCED 67
// FC VOL = 99 or 40, and the engine took "no CC4 yet" as foot pedal 0, muting them. This test loads
// TQ5-shaped records through the librarian path (parseSyx -> record -> audition / 191-byte dump ->
// edit buffer) and checks that fm4.wasm actually renders sound. tests/data/DSBANK_A.SYX (a real
// TQ5 bank, user-supplied and not redistributed) is also checked when present.
import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { codec, WEBAPP } from "./helpers.mjs";
import { parseSyx, bank100Syx } from "../js/syx.js";
import { splitMessages, voiceName, REQUEST, ACTION } from "../js/codec.js";

const c = await codec();
const REAL = join(WEBAPP, "..", "tests", "data", "DSBANK_A.SYX");

/** A VMEM record laid out the way a TQ5 writes one (our own values). */
function tq5LikeRecord(name, fcVol, alg = 7) {
  const r = new Uint8Array(128);
  for (let k = 0; k < 4; k++) {
    r.set([31, 6, 2, 6, 13, 12, 0x02, k === 3 ? 92 : alg === 7 ? 0 : 60, 4, (1 << 3) | 3], 10 * k);
  }
  r[40] = alg; r[41] = 30; r[45] = 0x22; r[46] = 24; r[47] = 2; r[48] = 0x04;
  r[50] = fcVol; r[51] = 50; r[55] = 50;
  for (let i = 0; i < 10; i++) r[57 + i] = (name.padEnd(10, " ")).charCodeAt(i);
  r.fill(0x32, 67, 73);                       // PEG "222222"
  r[90] = 3; r[91] = 1; r[95] = 0x32; r[96] = 0x64;
  return r;
}

/** Play note 60 on part 0 for ~0.3 s and return the RMS of the left channel. */
function play() {
  c.x.fm4_all_off(); c.x.fm4_render();
  c.x.fm4_midi(0x90, 60, 100);
  let e = 0, n = 0;
  for (let b = 0; b < 100; b++) {
    c.x.fm4_render();
    const out = new Float32Array(c.x.memory.buffer, c.x.fm4_out(), 256);
    for (let i = 0; i < 256; i += 2) { e += out[i] * out[i]; n++; }
  }
  c.x.fm4_midi(0x80, 60, 0); c.x.fm4_all_off();
  return Math.sqrt(e / n);
}

/** Librarian "write to device": record -> 191-byte dump -> a fresh edit buffer (part 0). */
function viaDump(record) {
  c.vmemUnpack(7, record);
  const dump = c.dump(7, REQUEST.EFEDS, 0);
  c.resetVoice(0);
  for (const m of splitMessages(dump)) assert.equal(c.parse(0, m, 16).action, ACTION.VOICE_EDIT);
  return c.getVoice(0);
}

test("TQ5-shaped voices with FC VOL 99/40 sound after loading a 100-voice bank file", () => {
  const recs = [tq5LikeRecord("FootVol99", 99), tq5LikeRecord("FootVol40", 40, 0)];
  const file = bank100Syx(recs, c, 0);
  const p = parseSyx(file, c);
  assert.equal(p.format, "tq5-100");
  for (const [i, want] of [[0, 99], [1, 40]]) {
    const v = p.voices[i];
    assert.equal(v.record[50], want);
    c.vmemUnpack(0, v.record);                // audition path
    assert.ok(play() > 0.005, `${v.name} audition is silent`);
    const voice = viaDump(v.record);          // device write path (edit buffer)
    assert.equal(voice[67], want, "FC VOL survives the dump");
    assert.ok(play() > 0.005, `${voiceName(voice)} is silent after the dump`);
  }
});

test("real TQ5 bank: every non-INIT voice is audible", { skip: !existsSync(REAL) && "tests/data/DSBANK_A.SYX not present" }, () => {
  const p = parseSyx(new Uint8Array(readFileSync(REAL)), c);
  assert.equal(p.format, "tq5-100");
  const silent = [];
  let count = 0;
  for (const v of p.voices) {
    if (v.name === "INIT VOICE") continue;
    count++;
    c.vmemUnpack(0, v.record);
    if (!(play() > 1e-3)) silent.push(v.name);
  }
  assert.ok(count > 0);
  assert.deepEqual(silent, [], `${silent.length} of ${count} voices silent`);
});
