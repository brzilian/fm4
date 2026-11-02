// SPDX-License-Identifier: GPL-3.0-only
// FM-1 full-flash dump checker for FM-4 users who returned to stock V15.
//
// Input: a 1 MiB dump of the FM-1's SPI flash, as MvaveFM1Unbricker's `backup` / `restore` save it
// (backups/fm1-recovery-*/full-flash-before.bin) or as jl-uboot-tool's `read 0 0x100000 dump.bin`
// writes it. Nothing here talks to a device: the page and tools/fm1_dump_check.mjs only read a file.
//
// Flash map (1 MiB). Stock V15 from its package's flash directory; the rest from
// MvaveFM1Unbricker (fm1_unbrick.py: VM_START.., BTIF_START, USR_START, GLOBALS_START) and
// fm1_open's Package.cs (FlashLimit 0xFF000, UserSize 536576):
//   0x00000..0x03FFF  boot head (SPL, isd_config): no installer writes it
//   0x04000..0x92FFF  stock V15 firmware region (what a stock restore writes)
//   0x93000..0xE8FFF  stock V15 settings store (JieLi VM); factory state: only 0x93000 holds data,
//                     starting 55 AA AA 55
//   0xE9000           stock Bluetooth data;  0xEA000..0xFBFFF stock factory voices (USR)
//   0xFC000..0xFEFFF  erased on a factory unit;  0xFF000 key_mac
//   0x7C000..0xFEFFF  the fm1_open SDK user partition FM-4 stores into (offset 0 = 0x7C000)

export const FLASH_BYTES = 0x100000;
export const SECTOR = 0x1000;
export const HEAD_END = 0x4000;
export const FW_START = 0x4000, FW_END = 0x93000;
export const VM_START = 0x93000, VM_END = 0xE9000;
export const BTIF_START = 0xE9000, USR_START = 0xEA000, USR_END = 0xFC000;
export const USER_PARTITION = 0x7C000;
export const VM_MAGIC = [0x55, 0xAA, 0xAA, 0x55];
const MAGICS = { 0x53344D46: "system", 0x56344D46: "voices", 0x47344D46: "song", 0x4C344D46: "layout marker" };
const LEGACY_SONG_BYTES = 32796;   // sizeof(seq_song) in FM-4 1.0: 28-byte head + 4096 * 8-byte events

const u32 = (b, at) => (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
const hex = (n, w = 5) => "0x" + n.toString(16).toUpperCase().padStart(w, "0");
const erased = (b, from, to) => { for (let i = from; i < to; i++) if (b[i] !== 0xFF) return false; return true; };
const same = (a, b, from, to) => { for (let i = from; i < to; i++) if (a[i] !== b[i]) return false; return true; };

/** flash.bin of an Open-built or stock package, from FM1Web.inspectPackage()/prepare() output. */
export function flashOfPackage(pkg, stock = false) {
  if (!pkg?.image) return null;
  if (stock) return pkg.image.subarray(1024, 1024 + FW_END);
  // Package.cs Wrap(): flash.bin at 1024, the loader right after it (32-byte aligned)
  const loaderAt = pkg.image.length - ((pkg.loader.length + 31) & ~31);
  return pkg.image.subarray(1024, loaderAt);
}

/** FM-4 record headers at sector starts in the user partition. */
export function findFm4Records(dump) {
  const out = [];
  for (let a = USER_PARTITION; a < 0xFF000; a += SECTOR) {
    const kind = MAGICS[u32(dump, a)], version = u32(dump, a + 4), bytes = u32(dump, a + 8);
    if (!kind || (version !== 1 && version !== 2) || bytes > 0x80000) continue;
    const legacySong = kind === "song" && version === 1 && bytes === LEGACY_SONG_BYTES;
    const sectors = legacySong ? 9 : Math.max(1, Math.ceil((16 + bytes) / SECTOR));
    out.push({ addr: a, offset: a - USER_PARTITION, kind, version, bytes, legacySong, end: a + sectors * SECTOR,
      inStockData: a >= VM_START });
  }
  return out;
}

/**
 * Analyse a full dump. refs: {stock: V15 flash.bin (0x93000 bytes), fm4: FM-4 flash.bin (optional)}.
 * Returns {findings: [{level: "ok"|"warn"|"bad"|"info", text}], records, actions: [text], commands: [text], verdict}.
 */
export function analyzeDump(dump, refs = {}) {
  const findings = [], actions = [], commands = [];
  const say = (level, text) => findings.push({ level, text });
  if (!(dump instanceof Uint8Array) || dump.length !== FLASH_BYTES)
    return { findings: [{ level: "bad", text: `This is not a full FM-1 flash dump: ${dump?.length ?? 0} bytes, expected 1048576 (1 MiB).` }], records: [], actions: ["Make the dump again with MvaveFM1Unbricker (backup) or jl-uboot-tool (read 0 0x100000 dump.bin)."], commands: [], verdict: "notadump" };
  const { stock, fm4 } = refs;

  // (a) boot head
  if (stock) {
    if (same(dump, stock, 0, HEAD_END)) say("ok", "Boot head 0x0000..0x3FFF is identical to the official V15 package's.");
    else if (fm4 && same(dump, fm4, 0, HEAD_END)) say("warn", "Boot head 0x0000..0x3FFF is the fm1_open/FM-4 package's head, not V15's. Installers normally never write it; report this.");
    else say("warn", "Boot head 0x0000..0x3FFF differs from the V15 package's. Nothing here (or in the Unbricker) writes the head; restore V15 first and ask for help if it still does not start.");
  }

  // (b) firmware region
  let verdict = "unknown";
  if (stock) {
    let eqStock = 0, eqFm4 = 0, blank = 0;
    for (let a = FW_START; a < FW_END; a += SECTOR) {
      if (same(dump, stock, a, a + SECTOR)) eqStock++;
      else if (fm4 && a + SECTOR <= fm4.length && same(dump, fm4, a, a + SECTOR)) eqFm4++;
      if (erased(dump, a, a + SECTOR)) blank++;
    }
    const total = (FW_END - FW_START) / SECTOR;
    if (eqStock === total) { say("ok", "Firmware region 0x4000..0x92FFF is exactly official V15: the stock restore was written completely."); verdict = "v15"; }
    else if (fm4 && same(dump, fm4, FW_START, fm4.length)) {
      say("bad", "Firmware region holds the FM-4 firmware: the stock restore never wrote."); verdict = "fm4";
    } else {
      say("bad", `Firmware region is a mix: ${eqStock}/${total} sectors are V15's${fm4 ? `, ${eqFm4} are FM-4's` : ""}, ${blank} are erased. The stock write was interrupted.`);
      verdict = "partial";
    }
    if (verdict !== "v15") {
      actions.push("Write official V15's firmware region with MvaveFM1Unbricker: py fm1_unbrick.py restore FM-1.fwsc (it backs up first, writes only 0x4000..0x92FFF and verifies every byte).");
    }
  } else say("info", "No V15 reference loaded: the firmware region was not compared.");

  // (c) FM-4 data
  const records = findFm4Records(dump);
  const inWindow = records.filter((r) => !r.inStockData), outside = records.filter((r) => r.inStockData);
  if (inWindow.length) say("info", `${inWindow.length} FM-4 record(s) below 0x93000 (inside the stock firmware region; ${verdict === "v15" ? "these are stale reads of" : "a stock restore overwrites"} that area). Harmless to stock.`);
  if (outside.length) {
    for (const r of outside)
      say("bad", `FM-4 ${r.kind} record (layout ${r.version === 2 ? 2 : 1}${r.legacySong ? ", FM-4 1.0 song" : ""}) at ${hex(r.addr)}..${hex(r.end - 1)}: inside stock V15's ${r.addr < VM_END ? "settings store" : r.addr < USR_START ? "Bluetooth data" : r.addr < USR_END ? "factory voice area" : "reserved area"}.`);
    actions.push("Erase the FM-4 records that lie in stock's data area (jl-uboot-tool, after the double backup). Only those sectors are erased; stock's own data around them is left alone.");
    for (const r of outside) commands.push(`erase ${hex(r.addr)} ${hex(r.end - r.addr, 4)}`);
  } else say("ok", "No FM-4 data in stock V15's data areas (0x93000 and up).");

  // (d) stock settings store
  const magicOk = VM_MAGIC.every((b, i) => dump[VM_START + i] === b);
  const vmCovered = outside.some((r) => r.addr <= VM_START && r.end > VM_START);
  if (magicOk) say("ok", "Stock settings store starts with its factory header (55 AA AA 55 at 0x93000).");
  else if (vmCovered) say("bad", "The first stock settings sector (0x93000) holds FM-4 data instead of stock's header (55 AA AA 55). This is the most likely reason stock V15 does not start.");
  else if (erased(dump, VM_START, VM_START + SECTOR)) say("warn", "The first stock settings sector (0x93000) is erased.");
  else say("warn", `The first stock settings sector (0x93000) does not start with 55 AA AA 55 (found ${Array.from(dump.subarray(VM_START, VM_START + 4), (b) => b.toString(16).padStart(2, "0")).join(" ")}).`);
  if (!magicOk) {
    actions.push("If stock still shows a blank screen after the steps above: put the settings store back to factory state with MvaveFM1Unbricker repair-settings --factory DUMP.bin (needs a dump of a factory-fresh unit), or, as a last resort, erase the whole stock settings store (stock user patches and settings are lost; that stock rebuilds it from blank is expected JieLi behaviour but unverified on this device).");
    commands.push(`# last resort only: erase ${hex(VM_START)} ${hex(VM_END - VM_START, 4)}`);
  }

  // (e) Bluetooth data, factory voices
  if (erased(dump, USR_START, USR_END)) say("warn", "The factory voice area 0xEA000..0xFBFFF is erased. Once stock V15 starts, restore the voices from this page (Restore stock V15 on a V15 device) or with System > Reset Patches.");
  if (erased(dump, BTIF_START, BTIF_START + SECTOR)) say("info", "The Bluetooth data sector 0xE9000 is erased (pairings are lost; stock recreates it).");

  if (verdict === "v15" && !outside.length && magicOk)
    say("info", "Nothing FM-4-related explains a blank screen in this dump. Check the boot head line above and ask for help with this report.");
  return { findings, records, actions, commands, verdict };
}

export function formatReport(r) {
  const tag = { ok: "OK  ", warn: "WARN", bad: "BAD ", info: "INFO" };
  const lines = ["FM-1 flash dump check (FM-4 webapp)", ""];
  for (const f of r.findings) lines.push(`${tag[f.level] || "    "} ${f.text}`);
  if (r.actions.length) { lines.push("", "What to do, in order:"); r.actions.forEach((a, i) => lines.push(`${i + 1}. ${a}`)); }
  if (r.commands.length) { lines.push("", "jl-uboot-tool commands (type them at the =>JL: prompt, after the backup):"); for (const c of r.commands) lines.push(`    ${c}`); }
  return lines.join("\n");
}

/** Wire the Recovery section: a file input and a report. getRefs() -> {stock, fm4} flash.bin bytes. */
export function mountRecovery(doc, getRefs) {
  const input = doc.getElementById("rec-file"), out = doc.getElementById("rec-report");
  if (!input || !out) return null;
  input.addEventListener("change", async () => {
    const file = input.files && input.files[0];
    if (!file) return;
    out.hidden = false;
    out.textContent = "Reading the dump…";
    try { out.textContent = formatReport(analyzeDump(new Uint8Array(await file.arrayBuffer()), await getRefs())); }
    catch (e) { out.textContent = `Could not read the dump: ${e.message}`; }
  });
  return { input, out };
}
