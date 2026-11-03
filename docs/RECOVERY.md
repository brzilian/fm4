# Recovering an FM-1 that shows a blank screen after returning from FM-4 to stock V15

Status: 2026-11-02. Written after one report from real hardware ("flashing back to stock bricked
it: blank screen at power-up"). The steps use only tools that already exist and have been used by
the FM-1 community: this repository's webapp, [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker)
(MIT) and [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) (MIT). None of the steps
below has been run by us on a unit bricked this way. Each step says what is certain and what is
not.

Nothing here ever writes below `0x4000` (the boot head). Never use `erasechip`, never let Windows
format the FM-1's disk.

## What probably happened

FM-1 flash (1 MiB), as stock V15 uses it:

| Flash | Stock V15 | Written by a stock restore? |
| --- | --- | --- |
| `0x00000..0x03FFF` | boot head | no (never) |
| `0x04000..0x92FFF` | the V15 firmware | **yes** |
| `0x93000..0xE8FFF` | stock settings store; on a factory unit only `0x93000` holds data, starting `55 AA AA 55` | no |
| `0xE9000` | Bluetooth data | no |
| `0xEA000..0xFBFFF` | factory voices | no |

FM-4 stores its data from flash `0x7C000` (the SDK's user partition). FM-4 **1.0** saved
**songs 3–8** at `0x93000..0xC8FFF`, song 3 exactly on stock's first settings sector. A stock
restore does not touch that area, so stock V15 then starts on FM-4 song data where it expects its
settings header. That is the leading suspect **if** songs 3–8 were ever saved (SONG page 2, SAVE
SONG). FM-4's settings, user voices and songs 1–2 were inside the stock firmware region and were
overwritten by the restore: they cannot cause this.

Other causes give the same blank screen: a stock write that did not finish (the unit then sits in
its update loader or with a half-written firmware). The full-flash dump in step 3 tells which.

FM-4 **1.1** fixes this: it never writes at or above `0x93000`, moves 1.0 songs out on its first
start, and the webapp asks it to confirm that its data is clear before restoring stock
(`docs/update_protocol.md` §3.5).

## Windows (recommended)

You need: the FM-1, a USB **data** cable plugged straight into the PC (no hub), a Windows 10/11 PC
where you can open an **administrator** PowerShell, and an internet connection once.

### Step 0. Downloads

1. **Python 3.9 or newer** from <https://www.python.org/downloads/windows/>. In the installer tick
   "Add python.exe to PATH". Check in a new PowerShell: `py --version`.
2. **MvaveFM1Unbricker**: <https://github.com/Quixotic7/MvaveFM1Unbricker> → Code → Download ZIP.
   Unzip it, for example to `C:\fm1\MvaveFM1Unbricker`.
3. **The official V15 package `FM-1.fwsc`**: from M-VAVE's product page
   <https://www.m-vave.com/product?id=fm-1>, or the identical copy in this repository,
   `webapp/firmware/FM-1_015.fwsc` (rename it `FM-1.fwsc`). Its SHA-256 must be
   `db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a`
   (`Get-FileHash FM-1.fwsc`). Put it in the MvaveFM1Unbricker folder.
4. Optional, for step 3: **Node.js 20+** from <https://nodejs.org/> and this repository (Code →
   Download ZIP), or simply the FM-4 webapp in Chrome.

### Step 1. See what the FM-1 shows up as

Switch the FM-1 on and plug it in. Wait 30 seconds.

**a. Is it still an FM-1 on MIDI?** Open the FM-4 webapp in Chrome (`webapp/`, served with
`py -m http.server` from that folder, then <http://localhost:8000>), go to **Install**, choose
**Restore stock V15**, press **Connect**.

- `ota-FM-1_015: vendor update loader`: the stock write was interrupted and the loader is waiting
  (its screen is blank too). Tick "I already saved a backup…", press **Resume install…**, type
  INSTALL. This finishes the write. Success = the page says "Stock V15 restored and verified" and
  the FM-1 shows its normal screen. **Stop here.**
- `ota-FM1C_<16 hex digits>: fm1_open update loader`: an update started from fm1_open-built
  firmware (FM-4, M-VAVE Open) was interrupted, or another updater armed it. Other updaters
  (e.g. BAUDGIRL's) do not recognise this loader. With **Restore stock V15** selected, tick the
  box, press **Force-resume with stock V15…** and type INSTALL: the page writes stock V15 through
  this loader (M-VAVE's own Open updater does the same under "force"). If it reports that the
  loader asked to finish its preparation step instead, wait 10 s and press Connect: an
  `ota-FM-1_015` answer is resumed as above; the same `ota-FM1C_…` answer means continue with 1b.
  Confirmed on hardware (2026-11-03): a unit stuck at `ota-FM1C_0000000000000000` after a frozen
  third-party install was restored to stock V15 this way, verified by the page.
- `FM-1_015: M-VAVE stock firmware V15` with a blank screen: stock V15 is running but stops
  before it draws, which points at its data (step 3). Put it into ROM boot with
  `py fm1_unbrick.py softkey` (step 1c), then continue with step 2.
- "No FM-1 answered": continue.

**b. Is it the ROM boot disk?** In an **administrator** PowerShell:

```powershell
Get-CimInstance Win32_DiskDrive | Format-Table Index, Model, InterfaceType
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_4C4A*' } | Format-Table Status, Class, FriendlyName, InstanceId
```

If you see **`WL82 UBOOT1.00 USB Device`** (USB `VID_4C4A&PID_8057`), the chip is in its mask-ROM
boot mode and can be repaired: go to step 2. If Windows offers to format a disk: **Cancel**.

**c. Neither?** There is no documented button combination that forces stock V15 or FM-4 into ROM
boot. These are the community's tries (MvaveFM1Unbricker README, Troubleshooting), in order:
leave it switched on for two full minutes and check again; switch off, hold OCT− and OCT+, switch
on and keep holding 10 s; three quick restarts (on 10 s, off, three times). After each, repeat
1a and 1b. If the stock firmware is actually running (1a showed `FM-1_015`), MvaveFM1Unbricker's
`py fm1_unbrick.py softkey` sends the stock key `F0 22 24 35 7D F7` over USB-MIDI and drops it
into ROM boot (type `UBOOT` to confirm; it writes nothing). If nothing ever appears with vendor ID
4C4A, the only route is the hardware **FM-1 Transporter** (a Seeed XIAO RP2040 on the USB data
lines): MvaveFM1Unbricker's `TRANSPORTER-GUIDE.md` and `fm1_transporter_recover.py` (that script
also works on macOS).

### Step 2. Rewrite the V15 firmware region (and take the backup)

In the **administrator** PowerShell:

```powershell
cd C:\fm1\MvaveFM1Unbricker
py fm1_unbrick.py setup
py fm1_unbrick.py find
py fm1_unbrick.py restore FM-1.fwsc --verify-v15
```

`setup` fetches jl-uboot-tool at the tested commit. `restore` checks the package (identity
`FM-1_015`, firmware-region SHA-256 `6edf3c37…58a8255686`), finds exactly one `WL82 UBOOT1.00`
disk, checks chip key `0x980F` and flash ID `0x856014`, reads the whole flash **twice** and
saves it as `backups\fm1-recovery-<date>\full-flash-before.bin`, then asks you to type `WRITE`. It
erases and writes exactly `0x4000..0x92FFF` and compares every byte. If it reports that verification
failed: **do not power off**, run the same command again (another cable or port helps).

When it says Done: unplug USB, switch the FM-1 off, wait 5 s, switch it on.

- **The stock screen appears:** go to step 5.
- **It said the region "already holds exactly this firmware"**, or the screen is still blank: the
  firmware is fine and something in the data areas stops it. Go to step 3. (Keep the backup folder
  in every case: it is the only copy of everything that was on the unit.)

### Step 3. Check the dump for FM-4 data

Either in the webapp (Install → **Recovery** → choose `full-flash-before.bin`; it runs in the
browser, nothing is uploaded or sent to the FM-1), or with Node from this repository:

```powershell
node webapp\tools\fm1_dump_check.mjs C:\fm1\MvaveFM1Unbricker\backups\fm1-recovery-<date>\full-flash-before.bin
```

The report says whether the firmware region is V15, whether the boot head is V15's, whether the
stock settings store starts with `55 AA AA 55`, and lists every FM-4 record it finds. For each FM-4
record at `0x93000` or above it prints an exact command, for example:

```
erase 0x93000 0x9000
erase 0xAE000 0x9000
```

(Each FM-4 1.0 song is 9 sectors, `0x9000` bytes, at `0x93000 + k·0x9000`.) If it lists **no**
FM-4 data at `0x93000` or above, FM-4 left nothing in stock's area and these erases are not the
fix: share the report and the backup folder with the community/maintainers.

### Step 4. Erase only those FM-4 sectors (jl-uboot-tool)

The FM-1 must still be in `WL82 UBOOT1.00` mode. If it now boots (even to a blank screen), get it
back into ROM boot as in step 1c. Find its disk number again (it changes after a replug):

```powershell
Get-CimInstance Win32_DiskDrive | Format-Table Index, Model, InterfaceType
cd C:\fm1\MvaveFM1Unbricker\jl-uboot-tool
py jluboottool.py --device '\\.\PhysicalDrive<Index>'
```

The Quick info must show **Chip key: 0x980F** and **ID: 0x856014**; otherwise type `exit` and
stop. At the `=>JL:` prompt type the `erase` lines from the report, exactly, one by one, then:

```text
=>JL: read 0x93000 0x56000 vm-after.bin
=>JL: exit
```

`vm-after.bin` lets you (or the checker, with a fresh `py fm1_unbrick.py backup DIR`) confirm the
erased ranges are all `FF`. Unplug, switch off, wait 5 s, switch on.

What is certain: these commands only erase sectors that start with an FM-4 record header, all
inside stock's settings store; the firmware, Bluetooth data and factory voices are untouched.
What is not: whether stock V15 rebuilds its settings when the first settings sector is erased
rather than holding its factory `55 AA AA 55` header. JieLi's settings code normally formats a
blank area (the SDK contains `vm_reset` / `_vm_area_erase`), but this has not been tried on an
FM-1. If the screen stays blank:

- MvaveFM1Unbricker `repair-settings --factory DUMP.bin` puts `0x93000..0xE8FFF` and
  `0xFC000..0xFEFFF` back to a factory unit's state. It needs `DUMP.bin`, a full-flash dump of a
  factory-fresh FM-1 (ideally your own from before FM-4; otherwise ask the community for one).
- Last resort, after a backup: `erase 0x93000 0x56000` (the whole stock settings store: stock user
  patches and settings are lost). Unverified, as above.

### Step 5. Confirm success and finish

1. The FM-1 shows the stock V15 screen after a power-on.
2. In the webapp: Install → **Restore stock V15** → **Connect** shows
   `FM-1_015: M-VAVE stock firmware V15`. Tick the acknowledgement and run **Restore stock V15…**:
   on a unit already running V15 this only rewrites the four factory voice banks (FM-4 does not
   touch them, but it makes sure they match V15). On the FM-1: System → Reset Patches.
3. Keep the backup folder. If you later install FM-4 again, use 1.1 or later.

## macOS

macOS cannot drive the `WL82 UBOOT1.00` disk: jl-uboot-tool has no macOS SCSI back end, Chrome's
WebUSB blocks mass-storage devices, and MvaveFM1Unbricker exits with "not supported" on macOS
(certain). From a Mac you can still:

- run step 1a (the webapp resumes an interrupted write over Web MIDI: works on macOS);
- run step 3 (the dump checker, webapp or `node webapp/tools/fm1_dump_check.mjs`);
- run `fm1_unbrick.py softkey` and the Transporter script (`fm1_transporter_recover.py`).

For steps 2 and 4 use a Windows PC or Linux (a Raspberry Pi works: `sudo modprobe sg`, run with
`sudo`; same commands with `/dev/sgN` instead of `\\.\PhysicalDriveN`). A Windows virtual machine
with USB pass-through (Parallels, VMware Fusion) may work, since the tool only needs the disk to be
visible to Windows, but the FM-1 re-enumerates during the process and a pass-through that drops
the device mid-write is a real risk: prefer a real PC. Melodee's project publishes standalone
recovery tools that are described as running on macOS; we have not checked them.
