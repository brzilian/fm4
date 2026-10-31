// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: main-thread side of the audition engine (worklet.js). Started on the first user
// gesture because browsers keep audio suspended until then.

export class Audio {
  constructor(wasmBytes) {
    this.bytes = wasmBytes; this.ctx = null; this.node = null; this.ready = null;
    this.onmeter = null; this.pending = [];
  }
  /** Create (or resume) the AudioContext. Must be called from a user gesture. */
  start() {
    if (this.ready) { this.ctx.resume?.(); return this.ready; }
    const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!AC) return (this.ready = Promise.reject(new Error("Web Audio is not available")));
    try { this.ctx = new AC({ sampleRate: 44100, latencyHint: "interactive" }); }
    catch { this.ctx = new AC({ latencyHint: "interactive" }); }
    this.ctx.resume?.();
    this.ready = (async () => {
      await this.ctx.audioWorklet.addModule(new URL("./worklet.js", import.meta.url));
      this.node = new AudioWorkletNode(this.ctx, "fm4-engine", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
      this.node.connect(this.ctx.destination);
      const ok = new Promise((resolve, reject) => {
        this.node.port.onmessage = (e) => {
          const m = e.data;
          if (m.type === "ready") resolve();
          else if (m.type === "error") reject(new Error(m.message));
          else if (m.type === "meter") this.onmeter?.(m);
        };
      });
      this.node.port.postMessage({ type: "init", bytes: this.bytes.slice(0), polyphony: 8 });
      await ok;
      for (const m of this.pending.splice(0)) this.node.port.postMessage(m);
      if (this.ctx.sampleRate !== 44100) console.warn(`FM-4 audition runs at ${this.ctx.sampleRate} Hz; pitch is offset from 44.1 kHz.`);
    })();
    return this.ready;
  }
  post(m) { if (this.node) this.node.port.postMessage(m); else this.pending = [...this.pending.filter((p) => !(m.type === "voice" && p.type === "voice" && p.part === m.part)), m]; }
  voice(part, bytes) { this.post({ type: "voice", part, bytes: Uint8Array.from(bytes) }); }
  midi(data) { if (this.node) this.node.port.postMessage({ type: "midi", data: Array.from(data) }); }
  noteOn(ch, note, vel = 100) { this.start(); this.midi([0x90 | ch, note, vel]); }
  noteOff(ch, note) { this.midi([0x80 | ch, note, 0]); }
  allOff() { if (this.node) this.node.port.postMessage({ type: "alloff" }); }
}
