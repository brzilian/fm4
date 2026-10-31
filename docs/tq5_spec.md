# Yamaha TQ5 SysEx and Sound Engine Specification

Scope: what firmware needs to be SysEx-compatible with the Yamaha TQ5 (1988) and to emulate its YM2414 (OPZ) sound engine. The TQ5 belongs to the same 4-operator family as the YS100, YS200, B200, TX81Z, DX11 and V50.

Status markers used throughout:

- **[CONFIRMED]**: stated in a primary source (a Yamaha manual) or shown by two independent sources that agree (for example, a hardware-tested editor plus the manual).
- **[COMMUNITY]**: from open-source editors tested on real hardware (Edisyn, TX81Z Programmer), but not checked against the TQ5 manual.
- **UNVERIFIED**: my best inference. Treat it as a default and check it against hardware or the manual.

## 0. Sources

| Tag | Source | What it provides |
|---|---|---|
| [YS-M] | YS100 owner's manual, MIDI Data Format appendix ("Attached list 4/5"); only OCR excerpts read, via manualslib.de/manual/36356 | Dump request list, parameter change group list, VCED and VMEM sizes, checksum framing. The YS100 and YS200 share the TQ5's data formats. |
| [MIDIORG] | midi.org forum thread "Yamaha TQ5 single voice sysex dump message" | `F0 43 20 7E "LM  8036EF" F7` returns EFEDS+ACED2+ACED+VCED, 191 bytes. `F0 43 20 03 F7` returns VCED only. |
| [EDISYN] | github.com/eclab/edisyn, `edisyn/synth/yamaha4op/Yamaha4Op.java`, `Yamaha4OpRec.java` (Sean Luke; TQ5 support reported working by a TQ5 owner) | Exact headers, byte counts, EFEDS layout, TQ5 effect list, TQ5 bank-select remote switches, 100-voice bulk format, VMEM bytes 84-93 |
| [TXP] | github.com/mgregory22/tx81z-programmer, `src/tx81z.h`, `src/txpack.c`, `src/snapshot.c`, `src/freqratios.c`, `src/dlg/voicedlg.c` | TX81Z VCED/ACED/VMEM bit packing, ranges, checksum, 64x16 ratio table, fixed-frequency formula |
| [YMFM] | github.com/aaronsgiles/ymfm (BSD-3), `src/ymfm_opz.cpp`, `src/ymfm_opz.h`, `src/ymfm_fm.ipp`, `src/ymfm_fm.h` | OPZ chip math |
| [FUR] | github.com/tildearrow/furnace, `src/engine/platform/sound/ymfm/ymfm_opz.{cpp,h}`, `fmsharedbase.h`, `doc/7-systems/opz.md` | A ymfm fork corrected using nukeykt's die analysis of the OPZ: fixed-frequency fix, LFO select bits, TL-ramp register, detune register table |
| [REV] | Music Technology, Sound On Sound and Making Music reviews of the TQ5 on muzines.co.uk (articles 83, 5206, 12783); polynominal.com/yamaha-tq5 | Product facts: 8 voices, 8 parts, a single Multi setup, 10 effects, sequencer specs |

I could not get the TQ5 manual PDF itself. The soundprogramming.net and polynominal.com PDFs are blocked from this environment, and the all-guidesbox copy is image-only. Some TQ5-specific numbers therefore depend on the YS100 manual plus Edisyn. The TQ5 manual's pages Add-2 to Add-4 (per [MIDIORG]) should be read to close the UNVERIFIED items.

---

## 1. SysEx framing

### 1.1 Status bytes

| Message | Bytes | Notes |
|---|---|---|
| Bulk dump (fixed format) | `F0 43 0n ff bh bl <data> cs F7` | n = device number = basic MIDI channel 0-15. ff = format number. bh,bl = byte count as two 7-bit bytes (count = bh*128 + bl). [CONFIRMED YS-M, EDISYN] |
| Universal bulk dump ("LM" ASCII header) | `F0 43 0n 7E bh bl "LM  xxxxxx" <data> cs F7` | The byte count **includes** the 10 header bytes. [CONFIRMED EDISYN, TXP] |
| Parameter change | `F0 43 1n gg pp dd F7` | gg = `0ggggghh` (g = group, h = subgroup). [CONFIRMED YS-M] |
| Dump request (fixed format) | `F0 43 2n ff F7` | [CONFIRMED YS-M] |
| Dump request (universal) | `F0 43 2n 7E "LM  xxxxxx" F7` (15 bytes) | No byte count and no checksum. [CONFIRMED YS-M, MIDIORG, EDISYN] |

On receive, n is matched against the basic receive channel. Edisyn always sends dumps with `0n`. On transmit the TQ5 uses its transmit channel (UNVERIFIED; this is how the TX81Z behaves).

### 1.2 Checksum

```
sum = 0
for b in covered_bytes: sum = (sum + b) & 0xFF
cs  = (0x100 - sum) & 0x7F          // equivalently (-sum) & 0x7F
```

A receiver validates with `(sum(covered) + cs) & 0x7F == 0`. [CONFIRMED TXP snapshot.c:195-204, EDISYN produceChecksum]

Covered range:

- **Fixed formats (03 VCED, 04 VMEM):** only the data bytes after `bh bl`. The count bytes are not included.
- **7E universal formats:** the 10 ASCII header bytes `"LM  xxxxxx"` plus the data bytes. The count bytes are not included. [CONFIRMED EDISYN: the checksum array starts at 'L']

### 1.3 Format and header table

| Block | Format byte | 10-byte header (ASCII, 2 spaces after LM) | Header hex | Count (bh bl) | Data bytes | Total message |
|---|---|---|---|---|---|---|
| VCED (voice edit buffer) | `03` | none | none | `00 5D` (93) | 93 | 101 |
| VMEM (32-voice bulk) | `04` | none | none | `20 00` (4096) | 4096 | 4104 |
| ACED (TX81Z additional) | `7E` | `LM  8976AE` | `4C 4D 20 20 38 39 37 36 41 45` | `00 21` (33) | 23 | 41 |
| ACED2 (DX11 additional) | `7E` | `LM  8023AE` | `4C 4D 20 20 38 30 32 33 41 45` | `00 14` (20) | 10 | 28 |
| EFEDS (TQ5/YS effect) | `7E` | `LM  8036EF` | `4C 4D 20 20 38 30 33 36 45 46` | `00 0D` (13) | 3 | 21 |
| ACED3 (V50 only, not TQ5) | `7E` | `LM  8073AE` | `4C 4D 20 20 38 30 37 33 41 45` | `00 1E` (30) | 20 | 38 |

All rows are [CONFIRMED EDISYN parse/emit code; sizes cross-checked: 21+28+41+101 = 191, matching MIDIORG].

The VMEM count byte is `20 00`. The TX81Z manual misprints it as `10 00` [EDISYN comment].

Other TX81Z "LM" headers, for reference only (a TQ5 most likely ignores them; UNVERIFIED): `LM  8976PE` PCED, `LM  8976PM` PMEM, `LM  8976S0` system, `LM  8976S1` program-change table, `LM  8976S2` effect, `LM  MCRTE0`/`MCRTE1` microtuning [TXP tx81z_meta.c].

### 1.4 Single-voice transmission order

A complete TQ5 voice is four messages, sent in this order [CONFIRMED MIDIORG, EDISYN]:

```
EFEDS (21) -> ACED2 (28) -> ACED (41) -> VCED (101)       = 191 bytes
```

On receive, accept any subset in any order. Apply each block to the edit buffer as it arrives; VCED arriving completes the voice. Edisyn's parser treats a set of blocks ending in VCED as one voice.

---

## 2. Parameter tables

Operator order in VCED, ACED and VMEM is **OP4, OP2, OP3, OP1**. This matches chip slot order M1, M2, C1, C2 (see section 6.1).

### 2.1 VCED: 93 bytes (format 03; parameter change group `12`)

Offset = parameter number pp in parameter change. Per-operator block k (k = 0 for OP4, 1 for OP2, 2 for OP3, 3 for OP1), base = 13*k.

| Off | Name | Range | Meaning |
|---|---|---|---|
| base+0 | AR | 0-31 | Attack rate |
| base+1 | D1R | 0-31 | Decay 1 rate |
| base+2 | D2R | 0-31 | Decay 2 (sustain) rate. 0 = hold |
| base+3 | RR | 1-15 | Release rate. 0 is stored but the UI minimum is 1 (UNVERIFIED on TQ5) |
| base+4 | D1L | 0-15 | Decay 1 level. 15 = full level (no decay) |
| base+5 | LS | 0-99 | Level scaling (keyboard level scaling depth) |
| base+6 | RS | 0-3 | Rate scaling (maps to chip KS) |
| base+7 | EBS | 0-7 | EG bias sensitivity |
| base+8 | AME | 0-1 | Amplitude modulation enable |
| base+9 | KVS | 0-7 | Key velocity sensitivity. V50 uses bit 3 as a sign; TQ5 ignores it |
| base+10 | OUT | 0-99 | Output level |
| base+11 | CRS | 0-63 | Coarse frequency (ratio index; see 6.3) |
| base+12 | DET | 0-6 | Fine detune, 3 = centre (-3..+3) |

| Off | Name | Range | Meaning | TQ5 |
|---|---|---|---|---|
| 52 | ALG | 0-7 | Algorithm 1-8 | yes |
| 53 | FBL | 0-7 | Feedback level (on OP4) | yes |
| 54 | LFS | 0-99 | LFO speed | yes |
| 55 | LFD | 0-99 | LFO delay | yes |
| 56 | PMD | 0-99 | Pitch mod depth | yes |
| 57 | AMD | 0-99 | Amp mod depth | yes |
| 58 | SYNC | 0-1 | LFO key sync | yes |
| 59 | LFW | 0-3 | 0 saw up, 1 square, 2 triangle, 3 S&H | yes |
| 60 | PMS | 0-7 | Pitch mod sensitivity | yes |
| 61 | AMS | 0-3 | Amp mod sensitivity | yes |
| 62 | TRPS | 0-48 | Transpose, 24 = C3 (0 semitones) | yes |
| 63 | POLY/MONO | 0-1 | 0 poly, 1 mono | yes (UNVERIFIED) |
| 64 | PBR | 0-12 | Pitch-bend range in semitones | yes |
| 65 | PM (PORT MODE) | 0-1 | 0 full-time, 1 fingered. Encoding UNVERIFIED | probably unused (portamento reportedly not implemented on TQ5) |
| 66 | PORT TIME | 0-99 | Portamento time | probably unused (UNVERIFIED) |
| 67 | FC VOL | 0-99 | Foot controller volume | UNVERIFIED |
| 68 | SUSTAIN | 0-1 | DX21/27 foot-switch flag | ignored (Edisyn: "unused") |
| 69 | PORTAMENTO | 0-1 | DX21/27 foot-switch flag | ignored |
| 70 | CHORUS | 0-1 | DX21/27 chorus | ignored |
| 71 | MW PITCH | 0-99 | Mod wheel to pitch depth | yes |
| 72 | MW AMP | 0-99 | Mod wheel to amp depth | yes |
| 73 | BC PITCH | 0-99 | Breath to pitch | yes (reviews list breath control) |
| 74 | BC AMP | 0-99 | Breath to amp | yes |
| 75 | BC PITCH BIAS | 0-99 | Breath pitch bias, 50 = 0 | yes (UNVERIFIED) |
| 76 | BC EG BIAS | 0-99 | Breath EG bias | yes |
| 77-86 | VOICE NAME 1-10 | 32-127 | ASCII | yes |
| 87 | PR1 | 0-99 | Pitch EG rate 1 (DX21/V50) | ignored |
| 88 | PR2 | 0-99 | Pitch EG rate 2 | ignored |
| 89 | PR3 | 0-99 | Pitch EG rate 3 | ignored |
| 90 | PL1 | 0-99 | Pitch EG level 1 | ignored |
| 91 | PL2 | 0-99 | Pitch EG level 2 | ignored |
| 92 | PL3 | 0-99 | Pitch EG level 3 | ignored |
| (93) | OP ON/OFF | 0-15 | **Parameter change only**, not in the dump: bit 3 = OP4 ... bit 0 = OP1 (TX81Z). [YS-M lists "p=0-92, 93"] | yes (UNVERIFIED bit order) |

Offsets and ranges are [CONFIRMED TXP tx81z.h/txpack.c, EDISYN, YS-M "93 byte"]. The "TQ5" column is UNVERIFIED unless a source is noted. [YS-M] page 60 says the parameters marked "%%%" (shared format with the DX11) do not function on the YS100/200, but the OCR did not keep the marks. The likeliest set is PEG 87-92, SUSTAIN/PORTAMENTO/CHORUS 68-70, and probably the portamento parameters 65-66.

**VCED dump example (header):** `F0 43 0n 03 00 5D d0..d92 cs F7`, where cs is computed over d0..d92.

### 2.2 ACED: 23 data bytes (header `LM  8976AE`; parameter change group `13`, pp 0-22)

Per-operator block k (k = 0 for OP4, 1 for OP2, 2 for OP3, 3 for OP1), base = 5*k:

| Off | Name | Range | Meaning |
|---|---|---|---|
| base+0 | FIX | 0-1 | 0 ratio, 1 fixed frequency |
| base+1 | FIXRG | 0-7 | Fixed range: 255 Hz, 510 Hz, 1 kHz, 2 kHz, 4 kHz, 8 kHz, 16 kHz, 32 kHz (top of range) |
| base+2 | FINE | 0-15 | Fine ratio or fine fixed frequency. **0-7 only when CRS < 4** |
| base+3 | OSW | 0-7 | Operator waveform W1-W8 |
| base+4 | SHFT | 0-3 | EG shift: 0 = off (96 dB), 1 = 48 dB, 2 = 24 dB, 3 = 12 dB. OP1 is fixed at 0 in the TX81Z UI |

| Off | Name | Range |
|---|---|---|
| 20 | REV | 0-7, reverb rate (0 = off) |
| 21 | FC PITCH | 0-99, foot controller to pitch |
| 22 | FC AMP | 0-99, foot controller to amplitude |

All rows are [CONFIRMED TXP, EDISYN]. Total ACED message: `F0 43 0n 7E 00 21 "LM  8976AE" a0..a22 cs F7` (41 bytes).

### 2.3 ACED2: 10 data bytes (header `LM  8023AE`; parameter change group `13`, pp 23-32)

| Off | pp | Name | Range | TQ5 |
|---|---|---|---|---|
| 0 | 23 | AT PITCH | 0-99 | yes (reviews: aftertouch supported) |
| 1 | 24 | AT AMP | 0-99 | yes |
| 2 | 25 | AT PITCH BIAS | 0-100, 50 = 0 (DX11 range -50..+50). Edisyn uses 0-99; UNVERIFIED | yes |
| 3 | 26 | AT EG BIAS | 0-99 | yes |
| 4 | 27 | OP4 FIXRM | 0-1 (V50 "fix range mode" LO) | ignore, send 0 |
| 5 | 28 | OP2 FIXRM | 0-1 | ignore |
| 6 | 29 | OP3 FIXRM | 0-1 | ignore |
| 7 | 30 | OP1 FIXRM | 0-1 | ignore |
| 8 | 31 | LS SIGN | bitmask (V50): bit 0 = OP1 .. bit 3 = OP4 (Edisyn's guess) | ignore, send 0 |
| 9 | 32 | reserved | 0 | |

Layout from [EDISYN]; [YS-M] parameter list: "ACED2 $13 (g=4,h=3) p=23-33". There is no separate ACED2 group; pp continues on from ACED. Total: `F0 43 0n 7E 00 14 "LM  8023AE" b0..b9 cs F7` (28 bytes).

### 2.4 EFEDS: 3 data bytes (header `LM  8036EF`)

| Off | Name | Range | Meaning |
|---|---|---|---|
| 0 | EFFECT PRESET | 0-10 | See the table below. 0 (Off) can only be set over SysEx |
| 1 | EFFECT TIME | 0-40 (reverb, gate), 0-75 (delay and echo types) | Effect time/size |
| 2 | EFFECT BALANCE | 0-99 | Dry/effect balance |

[CONFIRMED EDISYN (format and ranges, with note "Off is only available over sysex"); count 0x0D = 10+3]. Total: `F0 43 0n 7E 00 0D "LM  8036EF" e0 e1 e2 cs F7` (21 bytes).

**TQ5 effect types** [EDISYN EFFECTS list; names and count confirmed by REV]:

| Value | Effect | Time meaning / range (UNVERIFIED mapping to seconds) |
|---|---|---|
| 0 | Off (SysEx only) | n/a |
| 1 | Reverb Hall | Reverb time, up to about 10 s |
| 2 | Reverb Room | Reverb time |
| 3 | Reverb Plate | Reverb time |
| 4 | Delay | Delay time, up to about 300 ms (0-75) |
| 5 | Delay L/R | Delay time (a single repeat on each output) |
| 6 | Stereo Echo | Delay time (repeats alternate left and right) |
| 7 | Distortion + Reverb | Reverb time |
| 8 | Distortion + Echo | Echo time (0-75) |
| 9 | Gate Reverb | Gate time, up to about 3.2 s |
| 10 | Reverse Gate | Gate time |

The effect processor is the YM3413 [polynominal]. The effect is global: one effect for the whole unit (reviews: "only one effect type can be assigned per song"). It is loaded from the selected voice in single mode (UNVERIFIED in multi mode; probably taken from the Multi or the part 1 voice).

### 2.5 VMEM: 32 voices x 128 bytes (format 04)

`F0 43 0n 04 20 00 <32 x 128 bytes> cs F7` (4104 bytes). The checksum covers the 4096 data bytes. One VMEM voice record packs VCED, ACED, ACED2 and EFEDS. Voice v starts at data offset 128*v. Bytes 94-127 are unused (send 0).

Per-operator block k (OP4 at 0-9, OP2 at 10-19, OP3 at 20-29, OP1 at 30-39), base = 10*k:

| Byte | b6 | b5 | b4 | b3 | b2 | b1 | b0 |
|---|---|---|---|---|---|---|---|
| base+0 | 0 | 0 | AR4 | AR3 | AR2 | AR1 | AR0 |
| base+1 | 0 | 0 | D1R (5 bits) ||||  |
| base+2 | 0 | 0 | D2R (5 bits) ||||  |
| base+3 | 0 | 0 | 0 | RR (4 bits) |||  |
| base+4 | 0 | 0 | 0 | D1L (4 bits) |||  |
| base+5 | LS (7 bits, 0-99) ||||||  |
| base+6 | AME | EBS2 | EBS1 | EBS0 | KVS2 | KVS1 | KVS0 |
| base+7 | OUT (7 bits, 0-99) ||||||  |
| base+8 | 0 | CRS (6 bits) |||||  |
| base+9 | (V50 LS sign) | 0 | RS1 | RS0 | DET2 | DET1 | DET0 |

V50 also uses base+9 bit 3 as the KVS sign. On TQ5 it is 0, and **RS occupies bits 3-4**.

Common bytes:

| Byte | Contents |
|---|---|
| 40 | b6 SYNC, b5-b3 FBL, b2-b0 ALG |
| 41 | LFS |
| 42 | LFD |
| 43 | PMD |
| 44 | AMD |
| 45 | b6-b4 PMS, b3-b2 AMS, b1-b0 LFW |
| 46 | TRPS (0-48) |
| 47 | PBR (0-12) |
| 48 | b4 CHORUS, b3 POLY/MONO, b2 SUSTAIN, b1 PORTAMENTO, b0 PORT MODE |
| 49 | PORT TIME |
| 50 | FC VOL |
| 51 | MW PITCH |
| 52 | MW AMP |
| 53 | BC PITCH |
| 54 | BC AMP |
| 55 | BC PITCH BIAS |
| 56 | BC EG BIAS |
| 57-66 | VOICE NAME 1-10 |
| 67-72 | PR1, PR2, PR3, PL1, PL2, PL3 |
| 73 + 2k | ACED op k (k = 0 for OP4, 1 for OP2, 2 for OP3, 3 for OP1): b6 V50 FIXRM (0 on TQ5), b5-b4 SHFT, b3 FIX, b2-b0 FIXRG |
| 74 + 2k | b6-b4 OSW, b3-b0 FINE |
| 81 | REV (0-7) |
| 82 | FC PITCH |
| 83 | FC AMP |
| 84 | AT PITCH |
| 85 | AT AMP |
| 86 | AT PITCH BIAS |
| 87 | AT EG BIAS |
| 88-90 | reserved (0) |
| 91 | EFFECT PRESET (0-10) |
| 92 | EFFECT TIME |
| 93 | EFFECT BALANCE |
| 94-100 | V50 ACED3 (ignore) |
| 101-127 | unused, 0 |

Sources: bytes 0-83 are [CONFIRMED TXP txpack.c and EDISYN, which agree]. TXP lists SYNC as byte 40 **bit 7**, which is impossible in 7-bit data. Edisyn uses bit 6, and that is the value to use. [YS-M p.67] fragment: "PEG PR1 ... PEG PL3 OP.4 -EGSFT- FIX -FIXRG" matches byte 73. Bytes 84-93 are [COMMUNITY EDISYN]. Note that Edisyn's VMEM AT code masks with `& 7`, which is a bug: store the full 7-bit values.

### 2.6 TQ5 100-voice bank bulk (4 blocks of 25 voices)

The TQ5 holds 100 voices per bank, sent as four VMEM messages. Each is preceded by a block-select parameter change [COMMUNITY EDISYN emitBank/parse, tested on TQ5]:

```
repeat b = 1..4:
  F0 43 1n 24 07 bb F7                 ; bb = block 1..4 (voices 0-24, 25-49, 50-74, 75-99)
  F0 43 0n 04 20 00 <32x128> cs F7     ; voices 0..24 of the block in slots 0..24; slots 25..31 are padding
```

Total 4 x (7 + 4104) = 16444 bytes. Padding content: Edisyn copies slot 0. What the TQ5 itself sends is UNVERIFIED; on receive, ignore slots 25-31. A plain VMEM received with no preceding `24 07 bb` header goes to block 1 (UNVERIFIED). This header occupies the SYS group (`24`) at pp = 7.

The front-panel path to send the 100 user voices is JOB > BULK > 100 USER > YES [MIDIORG].

---

## 3. Parameter change messages (`F0 43 1n gg pp dd F7`)

From the [YS-M] "Attached list 5" (YS100/200), cross-checked with [EDISYN] TQ5 code:

| Group byte gg | g, h | pp range | Block | Notes |
|---|---|---|---|---|
| `12` | g=4, h=2 | 0-92, 93 | VCED | pp = VCED offset (section 2.1). pp 93 = OP on/off |
| `13` | g=4, h=3 | 0-22 | ACED | pp = ACED offset |
| `13` | g=4, h=3 | 23-32 (list says 23-33) | ACED2 | pp = 23 + ACED2 offset |
| `24` | g=9, h=0 | 0-7 | SYS (setup) | Includes the EFEDS parameters (below) and the bank block header (pp 7) |
| `24` | g=9, h=0 | 91-127 | SYS (remote switch) | Front-panel buttons |
| `10` | g=4, h=0 | 0-95 | SYS (pfm), the multi setup | Probably 8 parts x 12 parameters, TX81Z PCED style (section 3.3). UNVERIFIED for TQ5 |
| `10` | g=4, h=0 | 123 | MASTER TUNING | 0-127, 64 = centre |

### 3.1 EFEDS by parameter change (group `24`) [COMMUNITY EDISYN, TQ5-tested]

| pp | Parameter | dd |
|---|---|---|
| 4 | EFFECT PRESET | 0-10 |
| 5 | EFFECT TIME | 0-75 |
| 6 | EFFECT BALANCE | 0-99 |
| 7 | (bulk block select) | 1-4 |

pp 0-3 are UNVERIFIED. They are likely system setup items such as receive channel and program-change or sysex switches.

### 3.2 Remote switch (group `24`, pp 91-127)

dd = `7F` presses the button; dd = `00` releases it (TX81Z convention; Edisyn sends only `7F`, which works on the TQ5).

| pp | Button | Status |
|---|---|---|
| 91-100 | Ten-key 0-9 | UNVERIFIED (the [YS-M] OCR reads "switch ... 91 ten key 0, ten key 1, ten key 2 ...") |
| 101, 102 | Ten-key "-" and "+" (or -1/+1) | UNVERIFIED |
| 116 | PRESET bank button | [COMMUNITY EDISYN, TQ5-tested. The manual reportedly says 116 = card; Edisyn found 116 = preset] |
| 117 | USER (internal) bank button | [COMMUNITY EDISYN] |
| 118 | CARD bank button | [COMMUNITY EDISYN] |
| others | other panel buttons | UNVERIFIED, need the TQ5 manual page "remote switch" |

For reference, the TX81Z remote numbers live in group `13`: 64 POWER, 65 STORE, 66 UTILITY, 67 COMPARE, 68 PLAY/PERFORM, 69/70 PARAM left/right, 71/72 DATA dec/inc, 73/74 CURSOR left/right, 75 CURSOR. On the DX11, 118 = SINGLE and 119 = PERFORM [TXP, EDISYN]. Do not use these on the TQ5.

Recommended firmware behaviour: implement 116/117/118 exactly as above. For the other numbers in 91-127, map to your own buttons and document the mapping. Silently ignore unknown numbers.

### 3.3 Multi (8-part) setup

Facts [REV]: the TQ5 is 8-voice polyphonic and 8-part multitimbral. It stores **one** Multi setup, plus 7 preset "Part Types". Edisyn notes that the YS-family "multimode" is a global system setting, not a performance patch. [YS-M] lists "SYS(pfm) `$10` p=0-95", which is 96 = 8 x 12, the exact size of the TX81Z PCED instrument area. Recommended layout (UNVERIFIED for TQ5; this is the TX81Z PCED instrument layout, pp = 12*part + i):

| i | TX81Z name | Range | Notes |
|---|---|---|---|
| 0 | MAX NOTES | 0-8 | Voice reserve per part |
| 1 | VOICE No. MSB | 0-1 | On TQ5, bank select or unused (UNVERIFIED) |
| 2 | VOICE No. LSB | 0-127 | TQ5: 0-99 |
| 3 | RECEIVE CH | 0-16 | 16 = omni |
| 4 | LIMIT LOW | 0-127 | |
| 5 | LIMIT HIGH | 0-127 | |
| 6 | DETUNE | 0-14 | 7 = centre |
| 7 | NOTE SHIFT | 0-48 | 24 = centre |
| 8 | VOLUME | 0-99 | |
| 9 | OUT ASSIGN | 0-3 | off/L/R/L+R |
| 10 | LFO SELECT | 0-3 | off/inst1/inst2/vib |
| 11 | MICRO TUNE | 0-1 | probably unused on TQ5 |

There is no known dump request for the multi or system setup on the TQ5 ([YS-M] mentions a "100 byte system set up" block; its header string is UNKNOWN). Recommendation: accept the parameter changes above, and do not invent a dump format for the multi until the TQ5 manual is checked.

### 3.4 Easy Edit and sequencer

- Easy Edit on the TQ5 (EG, Tone/Brilliance, LFO, Name, Effects; changes in steps of plus or minus 10) is a front-panel macro layer over VCED. No SysEx for it is documented (UNVERIFIED). Implement it as UI-only edits that emit normal VCED/ACED parameter changes.
- 8-track sequencer: 8 songs, 999 bars, about 10,000 notes, quantise to 1/48. Its data storage format over SysEx is **not documented** in any source I found (UNVERIFIED/UNKNOWN). Songs can be saved over MIDI ([polynominal]: "MIDI exclusives") and to card. Do not attempt binary compatibility with TQ5 song dumps.

---

## 4. Dump requests the TQ5 answers

| Request | Reply | Status |
|---|---|---|
| `F0 43 2n 03 F7` | VCED (101 bytes) of the edit buffer; no effect data | [CONFIRMED MIDIORG, YS-M] |
| `F0 43 2n 04 F7` | VMEM | [YS-M] lists it. On TQ5, which 32 voices it returns (block 1 of the current bank?) is UNVERIFIED |
| `F0 43 2n 7E 4C 4D 20 20 38 39 37 36 41 45 F7` ("LM  8976AE") | ACED + VCED (41 + 101) | [YS-M] |
| `F0 43 2n 7E 4C 4D 20 20 38 30 32 33 41 45 F7` ("LM  8023AE") | ACED2 + ACED + VCED (28 + 41 + 101) | [YS-M] |
| `F0 43 2n 7E 4C 4D 20 20 38 30 33 36 45 46 F7` ("LM  8036EF") | EFEDS + ACED2 + ACED + VCED (191 bytes) | [CONFIRMED YS-M, MIDIORG, EDISYN] |
| EFEDS-only request | The manual lists one on page Add-3 [MIDIORG], but its bytes are not quoted | UNVERIFIED. Probably the same 7E form; recommend also answering it with EFEDS only |

The reply is the current edit buffer, which is the currently selected voice in the active bank (Preset, User or Card) [MIDIORG]. There is no addressed single-voice request. To fetch voice X, editors first select the bank (remote 116/117/118), then send a Program Change X, then send the request [EDISYN].

Panel edits: there is no evidence that the TQ5 transmits SysEx on its own when a voice is edited on the panel (UNVERIFIED). It transmits dumps when a bulk or voice transmit job is run from the panel (JOB > BULK). Recommended default: send nothing automatically. Optionally provide a "SysEx transmit" setting that sends parameter changes.

Reception requires the SysEx receive switch on and memory protect off (TX81Z/Edisyn convention). Received VCED/ACED/ACED2/EFEDS go to the edit buffer only; storing to memory is done from the panel [EDISYN "Gotchas"]. Received VMEM writes to user memory.

---

## 5. Program change, banks, channels, controllers

- **Banks:** Preset (100, ROM), User/Internal (100, RAM), Card (100, MCD-32 RAM card). Voices are numbered 00-99. [CONFIRMED REV, EDISYN]
- **Program change:** PC 0-99 selects voice 00-99 within the currently selected bank [COMMUNITY EDISYN]. PC 100-127: UNVERIFIED; recommend ignoring them. There is no Bank Select CC (1988 design); banks change only through the panel or remote 116/117/118. Recommend optionally also accepting CC0 = 0/1/2 for preset/user/card as an extension. The V50's PC 122/123/124 bank trick is **not** a TQ5 behaviour.
- **Channels:** basic receive channel 1-16 or omni; transmit channel 1-16 [REV: "OMNI or any channel for IN or OUT"]. In multi mode each of the 8 parts has its own receive channel (section 3.3).
- **Polyphony:** 8 voices total (the YM2414 has 8 channels), shared across parts.
- **Controllers recognised:** velocity, mod wheel (CC1), breath (CC2), foot controller (CC4), volume (CC7), sustain (CC64), channel aftertouch, pitch bend [REV]. Portamento CCs 5/65 are probably ignored. Data entry, NRPN and RPN: no. All of this list is UNVERIFIED in its CC numbers, which are the standard ones used by the TX81Z.
- **Mono/poly mode messages:** CC126/127 should switch the VCED POLY/MONO flag (TX81Z behaviour, UNVERIFIED on TQ5).

---

## 6. OPZ (YM2414) synthesis internals

All chip math is from ymfm upstream, with corrections from Furnace's nukeykt-informed fork where noted. Line numbers refer to the files as cloned (ymfm master; Furnace master).

### 6.1 Clock, sample rate, operator mapping

- Master clock: 3.579545 MHz (TX81Z; TQ5 assumed the same, UNVERIFIED). Prescale 2, 32 operator slots. Sample rate fs = 3579545 / 64 = **55930.4 Hz** [YMFM ymfm_opz.h `DEFAULT_PRESCALE = 2`, `OPERATORS = 32`].
- 8 channels x 4 operators. Chip operator order per channel: O1 (slot M1, has feedback), O2 (C1), O3 (M2), O4 (C2) [YMFM ymfm_opz.cpp:188-209].
- **TX81Z operator n maps to chip On' with n' = 5 - n:** OP4 = O1 (feedback), OP3 = O2, OP2 = O3, OP1 = O4 (always a carrier). Register slot order M1, M2, C1, C2 is therefore OP4, OP2, OP3, OP1, which is exactly the VCED order. I verified this against all 8 algorithms below.
- Output: 14-bit per channel with no intermediate clipping. Channels are summed and clamped to 16 bits [YMFM ymfm_opz.cpp:795-808].

### 6.2 Algorithms (TX81Z numbering; operators in TX81Z numbering)

From [YMFM ymfm_fm.ipp:1008-1065] `s_algorithm_ops`, mapped with On' = OP(5-n):

| ALG (VCED value) | Routing | Carriers |
|---|---|---|
| 1 (0) | OP4 -> OP3 -> OP2 -> OP1 | OP1 |
| 2 (1) | (OP4 + OP3) -> OP2 -> OP1 | OP1 |
| 3 (2) | (OP4 + (OP3 -> OP2)) -> OP1 | OP1 |
| 4 (3) | ((OP4 -> OP3) + OP2) -> OP1 | OP1 |
| 5 (4) | OP4 -> OP3 ; OP2 -> OP1 | OP3, OP1 |
| 6 (5) | OP4 -> each of OP3, OP2, OP1 | OP3, OP2, OP1 |
| 7 (6) | OP4 -> OP3 ; OP2 ; OP1 | OP3, OP2, OP1 |
| 8 (7) | OP4, OP3, OP2, OP1 | all four |

Modulation input is the sum of the modulator outputs, shifted right by 1 (`opmod = opout[x] >> 1`). Carriers are summed with clamping.

**Feedback** (OP4 only) [YMFM ymfm_fm.ipp:923-930]: `opmod = (fb[n-1] + fb[n-2]) >> (10 - FBL)` when FBL > 0. It uses the average of the last two OP4 outputs, and FBL is the VCED value 0-7 directly.

### 6.3 Frequency: ratio mode

Chip: `multiple(x.4) = (MUL << 4 | FINE)`, with MUL = 0 meaning 0.5, so the value is `0x08 | FINE`. Phase step = keycode phase step with the DT2 offset applied, plus DT1, all times multiple / 16 [YMFM ymfm_opz.cpp:428-433, 502-546]. DT2 adds a keycode delta of +0, +600, +781 or +950 cents (factors 1, 1.41421, 1.5708, 1.7321) [YMFM :506].

**CRS (0-63) to (MUL, DT2)** (derived by me; it reproduces every entry of TXP's 64x16 ratio table to 2 decimals, so treat it as reliable):

| CRS | MUL,DT2 | Ratio | CRS | MUL,DT2 | Ratio | CRS | MUL,DT2 | Ratio | CRS | MUL,DT2 | Ratio |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0,0 | 0.50 | 16 | 5,0 | 5.00 | 32 | 6,3 | 10.38 | 48 | 10,2 | 15.70 |
| 1 | 0,1 | 0.71 | 17 | 3,3 | 5.19 | 33 | 7,2 | 10.99 | 49 | 12,1 | 16.96 |
| 2 | 0,2 | 0.78 | 18 | 4,1 | 5.65 | 34 | 11,0 | 11.00 | 50 | 11,2 | 17.27 |
| 3 | 0,3 | 0.87 | 19 | 6,0 | 6.00 | 35 | 8,1 | 11.30 | 51 | 10,3 | 17.30 |
| 4 | 1,0 | 1.00 | 20 | 4,2 | 6.28 | 36 | 12,0 | 12.00 | 52 | 13,1 | 18.37 |
| 5 | 1,1 | 1.41 | 21 | 4,3 | 6.92 | 37 | 7,3 | 12.11 | 53 | 12,2 | 18.84 |
| 6 | 1,2 | 1.57 | 22 | 7,0 | 7.00 | 38 | 8,2 | 12.56 | 54 | 11,3 | 19.03 |
| 7 | 1,3 | 1.73 | 23 | 5,1 | 7.07 | 39 | 9,1 | 12.72 | 55 | 14,1 | 19.78 |
| 8 | 2,0 | 2.00 | 24 | 5,2 | 7.85 | 40 | 13,0 | 13.00 | 56 | 13,2 | 20.41 |
| 9 | 2,1 | 2.82 | 25 | 8,0 | 8.00 | 41 | 8,3 | 13.84 | 57 | 12,3 | 20.76 |
| 10 | 3,0 | 3.00 | 26 | 6,1 | 8.48 | 42 | 14,0 | 14.00 | 58 | 15,1 | 21.20 |
| 11 | 2,2 | 3.14 | 27 | 5,3 | 8.65 | 43 | 10,1 | 14.10 | 59 | 14,2 | 21.98 |
| 12 | 2,3 | 3.46 | 28 | 9,0 | 9.00 | 44 | 9,2 | 14.13 | 60 | 13,3 | 22.49 |
| 13 | 4,0 | 4.00 | 29 | 6,2 | 9.42 | 45 | 15,0 | 15.00 | 61 | 15,2 | 23.55 |
| 14 | 3,1 | 4.24 | 30 | 7,1 | 9.89 | 46 | 11,1 | 15.55 | 62 | 14,3 | 24.22 |
| 15 | 3,2 | 4.71 | 31 | 10,0 | 10.00 | 47 | 9,3 | 15.57 | 63 | 15,3 | 25.95 |

The ratios are those printed by Yamaha (truncated). FINE adds `FINE/16 x MUL'` before the DT2 factor, where MUL' = 0.5 for MUL = 0. The effective ratio is `(MUL' + FINE/16) x DT2factor`. For CRS 0-3, FINE is limited to 0-7, giving a top of 0.93 for CRS 0. The maximum ratio is CRS 63 with FINE 15 = 27.57 [TXP freqratios.c, EDISYN FREQUENCY_RATIOS_MAX].

The frequency of a note: OPM keycode (block, note, key fraction 6 bits) is converted by `opm_key_code_to_phase_step` [YMFM ymfm_fm.ipp:206ff], with `f_out = phase_step x fs / 2^20`. With fs = 55930 Hz, A4 = 440 Hz at the default master tune. Pitch bend, transpose, detune and master tune are applied by the firmware through keycode and key fraction (1/64-semitone resolution).

### 6.4 Frequency: fixed mode (ACED FIX = 1)

Firmware formula [CONFIRMED TXP voicedlg.c:526-529, EDISYN computeFrequency]:

```
base = (CRS < 4) ? (8 + min(FINE,7)) : (16*(CRS >> 2) + FINE)      // 8..255
f_Hz = base << FIXRG                                                // FIXRG 0..7 -> up to 32640 Hz
```

Chip registers: `fix_frequency = CRS >> 2` (4 bits), `fine = FINE`, `fix_range = FIXRG`, and the FIX bit is set (register 0x80 bit 5). The chip computes `freq = (fixfreq<<4 | fine)` (0 means 8), shifted left by range. DT1, MUL and DT2 are not applied, and fixed mode ignores pitch bend and LFO PM. Correct phase step: `step = (75*1024*freq) >> 12` with a 12-bit sub-step accumulator, which gives f_out = freq Hz at fs = 55930 Hz [FUR ymfm_opz.cpp compute_phase_step, "matches hardware"]. **Upstream ymfm uses `75*freq`, which is 1024 times too low. Use the Furnace version or compute `phase_inc = f x 2^20 / fs` directly.** Whether the LFO pitch modulation affects fixed operators: UNVERIFIED (ymfm: no).

### 6.5 Detune (DET 0-6)

The chip DT1 register is 3 bits; bit 2 is the sign. Firmware mapping [FUR fmsharedbase.h dtTable; UNVERIFIED but standard for OPM]:

| DET | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| shown | -3 | -2 | -1 | 0 | +1 | +2 | +3 |
| DT1 reg | 7 | 6 | 5 | 0 | 1 | 2 | 3 |

The offset is a keycode-dependent phase-step delta from `s_detune_adjustment[32][4]` [YMFM ymfm_fm.ipp:179-194], added before the multiple is applied.

### 6.6 Waveforms (OSW W1-W8)

[YMFM ymfm_opz.cpp:103-124]. One period has 1024 phase steps; the "second half" is phase 512-1023. "sin^2" here is the log-domain doubling ymfm uses, a guess "based on OPX description". In linear terms it is `sign(sin)·sin²`.

| OSW | Shape |
|---|---|
| W1 (0) | sine |
| W2 (1) | sign(sin)·sin² (narrower peaks, both halves) |
| W3 (2) | half sine: positive half of W1, second half = 0 |
| W4 (3) | positive half of W2, second half = 0 |
| W5 (4) | full W1 cycle at 2x speed in the first half, second half = 0 |
| W6 (5) | same as W5 using W2 |
| W7 (6) | two positive W1 humps at 2x speed (abs sine) in the first half, second half = 0 |
| W8 (7) | same as W7 using W2 |

The silence value is the minimum-level attenuation (`zeroval`). Log-sine table: [YMFM ymfm_fm.ipp:46-75] (256-entry quarter wave, 4.8 fixed). Exp table: [:86-131]. Waveform 2's exact curve (true sin² or a different ROM) is UNVERIFIED.

### 6.7 Envelope generator

Chip register values and rate computation [YMFM ymfm_opz.cpp:447-464; ymfm_fm.h:145-148]:

| Stage | Chip rate (6-bit) |
|---|---|
| Attack | `AR*2 + ksr` (0 if AR = 0) |
| Decay 1 | `D1R*2 + ksr` |
| Decay 2 (sustain) | `D2R*2 + ksr` |
| Release | `RR*4 + 2 + ksr` |
| Reverb | `min(REV*4 + 2 + ksr, release rate)` when REV > 0, else the release rate |

All rates clamp to 63. `ksr = keycode5 >> (RS ^ 3)`, where keycode5 = block (3 bits) plus the top 2 bits of the note.

- **Sustain (D1L to SL):** chip SL = **15 - D1L** (inferred from DX21/TX81Z semantics, where D1L 15 = no decay; UNVERIFIED). Chip SL threshold: `(SL | ((SL+1)&0x10)) << 5`, so SL 15 means maximum attenuation [YMFM :447-450].
- **Attenuation:** 10 bits (0-1023), about 0.09375 dB per step (96 dB full scale). Per-step increments come from `s_increment_table` [YMFM ymfm_fm.ipp:145-167]. Attack: `att += (~att * inc) >> 4`. Rates 62-63 are instantaneous. Decay and release increment linearly in the log domain [ymfm_fm.ipp:675-760]. EG clock divider 3 (envelope updated every 3 samples) [ymfm_opz.h `EG_CLOCK_DIVIDER = 3`].
- **Reverb:** after key-off, once attenuation reaches `0xC0` (-18 dB), the EG switches to the REVERB state and uses the reverb rate [YMFM ymfm_fm.ipp:735-739]. This gives a slower tail.
- **EG shift (SHFT):** `env_att_effective = env_att >> SHFT`. Shift 1 limits the envelope's range to 48 dB, 2 to 24 dB, 3 to 12 dB. TL and AM are added afterwards [YMFM ymfm_fm.ipp:764-785]. Upstream ymfm forces shift 0 on chip slot O1. Furnace removes this ("nothing more than a software constraint") and also skips the "quiet" early-out when shift ≠ 0. The TX81Z/TQ5 UI fixes OP1's shift at 0 [EDISYN]. Recommendation: ignore SHFT for OP1 at the firmware level, and implement shift on all operators in the engine.
- **Total level:** chip TL 0-127, 0.75 dB per step (`TL << 3` in attenuation units).

### 6.8 Output level, velocity and level scaling to TL (firmware layer)

The TQ5 CPU computes chip TL from OUT, KVS, LS, EBS and controller biases. The exact ROM tables are **UNVERIFIED** (not reverse-engineered publicly). Recommended defaults:

- `L(OUT)`: for OUT >= 20, `L = OUT + 28`; for OUT < 20, the DX7 table `{0,5,9,13,17,20,23,25,27,29,31,33,35,37,39,41,42,43,45,46}`. Then `TL = 127 - L`, so OUT 99 gives TL 0 and each step is 0.75 dB. (This is the DX7 curve, as in Dexed `scaleoutlevel`; the 4-op family uses 0.75 dB steps too.)
- Velocity: add `KVS * (127 - vel) * k` attenuation, with k chosen so that KVS = 7 at vel = 1 gives about 30-40 dB. Shape UNVERIFIED.
- LS 0-99: attenuation rising with key number above a fixed low breakpoint (about C1). Roughly `LS * max(0, key - 24) / 64` in TL units. UNVERIFIED.
- EBS 0-7: scales how much breath, aftertouch or foot "EG bias" attenuates this operator. UNVERIFIED.
- The per-channel register 0x00-0x07 is a TL-ramp speed [FUR, nukeykt], **not** a volume. Implement part volume and CC7 in TL.

### 6.9 LFO

- Waveforms [YMFM ymfm_opz.cpp:128-147]: 0 sawtooth (AM falls 255 to 0, PM rises), 1 square, 2 triangle, 3 noise S&H (17-bit LFSR, latched per LFO step). The VCED LFW order matches the chip.
- Rate: the 8-bit register is 4.4 floating point. Counter `+= (16 | r&15) << (r>>4)` per sample; phase = bits 22-29 [YMFM :333-341]. So `f_LFO = fs * ((16 + (r & 15)) << (r >> 4)) / 2^30`: r = 0xFF gives about 52.9 Hz, r = 0x30 about 0.007 Hz. Furnace: r = 0 stops the LFO.
- **LFS 0-99 to register: UNVERIFIED** (the TX81Z ROM uses a non-linear table). The specified range is about 0.007-50 Hz [polynominal]. Recommendation: `r = round(0x30 + LFS * (0xFF - 0x30) / 99)`, which is exponential in Hz.
- Depth: chip PMD and AMD are 7 bits. Recommendation: `depth = round(PMD * 127 / 99)` (UNVERIFIED). The firmware ramps depth from 0 according to LFD (the delay) after key-on, and adds mod wheel, breath, aftertouch and foot contributions (MW PITCH etc.) to the effective depth.
- PMS 0-7: about ±0, 5, 10, 20, 50, 100, 400, 700 cents at full PMD. Implemented as `pm >> (6-PMS)` for PMS < 6, `pm << (PMS-5)` otherwise, added to the keycode delta in 1/64 semitone [YMFM :509-522].
- AMS 0-3: off, then `am << (AMS-1)`, nominally 0 / 23.9 / 47.8 / 95.6 dB maximum. Applies only to operators with AME = 1 [YMFM :372-395, ymfm_fm.ipp:778-779].
- SYNC = 1 resets the LFO phase at key-on. The chip has 2 programmable LFOs (plus 2 fixed triangle LFOs per [FUR]). Each channel selects LFO 1 or 2 for PM and AM through select bits (not additive) [FUR ymfm_opz.h register 0x38 bits 7 and 2]. **With 8 parts, real hardware can run only 2 distinct LFO settings at once** (TX81Z "LFO select"). An emulator may run one LFO per part as a superset.

### 6.10 Pitch bend, transpose, portamento

- Pitch bend: ±PBR semitones (0-12), applied to keycode and key fraction, which gives a 1/64-semitone step. Fixed-frequency operators do not bend.
- Transpose: TRPS - 24 semitones.
- Portamento: reportedly not implemented on the TQ5 [polynominal] (UNVERIFIED). Recommend implementing it per VCED 65/66 anyway, as harmless when PORT TIME = 0.

---

## 7. Implementation notes: ambiguities and recommended defaults

1. **Checksum:** `(-sum) & 0x7F`. For 7E blocks the sum includes the 10 ASCII header bytes; for 03/04 it covers data only. On receive, reject bad checksums, but optionally provide a "lenient" mode, because some librarian files have wrong checksums.
2. **Accept every TX81Z, DX11 and TQ5 form:** VCED alone, ACED+VCED, ACED2+ACED+VCED, EFEDS+ACED2+ACED+VCED, and requests for each of them (8976AE, 8023AE, 8036EF). Answer `8036EF` with the full 191-byte set. Ignore ACED3 (V50).
3. **Device number:** match n against the receive channel. Transmit dumps with `0n` = transmit channel.
4. **Operator order:** always OP4, OP2, OP3, OP1 in VCED, ACED and VMEM. Engine op On' = OP(5-n).
5. **FINE with CRS < 4:** clamp FINE to 0-7, in both ratio and fixed mode.
6. **SYNC bit:** VMEM byte 40 bit 6 (not bit 7, as TXP has it).
7. **VMEM bytes 84-93:** store and return them exactly. Edisyn's `& 7` mask on the AT bytes is a bug.
8. **100-voice bulk:** accept and emit the `F0 43 1n 24 07 bb F7` + VMEM (25 used, 7 padding) sequence. Pad slots 25-31 with copies of slot 0 (matches Edisyn). Treat a lone VMEM as block 1.
9. **Remote switch:** implement 116 = Preset, 117 = User, 118 = Card (dd = 7F). The TQ5 manual's numbering for these is reportedly wrong. Ignore other numbers until the TQ5 manual "remote switch" table is transcribed.
10. **EFEDS:** a value of 0 (Off) is valid over SysEx. Clamp TIME to 0-40 for reverb/gate types and 0-75 for delay/echo types (4, 5, 6, 8). Default: preset 1 (Hall), time 20, balance 50 (UNVERIFIED defaults). The effect is global: in single mode it follows the current voice. In multi mode, use part 1's voice (UNVERIFIED).
11. **Ignored VCED parameters** (store and echo, do not use): 68-70 (sustain, portamento and chorus flags), 87-92 (PEG), ACED2 4-9 (V50). Probably also 65-66 (portamento); see 6.10.
12. **Fixed-frequency math:** use Furnace's `75*1024*freq` correction (or a direct Hz computation), not upstream ymfm.
13. **EG shift:** honour it in the engine for all operators. Force OP1 to 0 on SysEx receive only if you want strict TX81Z UI behaviour. Recommended: keep the received value and ignore it for OP1.
14. **Values that need ROM-level confirmation** (all UNVERIFIED; the defaults above are the DX7/TX81Z-style choices): LFS-to-Hz table, PMD/AMD scaling, LFO delay curve, OUT-to-TL curve, KVS curve, LS curve and breakpoint, EBS, effect-time-to-seconds tables, W2 shape.
15. **Multi and system setup:** use group `10` pp 0-95 in TX81Z PCED instrument layout and group `24` pp 0-7 for system setup, but treat both as provisional. No TQ5 dump format for the multi, system or sequencer is known. Do not transmit one.
16. **Panel edits:** transmit nothing by default. Send dumps only on request or from a JOB > BULK style menu.
17. **Program change** 0-99 within the current bank; ignore 100-127. Optionally accept CC0 0/1/2 as a bank-select extension.
18. **Next step to close the gaps:** get the TQ5 Owner's Manual MIDI appendix (pages Add-2 to Add-4 and the remote switch list), via the soundprogramming.net `Yamaha_TQ5_Manual_2.PDF` or polynominal.com `yamaha-tq5-manual.pdf`. Both are blocked from this environment. Then confirm sections 3.1-3.3 and 4.
