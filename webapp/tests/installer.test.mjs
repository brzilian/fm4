// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// Modified by FM-4 contributors, 2026-10-31: after ORBIT's test_installer.mjs (FakeDocument,
// mount() injection, "no UPGRADE before consent" assertion), rewritten for the FM-4 installer: the
// real fm1_open FM1Web.Updater drives FakeFM2 through every route (FM-4 update, stock migration,
// Felucca migration, fm1_open, loader resume, stock restore) with fault injection.
//
// (c) installer tests. Run: node --test tests/*.test.mjs

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { WEBAPP, read, codec as makeCodec, randomVoice, eq } from "./helpers.mjs";
import { FakeFM2 } from "./fakefm2.mjs";
import { makeFwsc } from "./fwsc.mjs";
import { mount, IDS, classify, describeError, codeOf, progressOf, ERRORS, fm4CanPrepare, stockRegionOk } from "../js/installer.js";
import { parseSyx } from "../js/syx.js";
import { readZip } from "../js/zip.js";
import { BACKUP_IDS, readBackup } from "../vendor/felucca/fm1backup.js";
import * as felucca from "../vendor/felucca/fm1backup.js";

/* ---------------------------------------------------------------- FM1Web in Node */
globalThis.location = { href: "http://localhost/", origin: "http://localhost" };
await import("../vendor/fm1_open/WebUpdate.js");
await import("../vendor/fm1_open/I18n.js");
const FM1Web = globalThis.FM1Web;
const sha = (b) => createHash("sha256").update(b).digest("hex");
const OTA = new Uint8Array(readFileSync(join(WEBAPP, "../firmware/sdk/tools/pack/ota.bin")));
const STOCK = read("firmware/FM-1_015.fwsc");
const BUILD = "4d".repeat(32), OLD = "11".repeat(32);
const FM4 = makeFwsc({ product: "FM-1_8404100", loader: OTA, flashSize: 0x4000 });
const stockMeta = JSON.parse(readFileSync(join(WEBAPP, "firmware/fm4.json"), "utf8")).stock;
const fm4Meta = { schema: 1, product: "FM-1_8404100", profile: "dx7", family: 7, release_version: "1.0", build_id: BUILD,
  sha256: sha(FM4), bytes: FM4.length, loader_sha256: sha(OTA), hardware_validated: false, url: "fm4.fwsc", stock: stockMeta };
const FILES = { "fm4.fwsc": FM4, "FM-1_015.fwsc": STOCK };
const net = { meta: fm4Meta };
globalThis.fetch = async (url) => {
  const u = new URL(url), name = u.pathname.split("/").pop();
  if (name === "fm4.json") return net.meta ? new Response(JSON.stringify(net.meta)) : new Response("not found", { status: 404 });
  if (FILES[name]) return new Response(FILES[name], { headers: { "Content-Length": String(FILES[name].length) } });
  return new Response("not found", { status: 404 });
};
const image = (bytes, stock = false) => FM1Web.inspectPackage(bytes, stock).image;
const FM4_IMAGE = image(FM4), STOCK_IMAGE = image(STOCK, true);
const factory = FM1Web.inspectPackage(STOCK, true).factory;
const codec = await makeCodec();
const TIMING = { settle: 15, reconnect: 3000, idle: 2500, writeIdle: 2500, ext: 300 };

/* ---------------------------------------------------------------- fake document (after ORBIT) */
class El {
  constructor(id) { this.id = id; this.text = ""; this.history = []; this.disabled = false; this.hidden = false; this.value = ""; this.checked = false;
    this.attrs = {}; this.listeners = {}; this.open = false; }
  get textContent() { return this.text; }
  set textContent(v) { this.text = String(v); this.history.push(this.text); }
  addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }
  setAttribute(n, v) { this.attrs[n] = String(v); }
  removeAttribute(n) { delete this.attrs[n]; }
  getAttribute(n) { return n in this.attrs ? this.attrs[n] : null; }
  showModal() { this.open = true; }
  close() { this.open = false; }
  dispatch(t) { return Promise.all((this.listeners[t] || []).map((f) => f({ type: t, target: this }))); }
  click() { return this.disabled ? Promise.resolve() : this.dispatch("click"); }
  check(v = true) { this.checked = v; return this.dispatch("change"); }
  type(v) { this.value = v; return this.dispatch("input"); }
}
const PAGE = readFileSync(join(WEBAPP, "index.html"), "utf8");
const TAGS = [...PAGE.matchAll(/<(\w+)([^>]*)\sid="([^"]+)"([^>]*)>/g)].map((m) => ({ id: m[3], attrs: ` ${m[2]} ${m[4]} ` }));
class FakeDocument {
  constructor() {
    this.ids = {};
    for (const { id, attrs } of TAGS) { const e = new El(id); e.hidden = /\shidden[\s>]/.test(attrs); e.disabled = /\sdisabled[\s>]/.test(attrs); e.checked = /\schecked[\s>]/.test(attrs); this.ids[id] = e; }
    this.baseURI = "http://localhost/index.html";
    this.unload = [];
    this.defaultView = null;
  }
  getElementById(id) { return this.ids[id] || null; }
}
const ui = (doc) => Object.fromEntries(Object.entries(IDS).map(([k, id]) => [k, doc.ids[id]]));

async function page(device, extra = {}) {
  const doc = new FakeDocument(), u = ui(doc);
  const downloads = [], win = { listeners: new Set(), addEventListener: (t, f) => t === "beforeunload" && win.listeners.add(f), removeEventListener: (t, f) => win.listeners.delete(f) };
  const locks = [];
  const calls = [];
  const ctl = mount(doc, {
    FM1Web, felucca, timing: TIMING, secure: true, window: win,
    requestMIDIAccess: (o) => { calls.push(o); return Promise.resolve(device.access); },
    fetch: (u2, o) => globalThis.fetch(u2, o),
    wakeLock: async () => { const l = { released: false, release: async () => { l.released = true; } }; locks.push(l); return l; },
    download: async (name, bytes) => { downloads.push({ name, bytes: bytes.slice(), sentSoFar: device.sent.length }); },
    codec: async () => codec,
    ...extra,
  });
  await ctl.ready;
  return { doc, u, ctl, downloads, win, locks, calls };
}
const upgradesIn = (msgs) => msgs.filter((m) => m[0] === 0xF0 && m[1] === 0x22 && m[2] === 0x24 && m[3] === 0x35).length;
const armIn = (msgs) => msgs.filter((m) => { const p = FM1Web.parse(Uint8Array.from(m)); return p && p.command === 50; }).length;
const fm4Voices = () => Array.from({ length: 100 }, (_, i) => randomVoice(codec, 9000 + i));

/* ---------------------------------------------------------------- pure functions */
test("classify, error mapping and phases", async () => {
  const pkg = await FM1Web.prepare({ ...fm4Meta, url: "http://localhost/fm4.fwsc" });
  assert.equal(pkg.loaderIdentity, "ota-FM-1_015");
  assert.equal(classify({ identity: "FM-1_8404100", current: OLD, same: false }, pkg).kind, "fm4");
  assert.equal(classify({ identity: "FM-1_8000014", current: OLD, same: false }, pkg).kind, "open");
  assert.equal(classify({ identity: "FM-1_8404100", current: BUILD, same: true }, pkg).kind, "same");
  assert.deepEqual(classify({ identity: "FM-1_015", migration: true }, pkg), { kind: "stock", identity: "FM-1_015", force: true, migration: true });
  assert.equal(classify({ identity: "FM-1_920", migration: true }, pkg).kind, "felucca");
  assert.equal(classify({ identity: "FM-1_000", migration: true }, pkg).kind, "felucca");
  assert.equal(classify({ identity: "SMK-37 Pro_016", migration: true }, pkg).kind, "refuse");
  assert.equal(classify({ identity: "ota-FM-1_015", resume: true }, pkg).kind, "resume");
  assert.equal(classify({ identity: "ota-FM-1_904", resume: true }, pkg).kind, "foreign");
  assert.equal(classify({ identity: "ota-FM1C_0000000000000000", resume: true }, pkg).kind, "openloader", "FM-4 package: never forced");
  assert.equal(classify({ identity: "ota-FM1C_0000000000000000", migration: true }, pkg).kind, "openloader");
  assert.equal(codeOf(new Error("设备的升级阶段不匹配")), "stagemismatch");
  assert.equal(codeOf(new Error("USB 意外断开；重新连接同一设备后用此网页重试")), "lost");
  assert.equal(codeOf(new Error("设备已重启，但固件版本校验不一致")), "mismatch");
  assert.equal(codeOf(Object.assign(new Error("x"), { code: "AMBIGUOUS" })), "ambiguous");
  const d = describeError(new Error("设备停止请求数据，升级未完成"), (s) => FM1I18n.t(s, "en"));
  assert.equal(d.code, "stopped"); assert.equal(d.title, ERRORS.stopped[0]);
  assert.deepEqual(progressOf("写入固件", 42).value, 42);
  assert.equal(progressOf("等待升级设备重新连接", 0).value, null);
});

test("no FM-4 package published: the page says so, Install FM-4 is unavailable, stock restore stays", async () => {
  net.meta = { schema: 1, available: false, stock: stockMeta };
  try {
    const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_015" });
    const { u, ctl } = await page(dev);
    assert.equal(ctl.state.pkgs.fm4, null);
    assert.ok(ctl.state.pkgs.stock);
    assert.match(u.pkg.textContent, /No FM-4 firmware package is published/);
    assert.equal(u.modeFm4.disabled, true);
    assert.equal(ctl.state.mode, "stock");
    assert.equal(u.connect.disabled, false);
  } finally { net.meta = fm4Meta; }
});

/* ---------------------------------------------------------------- FM-4 -> FM-4 update */
test("FM-4 device: INFO, mandatory 100-voice backup, confirm, write, verify", async () => {
  const voices = fm4Voices();
  const dropRead = new Set([7]);
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD, finalIdentity: "FM-1_8404100", fm4: { codec, voices, dropRead } });
  const { u, ctl, downloads, win, locks, calls } = await page(dev, { timing: { ...TIMING, ext: 250 } });
  assert.equal(dev.sent.length, 0, "loading the page sends nothing");
  assert.match(u.pkg.textContent, /FM-4 1.0 · FM-1_8404100 · build 4d4d4d4d4d4d/);
  await u.connect.click();
  assert.deepEqual(calls, [{ sysex: true }]);
  assert.match(u.device.textContent, /FM-1_8404100: FM-4, installed build 111111111111 \(FM-4 1.1 TQ5\)/);
  assert.equal(u.install.disabled, true, "Install is locked before the backup");
  assert.equal(u.backupBox.hidden, false); assert.equal(u.backupRun.hidden, false);
  await u.install.click(); await ctl.install();
  assert.equal(upgradesIn(dev.sent) + armIn(dev.sent), 0, "nothing starts an update before backup + confirm");
  await u.backupRun.click();
  assert.equal(downloads.length, 1, u.errorDetail.textContent);
  assert.match(downloads[0].name, /^FM-4-user-voices-\d{4}-\d\d-\d\d\.syx$/);
  const back = parseSyx(downloads[0].bytes, codec);
  assert.equal(back.format, "tq5-100"); assert.equal(back.voices.length, 100);
  back.voices.forEach((v, i) => assert.ok(eq(v.record, codec.voiceToRecord(voices[i])), `voice ${i}`));
  assert.equal(dropRead.size, 0, "the lost READ reply was retried");
  assert.match(u.backupStatus.textContent, /verified/);
  assert.equal(u.install.disabled, false);
  assert.equal(upgradesIn(dev.sent) + armIn(dev.sent), 0, "backup alone sends no update command");
  await u.install.click();
  assert.equal(u.confirm.open, true);
  assert.equal(u.confirmTypedBox.hidden, true, "a same-family update needs no typed confirmation");
  assert.match(u.confirmFrom.textContent, /FM-1_8404100 build 111111111111/);
  assert.equal(upgradesIn(dev.sent), 0, "opening the dialog sends nothing");
  const before = dev.sent.length;
  let guardedDuringWrite = false, lockDuringWrite = false;
  dev.onStage2 = () => { guardedDuringWrite = win.listeners.size === 1; lockDuringWrite = locks.length === 1 && !locks[0].released; };
  await u.confirmGo.click();
  assert.equal(u.error.hidden, true, `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.equal(upgradesIn(dev.sent.slice(0, before)), 0);
  assert.equal(dev.arms.length, 1); assert.equal(dev.arms[0].tag, BUILD); assert.equal(dev.arms[0].family, 7); assert.equal(dev.arms[0].length, FM4_IMAGE.length);
  assert.equal(dev.upgrades, 2); assert.equal(dev.bad, 0);
  assert.ok(guardedDuringWrite, "beforeunload guard while writing"); assert.ok(lockDuringWrite, "wake lock while writing");
  assert.equal(win.listeners.size, 0); assert.equal(locks[0].released, true);
  assert.equal(ctl.state.phase, "done");
  assert.equal(u.progress.getAttribute("value"), "100");
  assert.match(u.phase.textContent, /FM-4 installed and verified: build 4d4d4d4d4d4d/);
  assert.ok(u.phase.history.some((t) => /^Step 1 of 2/.test(t)) && u.phase.history.some((t) => /^Step 2 of 2: writing the firmware.*: \d+%/.test(t)), "phases shown");
});

test("the same build already installed is refused before anything is sent", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: BUILD, fm4: { codec, voices: fm4Voices() } });
  const { u } = await page(dev);
  await u.connect.click();
  assert.equal(u.error.getAttribute("data-code"), "same");
  assert.equal(u.install.disabled, true);
  assert.equal(upgradesIn(dev.sent) + armIn(dev.sent), 0);
});

test("a wrong build after reboot is not declared a success", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD, finalIdentity: "FM-1_8404100", finalBuild: OLD, fm4: { codec, voices: fm4Voices() } });
  const { u, ctl } = await page(dev);
  await u.connect.click(); await u.backupRun.click(); await u.install.click(); await u.confirmGo.click();
  assert.equal(u.error.getAttribute("data-code"), "mismatch");
  assert.notEqual(ctl.state.phase, "done");
});

/* ---------------------------------------------------------------- stock V15 -> FM-4 migration */
test("stock V15: acknowledgement gate, typed INSTALL, legacy route, unplug mid-write, resume", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_015", finalIdentity: "FM-1_8404100", finalBuild: BUILD,
    unplugAt: { stage: 2, after: 3 } });
  const { u, ctl } = await page(dev);
  await u.connect.click();
  assert.match(u.device.textContent, /FM-1_015: M-VAVE stock firmware V15/);
  assert.equal(u.ackBox.hidden, false); assert.equal(u.backupRun.hidden, true);
  assert.match(u.backupText.textContent, /cannot be backed up over MIDI/);
  assert.equal(u.install.disabled, true, "locked until the Unbricker acknowledgement");
  await ctl.install();
  assert.equal(upgradesIn(dev.sent), 0);
  await u.ack.check(true);
  assert.equal(u.install.disabled, false);
  await u.ack.check(false);
  assert.equal(u.install.disabled, true, "unticking locks it again");
  await u.ack.check(true);
  await u.install.click();
  assert.equal(u.confirmTypedBox.hidden, false); assert.equal(u.confirmGo.disabled, true);
  await u.confirmTyped.type("install");
  assert.equal(u.confirmGo.disabled, true, "lowercase does not count");
  await ctl.install();
  assert.equal(upgradesIn(dev.sent), 0, "install() without the typed word sends nothing");
  await u.confirmTyped.type("INSTALL");
  assert.equal(u.confirmGo.disabled, false);
  await u.confirmGo.click();
  // the device was unplugged after 3 stage-2 reads
  assert.equal(u.error.getAttribute("data-code"), "lost", `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.match(u.errorRecovery.textContent, /Resume/);
  assert.equal(dev.arms.length, 0, "stock has no arm command");
  assert.notEqual(ctl.state.phase, "done");
  // replug: the vendor loader is still there
  dev.replug("ota-FM-1_015");
  await u.connect.click();
  assert.match(u.device.textContent, /vendor update loader/);
  assert.equal(u.haveBox.hidden, false);
  assert.equal(u.install.disabled, true);
  await u.have.check(true);
  assert.equal(u.install.textContent, "Resume install…");
  await u.install.click();
  assert.equal(u.confirmTypedBox.hidden, false, "resume needs the typed word");
  await u.confirmTyped.type("INSTALL");
  await u.confirmGo.click();
  assert.equal(u.error.hidden, true, `${u.errorTitle.textContent} ${u.errorDetail.textContent} ${ctl.state.log.slice(-6).join(" | ")}`);
  assert.equal(ctl.state.phase, "done");
  assert.equal(dev.identity, "FM-1_8404100"); assert.equal(dev.bad, 0);
});

test("unplug during stage 1 leaves the old firmware; reconnect shows it again", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD, finalIdentity: "FM-1_8404100", fm4: { codec, voices: fm4Voices() }, unplugAt: { stage: 1, after: 1 } });
  const { u, ctl } = await page(dev);
  await u.connect.click(); await u.backupRun.click(); await u.install.click(); await u.confirmGo.click();
  assert.equal(u.error.getAttribute("data-code"), "lost");
  assert.equal(ctl.state.backup, null, "a new attempt needs a new backup");
  dev.replug("FM-1_8404100");
  await u.connect.click();
  assert.match(u.device.textContent, /FM-4, installed build 111111111111/);
  assert.equal(u.install.disabled, true);
});

/* ---------------------------------------------------------------- refusals */
test("a foreign loader, an unknown firmware and two devices are refused without sending", async () => {
  for (const [identity, code] of [["ota-FM-1_904", "foreign"], ["SMK-37 Pro_016", "refused"]]) {
    const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity });
    const { u } = await page(dev);
    await u.connect.click();
    assert.equal(u.error.getAttribute("data-code"), code, identity);
    assert.equal(u.install.disabled, true);
    assert.equal(upgradesIn(dev.sent), 0);
  }
  const a = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD });
  const b = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD, access: a.access, name: "FM-1 2" });
  const { u } = await page(a);
  await u.connect.click();
  assert.equal(u.error.getAttribute("data-code"), "ambiguous");
  assert.equal(upgradesIn(a.sent) + upgradesIn(b.sent), 0);
});

test("a device with an update in progress (state != 0) is refused", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD, state: 1 });
  const { u } = await page(dev);
  await u.connect.click();
  assert.equal(u.error.getAttribute("data-code"), "busydevice");
});

/* ---------------------------------------------------------------- Felucca migration */
test("Felucca family: archive backup with read-back, typed confirm, legacy install", async () => {
  const rnd = (n, s) => Uint8Array.from({ length: n }, (_, i) => (i * 131 + s * 17 + (i >> 3)) & 255);
  const objects = new Map([[0, rnd(3388, 2)], [1, rnd(1200, 3)], [2, rnd(3388, 4)]]);
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_920", finalIdentity: "FM-1_8404100", finalBuild: BUILD, felucca: { objects } });
  const { u, ctl, downloads } = await page(dev);
  await u.connect.click();
  assert.match(u.device.textContent, /Felucca-family/);
  await u.backupRun.click();
  assert.equal(downloads.length, 1, u.errorDetail.textContent);
  const archive = readBackup(new TextDecoder().decode(downloads[0].bytes));
  assert.equal(archive.objects.length, BACKUP_IDS.length);
  assert.ok(eq(archive.objects[2].bytes, objects.get(2)));
  assert.equal(upgradesIn(dev.sent), 0);
  await u.install.click();
  assert.equal(u.confirmTypedBox.hidden, false);
  await u.confirmTyped.type("INSTALL"); await u.confirmGo.click();
  assert.equal(u.error.hidden, true, `${u.errorTitle.textContent} ${u.errorDetail.textContent} ${ctl.state.log.slice(-6).join(" | ")}`);
  assert.equal(ctl.state.phase, "done");
});

test("a Felucca version without archive support blocks the backup and install", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_904", felucca: { objects: new Map(), noArchive: true } });
  const { u, downloads } = await page(dev);
  await u.connect.click(); await u.backupRun.click();
  assert.equal(u.error.getAttribute("data-code"), "backup");
  assert.equal(downloads.length, 0); assert.equal(u.install.disabled, true);
});

/* ---------------------------------------------------------------- fm1_open -> FM-4 */
test("fm1_open device: Library backup (zip) re-parsed before install", async () => {
  const project = new Uint8Array(4064); project[3852] = 4; for (let i = 0; i < 3852; i++) project[i] = (i * 7) & 255;
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8000014", build: OLD, finalIdentity: "FM-1_8404100", library: { project, slots: new Set([0, 3]) } });
  const { u, ctl, downloads } = await page(dev);
  await u.connect.click();
  assert.match(u.device.textContent, /fm1_open firmware/);
  await u.backupRun.click();
  assert.equal(downloads.length, 1, `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  const files = readZip(downloads[0].bytes);
  assert.deepEqual(files.map((f) => f.name).filter((n) => n.endsWith(".fm1p")), ["workspace.fm1p", "project-01.fm1p", "project-04.fm1p"]);
  assert.ok(eq(FM1Web.readProjectFile(files[0].data), project));
  await u.install.click(); await u.confirmGo.click();
  assert.equal(ctl.state.phase, "done", `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.equal(dev.arms[0].tag, BUILD);
});

/* ---------------------------------------------------------------- restore stock V15 */
test("restore stock V15 from FM-4: backup, typed confirm, write, factory voices", async () => {
  const dev = new FakeFM2(FM1Web, STOCK_IMAGE, { identity: "FM-1_8404100", build: BUILD, finalIdentity: "FM-1_015", fm4: { codec, voices: fm4Voices() } });
  const { u, ctl, downloads } = await page(dev);
  await u.modeStock.check(true);
  assert.equal(ctl.state.mode, "stock");
  await u.connect.click();
  await u.backupRun.click();
  assert.equal(downloads.length, 1);
  assert.equal(u.install.textContent, "Restore stock V15…");
  await u.install.click();
  assert.match(u.confirmTo.textContent, /stock V15/);
  await u.confirmTyped.type("INSTALL"); await u.confirmGo.click();
  assert.equal(ctl.state.phase, "done", `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.equal(dev.identity, "FM-1_015");
  assert.equal(dev.factoryWrites, 32);
  assert.ok(eq(dev.factory, factory), "the four factory banks match the pinned V15 data");
  assert.match(u.device.textContent, /Reset Patches/);
  assert.equal(dev.prepares, 1, "PREPARE_STOCK sent once");
  assert.ok(dev.events.indexOf("prepare") < dev.events.findIndex((e) => e.startsWith("upgrade")), "prepared before the update starts");
  assert.match(u.backupStatus.textContent, /none of its data is in stock V15's settings area/);
});

test("FM-4 -> FM-4 update never sends PREPARE_STOCK", async () => {
  const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: "FM-1_8404100", build: OLD, finalIdentity: "FM-1_8404100", fm4: { codec, voices: fm4Voices() } });
  const { u, ctl } = await page(dev);
  await u.connect.click(); await u.backupRun.click(); await u.install.click(); await u.confirmGo.click();
  assert.equal(ctl.state.phase, "done", `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.equal(dev.prepares ?? 0, 0);
  assert.equal(u.oldFm4Box.hidden, true);
});

test("restore stock: FM-4 reports leftover data -> blocked, nothing written", async () => {
  const dev = new FakeFM2(FM1Web, STOCK_IMAGE, { identity: "FM-1_8404100", build: BUILD, finalIdentity: "FM-1_015",
    fm4: { codec, voices: fm4Voices(), prepare: { status: 1, erased: 9, remaining: 2 } } });
  const { u, ctl } = await page(dev);
  await u.modeStock.check(true);
  await u.connect.click(); await u.backupRun.click();
  assert.equal(u.error.getAttribute("data-code"), "prepare");
  assert.match(u.errorDetail.textContent, /2 FM-4 record/);
  assert.equal(u.install.disabled, true);
  assert.notEqual(ctl.state.phase, "backedup");
  assert.equal(upgradesIn(dev.sent) + armIn(dev.sent), 0);
});

test("restore stock from FM-4 1.0: blocked until the user confirms songs 3-8 were never saved", async () => {
  const dev = new FakeFM2(FM1Web, STOCK_IMAGE, { identity: "FM-1_8404100", build: BUILD, finalIdentity: "FM-1_015",
    fm4: { codec, voices: fm4Voices(), version: "1.0" } });
  const { u, ctl, downloads } = await page(dev);
  await u.modeStock.check(true);
  await u.connect.click();
  assert.match(u.device.textContent, /FM-4 1\.0 TQ5/);
  assert.equal(u.oldFm4Box.hidden, false, "the 1.0 warning is shown");
  await u.backupRun.click();
  assert.equal(u.error.getAttribute("data-code"), "oldfm4");
  assert.equal(downloads.length, 0); assert.equal(u.install.disabled, true);
  await u.oldFm4.check(true);
  await u.backupRun.click();
  assert.equal(downloads.length, 1, u.errorDetail.textContent);
  assert.equal(dev.prepares ?? 0, 0, "1.0 has no PREPARE_STOCK; nothing is sent for it");
  assert.match(u.backupStatus.textContent, /never saved/);
  await u.install.click(); await u.confirmTyped.type("INSTALL"); await u.confirmGo.click();
  assert.equal(ctl.state.phase, "done", `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
});

test("FM-4 version gate and the V15 firmware-region pin", async () => {
  assert.equal(fm4CanPrepare("FM-4 1.0 TQ5"), false);
  assert.equal(fm4CanPrepare("FM-4 1.1 TQ5"), true);
  assert.equal(fm4CanPrepare("FM-4 2.0 TQ5"), true);
  assert.equal(fm4CanPrepare(undefined), false);
  const pkg = FM1Web.inspectPackage(STOCK, true);
  assert.equal(await stockRegionOk(pkg), true);
  const bad = { ...pkg, image: pkg.image.slice() }; bad.image[1024 + 0x50000] ^= 1;
  assert.equal(await stockRegionOk(bad), false);
});

test("an fm1_open loader (ota-FM1C_…) is finished only by force-resuming stock V15", async () => {
  const id = "ota-FM1C_0000000000000000";
  // FM-4 mode: refused, nothing sent
  {
    const dev = new FakeFM2(FM1Web, FM4_IMAGE, { identity: id });
    const { u } = await page(dev);
    await u.connect.click();
    assert.equal(u.error.getAttribute("data-code"), "openloader");
    assert.match(u.errorRecovery.textContent, /Restore stock V15/);
    assert.equal(upgradesIn(dev.sent), 0);
  }
  // stock mode: acknowledgement + typed INSTALL, stage-2 write, V15 verified, factory voices
  const dev = new FakeFM2(FM1Web, STOCK_IMAGE, { identity: id, finalIdentity: "FM-1_015" });
  const { u, ctl } = await page(dev);
  await u.modeStock.check(true);
  await u.connect.click();
  assert.equal(u.error.hidden, true, `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.match(u.device.textContent, /fm1_open update loader/);
  assert.equal(u.haveBox.hidden, false);
  assert.equal(u.install.disabled, true, "locked until the box is ticked");
  await u.have.check(true);
  assert.equal(u.install.textContent, "Force-resume with stock V15…");
  await u.install.click();
  assert.equal(u.confirmTypedBox.hidden, false);
  await ctl.install();
  assert.equal(upgradesIn(dev.sent), 0, "nothing without the typed word");
  await u.confirmTyped.type("INSTALL"); await u.confirmGo.click();
  assert.equal(ctl.state.phase, "done", `${u.errorTitle.textContent} ${u.errorDetail.textContent} ${ctl.state.log.slice(-6).join(" | ")}`);
  assert.equal(dev.identity, "FM-1_015"); assert.equal(dev.bad, 0);
  assert.equal(dev.arms.length, 0, "no arm command to a loader");
  assert.equal(dev.factoryWrites, 32);
});

test("restore stock on a device already running V15 restores only the factory voices", async () => {
  const dev = new FakeFM2(FM1Web, STOCK_IMAGE, { identity: "FM-1_015" });
  const { u, ctl } = await page(dev);
  await u.modeStock.check(true);
  await u.connect.click();
  await u.ack.check(true);
  await u.install.click(); await u.confirmTyped.type("INSTALL"); await u.confirmGo.click();
  assert.equal(ctl.state.phase, "done", `${u.errorTitle.textContent} ${u.errorDetail.textContent}`);
  assert.equal(dev.upgrades, 0); assert.equal(dev.factoryWrites, 32);
});

test("vendored files are byte-identical to their upstream sources", () => {
  const pairs = [["vendor/fm1_open/WebUpdate.js", "../firmware/sdk/WebUpdate.js"], ["vendor/fm1_open/I18n.js", "../firmware/sdk/I18n.js"],
    ["vendor/fm1_open/LICENSE.md", "../firmware/LICENSE.md"], ["vendor/fm1_open/THIRD_PARTY.md", "../firmware/THIRD_PARTY.md"],
    ["firmware/FM-1_015.fwsc", "../firmware/sdk/firmware/FM-1_015.fwsc"]];
  try { readdirSync("/root/fm4/Felucca/web"); pairs.push(["vendor/felucca/fm1backup.js", "/root/fm4/Felucca/web/fm1backup.js"]); } catch { /* not checked out here */ }
  for (const [a, b] of pairs) assert.ok(eq(read(a), new Uint8Array(readFileSync(b.startsWith("/") ? b : join(WEBAPP, b)))), a);
  assert.match(readFileSync(join(WEBAPP, "vendor/fm1_open/WebUpdate.js"), "utf8").split("\n")[0], /SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception/);
});
