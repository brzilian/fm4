// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// Modified by FM-4 contributors, 2026-10-31: rewritten around fm1_open's FM1Web.Updater / prepare /
// inspectPackage (vendor/fm1_open/WebUpdate.js) for every protocol and package check; adds device
// classification (FM-4, fm1_open, stock V15, Felucca family, loader resume), a mandatory verified
// backup step, a typed confirmation, wake lock and the stock V15 restore. ORBIT's ERRORS /
// describeError table, progress mapping, createInstaller(deps) injection and mount(doc) pattern are
// kept; its package validation and preflight are not used (they reject the vendor loader FM-4 uses).
//
// FM-4 firmware installer: load metadata -> connect -> identify / classify -> backup (mandatory,
// verified) -> confirm -> write -> verify -> done. Nothing that starts an update (the F0 22 24 35 keys
// or the 0x32 arm) is sent before the backup step has passed and the user has confirmed.

import { FM4_PRODUCT_RE, USER_SLOTS } from "./fm4ext.js";
import { Fm4Device } from "./device.js";
import { parseSyx, bank100Syx } from "./syx.js";
import { makeZip, readZip } from "./zip.js";

export const UNBRICKER_URL = "https://github.com/Quixotic7/MvaveFM1Unbricker";
export const STOCK_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a";
export const VENDOR_LOADER_SHA256 = "ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541";
export const STOCK_BYTES = 699956;
/** SHA-256 of V15's firmware region (flash 0x4000..0x92FFF), the bytes MvaveFM1Unbricker writes. */
export const V15_REGION_SHA256 = "6edf3c37fb5bbbc33607c89375ee024d5477c17914d72221c8c68e58a8255686";
/** The FM-4 release whose storage stays out of stock V15's data and that answers PREPARE_STOCK. */
export const FM4_PREPARE_VERSION = [1, 1];
/** Used when firmware/fm4.json cannot be loaded: the stock restore still works from the embedded file. */
export const FALLBACK_STOCK = Object.freeze({ schema: 1, kind: "stock", product: "FM-1_015", profile: "M-VAVE Original V15", family: 7,
  release_version: "V15", build_id: STOCK_SHA256, sha256: STOCK_SHA256, bytes: STOCK_BYTES, loader_sha256: VENDOR_LOADER_SHA256, url: "FM-1_015.fwsc" });

const fail = (code, msg, detail = "") => Object.assign(new Error(msg), { code, detail });
const FELUCCA_ID = /^FM-1_(9\d\d|000)$/;
const hex8 = (s) => (s || "").slice(0, 12);

// error codes -> what happened, what to do next
export const ERRORS = {
  unsupported: ["This browser cannot install firmware.", "Use desktop Chrome or Edge on https:// or http://localhost, with Web MIDI available."],
  denied: ["MIDI access with SysEx was denied.", "Allow MIDI (with SysEx) for this site in the browser's site settings, then press Connect again."],
  notfound: ["No FM-1 answered.", "Use a USB data cable straight into the computer, close other MIDI apps and editor tabs, switch the FM-1 off and on, then press Connect."],
  ambiguous: ["More than one possible FM-1 is connected.", "Unplug every other MIDI device so that only one FM-1 is connected, then press Connect."],
  busyport: ["The FM-1's MIDI port could not be opened.", "Close other pages and programs that use MIDI, replug the USB cable, then press Connect."],
  refused: ["This device cannot be updated from this page.", "Nothing was sent. Return it to stock V15 with its own firmware's updater first, or use the Unbricker."],
  foreign: ["The FM-1 is in another firmware's update loader.", "Nothing was sent. Finish that update with the updater that started it; this page only resumes through the vendor loader."],
  same: ["This build is already installed.", "Nothing was sent. There is nothing to do."],
  busydevice: ["The FM-1 reports an update already in progress.", "Switch the FM-1 off and on, then press Connect."],
  changed: ["The device changed since it was identified.", "Nothing was written. Press Connect again and repeat the backup."],
  rejected: ["The FM-1 refused this firmware package.", "Nothing was written. Check that this page and its package come from the same FM-4 release."],
  backup: ["The backup did not finish.", "Nothing was written. Keep the FM-1 connected and idle (sequencer stopped), then run the backup again."],
  package: ["The firmware package failed its checks.", "Nothing was sent. Reload the page; if this persists the package on this site is damaged or does not match fm4.json."],
  lost: ["The USB connection was lost during the update.", "Reconnect the same FM-1 without closing this page, press Connect, then Resume. A device left in its update loader resumes the write."],
  stopped: ["The FM-1 stopped asking for data.", "Keep it connected, press Connect, then Resume. If it no longer appears, switch it off and on first."],
  noloader: ["The update loader did not appear.", "Unplug and replug the USB cable, press Connect and Resume. If the FM-1 shows as a disk (WL82 UBOOT1.00), use the Unbricker."],
  noreturn: ["The FM-1 did not come back after writing.", "Switch it off and on and press Connect to read its identity. If it is still in its update loader, Resume."],
  mismatch: ["The FM-1 restarted with a different firmware than expected.", "This is not a successful install. Press Connect to read its identity; if it is in the loader, Resume."],
  badreq: ["The FM-1 asked for data outside the package.", "Stop here. Do not retry with this package; report the build id."],
  failed: ["The update did not finish.", "Keep the FM-1 connected and press Connect to see its state. If it no longer starts, see the recovery ladder below."],
  oldfm4: ["This FM-4 build (1.0) may have stored songs inside stock V15's settings area.", "Nothing was written. FM-4 1.0 saved songs 3–8 at flash 0x93000–0xC8FFF, where stock keeps its settings; stock can fail to start on that data (blank screen). Install FM-4 1.1 or later first (it moves its songs out on its first start), then restore stock. Only if you never saved songs 3–8, tick the box below and run the backup again."],
  prepare: ["The FM-4 could not clear its data out of stock V15's settings area.", "Nothing was written. Switch the FM-1 off and on, press Connect and run the backup again. If it fails again, do not restore stock from this page: make a full-flash dump with the Unbricker and see the Recovery section."],
};

const MESSAGE_CODES = [
  [/USB 连接已断开|USB 意外断开|USB 已断开/, "lost"],
  [/设备停止请求数据|升级超过最长等待时间/, "stopped"],
  [/等待设备重连超时/, "noreturn"],
  [/固件版本校验不一致/, "mismatch"],
  [/设备读取超出固件包范围|设备请求长度无效|设备的升级阶段不匹配/, "badreq"],
  [/多台|MIDI 端口过多/, "ambiguous"],
  [/打开 MIDI 端口超时|关闭 MIDI 端口超时/, "busyport"],
  [/设备身份已改变|设备状态已改变|请重新连接设备/, "changed"],
  [/设备正在处理另一次升级/, "busydevice"],
  [/设备拒绝此固件包/, "rejected"],
  [/设备已经是这个版本/, "same"],
  [/浏览器未发现|已发现 MIDI 端口|已找到 MIDI 端口|设备未响应/, "notfound"],
  [/固件|升级程序|原厂|网页固件信息|芯片型号/, "package"],
];

export function codeOf(err) {
  if (!err) return "failed";
  if (err.code && err.code in ERRORS) return err.code;
  if (err.code === "AMBIGUOUS") return "ambiguous";
  if (err.code === "PORT_TIMEOUT") return "busyport";
  const m = MESSAGE_CODES.find(([re]) => re.test(err.message || ""));
  return m ? m[1] : "failed";
}

export function describeError(err, t = (s) => s) {
  const code = codeOf(err);
  const [title, recovery] = ERRORS[code];
  return { code, title, detail: err ? t(err.message || "") : "", recovery };
}

// Updater status phases (WebUpdate.js strings) -> page phases
export const PHASES = {
  "准备升级程序": ["stage1", "Step 1 of 2: the running firmware is preparing the update loader"],
  "等待升级设备重新连接": ["reconnect", "Waiting for the FM-1 to restart into its update loader (up to 40 s)"],
  "写入固件": ["stage2", "Step 2 of 2: writing the firmware. Do not unplug, do not close this page"],
  "重启并核对新版本": ["verify", "Write complete. Waiting for the FM-1 to restart and checking the installed build"],
  "正在恢复四个原厂音色库": ["factory", "Restoring the four factory voice banks"],
  "更新完成": ["done", "Done"],
};
export function progressOf(phase, pct) {
  const [key, text] = PHASES[phase] || ["other", String(phase)];
  const value = key === "reconnect" ? null : key === "done" ? 100 : Number.isFinite(pct) ? pct : null;
  return { key, value, text: value === null || key === "done" ? text : `${text}: ${value}%` };
}

/**
 * Classify a WebUpdate connect() result. kind: "fm4" | "open" (tagged), "stock", "felucca",
 * "resume", "same", "refuse", "foreign". route: how install() must run.
 */
export function classify(result, pkg) {
  const id = result.identity || "";
  if (result.resume) {
    if (id !== pkg.loaderIdentity) return { kind: "foreign", identity: id };
    return { kind: "resume", identity: id, force: false };
  }
  if (result.migration || result.legacy) {
    if (/^ota-/i.test(id)) return { kind: "foreign", identity: id };
    if (id === "FM-1_015") return { kind: "stock", identity: id, force: true, migration: pkg.kind !== "stock" };
    if (FELUCCA_ID.test(id)) return { kind: "felucca", identity: id, force: true, migration: true };
    return { kind: "refuse", identity: id };
  }
  if (result.same) return { kind: "same", identity: id, current: result.current };
  const fm4 = FM4_PRODUCT_RE.test(id);
  return { kind: fm4 ? "fm4" : "open", identity: id, current: result.current, force: pkg.kind === "stock", migration: false };
}

export function describeDevice(c) {
  switch (c.kind) {
    case "fm4": return `${c.identity}: FM-4, installed build ${hex8(c.current)}`;
    case "open": return `${c.identity}: fm1_open firmware, installed build ${hex8(c.current)}`;
    case "same": return `${c.identity}: this exact build is already installed`;
    case "stock": return `${c.identity}: M-VAVE stock firmware V15`;
    case "felucca": return `${c.identity}: Felucca-family firmware (Felucca, SLOOP, ORBIT…)`;
    case "resume": return `${c.identity}: vendor update loader. An interrupted write can be resumed`;
    case "foreign": return `${c.identity}: another firmware's update loader`;
    default: return `${c.identity}: not a firmware this page can update`;
  }
}

/** What the backup step contains for each kind (table 2.1 of docs/update_protocol.md). */
export const BACKUP_TEXT = {
  fm4: "Reads all 100 user voices from the FM-4 and saves them as a TQ5 100-voice .syx file. Songs, the multi setup and system settings are not included.",
  open: "Saves the current project, every stored project slot, voice banks A–D and the sample bank as one .zip. Projects do not contain samples; banks are separate files.",
  felucca: "Saves Felucca's complete musical archive (projects, settings, presets, patches and samples) as one .json file.",
  stock: "Stock V15 cannot be backed up over MIDI. Only a full-flash dump made with the Unbricker (Windows or Linux) preserves stock settings and user patches.",
  resume: "An update loader cannot export a backup. Continue only if you already saved a backup before the interrupted update.",
};
const STOCK_FROM_FM4_TEXT = " Before stock V15 is written, the FM-4 is asked to confirm that none of its data lies in the flash area stock V15 uses for its settings (and to erase any it finds there). Songs are not included in the backup and are erased by the stock restore.";

/** "FM-4 1.1 TQ5" -> true when the build answers PREPARE_STOCK. */
export function fm4CanPrepare(infoText) {
  const m = /^FM-4\s+(\d+)\.(\d+)/.exec(infoText || "");
  if (!m) return false;
  const [a, b] = [Number(m[1]), Number(m[2])], [x, y] = FM4_PREPARE_VERSION;
  return a > x || (a === x && b >= y);
}

/** Extra pin on the stock package: its firmware region must be V15's, byte for byte. */
export async function stockRegionOk(pkg) {
  const region = pkg.image.subarray(1024 + 0x4000, 1024 + 0x93000);
  const hash = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", region)), (b) => b.toString(16).padStart(2, "0")).join("");
  return region.length === 0x8F000 && hash === V15_REGION_SHA256;
}

// Felucca INFO decoding, adapted from Felucca web/index_pkg.html archiveInfo() (GPL-3.0-only).
export function feluccaInfo(a) {
  let pos = a.indexOf(0); if (pos < 0) throw new Error("Invalid INFO");
  const version = String.fromCharCode(...a.slice(0, pos)); pos++;
  const engines = a[pos]; pos += 5;
  for (let i = 0; i < engines; i++) { const end = a.indexOf(0, pos); if (end < 0) throw new Error("Invalid INFO"); pos = end + 1; }
  const ext = a.slice(pos + 1);
  const supported = /^FELUCCA /i.test(version) && ext[0] === 16 && ext[1] === 0x55 && ext[2] === 1 &&
    ext[4] === 0x4d && ext[5] === 1 && ext[8] === 0x42 && ext[9] === 1 && Boolean(ext[10] & 1);
  return { version, supported };
}

function portLink(input, output) {
  const fns = new Set(), handler = (e) => { for (const f of fns) f(e.data); };
  input.addEventListener("midimessage", handler);
  return {
    send(bytes) { if (output.state === "disconnected" || input.state === "disconnected") return false; output.send(bytes); return true; },
    on(f) { fns.add(f); return () => fns.delete(f); },
    async close() { input.removeEventListener("midimessage", handler); fns.clear(); await Promise.allSettled([input.close?.(), output.close?.()]); },
  };
}

const today = () => new Date().toISOString().slice(0, 10);

export function createInstaller(deps) {
  const { ui, FM1Web } = deps;
  const t = deps.t || ((s) => s);
  const requestMIDIAccess = deps.requestMIDIAccess;
  const download = deps.download;
  const timing = deps.timing || {};
  const onBusy = deps.onBusy || (() => {});
  const state = {
    phase: "load",           // load | idle | connected | backedup | writing | done | error
    meta: null, pkgs: { fm4: null, stock: null }, mode: "fm4",
    access: null, updater: null, device: null, ports: null,
    backup: null, busy: false, writing: false, lastPhase: "", log: [],
  };
  const log = (s) => { state.log.push(s); if (ui.log) ui.log.textContent = state.log.slice(-200).join("\n"); };

  /* ------------------------------------------------------------------ rendering --- */
  function showError(err) {
    const d = describeError(err, t);
    state.lastError = d;
    ui.error.hidden = false;
    ui.error.setAttribute("data-code", d.code);
    ui.errorTitle.textContent = d.title;
    ui.errorDetail.textContent = d.detail;
    ui.errorRecovery.textContent = d.recovery;
    log(`error ${d.code}: ${d.detail}`);
  }
  function clearError() {
    ui.error.hidden = true; ui.error.removeAttribute("data-code");
    ui.errorTitle.textContent = ui.errorDetail.textContent = ui.errorRecovery.textContent = "";
  }
  function setProgress(p) {
    ui.progress.hidden = false;
    if (p.value === null) ui.progress.removeAttribute("value"); else ui.progress.setAttribute("value", String(p.value));
    ui.phase.textContent = p.text;
  }
  const pkg = () => state.pkgs[state.mode];

  function refresh() {
    const p = pkg(), c = state.device, busy = state.busy;
    ui.connect.disabled = busy || !p;
    if (ui.modeFm4) ui.modeFm4.disabled = busy || state.writing || !state.pkgs.fm4;
    if (ui.modeStock) ui.modeStock.disabled = busy || state.writing || !state.pkgs.stock;
    const needs = c && c.kind;
    ui.backupBox.hidden = !needs || !["fm4", "open", "felucca", "stock", "resume"].includes(needs);
    ui.backupRun.hidden = !["fm4", "open", "felucca"].includes(needs);
    ui.backupRun.disabled = busy || state.phase !== "connected";
    ui.ackBox.hidden = needs !== "stock";
    const oldFm4 = needs === "fm4" && state.mode === "stock" && !fm4CanPrepare(c.info);
    ui.oldFm4Box.hidden = !oldFm4;
    ui.haveBox.hidden = needs !== "resume";
    ui.backupText.textContent = needs ? (BACKUP_TEXT[needs] || "") + (needs === "fm4" && state.mode === "stock" ? STOCK_FROM_FM4_TEXT : "") : "";
    if (needs === "stock" || needs === "resume") {
      const box = needs === "stock" ? ui.ack : ui.have;
      if (!busy && (state.phase === "connected" || state.phase === "backedup")) {
        state.phase = box.checked ? "backedup" : "connected";
        state.backup = box.checked ? { kind: needs, acknowledged: true } : null;
      }
    }
    ui.install.disabled = busy || state.phase !== "backedup";
    ui.install.textContent = c && c.kind === "resume" ? "Resume install…" : state.mode === "stock" ? "Restore stock V15…" : "Install FM-4…";
  }

  function renderPackage() {
    const f = state.pkgs.fm4, s = state.pkgs.stock;
    if (f) ui.pkg.textContent = `FM-4 ${f.release_version || ""} · ${f.product} · build ${hex8(f.build_id)} · ${f.bytes.toLocaleString("en-US")} bytes`;
    else ui.pkg.textContent = state.metaError
      ? `No FM-4 firmware package is published on this site yet (${state.metaError}). You can still restore stock V15.`
      : "No FM-4 firmware package is published on this site yet. You can still restore stock V15.";
    ui.pkg.setAttribute("data-available", f ? "yes" : "no");
    if (ui.modeFm4) ui.modeFm4.disabled = !f;
    if (!f && state.mode === "fm4") { state.mode = "stock"; if (ui.modeStock) ui.modeStock.checked = true; }
    if (ui.stockInfo) ui.stockInfo.textContent = s ? `M-VAVE V15 (${s.bytes.toLocaleString("en-US")} bytes, SHA-256 ${hex8(s.sha256)}…), Apache-2.0` : "The stock V15 image could not be checked.";
  }

  function setBusy(on) { state.busy = on; onBusy(on || state.writing); refresh(); }

  /* ------------------------------------------------------------------ load --- */
  async function load() {
    clearError();
    if (deps.secure === false || !FM1Web || !globalThis.crypto?.subtle) {
      showError(fail("unsupported", "This page needs a secure context (https:// or http://localhost) and crypto.subtle."));
      state.phase = "idle"; refresh(); return;
    }
    let meta = null;
    const base = new URL(deps.metaUrl, deps.baseUrl);
    try {
      const res = await deps.fetch(base.href, { cache: "no-store" });
      if (!res.ok) throw new Error(`fm4.json: HTTP ${res.status}`);
      meta = await res.json();
    } catch (e) { state.metaError = e.message; }
    const resolve = (m) => m && typeof m.url === "string" ? { ...m, url: new URL(m.url, base).href } : m;
    state.meta = meta;
    if (meta && meta.product) {
      try { state.pkgs.fm4 = await FM1Web.prepare(resolve(meta)); log(`package ${meta.product} ${meta.build_id} ok, loader ${state.pkgs.fm4.loaderIdentity}`); }
      catch (e) { state.metaError = `the package failed its checks: ${t(e.message)}`; showError(fail("package", t(e.message))); }
    }
    try { state.pkgs.stock = await FM1Web.prepare(resolve(meta?.stock || FALLBACK_STOCK)); }
    catch (e) {
      try { state.pkgs.stock = await FM1Web.prepare(resolve(FALLBACK_STOCK)); }
      catch (e2) { log(`stock: ${e2.message}`); }
    }
    if (state.pkgs.stock && !(await stockRegionOk(state.pkgs.stock).catch(() => false))) {
      log("stock: the firmware region is not V15's (0x4000..0x92FFF hash)"); state.pkgs.stock = null;
    }
    state.phase = "idle";
    renderPackage(); refresh();
  }

  function setMode(mode) {
    if (state.busy || state.writing || !state.pkgs[mode]) return;
    state.mode = mode; resetDevice(); refresh();
  }
  function resetDevice() {
    state.device = null; state.backup = null; state.phase = state.phase === "load" ? "load" : "idle";
    ui.device.textContent = "Not connected";
    if (ui.ack) ui.ack.checked = false;
    if (ui.oldFm4) ui.oldFm4.checked = false;
    if (ui.have) ui.have.checked = false;
    ui.backupStatus.textContent = "";
  }

  /* ------------------------------------------------------------------ connect --- */
  // the only place that asks for MIDI, synchronously inside the click
  function connect() {
    if (state.busy || !pkg()) return Promise.resolve();
    let request;
    try {
      if (typeof requestMIDIAccess !== "function") throw fail("unsupported", "Web MIDI is not available in this browser.");
      request = requestMIDIAccess({ sysex: true });
    } catch (e) { showError(e.code ? e : fail("unsupported", e.message)); return Promise.resolve(); }
    return identify(request);
  }

  async function identify(request) {
    clearError(); resetDevice(); setBusy(true);
    ui.progress.hidden = true;
    try {
      let access;
      try { access = await request; }
      catch (e) { throw fail(e && e.name === "NotSupportedError" ? "unsupported" : "denied", e.message); }
      if (!access.sysexEnabled && access.sysexEnabled !== undefined) throw fail("denied", "SysEx permission was not granted.");
      state.access = access;
      await state.updater?.disconnect().catch(() => {});
      state.updater = new FM1Web.Updater(access, pkg(), status, timing);
      ui.device.textContent = "Looking for an FM-1…";
      const result = await state.updater.connect({ force: state.mode === "stock" });
      const c = classify(result, pkg());
      state.ports = state.updater.link ? { input: state.updater.link.input, output: state.updater.link.output } : null;
      state.device = c;
      log(`identity ${c.identity} -> ${c.kind}`);
      ui.device.textContent = describeDevice(c);
      if (c.kind === "fm4" && state.ports) {
        // Close the update link first: one owner of the port at a time.
        await state.updater.disconnect();
        const link = portLink(state.ports.input, state.ports.output);
        try { const info = await new Fm4Device(link, { timeout: timing.ext ?? 1000 }).info(); if (info) { c.info = info.text; ui.device.textContent += ` (${info.text})`; } }
        finally { await link.close(); }
      } else await state.updater.disconnect();
      if (c.kind === "same") throw fail("same", `${c.identity} already runs build ${c.current}.`);
      if (c.kind === "refuse") throw fail("refused", `${c.identity} is neither stock V15, fm1_open/FM-4 nor a Felucca-family firmware.`);
      if (c.kind === "foreign") throw fail("foreign", `The FM-1 reports ${c.identity}; this page resumes only through ${pkg().loaderIdentity}.`);
      if (state.mode === "stock" && c.kind === "stock" && !c.migration) log("device already runs V15: only the factory voices will be restored");
      state.phase = "connected";
    } catch (e) {
      showError(e);
      if (!state.device) ui.device.textContent = "Not connected";
    } finally { setBusy(false); }
  }

  /* ------------------------------------------------------------------ backup --- */
  async function runBackup() {
    const c = state.device;
    if (state.busy || state.phase !== "connected" || !c) return;
    clearError(); setBusy(true);
    const say = (s) => { ui.backupStatus.textContent = s; };
    try {
      const toStock = state.mode === "stock" && c.kind === "fm4";
      if (toStock && !fm4CanPrepare(c.info) && !ui.oldFm4.checked)
        throw fail("oldfm4", `${c.info || "This FM-4"} cannot confirm that its songs are out of stock V15's settings area.`);
      let file;
      if (c.kind === "fm4") file = await backupFm4(say);
      else if (c.kind === "open") file = await backupOpen(say);
      else if (c.kind === "felucca") file = await backupFelucca(say);
      else return;
      say(`Saving ${file.name}…`);
      await download(file.name, file.data, file.type);
      // Re-parse what was handed to the browser before unlocking Install.
      file.verify(file.data);
      let prepared = "";
      if (toStock) prepared = await prepareForStock(c, say);
      state.backup = { kind: c.kind, name: file.name, bytes: file.data.length, summary: file.summary, prepared };
      state.phase = "backedup";
      say(`Backup saved and verified: ${file.name}, ${file.summary}. Keep this file.${prepared ? " " + prepared : ""}`);
    } catch (e) {
      say("");
      showError(e.code && e.code in ERRORS ? e : fail("backup", t(e.message)));
    } finally { setBusy(false); }
  }

  async function backupFm4(say) {
    const { input, output } = state.ports;
    const link = portLink(input, output);
    const codec = await deps.codec();
    try {
      const dev = new Fm4Device(link, { timeout: timing.ext ?? 1000 });
      const dumps = await dev.readBank({ count: USER_SLOTS, onProgress: (n, total) => say(`Reading user voice ${n} of ${total}…`) });
      const records = dumps.map((d, i) => {
        codec.resetVoice(7);
        for (const m of splitMsgs(d)) codec.parse(7, m, 16);
        if (splitMsgs(d).length !== 4) throw new Error(`User voice ${i + 1}: incomplete reply`);
        return codec.vmemPack(7);
      });
      const data = bank100Syx(records, codec, 0);
      const verify = (bytes) => {
        const p = parseSyx(bytes, codec);
        if (p.format !== "tq5-100" || p.voices.length !== 100 || p.warnings.length) throw new Error("The saved voice bank did not read back as 100 voices.");
        p.voices.forEach((v, i) => { if (v.slot !== i || v.record.some((b, k) => b !== records[i][k])) throw new Error(`Voice ${i + 1} differs after reading the file back.`); });
      };
      return { name: `FM-4-user-voices-${today()}.syx`, type: "application/octet-stream", data, verify, summary: "100 user voices" };
    } finally { await link.close(); }
  }

  /** FM-4 -> stock: have the FM-4 erase/verify its data outside its storage window (PREPARE_STOCK). */
  async function prepareForStock(c, say) {
    if (!fm4CanPrepare(c.info)) {
      log(`prepare: ${c.info || "no INFO"} has no PREPARE_STOCK; the user confirmed that songs 3-8 were never saved`);
      return "This FM-4 build cannot check its data; you confirmed that songs 3–8 were never saved.";
    }
    const { input, output } = state.ports;
    const link = portLink(input, output);
    try {
      say("Asking the FM-4 to clear its data out of stock V15's settings area…");
      const r = await new Fm4Device(link, { timeout: timing.ext ?? 1000 }).prepareStock({ timeout: timing.prepare ?? 30000 });
      log(`prepare: ${r ? `status ${r.status}, ${r.erased} sectors erased, ${r.remaining} remaining` : "no reply"}`);
      if (!r) throw fail("prepare", "The FM-4 did not answer PREPARE_STOCK.");
      if (!r.ok) throw fail("prepare", r.status === 2 ? "The FM-4 reports that its storage is unavailable." : `${r.remaining} FM-4 record(s) are still in stock's area${r.status === 1 && !r.remaining ? " (an erase failed)" : ""}.`);
      return r.erased ? `The FM-4 erased ${r.erased} old song sector(s) from stock's settings area; nothing of FM-4 is left there.` : "The FM-4 confirmed that none of its data is in stock V15's settings area.";
    } finally { await link.close(); }
  }

  async function backupOpen(say) {
    const lib = new FM1Web.Library(state.access);
    const files = [];
    try {
      say("Connecting to the project library…");
      await lib.connect();
      say("Reading the current project…");
      files.push({ name: "workspace.fm1p", data: FM1Web.projectFile(await lib.download(0)) });
      for (let slot = 0; slot < 16; slot++) {
        say(`Reading project slot ${slot + 1} of 16…`);
        try { await lib.query(8, [slot]); } catch (e) { if (e.deviceCode === 1) continue; throw e; }
        files.push({ name: `project-${String(slot + 1).padStart(2, "0")}.fm1p`, data: FM1Web.projectFile(await lib.download(0)) });
      }
      if (lib.programs === 128) for (let b = 0; b < 4; b++) {
        say(`Reading voice bank ${"ABCD"[b]}…`);
        files.push({ name: `bank-${"ABCD"[b]}.syx`, data: FM1Web.bankFile(await lib.readBank(b)) });
      }
      try {
        const samples = await lib.samplesRead((f) => say(`Reading the sample bank: ${Math.round(f * 100)}%`));
        files.push({ name: "samples.fm1s", data: samples });
      } catch (e) { log(`samples skipped: ${e.message}`); files.push({ name: "samples-not-ready.txt", data: new TextEncoder().encode(t(e.message)) }); }
    } finally { await lib.disconnect().catch(() => {}); }
    const data = makeZip(files);
    const verify = (bytes) => {
      const back = readZip(bytes);
      if (back.length !== files.length) throw new Error("The backup archive is incomplete.");
      for (const f of back) {
        if (f.name.endsWith(".fm1p")) FM1Web.readProjectFile(f.data);
        else if (f.name.endsWith(".syx")) FM1Web.readVoices(f.data);
      }
    };
    return { name: `FM-1-open-backup-${today()}.zip`, type: "application/zip", data, verify, summary: `${files.length} files` };
  }

  async function backupFelucca(say) {
    const { BackupConnection, captureBackup, readBackup } = deps.felucca;
    const { input, output } = state.ports;
    const conn = new BackupConnection(input, output);
    try {
      const info = feluccaInfo(await conn.request([1, []]));
      if (!info.supported) throw new Error(`${info.version || "This Felucca version"} cannot export a full archive. Update Felucca first, or make a full-flash dump with the Unbricker.`);
      const archive = await captureBackup((r, o) => conn.request(r, o), info.version,
        (done, total) => say(`Reading the Felucca archive: ${Math.round(done / Math.max(1, total) * 100)}%`));
      const data = new TextEncoder().encode(JSON.stringify(archive));
      const verify = (bytes) => readBackup(new TextDecoder().decode(bytes));
      return { name: `Felucca-backup-${today()}.json`, type: "application/json", data, verify, summary: `${archive.objects.length} objects` };
    } finally { conn.close(); await Promise.allSettled([input.close?.(), output.close?.()]); }
  }

  /* ------------------------------------------------------------------ confirm --- */
  function confirmText() {
    const c = state.device, p = pkg();
    const target = p.kind === "stock" ? "M-VAVE stock V15 (with the four factory voice banks)" : `FM-4 ${p.release_version || ""} build ${hex8(p.build_id)}`;
    const from = c.kind === "resume" ? `${c.identity} (interrupted update)` : `${c.identity}${c.current ? ` build ${hex8(c.current)}` : ""}`;
    return { from, target, typed: needsTyped() };
  }
  const needsTyped = () => { const c = state.device; return Boolean(c && (c.migration || c.force || state.mode === "stock" || c.kind === "resume")); };

  function openConfirm() {
    if (state.busy || state.phase !== "backedup") return;
    const x = confirmText();
    ui.confirmFrom.textContent = x.from;
    ui.confirmTo.textContent = x.target;
    ui.confirmTypedBox.hidden = !x.typed;
    ui.confirmTyped.value = "";
    ui.confirmGo.disabled = x.typed;
    ui.confirm.showModal ? ui.confirm.showModal() : (ui.confirm.open = true);
  }
  function typedChanged() { ui.confirmGo.disabled = needsTyped() && ui.confirmTyped.value.trim() !== "INSTALL"; }
  function cancelConfirm() { ui.confirm.close ? ui.confirm.close() : (ui.confirm.open = false); }

  /* ------------------------------------------------------------------ write --- */
  function status(phase, pct) {
    state.lastPhase = phase;
    const p = progressOf(phase, pct);
    setProgress(p);
    if (p.key !== state.lastKey) { log(`phase ${p.key}`); state.lastKey = p.key; }
  }
  const guard = (e) => { e.preventDefault(); e.returnValue = ""; return ""; };

  async function install() {
    if (state.busy || state.phase !== "backedup" || !state.device) return;
    if (needsTyped() && ui.confirmTyped.value.trim() !== "INSTALL") return;
    cancelConfirm();
    clearError();
    const c = state.device, p = pkg();
    state.writing = true; state.phase = "writing"; setBusy(true);
    const win = deps.window;
    win?.addEventListener?.("beforeunload", guard);
    let lock = null;
    try { lock = await deps.wakeLock?.(); } catch (e) { log(`wake lock unavailable: ${e.message}`); }
    try {
      status("准备升级程序", 0);
      ui.phase.textContent = "Reconnecting to the FM-1…";
      const updater = new FM1Web.Updater(state.access, p, status, timing);
      state.updater = updater;
      const result = await updater.connect({ force: Boolean(c.force) });
      const again = classify(result, p);
      if (again.identity !== c.identity || again.kind !== c.kind) throw fail("changed", `Identified as ${c.identity} (${c.kind}), now ${again.identity} (${again.kind}).`);
      await updater.install({ force: Boolean(c.force) });
      state.phase = "done";
      setProgress({ value: 100, text: p.kind === "stock" ? "Stock V15 restored and verified." : `FM-4 installed and verified: build ${hex8(p.build_id)}.` });
      ui.device.textContent = p.kind === "stock"
        ? "FM-1_015: stock V15. Run System > Reset Patches if presets look wrong; Unbricker repair-settings fixes leftover data."
        : `${p.product}: FM-4 build ${hex8(p.build_id)} verified on the device.`;
    } catch (e) {
      const code = codeOf(e);
      if (code === "noreturn" && state.lastPhase === "等待升级设备重新连接") e.code = "noloader";
      showError(e);
      state.phase = "error";
      ui.device.textContent = "Unknown after the error: press Connect to read the device state.";
      state.device = null; state.backup = null;
    } finally {
      state.writing = false;
      win?.removeEventListener?.("beforeunload", guard);
      try { await lock?.release?.(); } catch { /* released already */ }
      await state.updater?.disconnect().catch(() => {});
      setBusy(false);
      if (state.phase !== "done") state.phase = state.phase === "error" ? "idle" : state.phase;
      refresh();
    }
  }

  const ready = load();
  return { state, ready, connect, runBackup, openConfirm, typedChanged, cancelConfirm, install, setMode, refresh };
}

function splitMsgs(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== 0xF0) continue;
    const e = bytes.indexOf(0xF7, i + 1); if (e < 0) break;
    out.push(bytes.subarray(i, e + 1)); i = e;
  }
  return out;
}

export const IDS = {
  pkg: "inst-pkg", stockInfo: "inst-stock-info", modeFm4: "inst-mode-fm4", modeStock: "inst-mode-stock",
  connect: "inst-connect", device: "inst-device",
  backupBox: "inst-backup", backupText: "inst-backup-text", backupRun: "inst-backup-run", backupStatus: "inst-backup-status",
  ackBox: "inst-ack-box", ack: "inst-ack", haveBox: "inst-have-box", have: "inst-have",
  oldFm4Box: "inst-oldfm4-box", oldFm4: "inst-oldfm4",
  install: "inst-install", progress: "inst-progress", phase: "inst-phase",
  error: "inst-error", errorTitle: "inst-error-title", errorDetail: "inst-error-detail", errorRecovery: "inst-error-recovery",
  confirm: "inst-confirm", confirmFrom: "inst-confirm-from", confirmTo: "inst-confirm-to", confirmTypedBox: "inst-confirm-typed-box",
  confirmTyped: "inst-confirm-typed", confirmGo: "inst-confirm-go", confirmCancel: "inst-confirm-cancel", log: "inst-log",
};

/** Wire the INSTALL tab. overrides inject doubles in tests. */
export function mount(doc, overrides = {}) {
  const ui = {};
  for (const [key, id] of Object.entries(IDS)) {
    const el = doc.getElementById(id);
    if (!el) throw new Error(`installer: missing #${id}`);
    ui[key] = el;
  }
  ui.modeInputs = [ui.modeFm4, ui.modeStock];
  const win = doc.defaultView;
  const ctl = createInstaller({
    ui,
    FM1Web: globalThis.FM1Web,
    t: (s) => globalThis.FM1I18n ? globalThis.FM1I18n.t(s, "en") : s,
    metaUrl: "firmware/fm4.json",
    baseUrl: doc.baseURI,
    secure: win ? win.isSecureContext : true,
    requestMIDIAccess: win?.navigator?.requestMIDIAccess ? (o) => win.navigator.requestMIDIAccess(o) : undefined,
    fetch: (u, o) => fetch(u, o),
    window: win,
    wakeLock: () => win?.navigator?.wakeLock?.request("screen"),
    download: (name, bytes, type) => {
      const url = URL.createObjectURL(new Blob([bytes], { type }));
      const a = doc.createElement("a"); a.href = url; a.download = name; doc.body.append(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 5000);
    },
    ...overrides,
  });
  ui.connect.addEventListener("click", () => ctl.connect());
  ui.backupRun.addEventListener("click", () => ctl.runBackup());
  ui.ack.addEventListener("change", () => ctl.refresh());
  ui.have.addEventListener("change", () => ctl.refresh());
  ui.oldFm4.addEventListener("change", () => ctl.refresh());
  ui.install.addEventListener("click", () => ctl.openConfirm());
  ui.confirmTyped.addEventListener("input", () => ctl.typedChanged());
  ui.confirmGo.addEventListener("click", () => ctl.install());
  ui.confirmCancel.addEventListener("click", () => ctl.cancelConfirm());
  ui.modeFm4.addEventListener("change", () => ctl.setMode("fm4"));
  ui.modeStock.addEventListener("change", () => ctl.setMode("stock"));
  return ctl;
}
