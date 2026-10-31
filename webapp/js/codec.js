// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: a thin wrapper around fm4.wasm, used as the TQ5 SysEx codec on the main thread (and
// in Node tests). The same module runs as the audio engine inside the AudioWorklet (worklet.js).
// Voice layout at fm4_voice_ptr(part): vced[93] +0, aced[23] +93, aced2[10] +116, efeds[3] +126,
// opmask +129; fm4_voice_size() = 130.

export const VOICE = Object.freeze({ VCED: 0, ACED: 93, ACED2: 116, EFEDS: 126, OPMASK: 129, SIZE: 130 });
export const ACTION = Object.freeze({ NONE: 0, ERROR: 1, VOICE_EDIT: 2, VMEM_BLOCK: 3, DUMP_REQUEST: 4, REMOTE: 5,
  MULTI_PARAM: 6, SYS_PARAM: 7, BLOCK_SELECT: 8, MASTER_TUNE: 9 });
export const CHANGED = Object.freeze({ VCED: 1, ACED: 2, ACED2: 4, EFEDS: 8 });
export const REQUEST = Object.freeze({ VCED: 0, VMEM: 1, ACED: 2, ACED2: 3, EFEDS: 4, EFEDS_ONLY: 5 });
const IO_SIZE = 8200;

export class Codec {
  static async create(bytes, polyphony = 8) {
    const { instance } = await WebAssembly.instantiate(bytes, {});
    return new Codec(instance, polyphony);
  }
  constructor(instance, polyphony = 8) {
    this.x = instance.exports;
    this.x.fm4_init(polyphony);
    if (this.x.fm4_voice_size() !== VOICE.SIZE) throw new Error("fm4.wasm voice layout changed");
    this.vcedMax = Uint8Array.from({ length: 93 }, (_, i) => this.x.fm4_vced_max(i));
    this.acedMax = Uint8Array.from({ length: 23 }, (_, i) => this.x.fm4_aced_max(i));
  }
  get mem() { return new Uint8Array(this.x.memory.buffer); }
  io(n = IO_SIZE) { return new Uint8Array(this.x.memory.buffer, this.x.fm4_io(), n); }
  voiceView(part) { return new Uint8Array(this.x.memory.buffer, this.x.fm4_voice_ptr(part), VOICE.SIZE); }
  getVoice(part) { return this.voiceView(part).slice(); }
  /** Write 130 voice bytes, clamp in the engine, return the clamped copy. */
  setVoice(part, bytes) { this.voiceView(part).set(bytes.subarray(0, VOICE.SIZE)); this.x.fm4_voice_commit(part); return this.getVoice(part); }
  resetVoice(part) { this.x.fm4_voice_reset(part); return this.getVoice(part); }
  /** Pack part's voice into a 128-byte VMEM record. */
  vmemPack(part) { this.x.fm4_vmem_pack(part); return this.io(128).slice(); }
  /** Unpack a 128-byte VMEM record into part; returns the voice bytes. */
  vmemUnpack(part, record) { this.io(128).set(record.subarray(0, 128)); this.x.fm4_vmem_unpack(part); return this.getVoice(part); }
  recordToVoice(record, part = 7) { const keep = this.getVoice(part); const v = this.vmemUnpack(part, record); this.setVoice(part, keep); return v; }
  voiceToRecord(voice, part = 7) { const keep = this.getVoice(part); this.setVoice(part, voice); const r = this.vmemPack(part); this.setVoice(part, keep); return r; }
  /** Parse one complete SysEx message into part's edit buffer. */
  parse(part, msg, rxChannel = 16) {
    if (msg.length > IO_SIZE) return { action: ACTION.ERROR };
    this.io(msg.length).set(msg);
    const action = this.x.fm4_sysex_parse(part, msg.length, rxChannel);
    const r = (i) => this.x.fm4_sysex_result(i);
    return { action, changed: r(0), block: r(1), request: r(2), part: r(3), pp: r(4), value: r(5), vmemAt: r(6) };
  }
  /** Full dump for a request kind (4 = EFEDS+ACED2+ACED+VCED, 191 bytes). */
  dump(part, request = REQUEST.EFEDS, ch = 0) { const n = this.x.fm4_sysex_dump(part, request, ch); return this.io(n).slice(); }
  /** 32 VMEM records -> one 4104-byte VMEM bulk message. */
  vmemBulk(records, ch = 0) {
    const io = this.io(); io.fill(0, 4104, 4104 + 4096);
    records.slice(0, 32).forEach((rec, i) => io.set(rec.subarray(0, 128), 4104 + i * 128));
    const n = this.x.fm4_vmem_bulk(ch); return this.io(n).slice();
  }
  paramMsg(ch, group, pp, value) { const n = this.x.fm4_param_msg(ch, group, pp, value); return this.io(n).slice(); }
  ratio(crs, fine) { return this.x.fm4_ratio_x100(crs, fine) / 100; }
  fixedHz(crs, range, fine) { return this.x.fm4_fixed_hz(crs, range, fine); }
}

/** Split a 191-byte (or any concatenated) dump into individual F0..F7 messages. */
export function splitMessages(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== 0xF0) continue;
    const end = bytes.indexOf(0xF7, i + 1);
    if (end < 0) break;
    out.push(bytes.slice(i, end + 1)); i = end;
  }
  return out;
}

export function voiceName(voice) {
  let s = "";
  for (let i = 0; i < 10; i++) { const c = voice[77 + i]; s += c >= 32 && c <= 126 ? String.fromCharCode(c) : " "; }
  return s;
}
export function setVoiceName(voice, name) {
  const t = String(name).slice(0, 10).padEnd(10, " ");
  for (let i = 0; i < 10; i++) { const c = t.charCodeAt(i); voice[77 + i] = c >= 32 && c <= 126 ? c : 32; }
}
