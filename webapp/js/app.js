// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp entry point: tabs, the MIDI status bar, the audition engine, and the three views.

import { Codec, voiceName } from "./codec.js";
import { Audio } from "./audio.js";
import { Midi, Fm4Device } from "./device.js";
import { Lcd } from "./lcd.js";
import { Editor, Keyboard } from "./editor.js";
import { Library } from "./library.js";
import { mount as mountInstaller } from "./installer.js";
import { mountRecovery, flashOfPackage } from "./recovery.js";
import * as felucca from "../vendor/felucca/fm1backup.js";

const $ = (id) => document.getElementById(id);
const lcd = new Lcd($("lcd"));
lcd.show("FM-4  TQ5 VOICE", "LOADING ENGINE");

function download(name, bytes, type = "application/octet-stream") {
  const url = URL.createObjectURL(new Blob([bytes], { type }));
  const a = document.createElement("a"); a.href = url; a.download = name;
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
}

/* ------------------------------------------------------------------ tabs */
const TABS = ["editor", "library", "install"];
function showTab(name) {
  for (const t of TABS) {
    const tab = $(`tab-${t}`), on = t === name;
    tab.setAttribute("aria-selected", String(on)); tab.tabIndex = on ? 0 : -1;
    $(`view-${t}`).hidden = !on;
  }
  try { localStorage.setItem("fm4-tab", name); } catch { /* storage blocked */ }
}
for (const t of TABS) {
  const tab = $(`tab-${t}`);
  tab.addEventListener("click", () => showTab(t));
  tab.addEventListener("keydown", (e) => {
    const d = { ArrowRight: 1, ArrowLeft: -1 }[e.key]; if (!d) return;
    e.preventDefault(); const n = TABS[(TABS.indexOf(t) + d + TABS.length) % TABS.length]; showTab(n); $(`tab-${n}`).focus();
  });
}
let startTab = "editor";
try { startTab = localStorage.getItem("fm4-tab") || "editor"; } catch { /* storage blocked */ }
if (location.hash === "#install") startTab = "install";
showTab(TABS.includes(startTab) ? startTab : "editor");

/* ------------------------------------------------------------------ MIDI */
const midi = new Midi();
const device = new Fm4Device(midi);
let installing = false;

function fillPorts() {
  for (const [sel, kind, cur] of [[$("midi-in"), "inputs", midi.input], [$("midi-out"), "outputs", midi.output]]) {
    const ports = midi.ports(kind);
    sel.replaceChildren(new Option("None", ""), ...ports.map((p) => new Option(p.name || p.id, p.id)));
    sel.value = cur ? cur.id : "";
    sel.disabled = installing || !midi.access;
  }
}
function autoPick() {
  const like = (p) => /fm.?1|fm-?4|usb.?midi/i.test(p.name || "");
  if (!midi.input) { const p = midi.ports("inputs").find(like); if (p) midi.setInput(p.id); }
  if (!midi.output) { const p = midi.ports("outputs").find(like); if (p) midi.setOutput(p.id); }
}
async function identify() {
  const id = $("midi-identity");
  if (!midi.output) { id.textContent = midi.access ? "Choose the FM-4 as MIDI out to edit the device." : "MIDI not connected"; return; }
  if (!midi.input) { id.textContent = `Out: ${midi.output.name}. Choose a MIDI in to receive from the device.`; return; }
  id.textContent = "Asking the device who it is…";
  try {
    const info = await device.info();
    id.textContent = info ? `${info.text} on ${midi.output.name}` : `${midi.output.name} did not answer the FM-4 info request. It may run other firmware; install FM-4 from the Install tab.`;
    if (info) lcd.show("FM-4 CONNECTED", info.text.slice(0, 16));
  } catch (e) { id.textContent = e.message; }
}
$("midi-connect").addEventListener("click", () => {
  if (!navigator.requestMIDIAccess) { $("midi-identity").textContent = "This browser has no Web MIDI. Use desktop Chrome or Edge."; return; }
  const req = navigator.requestMIDIAccess({ sysex: true });       // inside the click
  audio.start().catch(() => {});
  req.then((access) => {
    midi.attach(access);
    midi.onchange = () => { fillPorts(); };
    autoPick(); fillPorts(); identify();
    $("midi-connect").textContent = "Refresh ports";
  }, (e) => { $("midi-identity").textContent = `MIDI access was refused (${e.message}). Allow MIDI with SysEx in the site settings.`; });
});
$("midi-in").addEventListener("change", (e) => { midi.setInput(e.target.value); identify(); });
$("midi-out").addEventListener("change", (e) => { midi.setOutput(e.target.value); identify(); });

let blink = 0;
midi.on((data) => {
  const act = $("midi-activity"); act.classList.add("on"); clearTimeout(blink); blink = setTimeout(() => act.classList.remove("on"), 80);
  const s = data[0];
  if (s >= 0x80 && s < 0xF0) {
    // Play the editor's part regardless of the incoming channel.
    audio.midi([(s & 0xF0) | editor.channel, data[1] ?? 0, data[2] ?? 0]);
  }
});

/* ------------------------------------------------------------------ engine */
const wasm = new Uint8Array(await (await fetch(new URL("../fm4.wasm", import.meta.url))).arrayBuffer());
const codec = await Codec.create(wasm);
const audio = new Audio(wasm);
const meter = $("kb-meter");
audio.onmeter = (m) => { meter.value = Math.min(1, m.peak * 3); };
for (const ev of ["pointerdown", "keydown"]) document.addEventListener(ev, () => audio.start().catch((e) => console.warn(e)), { once: true, capture: true });

const editor = new Editor($("view-editor"), { codec, audio, midi, device, lcd });
const keyboard = new Keyboard($("kb"), {
  onNote: (n, v) => (v ? audio.noteOn(editor.channel, n, v) : audio.noteOff(editor.channel, n)),
});
$("kb-panic").addEventListener("click", () => { keyboard.allOff(); audio.allOff(); });
const typing = (t) => t.closest?.("input:not([type=range]):not([type=checkbox]):not([type=radio]), select, textarea, dialog");
document.addEventListener("keydown", (e) => { if ($("view-install").hidden && !typing(e.target) && keyboard.key(e, true)) e.preventDefault(); });
document.addEventListener("keyup", (e) => { keyboard.key(e, false); });
window.addEventListener("blur", () => keyboard.allOff());

const library = new Library($("view-library"), {
  codec, audio, editor, midi, device, lcd, download, showTab,
  presetsUrl: new URL("../presets/FM-4_PRESETS.syx", import.meta.url).href,
  fetch: (u, o) => fetch(u, o),
});
library.init();

/* ------------------------------------------------------------------ installer */
const installer = mountInstaller(document, {
  felucca,
  codec: async () => Codec.create(wasm),
  onBusy: (busy) => {
    // One owner of the port at a time: the editor and library stop using MIDI while installing.
    installing = busy;
    midi.suspended = busy;
    fillPorts();
    for (const id of ["midi-connect", "ed-get", "ed-send"]) $(id).disabled = busy;
    for (const t of TABS) if (t !== "install") $(`tab-${t}`).disabled = busy;
    if (busy) for (const id of ["lib-read", "lib-write", "lib-send-one"]) $(id).disabled = true;
    else library.buttons();
  },
});

// Recovery: offline check of a full-flash dump against the packages this page already verified.
mountRecovery(document, async () => {
  await installer.ready;
  return { stock: flashOfPackage(installer.state.pkgs.stock, true), fm4: flashOfPackage(installer.state.pkgs.fm4) };
});

lcd.show("FM-4  TQ5 VOICE", voiceName(editor.voice()).toUpperCase());

/* ------------------------------------------------------------------ device panel hook */
// A later "Device" panel (system settings, multi setup) mounts here:
//   FM4App.mountDevicePanel((root, {midi, device, codec, editor, lcd}) => { ... })
// The root element is #device-panel-hook (Install tab); it is shown when a panel mounts.
globalThis.FM4App = {
  midi, device, codec, editor, library, installer, lcd, audio,
  mountDevicePanel(fn) {
    const root = $("device-panel-hook"); root.hidden = false;
    return fn(root, { midi, device, codec, editor, lcd });
  },
};
