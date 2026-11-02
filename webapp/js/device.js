// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: talking to a running FM-4 over Web MIDI (editor and library traffic only; the
// firmware installer has its own port handling in installer.js / vendor WebUpdate.js).
//
// The device accepts SysEx frames up to 1024 bytes and sends messages up to 128 bytes, so voices
// travel as the 4 TQ5 messages (EFEDS 21, ACED2 28, ACED 41, VCED 101) and banks one voice at a
// time through the extension commands in fm4ext.js. One request is in flight at a time.

import { storeMsg, readMsg, infoMsg, prepareStockMsg, selectMsg, parseExtReply, FM4EXT_CMD, PACE, USER_SLOTS } from "./fm4ext.js";

const sleepReal = (ms) => new Promise((r) => setTimeout(r, ms));
const LM_EFEDS = [0x4C, 0x4D, 0x20, 0x20, 0x38, 0x30, 0x33, 0x36, 0x45, 0x46];

/** Which TQ5 voice block a message is, or null. */
export function voiceBlockKind(m) {
  if (m.length < 8 || m[0] !== 0xF0 || m[1] !== 0x43 || (m[2] & 0x70) !== 0) return null;
  if (m[3] === 0x03 && m.length === 101) return "VCED";
  if (m[3] !== 0x7E || m.length < 18) return null;
  const h = String.fromCharCode(...m.subarray(6, 16));
  return { "LM  8976AE": "ACED", "LM  8023AE": "ACED2", "LM  8036EF": "EFEDS" }[h] || null;
}

export const voiceRequest = (ch) => Uint8Array.of(0xF0, 0x43, 0x20 | (ch & 15), 0x7E, ...LM_EFEDS, 0xF7);

/** Minimal MIDI holder for the editor and library: chosen ports, listeners, suspend during install. */
export class Midi {
  constructor() { this.access = null; this.input = null; this.output = null; this.listeners = new Set(); this.suspended = false; this.onchange = null; this.sent = 0; }
  attach(access) {
    this.access = access;
    access.onstatechange = () => { this._revalidate(); this.onchange?.(); };
    this._revalidate();
  }
  ports(kind) { return this.access ? [...this.access[kind].values()].filter((p) => p.state !== "disconnected") : []; }
  _revalidate() {
    if (this.input && this.input.state === "disconnected") this.setInput(null);
    if (this.output && this.output.state === "disconnected") this.output = null;
  }
  setInput(id) {
    if (this.input) this.input.onmidimessage = null;
    this.input = id ? this.ports("inputs").find((p) => p.id === id) || null : null;
    if (this.input) this.input.onmidimessage = (e) => { if (!this.suspended) for (const fn of this.listeners) fn(e.data); };
  }
  setOutput(id) { this.output = id ? this.ports("outputs").find((p) => p.id === id) || null : null; }
  get ready() { return Boolean(this.output) && !this.suspended; }
  send(bytes) {
    if (!this.ready) return false;
    try { this.output.send(bytes); this.sent++; return true; } catch { return false; }
  }
  on(fn) { this.listeners.add(fn); return () => this.listeners.delete(fn); }
}

export class DeviceError extends Error {
  constructor(message, code) { super(message); this.code = code; }
}

export class Fm4Device {
  /** midi: {send(bytes) -> bool, on(fn) -> off}. opts: {timeout, retries, pace, sleep}. */
  constructor(midi, opts = {}) {
    this.midi = midi;
    this.timeout = opts.timeout ?? PACE.timeout;
    this.retries = opts.retries ?? PACE.retries;
    this.pace = opts.pace ?? PACE.message;
    this.sleep = opts.sleep || sleepReal;
    this.chain = Promise.resolve();
  }
  _exclusive(task) {
    const run = this.chain.then(task, task);
    this.chain = run.catch(() => {});
    return run;
  }
  _send(bytes) { if (!this.midi.send(bytes)) throw new DeviceError("No MIDI output is selected, or the port is busy.", "nooutput"); }
  /** Send msg and resolve with collect()'s result; collect(m) returns undefined to keep waiting. */
  _ask(msg, collect, timeout = this.timeout) {
    return new Promise((resolve, reject) => {
      let timer;
      const off = this.midi.on((m) => {
        let r; try { r = collect(m); } catch (e) { done(); reject(e); return; }
        if (r !== undefined) { done(); resolve(r); }
      });
      const done = () => { clearTimeout(timer); off(); };
      timer = setTimeout(() => { done(); reject(new DeviceError("The FM-4 did not reply.", "timeout")); }, timeout);
      try { this._send(msg); } catch (e) { done(); reject(e); }
    });
  }
  async _retry(fn) {
    let last;
    for (let i = 0; i <= this.retries; i++) {
      try { return await fn(); } catch (e) { last = e; if (e.code !== "timeout") throw e; }
    }
    throw last;
  }
  /** INFO: {text, version} or null when nothing answers. */
  info() {
    return this._exclusive(() => this._ask(infoMsg(), (m) => {
      const r = parseExtReply(m); return r && r.cmd === FM4EXT_CMD.INFO ? r : undefined;
    }).catch((e) => { if (e.code === "timeout") return null; throw e; }));
  }
  /**
   * PREPARE_STOCK (FM-4 1.1+): {ok, status, erased, remaining}, or null when nothing answers
   * (FM-4 1.0 has no such command). Erasing can take a few seconds, hence the longer wait.
   */
  prepareStock({ timeout = Math.max(this.timeout, 30000) } = {}) {
    return this._exclusive(() => this._ask(prepareStockMsg(), (m) => {
      const r = parseExtReply(m); return r && r.cmd === FM4EXT_CMD.PREPARE_STOCK ? r : undefined;
    }, timeout).catch((e) => { if (e.code === "timeout") return null; throw e; }));
  }
  /** Send the 4 messages of a 191-byte voice dump to an edit buffer, paced. */
  sendVoice(dump) { return this._exclusive(() => this._sendVoice(dump)); }
  async _sendVoice(dump) {
    const msgs = splitSysex(dump);
    if (msgs.length !== 4) throw new DeviceError("A voice dump must hold 4 messages.", "format");
    for (const m of msgs) { this._send(m); await this.sleep(this.pace); }
  }
  /** Collect EFEDS/ACED2/ACED/VCED replies until VCED; resolves with the concatenated bytes. */
  _voiceCollector() {
    const got = [];
    return (m) => {
      const k = voiceBlockKind(m);
      if (!k) return undefined;
      got.push(m);
      if (k !== "VCED") return undefined;
      const out = new Uint8Array(got.reduce((n, x) => n + x.length, 0));
      let at = 0; for (const x of got) { out.set(x, at); at += x.length; }
      return out;
    };
  }
  /** Current edit buffer of the part addressed by device number ch (TQ5 "LM  8036EF" request). */
  getVoice(ch) { return this._exclusive(() => this._retry(() => this._ask(voiceRequest(ch), this._voiceCollector()))); }
  /** User slot -> 191-byte dump (extension READ). */
  readSlot(slot) { return this._exclusive(() => this._retry(() => this._ask(readMsg(slot), this._voiceCollector()))); }
  /** Store part's edit buffer to a user slot; waits for the ack. */
  store(part, slot) { return this._exclusive(() => this._retry(() => this._store(part, slot))); }
  async _store(part, slot) {
    const r = await this._ask(storeMsg(part, slot), (m) => {
      const a = parseExtReply(m); return a && a.cmd === FM4EXT_CMD.STORE && a.slot === slot ? a : undefined;
    });
    if (!r.ok) throw new DeviceError(`The FM-4 could not store user voice ${slot + 1} (status ${r.status}). Check that memory protect is off.`, "store");
    return r;
  }
  /** Send a voice into part's edit buffer and store it in a slot, as one exclusive step. */
  writeSlot(part, slot, dump) {
    return this._exclusive(() => this._retry(async () => { await this._sendVoice(dump); return this._store(part, slot); }));
  }
  select(part, bank, program) { return this._exclusive(async () => { this._send(selectMsg(part, bank, program)); await this.sleep(this.pace); }); }

  /** Write up to 100 dumps (index = slot, null = skip) through part's edit buffer. */
  async writeBank(dumps, { part = 0, onProgress = () => {}, signal } = {}) {
    const slots = dumps.map((d, i) => (d ? i : -1)).filter((i) => i >= 0);
    let done = 0;
    for (const slot of slots) {
      if (signal?.aborted) throw new DeviceError(`Stopped after ${done} of ${slots.length} voices. The other user voices are unchanged.`, "aborted");
      await this.writeSlot(part, slot, dumps[slot]);
      onProgress(++done, slots.length, slot);
    }
    return done;
  }
  /** Read user slots 0..count-1 as dumps. */
  async readBank({ count = USER_SLOTS, onProgress = () => {}, signal } = {}) {
    const out = [];
    for (let slot = 0; slot < count; slot++) {
      if (signal?.aborted) throw new DeviceError(`Stopped after reading ${slot} voices.`, "aborted");
      out.push(await this.readSlot(slot));
      onProgress(slot + 1, count, slot);
    }
    return out;
  }
}

export function splitSysex(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== 0xF0) continue;
    const end = bytes.indexOf(0xF7, i + 1);
    if (end < 0) break;
    out.push(bytes.slice(i, end + 1)); i = end;
  }
  return out;
}

/** Coalescing sender: at most one message per interval; repeated keys keep only the newest value. */
export class Throttle {
  constructor(send, { interval = 1000 / 60, now = () => performance.now(), schedule = (fn, ms) => setTimeout(fn, ms) } = {}) {
    this.send = send; this.interval = interval; this.now = now; this.schedule = schedule;
    this.pending = new Map(); this.last = -Infinity; this.timer = null;
  }
  push(key, bytes) {
    this.pending.delete(key); this.pending.set(key, bytes);
    this._kick();
  }
  _kick() {
    if (this.timer || !this.pending.size) return;
    const wait = Math.max(0, this.last + this.interval - this.now());
    if (wait === 0) this._flushOne();
    else this.timer = this.schedule(() => { this.timer = null; this._flushOne(); }, wait);
  }
  _flushOne() {
    const [key, bytes] = this.pending.entries().next().value;
    this.pending.delete(key);
    this.last = this.now();
    this.send(bytes);
    if (this.pending.size) this.timer = this.schedule(() => { this.timer = null; this._flushOne(); }, this.interval);
  }
  clear() { this.pending.clear(); }
}
