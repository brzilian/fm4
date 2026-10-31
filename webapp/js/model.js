// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: the TQ5 voice as 130 bytes (codec.js VOICE layout) and what each byte means.
// Operator blocks inside VCED / ACED are stored OP4, OP2, OP3, OP1.

import { VOICE } from "./codec.js";

export const OP_BLOCK = Object.freeze({ 1: 3, 2: 1, 3: 2, 4: 0 });       // TX81Z operator -> block index
export const OPF = Object.freeze({ AR: 0, D1R: 1, D2R: 2, RR: 3, D1L: 4, LS: 5, RS: 6, EBS: 7, AME: 8, KVS: 9, OUT: 10, CRS: 11, DET: 12 });
export const ACF = Object.freeze({ FIX: 0, FIXRG: 1, FINE: 2, OSW: 3, SHFT: 4 });
export const V = Object.freeze({ ALG: 52, FBL: 53, LFS: 54, LFD: 55, PMD: 56, AMD: 57, SYNC: 58, LFW: 59, PMS: 60, AMS: 61,
  TRPS: 62, POLY: 63, PBR: 64, PORTMODE: 65, PORTTIME: 66, FCVOL: 67, SUS: 68, PORTSW: 69, CHORUS: 70,
  MWPITCH: 71, MWAMP: 72, BCPITCH: 73, BCAMP: 74, BCPBIAS: 75, BCEBIAS: 76, NAME: 77 });
export const A = Object.freeze({ REV: VOICE.ACED + 20, FCPITCH: VOICE.ACED + 21, FCAMP: VOICE.ACED + 22 });
export const A2 = Object.freeze({ ATPITCH: VOICE.ACED2, ATAMP: VOICE.ACED2 + 1, ATPBIAS: VOICE.ACED2 + 2, ATEBIAS: VOICE.ACED2 + 3 });
export const FX = Object.freeze({ PRESET: VOICE.EFEDS, TIME: VOICE.EFEDS + 1, BALANCE: VOICE.EFEDS + 2 });

export const opOffset = (op, field) => 13 * OP_BLOCK[op] + field;
export const acOffset = (op, field) => VOICE.ACED + 5 * OP_BLOCK[op] + field;

export const EFFECTS = ["Off", "Reverb Hall", "Reverb Room", "Reverb Plate", "Delay", "Delay L/R", "Stereo Echo",
  "Distortion + Reverb", "Distortion + Echo", "Gate Reverb", "Reverse Gate"];
export const LFO_WAVES = ["Saw up", "Square", "Triangle", "Sample & hold"];
export const EG_SHIFT = ["Off", "48 dB", "24 dB", "12 dB"];
export const FIX_RANGES = ["255 Hz", "510 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz", "32 kHz"];
const ACED2_MAX = [99, 99, 100, 99, 1, 1, 1, 1, 15, 0];
export const effectTimeMax = (preset) => ([4, 5, 6, 8].includes(preset) ? 75 : 40);

/** Largest value for byte `offset` of `voice` (some limits depend on other fields). */
export function maxFor(codec, offset, voice) {
  if (offset < VOICE.ACED) return codec.vcedMax[offset];
  if (offset < VOICE.ACED2) {
    const i = offset - VOICE.ACED;
    if (i < 20 && i % 5 === ACF.FINE && voice) {
      const block = Math.floor(i / 5), crs = voice[13 * block + OPF.CRS];
      return crs < 4 ? 7 : 15;
    }
    return codec.acedMax[i];
  }
  if (offset < VOICE.EFEDS) return ACED2_MAX[offset - VOICE.ACED2];
  if (offset === FX.PRESET) return 10;
  if (offset === FX.TIME) return effectTimeMax(voice ? voice[FX.PRESET] : 0);
  if (offset === FX.BALANCE) return 99;
  if (offset === VOICE.OPMASK) return 15;
  throw new RangeError(`no voice byte ${offset}`);
}

/** TQ5 parameter change address for a voice byte: [group, pp]. */
export function paramFor(offset) {
  if (offset < VOICE.ACED) return [0x12, offset];
  if (offset < VOICE.ACED2) return [0x13, offset - VOICE.ACED];
  if (offset < VOICE.EFEDS) return [0x13, 23 + offset - VOICE.ACED2];
  if (offset < VOICE.OPMASK) return [0x24, 4 + offset - VOICE.EFEDS];
  if (offset === VOICE.OPMASK) return [0x12, 93];
  throw new RangeError(`no voice byte ${offset}`);
}

/** Which operators are carriers for each algorithm (TX81Z numbering). */
export const CARRIERS = [[1], [1], [1], [1], [3, 1], [3, 2, 1], [3, 2, 1], [4, 3, 2, 1]];
/** Diagram layout: [op, col, row] and modulation edges [from, to]; OP4 always has feedback. */
export const ALGORITHMS = [
  { ops: [[4, 1, 0], [3, 1, 1], [2, 1, 2], [1, 1, 3]], edges: [[4, 3], [3, 2], [2, 1]] },
  { ops: [[4, 0.5, 0], [3, 1.5, 0], [2, 1, 1], [1, 1, 2]], edges: [[4, 2], [3, 2], [2, 1]] },
  { ops: [[3, 1.5, 0], [2, 1.5, 1], [4, 0.5, 1], [1, 1, 2]], edges: [[3, 2], [4, 1], [2, 1]] },
  { ops: [[4, 0.5, 0], [3, 0.5, 1], [2, 1.5, 1], [1, 1, 2]], edges: [[4, 3], [3, 1], [2, 1]] },
  { ops: [[4, 0.5, 0], [3, 0.5, 1], [2, 1.5, 0], [1, 1.5, 1]], edges: [[4, 3], [2, 1]] },
  { ops: [[4, 1, 0], [3, 0, 1], [2, 1, 1], [1, 2, 1]], edges: [[4, 3], [4, 2], [4, 1]] },
  { ops: [[4, 0, 0], [3, 0, 1], [2, 1, 1], [1, 2, 1]], edges: [[4, 3]] },
  { ops: [[4, 0, 0], [3, 1, 0], [2, 2, 0], [1, 3, 0]], edges: [] },
];

/** One period of waveform W1..W8 (index 0..7) at phase 0..1 (spec §6.6). */
export function waveSample(w, ph) {
  const s = (x) => Math.sin(2 * Math.PI * x), sq = (x) => Math.sign(s(x)) * s(x) * s(x);
  const base = w % 2 ? sq : s;
  switch (w >> 1) {
    case 0: return base(ph);
    case 1: return ph < 0.5 ? base(ph) : 0;
    case 2: return ph < 0.5 ? base(ph * 2) : 0;
    default: return ph < 0.5 ? Math.abs(base(ph * 2)) : 0;
  }
}

/** Envelope as [t, level] points (t in arbitrary units, level 0..1) for drawing. */
export function envelopePoints(ar, d1r, d1l, d2r, rr) {
  const seg = (rate, max) => (rate <= 0 ? 4 : 1.4 * Math.pow(2, -(rate / max) * 5) + 0.02);
  const pts = [[0, 0]];
  let t = 0;
  if (ar === 0) { pts.push([4, 0]); return pts; }
  t += seg(ar, 31) * 0.6; pts.push([t, 1]);
  const l1 = d1l / 15;
  t += d1l === 15 ? 0.05 : seg(d1r, 31); pts.push([t, d1r === 0 ? 1 : l1]);
  const top = d1r === 0 ? 1 : l1;
  const hold = 1.2, fall = d2r === 0 ? 0 : Math.min(1, hold / seg(d2r, 31) * 0.5);
  t += hold; const sus = Math.max(0, top * (1 - fall)); pts.push([t, sus]);
  t += sus > 0 ? seg(rr * 2 + 1, 31) : 0.05; pts.push([t, 0]);
  return pts;
}

/** Deterministic PRNG for tests and "randomize". */
export function mulberry32(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

const RATIO_CRS = [4, 4, 4, 8, 8, 10, 13, 16, 19, 0, 22, 25, 28];   // 1, 2, 3, 4, 5, 6, 0.5, 7, 8, 9 (weighted to low ratios)
/**
 * A musically constrained random voice: playable carriers, moderate modulators, mostly harmonic
 * ratios, gentle LFO, effect from the TQ5 list. Starts from base (130 bytes, e.g. INIT VOICE).
 */
export function randomVoice(base, rnd = Math.random) {
  const v = Uint8Array.from(base);
  const pick = (a) => a[Math.floor(rnd() * a.length)];
  const range = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));
  const alg = range(0, 7);
  v[V.ALG] = alg;
  v[V.FBL] = rnd() < 0.5 ? 0 : range(1, 6);
  const carriers = CARRIERS[alg];
  const pluck = rnd() < 0.45;
  for (const op of [1, 2, 3, 4]) {
    const car = carriers.includes(op);
    const o = (f) => opOffset(op, f), a = (f) => acOffset(op, f);
    v[o(OPF.AR)] = car ? range(pluck ? 29 : 18, 31) : range(20, 31);
    v[o(OPF.D1R)] = range(pluck ? 8 : 2, pluck ? 20 : 14);
    v[o(OPF.D1L)] = car ? range(pluck ? 4 : 10, 15) : range(3, 14);
    v[o(OPF.D2R)] = pluck ? range(2, 10) : rnd() < 0.6 ? 0 : range(1, 6);
    v[o(OPF.RR)] = range(car ? 4 : 5, 11);
    v[o(OPF.OUT)] = car ? range(84, 99) : range(carriers.length === 4 ? 0 : 45, 86);
    v[o(OPF.CRS)] = car && rnd() < 0.7 ? 4 : pick(RATIO_CRS);
    v[o(OPF.DET)] = rnd() < 0.6 ? 3 : range(1, 5);
    v[o(OPF.KVS)] = car ? range(0, 3) : range(1, 5);
    v[o(OPF.LS)] = range(0, 30);
    v[o(OPF.RS)] = range(0, 2);
    v[o(OPF.EBS)] = 0;
    v[o(OPF.AME)] = 0;
    v[a(ACF.FIX)] = 0;
    v[a(ACF.FIXRG)] = 0;
    v[a(ACF.FINE)] = rnd() < 0.85 ? 0 : range(1, 7);
    v[a(ACF.OSW)] = rnd() < 0.65 ? 0 : pick([1, 2, 3, 4, 6]);
    v[a(ACF.SHFT)] = 0;
  }
  if (carriers.length === 4) for (const op of [2, 3, 4]) v[opOffset(op, OPF.OUT)] = range(60, 85);
  v[V.LFS] = range(20, 45); v[V.LFD] = range(0, 40); v[V.PMD] = rnd() < 0.5 ? 0 : range(2, 12);
  v[V.AMD] = 0; v[V.LFW] = pick([0, 2, 2, 2]); v[V.SYNC] = 1; v[V.PMS] = range(0, 4); v[V.AMS] = 0;
  v[FX.PRESET] = pick([0, 1, 2, 3, 4, 6, 7]);
  v[FX.TIME] = range(8, Math.min(30, effectTimeMax(v[FX.PRESET])));
  v[FX.BALANCE] = v[FX.PRESET] ? range(15, 45) : 0;
  v[VOICE.OPMASK] = 15;
  const name = `RANDOM ${String(range(0, 999)).padStart(3, "0")}`;
  for (let i = 0; i < 10; i++) v[V.NAME + i] = name.charCodeAt(i);
  return v;
}

/** Bounded undo / redo of 130-byte snapshots. */
export class History {
  constructor(limit = 200) { this.limit = limit; this.past = []; this.future = []; }
  push(snapshot) {
    this.past.push(Uint8Array.from(snapshot)); this.future.length = 0;
    if (this.past.length > this.limit) this.past.shift();
  }
  undo(current) { if (!this.past.length) return null; this.future.push(Uint8Array.from(current)); return this.past.pop(); }
  redo(current) { if (!this.future.length) return null; this.past.push(Uint8Array.from(current)); return this.future.pop(); }
  get canUndo() { return this.past.length > 0; }
  get canRedo() { return this.future.length > 0; }
}
