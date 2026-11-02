# FM-4 webapp

A static single page for desktop Chrome and Edge. It installs the FM-4 firmware, edits TQ5 voices
and manages voice libraries for an M-VAVE FM-1 running FM-4. It uses plain HTML, CSS and ES modules
with no build step, no dependencies and no CDN.

| Tab | What it does |
| --- | --- |
| **Edit** | TQ5 voice editor for the 8 parts. It has an algorithm diagram, four operator panels (ratio or fixed frequency, waveform W1–W8, a drawn envelope, level, scaling and EG shift), LFO, pitch and performance settings, controllers and the effect. Each edit goes to the FM-4 as a TQ5 parameter change on the part's MIDI channel, throttled to about 60 messages per second. You can audition voices through the firmware engine (`fm4.wasm`) running in an AudioWorklet, from the on-screen keyboard, the computer keyboard or any MIDI input. Undo, redo, Init voice and Randomize are included. |
| **Library** | Loads `.syx` files: 32-voice VMEM banks (TQ5, TX81Z, DX11, DX21, DX27, DX100), TQ5 100-voice banks, and single voices (VCED alone or with ACED, ACED2 and EFEDS). It warns about bad checksums but still loads the data. Click a voice to hear it. You arrange a 100-slot user bank by dragging voices or with the keyboard, starting from `presets/FM-4_PRESETS.syx`. The bank can be sent to the device or received from it one voice at a time, and exported as a TQ5 100-voice or 32-voice VMEM `.syx`. The working library is kept in IndexedDB when the browser allows it. |
| **Install** | The firmware installer (design: `docs/update_protocol.md` §5). The steps are: load the metadata, connect, identify, take a backup (mandatory and verified), confirm, write and verify. It can also restore stock V15. |

## Serve it locally

```sh
cd webapp
python3 -m http.server
```

Then open <http://localhost:8000>. Web MIDI with SysEx needs a secure context. `localhost` counts
as one; any other host must use `https://`. Opening `index.html` as a `file://` URL does not work.

## Device protocol notes

- Voices use TQ5 SysEx (`docs/tq5_spec.md`). A full voice is 191 bytes in four messages: EFEDS,
  ACED2, ACED and VCED. In MULTI mode the device number `n` addresses the part on MIDI channel `n`.
  In SINGLE mode it is the basic receive channel.
- The FM-1 SDK receives SysEx frames of at most 1024 bytes and sends at most 128 bytes per
  message. A 4104-byte VMEM bulk therefore never goes over the wire. Bank transfers use the FM-4
  extension commands, one voice at a time, with one request in flight. The commands are defined in
  `js/fm4ext.js` and must match `firmware/app/fm4/fm4_core.h` (`FM4X_*`):

  | Command | Bytes | Reply |
  | --- | --- | --- |
  | STORE | `F0 7D 46 34 01 part slot F7` | `F0 7D 46 34 41 status slot F7` (0 = ok) |
  | READ | `F0 7D 46 34 02 slot F7` | the 191-byte voice |
  | SELECT | `F0 7D 46 34 03 part bank program F7` | none |
  | INFO | `F0 7D 46 34 04 F7` | `F0 7D 46 34 44 "FM-4 1.1 TQ5" F7` |
  | PREPARE_STOCK (1.1+) | `F0 7D 46 34 05 "STOCK" F7` | `F0 7D 46 34 45 status erased remaining F7` (0 = clean) |

- Before **Restore stock V15** on an FM-4, the Install tab sends PREPARE_STOCK after the voice
  backup and unlocks the install only when the FM-4 reports that none of its data lies in stock
  V15's data area (flash 0x93000 and up). FM-4 1.0 does not answer it and stored songs 3–8 there;
  the page blocks it unless the user confirms those songs were never saved. See
  `../docs/RECOVERY.md`.
- **Recovery** (Install tab): `js/recovery.js` checks a 1 MiB full-flash dump (MvaveFM1Unbricker's
  `full-flash-before.bin`) in the browser and prints the exact jl-uboot-tool `erase` commands for
  FM-4 data found in stock's area. The same check on the command line, on any OS:
  `node tools/fm1_dump_check.mjs full-flash-before.bin`.

- `.syx` import and export always use the full TQ5 formats.

## Updating `firmware/fm4.json`

The Install tab reads `firmware/fm4.json`, which follows the schema of `docs/update_protocol.md`
§5.2 and fm1_open `Build.ps1`:

```json
{ "schema": 1, "product": "FM-1_8404100", "profile": "dx7", "family": 7, "release_version": "1.0",
  "build_id": "<64 hex>", "sha256": "<64 hex>", "bytes": 123456, "loader_sha256": "ef1f2db8…",
  "url": "fm4.fwsc", "stock": { "kind": "stock", "product": "FM-1_015", "url": "FM-1_015.fwsc", … } }
```

The page passes each entry to fm1_open's `FM1Web.prepare()`. That function fetches `url` from the
same origin and checks the SHA-256, byte count, package structure, product identity, family 7 and
the loader hash. For the stock entry it also checks the pinned V15 hash and the factory voice
banks. Write the file with the tool, which runs the same checks:

```sh
# from a CI build (firmware/build/dx7/generated/*.fwsc and firmware/build/dx7/build-identity.json)
node tools/make_meta.mjs --fwsc FM-4-latest.fwsc --identity build-identity.json
# no FM-4 package yet: stock restore only (the current state)
node tools/make_meta.mjs --stock-only
```

The tool copies the package to `firmware/fm4.fwsc`. `loader_sha256` is the SHA-256 of
`firmware/sdk/tools/pack/ota.bin` (the vendor loader). The tool refuses a package that carries
any other loader. If `fm4.json` has no `product`, the page says that no FM-4 package is published
and disables Install FM-4. Restore stock V15 still works.

## Tests

```sh
cd webapp
node --test tests/*.test.mjs
for f in js/*.js tools/*.mjs vendor/*/*.js tests/*.mjs; do node --check "$f"; done
```

- `wasm.test.mjs`: VMEM pack and unpack, 191-byte dump parse, parameter changes, the device-number
  filter and engine output, all through `fm4.wasm`.
- `syx.test.mjs`: every file format, including a generated 100-voice bank and the shipped preset
  bank (byte-identical re-export), with bad checksums and stray data.
- `installer.test.mjs`: the real `FM1Web.Updater` drives `FakeFM2` (`fakefm2.mjs`, extended from
  ORBIT's FakeFM1), which emulates capability 0x31, arm 0x32 and the vendor loader `ota-FM-1_015`.
  The tests cover an FM-4 update, migration from stock V15, Felucca and fm1_open, resume from the
  loader, stock restore with factory voices, and refusals. They also inject an unplug during stage 1
  and during stage 2, and assert that no `F0 22 24 35` key or 0x32 arm is sent before the backup
  and the confirmation.
- `device.test.mjs`: the extension commands against `fm4_core.h`, bank write and read one voice at a
  time with message-size limits, retry and abort, and the 60-messages-per-second throttle.
- `make_meta.test.mjs`: `make_meta.mjs` on the real `FM-1_015.fwsc` and on a synthetic Open-style
  package (`fwsc.mjs`).

`fm4.wasm` is built from the firmware engine sources with `wasm/build.sh` (clang with the wasm32
target).

## Hooks

`globalThis.FM4App.mountDevicePanel((root, { midi, device, codec, editor, lcd }) => …)` mounts a
Device panel (system and multi setup) into `#device-panel-hook` on the Install tab.

## Credits and licensing

The webapp is **GPL-3.0-only** as a whole (see `../LICENSE.md`). It combines:

- **fm1_open** by 珠海笙科智能科技有限公司 (M-VAVE): `vendor/fm1_open/WebUpdate.js` and `I18n.js`,
  vendored verbatim under `GPL-3.0-only WITH GPL-3.0-linking-exception`.
  `vendor/fm1_open/LICENSE.md` and `THIRD_PARTY.md` ship with them. The 5×7 LCD font comes from
  fm1_open `app/screen.c`. The `.fwsc` test fixture follows `sdk/Package.cs` (MIT, kagaimiq and
  M-VAVE; format from kagaimiq's jl-misctools).
- **Felucca** and **ORBIT** by Leo Kuroshita (@kurogedelic), Hügelton Instruments, GPL-3.0-only.
  `vendor/felucca/fm1backup.js` is vendored verbatim. `js/installer.js`,
  `tests/installer.test.mjs` and `tests/fakefm2.mjs` adapt ORBIT's installer and its tests, and are
  marked "Modified by FM-4 contributors".
- **Stock V15 recovery image** `firmware/FM-1_015.fwsc` and the vendor loader `ota.bin`: M-VAVE,
  Apache-2.0. The licence text and notices are in `vendor/fm1_open/THIRD_PARTY.md`.
- **MvaveFM1Unbricker** (MIT, Michael P Jones) is linked from the page, not copied.

FM-4 is not affiliated with or endorsed by M-VAVE or Yamaha. Product names only identify
compatible formats.
