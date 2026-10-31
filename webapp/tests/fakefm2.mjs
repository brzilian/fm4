// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// Modified by FM-4 contributors, 2026-10-31: extends ORBIT's FakeFM1 (orbit/web/test_installer.mjs)
// into FakeFM2: fm1_open capability (0x31) and arm (0x32) replies, the vendor loader identity
// ota-FM-1_015, re-enumeration with new port objects, the FM-4 SysEx extension (INFO, READ), the
// Felucca archive protocol (after Felucca web/test_backup.mjs), a minimal fm1_open project-library
// responder, the stock V15 factory-bank commands (0x21/0x22) and fault injection.

import { BACKUP_IDS, bkU32, bkR32, bkPack, bkCrc } from "../vendor/felucca/fm1backup.js";

const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const le = (p, at, n) => { let v = 0; for (let i = 0; i < n; i++) v += p[at + i] * 2 ** (8 * i); return v; };
const ascii27 = (s) => { const a = new Array(27).fill(0); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i); return a; };
const hexBytes = (h) => h.match(/../g).map((x) => parseInt(x, 16));

export class FakeFM2 {
  /**
   * opts: identity, name, family, state, build (64 hex), loader ('ota-FM-1_015'), finalIdentity,
   * finalBuild, capability (answers 0x31), fm4 ({codec, voices:[130-byte]} answers the F4 extension),
   * felucca ({objects: Map}), library (fm1_open project library), stock (answers 0x21/0x22),
   * unplugAt: {stage: 1|2, after: n reads}, access (shared with other fakes), loaderReads: number
   */
  constructor(FM1Web, image, opts = {}) {
    this.W = FM1Web; this.image = image; this.o = { loader: "ota-FM-1_015", family: 7, state: 0, capability: true, ...opts };
    this.access = opts.access || { inputs: new Map(), outputs: new Map(), sysexEnabled: true };
    this.sent = []; this.upgrades = 0; this.arms = []; this.served = 0; this.bad = 0; this.stageReads = 0;
    this.factory = new Uint8Array(16384); this.factoryWrites = 0; this.events = []; this.onStage2 = null;
    this.mine = [];
    this.boot(this.o.identity, this.o.name || "FM-1");
  }
  get build() { return this._build ?? this.o.build; }
  boot(identity, name) {
    this.identity = identity; this.queue = []; this.waiting = null; this.events.push(`boot ${identity}`);
    for (const p of this.mine) { p.state = "disconnected"; this.access.inputs.delete(p.id); this.access.outputs.delete(p.id); }
    const id = Math.random().toString(36).slice(2);
    const listeners = new Set();
    const input = { id: "i" + id, name, manufacturer: "fake", state: "connected", connection: "closed", _on: null,
      addEventListener: (t, f) => t === "midimessage" && listeners.add(f), removeEventListener: (t, f) => listeners.delete(f),
      set onmidimessage(f) { this._on = f; }, get onmidimessage() { return this._on; },
      open: async () => { input.connection = "open"; }, close: async () => { input.connection = "closed"; } };
    const output = { id: "o" + id, name, manufacturer: "fake", state: "connected", connection: "closed",
      open: async () => { output.connection = "open"; }, close: async () => { output.connection = "closed"; },
      send: (d) => {
        if (output.state !== "connected") throw new Error("InvalidStateError: port disconnected");
        const bytes = Array.from(d); this.sent.push(bytes);
        setTimeout(() => { if (output.state === "connected") this.rx(bytes); }, 1);
      } };
    this.input = input; this.output = output; this.listeners = listeners;
    this.access.inputs.set(input.id, input); this.access.outputs.set(output.id, output);
    this.mine = [input, output];
  }
  /** Unplug: every port disappears; nothing answers until replug(). */
  unplug() { this.events.push("unplug"); for (const p of this.mine) { p.state = "disconnected"; this.access.inputs.delete(p.id); this.access.outputs.delete(p.id); } this.mine = []; this.queue = []; this.waiting = null; }
  replug(identity = this.identity, name = "FM-1") { this.boot(identity, name); }
  tx(bytes) {
    const input = this.input;
    setTimeout(() => {
      if (input.state !== "connected") return;
      const ev = { data: Uint8Array.from(bytes) };
      for (const f of this.listeners) f(ev);
      input._on?.(ev);
    }, 1);
  }
  frame(cmd, payload) { this.tx(Array.from(this.W.frame(cmd, payload))); }

  get inLoader() { return /^ota-/.test(this.identity); }
  rx(d) {
    if (eq(d, UPGRADE)) return this.upgrade();
    if (d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x34) return this.ext(d);
    if (d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x4C) return this.felucca(d);
    if (d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x4D) return this.library(d);
    const p = this.W.parse(Uint8Array.from(d));
    if (!p) return;
    if (p.command === 17) return this.frame(17, ascii27(this.identity));
    // Only fm1_open-family firmware (FM-1_8NNNNNN) answers the capability query; stock V15,
    // the Felucca family and every loader stay silent.
    if (p.command === 49 && /^FM-1_8\d{6}$/.test(this.identity)) {
      const c = new Array(47).fill(0);
      c.splice(0, 4, 70, 77, 67, 49); c[4] = 1; c[5] = this.o.family; c[6] = this.o.state; c[7] = 1;
      c[8] = 0; c[9] = 2; c[10] = 0; c[11] = 0; c[12] = 16; c[13] = 0;
      hexBytes(this.build).forEach((b, i) => { c[14 + i] = b; });
      return this.frame(49, c);
    }
    if (p.command === 50 && !this.inLoader) {
      this.arms.push({ family: p.data[1], length: le(p.data, 2, 4), tag: Array.from(p.data.subarray(6, 38), (b) => b.toString(16).padStart(2, "0")).join("") });
      return this.frame(50, [this.o.rejectArm ? 1 : 0]);
    }
    if ((p.command === 33 || p.command === 34) && this.identity === "FM-1_015") {
      if (p.command === 34) { const off = p.data[1] | (p.data[2] << 8); this.factory.set(p.data.subarray(8, 8 + 512), off); this.factoryWrites++; }
      return this.frame(0, [0]);
    }
    if (p.command === 48 && this.waiting) return this.answer(p.data);
  }
  upgrade() {
    this.upgrades++; this.events.push(`upgrade ${this.identity}`);
    const n = this.image.length;
    if (this.inLoader) {
      const reads = [];
      const limit = this.o.loaderReads ? Math.min(n, this.o.loaderReads * 512) : n;
      for (let a = 0; a < limit; a += 512) reads.push([a, Math.min(512, n - a)]);
      reads.splice(3, 0, reads[1]);                       // an overlapping repeat read
      this.queue = [...reads, [0xF0000000, 8]];
      this.stage = 2;
    } else {
      this.queue = [[0, 64], [64, 160], [1024, 512], [0xE0000000, 8]];
      this.stage = 1;
    }
    this.stageReads = 0;
    setTimeout(() => this.next(), 2);
  }
  next() {
    const r = this.queue.shift();
    if (!r) return;
    const u = this.o.unplugAt;
    if (u && u.stage === this.stage && this.stageReads === u.after) { this.o.unplugAt = null; this.unplug(); return; }
    if (this.stage === 2 && this.stageReads === 0) this.onStage2?.();
    this.waiting = r;
    const [addr, len] = r;
    this.frame(48, [0, addr & 255, (addr >>> 8) & 255, (addr >>> 16) & 255, (addr >>> 24) & 255, len & 255, (len >> 8) & 255, 0]);
  }
  answer(data) {
    const [addr, len] = this.waiting;
    if (le(data, 1, 4) !== addr) return;
    const got = data.subarray(8);
    const want = addr >= 0xE0000000 ? [115, 117, 99, 99, 101, 115, 115, 0] : this.image.subarray(addr, addr + len);
    if (!eq(Array.from(got), Array.from(want))) this.bad++;
    this.waiting = null; this.served++; this.stageReads++;
    if (addr === 0xE0000000) {
      this.frame(48, [0, 0, 0, 0, 0xE0, 8, 0, 0]);         // a duplicate finish request
      setTimeout(() => this.boot(this.o.loader, "FM-1"), 30);
    } else if (addr === 0xF0000000) {
      setTimeout(() => { this._build = this.o.finalBuild ?? (this.arms.at(-1)?.tag || this.o.build); this.boot(this.o.finalIdentity, "FM-1"); }, 30);
    } else setTimeout(() => this.next(), 0);
  }

  /* ---- FM-4 extension (F0 7D 46 34 ...) ---- */
  ext(d) {
    const f = this.o.fm4; if (!f || this.inLoader) return;
    const cmd = d[4];
    if (cmd === 0x04 && d.length === 6) this.tx([0xF0, 0x7D, 0x46, 0x34, 0x44, ...new TextEncoder().encode("FM-4 1.0 TQ5"), 0xF7]);
    if (cmd === 0x02 && d.length === 7) {
      if (f.dropRead?.has(d[5])) { f.dropRead.delete(d[5]); return; }   // lost reply: host must retry
      const c = f.codec; c.setVoice(5, f.voices[d[5]]);
      const dump = c.dump(5, 4, 0);
      for (let i = 0, s = 0; i < dump.length; i++) if (dump[i] === 0xF7) { this.tx(Array.from(dump.subarray(s, i + 1))); s = i + 1; }
    }
  }

  /* ---- Felucca archive (F0 7D 46 4C cmd args F7), after Felucca web/test_backup.mjs ---- */
  felucca(d) {
    const f = this.o.felucca; if (!f) return;
    const cmd = d[4], a = d.slice(5, -1);
    const reply = (args) => this.tx([0xF0, 0x7D, 0x46, 0x4C, cmd, ...args, 0xF7]);
    if (cmd === 1) {
      const enc = (s) => [...new TextEncoder().encode(s)];
      return reply([...enc("FELUCCA 1.0.3"), 0, 1, 0, 0, 0, 0, ...enc("FM"), 0, 0, 16, 0x55, 1, 0, 0x4d, 1, 0, 0, 0x42, 1, f.noArchive ? 0 : 1]);
    }
    if (cmd === 65) {
      const out = [1, 0, BACKUP_IDS.length];
      for (const id of BACKUP_IDS) { const v = f.objects.get(id) || new Uint8Array(0); out.push(id, ...bkU32(v.length), ...bkU32(v.length ? bkCrc(v) : 0)); }
      return reply(out);
    }
    if (cmd === 66) {
      const id = a[0], off = bkR32(a, 1), n = a[6] | (a[7] << 7), v = f.objects.get(id);
      return reply([id, 0, ...bkU32(off), n & 127, n >> 7, ...bkPack(v.subarray(off, off + n))]);
    }
  }

  /* ---- fm1_open project library (F0 7D 46 4D 02 ...), minimal: one project, no banks, no samples ---- */
  library(d) {
    const L = this.o.library; if (!L) return;
    const p = this.W.demoParse(Uint8Array.from(d)); if (!p) return;
    const reply = (data) => this.tx(Array.from(this.W.demoFrame(p.command | 64, p.token, data)));
    switch (p.command) {
      case 0: return reply([0, 4, 16, 0, 4064 & 255, 4064 >> 8, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
      case 21: case 22: return reply([71]);
      case 8: return reply([L.slots.has(p.data[0]) ? 0 : 1]);
      case 1: return reply([0, 4064 & 255, 4064 >> 8]);
      case 2: { const at = p.data[0] | (p.data[1] << 8), n = p.data[2]; return reply([0, ...L.project.subarray(at, at + n)]); }
      case 6: return reply([0]);
      case 11: return reply([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
      default: return reply([71]);
    }
  }
}
