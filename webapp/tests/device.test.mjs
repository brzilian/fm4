// SPDX-License-Identifier: GPL-3.0-only
// FM-4 extension commands and one-voice-at-a-time bank transfers against a scripted device.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { codec as makeCodec, randomVoice, eq, WEBAPP } from "./helpers.mjs";
import { Fm4Device, Throttle, splitSysex, voiceRequest } from "../js/device.js";
import { storeMsg, readMsg, selectMsg, infoMsg, parseExtReply, FM4EXT_CMD, DEVICE_MAX_RX, DEVICE_MAX_TX, FM4_PRODUCT } from "../js/fm4ext.js";
import { ACTION } from "../js/codec.js";

const c = await makeCodec();

test("extension bytes match the firmware's FM4X_* constants", () => {
  assert.deepEqual(Array.from(storeMsg(2, 41)), [0xF0, 0x7D, 0x46, 0x34, 0x01, 2, 41, 0xF7]);
  assert.deepEqual(Array.from(readMsg(99)), [0xF0, 0x7D, 0x46, 0x34, 0x02, 99, 0xF7]);
  assert.deepEqual(Array.from(selectMsg(7, 1, 12)), [0xF0, 0x7D, 0x46, 0x34, 0x03, 7, 1, 12, 0xF7]);
  assert.deepEqual(Array.from(infoMsg()), [0xF0, 0x7D, 0x46, 0x34, 0x04, 0xF7]);
  assert.throws(() => readMsg(100)); assert.throws(() => storeMsg(8, 0)); assert.throws(() => selectMsg(0, 2, 0));
  const h = readFileSync(join(WEBAPP, "../firmware/app/fm4/fm4_core.h"), "utf8");
  for (const [k, v] of Object.entries(FM4EXT_CMD)) assert.match(h, new RegExp(`FM4X_${k} = 0x0${v}`), k);
  assert.match(readFileSync(join(WEBAPP, "../firmware/app/version.h"), "utf8"), new RegExp(`"${FM4_PRODUCT}"`));
  assert.deepEqual(parseExtReply(Uint8Array.of(0xF0, 0x7D, 0x46, 0x34, 0x41, 1, 5, 0xF7)), { cmd: 1, ok: false, status: 1, slot: 5 });
  assert.equal(parseExtReply(Uint8Array.of(0xF0, 0x7D, 0x46, 0x34, 0x44, ...new TextEncoder().encode("FM-4 1.0 TQ5"), 0xF7)).version, "1.0");
});

/** A scripted FM-4: user bank of 130-byte voices, edit buffers per channel, size limits enforced. */
function fakeFm4({ dropAck = new Set(), failStore = new Set() } = {}) {
  const user = Array.from({ length: 100 }, (_, i) => randomVoice(c, 5000 + i));
  const edit = new Map(), fns = new Set(), log = [];
  const tx = (m) => { assert.ok(m.length <= DEVICE_MAX_TX, `device sends ${m.length} > 128`); setTimeout(() => fns.forEach((f) => f(Uint8Array.from(m))), 1); };
  const link = {
    send(m) {
      assert.ok(m.length <= DEVICE_MAX_RX, `host sent ${m.length} > 1024 bytes`);
      log.push(Array.from(m));
      if (m[1] === 0x43) {
        const ch = m[2] & 15;
        if ((m[2] & 0x70) === 0x20) { c.setVoice(5, edit.get(ch) || c.resetVoice(5)); for (const x of splitSysex(c.dump(5, 4, ch))) tx(x); return true; }
        c.setVoice(5, edit.get(ch) || c.resetVoice(5));
        assert.equal(c.parse(5, Uint8Array.from(m), 16).action, ACTION.VOICE_EDIT);
        edit.set(ch, c.getVoice(5)); return true;
      }
      const cmd = m[4];
      if (cmd === 1) {
        const slot = m[6];
        if (dropAck.has(slot)) { dropAck.delete(slot); return true; }
        const ok = !failStore.has(slot);
        if (ok) user[slot] = edit.get(m[5]) || c.resetVoice(5);
        tx([0xF0, 0x7D, 0x46, 0x34, 0x41, ok ? 0 : 1, slot, 0xF7]);
      } else if (cmd === 2) { c.setVoice(5, user[m[5]]); for (const x of splitSysex(c.dump(5, 4, 0))) tx(x); }
      else if (cmd === 4) tx([0xF0, 0x7D, 0x46, 0x34, 0x44, ...new TextEncoder().encode("FM-4 1.0 TQ5"), 0xF7]);
      return true;
    },
    on(f) { fns.add(f); return () => fns.delete(f); },
  };
  return { link, user, edit, log };
}
const voiceOf = (bytes) => { c.resetVoice(4); for (const m of splitSysex(bytes)) c.parse(4, m, 16); return c.getVoice(4); };
const opts = { timeout: 100, pace: 0, sleep: async () => {} };
const same = (a, b) => eq(a.subarray(0, 129), b.subarray(0, 129));

test("INFO, get voice, send voice", async () => {
  const d = fakeFm4(), dev = new Fm4Device(d.link, opts);
  assert.equal((await dev.info()).text, "FM-4 1.0 TQ5");
  const v = randomVoice(c, 1); c.setVoice(3, v);
  await dev.sendVoice(c.dump(3, 4, 2));
  assert.deepEqual(d.log.slice(-4).map((m) => m.length), [21, 28, 41, 101], "EFEDS, ACED2, ACED, VCED");
  assert.ok(same(d.edit.get(2), v));
  assert.ok(same(voiceOf(await dev.getVoice(2)), v));
  assert.ok(eq(d.log.at(-1), Array.from(voiceRequest(2))));
});

test("write and read a 100-voice bank one voice at a time, with a lost ack retried", async () => {
  const d = fakeFm4({ dropAck: new Set([10]) }), dev = new Fm4Device(d.link, opts);
  const bank = Array.from({ length: 100 }, (_, i) => (i % 9 === 4 ? null : randomVoice(c, 7000 + i)));
  const dumps = bank.map((v) => (v ? (c.setVoice(3, v), c.dump(3, 4, 0)) : null));
  const keep = d.user.slice();
  const seen = [];
  const n = await dev.writeBank(dumps, { onProgress: (k, t, slot) => seen.push(slot) });
  assert.equal(n, bank.filter(Boolean).length);
  bank.forEach((v, i) => assert.ok(same(d.user[i], v || keep[i]), `slot ${i}`));
  assert.ok(d.log.every((m) => m.length <= 101), "no message over 101 bytes");
  const back = await dev.readBank();
  assert.equal(back.length, 100);
  back.forEach((b, i) => assert.ok(same(voiceOf(b), d.user[i]), `read ${i}`));
});

test("a refused store stops with a clear error; abort stops between voices", async () => {
  const d = fakeFm4({ failStore: new Set([3]) }), dev = new Fm4Device(d.link, opts);
  const dumps = Array.from({ length: 6 }, (_, i) => (c.setVoice(3, randomVoice(c, i)), c.dump(3, 4, 0)));
  await assert.rejects(dev.writeBank(dumps), /could not store user voice 4/);
  const ac = new AbortController();
  await assert.rejects(dev.writeBank(dumps.slice(0, 3), { signal: ac.signal, onProgress: () => ac.abort() }), /Stopped after 1 of 3/);
});

test("no reply: timeout after one retry", async () => {
  const dev = new Fm4Device({ send: () => true, on: () => () => {} }, opts);
  const t0 = Date.now();
  await assert.rejects(dev.readSlot(0), (e) => e.code === "timeout");
  assert.ok(Date.now() - t0 >= 190);
  assert.equal(await dev.info(), null);
});

test("throttle: a dragged slider sends at most ~60 messages per second, newest value wins", async () => {
  let now = 0; const timers = [], sent = [];
  const th = new Throttle((b) => sent.push([now, b]), { interval: 1000 / 60, now: () => now, schedule: (f, ms) => timers.push([now + ms, f]) });
  for (let t = 0; t < 1000; t += 2) {
    now = t;
    th.push(10, Uint8Array.of(t & 127));
    timers.sort((a, b) => a[0] - b[0]);
    while (timers.length && timers[0][0] <= now) timers.shift()[1]();
  }
  now = 2000; while (timers.length) timers.shift()[1]();
  assert.ok(sent.length <= 61 && sent.length >= 55, `${sent.length} messages`);
  for (let i = 1; i < sent.length; i++) assert.ok(sent[i][0] - sent[i - 1][0] >= 16, "spacing");
  assert.equal(sent.at(-1)[1][0], 998 & 127, "the last value is delivered");
});
