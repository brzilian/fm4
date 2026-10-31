// SPDX-License-Identifier: GPL-3.0-only
// (d) tools/make_meta.mjs on the real stock file, and on a synthetic Open-style package.
import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, readFileSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { WEBAPP, read } from "./helpers.mjs";
import { makeMeta, stockEntry, checkWithPrepare, loadFM1Web, STOCK_SHA256 } from "../tools/make_meta.mjs";
import { makeFwsc } from "./fwsc.mjs";

const stock = read("firmware/FM-1_015.fwsc");
const ota = new Uint8Array(readFileSync(join(WEBAPP, "../firmware/sdk/tools/pack/ota.bin")));
const sha = (b) => createHash("sha256").update(b).digest("hex");

test("stock entry from the real FM-1_015.fwsc", async () => {
  const s = await stockEntry(stock, ota);
  assert.deepEqual(s, { schema: 1, kind: "stock", product: "FM-1_015", profile: "M-VAVE Original V15", family: 7, release_version: "V15",
    build_id: STOCK_SHA256, sha256: STOCK_SHA256, bytes: 699956, loader_sha256: "ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541", url: "FM-1_015.fwsc" });
  const FM1Web = await loadFM1Web();
  const { url, ...rest } = s;
  const pkg = await FM1Web.prepare({ ...rest, data: Buffer.from(stock).toString("base64") });
  assert.equal(pkg.loaderIdentity, "ota-FM-1_015");
  assert.equal(pkg.factory.length, 16384);
});

test("the shipped fm4.json matches what make_meta writes", async () => {
  const shipped = JSON.parse(readFileSync(join(WEBAPP, "firmware/fm4.json"), "utf8"));
  const fresh = await makeMeta({ fwscBytes: null, stockBytes: stock, otaBytes: ota });
  if (shipped.product) assert.equal(shipped.stock.sha256, STOCK_SHA256);
  else assert.deepEqual(shipped, fresh);
});

test("a tampered stock file is refused", async () => {
  const bad = stock.slice(); bad[5000] ^= 1;
  await assert.rejects(stockEntry(bad, ota), /not the pinned/);
});

test("an FM-4 package: fields, prepare() acceptance, CLI output", async () => {
  const fwsc = makeFwsc({ product: "FM-1_8404100", loader: ota });
  const identity = { profile: "dx7", family: "dx7", release_version: "1.0", build_id: "ab".repeat(32) };
  const meta = await makeMeta({ fwscBytes: fwsc, identity, stockBytes: stock, otaBytes: ota });
  assert.equal(meta.schema, 1); assert.equal(meta.product, "FM-1_8404100"); assert.equal(meta.family, 7); assert.equal(meta.profile, "dx7");
  assert.equal(meta.sha256, sha(fwsc)); assert.equal(meta.bytes, fwsc.length); assert.equal(meta.loader_sha256, sha(ota));
  assert.equal(meta.url, "fm4.fwsc"); assert.equal(meta.stock.kind, "stock"); assert.equal(meta.stock.url, "FM-1_015.fwsc");
  const dir = mkdtempSync(join(tmpdir(), "fm4meta-"));
  writeFileSync(join(dir, "pkg.fwsc"), fwsc);
  writeFileSync(join(dir, "build-identity.json"), "﻿" + JSON.stringify(identity));   // PowerShell writes a BOM
  copyFileSync(join(WEBAPP, "firmware/FM-1_015.fwsc"), join(dir, "FM-1_015.fwsc"));
  const out = execFileSync(process.execPath, [join(WEBAPP, "tools/make_meta.mjs"), "--fwsc", join(dir, "pkg.fwsc"), "--identity", join(dir, "build-identity.json"), "--out", dir], { encoding: "utf8" });
  assert.match(out, /FM-1_8404100 1.0 build abababababab/);
  const written = JSON.parse(readFileSync(join(dir, "fm4.json"), "utf8"));
  assert.deepEqual(written, meta);
  const prepared = await checkWithPrepare(written, dir);
  assert.equal(prepared.fm4.loaderIdentity, "ota-FM-1_015");
  assert.equal(prepared.fm4.build_id, identity.build_id);
});

test("a package without the vendor loader, or a bad identity file, is refused", async () => {
  const other = ota.slice(); other[100] ^= 1;
  const fwsc = makeFwsc({ product: "FM-1_8404100", loader: other });
  await assert.rejects(makeMeta({ fwscBytes: fwsc, identity: { release_version: "1", build_id: "ab".repeat(32) }, stockBytes: stock, otaBytes: ota }), /vendor ota.bin/);
  await assert.rejects(makeMeta({ fwscBytes: makeFwsc({ loader: ota }), identity: { release_version: "1", build_id: "xyz" }, stockBytes: stock, otaBytes: ota }), /build_id/);
});
