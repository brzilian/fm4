// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: the LIBRARY tab. Loaded .syx files are listed by voice; a 100-slot user bank is
// arranged by drag and drop (or keyboard) and kept in IndexedDB when available. Device transfers go
// one voice at a time (fm4ext.js): the device takes SysEx frames up to 1024 bytes and sends at most
// 128 bytes per message, so whole VMEM bulks are file-only.

import { parseSyx, bank100Syx, vmem32Syx, recordName, initRecord } from "./syx.js";
import { splitSysex } from "./device.js";
import { USER_SLOTS } from "./fm4ext.js";
import * as store from "./store.js";

const FORMAT = { "tq5-100": "TQ5 100-voice bank", "vmem-32": "32-voice VMEM", single: "single voice", mixed: "several formats", none: "no voices" };
const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v; else if (k === "text") e.textContent = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v); else e.setAttribute(k, v);
  }
  e.append(...kids); return e;
};

export class Library {
  /** ctx: {codec, audio, editor, midi, device, download, presetsUrl, fetch} */
  constructor(root, ctx) {
    this.root = root; this.ctx = ctx; this.codec = ctx.codec;
    this.sources = [];                   // {name, format, voices:[{record,name}]}
    this.bank = new Array(USER_SLOTS).fill(null);
    this.sel = null;                     // {kind:'src', s, v} | {kind:'slot', i}
    this.abort = null;
    this.$ = (id) => root.querySelector(`#${id}`);
    this.wire();
  }
  status(t) { this.$("lib-status").textContent = t; }
  warn(list) { const ul = this.$("lib-warnings"); ul.replaceChildren(...list.map((w) => el("li", { text: w }))); }

  async init() {
    const saved = await store.load("library");
    if (saved && Array.isArray(saved.bank) && saved.bank.length === USER_SLOTS) {
      this.bank = saved.bank.map((r) => (r ? Uint8Array.from(r) : null));
      this.sources = (saved.sources || []).map((s) => ({ ...s, voices: s.voices.map((v) => ({ ...v, record: Uint8Array.from(v.record) })) }));
      this.status("Restored your working library from this browser.");
    } else {
      await this.loadPresets(true);
    }
    this.render();
  }
  persist() { store.save("library", { bank: this.bank.map((r) => (r ? Array.from(r) : null)), sources: this.sources.map((s) => ({ ...s, voices: s.voices.map((v) => ({ ...v, record: Array.from(v.record) })) })) }); }

  async loadPresets(intoBank = false) {
    try {
      const res = await this.ctx.fetch(this.ctx.presetsUrl);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const src = this.addFile("FM-4_PRESETS.syx", new Uint8Array(await res.arrayBuffer()));
      if (intoBank && src) src.voices.forEach((v, i) => { if (i < USER_SLOTS) this.bank[v.slot ?? i] = v.record; });
      this.status(intoBank ? "The FM-4 preset bank is loaded into the user bank. Arrange it, then write it to the device." : "Loaded the FM-4 preset bank.");
    } catch (e) { this.status(`The FM-4 preset bank could not be loaded (${e.message}). Load a .syx file instead.`); }
    this.render(); this.persist();
  }

  addFile(name, bytes) {
    const p = parseSyx(bytes, this.codec);
    this.warn(p.warnings.map((w) => `${name}: ${w}`));
    if (!p.voices.length) return null;
    this.sources = this.sources.filter((s) => s.name !== name);
    const src = { name, format: p.format, voices: p.voices.map((v) => ({ record: v.record, name: v.name, slot: v.slot })) };
    this.sources.unshift(src);
    return src;
  }

  wire() {
    this.$("lib-file").addEventListener("change", async (e) => {
      for (const f of e.target.files) this.addFile(f.name, new Uint8Array(await f.arrayBuffer()));
      e.target.value = ""; this.render(); this.persist();
    });
    const doc = this.root.ownerDocument;
    this.root.addEventListener("dragover", (e) => { if ([...e.dataTransfer.types].includes("Files")) { e.preventDefault(); doc.body.classList.add("dragging"); } });
    this.root.addEventListener("dragleave", (e) => { if (e.target === this.root) doc.body.classList.remove("dragging"); });
    this.root.addEventListener("drop", async (e) => {
      doc.body.classList.remove("dragging");
      if (!e.dataTransfer.files.length) return;
      e.preventDefault();
      for (const f of e.dataTransfer.files) this.addFile(f.name, new Uint8Array(await f.arrayBuffer()));
      this.render(); this.persist();
    });
    this.$("lib-presets").addEventListener("click", () => this.loadPresets(false));
    this.$("lib-to-editor").addEventListener("click", () => { const r = this.selectedRecord(); if (r) this.toEditor(r); });
    this.$("lib-from-editor").addEventListener("click", () => {
      if (this.sel?.kind !== "slot") return;
      this.bank[this.sel.i] = this.codec.voiceToRecord(this.ctx.editor.voice());
      this.render(); this.persist(); this.status(`Stored the editor voice in user slot ${this.sel.i + 1}.`);
    });
    this.$("lib-clear").addEventListener("click", () => { if (this.sel?.kind === "slot") { this.bank[this.sel.i] = null; this.render(); this.persist(); } });
    this.$("lib-send-one").addEventListener("click", () => this.sendOne());
    this.$("lib-export100").addEventListener("click", () => this.ctx.download("FM-4_user_bank.syx", bank100Syx(this.bank, this.codec, 0)));
    this.$("lib-export32").addEventListener("click", () => {
      const c = Number(this.$("lib-chunk").value);
      this.ctx.download(`FM-4_user_${c * 32 + 1}-${Math.min(100, c * 32 + 32)}.syx`, vmem32Syx(this.bank.slice(c * 32, c * 32 + 32), this.codec, 0));
    });
    this.$("lib-write").addEventListener("click", () => this.writeBank());
    this.$("lib-read").addEventListener("click", () => this.readBank());
    this.$("lib-stop").addEventListener("click", () => this.abort?.abort());
  }

  selectedRecord() {
    if (!this.sel) return null;
    if (this.sel.kind === "slot") return this.bank[this.sel.i];
    return this.sources[this.sel.s]?.voices[this.sel.v]?.record || null;
  }
  audition(record) {
    // Unpack into part 0 via fm4_vmem_unpack and play it from the editor's part 0.
    const v = this.codec.vmemUnpack(0, record);
    this.ctx.audio.voice(0, v);
    if (this.ctx.editor.part === 0) this.ctx.editor.load(0, v, "Library");
    else this.ctx.editor.load(0, v);
    this.ctx.lcd?.show(`AUDITION ${recordName(record)}`, "PLAY KEYS OR MIDI");
  }
  toEditor(record) { this.ctx.editor.selectPart(0); this.audition(record); this.ctx.showTab("editor"); }

  select(sel) {
    this.sel = sel;
    for (const n of this.root.querySelectorAll("[aria-selected=true]")) n.setAttribute("aria-selected", "false");
    const node = sel.kind === "slot" ? this.root.querySelector(`#lib-slot-${sel.i}`) : this.root.querySelector(`#lib-v-${sel.s}-${sel.v}`);
    node?.setAttribute("aria-selected", "true");
    const rec = this.selectedRecord();
    if (rec) this.audition(rec);
    this.buttons();
  }
  buttons() {
    const busy = Boolean(this.abort);
    this.$("lib-to-editor").disabled = !this.selectedRecord();
    this.$("lib-from-editor").disabled = this.sel?.kind !== "slot";
    this.$("lib-clear").disabled = this.sel?.kind !== "slot" || !this.bank[this.sel.i];
    this.$("lib-send-one").disabled = busy || !this.selectedRecord();
    this.$("lib-write").disabled = busy; this.$("lib-read").disabled = busy;
    this.$("lib-stop").hidden = !busy;
  }

  render() {
    const box = this.$("lib-sources");
    if (!this.sources.length) box.replaceChildren(el("p", { class: "empty", text: "Drop .syx files here, or press Load .syx files. TQ5, TX81Z, DX11, DX21, DX27 and DX100 banks and single voices are read." }));
    else box.replaceChildren(...this.sources.map((src, s) => {
      const all = el("button", { class: "btn small", text: "Fill user bank", title: "Copy these voices into the user bank from slot 1" });
      all.addEventListener("click", () => {
        src.voices.slice(0, USER_SLOTS).forEach((v, i) => { this.bank[v.slot ?? i] = v.record; });
        this.render(); this.persist(); this.status(`Copied ${Math.min(USER_SLOTS, src.voices.length)} voices from ${src.name} into the user bank.`);
      });
      const remove = el("button", { class: "btn small", text: "Remove", "aria-label": `Remove ${src.name}` });
      remove.addEventListener("click", () => { this.sources.splice(s, 1); this.sel = null; this.render(); this.persist(); });
      const ul = el("ul", { class: "vlist", role: "listbox", "aria-label": src.name });
      src.voices.forEach((v, i) => {
        const li = el("li", { class: "vitem", role: "option", id: `lib-v-${s}-${i}`, tabindex: "0", draggable: "true", "aria-selected": "false", text: v.name, title: `${v.name.trim()} (${i + 1})` });
        li.addEventListener("click", () => this.select({ kind: "src", s, v: i }));
        li.addEventListener("keydown", (e) => {
          if (e.key === "Enter" || e.key === " ") { e.preventDefault(); this.select({ kind: "src", s, v: i }); }
          const d = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
          if (d) { e.preventDefault(); this.root.querySelector(`#lib-v-${s}-${i + d}`)?.focus(); }
        });
        li.addEventListener("dragstart", (e) => { e.dataTransfer.setData("text/x-fm4", JSON.stringify({ kind: "src", s, v: i })); e.dataTransfer.effectAllowed = "copy"; });
        ul.append(li);
      });
      return el("div", { class: "src" }, el("h3", {}, src.name, el("span", { class: "fmt", text: `${FORMAT[src.format] || src.format}, ${src.voices.length} voices` }), all, remove), ul);
    }));

    const bank = this.$("lib-bank");
    bank.replaceChildren(...this.bank.map((rec, i) => {
      const li = el("li", { class: `slot${rec ? "" : " empty-slot"}`, id: `lib-slot-${i}`, role: "option", tabindex: "0", draggable: rec ? "true" : "false",
        "aria-selected": String(this.sel?.kind === "slot" && this.sel.i === i), "aria-label": `User ${String(i + 1).padStart(2, "0")}: ${rec ? recordName(rec).trim() : "empty"}` },
        el("span", { class: "n", text: String(i + 1).padStart(2, "0") }), el("span", { class: "nm", text: rec ? recordName(rec) : "empty" }));
      li.addEventListener("click", () => this.select({ kind: "slot", i }));
      li.addEventListener("keydown", (e) => this.slotKey(e, i));
      li.addEventListener("dragstart", (e) => { e.dataTransfer.setData("text/x-fm4", JSON.stringify({ kind: "slot", i })); e.dataTransfer.effectAllowed = "move"; });
      li.addEventListener("dragover", (e) => { if ([...e.dataTransfer.types].includes("text/x-fm4")) { e.preventDefault(); li.classList.add("drop"); } });
      li.addEventListener("dragleave", () => li.classList.remove("drop"));
      li.addEventListener("drop", (e) => {
        li.classList.remove("drop");
        const raw = e.dataTransfer.getData("text/x-fm4"); if (!raw) return;
        e.preventDefault(); e.stopPropagation();
        this.place(JSON.parse(raw), i);
      });
      return li;
    }));
    bank.setAttribute("role", "listbox"); bank.setAttribute("aria-label", "User bank");
    this.buttons();
  }

  /** Put a source voice into slot `to`, or move a slot there (swapping). */
  place(from, to) {
    if (from.kind === "slot") { const t = this.bank[to]; this.bank[to] = this.bank[from.i]; this.bank[from.i] = t; }
    else { const r = this.sources[from.s]?.voices[from.v]?.record; if (!r) return; this.bank[to] = r; }
    this.sel = { kind: "slot", i: to };
    this.render(); this.persist();
    this.root.querySelector(`#lib-slot-${to}`)?.focus();
  }
  slotKey(e, i) {
    const cols = Math.max(1, Math.round(this.$("lib-bank").clientWidth / 135));
    const d = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: cols, ArrowUp: -cols }[e.key];
    if (d) {
      e.preventDefault();
      const j = Math.max(0, Math.min(USER_SLOTS - 1, i + d));
      if (e.altKey) this.place({ kind: "slot", i }, j); else this.root.querySelector(`#lib-slot-${j}`)?.focus();
      return;
    }
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      if (this.sel?.kind === "src") this.place(this.sel, i); else this.select({ kind: "slot", i });
    }
    if (e.key === "Delete" || e.key === "Backspace") { this.bank[i] = null; this.render(); this.persist(); this.root.querySelector(`#lib-slot-${i}`)?.focus(); }
  }

  /* ---------------------------------------------------------------- device */
  needDevice() {
    if (this.ctx.midi.ready) return true;
    this.status("Connect MIDI and choose the FM-4 as MIDI out (and in, to receive) first."); return false;
  }
  dumpOf(record, ch) { const keep = this.codec.getVoice(7); this.codec.vmemUnpack(7, record); const d = this.codec.dump(7, 4, ch); this.codec.setVoice(7, keep); return d; }

  async sendOne() {
    const r = this.selectedRecord(); if (!r || !this.needDevice()) return;
    const ch = this.ctx.editor.channels[0];
    try { await this.ctx.device.sendVoice(this.dumpOf(r, ch)); this.status(`Sent ${recordName(r).trim()} to the edit buffer of the part on channel ${ch + 1}.`); }
    catch (e) { this.status(e.message); }
  }

  async writeBank() {
    if (!this.needDevice()) return;
    const ch = this.ctx.editor.channels[0];
    const filled = this.bank.filter(Boolean).length;
    if (!filled) { this.status("The user bank is empty."); return; }
    if (!this.root.ownerDocument.defaultView.confirm(`Write ${filled} voices to the FM-4's user bank? This replaces those user voices on the device.`)) return;
    const dumps = this.bank.map((r) => (r ? this.dumpOf(r, ch) : null));
    await this.transfer("Writing", async (signal, progress) => {
      const n = await this.ctx.device.writeBank(dumps, { part: 0, signal, onProgress: (k, t, slot) => progress(k, t, slot) });
      return `Wrote and stored ${n} voices in the FM-4's user bank.`;
    });
  }

  async readBank() {
    if (!this.needDevice()) return;
    await this.transfer("Reading", async (signal, progress) => {
      const dumps = await this.ctx.device.readBank({ signal, onProgress: (k, t, slot) => progress(k, t, slot) });
      const voices = dumps.map((d) => {
        this.codec.resetVoice(7);
        for (const m of splitSysex(d)) this.codec.parse(7, m, 16);
        const record = this.codec.vmemPack(7);
        return { record, name: recordName(record) };
      });
      voices.forEach((v, i) => { v.slot = i; });
      const name = `FM-4 user bank (${new Date().toLocaleString()})`;
      this.sources.unshift({ name, format: "tq5-100", voices });
      return `Received ${voices.length} user voices. They are listed under "${name}"; Fill user bank copies them into the working bank.`;
    });
  }

  async transfer(verb, job) {
    const bar = this.$("lib-progress");
    this.abort = new AbortController(); this.buttons();
    bar.hidden = false; bar.value = 0;
    try {
      const msg = await job(this.abort.signal, (k, t, slot) => {
        bar.value = Math.round((k / t) * 100);
        this.status(`${verb} user voice ${slot + 1} (${k} of ${t})…`);
        for (const n of this.root.querySelectorAll(".slot.busy")) n.classList.remove("busy");
        this.root.querySelector(`#lib-slot-${slot}`)?.classList.add("busy");
      });
      this.status(msg);
    } catch (e) { this.status(e.code === "timeout" ? `${e.message} Check the MIDI in port, then try again.` : e.message); }
    finally {
      this.abort = null; bar.hidden = true;
      for (const n of this.root.querySelectorAll(".slot.busy")) n.classList.remove("busy");
      this.render(); this.persist();
    }
  }
}

export { initRecord };
