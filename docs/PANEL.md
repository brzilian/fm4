# FM-4 panel guide

FM-4 turns the M-VAVE FM-1 into a Yamaha TQ5-style 4-operator FM tone generator: 8 voices of polyphony (4–16 selectable), 8-part multitimbral, the TQ5's 10 effect programs, an 8-track song sequencer, and TQ5-compatible SysEx.

```
 [MASTER]  [SELECT]      +--------+   [KNOB1][KNOB2][KNOB3][KNOB4]
 [PRESETS] [ALGORITHM]   |  LCD   |   [FX] [SEL] [ENV] [LFO] [EDIT] [GLO]
 [OCT-] [OCT+]           |        |   [HOME][SAVE][ARP] [SEQ] [PLAY] [REC]
 ─────────────────────────── 27 keys (F2 … G4 at octave 0) ───────────────────────────
```

## Controls that work everywhere

| Control | Function |
|---|---|
| **PRESETS** | Voice number (00–99) of the active part. Turning past 99 or below 00 switches between the PRESET and USER banks. |
| **ALGORITHM** | Algorithm 1–8 of the active voice (on the SONG page: selects the song track). |
| **KNOB 1–4** | The four parameters shown in the tiles at the bottom of the screen. |
| **MASTER** | Output volume. |
| **OCT− / OCT+** | Keyboard octave −3…+3. Press both together to reset. The button lights when shifted. |
| **Keys** | Play the active part. Velocity is fixed (SYSTEM page 2, KEY VEL). |
| **HOME + PLAY** | All notes off (panic) and stop the song. |

Pressing the lit page button again steps to its next sub-page (shown as `1/3` in the header).

## Pages

| Button | Page | SELECT | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
|---|---|---|---|---|---|---|
| **HOME** | Play (TQ5 *Easy Edit*) | Bank PRESET/USER (single) or part (multi) | TONE (modulator levels) | ATTACK (all operators) | RELEASE (all operators) | FX BALANCE |
| **EDIT** 1/3 | Operator frequency | Operator 1–4 | FREQ (ratio 0.50–25.95, or Hz when fixed) | FINE | DETUNE −3…+3 | LEVEL (OUT 0–99) |
| **EDIT** 2/3 | Operator wave | Operator | WAVE W1–W8 | FIXED on/off | RANGE (fixed) | OP ON/OFF |
| **EDIT** 3/3 | Operator scaling | Operator | VELO (KVS) | LVL SCL (LS) | RATE SCL (RS) | AM enable |
| **ENV** 1/2 | Envelope | Operator | ATTACK (AR) | DECAY1 (D1R) | D1 LEVEL (D1L) | DECAY2 (D2R) |
| **ENV** 2/2 | Envelope | Operator | RELEASE (RR) | EG SHIFT | REVERB rate | EG BIAS |
| **LFO** 1/2 | LFO | — | SPEED | DELAY | PITCH MD | AMP MD |
| **LFO** 2/2 | LFO | — | WAVE saw/square/tri/S&H | KEY SYNC | PITCH SENS | AMP SENS |
| **SEL** 1/3 | Voice common | — | FEEDBACK | TRANSPOSE | BEND RANGE | POLY/MONO |
| **SEL** 2/3 | Portamento & wheel | — | PORTA time | PORT MODE | MW PITCH | MW AMP |
| **SEL** 3/3 | Breath & aftertouch | — | BC PITCH | BC AMP | AT PITCH | AT AMP |
| **FX** | Effect (global, stored with the voice) | — | EFFECT type | TIME | BALANCE | part SEND |
| **GLO** 1/3 | System | — | MODE single/multi | RX CHANNEL | MASTER TUNE | VOICES (polyphony) |
| **GLO** 2/3 | System | — | SYSEX RX | PROTECT | KEY VELOCITY | CLOCK int/MIDI |
| **GLO** 3/3 | System | — | LOCAL on/off | KEY OUT (keys → MIDI out) | TX CHANNEL | — |
| **SAVE** | Store voice | Name cursor | CHARACTER | — | — | destination USER slot |
| **ARP** (MULTI) 1/3 | Multi part setup | Part 1–8 | CHANNEL | VOLUME | VOICES (reserve) | PAN |
| **ARP** (MULTI) 2/3 | Part range | Part | LOW KEY | HIGH KEY | DETUNE | NOTE SHIFT |
| **ARP** (MULTI) 3/3 | Part | Part | FX SEND | ENABLED | — | — |
| **SEQ** (SONG) 1/2 | Song | Song 1–8 (when stopped) | TEMPO | MEASURES | QUANTIZE | LOOP |
| **SEQ** (SONG) 2/2 | Song tools | Song | CLICK | TRACK → PART | CLEAR TRACK (turn) | SAVE SONG (turn) |

**Storing a voice:** press SAVE, set the name with SELECT (cursor) and KNOB 1 (character), choose the USER slot with KNOB 4, then press SAVE again. Any other page button cancels.

## Song sequencer

- **PLAY** starts/stops the current song. **REC** starts recording on the track chosen with ALGORITHM on the SONG page, playing the active part; press REC or PLAY to stop. The take is quantized (QUANTIZE) and merged into the song.
- **SEQ + white key 1–8**: mute/unmute tracks 1–8.
- Songs are 1–99 measures of 4/4 at 96 PPQ, up to 4096 events each while playing. 8 songs are kept in Flash (SONG page 2: SAVE SONG); a saved song holds up to 1018 events (about 500 notes). A longer song is not saved and the screen shows SONG TOO LONG; clear a track or shorten it and save again.
- FM-4 1.1 changed the storage layout so that nothing is ever written where stock V15 keeps its data. On the first start after updating from 1.0, songs move to the new layout (SONGS MOVED; songs over 1018 events keep their first 1018 events: OLD SONGS SHORTENED).
- CLOCK = MIDI follows external MIDI clock and Start/Stop/Continue.

## Single and multi mode

- **SINGLE:** part 1 plays on the basic receive channel (GLO RX CH) with all voices.
- **MULTI:** 8 parts, each with its own channel, voice, volume, pan, key range, detune, note shift, FX send and voice reserve (TQ5/TX81Z "max notes"). SELECT chooses which part the keys and edit pages address.

## MIDI

- Program change 0–99 selects a voice in the part's current bank; CC0 = 0/1 selects PRESET/USER.
- Recognised controllers: mod wheel (1), breath (2), foot (4), portamento time (5), volume (7), pan (10), expression (11), sustain (64), all sound off (120), reset controllers (121), all notes off (123), mono/poly (126/127), pitch bend, channel aftertouch.
- SysEx: see [SYSEX.md](SYSEX.md).
