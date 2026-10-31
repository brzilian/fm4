// SPDX-License-Identifier: GPL-3.0-only
// Shared helpers for the FM-4 webapp tests (node:test, no dependencies).
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { Codec, VOICE } from "../js/codec.js";
import { mulberry32 } from "../js/model.js";

export const WEBAPP = join(dirname(fileURLToPath(import.meta.url)), "..");
export const read = (p) => new Uint8Array(readFileSync(join(WEBAPP, p)));
export const codec = () => Codec.create(read("fm4.wasm"));
export const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

/** A random voice with every field inside the TQ5 ranges (as the engine stores it). */
export function randomVoice(c, seed) {
  const r = mulberry32(seed), v = new Uint8Array(VOICE.SIZE);
  for (let i = 0; i < 93; i++) v[i] = Math.floor(r() * (c.vcedMax[i] + 1));
  for (let i = 77; i < 87; i++) v[i] = 32 + Math.floor(r() * 95);
  for (let i = 0; i < 23; i++) v[93 + i] = Math.floor(r() * (c.acedMax[i] + 1));
  for (let i = 0; i < 4; i++) v[116 + i] = Math.floor(r() * ([99, 99, 100, 99][i] + 1));
  v[126] = Math.floor(r() * 11); v[127] = Math.floor(r() * 76); v[128] = Math.floor(r() * 100);
  v[129] = 15;
  return c.setVoice(6, v);   // let the engine clamp the dependent ranges
}
