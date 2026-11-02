#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Check a full FM-1 flash dump (1 MiB) after a return from FM-4 to stock V15. Read-only; runs on
// macOS, Windows and Linux with Node 20+. The same check is in the webapp (Install > Recovery).
//
//   node tools/fm1_dump_check.mjs full-flash-before.bin [--stock FM-1_015.fwsc] [--fm4 fm4.fwsc]
//
// Defaults: the V15 package and FM-4 package bundled in webapp/firmware/. Exit code 0 when nothing
// FM-4-related is wrong, 1 when the report lists actions, 2 on bad input.

import { readFileSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { analyzeDump, formatReport, flashOfPackage } from "../js/recovery.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const WEBAPP = resolve(HERE, "..");

async function main(argv) {
  const args = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--stock" || argv[i] === "--fm4") args[argv[i].slice(2)] = argv[++i];
    else args._.push(argv[i]);
  }
  if (args._.length !== 1) { console.error("usage: node tools/fm1_dump_check.mjs DUMP.bin [--stock FM-1_015.fwsc] [--fm4 fm4.fwsc]"); return 2; }
  globalThis.location ??= { href: "http://localhost/", origin: "http://localhost" };
  await import(pathToFileURL(join(WEBAPP, "vendor/fm1_open/WebUpdate.js")).href);
  const W = globalThis.FM1Web;
  const stockPath = args.stock || join(WEBAPP, "firmware/FM-1_015.fwsc");
  const fm4Path = args.fm4 || join(WEBAPP, "firmware/fm4.fwsc");
  const stock = flashOfPackage(W.inspectPackage(new Uint8Array(readFileSync(stockPath)), true), true);
  const fm4 = existsSync(fm4Path) ? flashOfPackage(W.inspectPackage(new Uint8Array(readFileSync(fm4Path)), false)) : null;
  const report = analyzeDump(new Uint8Array(readFileSync(args._[0])), { stock, fm4 });
  console.log(formatReport(report));
  return report.verdict === "notadump" ? 2 : report.actions.length ? 1 : 0;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) process.exitCode = await main(process.argv.slice(2));
export { main };
