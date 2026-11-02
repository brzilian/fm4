# Porting FM-4 onto M-VAVE fm1_open: a code-level reference

**Scope.** This is for FM-4: a Yamaha TQ5 style instrument (4-operator FM, 8 parts) for the M-VAVE FM-1. FM-4 is built on M-VAVE's official open firmware, cloned at `/root/fm4/fm1_open`.

**How this was established.** By reading `fm1_open` (app sources, the SDK headers, `Build.ps1`, `Package.cs` and the linker template) and by disassembling the symbol and relocation tables of `sdk/lib/libfm1_engine.a`. That library is one object, `engine_startup.S.o`, with 4807 sections. I also read the reference repos Felucca, orbit, mvave-fm1 and fm1-custom-fw.

**Confidence labels.** "Inferred" marks a claim taken from relocations, not from source. "Unverified" marks something not tested on hardware or with a toolchain. No pi32 toolchain was available, so nothing here was compiled.

---

## 0. Answers to the five priority questions

1. **Control-loop contract.** The SDK owns `main` and the RTOS task, and calls these app hooks:
   - `groove_*_init` from `fm1_app_init`;
   - `groove_{midi,samples,project,transfer}_poll` and `groove_screen_control` from `fm1_app_control`;
   - `fm1_audio_start/suspend/resume`;
   - `fm1_app_sysex` and `fm1_app_midi_received` from the MIDI routing task.

   Every `fm1_r_*` name is a **library-internal** function, not an app function. They map to `fm1_app_init`, `fm1_app_control`, `fm1_app_tick`, the CPU1 audio worker and the internals of `fm1_engine_render` (§1.3). Their checks keep passing as long as the app keeps calling `fm1_engine_render` and defines the hooks.
2. **Own engine in the ISR.** Yes, `app/board/audio.c` is ours.
   - Keep a live call to `fm1_engine_render`, or Test-Firmware fails. The callback, the native DSP kernels and `groove_sequence_*` are only reachable through it, and `--gc-sections` would drop them.
   - Cheapest safe option: call it every block with all 8 groove tracks set to role OFF, then add the FM-4 buffer before `fm1_output_mix`.
   - The groove engine uses a second core through an internal `exchange` mailbox (inferred). The public API gives FM-4 no access to CPU1, so FM-4 DSP runs on CPU0 inside a 2.90 ms block.
3. **Panel.**
   - Encoders: 0 = SELECT, 1 = ALGORITHM, 2..5 = KNOB 1..4 (top right, labelled at the bottom of the LCD), 6 = PRESETS.
   - Buttons 0..13: OCT−, OCT+, FX, SEL (`SCL` in `screen.c`), ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC.
   - The knob ADC is the MASTER pot on PB6, ADC channel 4. There is no SHIFT button. Layout table in §2.
4. **SysEx.**
   - Send: `fm1_midi_send_port`/`fm1_midi_send` accept only *complete* messages of 128 bytes or less. The simulator returns −1 above 128.
   - Receive: `fm1_app_sysex` gets complete frames of 1024 bytes or less. Larger frames are discarded.
   - So a 4104-byte DX7 or TQ5 bank can be neither sent nor received as one SysEx frame. fm1_open never does either. The browser pulls 64-byte chunks with a private 7-bit protocol and builds the `.syx` file in JavaScript (`WebUpdate.js` `bankFile`).
   - Whether a 4104-byte stream can be pushed as F0-less fragments is untested and against the header contract. Use single-voice dumps (they fit) plus a host-assisted bulk transfer.
5. **Package overflow.**
   - `Package.cs:141`: the app image must be **453,921 bytes or less**. That is 0xFF000 − 536576 user − 4096 BTIF − 32768 VM − 0x4000 − 0x120 − 383 − 64.
   - The CI image of 481,556 bytes is **27,635 bytes over**. The user partition is fixed at 536,576 bytes in both `Build.ps1:346` and `Package.cs:13`, and both files are hash-pinned.
   - Fix by dropping stock UI and content, in order: `app/screen.c` (154 KB of source, by far the largest object), `app/presets.c` (factory projects), `app/fm_factory.inc` (~10.5k-byte packed DX7 bank, plus its 16 KiB of BSS), sample transfer in `samples.c`/`transfer.c`, then `project.c`.
   - Each dropped app reference also lets the lib's editing code be garbage-collected. Replace them with small stubs for the hooks listed in §1.2 and the keep-alive references in §1.4.

---

## 1. Runtime architecture

### 1.1 Contexts

| Context | Who runs it | App code in it |
|---|---|---|
| Boot, `main` (lib, `.text.main`) | CPU0 | none. It starts CPU1 via `fm1_r_7919135c9963`, `os_init`, `task_create(app_task_handler)` and `os_start` |
| **Control task** `app_task_handler` (lib) | CPU0 RTOS task | `fm1_app_init` hooks once, then `fm1_app_control` hooks on every `fm1_control_request` |
| `tick` (lib `sys_timer`) | timer | calls `fm1_r_d16405f329ed` (= `fm1_app_tick`, which calls `fm1_control_request()` unless `fm1_update_busy`) |
| **MIDI routing task** `midi_scheduler_task` (lib) | RTOS task | `fm1_app_midi_received` (short messages), `fm1_app_sysex` (complete SysEx) |
| **Audio ISR** `fm1_audio_irq` (app, `app/board/audio.c:27`) | IRQ 11, priority 4, once per 128 frames | `fm1_engine_render`, then `fm1_output_mix` |
| **Panel ISR** `fm1_panel_irq` (app, `app/board/device.c`) | TIMER5 at 10 kHz, IRQ 63, priority 5 | `fm1_input_tick` and `fm1_scan_decode` (`app/board/fm1_input.h`) |
| CPU1 worker `fm1_r_2c729ca92be9` → `fm1_r_e7d3da61dac2` (lib) | second core | none. It shares `exchange` (1248 B) with `fm1_engine_render`, parks for Flash (`fm1_flash_worker_prepare`) and calls the budget helpers (inferred) |

`app_task_handler` startup order, from its relocation order:
1. `sys_timer_init`, devices, initcalls, `power_init`, ADC.
2. `fm1_flash_init`.
3. **`fm1_r_dee1ea411566` (`fm1_app_init`)**.
4. `fm1_update_init`.
5. `fm1_io_start`: USB, BLE, TRS MIDI, calls `fm1_io_trs_midi_enabled`.
6. **`fm1_audio_start`** (app, `audio.c:82`).
7. `sys_timer_add(tick)`.
8. Loop `os_task_pend`: `fm1_update_message` / `fm1_application_message`. The latter calls **`fm1_r_f03cc16d330d` (`fm1_app_control`)** when `control_pending` is set and `!fm1_update_busy`.

### 1.2 SDK → app hooks (undefined symbols in `libfm1_engine.a` that the app must define)

`fm1_app_init` (lib, `.text.cold`) calls, in order:
1. `groove_midi_init`
2. `fm1_pitch_reset` (lib)
3. `groove_fm_library_init`
4. internal setup of the 8 groove tracks (`source_create`, default types DX7, 303, PSG, PLUCK, SAMPLE, DX7, 303, 808)
5. `groove_sequence_init`
6. `groove_clock_set`
7. `groove_effects_restore`
8. `groove_edit_init`
9. **`groove_samples_init`**
10. **`groove_project_init`**
11. **`groove_transfer_init`**
12. **`groove_screen_init`**

`fm1_app_control` calls:
1. **`groove_midi_poll`**
2. **`groove_samples_poll`**
3. **`groove_project_poll`**
4. **`groove_transfer_poll`**
5. `groove_edit_poll` (lib)
6. **`groove_screen_control`**
7. delay/effects reconfiguration (lib)

The web build mirrors this in `web/engine/sound.c:1043` `fm1_web_control`.

The complete list of app-provided symbols the lib references:
- Generated by `Build.ps1`: `fm1_layout`, `fm1_build_id`, `fm1_update_identity`.
- Audio: `fm1_audio_start`, `fm1_audio_suspend`, `fm1_audio_resume`. Suspend and resume are called by `fm1_flash_write`/`erase` and `fm1_update_message`.
- FM library: `groove_fm_library_init`, `groove_fm_program` (called from the lib's MIDI program change and `source_create`; must be `FM1_DSP_CODE`).
- MIDI: `groove_midi_init`, `groove_midi_poll`, `groove_midi_panic`, `groove_midi_clock_reset`, `groove_midi_mode`, `groove_midi_note`, `groove_midi_track_reset`, `groove_midi_transport`, `groove_midi_clock`. The last five are called from the audio path and must be SRAM code.
- Project: `groove_project_init`, `groove_project_poll`, `groove_project_metadata`, `groove_project_name`, `groove_project_scratch` (must return a 4064-byte `struct groove_project` buffer; the lib's undo/clipboard uses it).
- `groove_samples_init`, `groove_samples_poll`.
- `groove_transfer_init`, `groove_transfer_poll`.
- **`groove_screen_init`, `groove_screen_control`.** These two are the real UI entry points for FM-4.

**Weak defaults in the lib (FM-4 overrides them):** `fm1_app_midi_received`, `fm1_app_sysex`, `fm1_io_trs_midi_enabled`, `fm1_midi_input_ports`, `fm1_midi_output_ports`, plus `malloc`, `calloc`, `free`, `realloc`. `Build.ps1:376` compiles `screen.c`, `transfer.c` and `midi_router.c` with `-fno-lto` because "This PI32 LTO plugin can discard application definitions when a native SDK object has weak defaults". **So put FM-4's strong definitions of these symbols in a file with one of those three names, or edit Build.ps1 (§6).**

### 1.3 The obfuscated `fm1_r_*` symbols

All of these are *local* to `engine_startup.S.o`, so none of them is an app function. The mapping comes from section names and relocations.

| Symbol | Section / size | Meaning (inferred) | Build.ps1 rule |
|---|---|---|---|
| `fm1_r_dee1ea411566` | `.text.cold.fm1_app_init`, 582 B | **`fm1_app_init`**: calls the `groove_*_init` hooks | must exist as T/N with size (`:94`) |
| `fm1_r_f03cc16d330d` | `.text.fm1_app_control`, 124 B | **`fm1_app_control`**: calls the `*_poll` hooks and `groove_screen_control` | same |
| `fm1_r_d16405f329ed` | `.text.fm1_app_tick`, 10 B | **`fm1_app_tick`**: calls `fm1_control_request` | same |
| `fm1_r_7919135c9963` | `.os_code`, 44 B | CPU1 launch from `main` | required (`:74`) |
| `fm1_r_1f449c2c7b8f` | `.sram`, 1 B object | dual-core/SFC critical flag (also used by `cpu_enter_sfc_critical`) | required |
| `fm1_r_2c729ca92be9` | SRAM, 40 B | CPU1 entry, called from `_start` | — |
| `fm1_r_e7d3da61dac2` | SRAM, 138 B | CPU1 audio worker loop (`exchange`, `fm1_flash_worker_prepare`) | required, in SRAM |
| `fm1_r_05431712fb7b`, `fm1_r_1993f9b103e3`, `fm1_r_da3931a5b7a8` | SRAM, 46/28/8 B | worker helpers; 0543 touches `audio_budget` | required, in SRAM |
| `fm1_r_ffe3f82683f6` | SRAM, 348 B | body of **`fm1_engine_render`** (CPU0 side of the exchange) | in SRAM |
| `fm1_r_0aa010fe1de1` | SRAM, 40 B | calls `groove_sequence_click` (metronome) | callback, in SRAM if present |
| `fm1_r_33d5e5b63b6b` | SRAM, 46 B | render budget accounting | same |
| `fm1_r_63cf2570ee9b` | SRAM, 30 B | master mix (`fm1_master_mix128`) | — |
| `fm1_r_74310ca1fed9`, `fm1_r_7e12ed19990d` | SRAM, 16/74 B | render leaf helpers | in SRAM |
| `fm1_r_c57f0724a655` | SRAM, 10 B | body of `fm1_engine_output_level` (reads the global master level) | — |
| `fm1_r_7e5e04f16253`, `fm1_r_aa9848f062ec` | `.text` | body of `fm1_engine_pause` / `resume` (render gate) | — |
| `fm1_r_945678bce933` | `.text` | body of `fm1_midi_post` (critical-section queue push) | — |
| `fm1_r_eba215ed3c8c`, `fm1_r_6b2771b0c9d1`, `fm1_r_c525e103c04b`, `fm1_r_060f1197193e` | linker-defined | stack and engine-workspace boundaries (`linker.ld.in:328-345`) | — |
| `fm1_r_83b2de623a96`, `fm1_r_c327ea18da09` | — | must **not** exist (forbidden list, `:109`) | — |

The required set is reachable two ways:
- From `main`, `app_task_handler` and `tick`, which are always alive.
- From **`fm1_engine_render`**. Nothing in the lib calls `fm1_engine_render`; only `app/board/audio.c:43` does.

### 1.4 What Test-Firmware (`sdk/Build.ps1:30-224`) checks

**Required symbols (`:74-85`):** the `fm1_r_*` symbols above, `vSoftwareInterruptISR`, the update/flash/BT/BLE/USB-audio symbols, and `groove_tuning_capture`, `groove_tuning_apply`, `groove_project_apply_edit`. Several are only kept alive by app references:
- `fm1_ble_set_role`, `scan`, `connect`, `disconnect`, `get_status`, `get_device`: only `screen.c` and `project.c` reference them.
- `fm1_bluetooth_configure`, `fm1_bluetooth_get_status`: same.
- `groove_tuning_apply`: has no lib caller.

If FM-4 drops `screen.c`, add a never-called-but-live keep-alive function. A reference from code that is actually reachable survives `--gc-sections`. A `used` data object alone may not.

**Real callbacks, not weak (`:94-100`):** `fm1_io_trs_midi_enabled` (`panel.c:8`) and **`fm1_app_sysex`** must be strong definitions with nonzero size.

**SRAM placement (`:138-168`):** these must live in 0x01c00000–0x01c80000.
- App: `fm1_audio_irq`, `fm1_audio_copy`, `fm1_output_mix`, and the panel symbols `fm1_input_tick`, `fm1_panel_irq`, `fm1_scan_decode`, `fm1_key_pin`, `fm1_encoder_pin`, `fm1_quadrature`.
- Lib: `fm1_engine_render`, `fm1_engine_output_level`, the USB/BT audio I/O and the `$extra` list at `:350-351`. That list is the native DSP: `groove_sequence_*`, `synth_dx7_*`, `psg_*`, `fm1_mix128` and so on, plus `synth.json` `sram_required`.
- Most of `$extra` is reachable **only** through `render` ← `fm1_engine_render`. So FM-4 must keep calling `fm1_engine_render` from live code.
- Optional names such as `render_stereo`, `effects_render` and `engine_note` are checked only if present. FM-4 can reuse them: name its own SRAM functions anything else, or put them in SRAM too.

**Sections (`:202`):** only `.text .data .bss .ram0_data .ram0_bss .boot_info .cache_ram_data .cache_ram_bss` may be allocated, so do not invent new output sections. Use `FM1_AUDIO_CODE` (`section(".volatile_ram_code")`, `noinline, used`, at `sdk/include/fm1.h:16`) or `FM1_DSP_CODE`/`FM1_DSP_DATA` (`app/synth/engine.h`) for audio code and its tables. Large zeroed buffers go in ordinary BSS, which `.ram0_bss` places in internal SRAM.

**Other checks:**
- Heap: `_MALLOC_SIZE`, which is everything from the end of `.ram0_bss` up to 0x1c7fe00−180, must be **at least 49,152 bytes** (`:137`).
- ACL pools: at least 1024/4096.
- No LVGL, no FAT, no `libfm1_runtime.a`.
- Entry 0x02000120.
- `fm1_build_id` is 32 B and `fm1_update_identity` is 27 B.

### 1.5 Rendering FM-4: options and the recommendation

`audio.c:43-47` does:

```
fm1_engine_render(rendered, 128) → fm1_output_mix(rendered) → copy to DMA half
```

`fm1_output_mix` (`app/audio_output.c:14`) applies the master gain, which ramps toward `fm1_engine_output_level()`. That value is set by `groove_master_update(knob)`, which `screen.c` calls. It then mixes USB and Bluetooth audio in, clips to Q23 and feeds USB recording.

- **(a) Replace or mix: yes.** `audio.c` and `audio_output.c` are app code.
  - Recommended sequence: call `fm1_engine_render(rendered,128)`, then `fm4_render(fm4buf)` (an `FM1_AUDIO_CODE` function), then sum the two into `rendered`, then `fm1_output_mix(rendered)`.
  - Q23 stereo interleaved, 256 words per block.
  - Keep calling `groove_master_update(controls.knob)` from the FM-4 UI so the MASTER pot still drives `fm1_engine_output_level()`. Or replace that gain inside `fm1_output_mix`.
- **(b) Idle groove: yes.** In `groove_screen_init` (control task, after `fm1_app_init` has built the tracks), call `groove_track_role_set(t, GROOVE_TRACK_OFF)` for t = 0..7. Per `instrument.h:88`, "Off releases its engine", which frees its heap. Also do not post notes to `fm1_midi_post`.
  - The lib render then costs only bookkeeping. All symbols stay linked and the DMA timing statistics stay meaningful.
  - Not calling `fm1_engine_render` at all saves a little CPU but fails Test-Firmware unless FM-4 keeps a live but runtime-dead call, for example `if (fm4_groove_enabled) fm1_engine_render(...)` with a `volatile` flag.
  - Unverified: whether render with all tracks OFF still runs `groove_sequence_render` (internal tempo clock and MIDI clock output via `groove_midi_clock`). FM-4's `groove_midi_clock`/`transport` hooks can simply ignore it.
- **MIDI routing for FM-4.** `fm1_app_midi_received` (`midi_router.c:319`) runs on the routing task. Today it forwards to `fm1_midi_post`, the lib queue that is drained inside render. For FM-4, push into an FM-4 single-producer ring instead. Drain that ring at the start of `fm4_render`, or in the control task.
  - Rules from `fm1.h:34`: no blocking, Flash, UI, allocation or audio edits in the routing task.
  - Panel keys are read in `groove_screen_control` (control task) and should go into the same ring.
- **CPU budget.** One block is 128/44100 = **2.902 ms**. `audio.c:50` commits only if the block finishes within 7/8 of a block period (about 2.54 ms) from the IRQ start, including the lib render and `fm1_output_mix`. Otherwise it counts `overruns`.
  - Measure with `fm1_audio_stats_read` (`peak_ticks` in `FM1_AUDIO_TIMER_COUNT` units) or with transfer command 19.
  - The panel scan ISR at 10 kHz has higher priority (5 vs 4) and pre-empts the render.
  - No FPU. Use integer or Q math. `libcompiler_rt` has soft-float `__addsf3` etc., but those are flash code and slow.
- **SRAM budget (approximate).**
  - RAM0 = 0x1c00000..0x1c7fe00 − 180 B, about 511 KiB.
  - The lib, before GC, holds about 69 KiB of SRAM code, 17 KiB of SRAM data and 138 KiB of BSS. That BSS includes `fm1_fx_states` 48 KiB, BT PCM 16 KiB, groove `scratch` 8 KiB and `undo` 4 KiB.
  - Stock app: `factory_raw` 16 KiB, `paint_pixels` 7.5 KiB, the project `record` 4 KiB.
  - Heap must stay at least 48 KiB. Groove voices come from `calloc` in `source_create` and `synth_dx7_create`; they are released when tracks are set OFF.
  - Realistically about 100–180 KiB is left for FM-4 SRAM code, tables and BSS after dropping stock UI data. Check `sdk.map` `_MALLOC_SIZE` after every build.
- **Flash-write safety.**
  - `fm1_flash_write`/`erase` call `fm1_audio_suspend()`, which sets `mute_depth`. The ISR then returns before rendering (`audio.c:34`), and the SDK parks CPU1.
  - The ISR itself and everything it calls (FM-4 render, tables, `memcpy` if used) must still be in SRAM. Libc `memcpy`/`memset` are in plain `.text`, which is Flash, so audio code should use its own loops (`audio.c:21` comment: "No libc calls in the DMA commit window").
  - Call Flash only from the control task. Expect audio dropouts during writes.

---

## 2. Panel inventory

Sources: `app/screen.c:127` enum, `app/board/fm1_input.h` wiring, the faceplate geometry in `sdk/Simulator.js:693,712-714` ("Control positions follow the FM-1 faceplate"), Felucca/Dinghy names (`mvave-fm1/skills/.../SKILL.md:43-47`) and ORBIT `README-SLOOP.md:191-205`.

**Physical layout.** Coordinates are simulator units on a 1000×624 faceplate.

```
 [MASTER pot] [SELECT e0]   +--------+   [KNOB1 e2][KNOB2 e3][KNOB3 e4][KNOB4 e5]
 [PRESETS e6][ALGORITHM e1] |240x240 |   [FX 2][SEL 3][ENV 4][LFO 5][EDIT 6][GLO 7]
 [OCT− 0][OCT+ 1]           |  LCD   |   [HOME 8][SAVE 9][ARP 10][SEQ 11][PLAY 12][REC 13]
 ───────────── 27 keys F3..G5: 16 white (steps 1-16) + 11 black ─────────────
```

The LCD spans x ≈ 29–55 %. KNOB 1–4 are named on screen by labels drawn at y = 195..227, x = 6 + 59·col (`screen.c:417-439`).

| Index | Name (`screen.c`) | Panel / Felucca name | Location | Stock use (`apply_controls`, `screen.c:1840-2562`) |
|---|---|---|---|---|
| btn 0 | `OCT_DOWN` | OCT− | left, under PRESETS | octave −; previous 16-step page on SEQ/ACTIONS |
| btn 1 | `OCT_UP` | OCT+ | left | octave +; next page |
| btn 2 | `FX` | FX | right block row 1, #1 | effects page (track sends, shared, delay) |
| btn 3 | `SCL` | **SEL** (silkscreen; ORBIT: "between FX and ENV") | row 1, #2 | DX7 keyboard scaling page; advanced tone edit for other sources |
| btn 4 | `ENV` | ENV | row 1, #3 | ADSR page |
| btn 5 | `LFO` | LFO | row 1, #4 | LFO page |
| btn 6 | `EDIT` | EDIT | row 1, #5 | tap: quick edit; hold 600 ms: full edit; EDIT+REC: patch name; SEQ+EDIT: actions |
| btn 7 | `GLO` | GLO | row 1, #6 | tap: MIX page; hold: mixer layer (SELECT track, KNOB 1–4 levels, keys mute/solo); HOME+GLO: Bluetooth |
| btn 8 | `HOME` | HOME | row 2, #1 | overview / preset browser; HOME+PLAY panic; HOME+SAVE undo; HOME+SEQ tap tempo; HOME+PRESETS source select |
| btn 9 | `SAVE` | SAVE | row 2, #2 | project save page / confirm |
| btn 10 | `ARP` | ARP | row 2, #3 | tap: arp on/off; hold: arp settings |
| btn 11 | `SEQ` | SEQ | row 2, #4 | step page; hold 600 ms: actions menu |
| btn 12 | `PLAY` | PLAY (STOP) | row 2, #5 | start/stop (posts FA/FB/FC) |
| btn 13 | `REC` | REC | row 2, #6 | record toggle; REC+SELECT on overview moves the cursor |
| enc 0 | — | SELECT | left block, top right | track select; menus |
| enc 1 | — | ALGORITHM | left block, bottom right | DX7 algorithm / cutoff; bottom page select on SEQ/ARP |
| enc 2–5 | — | KNOB 1–4 | top right row | the four on-screen parameters |
| enc 6 | — | PRESETS | left block, bottom left | preset/program, step cursor, slot |
| knob | `fm1_controls.knob` | MASTER pot | left block, top left | ADC channel 4 (PB6), 0..1023 → `groove_master_update` |
| battery | `.battery` | — | — | ADC channel 3 (PB1) |
| keys bits 0..26 | — | F3 (MIDI 53) .. G5 | bottom | play notes. White = `{0,2,4,6,7,9,11,12,14,16,18,19,21,23,24,26}` (steps 1–16). Black keys on FM edit = `{1,3,5,8,10,13,15,17,20,22}` → OP1..OP6, PITCH, GLOBAL, MONO, POLY (`screen.c:130-132`; simulator labels) |

**Rules that follow from the hardware and API:**
- **No SHIFT button.** Stock layers are hold-based: GLO hold, EDIT/ARP/SEQ long press (600 ms), and two-button chords.
- **LEDs:** `fm1_led_set(id,on)` with ids 0..13 for buttons and 14..40 for keys (`key+14`). Encoders have no LEDs. LEDs are single-brightness bits multiplexed in the scan (`fm1_led[11]`). Stock usage is in `screen.c:1181-1240`.
- **Input API:** `fm1_controls_read` (`panel.c:27`) returns held key and button bitmasks plus encoder detents accumulated since the last read. It is debounced at 2 presses / 8 releases per scan pass, 11 columns at 10 kHz, so about 1.1 ms per pass. The scan wiring tables are `fm1_key_pin[41]`: indices 0..13 are buttons, 14..40 are keys.
- Holding **OCT− and OCT+** at power-on is the community update-mode convention (fm1-custom-fw `REVERSE_ENGINEERING.md:188`). The fm1_open SDK handles updates itself.

---

## 3. Display

- **API** (`fm1.h:108-139`, implemented in the app at `app/board/lcd.inc`, ST7789 on SPI1):
  - `fm1_screen_init`.
  - `fm1_screen_fill(x,y,w,h,rgb565)`: blocking.
  - `fm1_screen_blit(...)`: blocking, byte-swaps through a 240-pixel row.
  - `fm1_screen_submit(x,y,w,h,pixels,count)`: non-blocking DMA of at most `FM1_SCREEN_DMA_PIXELS` = 240×16. It byte-swaps the buffer *in place* and borrows it until `fm1_screen_pending()==0`.
  - All of these are control-task only. Native-endian RGB565.
- **`screen.c` renderer, reusable as is:**
  - A 16-row band compositor: `paint_pixels[240*16]` (`:35`), dirty-band tracking with `paint_mark`/`paint_left`/`paint_right` and a priority mask.
  - `draw()` gathers state, then `paint_flush()` (`:1649`) composes and submits bands for at most 4 ms per control poll. Each draw primitive first marks damage (`paint_rendering=0`), then rasterizes when its band is rendered.
  - Primitives:
    - `paint_fill` (`:51`)
    - `frame` (`:375`)
    - `line` (`:382`, Bresenham)
    - `text(x,y,str,scale 1..4,fg,bg)` (`:80`)
    - `number` and `decimal` (`:330-355`)
    - `note_name` (`:356`)
    - `knob(col,label,value,color)` (`:417`, the four bottom parameter tiles)
    - `header` (`:440`, the BPM/transport bar)
    - `algorithm_draw` (`:2932`, DX7 algorithm diagrams from `algorithms[32][12]`, adaptable to the 8 TQ5/DX21 algorithms)
  - Font: 5×7 column-major `font[36][5]` (digits and A–Z, lowercase folded) plus a `punctuation` table, at 6×7 px per cell × scale.
  - Colours: `BACK, PANEL, TEXT, ACCENT, DIM, GREEN, AMBER` (`:8-14`) and per-track `colors[8]`.
  - Caching: signatures (`signature()`) per region in `struct draw_cache` avoid redraws.
- **Recommendation.** Copy the first ~440 lines of `screen.c` (paint engine, text, primitives) into `app/fm4_ui.c`. Then write FM-4 pages on top of them inside `groove_screen_init`/`groove_screen_control`.

---

## 4. MIDI

- **Input, short messages.**
  - `fm1_app_midi_received(source, msg, n)`: source 0 = DIN/TRS, 1 = USB, 2 = BLE. It runs in the routing task, with complete messages only.
  - Stock (`midi_router.c:319-330`) filters clock by the chosen clock source, forwards to `fm1_midi_post` (the lib engine queue) and enqueues Thru.
- **Output.**
  - `fm1_midi_send(msg,n)` sends to all ports.
  - `fm1_midi_send_port(port,msg,n)` uses per-port bounded queues and returns 0 / −1 invalid or offline / −2 full (retry).
  - Control task only. **Complete messages of 128 bytes or less** (`fm1.h:26-33`); the simulator rejects anything over 128 (`web/engine/Simulator.c:163`).
  - Output ports exist for USB and BLE only. The TRS jack is input only (`fm1_midi_output_ports` returns 6 in the simulator, and the README says "DIN, USB and BLE MIDI input, with USB/BLE output").
- **Router (`midi_router.c`).**
  - Device config `struct groove_midi_config` (8 bytes): input channel nibbles, per-track mode (local / local+MIDI / MIDI), port flags, LOCAL_OFF, CLOCK_OUT, LIVE_OUT and Thru.
  - Defaults: tracks 1–7 use channels 1–7, track 8 uses channel 10 (`groove_midi_defaults`, `:45`).
  - Local key routing: `groove_midi_local` (`:275`) finds every non-OFF track whose input channel matches, posts once to the engine, and echoes out if LIVE_OUT.
  - Sequencer output: SRAM `enqueue` lanes (128 events each), drained by `groove_midi_poll` in the control task with per-port note ownership and recovery (All Notes Off, etc.).
  - FM-4 can keep this file almost unchanged for 8-part channel mapping. Replace the `fm1_midi_post` calls with the FM-4 queue.
- **SysEx in.** `fm1_app_sysex(msg,n)` delivers complete F0..F7 frames of **1024 bytes or less**; larger or interrupted frames are discarded and USB, BLE and UART assemble separately.
  - It is called from `fm1_midi_input_sysex` ← `midi_scheduler_task`, the **routing task**, even though the header says "Control task hook".
  - Firmware-update messages are matched and consumed before this hook (`fm1_midi_input_sysex` references `fm1_update_busy` and `platform_enter_mask_usb_update`).
  - `fm1_midi_input_source()` is valid only inside the hook.
  - Pattern (`transfer.c:38-52`): validate the header, copy into a one-slot mailbox (`incoming`, 192 B), `csync`, set the length, call `fm1_control_request()`. Then process in `groove_transfer_poll` on the control task.
  - **No Flash writes in the hook.**
- **DX7 SysEx in fm1_open.**
  - Single voice: `F0 43 0n 00 01 1B <155> sum F7`, 163 bytes. Accepted in `fm1_app_sysex` (`:45`), checksum-checked and imported to every track listening on channel n (`transfer.c:272-289`).
  - Single voices are **never sent** by the firmware.
  - **32-voice bank (4104 B):** never travels as one SysEx frame in either direction. The browser (`sdk/WebUpdate.js:486-536`) converts .syx ↔ raw VMEM. The device moves 4096 raw bytes through the private project protocol (`transfer.c:9-15`):

    ```
    F0 7D 'F' 'M' 02 cmd tokLo tokHi <LSB-first 8→7 packed payload> checksum F7
    ```

    - Download: `cmd 1 [kind=4, bank]`, then repeated `cmd 2 [offLo, offHi, n≤64]`.
    - Upload: `cmd 3 [kind, target, sizeLo, sizeHi]`, `cmd 4 [off, ≤64 B]`…, then `cmd 5 [CRC32]`.
    - Replies set cmd bit 6 and are 96 bytes or less, so they fit the 128-byte limit. Exact retries get the cached reply. One session per port.
- **TQ5 plan.**
  - Accept TQ5/DX21-family single-voice VCED/ACED dumps and parameter changes directly in `fm1_app_sysex`; they are well under 1024 B. Reply with single-voice dumps, which are under 128 B only if the format allows. A TQ5 VCED of about 101 B fits; a combined VCED+ACED may not.
  - For 32-voice VMEM bulk (4104 B) and anything over 1024/128 B, do what fm1_open does: a host tool or web page splits and reassembles through a chunked private protocol. Reuse `transfer.c` `command()` and its packing and reply cache.
  - Sending a large dump as several `fm1_midi_send_port` calls (F0 head, middle, F7 tail) violates the "complete message" contract. It is untested on hardware; try it only as an experiment.

---

## 5. Storage

- **API** (`fm1.h:141-167`, `fm1_device.h:28`):
  - `fm1_flash_size()` is 536,576 bytes, or 0 if invalid. `read`, `write` and `erase` work in 4 KiB sectors.
  - `fm1_flash_map(off,n)` gives read-only XIP pointers.
  - Control task only, with interrupts enabled. Writes verify 1→0 transitions and are not transactional.
  - The partition lies at Flash 0xFF000 − 536576 = 0x7C000..0xFF000 (`Package.cs:105`).
- **Stock layout** (offsets within the partition; `project.c:13-22`, `instrument.h:326-334`, `fm_library.c:14`):

| Offset | Content |
|---|---|
| 0 .. 15×4 KiB | 16 project slots, one `struct record` = header{magic, generation, target, crc, commit} + 4064 B project + settings = exactly 4096 B. Slot 15's sector also carries device settings |
| 16×4 KiB (65536) | shared **staging** sector (journal) |
| 69632 | sample bank, up to `GROOVE_SAMPLE_LIMIT` |
| 516096 | 4 DX7 library banks, 1 sector each: magic `FM1B`, version, index, crc, commit + 32×112 B 7-bit-packed VMEM (`fm_library.c:15`) |
| 532480 | tuning and session sector |

- **Commit protocol (`program_record`, `project.c:427`).**
  1. Erase the sector.
  2. Write header and body with `commit = 0xFFFFFFFF`.
  3. Write the commit word as 0.
  4. Read back and check the CRC (`groove_crc32`, nibble table, `:53`).

  A replacement is first committed to staging, then copied to its target. `recover_staging` (`:449`) finishes an interrupted copy before the next write. Generations use wrap-safe comparison (`latest`, `:130`). Readers accept either copy.
- **FM-4 recommendation.** Own the whole partition with FM-4 magics and do not reuse stock records. Stock data may be present after moving from stock, so validate magics and never auto-erase.

  | Sectors | Content |
  |---|---|
  | 0–1 | system settings, A/B with generation |
  | 2–5 | 100 user voices: 4 × 32 slots of 128 B TQ5 VMEM (or 112 B packed as in `fm_library.c`) + header and CRC |
  | 6 | effects presets |
  | 7–8 | 8 multis (part: voice, channel, level, pan, detune, note range, FX send ≈ 32 B × 8 parts × 8 multis = 2 KiB) |
  | 9 | staging journal |
  | 10.. | 8 songs, each 2–8 sectors depending on the event format |

  Keep `struct header { magic, version, generation, target, crc, commit }` and the staging → target sequence. Debounce settings writes as `project.c:832` does (1.5 s). There is about 500 KiB available.

  > **Correction (2026-11-02).** "About 500 KiB available" is true for FM-4 itself but ignores the
  > way back to stock. Partition offsets 0x17000 and up are flash 0x93000..0xFEFFF, which stock V15
  > uses for its settings store (JieLi VM, 0x93000..0xE8FFF, factory header `55 AA AA 55` at
  > 0x93000), Bluetooth data (0xE9000) and factory voices (0xEA000..0xFBFFF). A stock restore
  > rewrites only 0x4000..0x92FFF, so anything FM-4 writes above 0x93000 stays behind and stock
  > reads it as its own. FM-4 1.0 put songs 3–8 there (song 3 exactly on 0x93000). FM-4 1.1 keeps
  > everything in offsets 0..0x16FFF (flash 0x7C000..0x92FFF, inside the stock image) and has a
  > PREPARE_STOCK command; see `fm4_core.h`, `docs/update_protocol.md` §3.5 and `docs/RECOVERY.md`.

---

## 6. Build

**Exact pipeline (`sdk/Build.ps1`):**
- **Version checks:** `app/version.h` must contain `FM1_RELEASE_VERSION "Vnn"` and `FM1_UPDATE_PRODUCT "FM-1_8nnnnnn"` (`:289-294`).
- **Manifest checks:** every file in `manifest.json` "files" is hashed. Any extra file under `sdk/` is rejected (`:332-344`). The toolchain must match 22 hashes of Windows `.exe`/`.dll`/header files (`:304-315`). `synth.json` must match the hashes of `fm1_block.h` and `libfm1_engine.a`. `app/synth/engine.h` must say `FM1_DSP_ABI 3U`.
- **Sources (`:348-349`):** every `.c` and `.S` under `app/` recursively, *except* `app/synth/`, sorted. **New files under `app/fm4/` are compiled automatically.**
- **Includes:** `sdk/include`, `sdk/include/libc`, `app/synth`, `sdk/config`, `app`, `app/board`.
- **Flags (`:358-362`):**

  ```
  -target pi32v2 -integrated-as -mcpu=r3 -mfprev1 -Wuninitialized -Wno-invalid-noreturn -fno-common -flto
  -fallow-pointer-null -fprefer-gnu-section -femulated-tls -Wno-shift-negative-value -Wframe-larger-than=2560
  -fms-extensions -fno-unwind-tables -ffunction-sections -fdata-sections -fmessage-length=0
  -cl-single-precision-constant -mllvm -pi32v2-large-program=true -D__GCC_PI32V2__ -O2
  ```

  Plus `-fwrapv -fno-fast-math -ffp-contract=off` for every file.
  - `-fno-lto` for `screen.c`, `transfer.c`, `midi_router.c`.
  - `-fno-lto -fno-pic` for `board/panel.c`.
  - `-Oz` for `project`, `transfer`, `samples`, `screen`, `presets`, `midi_router`, `fm_factory`, `fm_library`.
  - `-fno-inline-functions` for `screen.c`.
  - These per-file lists match exact paths (`:376-383`), so a new file needing `-fno-lto` or `-Oz` requires a Build.ps1 edit.
- **Generated:** `build_identity.c` (layout, `fm1_build_id` = SHA-256 of all `app/` files plus the manifest hash, and `fm1_update_identity`).
- **Link (`:414-424`):**
  - `pi32v2-ld.exe` with the LLVMgold plugin and LTO plugin options.
  - `--whole-archive libfm1_engine.a`, then `libc.a` and `libcompiler_rt.a`.
  - `--gc-sections -Tgenerated/linker`.
  - `linker.ld.in` with the placeholders `@SBC_DECODER@`, `@ACL_TX_EXTRA@` and `@ACL_RX_EXTRA@` replaced.
- **Then:** Test-Firmware.
- **Packaging (`Package-Firmware`, `:226`, run for `-FirmwareOnly` or `-Package`):**
  1. `llvm-objcopy -O binary` on `.text`, `.data`, `.ram0_data` and `.cache_ram_data`, concatenated into the app image.
  2. `Package.cs` `Create()` builds the 1 MiB flash image: uboot.boot, isd_config, `app.bin` + `cfg_tool.bin`, VM, BTIF and the USER region, encrypted with chip key 0x980F and CRC16. It wraps that in a UFW container with `ota.bin`, embedding the product id every 47 bytes.
  3. Output: `build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc`.
- **App-size limit:** **453,921 bytes** of image, including the SRAM code and data copies (`.ram0_data`), not just Flash `.text` (`Package.cs:141`).
- **`-Package` additionally:**
  - Requires `FM1_SOURCE_URL` (https).
  - Embeds the hash-pinned stock `sdk/firmware/FM-1_015.fwsc` as base64 "stock" recovery firmware in the web package JSON (`:436-447`).
  - Runs `tools/Build-Web.ps1` to produce the single-file workbench HTML. FM-4 only needs `-FirmwareOnly`.
- **Pinning (`manifest.json`):** it pins `sdk/Build.ps1`, `Package.cs`, `linker.ld.in`, `layout.h`, the headers, libs, JS, `synth.json`, the stock `.fwsc` and the pack tools. `manifest.json` itself is **not** pinned and is excluded from its own check (`:341`). Editing `Build.ps1` therefore works if FM-4 also updates its SHA-256 in `manifest.json`. That makes the check advisory, so document the fork. FM-4 must not add files under `sdk/`.
- **Linux build (pwsh 7 + JieLi Linux toolchain 20250324.1).** `Build.ps1` needs edits (unverified):
  1. Path separators. `$full.StartsWith($sdk + '\')` (`:334`) fails for every file on Linux. The same applies to the `app/synth` exclusion (`:349`) and the `$root + '\build\'` check (`:356`). Use `[IO.Path]::DirectorySeparatorChar`.
  2. Tool names. Replace `clang.exe`, `pi32v2-ld.exe`, `llvm-nm.exe`, `llvm-objcopy.exe` and `LLVMgold.dll` with the Linux layout. Felucca uses `pi32v2/bin/clang`, `pi32v2/bin/ld` and `common/bin/objcopy`/`objdump`. Whether the Linux tarball ships `llvm-nm` and an `LLVMgold.so` LTO plugin must be checked. Without the plugin, drop `-flto` and the plugin options; the lib has no bitcode, so only app LTO is lost.
  3. Toolchain hashes. Skip or replace the `external_toolchain` hash block (`:304-315`) with Linux hashes.
  4. The vendor `nm` output format, used by the regex at `:41` and the T/N kinds, may differ.
  5. Codegen may differ between 2.5.3 and the 20250324.1 clang 4.0.1 build, so hardware test the result.

  `Package.cs` compiles under pwsh 7 `Add-Type` on Linux. It needs no Windows APIs.
- **GitHub Actions windows-latest.**
  - The README points to `https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe`. That host was blocked from this sandbox (HTTP 403), so the installer type is unknown.
  - Try in order: `innoextract`, `7z x`, a silent flag (`/S` or `/VERYSILENT`).
  - Cache `C:\JL\pi32` and verify the 22 manifest hashes. That verification is the real guarantee, whichever extraction method works.
  - JieLi's licence forbids redistributing the toolchain, so do not commit or publish it. The mvave-fm1 container hit geo-blocking on JieLi hosts, so expect flaky downloads and use `continue-on-error` plus a cache.

---

## 7. Web simulator (WASM)

- **`tools/Build-Web.ps1`:**
  - Downloads WASI SDK 27 for Windows, hash-pinned, or uses `FM1_WASI_SDK`.
  - Compiles every `.c` under `app/` (excluding `app/board/`), `web/` and `third_party/synth` in **one** clang invocation:

    ```
    -std=c11 -O2 -fwrapv -fno-fast-math -ffp-contract=off -DFM1_HOST_TEST -DFM1_OPEN_WEB -Wall -Wextra -Werror
    -mexec-model=reactor -Wl,--strip-all -Wl,-z,stack-size=131072 -Wl,--initial-memory=4194304 -Wl,--max-memory=33554432
    ```

    Includes: `web/engine`, `web/synth`, `third_party/synth`, `sdk/include`, `app/synth`, `app`.
  - Base64-embeds the result into `sdk/Update.html.in` together with `Simulator.js`, `WebUpdate.js`, `I18n.js` and `Samples.js`.
  - `-LocalPreview` writes `build/preview/index.html`.
- **What the WASM contains:**
  - `web/engine/sound.c`, `sequencer.c`, `edit.c`, `effects.c`: a C re-implementation of the whole closed lib API, including `fm1_engine_render` at `sound.c:1201`, `fm1_web_init` (mirrors `fm1_app_init`) and `fm1_web_control` (mirrors `fm1_app_control`).
  - `web/synth/*`: the DX7, PSG and sample synths.
  - `web/engine/Simulator.c`: SDK stubs (Flash in a RAM array, LCD `pixels[240*240]`, a MIDI capture ring, controls) and `EXPORT("sim_*")` entry points:
    - `sim_boot`
    - `sim_control(kind,index,value)`: kind 0 = button, 1 = key, 2 = encoder, 3 = knob
    - `sim_render()`: one 128-frame block, calls `fm1_engine_render` + `fm1_output_mix`, returns float stereo, and runs control every 10 ms
    - `sim_pixels`, `sim_state`, `sim_midi_*`, `sim_file`, …
  - The real `app/screen.c`, `project.c`, `transfer.c` and so on are compiled unchanged under `FM1_HOST_TEST`.
- **Browser side:** `sdk/Simulator.js` (pinned) runs the module inside an AudioWorklet (`FM1Processor`, `:145`). It calls `sim_render` per block and resamples to the context rate. It also posts `sim_pixels` to a 240×240 canvas and wires the faceplate controls.
- **FM-4 plan.**
  - Build with wasi-sdk 27 for Linux and the same flags. A plain shell script is enough.
  - Keep `Simulator.c` and its `sim_*` exports.
  - Replace the groove re-implementation with:
    - the FM-4 engine (the same C as the firmware's `fm4_render`);
    - a thin `fm1_engine_render` shim that renders silence;
    - stubs for the `groove_*` symbols the FM-4 UI still calls.

    Same flags and the same exports as the firmware build, so the identical FM-4 UI and DSP run in the browser.
  - `Simulator.js` expects groove-specific exports (`sim_lcd_targets`, `sim_select`, …). For FM-4, a small custom page is simpler: AudioWorklet + canvas + panel → `sim_control`. Reuse the `audioThread`/`FM1Processor` pattern.

---

## 8. Recommended FM-4 hook-in (summary)

1. **Replace `app/screen.c` with the FM-4 UI.** Keep the file name, or edit Build.ps1, so it stays `-fno-lto`. It defines `groove_screen_init` and `groove_screen_control`.
   - In `groove_screen_init`: set all 8 groove tracks to `GROOVE_TRACK_OFF`, call `fm1_controls_init` and `fm1_screen_init`, initialise FM-4.
   - In `groove_screen_control`: read controls, call `groove_master_update(knob)`, drive the FM-4 UI, call `paint_flush`, and keep live references to `fm1_ble_*`, `fm1_bluetooth_*` and `groove_tuning_capture/apply`.
2. **Edit `app/board/audio.c`.** Keep `fm1_engine_render(rendered)`, then `fm4_render_add(rendered)` (`FM1_AUDIO_CODE`, integer math, SRAM tables via `FM1_DSP_DATA`), then `fm1_output_mix`.
3. **Edit `app/midi_router.c`.** Route `fm1_app_midi_received` and panel notes into an FM-4 SPSC event ring, consumed at the start of `fm4_render`.
4. **Edit `app/transfer.c`.** Keep the mailbox and chunked protocol and add TQ5 SysEx parsing. Keep `fm1_app_sysex` strong.
5. **Storage.** Replace `project.c`, `presets.c`, `samples.c`, `fm_factory.*` and `fm_library.c` with small stubs for the hooks the lib needs (§1.2) plus FM-4 storage (§5). This also brings the image under 453,921 B.
6. **Before every flash:** verify `firmware-report.json`, `sdk.map` `_MALLOC_SIZE` (at least 48 KiB) and audio `peak_ticks`/`overruns` on hardware.
