# FM-4

**FM-4** is a custom firmware for the **M-VAVE FM-1** pocket synthesizer that turns it into a **Yamaha TQ5**-style 4-operator FM tone generator, plus a browser app to install it and edit/librarian TQ5 voices.

- **Sound engine:** an open-source YM2414 (OPZ) style 4-operator engine — 8 algorithms, 8 waveforms (W1–W8), TX81Z ratio table (0.50–25.95) and fixed frequencies, OPZ envelopes with EG shift and "reverb" rate, LFO with 4 waves and key sync, feedback. Integer-only, runs from SRAM inside the FM-1 audio interrupt. Chip math follows [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3).
- **TQ5 functions:** 8 voices of polyphony (4–16 selectable), single and 8-part multi mode with voice reserve, the TQ5's 10 effect programs (hall/room/plate reverb, delay, delay L/R, stereo echo, distortion + reverb/echo, gate and reverse gate), an 8-track × 8-song sequencer, 100 preset + 100 user voices.
- **TQ5 SysEx:** VCED / ACED / ACED2 / EFEDS voice dumps, parameter changes, dump requests (including the 191-byte full voice), TQ5 bank block select, remote bank switches, TX81Z-style multi parameters. See [docs/SYSEX.md](docs/SYSEX.md).
- **Panel:** every FM-1 button, encoder and key is mapped — see [docs/PANEL.md](docs/PANEL.md).
- **Original preset bank:** 100 new voices (keys, basses, brass, bells, organs, pads, leads, plucks, percussion, effects); no Yamaha factory data is included. Load your own TQ5/TX81Z/DX11 `.syx` banks with the webapp.

## Repository

| Path | What |
|---|---|
| `firmware/` | Fork of [m-vave/fm1_open](https://github.com/m-vave/fm1_open) (see `firmware/UPSTREAM.md`). FM-4 lives in `firmware/app/fm4/` (engine, multi, effects, sequencer, TQ5 codec, core) and replaces the stock app's `screen.c` (panel UI), `midi_router.c`, `transfer.c`, `project.c`, `samples.c`, `fm_library.c`. |
| `webapp/` | Installer (backup → flash → verify, restore stock V15), TQ5 voice editor with in-browser audio (the same C engine compiled to WebAssembly), and librarian. |
| `tests/` | Host tests: engine pitch/envelopes, sequencer, effects, multi, TQ5 codec, all 100 presets, the firmware core, and the panel UI with LCD screenshots. `make -C tests`. |
| `tools/gen_presets.py` | Generates the preset bank. |
| `docs/` | Panel guide, SysEx reference, TQ5 spec research, fm1_open porting notes, update protocol notes. |

## Building

The firmware builds on **GitHub Actions** (`.github/workflows/build.yml`) with JieLi's Windows toolchain 2.5.3, fetched from JieLi and hash-checked on every run (it is not redistributable, so it is never committed). Each push to `main` runs the host tests and publishes `FM-4-<commit>.fwsc` plus the build reports to the `builds` branch.

Locally: `make -C tests` (any Linux/macOS C compiler) and `webapp/wasm/build.sh` (clang with the wasm32 target).

## Installing

Use the webapp's INSTALL tab in Chrome or Edge on a desktop computer with the FM-1 connected by a USB data cable. It backs up what the current firmware exposes, installs FM-4 through the FM-1's standard update protocol, verifies the build afterwards, and can restore M-VAVE's stock V15.

> **There is no automatic rollback.** If the FM-1 loses power during the write it may need hardware recovery (see [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker)). FM-4 has not yet been validated on hardware; check `firmware-report.json` and run the hardware checklist in `docs/` before relying on it.

## Licence

GPL-3.0-only (fm1_open's linking exception applies to the firmware, which links M-VAVE's binary FM-1 SDK; see `firmware/LICENSE.md` and `firmware/THIRD_PARTY.md`). ymfm tables: BSD-3-Clause (`firmware/app/fm4/LICENSE-ymfm`). Not affiliated with M-VAVE or Yamaha. "TQ5", "TX81Z" and "DX" are Yamaha trademarks, used only to describe compatibility.
