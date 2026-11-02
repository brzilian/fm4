# FM-1 firmware update, backup and restore: protocol notes and installer design for FM-4

Status: research notes, 2026-10-31. Sources are the local clones under `/root/fm4`. Line numbers
refer to those trees as checked out. Nothing here has been run on hardware. fm1_open's own web
package JSON says `hardware_validated=$false` (`fm1_open/sdk/Build.ps1:446`).

Short names used below:

| Name | Path | Licence |
| --- | --- | --- |
| **Open** | `fm1_open/sdk/WebUpdate.js`, `Update.html.in`, `I18n.js`, `Build.ps1`, `tools/Build-Web.ps1` | GPL-3.0-only WITH GPL-3.0-linking-exception |
| **Felucca** | `Felucca/web/fm1ota.js`, `fm1pkg.js`, `fm1backup.js`, `index_pkg.html` | GPL-3.0-only |
| **ORBIT** | `orbit/web/fm1ota.js`, `fm1pkg.js`, `installer.js`, `installer.html` | GPL-3.0-only |
| **Unbricker** | `MvaveFM1Unbricker/fm1_unbrick.py`, `fm1_transporter_recover.py` | MIT |
| **RE** | `fm1-custom-fw/REVERSE_ENGINEERING.md`, `fm1_flasher.py` | (no licence file; reference only) |

---

## 1. The OTA-over-SysEx update protocol

### 1.1 Three SysEx dialects on the same port

| Dialect | Wire prefix | Used for | Source |
| --- | --- | --- | --- |
| Vendor update and identity frames | `F0 <pack7(00 59 cmd len…)> F7`, which shows on the wire as `F0 00 32 45 …` or `F0 00 32 41 …` | identity, capability, read requests and responses, stock bank writes | `WebUpdate.js:159-175` |
| Raw "keys" (not packed) | `F0 22 24 35 7F F7` = UPGRADE; `F0 22 24 35 7D F7` = jump to mask-ROM UBOOT | starting each update stage; Unbricker's `softkey` | `WebUpdate.js:427,432`; `Felucca/web/fm1ota.js:11`; `fm1_unbrick.py:1612-1621` |
| Editor and library protocols (non-commercial ID `7D`) | Open: `F0 7D 46 4D 02 …` ("FM", v2). Felucca: `F0 7D 46 4C …` ("FL") | projects, voices and samples (Open); full archive (Felucca) | `WebUpdate.js:461-474`; `Felucca/web/EDITOR_PROTOCOL.md:79-81` |

`00 32 45` is **not a manufacturer ID**. It is how pack7 encodes the raw bytes `00 59 11`. RE and
`fm1_flasher.py:24` call it an "Arturia prefix", which is wrong. The logical header is `00 59`.
Felucca's hard-coded `HS_QUERY` (`Felucca/web/fm1ota.js:10`) is byte-for-byte the same as Open's
`frame(17)`. I checked this by running Open's `frame()`: `frame(17)` gives
`f0 00 32 45 00 00 00 40 7f f7`.

### 1.2 Frame format (vendor dialect)

The raw frame, before packing (`WebUpdate.js:159-163`; Felucca `fm1ota.js:39-48`):

```
00 59 <cmd> <len lo> <len hi> <len b2=0> <payload: len bytes> <~sum(payload) & 0xFF>
```

- **7-bit packing** is a continuous LSB-first bitstream: 8 data bytes become 7 wire bytes,
  rounded up (`pack7` `WebUpdate.js:154-158`; `unpack7` Felucca `fm1ota.js:30-37`). The packing
  covers the whole raw frame, so it is **not** the DX7-style "MSB byte per 7" packing.
  `fm1_flasher.py:30-48` uses the wrong scheme, and its own header says its flashing does not
  work (`fm1_flasher.py:6-9`; RE:365).
- **Length** is a 3-byte little-endian count of the payload bytes. Open's parser requires
  `len == raw.length-7` (`WebUpdate.js:172`).
- **Checksum** is a single byte, `~(sum of payload bytes) & 0xFF`. It covers the payload only,
  not the header. There is no CRC per chunk and no host-side signature. Image integrity rests on
  the CRC16s inside the package and the host's SHA-256 checks (§1.6). RE §2.3 (RE:228-235)
  confirms there is no asymmetric signature.
- Size limits: Open caps an outgoing payload at 520 bytes (`WebUpdate.js:160`) and accepts
  incoming frames of 10–608 wire bytes (`WebUpdate.js:165`).
- RE §4.3 (RE:276-290) gives a different checksum and field description, taken from AL-255's
  notes. The code in Open, Felucca and ORBIT is consistent across all three and works in
  their test simulators, so treat RE §4.3 as unreliable.

### 1.3 Commands

| cmd | Direction | Payload | Reply | Source |
| --- | --- | --- | --- | --- |
| `0x11` (17) identity | host → device, empty payload | — | 27 bytes of ASCII product, NUL-padded (34 raw bytes in total) | `WebUpdate.js:252-256`; Felucca `fm1ota.js:60-66` |
| `0x31` (49) capability (Open only) | host → device, empty payload | — | 47 bytes: `"FMC1"`, ver=1, **family (≤7)**, state (0=idle), loader type=1, `u16 512` (max chunk), `u32 1048576` (flash size), 32-byte **build_id**, error byte | `WebUpdate.js:219-223` |
| `0x32` (50) arm (Open only) | host → device | `01, family, u32 image_len, build_id[32]` | `[0]` = accepted | `WebUpdate.js:423-425` |
| `0x30` (48) read request | **device → host** | `fl, u32 addr, u24 len` (8 bytes) | — | `WebUpdate.js:347-348`; Felucca `fm1ota.js:50-58` |
| `0x30` (48) read response | host → device | `fl, u32 addr, u24 len, data[len]` | — | `WebUpdate.js:367`; Felucca `fm1ota.js:39-48` |
| `0x30` finish | device asks for `addr=0xE0000000` (stage 1) or `0xF0000000` (stage 2), `len=8` | host replies `"success\0"` | — | `WebUpdate.js:350-361`; Felucca `fm1ota.js:158-161` |
| `0x21`/`0x22` (33/34) stock bank write | host → stock V15 | `33: [5,0,bank>>8,0,0]`; `34: [5,off lo,off hi,0,0,0,2,0, 512 bytes]` | cmd `0x00` reply `[0]` | `WebUpdate.js:383-400` |

### 1.4 Session flow and flow control ("the device asks, the host answers")

The update is **device-pull** (Felucca `fm1ota.js:4-8`; RE:259-274):

1. **Identify.** Send `0x11` and wait for the product string (Open: 750 ms × 2 tries per port,
   `WebUpdate.js:251-254`; Felucca/ORBIT: 1 s × 2–3 tries, `fm1ota.js:96-109`).
2. **(Open only) Capability and arm.** Send `0x31`, then check the family, `state==0` and that
   `build_id` differs from the one already installed. Then send `0x32` with the length and tag
   (`WebUpdate.js:419-426`).
3. **Stage 1: the running firmware stages the loader.** Send the UPGRADE key
   `F0 22 24 35 7F F7`. The device then sends `0x30` read requests for parts of the *logical
   image* (header, entry table, the ota.bin loader). Answer each one. It ends with a read of
   `0xE0000000`; answer `"success\0"` (`WebUpdate.js:427-428`; Felucca `fm1ota.js:181-185`). The
   device then reboots into the loader. RE:267-269 says it re-enumerates as USB `4D4A:4155`.
4. **Wait for the loader**, which reports an identity of `ota-<product>`. The vendor loader
   always reports **`ota-FM-1_015`**, even when Open started it (`WebUpdate.js:145-149`). The
   Felucca loader reports `ota-FM-1_9xx`. Open waits up to 40 s, Felucca/ORBIT up to 30 s.
5. **Stage 2: the loader writes.** Send UPGRADE again. The loader reads the whole image
   (requests of at most 512 bytes; Open rejects anything larger, `WebUpdate.js:349`) and ends
   with `0xF0000000` (`WebUpdate.js:432-433`). The idle timeout is 180 s (`WebUpdate.js:243`).
6. **Reboot and verify.** Wait for the product identity (40 s). Open then reads the capability
   again and requires `build_id == pkg.build_id`, the same family and `state==0`
   (`WebUpdate.js:442-443`). Felucca and ORBIT compare only the identity string
   (Felucca `fm1ota.js:197`).

Flow control and timing:

- Open sends each response as soon as it receives the request. Pacing defaults to 0
  (`WebUpdate.js:364-366`), because the request itself is the flow control. Felucca and ORBIT
  add `sleep(10)` before each response (`fm1ota.js:164`) and `sleep(2000)` after UPGRADE
  (`fm1ota.js:182`).
- Open counts progress by byte coverage, because requests can overlap or repeat
  (`WebUpdate.js:368-373`). It stops if the device stays silent for longer than `idle`, or for
  longer than 3×idle in total (`WebUpdate.js:343-346`). It also answers repeated finish requests
  for 600 ms before closing, so the reply is not lost in Web MIDI's send queue
  (`WebUpdate.js:353-360`).
- Port discovery: Open accepts names matching `/fm.?1|^usb.?midi/` for the application and a
  wider pattern for the loader (`ota|sinco|composite…`). It refuses when more than one candidate
  answers (`WebUpdate.js:224-239`, `273`). Felucca and ORBIT probe only ports matching
  `PORT_RE` (ORBIT `fm1ota.js:37`; Felucca `fm1ota.js:122`).
- Felucca's editor rule applies to the update stream too: one request in flight at a time. The
  device drops a second frame while it is busy (`Felucca/web/EDITOR_PROTOCOL.md:451-460`). That
  file also says editors must **never** send the `22 24 35` keys or `00 59` frames
  (`EDITOR_PROTOCOL.md:463-465`).

### 1.5 Identity strings: how version and product are reported

| Identity | Meaning | Source |
| --- | --- | --- |
| `FM-1_015`, `FM-1_014` | M-VAVE stock V15 / V14 (version number `0xx`) | `Unbricker fm1_unbrick.py` `describe_identity` (~line 1676); RE:255 |
| `FM-1_8NNNNNN` (e.g. `FM-1_8000014`) | Official Open (fm1_open) builds. Set by `FM1_UPDATE_PRODUCT` in `app/version.h:6`; `Build.ps1:292` and `Package.cs:139` both enforce `FM-1_8\d{6}` | `WebUpdate.js:58` `openIdentity` |
| `FM-1_9xx` (e.g. `FM-1_900`, `_904`, `_920`) | Felucca family (Felucca, SLOOP/ORBIT, ChoralRoot…) | ORBIT `fm1ota.js:14-18`, `installer.js:59` |
| `FM-1_000` | Felucca-family USB rescue boot | `fm1-custom-fw/README.md:191`; ORBIT test `test_web.mjs` "recovery mode" |
| `ota-FM-1_015` | Vendor loader (stock **and** Open packages) | `WebUpdate.js:148-149` |
| `ota-FM-1_9xx` | Felucca loader (`FELUCCA-LOADER-1` marker) | ORBIT `fm1ota.js:18`, `fm1pkg.js:95,119` |
| `FM1_OFFICIAL_OPEN`, `FM1_COMMUNITY_n` | Also accepted by `openIdentity` | `WebUpdate.js:58` |

Felucca and ORBIT parse the identity as `<model>_<decimal version>` (`fm1ota.js:60-66`), so
`FM-1_8000014` parses as model `FM-1`, version 8000014.

**For FM-4:** `Package.cs` will only embed an `FM-1_8NNNNNN` identity (`Package.cs:139-140`).
FM-4 therefore shares a namespace with official Open releases. Pick a number range that M-VAVE
is unlikely to use, and rely on `build_id` (`Build.ps1:390-408`) as the real identity.

### 1.6 The `.fwsc` container

The builder is `Package.cs:111-137` (`Wrap`) and `73-110` (`BuildFlash`). Parsers are
`WebUpdate.js:60-104`, Felucca `fm1pkg.js:76-209` and ORBIT `fm1pkg.js:7-125`.

1. **Identity interleave.** In the file, each of the first 20 blocks of 47 bytes is followed by
   one marker byte, `char + i + 1`. The value `0x7D` means "no character". The product name is
   read from these markers. The **logical image** the device addresses is the file with those
   20 bytes removed, so `image.length = file.length - 20` (`Package.cs:128-135`;
   `WebUpdate.js:62-69`; Felucca `fm1pkg.js:211-228`). Some SMK-37 packages use 36 blocks
   (ORBIT `fm1pkg.js:11-25`); FM-1 packages use 20.
2. **UFW header (64 bytes)**, XOR-ciphered with the JieLi CRC16 LFSR keystream, key `0xFFFF`
   (`decodeHeader`, `WebUpdate.js:53-56`):
   `+0 u16 CRC16(hdr[2..63])`, `+2 u16 CRC16(entry table)`, `+4 u32 total length`, `+8 u16 entry
   count`, `+10 u16 = 4`, `+12 u32 = 512`, `+16 "AC791N"` (`Package.cs:125-126`).
3. **Entry table**: `count × 80` bytes, each entry ciphered on its own. Fields:
   `+0 u16 type`, `+2 index`, `+4 u16 CRC16(data)`, `+8 u32 offset`, `+12 u32 size`,
   `+16 u32 stored (32-aligned)`, `+24 encrypted length`, `+64 name`. Types: **0 = flash.bin**,
   **100 = ota.bin (the loader)**, 50 = USR (factory voices), 52 isd_config.ini, 2 info.log,
   251 script.ver, 161 blimit.bin, 255 tail.bin. I ran Open's inspector on the bundled stock
   file: it has 8 entries; flash.bin is 602112 bytes (0x93000) at offset 1024, and ota.bin is
   19969 bytes. Open packages contain only flash.bin and ota.bin (`Package.cs:116`).
4. **flash.bin** is a full JieLi flash image from 0x0: boot head, `uboot.boot`,
   `isd_config.ini` with chip key `0x980F`, `app_dir_head` at `0x4000`, `app.bin` at `0x4120`,
   then VM/PRCT/BTIF/USER/EXT_RESERVED records. It is SFC-ciphered with the chip key
   (`Package.cs:73-110`). Unbricker states that the OTA installers rewrite only the firmware
   region `0x4000..0x92FFF` (`MvaveFM1Unbricker/README.md:215-218`).
5. **CRC16** is CRC-16/XMODEM (poly 0x1021, init 0).

What each tool verifies before sending anything:

| Check | Open `inspectPackage`/`prepare` | Felucca | ORBIT `validatePackage` |
| --- | --- | --- | --- |
| Outer SHA-256 against a pinned value or manifest | yes, `meta.sha256` (`WebUpdate.js:130,142`) | stock only (`fm1pkg.js:237-246`) | manifest SHA + size (`installer.js:258-261`) |
| Header CRC, length, `AC791N`, table CRC | yes (`:71-74`) | header + table (`:103-112`) | yes (`:99-113`) |
| Per-entry bounds, no overlap, CRC of plain entries | yes (`:80-86`) | flash.bin CRC (`:126`) | all files (`:107-112`) |
| Exactly 1 flash.bin (16 KiB–1044480, 4 KiB aligned) and 1 ota.bin (<32 KiB) | yes (`:90-92`) | — | — |
| Loader identity | `sha256(ota.bin) == meta.loader_sha256` (`:144`) | — | requires `FELUCCA-LOADER-1` marker (`:119-120`) |
| Product matches metadata | yes (`:139,144`) | `productOf == meta.product` (`index_pkg.html:153`) | identity matches manifest (`installer.js:264-266`) |
| family == 7 | yes (`:141`) | — | — |
| Stock: USR bank decrypts and its factory voices hash to a pinned value | yes (`:94-102,151-153`) | layout pinned (`fm1pkg.js:241-244`) | — |

### 1.7 Families, `update_family=7` and the loader

- `Build.ps1:346` hard-codes `update_family=7` and compiles it into
  `fm1_layout = {user_bytes, family}` (`Build.ps1:398-405`). The device reports it in the
  capability reply. The web JSON has `family=7` (`Build.ps1:444`), and `prepare()` refuses any
  other value (`WebUpdate.js:141`).
- In `connect()`, a device on a different family, or one that does not answer `0x31` at all
  (stock V15, Felucca family), is classed as **migration**. That requires the explicit "force"
  route (`WebUpdate.js:290-317`). The arm command includes the family, and the post-reboot
  check requires it again (`:424`, `:443`).
- `loader_sha256` is the SHA-256 of `sdk/tools/pack/ota.bin`, the vendor loader
  (`Build.ps1:446`): `ef1f2db8…004541`. When it matches, Open expects the loader identity
  `ota-FM-1_015`. Otherwise it expects `ota-FM1C_<build_id[0:16]>` (`WebUpdate.js:148-149`).
  The loader decides the flash layout (`WebUpdate.js:4-5`). That is why ORBIT refuses to resume
  through a loader that is not its own (ORBIT `fm1ota.js:14-18`, `installer.js:66-68`).
- **Consequence for FM-4:** an fm1_open-built package carries the vendor `ota.bin`. During the
  update it shows as `ota-FM-1_015`, exactly like stock. ORBIT's installer treats that as a
  "foreign" loader and refuses it, and ORBIT's `validatePackage` rejects a package without the
  Felucca loader marker. ORBIT's page cannot install FM-4 unchanged.

### 1.8 What is irreversible, the risks, and how each installer mitigates them

| Stage | State after an interruption | Reversible? |
| --- | --- | --- |
| Identify, capability | no change | yes |
| Arm (0x32) / stage 1 before the `0xE0000000` finish | Felucca says "nothing was written" (`fm1ota.js:185`). RE says the device writes a JieLi `UPDATA_PARM` boot record when stage 1 completes (RE:268) | generally yes |
| After the stage-1 finish | the device boots into the loader and stays in update mode until a write completes (Felucca `fm1ota.js:189`; ORBIT `installer.js:35`) | no way back to the old firmware without a full write; resumable |
| Stage 2 (erase and write of 0x4000..0x92FFF) | **no A/B slot, no rollback.** If the host disconnects, the loader usually remains and can resume. If power fails on the device itself, the unit can **hard-brick**: the half-written app hangs, the SPL does not fall back to the loader, and recovery needs a USB_KEY dongle (Transporter) | **no** (`MvaveFM1Unbricker/README.md:258-266`) |
| Firmware switch (data areas) | Felucca keeps data at `0x97000..0xDFFFF`, its loader at `0xE0000` and globals at `0xFC000`. These overlap stock V15's VM (`0x93000..0xE8FFF`) and are **not erased** by an OTA install. Stock then misreads them, so presets come out corrupted | needs System > Reset Patches or `repair-settings` (`MvaveFM1Unbricker/README.md:213-236`; `fm1_unbrick.py:65-84`) |
| Open → stock | Open's USER area is 536576 bytes ending at `0xFF000` (`Package.cs:13,105`). It overwrites the USR area where stock keeps its factory voices, so Open rewrites the four stock voice banks after installing V15 | handled by `restoreFactory` (`WebUpdate.js:377-402`) |
| FM-4 → stock | The same user partition is flash `0x7C000..0xFEFFF`. Only offsets `0..0x16FFF` (flash `0x7C000..0x92FFF`) are rewritten by the stock image; everything above is stock's settings store, Bluetooth data and factory voices. FM-4 1.0 stored songs 3–8 at flash `0x93000..0xC8FFF`; they survive the restore and stock reads them as its settings | FM-4 1.1 never writes there, migrates 1.0 songs, and the page sends `PREPARE_STOCK` before the restore (§3.5) |

Mitigations:

| Mitigation | Open | Felucca | ORBIT |
| --- | --- | --- | --- |
| Backup gate before writing | checkbox "已保存…" (`Update.html.in:98,378,687`) | stock return: automatic full archive (`index_pkg.html:237-243`) | none; the page only warns (`installer.html` "During the write") |
| Typed or explicit confirm showing the device identity | `confirm()` for stock or force (`Update.html.in:689-690`) | `confirm(stockConfirm)` (`index_pkg.html:231,243`) | none beyond the Install button |
| Re-identify immediately before arming | yes (`WebUpdate.js:411-412,418-421`) | — | resume re-finds the loader (`installer.js:334-337`) |
| Refuse ambiguous or multiple devices | yes (`:265,273`) | first match wins | first match wins |
| Refuse foreign loaders | via `loaderIdentity` (`:290,329`) | no | yes (`fm1ota.js:262-265`) |
| Resume a write left in update mode | yes, mode `loader` (`:290-292`) | yes (`index_pkg.html:178-182`) | yes (`installer.js:333-340`) |
| Success declared only on verified identity / build_id | build_id (`:442-443`) | identity (`fm1ota.js:197`) | identity (`fm1ota.js:239`) |
| `beforeunload` guard | `Update.html.in:436` | `index_pkg.html:145` | `installer.js:392` |
| Detect unplug quickly | `link.lost` (`WebUpdate.js:188,346`) | `lost` (`fm1ota.js:151`) | same |
| Abort with port cleanup | AbortSignal, port open/close timeouts (`:11-43`) | — | — |

---

## 2. Backup options

### 2.1 What the browser can read over Web MIDI

| Running firmware | Readable over MIDI | API | Full flash? |
| --- | --- | --- | --- |
| **Stock V15** (`FM-1_015`) | Essentially nothing in bulk. Open notes that stock reads expose only the current edit buffer, not the bank address space (`WebUpdate.js:379-380`). There is no project or sample dump command in the sources. | — | no |
| **Open / FM-4** (`FM-1_8…`) | 16 project slots (each slot is loaded with cmd 8 and then downloaded), track timbres, DX7 voice banks A–D (kind 4, 4096 bytes each), and the full sample bank (cmd 11/15). Files are written with CRC32 headers: `FM1P`, `FM1T`, `.syx` and `.fm1s` | `FM1Web.Library` (`WebUpdate.js:590-725`, `download` `:649-655`, `readBank` `:694-697`, `samplesRead` `:671-677`); the workbench's backup steps are at `Update.html.in:264` | no |
| **Felucca family** with INFO caps bit 0 | Full musical archive: runtime project, settings, 4 project slots, user presets, FM6 patches and 3 sample slots (13 objects), as JSON with CRC32 per object | `fm1backup.js` `captureBackup` / `restoreBackup` (`:92-150`); protocol at `EDITOR_PROTOCOL.md:811-861` | no ("Requests name objects, never flash addresses", `EDITOR_PROTOCOL.md:813-814`) |
| Any loader (`ota-*`) | nothing ("An update loader cannot export a backup", `index_pkg.html:107`) | — | no |

There is **no full-flash read over Web MIDI** in any of these firmwares. The browser can send
the UBOOT key `F0 22 24 35 7D F7`, which stock V15 and the Felucca family accept
(`fm1_unbrick.py:1612-1621`). After that, though, the device is a SCSI mass-storage disk
(`WL82 UBOOT1.00`, `4C4A:8057`). Chrome's WebUSB blocks the mass-storage class, so a web page
cannot drive it. I did not find whether Open firmware handles the `7D` key: it lives in the
binary SDK, and `app/transfer.c:43-46` passes only `7D 'F' 'M' 02`, universal and DX7 SysEx to
the app.

### 2.2 Tools that need UBOOT mode (outside the browser)

- **`fm1_unbrick.py backup DIR`**: reads all 1 MiB **twice** and compares the reads, saves
  `full-flash-before.bin`, and records SHA-256s (`MvaveFM1Unbricker/README.md:49-56,77`). It
  needs Windows (admin) or Linux (root, `sg`). **macOS is not supported** (`README.md:126-128`).
  `restore` is gated on that backup, writes only `0x4000..0x92FFF`, never touches anything below
  `0x4000`, and verifies every byte (`README.md:43-66,199-211`).
- **`softkey`** sends the `7D` key over MIDI to get a running unit into UBOOT (`README.md:79`).
- **`fm1_transporter_recover.py`**, the hard-brick route: an RP2040 USB_KEY dongle with
  `dump`, `analyse` and `restore` (`README.md:84-112`).

### 2.3 What a user should do to stay safe (put this in the UI and docs)

1. Before the **first** custom install, ideally make a full-flash dump with Unbricker
   (`softkey`, then `backup`) on Windows or Linux. It is the only backup that captures stock
   settings, factory data and the boot head. Keep it. It is also the "factory-clean"
   `DUMP.bin` that `repair-settings` needs later (`README.md:231-236`).
2. Always export what the current firmware exposes (table 2.1). Check the exported file by
   reading it back (`readBackup`, `readProjectFile`) before allowing a write.
3. Use a USB data cable plugged directly into the computer, a charged or powered unit, and
   close any other MIDI app or editor.
4. Have the official `FM-1.fwsc` (V15) on disk. Know the recovery ladder: resume in the
   browser → Unbricker (UBOOT) → Transporter.

---

## 3. Restore to stock

### 3.1 fm1_open: embeds V15 and restores the factory voices

- `Build.ps1:436-443` embeds `sdk/firmware/FM-1_015.fwsc` (pinned SHA-256 `db1642b2…7edb8a`;
  `build_id = sha256`) as the `stock` sub-object of the web package JSON. That sub-object has
  `kind:'stock'`, `family:7`, the vendor `loader_sha256` and the file as base64 `data`.
  `Build-Web.ps1:118` adds a notice naming the M-VAVE download URL and Apache-2.0.
- **Redistribution is allowed.** `THIRD_PARTY.md:20,39` licenses `uboot.boot`, `ota.bin`,
  `cfg_tool.bin`, `boot-config.bin` **and `FM-1_015.fwsc`** under Apache-2.0. The conditions are
  to include the licence, keep the NOTICE and attribution, and mark modified files. Felucca,
  ORBIT and Unbricker chose not to ship it (ORBIT `fm1pkg.js:143-146`;
  `MvaveFM1Unbricker/README.md:137`). That is their choice, not a licence restriction.
- Flow (`Update.html.in:99-100,486-497,689,696-697`): select "刷回原厂 V15" (flash back to
  stock V15). This turns force on automatically. `prepare()` checks the stock pins and the
  decoded USR factory bank hash (`WebUpdate.js:139-140,147,151-153`). An Open device uses the
  `tagged` route, a stock device the `legacy` route. After reboot the identity must read
  `FM-1_015`, because stock has no build-hash capability (`WebUpdate.js:436-440`). Then
  `restoreFactory()` writes the four original DX7 banks (16 KiB) with stock cmds 33/34, 512
  bytes per acknowledged chunk (`:377-402`). If the device already runs V15, only the voices
  are restored (`:414-416`).

### 3.2 Felucca: the user supplies the file; archive first

`index_pkg.html:40-48,193-250`. The user picks `FM-1.fwsc`. `validateStockPackage` requires
exactly 699956 bytes, the pinned SHA-256 and the pinned layout (`fm1pkg.js:237-246`). The page
then:

1. finds `FM-1`;
2. checks via INFO that the firmware supports full backups (`archiveInfo`, `:208-217`);
3. runs `captureBackup` and saves the JSON;
4. asks `confirm(...)`;
5. runs `install()`.

If the device is already in the loader, the user must tick "I already saved a complete backup"
before resuming (`:227-232`). Restoring the archive later requires reinstalling Felucca and
using its editor (`:104`).

### 3.3 ORBIT

`fm1pkg.js:190-195` validates V15 (and the SMK-37 stock files), and `fm1ota.js:259-284`
`resume({product})` allows writing through a foreign loader only for this return-to-stock
case. The shipped `installer.js`/`installer.html` has **no stock-return UI**. The README points
to "the installer page" with "a backup with the editor first" (`orbit/README-SLOOP.md:52,169`).
Only the library functions and tests (`test_package.mjs`, `test_web.mjs`) exist here.

### 3.4 After going back to stock

Stale data from the previous firmware stays in VM and USR. Advise the user to run System >
Reset Patches, then Unbricker `repair-settings` if presets still look wrong
(`MvaveFM1Unbricker/README.md:213-236`). Open's `restoreFactory` already covers the voice-bank
part.

### 3.5 FM-4 → stock: keep stock's data area clean (added 2026-11-02)

A unit returned from FM-4 1.0 to V15 came up with a blank screen. What is known, and what is not:

| Fact | Evidence |
| --- | --- |
| The SDK user partition (`fm1_flash_*`, offset 0) is flash `0x7C000..0xFEFFF` | `Package.cs:13,105` (`FlashLimit 0xFF000`, `UserSize 536576`, record `USER` at `FlashLimit-UserSize`); `Build.ps1:346` |
| Stock V15's flash directory: app `0x4000..0x92FFF`, `VM 0x93000` size `0x56000`, `BTIF 0xE9000`, `USR 0xEA000` size `0x12000` | decoded from `FM-1_015.fwsc` flash.bin `app_dir_head` (cipher `0x980F ^ (p>>2)`, as `Package.cs:107`) |
| A factory unit has data only in sector `0x93000` of the VM, starting `55 AA AA 55` | `MvaveFM1Unbricker/fm1_unbrick.py` `VM_MAGIC`, `repair-settings` |
| Every stock restore (vendor loader or Unbricker) writes `0x4000..0x92FFF` only | Unbricker README; flash.bin of V15 is `0x93000` bytes |
| FM-4 1.0 songs `n` were at offset `0x5000 + n·0x9000`: song 3 (`n=2`) at flash `0x93000` exactly, songs 3–8 cover `0x93000..0xC8FFF` | `fm4_core.c` layout 1 (`OFF_SONGS`, `SONG_STRIDE`) |
| FM-4 1.0 wrote a song only on SAVE SONG; settings and voices (flash `0x7C000..0x80FFF`) are inside the stock image and are overwritten by the restore | `fm4_core.c`, `screen.c` |
| Not known: how stock's JieLi VM code reacts to foreign bytes at `0x93000`. Felucca data at `0x97000..0xDFFFF` only corrupted presets; data on the VM's first sector has not been reported before | Unbricker README "Settings corrupted after going back to stock" |

So FM-4 1.0 is a confirmed hazard for stock **if songs 3–8 were saved**, and the likeliest
FM-4-specific cause of the blank screen. If they were never saved, FM-4 left nothing in stock's
area and the cause is elsewhere (an incomplete write; see `RECOVERY.md`). Only a full-flash dump
settles it.

Fixes:

1. **Firmware (FM-4 1.1, storage layout 2).** All FM-4 records live in partition offsets
   `0..0x16FFF` (`FM4_FLASH_WINDOW`), i.e. inside the stock image; `save_record` refuses anything
   that would end beyond it; `fm4_hw_flash_size()` reports 0 unless the SDK partition is exactly
   536576 bytes (otherwise the flash address of offset 0 is unknown). Songs take 8 KiB each
   (1018 events saved). On its first start 1.1 moves 1.0 songs into layout 2 and erases the old
   records (only sectors that carry an FM-4 1.0 song header), then writes a layout marker.
2. **PREPARE_STOCK** (`F0 7D 46 34 05 "STOCK" F7`, `docs/SYSEX.md`): erases any FM-4 1.0 song
   records outside the window and reports whether any FM-4 header remains there.
3. **Installer.** For *Restore stock V15* on an FM-4: after the verified voice backup, the page
   sends PREPARE_STOCK and unlocks the install only on status 0. FM-4 1.0 does not answer; the
   page then blocks unless the user confirms that songs 3–8 were never saved (better: install 1.1
   first). The stock package is also checked against V15's firmware-region hash
   `6edf3c37…8255686` (the region the Unbricker writes) in addition to the file hash.
4. **Recovery.** Install > Recovery and `webapp/tools/fm1_dump_check.mjs` check a full-flash dump
   and print the exact jl-uboot-tool `erase` commands. Step-by-step: `docs/RECOVERY.md`.

---

## 4. Licensing

| File | SPDX / copyright | Reuse in a GPL-3.0 webapp |
| --- | --- | --- |
| `fm1_open/sdk/WebUpdate.js:1`, `I18n.js:1`, `Update.html.in:2`, `Build.ps1:1`, `Build-Web.ps1:1` | `GPL-3.0-only WITH GPL-3.0-linking-exception`. Copyright (c) 2026 珠海笙科智能科技有限公司 (M-VAVE) (`LICENSE.md:3`) | **Yes.** The exception is an *additional permission* (GPLv3 §7) to link with the binary FM1 SDK (`LICENSE.md:7-13`). It adds no restriction, and when you convey a modified copy you may keep it or drop it. Keep it on any file that ends up linked with `libfm1_engine.a` (the firmware side). |
| `fm1_open/sdk/Package.cs:1-4` | MIT (kagaimiq + M-VAVE) | yes; keep the notice |
| `sdk/tools/pack/*`, `sdk/firmware/FM-1_015.fwsc` | Apache-2.0 (`THIRD_PARTY.md:20,39`) | yes (binary). Ship the Apache-2.0 text and notices. |
| `sdk/lib/libfm1_engine.a` (inside the FM-4 firmware) | proprietary; FM1-devices-only grant (`THIRD_PARTY.md:27-35`) | firmware may be redistributed for FM-1 devices; include `THIRD_PARTY.md` |
| `Felucca/web/fm1ota.js`, `fm1pkg.js`, `fm1backup.js`, `index_pkg.html`, tests | `GPL-3.0-only`, © 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (`fm1ota.js:1-2`; `Felucca/LICENSING.md`) | yes |
| `orbit/web/fm1ota.js`, `fm1pkg.js`, `installer.js`, `installer.html`, tests | `GPL-3.0-only`, same copyright (`installer.js:1-2`) | yes |
| `MvaveFM1Unbricker/*` | MIT, © 2026 Michael P Jones (`LICENSE:1-3`) | link to it; copying code is fine with the notice |
| `fm1-custom-fw` | no licence found | do not copy code; cite only |

Constraints and attribution:

- Every JS source here is **GPL-3.0-only**. If FM-4's webapp is "GPL-3.0-or-later", the
  combined work can only be conveyed under GPL-3.0. Declare the webapp `GPL-3.0-only` (or keep
  per-file SPDX headers and state "GPL-3.0-only as a whole").
- Keep each reused file's SPDX header and copyright line. Add an `SPDX-FileCopyrightText`
  line, or a "Modified by FM-4 contributors, <date>: <what>" line, to every file you change
  (GPLv3 §5a). Add a credits section naming M-VAVE (fm1_open), Leo Kuroshita / Hügelton
  Instruments (Felucca, ORBIT) and kagaimiq (jl-misctools, via Package.cs).
- Ship (or link from the page) `LICENSE` (GPLv3), fm1_open's `LICENSE.md` and `THIRD_PARTY.md`.
  `LICENSE.md:13` requires them for distributions of exception-marked code. Add the Apache-2.0
  text when V15, `ota.bin` or `uboot.boot` are shipped, and provide corresponding source with a
  link on the page, as `Build-Web.ps1:102-113` enforces through `FM1_SOURCE_URL`.
- Do not use M-VAVE's or Hügelton's branding on the page (`Felucca/web/index_pkg.html:56`
  trademark line).

---

## 5. Recommended design for the FM-4 installer module

### 5.1 What to reuse

| Piece | Decision | Why |
| --- | --- | --- |
| `fm1_open/sdk/WebUpdate.js`: `inspectPackage`, `prepare`, `frame`/`parse`, `Link`, `pairs`, `capability`, **`Updater`** (`:53-453`) | **Reuse verbatim**, vendored as `vendor/fm1_open/WebUpdate.js` with its SPDX header | It is the only implementation that understands the protocol FM-4 will speak: `FM-1_8…` identity, family 7, vendor loader `ota-FM-1_015`, arm with build tag, and post-reboot build_id check. It also has abort, timeout and ambiguity handling, and the stock V15 path including the factory voice restore. FM-4 is built from fm1_open, so its SDK-side `fm1_update_*` handlers (`Build.ps1:75`) answer 0x31/0x32. |
| `WebUpdate.js` `Library` (`:590-725`) | **Reuse verbatim** for backup and restore when the device runs Open or FM-4 | same project file formats |
| `fm1_open/sdk/I18n.js` | Reuse, or wrap | `WebUpdate.js` errors are Chinese strings; I18n.js maps them to English |
| `Felucca/web/fm1backup.js` (`captureBackup`, `readBackup`, `BackupConnection`) | **Reuse verbatim** for devices on the Felucca family (`FM-1_9xx` with INFO caps) | the only browser-side full archive for those users |
| `Felucca/web/fm1pkg.js` `validateStockPackage` | Adapt (optional) | if V15 is user-supplied rather than embedded |
| ORBIT `installer.js`: `ERRORS`/`describeError`, `progressOf`, `createInstaller(deps)` dependency-injection pattern, `mount(doc, overrides)` | **Adapt** the UI state machine and error table; **do not** reuse its `preflight` and package checks | `validatePackage` requires the Felucca loader marker (`orbit/web/fm1pkg.js:119`) and `preflight` refuses `ota-FM-1_015` (`installer.js:66-68`), so it would reject FM-4 |
| ORBIT/Felucca `fm1ota.js` `Updater` | Do not use for writing | no capability or family check, fixed sleeps. Use as a cross-check reference and for its test simulator |
| Test harnesses | Copy and extend `FakeFM1` (`orbit/web/test_installer.mjs:497-551`, `orbit/web/test_web.mjs:1705-1765`), `FakeDocument` (`test_installer.mjs:97-116`) and the device mock in `Felucca/web/test_backup.mjs:19-48` | — |

`WebUpdate.js` is an IIFE that sets `globalThis.FM1Web`. Load it as a classic script, or
`import "./vendor/fm1_open/WebUpdate.js"` for its side effects and then read
`globalThis.FM1Web`. I checked the second approach in Node 20+ with a `globalThis.location`
stub: `inspectPackage(stock, true)` and `prepare(stockMeta)` both pass on the bundled
`FM-1_015.fwsc` and report the loader `ota-FM-1_015`.

### 5.2 Package metadata

Use the JSON that `Build.ps1:444-447` emits:
`{schema:1, product:"FM-1_8NNNNNN", profile, family:7, release_version, build_id, sha256, bytes,
loader_sha256, data|url, stock:{…}}`. Serve the `.fwsc` from the same origin through `url`.
`assetBytes` already enforces same-origin, a size cap and SHA-256 (`WebUpdate.js:106-136`).
Keep `stock` embedded, or switch it to user-supplied (see §3.1).

### 5.3 Flow

```
load ─► connect ─► identify/classify ─► backup (mandatory, verified) ─► confirm ─► write ─► verify ─► reboot/finish
```

1. **Load.** `await FM1Web.prepare(meta)` for FM-4 and `prepare(meta.stock)`. Require a secure
   context and `crypto.subtle`, as `Update.html.in:715` does. Nothing is sent to the device.
2. **Connect.** Call `navigator.requestMIDIAccess({sysex:true})` **synchronously inside the
   click** (ORBIT `installer.js:279-289`) and require `access.sysexEnabled`
   (`Update.html.in:356-357`). Create `new FM1Web.Updater(access, pkg, status)` and call
   `connect({force:false, signal})`.
3. **Identify and classify** from the returned `{identity, mode, same, current}`:
   - `tagged` (Open or FM-4): show the current build_id. If `same`, stop.
   - `migration` with `FM-1_015`: stock. Requires the explicit "install over stock" consent, then
     reconnect with `force:true` (`legacy` mode).
   - `migration` with `FM-1_9xx` or `FM-1_000`: Felucca family. Consent is required, plus the
     Felucca archive.
   - `loader` (`ota-FM-1_015`): an interrupted write. Offer **Resume** only, and only after the
     user confirms a backup already exists (as Felucca does, `index_pkg.html:227-232`). Refuse any
     other `ota-*` unless force is on.
   - anything else: refuse.
4. **Backup (mandatory).** Close the update link first (`updater.disconnect()`); only one owner
   of the port at a time.
   - Open/FM-4: `Library.connect()`; download every project slot (load cmd 8, `download(0)`),
     banks A–D (`readBank`), samples (`samplesRead`); save as `FM1P`/`.syx`/`.fm1s` in a zip;
     re-parse each file before continuing.
   - Felucca family: `BackupConnection` + `captureBackup`; `readBackup(JSON)` round-trip check.
   - Stock V15: no MIDI bulk backup exists. Require the user to tick "I made a full-flash dump
     with MvaveFM1Unbricker, or I accept losing stock settings and user patches", with a link.
   - Unlock **Install** only after the file download is triggered **and** the re-parse passes.
     Then reconnect the updater and check that the identity has not changed (`install()` already
     checks this, `WebUpdate.js:411-412`).
5. **Confirm.** Use a modal (not `confirm()`) that states: the device identity, the current →
   new build, "do not unplug or let the battery run down", "no automatic rollback", and the
   recovery link. Ask for a typed `INSTALL` when `force` or migration applies.
6. **Write.** `await updater.install({force})`. While writing: `navigator.wakeLock.request('screen')`,
   a `beforeunload` guard, disabled navigation, a phase label (stage 1 "preparing loader",
   "waiting for reconnect", stage 2 "writing"), and percentage from the status callback.
7. **Verify.** `install()` already requires the `build_id`, family and state match after reboot
   (`WebUpdate.js:442-443`). Only show "Done" when it resolves.
8. **Finish.** For Open-derived firmware, run the sample-area check and initialisation
   (`FM1EnsureSamples`, `Update.html.in:698-703`; `Samples.js:192`). Offer to restore the backup
   (projects and banks) with `Library`.

Restore-to-stock follows the same flow with `pkg = stock`: back up FM-4 data first, then
`install({force:true})`, which includes the factory voice restore. Then show the
"Reset Patches / repair-settings" note from §3.4.

### 5.4 Required UI warnings (minimum)

- Before connecting: Chrome or Edge on desktop; a USB **data** cable directly to the computer,
  no hub; close other MIDI apps and editors; a charged or powered unit.
- At backup: what is and is not included for the detected firmware (table 2.1). Projects do not
  contain samples. Banks A–D are separate. Stock V15 cannot be backed up over MIDI.
- At confirm: this replaces the firmware; there is **no rollback**; an interruption during
  writing can require Unbricker or the Transporter; switching firmware can leave old data that
  corrupts presets.
- During the write: "Do not unplug, do not close this page, do not let the FM-1 power off."
  The device restarts twice.
- On error: map each failure to a next step (ORBIT `ERRORS`, `installer.js:26-42`). If the
  loader is up, resume. If the device is silent, power-cycle and reconnect. If it shows as
  `WL82 UBOOT1.00`, use Unbricker.
- Footer: GPL-3.0-only, source link, credits, the Apache-2.0 notice for V15, and that this is
  not affiliated with M-VAVE.

### 5.5 Test strategy

- **Run the existing suites as-is in CI** (both pass locally): `node orbit/web/test_installer.mjs`
  and `node Felucca/web/test_backup.mjs`. They pin the shared wire format and the
  backup/restore ordering. Also run `node orbit/web/test_web.mjs` and
  `node orbit/web/test_package.mjs`.
- **Golden-frame tests:** `FM1Web.frame(17)` equals Felucca `HS_QUERY`; `parse()` of a FakeFM1
  identity reply; a read response checked against Felucca's `response()` for random
  `addr`/`len`/data.
- **Package tests:** `inspectPackage` on the real `FM-1_015.fwsc` (stock=true) and on the FM-4
  build output. Mutate one byte in each region (marker, header, table, flash.bin, ota.bin) and
  expect a rejection. Check that a `loader_sha256` mismatch is rejected.
- **Simulated device (`FakeFM2`)**, extending ORBIT's `FakeFM1` (`test_installer.mjs:497-551`)
  with:
  - replies to 0x31/0x32 (family, state, build_id);
  - re-enumeration with new port objects (old ones go `disconnected`);
  - vendor loader identity `ota-FM-1_015`;
  - duplicate finish requests and overlapping reads;
  - fault injection: `unplugAfter` in stage 1 and stage 2, a wrong build_id after reboot, family
    8, `state≠0`, two devices (AMBIGUOUS), no reply to 0x31 (stock), a foreign loader
    `ota-FM-1_904`, an oversize request.

  Drive the real `FM1Web.Updater` with `timing:{settle:10, reconnect:2000, idle:500}` so the
  tests run in seconds. Assert that **no `F0 22 24 35` byte is ever sent before backup and
  confirm**, as ORBIT checks (`device.upgrades === 0`, `test_installer.mjs:568-571`).
- **Stock path:** a fake V15 that acknowledges cmd 33/34 with cmd 0, then
  `restoreFactory` writes 16384 bytes that match `pkg.factory`.
- **Backup adapters:** reuse the Felucca device mock (`test_backup.mjs:19-48`). For Library, a
  mock `7D 46 4D 02` responder; assert that the install button stays disabled until the backup
  re-parses.
- **UI:** use ORBIT's `FakeDocument` and `mount(doc, overrides)` injection
  (`test_installer.mjs:83-116`) to test gating (disabled states, beforeunload, typed confirm).
- **Hardware:** a manual checklist on a sacrificial unit with a prior Unbricker dump:
  stock→FM-4, FM-4→FM-4 (new build), FM-4→stock, an unplug during stage 2 then resume, and
  Felucca→FM-4.

### 5.6 Blockers and open questions

1. **No browser full-flash backup**, and stock V15 exposes no bulk data over MIDI. For stock
   users, "mandatory backup" can only mean an acknowledged Unbricker dump, which needs
   Windows or Linux and is not available on macOS.
2. **ORBIT's installer cannot install FM-4 as-is.** It requires the Felucca loader marker and
   refuses the vendor loader. Reuse its UI pattern and tests, not its validation.
3. **The identity namespace is shared with official Open** (`FM-1_8\d{6}` is enforced by
   `Package.cs:139`). Choose a distinctive number and rely on build_id.
4. Going from stock V15 to an Open-derived firmware takes Open's **force/legacy** route
   (`WebUpdate.js:293-301`). The UI must present this as normal migration consent, not as an
   alarming "force".
5. It is unverified whether Open/FM-4 honours the UBOOT softkey (`7D`), because that handler is
   in the binary SDK.
6. `WebUpdate.js` messages are Chinese and errors mostly carry no `code`. Bundle `I18n.js`, or
   map messages to codes in a thin wrapper without editing the vendored file.
7. Unbricker's `repair-settings` has not been tested on a real unit
   (`MvaveFM1Unbricker/README.md:233-234`). Nothing in the browser flow is hardware-validated by
   us.
