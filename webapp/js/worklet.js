// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: AudioWorklet that runs fm4.wasm (the firmware's engine) for auditioning.
// The main thread posts the wasm bytes; the engine renders 128-frame blocks of interleaved float
// stereo at 44.1 kHz (FM4_RATE) into fm4_out().
// Messages in:  {type:'init', bytes, polyphony}  {type:'midi', data:[s,d1,d2]}
//               {type:'voice', part, bytes(130)}  {type:'alloff'}  {type:'part', part, enabled, channel, reserve, volume, pan, send}
// Messages out: {type:'ready'} {type:'error', message} {type:'meter', voices, peak}

class FM4Processor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.x = null; this.queue = []; this.meterAt = 0; this.peak = 0; this.voices = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }
  onMessage(m) {
    if (m.type === "init") {
      WebAssembly.instantiate(m.bytes, {}).then(({ instance }) => {
        this.x = instance.exports;
        this.x.fm4_init(m.polyphony || 8);
        for (const q of this.queue.splice(0)) this.onMessage(q);
        this.port.postMessage({ type: "ready" });
      }, (err) => this.port.postMessage({ type: "error", message: String(err) }));
      return;
    }
    if (!this.x) { this.queue.push(m); return; }
    const x = this.x;
    switch (m.type) {
      case "midi": x.fm4_midi(m.data[0], m.data[1] ?? 0, m.data[2] ?? 0); break;
      case "voice": {
        const part = m.part & 7;
        new Uint8Array(x.memory.buffer, x.fm4_voice_ptr(part), x.fm4_voice_size()).set(m.bytes);
        x.fm4_voice_commit(part);
        break;
      }
      case "part": x.fm4_part_config(m.part, m.enabled, m.channel, m.reserve, m.volume, m.pan, m.send); break;
      case "alloff": x.fm4_all_off(); break;
    }
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    if (!this.x || !out || !out.length) return true;
    const L = out[0], R = out[1] || out[0];
    this.voices = this.x.fm4_render();
    const buf = new Float32Array(this.x.memory.buffer, this.x.fm4_out(), 256);
    const n = Math.min(L.length, 128);
    for (let i = 0; i < n; i++) {
      const l = buf[2 * i], r = buf[2 * i + 1];
      L[i] = l; R[i] = r;
      const a = l < 0 ? -l : l; if (a > this.peak) this.peak = a;
    }
    if (++this.meterAt >= 20) {
      this.port.postMessage({ type: "meter", voices: this.voices, peak: this.peak });
      this.meterAt = 0; this.peak = 0;
    }
    return true;
  }
}
registerProcessor("fm4-engine", FM4Processor);
