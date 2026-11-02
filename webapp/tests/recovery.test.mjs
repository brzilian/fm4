// SPDX-License-Identifier: GPL-3.0-only
// Full-flash dump checker (js/recovery.js, tools/fm1_dump_check.mjs) on synthetic FM-1 dumps built
// from the real V15 package.
import test from "node:test";
import assert from "node:assert/strict";
import { writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { read } from "./helpers.mjs";
import { analyzeDump, findFm4Records, formatReport, flashOfPackage, FLASH_BYTES, VM_START } from "../js/recovery.js";
import { makeFwsc } from "./fwsc.mjs";

globalThis.location ??= { href: "http://localhost/", origin: "http://localhost" };
await import("../vendor/fm1_open/WebUpdate.js");
const W = globalThis.FM1Web;
const STOCK = flashOfPackage(W.inspectPackage(read("firmware/FM-1_015.fwsc"), true), true);
const OTA = read("../firmware/sdk/tools/pack/ota.bin");
const FM4 = flashOfPackage(W.inspectPackage(makeFwsc({ loader: OTA, flashSize: 0x70000, seed: 3 })));

/** A healthy unit back on V15: V15 everywhere below 0x93000, factory settings sector, voices. */
function v15Unit() {
  const d = new Uint8Array(FLASH_BYTES).fill(0xFF);
  d.set(STOCK, 0);
  d.set([0x55, 0xAA, 0xAA, 0x55, 1, 2, 3, 4], VM_START);
  for (let a = 0xEA000; a < 0xFC000; a++) d[a] = (a * 7) & 255;
  return d;
}
/** An FM-4 record header as fm4_core.c writes it ({magic, version, bytes, crc}). */
function putRecord(d, addr, magic, version, bytes) {
  const h = new DataView(d.buffer, addr, 16);
  h.setUint32(0, magic, true); h.setUint32(4, version, true); h.setUint32(8, bytes, true); h.setUint32(12, 0x12345678, true);
  for (let i = 16; i < Math.min(16 + bytes, 0x9000); i++) d[addr + i] = i & 255;
}
const SONG = 0x47344D46, SYS = 0x53344D46;

test("flash.bin extraction matches both package layouts", () => {
  assert.equal(STOCK.length, 0x93000);
  assert.equal(FM4.length, 0x70000);
});

test("a healthy V15 unit: nothing to do", () => {
  const r = analyzeDump(v15Unit(), { stock: STOCK, fm4: FM4 });
  assert.equal(r.verdict, "v15");
  assert.deepEqual(r.actions, []); assert.deepEqual(r.commands, []);
  assert.ok(r.findings.every((f) => f.level !== "bad"), formatReport(r));
});

test("FM-4 1.0 songs 3 and 6 in stock's settings store: exact erase commands, settings header flagged", () => {
  const d = v15Unit();
  // FM-4 1.0: user offset 0x5000 + n * 0x9000, partition at 0x7C000 -> song 3 at 0x93000, song 6 at 0xAE000
  putRecord(d, 0x7C000 + 0x5000 + 2 * 0x9000, SONG, 1, 32796);
  putRecord(d, 0x7C000 + 0x5000 + 5 * 0x9000, SONG, 1, 32796);
  const recs = findFm4Records(d);
  assert.deepEqual(recs.map((r) => [r.addr, r.end, r.legacySong, r.inStockData]), [[0x93000, 0x9C000, true, true], [0xAE000, 0xB7000, true, true]]);
  const r = analyzeDump(d, { stock: STOCK, fm4: FM4 });
  assert.equal(r.verdict, "v15");
  assert.deepEqual(r.commands.slice(0, 2), ["erase 0x93000 0x9000", "erase 0xAE000 0x9000"]);
  assert.ok(r.findings.some((f) => f.level === "bad" && /most likely reason/.test(f.text)), formatReport(r));
  assert.ok(r.commands.some((c) => /last resort only: erase 0x93000 0x56000/.test(c)));
});

test("an interrupted stock write and a never-written one are told apart", () => {
  const partial = v15Unit(); partial.fill(0xFF, 0x50000, 0x93000);
  assert.equal(analyzeDump(partial, { stock: STOCK, fm4: FM4 }).verdict, "partial");
  const fm4 = v15Unit(); fm4.set(FM4, 0);
  const r = analyzeDump(fm4, { stock: STOCK, fm4: FM4 });
  assert.equal(r.verdict, "fm4");
  assert.match(r.actions[0], /fm1_unbrick\.py restore FM-1\.fwsc/);
});

test("FM-4 layout-2 data below 0x93000 is harmless; a wrong-size file is refused", () => {
  const d = v15Unit(); d.set(FM4, 0);
  putRecord(d, 0x7C000, SYS, 1, 200);
  putRecord(d, 0x7C000 + 0x5000, SONG, 2, 1000);
  const r = analyzeDump(d, { stock: STOCK, fm4: FM4 });
  assert.equal(r.records.length, 2);
  assert.ok(r.records.every((x) => !x.inStockData));
  assert.ok(!r.commands.some((c) => c.startsWith("erase")));
  assert.equal(analyzeDump(new Uint8Array(1000), {}).verdict, "notadump");
});

test("command-line tool", async () => {
  const { main } = await import("../tools/fm1_dump_check.mjs");
  const dir = mkdtempSync(join(tmpdir(), "fm1dump-"));
  const d = v15Unit(); putRecord(d, 0x93000, SONG, 1, 32796);
  writeFileSync(join(dir, "dump.bin"), d);
  const log = console.log; let out = "";
  console.log = (s) => { out += s + "\n"; };
  try { assert.equal(await main([join(dir, "dump.bin")]), 1); } finally { console.log = log; }
  assert.match(out, /erase 0x93000 0x9000/);
});
