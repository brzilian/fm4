#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Build webapp/firmware/fm4.json (schema of docs/update_protocol.md §5.2, as fm1_open Build.ps1
// emits it) from a CI firmware package and its build-identity.json, and copy the package next to it.
//
//   node tools/make_meta.mjs --fwsc FM-4-latest.fwsc --identity build-identity.json [--out firmware]
//   node tools/make_meta.mjs --stock-only [--out firmware]       (no FM-4 package published yet)
//
// Options: --stock <FM-1_015.fwsc> (default: <out>/FM-1_015.fwsc), --ota <ota.bin>
// (default: ../firmware/sdk/tools/pack/ota.bin), --print (write nothing, print the JSON).
// The result is checked with fm1_open's own FM1Web.inspectPackage() and prepare(), the same code
// the INSTALL tab runs, so a package that this tool accepts is one the page accepts.

import { createHash } from "node:crypto";
import { readFileSync, writeFileSync, copyFileSync, mkdirSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const WEBAPP = resolve(HERE, "..");
export const STOCK_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a";
export const PACKAGE_NAME = "fm4.fwsc";
export const STOCK_NAME = "FM-1_015.fwsc";
const sha256 = (b) => createHash("sha256").update(b).digest("hex");

export async function loadFM1Web() {
  if (!globalThis.FM1Web) {
    globalThis.location ??= { href: "http://localhost/", origin: "http://localhost" };
    await import(pathToFileURL(join(WEBAPP, "vendor/fm1_open/WebUpdate.js")).href);
  }
  return globalThis.FM1Web;
}

export async function stockEntry(stockBytes, otaBytes) {
  const FM1Web = await loadFM1Web();
  const sha = sha256(stockBytes);
  if (sha !== STOCK_SHA256) throw new Error(`stock file SHA-256 ${sha} is not the pinned M-VAVE V15 image`);
  const pkg = FM1Web.inspectPackage(new Uint8Array(stockBytes), true);
  const loader = sha256(otaBytes);
  if (sha256(pkg.loader) !== loader) throw new Error("the stock package's ota.bin differs from the vendor loader");
  return { schema: 1, kind: "stock", product: pkg.product, profile: "M-VAVE Original V15", family: 7, release_version: "V15",
    build_id: sha, sha256: sha, bytes: stockBytes.length, loader_sha256: loader, url: STOCK_NAME };
}

export async function makeMeta({ fwscBytes, identity, stockBytes, otaBytes }) {
  const FM1Web = await loadFM1Web();
  const stock = await stockEntry(stockBytes, otaBytes);
  if (!fwscBytes) return { schema: 1, available: false, stock };
  if (!identity || !/^[0-9a-f]{64}$/.test(identity.build_id || "")) throw new Error("build-identity.json needs a 64-hex build_id");
  if (typeof identity.release_version !== "string" || !identity.release_version) throw new Error("build-identity.json needs release_version");
  const pkg = FM1Web.inspectPackage(new Uint8Array(fwscBytes), false);
  const loader = sha256(otaBytes);
  if (sha256(pkg.loader) !== loader) throw new Error("the package does not carry the vendor ota.bin loader (loader_sha256 would not match)");
  return {
    schema: 1, product: pkg.product, profile: "dx7", family: 7, release_version: identity.release_version,
    build_id: identity.build_id, sha256: sha256(fwscBytes), bytes: fwscBytes.length, loader_sha256: loader,
    hardware_validated: false, url: PACKAGE_NAME, stock,
  };
}

/** Run prepare() exactly as the page does, with the files served from dir. */
export async function checkWithPrepare(meta, dir) {
  const FM1Web = await loadFM1Web();
  const asData = (m, file) => {
    const { url, ...rest } = m;
    return { ...rest, data: readFileSync(join(dir, file)).toString("base64") };
  };
  const out = { stock: await FM1Web.prepare(asData(meta.stock, meta.stock.url)) };
  if (meta.product) out.fm4 = await FM1Web.prepare(asData(meta, meta.url));
  return out;
}

function args(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) throw new Error(`unexpected argument ${a}`);
    const key = a.slice(2);
    if (["stock-only", "print"].includes(key)) o[key] = true;
    else o[key] = argv[++i];
  }
  return o;
}

async function main() {
  const o = args(process.argv.slice(2));
  const out = resolve(o.out || join(WEBAPP, "firmware"));
  const stockPath = resolve(o.stock || join(out, STOCK_NAME));
  const otaPath = resolve(o.ota || join(WEBAPP, "../firmware/sdk/tools/pack/ota.bin"));
  if (!o["stock-only"] && (!o.fwsc || !o.identity)) throw new Error("give --fwsc and --identity, or --stock-only");
  const meta = await makeMeta({
    fwscBytes: o["stock-only"] ? null : readFileSync(o.fwsc),
    identity: o["stock-only"] ? null : JSON.parse(readFileSync(o.identity, "utf8").replace(/^﻿/, "")),
    stockBytes: readFileSync(stockPath), otaBytes: readFileSync(otaPath),
  });
  const json = JSON.stringify(meta, null, 2) + "\n";
  if (o.print) { process.stdout.write(json); return; }
  mkdirSync(out, { recursive: true });
  if (resolve(stockPath) !== join(out, STOCK_NAME)) copyFileSync(stockPath, join(out, STOCK_NAME));
  if (meta.product) copyFileSync(o.fwsc, join(out, PACKAGE_NAME));
  await checkWithPrepare(meta, out);
  writeFileSync(join(out, "fm4.json"), json);
  console.log(meta.product
    ? `fm4.json: ${meta.product} ${meta.release_version} build ${meta.build_id.slice(0, 12)} (${meta.bytes} bytes), checked with prepare()`
    : "fm4.json: no FM-4 package (stock V15 restore only), checked with prepare()");
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((e) => { console.error(`make_meta: ${e.message}`); process.exit(1); });
}
