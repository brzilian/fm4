// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: the EDITOR tab. The edit buffers of the 8 parts live in the main-thread codec
// (fm4.wasm); every edit is clamped by the engine, mirrored to the audition worklet and sent to the
// FM-4 as a TQ5 parameter change (F0 43 1n gg pp dd F7) on device number n = the part's channel.

import { VOICE, voiceName, setVoiceName, ACTION } from "./codec.js";
import { OPF, ACF, V, A, A2, FX, opOffset, acOffset, maxFor, paramFor, EFFECTS, LFO_WAVES, EG_SHIFT, FIX_RANGES,
  ALGORITHMS, CARRIERS, waveSample, envelopePoints, randomVoice, History } from "./model.js";
import { Throttle, splitSysex } from "./device.js";

const NS = "http://www.w3.org/2000/svg";
const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const noteName = (n) => `${NOTE[n % 12]}${Math.floor(n / 12) - 2}`;    // Yamaha: middle C (60) = C3
const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v; else if (k === "text") e.textContent = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v); else e.setAttribute(k, v);
  }
  e.append(...kids); return e;
};
const svg = (tag, attrs = {}) => { const e = document.createElementNS(NS, tag); for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v); return e; };
let uid = 0;

// Control descriptors: [label, offset, kind, fmt|options]
const signed = (c) => (v) => (v - c > 0 ? `+${v - c}` : String(v - c));
const COMMON = [["Feedback", V.FBL], ["Transpose", V.TRPS, "range", (v) => `${signed(24)(v)}`], ["Reverb rate", A.REV, "range", (v) => (v ? String(v) : "Off")]];
const LFO = [["Wave", V.LFW, "select", LFO_WAVES], ["Speed", V.LFS], ["Delay", V.LFD], ["Pitch depth", V.PMD], ["Amp depth", V.AMD],
  ["Pitch sens.", V.PMS], ["Amp sens.", V.AMS], ["Key sync", V.SYNC, "toggle"]];
const PERF = [["Mode", V.POLY, "seg", ["Poly", "Mono"]], ["Bend range", V.PBR], ["Porta mode", V.PORTMODE, "seg", ["Full", "Fingered"]],
  ["Porta time", V.PORTTIME]];
const CTRL = [["sub", "Mod wheel"], ["Pitch", V.MWPITCH], ["Amp", V.MWAMP],
  ["sub", "Breath"], ["Pitch", V.BCPITCH], ["Amp", V.BCAMP], ["Pitch bias", V.BCPBIAS, "range", signed(50)], ["EG bias", V.BCEBIAS],
  ["sub", "Foot controller"], ["Volume", V.FCVOL], ["Pitch", A.FCPITCH], ["Amp", A.FCAMP],
  ["sub", "Aftertouch"], ["Pitch", A2.ATPITCH], ["Amp", A2.ATAMP], ["Pitch bias", A2.ATPBIAS, "range", signed(50)], ["EG bias", A2.ATEBIAS]];
const FXC = [["Type", FX.PRESET, "select", EFFECTS], ["Time", FX.TIME], ["Balance", FX.BALANCE]];

export class Editor {
  /** ctx: {codec, audio, midi, device, lcd, onVoiceChange} */
  constructor(root, ctx) {
    this.root = root; this.ctx = ctx; this.codec = ctx.codec;
    this.part = 0; this.channels = [0, 1, 2, 3, 4, 5, 6, 7];
    this.histories = Array.from({ length: 8 }, () => new History());
    this.controls = []; this.gesture = false; this.lastLabel = "";
    this.throttle = new Throttle((bytes) => this.ctx.midi.send(bytes), { interval: 1000 / 60 });
    this.$ = (id) => root.querySelector(`#${id}`);
    this.build();
    for (let p = 0; p < 8; p++) this.ctx.audio.voice(p, this.voice(p));
    this.selectPart(0);
  }
  voice(p = this.part) { return this.codec.getVoice(p); }
  get channel() { return this.channels[this.part]; }

  /* ------------------------------------------------------------- building */
  build() {
    const partSel = this.$("ed-part"), chSel = this.$("ed-channel");
    for (let p = 0; p < 8; p++) partSel.append(el("option", { value: p, text: `Part ${p + 1}` }));
    for (let c = 0; c < 16; c++) chSel.append(el("option", { value: c, text: String(c + 1) }));
    partSel.addEventListener("change", () => this.selectPart(Number(partSel.value)));
    chSel.addEventListener("change", () => { this.channels[this.part] = Number(chSel.value); this.configureAudio(); this.ctx.onChannel?.(); });
    const name = this.$("ed-name");
    name.addEventListener("focus", () => this.begin());
    name.addEventListener("input", () => {
      const before = this.voice(), v = before.slice();
      setVoiceName(v, name.value.replace(/[^\x20-\x7e]/g, ""));
      this.commit(v, before, "Name");
    });
    name.addEventListener("change", () => this.end());

    const picks = this.$("ed-algo-picks");
    this.algoButtons = Array.from({ length: 8 }, (_, a) => {
      const b = el("button", { type: "button", role: "radio", "aria-checked": "false", "aria-label": `Algorithm ${a + 1}`, text: String(a + 1) });
      b.addEventListener("click", () => this.set(V.ALG, a, "Algorithm"));
      b.addEventListener("keydown", (e) => {
        const d = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
        if (d) { e.preventDefault(); const n = (a + d + 8) % 8; this.set(V.ALG, n, "Algorithm"); this.algoButtons[n].focus(); }
      });
      picks.append(b); return b;
    });
    this.list(this.$("ed-common"), COMMON);
    this.list(this.$("ed-lfo"), LFO);
    this.list(this.$("ed-perf"), PERF);
    this.list(this.$("ed-ctrl"), CTRL);
    this.list(this.$("ed-fx"), FXC);

    const ops = this.$("ed-ops");
    this.opCards = {};
    for (const op of [4, 3, 2, 1]) ops.append(this.opCard(op));

    this.$("ed-undo").addEventListener("click", () => this.undo());
    this.$("ed-redo").addEventListener("click", () => this.redo());
    this.$("ed-init").addEventListener("click", () => { const before = this.voice(); this.push(before); this.commit(this.initVoice(), before, "Init voice"); });
    this.$("ed-random").addEventListener("click", () => { const before = this.voice(); this.push(before); this.commit(randomVoice(this.initVoice()), before, "Randomize"); });
    this.$("ed-get").addEventListener("click", () => this.getFromDevice());
    this.$("ed-send").addEventListener("click", () => this.sendToDevice());
    this.root.addEventListener("keydown", (e) => {
      if (!(e.ctrlKey || e.metaKey) || e.target.matches("input:not([type=range]),textarea")) return;
      if (e.key.toLowerCase() === "z") { e.preventDefault(); e.shiftKey ? this.redo() : this.undo(); }
      if (e.key.toLowerCase() === "y") { e.preventDefault(); this.redo(); }
    });
  }
  initVoice() { const keep = this.codec.getVoice(7); const v = this.codec.resetVoice(7); this.codec.setVoice(7, keep); return v; }

  list(container, defs) { for (const d of defs) container.append(...this.control(d)); }

  /** One control row: label, input, value. Returns nodes for a 3-column grid. */
  control([label, off, kind = "range", extra], opts = {}) {
    if (label === "sub") return [el("div", { class: "subhead", text: off })];
    const id = `c${++uid}`, out = el("output", { for: id });
    const fmt = typeof extra === "function" ? extra : opts.fmt || String;
    const lbl = el("label", { for: id, text: label });
    let input, update;
    const name = opts.name || label;
    if (kind === "select") {
      input = el("select", { id }, ...extra.map((t, i) => el("option", { value: i, text: t })));
      input.addEventListener("change", () => { this.begin(); this.set(off, Number(input.value), name); this.end(); });
      update = (v) => { input.value = String(v[off]); };
      return this.reg(update, off, [lbl, input]);
    }
    if (kind === "toggle") {
      input = el("input", { id, type: "checkbox" });
      input.addEventListener("change", () => { this.begin(); this.set(off, input.checked ? 1 : 0, name); this.end(); });
      update = (v) => { input.checked = Boolean(v[off]); };
      return this.reg(update, off, [lbl, el("div", { class: "toggles" }, input)]);
    }
    if (kind === "seg") {
      const box = el("div", { class: "seg", role: "radiogroup", "aria-label": label });
      const btns = extra.map((t, i) => {
        const b = el("button", { type: "button", role: "radio", text: t });
        b.addEventListener("click", () => { this.begin(); this.set(off, i, name); this.end(); });
        box.append(b); return b;
      });
      update = (v) => btns.forEach((b, i) => b.setAttribute("aria-checked", String(v[off] === i)));
      return this.reg(update, off, [el("span", { class: "lbl", text: label }), box]);
    }
    input = el("input", { id, type: "range", min: 0, max: 99, step: 1 });
    input.addEventListener("pointerdown", () => this.begin());
    input.addEventListener("keydown", () => this.begin());
    input.addEventListener("input", () => { this.begin(); this.set(off, Number(input.value), name); });
    input.addEventListener("change", () => this.end());
    update = (v) => {
      const max = maxFor(this.codec, off, v);
      if (Number(input.max) !== max) input.max = String(max);
      input.value = String(v[off]); out.value = fmt(v[off], v);
      input.setAttribute("aria-valuetext", out.value);
    };
    return this.reg(update, off, [lbl, input, out]);
  }
  reg(update, off, nodes) { this.controls.push({ update, off }); return nodes; }

  opCard(op) {
    const o = (f) => opOffset(op, f), a = (f) => acOffset(op, f);
    const card = el("section", { class: "op-card", "aria-label": `Operator ${op}`, id: `ed-op${op}` });
    const on = el("input", { type: "checkbox", "aria-label": `Operator ${op} on` });
    on.addEventListener("change", () => {
      const v = this.voice(), bit = 1 << (op - 1);
      this.begin(); this.set(VOICE.OPMASK, on.checked ? v[VOICE.OPMASK] | bit : v[VOICE.OPMASK] & ~bit, `OP${op} on`); this.end();
    });
    const role = el("span", { class: "role" }), freq = el("span", { class: "freq", "aria-live": "off" });
    card.append(el("div", { class: "op-head" }, el("h3", { text: `OP${op}` }), role, el("label", { class: "check" }, on, "On"), freq));
    const env = svg("svg", { class: "env", viewBox: "0 0 200 58", preserveAspectRatio: "none", role: "img" });
    const envFill = svg("path", { class: "fill" }), envLine = svg("path");
    env.append(svg("line", { class: "grid", x1: 0, x2: 200, y1: 55, y2: 55 }), envFill, envLine);
    card.append(env);
    const grid = el("div", { class: "ctl-list" });
    const P = (t) => `OP${op} ${t}`;
    // frequency
    grid.append(...this.control(["Mode", a(ACF.FIX), "seg", ["Ratio", "Fixed"]], { name: P("mode") }));
    grid.append(...this.control(["Coarse", o(OPF.CRS)], { name: P("coarse"), fmt: (x, v) => v[a(ACF.FIX)] ? String(x) : this.codec.ratio(x, 0).toFixed(2) }));
    grid.append(...this.control(["Fine", a(ACF.FINE)], { name: P("fine") }));
    grid.append(...this.control(["Range", a(ACF.FIXRG), "range", (x) => FIX_RANGES[x]], { name: P("range") }));
    grid.append(...this.control(["Detune", o(OPF.DET), "range", signed(3)], { name: P("detune") }));
    // waveform
    const waves = el("div", { class: "waves", role: "radiogroup", "aria-label": `OP${op} waveform` });
    const wbtn = Array.from({ length: 8 }, (_, w) => {
      const icon = svg("svg", { viewBox: "0 0 32 16", "aria-hidden": "true" });
      let d = ""; for (let i = 0; i <= 32; i++) d += `${i ? "L" : "M"}${i} ${(8 - waveSample(w, i / 32) * 6.5).toFixed(2)}`;
      icon.append(svg("path", { d }));
      const b = el("button", { type: "button", role: "radio", "aria-label": `W${w + 1}`, title: `W${w + 1}` }, icon, `W${w + 1}`);
      b.addEventListener("click", () => { this.begin(); this.set(a(ACF.OSW), w, P("wave")); this.end(); });
      waves.append(b); return b;
    });
    grid.append(el("span", { class: "lbl", text: "Wave" }), waves);
    this.controls.push({ off: a(ACF.OSW), update: (v) => wbtn.forEach((b, w) => b.setAttribute("aria-checked", String(v[a(ACF.OSW)] === w))) });
    grid.append(el("div", { class: "subhead", text: "Envelope" }));
    for (const [t, f] of [["Attack", OPF.AR], ["Decay 1", OPF.D1R], ["Decay 1 level", OPF.D1L], ["Decay 2", OPF.D2R], ["Release", OPF.RR]])
      grid.append(...this.control([t, o(f)], { name: P(t.toLowerCase()) }));
    grid.append(...this.control(["EG shift", a(ACF.SHFT), "select", EG_SHIFT], { name: P("EG shift") }));
    grid.append(el("div", { class: "subhead", text: "Level" }));
    for (const [t, f] of [["Output", OPF.OUT], ["Velocity sens.", OPF.KVS], ["Level scaling", OPF.LS], ["Rate scaling", OPF.RS], ["EG bias sens.", OPF.EBS]])
      grid.append(...this.control([t, o(f)], { name: P(t.toLowerCase()) }));
    grid.append(...this.control(["Amp mod", o(OPF.AME), "toggle"], { name: P("AM enable") }));
    card.append(grid);
    this.controls.push({ off: -1, update: (v) => {
      const fixed = v[a(ACF.FIX)], crs = v[o(OPF.CRS)], fine = v[a(ACF.FINE)];
      freq.textContent = fixed ? `${this.codec.fixedHz(crs, v[a(ACF.FIXRG)], fine)} Hz` : `× ${this.codec.ratio(crs, fine).toFixed(2)}`;
      const isOn = (v[VOICE.OPMASK] >> (op - 1)) & 1, car = CARRIERS[v[V.ALG]].includes(op);
      on.checked = Boolean(isOn);
      card.classList.toggle("off", !isOn); card.classList.toggle("carrier", car);
      role.textContent = car ? "carrier" : "modulator";
      const pts = envelopePoints(v[o(OPF.AR)], v[o(OPF.D1R)], v[o(OPF.D1L)], v[o(OPF.D2R)], v[o(OPF.RR)]);
      const T = Math.max(...pts.map((p) => p[0])) || 1, lvl = v[o(OPF.OUT)] / 99;
      const xy = pts.map(([t, l]) => [(t / T) * 198 + 1, 55 - l * (0.25 + 0.75 * lvl) * 50]);
      const d = xy.map(([x, y], i) => `${i ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`).join("");
      envLine.setAttribute("d", d); envFill.setAttribute("d", `${d}L${xy.at(-1)[0]} 55L1 55Z`);
      env.setAttribute("aria-label", `OP${op} envelope: attack ${v[o(OPF.AR)]}, decay 1 ${v[o(OPF.D1R)]}, level ${v[o(OPF.D1L)]}, decay 2 ${v[o(OPF.D2R)]}, release ${v[o(OPF.RR)]}`);
    } });
    this.opCards[op] = card;
    return card;
  }

  drawAlgorithm(v) {
    const g = this.$("ed-algo"); g.replaceChildren();
    const alg = v[V.ALG], spec = ALGORITHMS[alg], car = CARRIERS[alg];
    const W = 40, H = 28, X = (c) => 24 + c * 50, Y = (r) => 14 + r * 46;
    const pos = Object.fromEntries(spec.ops.map(([op, c, r]) => [op, { x: X(c), y: Y(r) }]));
    for (const [from, to] of spec.edges) {
      const s = pos[from], t = pos[to];
      g.append(svg("path", { class: "edge", d: `M${s.x + W / 2} ${s.y + H}L${s.x + W / 2} ${(s.y + H + t.y) / 2}L${t.x + W / 2} ${(s.y + H + t.y) / 2}L${t.x + W / 2} ${t.y}` }));
    }
    const f = pos[4];
    g.append(svg("path", { class: "edge", d: `M${f.x + W / 2} ${f.y + H}L${f.x + W / 2} ${f.y + H + 6}L${f.x + W + 8} ${f.y + H + 6}L${f.x + W + 8} ${f.y - 6}L${f.x + W / 2} ${f.y - 6}L${f.x + W / 2} ${f.y}` }));
    const bottom = Math.max(...car.map((op) => pos[op].y)) + H + 16;
    const xs = car.map((op) => pos[op].x + W / 2);
    for (const op of car) g.append(svg("path", { class: "out", d: `M${pos[op].x + W / 2} ${pos[op].y + H}L${pos[op].x + W / 2} ${bottom}` }));
    g.append(svg("path", { class: "out", d: `M${Math.min(...xs)} ${bottom}L${Math.max(...xs)} ${bottom}M${(Math.min(...xs) + Math.max(...xs)) / 2} ${bottom}L${(Math.min(...xs) + Math.max(...xs)) / 2} ${bottom + 10}` }));
    const t = svg("text", { class: "outlabel", x: (Math.min(...xs) + Math.max(...xs)) / 2, y: bottom + 22 }); t.textContent = "out"; g.append(t);
    for (const [op] of spec.ops) {
      const p = pos[op], on = (v[VOICE.OPMASK] >> (op - 1)) & 1;
      const node = svg("g", { class: `op${car.includes(op) ? " carrier" : ""}${on ? "" : " off"}`, transform: `translate(${p.x} ${p.y})` });
      node.append(svg("rect", { width: W, height: H, rx: 2 }));
      const label = svg("text", { x: W / 2, y: H / 2 }); label.textContent = String(op); node.append(label);
      node.style.cursor = "pointer";
      node.addEventListener("click", () => this.opCards[op].scrollIntoView({ block: "nearest", behavior: "smooth" }));
      g.append(node);
    }
    g.setAttribute("viewBox", `0 0 ${Math.max(220, 24 + 3 * 50 + W + 20)} ${bottom + 30}`);
    const desc = ["4 → 3 → 2 → 1", "(4 + 3) → 2 → 1", "(4 + (3 → 2)) → 1", "((4 → 3) + 2) → 1", "4 → 3, 2 → 1", "4 → 3, 2 and 1", "4 → 3, 2, 1", "4, 3, 2, 1 in parallel"][alg];
    g.setAttribute("aria-label", `Algorithm ${alg + 1}: ${desc}; carriers ${car.join(", ")}; operator 4 has feedback`);
  }

  /* ------------------------------------------------------------- editing */
  begin() { if (!this.gesture) { this.gesture = true; this.push(this.voice()); } }
  end() { this.gesture = false; }
  push(snapshot) { this.histories[this.part].push(snapshot); this.refreshHistory(); }
  set(off, value, label) {
    const before = this.voice(), v = before.slice();
    v[off] = value;
    if (!this.gesture) this.push(before);
    this.lastOff = off;
    this.commit(v, before, label);
  }
  /** Apply bytes, let the engine clamp them, then mirror the changes everywhere. */
  commit(v, before, label = "", fromDevice = false) {
    const after = this.codec.setVoice(this.part, v);
    this.ctx.audio.voice(this.part, after);
    if (!fromDevice && this.$("ed-live").checked) {
      for (let i = 0; i < VOICE.SIZE; i++) {
        if (after[i] === before[i]) continue;
        const [g, pp] = paramFor(i);
        this.throttle.push(i, this.codec.paramMsg(this.channel, g, pp, after[i]));
      }
    }
    if (this.lastLabelPending !== true) this.lastLabel = label;
    if (!label || /^(Undo|Redo|Loaded|From device|Init voice|Randomize|Name)$/.test(label)) this.lastOff = -1;
    this.render(after);
    this.ctx.onVoiceChange?.(this.part, after);
  }
  render(v = this.voice()) {
    for (const c of this.controls) c.update(v);
    this.algoButtons.forEach((b, a) => { b.setAttribute("aria-checked", String(v[V.ALG] === a)); b.tabIndex = v[V.ALG] === a ? 0 : -1; });
    this.drawAlgorithm(v);
    const name = this.$("ed-name"); if (document.activeElement !== name) name.value = voiceName(v).trimEnd();
    this.showLcd(v);
    this.refreshHistory();
  }
  showLcd(v) {
    const top = `P${this.part + 1} ${voiceName(v)}`;
    let bottom = `ALG ${v[V.ALG] + 1}  CH ${this.channel + 1}`;
    if (this.lastLabel) {
      const val = this.lastOff >= 0 && this.lastOff !== V.NAME ? String(v[this.lastOff]) : "";
      bottom = `${this.lastLabel.slice(0, 15 - val.length)} ${val}`.trim();
    }
    this.ctx.lcd?.show(top, bottom);
  }
  refreshHistory() {
    const h = this.histories[this.part];
    this.$("ed-undo").disabled = !h.canUndo; this.$("ed-redo").disabled = !h.canRedo;
  }
  undo() { const before = this.voice(), s = this.histories[this.part].undo(before); if (s) this.commit(s, before, "Undo"); }
  redo() { const before = this.voice(), s = this.histories[this.part].redo(before); if (s) this.commit(s, before, "Redo"); }

  /** Load 130 voice bytes (e.g. from the library) into a part, with undo. */
  load(part, voice, label = "Loaded") {
    if (part !== this.part) { this.histories[part].push(this.codec.getVoice(part)); this.codec.setVoice(part, voice); this.ctx.audio.voice(part, this.codec.getVoice(part)); return; }
    const before = this.voice(); this.push(before); this.commit(voice, before, label, true);
  }
  selectPart(p) {
    this.part = p; this.gesture = false; this.throttle.clear();
    this.$("ed-part").value = String(p); this.$("ed-channel").value = String(this.channels[p]);
    this.configureAudio(); this.lastLabel = ""; this.render();
  }
  configureAudio() {
    for (let p = 0; p < 8; p++)
      this.ctx.audio.post({ type: "part", part: p, enabled: p === this.part ? 1 : 0, channel: this.channels[p], reserve: p === this.part ? 8 : 0, volume: 90, pan: 64, send: 40 });
    this.ctx.audio.allOff();
  }
  status(text) { this.$("ed-status").textContent = text; }

  async getFromDevice() {
    if (!this.ctx.midi.ready) { this.status("Connect MIDI and choose the FM-4 as MIDI out and in first."); return; }
    this.status(`Requesting the voice of the part on MIDI channel ${this.channel + 1}…`);
    try {
      const bytes = await this.ctx.device.getVoice(this.channel);
      const before = this.voice(), scratch = this.part;
      this.push(before);
      let ok = 0;
      for (const m of splitSysex(bytes)) if (this.codec.parse(scratch, m, 16).action === ACTION.VOICE_EDIT) ok++;
      const after = this.codec.getVoice(scratch);
      this.codec.setVoice(scratch, before);
      this.commit(after, before, "From device", true);
      this.status(`Received ${voiceName(after).trim() || "voice"} (${ok} blocks).`);
    } catch (e) { this.status(`${e.message} Check the MIDI in port and that the part listens on channel ${this.channel + 1}.`); }
  }
  async sendToDevice() {
    if (!this.ctx.midi.ready) { this.status("Connect MIDI and choose the FM-4 as MIDI out first."); return; }
    try {
      this.throttle.clear();
      await this.ctx.device.sendVoice(this.codec.dump(this.part, 4, this.channel));
      this.status(`Sent ${voiceName(this.voice()).trim()} to the edit buffer on channel ${this.channel + 1} (191 bytes). Store it on the FM-4 to keep it.`);
    } catch (e) { this.status(e.message); }
  }
}

/* ------------------------------------------------------------- keyboard */
const LOWER = "zsxdcvgbhnjm", UPPER = "q2w3er5t6y7u";
export class Keyboard {
  constructor(root, { onNote, lcd }) {
    this.root = root; this.onNote = onNote; this.lcd = lcd; this.base = 48; this.down = new Map(); this.held = new Set();
    this.vel = root.ownerDocument.getElementById("kb-vel");
    root.ownerDocument.getElementById("kb-down").addEventListener("click", () => this.octave(-12));
    root.ownerDocument.getElementById("kb-up").addEventListener("click", () => this.octave(12));
    this.build();
  }
  octave(d) { this.allOff(); this.base = Math.max(12, Math.min(96, this.base + d)); this.build(); this.root.ownerDocument.getElementById("kb-oct").value = noteName(this.base); }
  build() {
    this.root.replaceChildren(); this.keys = [];
    const whites = [];
    for (let i = 0; i <= 24; i++) {
      const n = this.base + i, black = [1, 3, 6, 8, 10].includes(n % 12);
      const k = el("div", { class: black ? "b" : "w", role: "button", tabindex: i === 0 ? "0" : "-1", "aria-label": noteName(n), "data-note": n });
      if (!black && n % 12 === 0) k.append(el("span", { text: noteName(n) }));
      k.addEventListener("pointerdown", (e) => { e.preventDefault(); k.setPointerCapture?.(e.pointerId); k.focus({ preventScroll: true }); this.on(n, "p" + e.pointerId); });
      k.addEventListener("pointerup", (e) => this.off("p" + e.pointerId));
      k.addEventListener("pointercancel", (e) => this.off("p" + e.pointerId));
      k.addEventListener("keydown", (e) => {
        if ((e.key === " " || e.key === "Enter") && !e.repeat) { e.preventDefault(); this.on(n, "k"); }
        const d = { ArrowRight: 1, ArrowLeft: -1 }[e.key];
        if (d) { e.preventDefault(); const t = this.keys[Math.max(0, Math.min(24, i + d))]; k.tabIndex = -1; t.tabIndex = 0; t.focus(); }
      });
      k.addEventListener("keyup", (e) => { if (e.key === " " || e.key === "Enter") this.off("k"); });
      this.keys.push(k);
      if (black) k.dataset.after = String(whites.length);
      else whites.push(k);
      this.root.append(k);
    }
    const w = 100 / whites.length;
    for (const k of this.keys) if (k.classList.contains("b")) { k.style.left = `${Number(k.dataset.after) * w - w * 0.32}%`; k.style.width = `${w * 0.64}%`; }
  }
  on(note, src) {
    if (this.down.has(src)) this.off(src);
    this.down.set(src, note);
    this.onNote(note, Number(this.vel.value));
    this.mark(note, true);
  }
  off(src) {
    const note = this.down.get(src); if (note === undefined) return;
    this.down.delete(src);
    if (![...this.down.values()].includes(note)) { this.onNote(note, 0); this.mark(note, false); }
  }
  mark(note, on) { const k = this.keys[note - this.base]; if (k) k.classList.toggle("down", on); }
  allOff() { for (const src of [...this.down.keys()]) this.off(src); }
  /** Computer keyboard: returns true when handled. */
  key(e, isDown) {
    if (e.ctrlKey || e.metaKey || e.altKey) return false;
    const c = e.key.toLowerCase();
    let i = LOWER.indexOf(c); if (i < 0 && UPPER.includes(c)) i = 12 + UPPER.indexOf(c); if (i < 0 && c === "i") i = 24;
    if (i < 0 || (c.length !== 1)) return false;
    if (isDown) { if (!e.repeat) this.on(this.base + i, "c" + c); } else this.off("c" + c);
    return true;
  }
}
