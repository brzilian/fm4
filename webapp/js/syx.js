// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: reading and writing 4-operator Yamaha .syx files.
//   - 32-voice VMEM banks (TQ5, TX81Z, DX11, DX21, DX27, DX100): F0 43 0n 04 20 00 <4096> cs F7
//   - TQ5 100-voice banks: 4 x [F0 43 1n 24 07 bb F7 + VMEM], slots 0-24 of each block used
//   - single voices: VCED alone or with ACED / ACED2 / EFEDS (any order, VCED completes a voice)
// Files may hold several messages; bad checksums are accepted with a warning.
// The library keeps every voice as its 128-byte VMEM record.

export const VMEM_LEN = 4104;
export const BANK100_LEN = 4 * (7 + VMEM_LEN);
const LM = { "LM  8976AE": "ACED", "LM  8023AE": "ACED2", "LM  8036EF": "EFEDS" };

export const checksum = (bytes) => (-bytes.reduce((a, b) => a + b, 0)) & 0x7F;
export const recordName = (rec) => {
  let s = "";
  for (let i = 57; i < 67; i++) s += rec[i] >= 32 && rec[i] <= 126 ? String.fromCharCode(rec[i]) : " ";
  return s;
};

/** Split raw bytes into F0..F7 messages; report stray bytes and unterminated data. */
export function splitSysex(bytes) {
  const messages = [], warnings = [];
  let i = 0, stray = 0;
  while (i < bytes.length) {
    if (bytes[i] !== 0xF0) { stray++; i++; continue; }
    let j = i + 1;
    while (j < bytes.length && bytes[j] !== 0xF7 && bytes[j] < 0x80) j++;
    if (j >= bytes.length) { warnings.push(`Unterminated SysEx message at byte ${i} was ignored.`); break; }
    if (bytes[j] !== 0xF7) { warnings.push(`Broken SysEx message at byte ${i} was ignored.`); i = j; continue; }
    messages.push(bytes.slice(i, j + 1));
    i = j + 1;
  }
  if (stray) warnings.push(`${stray} byte${stray === 1 ? "" : "s"} outside SysEx messages ignored.`);
  return { messages, warnings };
}

function classify(m) {
  if (m.length < 5 || m[1] !== 0x43) return { kind: "other" };
  const sub = m[2] & 0x70, ch = m[2] & 15;
  if (sub === 0x10 && m.length === 7 && m[3] === 0x24 && m[4] === 0x07) return { kind: "block", ch, block: m[5] };
  if (sub !== 0x00 || m.length < 8) return { kind: "other" };
  const count = (m[4] << 7) | m[5], format = m[3];
  if (m.length !== count + 8) return { kind: "bad", why: "length does not match its byte count" };
  const covered = format === 0x7E ? m.subarray(6, 6 + count) : m.subarray(6, 6 + count);
  const ok = checksum(covered) === m[6 + count];
  if (format === 0x04 && count === 4096) return { kind: "vmem", ch, ok };
  if (format === 0x03 && count === 93) return { kind: "VCED", ch, ok };
  if (format === 0x7E && count >= 10) {
    const hdr = String.fromCharCode(...m.subarray(6, 16));
    const kind = LM[hdr];
    const want = { ACED: 23, ACED2: 10, EFEDS: 3 }[kind];
    if (kind && count - 10 === want) return { kind, ch, ok };
    return { kind: "lm", hdr };
  }
  return { kind: "other" };
}

/** Same message with a corrected checksum (for lenient import). */
function fixChecksum(m) {
  const count = (m[4] << 7) | m[5], out = m.slice();
  out[6 + count] = checksum(out.subarray(6, 6 + count));
  return out;
}

/**
 * Parse a .syx file. codec: a Codec (codec.js) used to turn single-voice blocks into VMEM records.
 * Returns { voices: [{record, name, slot?}], format, warnings }.
 */
export function parseSyx(bytes, codec, scratch = 7) {
  const { messages, warnings } = splitSysex(bytes);
  const bank = new Array(100).fill(null), plain = [], singles = [];
  let block = null, sawBlock = false, voiceOpen = false, badSums = 0, kinds = new Set();
  const startVoice = () => { if (!voiceOpen) { codec.resetVoice(scratch); voiceOpen = true; } };
  for (const m of messages) {
    const c = classify(m);
    if (c.ok === false) badSums++;
    switch (c.kind) {
      case "block":
        if (c.block >= 1 && c.block <= 4) { block = c.block - 1; sawBlock = true; }
        else warnings.push(`Block select ${c.block} is outside 1-4 and was ignored.`);
        break;
      case "vmem": {
        kinds.add(block === null ? "vmem" : "bank100");
        const data = m.subarray(6, 6 + 4096);
        const recs = Array.from({ length: 32 }, (_, v) => data.slice(v * 128, v * 128 + 128));
        if (block !== null) { for (let v = 0; v < 25; v++) bank[block * 25 + v] = recs[v]; block = null; }
        else plain.push(...recs);
        break;
      }
      case "ACED": case "ACED2": case "EFEDS": case "VCED": {
        kinds.add("single");
        startVoice();
        const r = codec.parse(scratch, c.ok ? m : fixChecksum(m), 16);
        if (r.action !== 2) warnings.push(`A ${c.kind} block could not be read.`);
        if (c.kind === "VCED") { singles.push(codec.vmemPack(scratch)); voiceOpen = false; }
        break;
      }
      case "lm": warnings.push(`Skipped a "${c.hdr}" block (not voice data).`); break;
      case "bad": warnings.push(`Skipped a Yamaha message whose ${c.why}.`); break;
      default: warnings.push(`Skipped a message that is not 4-operator voice data (${Array.from(m.subarray(0, 4), (x) => x.toString(16).padStart(2, "0")).join(" ")}...).`);
    }
  }
  if (voiceOpen) warnings.push("The file ends with voice blocks but no VCED; that voice was dropped.");
  if (badSums) warnings.push(`${badSums} message${badSums === 1 ? " has a" : "s have"} a wrong checksum; the data was loaded anyway.`);
  if (block !== null) warnings.push("A block select was not followed by a VMEM bank.");
  const voices = [];
  if (sawBlock) bank.forEach((record, slot) => { if (record) voices.push({ record, name: recordName(record), slot }); });
  for (const record of [...plain, ...singles]) voices.push({ record, name: recordName(record) });
  const format = kinds.size > 1 ? "mixed" : kinds.has("bank100") ? "tq5-100" : kinds.has("vmem") ? "vmem-32" : kinds.has("single") ? "single" : "none";
  if (!voices.length) warnings.push("No voices were found in this file.");
  return { voices, format, warnings };
}

export const blockSelect = (ch, b) => Uint8Array.of(0xF0, 0x43, 0x10 | (ch & 15), 0x24, 0x07, b, 0xF7);

/** TQ5 100-voice bank: 4 x (block select + VMEM). Slots 25-31 of each block repeat slot 0. */
export function bank100Syx(records, codec, ch = 0) {
  const init = initRecord(codec), out = new Uint8Array(BANK100_LEN);
  let at = 0;
  for (let b = 0; b < 4; b++) {
    const recs = Array.from({ length: 25 }, (_, v) => records[b * 25 + v] || init);
    while (recs.length < 32) recs.push(recs[0]);
    out.set(blockSelect(ch, b + 1), at); at += 7;
    out.set(codec.vmemBulk(recs, ch), at); at += VMEM_LEN;
  }
  return out;
}

/** One 32-voice VMEM bulk; missing voices are INIT VOICE. */
export function vmem32Syx(records, codec, ch = 0) {
  const init = initRecord(codec);
  return codec.vmemBulk(Array.from({ length: 32 }, (_, v) => records[v] || init), ch);
}

export function initRecord(codec, scratch = 7) {
  const keep = codec.getVoice(scratch);
  codec.resetVoice(scratch);
  const r = codec.vmemPack(scratch);
  codec.setVoice(scratch, keep);
  return r;
}
