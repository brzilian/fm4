# FM-4 MIDI System Exclusive

FM-4 speaks the Yamaha TQ5 / YS-series 4-operator SysEx dialect (details and sources in [tq5_spec.md](tq5_spec.md)), plus a small FM-4 extension for things the FM-1's USB MIDI stack cannot carry in one message.

## Device number

- **SINGLE mode:** `n` = the basic receive channel (GLO → RX CH; OMNI accepts any `n`).
- **MULTI mode:** `n` addresses the part whose MIDI channel is `n`.
- Replies are sent with `n` = TX CH.

## Received

| Message | Bytes | Effect |
|---|---|---|
| VCED voice | `F0 43 0n 03 00 5D <93> cs F7` | edit buffer |
| ACED | `F0 43 0n 7E 00 21 "LM  8976AE" <23> cs F7` | edit buffer (fixed freq, fine, wave, EG shift, reverb rate, foot ctrl) |
| ACED2 | `F0 43 0n 7E 00 14 "LM  8023AE" <10> cs F7` | edit buffer (aftertouch) |
| EFEDS | `F0 43 0n 7E 00 0D "LM  8036EF" <3> cs F7` | edit buffer (effect preset, time, balance) |
| Parameter change | `F0 43 1n gg pp dd F7` | `12` VCED (pp 0–92, 93 = op on/off), `13` ACED (0–22) / ACED2 (23–32), `24` EFEDS (pp 4–6), bank block select (pp 7), remote switch (116 PRESET, 117 USER), `10` multi (pp = 12·part + index, TX81Z PCED layout), master tune (pp 123) |
| Dump request | `F0 43 2n 03 F7` | replies VCED |
| | `F0 43 2n 7E "LM  8976AE" F7` | ACED + VCED |
| | `F0 43 2n 7E "LM  8023AE" F7` | ACED2 + ACED + VCED |
| | `F0 43 2n 7E "LM  8036EF" F7` | EFEDS + ACED2 + ACED + VCED (191 bytes, the full TQ5 voice) |
| | `F0 43 2n 04 F7` | 100 user voices as 4 × (`F0 43 1n 24 07 bb F7` + VMEM). *Experimental:* each VMEM is streamed in 128-byte pieces. |
| VMEM bulk | `F0 43 0n 04 20 00 <4096> cs F7` | **Not accepted over USB:** the FM-1 SDK drops SysEx frames longer than 1024 bytes. Use the webapp's Library, which writes banks voice by voice. |

Checksums are `(-sum) & 0x7F`; for the `7E` "LM" blocks the sum includes the 10 header characters.

## FM-4 extension (`F0 7D 46 34 …`, non-commercial ID, "F4")

| Command | Request | Reply |
|---|---|---|
| STORE | `F0 7D 46 34 01 part slot F7` — store the part's edit buffer to USER `slot` (0–99) | `F0 7D 46 34 41 status slot F7` (status 0 = ok) |
| READ | `F0 7D 46 34 02 slot F7` | the 191-byte TQ5 voice dump of USER `slot` |
| SELECT | `F0 7D 46 34 03 part bank program F7` (bank 0 PRESET, 1 USER) | — |
| INFO | `F0 7D 46 34 04 F7` | `F0 7D 46 34 44 "FM-4 1.1 TQ5" F7` (the version after `FM-4`) |
| PREPARE_STOCK (FM-4 1.1+) | `F0 7D 46 34 05 53 54 4F 43 4B F7` (`05 "STOCK"`) | `F0 7D 46 34 45 status erased remaining F7` |

Send one request at a time and wait for the reply.

**PREPARE_STOCK** is sent by the webapp right before it restores stock V15. FM-4 stops the
sequencer, erases every FM-4 1.0 song record that lies outside FM-4's storage window (FM-4 1.0
kept songs 3–8 at flash 0x93000–0xC8FFF, inside stock V15's settings store), then scans that area
again for any FM-4 record header.

- `status`: 0 = no FM-4 data outside the window (safe to restore stock), 1 = an erase failed or
  FM-4 data remains, 2 = storage unavailable (the SDK reported an unexpected user partition).
- `erased`: sectors erased (clamped to 127). `remaining`: FM-4 record headers still found.
- Only sectors that carry an FM-4 layout-1 song header are erased; stock's own data is never
  touched. The five-letter word guards against accidental triggering; anything else is ignored.
- FM-4 1.0 does not answer. It can take several seconds (each erased sector stalls audio
  briefly); wait up to 30 s.
- FM-4's own data inside the window (settings, user voices, songs) is not erased: the stock
  image overwrites that area when it is installed. Back up the user voices first.

## Not compatible

- TQ5 sequencer song dumps and the TQ5 system-setup dump are undocumented; FM-4 neither sends nor accepts them.
- Portamento parameters (VCED 65–66) are honoured by FM-4 although the original TQ5 reportedly ignores them. Pitch-EG (87–92) and the DX21 sustain/portamento/chorus flags (68–70) are stored and returned but have no effect, as on the TQ5.
