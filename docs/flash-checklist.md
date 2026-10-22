# First flash on hardware — pre-flight and rollback

Target: `build/ctrl.fwsc`, identity `FM-1_9299`, 609,632 B.

Two goals: **1. do not brick the unit. 2. enumerate as a USB MIDI device and emit
MIDI events.**

## Why bricking is unlikely here

| layer | who owns it | does our package touch it |
|---|---|---|
| 0x00000–0x04000 flash head: JieLi SDK header, JLFS entries, SPL (`uboot.boot`), `isd_config`, chip key | factory / bootloader | **never** — `fm1pkg_make.flash_image` writes from 0x4000 up only ("The update loader never writes the head") |
| update loader (`ota.bin`) | written by the OTA, staged at 0x01C0A800 | yes, replaced — but only after the app has answered the handshake |
| app area 0x04000.. | this package | yes |

Consequences:

- **The SPL survives any app we write**, so `OCT−` held at power-on still enters update
  mode, and the custom bootloader + `m-flasher restore` still work.
- **The app writes no flash at runtime.** `plat_store_load/save` are stubs returning
  `-1` (`plat_ctrl.c`); the only writer is `ota_session()`, and it runs only after an
  explicit M-UPGRADE SysEx.
- Identity `FM-1_9299` sits in the Felucca-family 9xx band, so any Felucca-lineage
  installer accepts the unit and can write over it.

Failure modes that remain, and what they look like:

| symptom | meaning | recovery |
|---|---|---|
| black screen, no USB device | app faults before `usb_start()` | `OCT−` + power on → update mode; reinstall melodee/Felucca |
| USB device but silent keys | app runs, scan/wiring wrong | same |
| unit not enumerated at all, screen shows nothing | app never handed over | `m-flasher restore` from the full-flash backup |

## Pre-flight

1. **Backup already taken** with `m-flasher` (full flash, custom bootloader installed).
   Confirm the backup file is complete and readable *before* flashing.
2. Charged unit, USB **data** cable, direct to the computer (no hub).
3. Close Ableton / any MIDI app / other browser tabs holding the FM-1 port.
4. Know the update-mode chord: **`OCT−` held while switching on**. Then the unit appears
   as an installer target and any `.fwsc` can be written.

## Flash

```sh
python3 /tmp/fm1-x0x/tools/fm1_install.py build/ctrl.fwsc
```

Watch the handshake: the installer sends M-UPGRADE, the running firmware stages the
loader, the loader writes the image. If step 1 stops after a couple of requests, nothing
was written — power-cycle and retry.

## What success looks like

1. Screen draws `fm1-ctrl` / `EVERY CONTROL EMITS`.
2. The port appears as **"FM-1 ctrl"** (manufacturer `fm1-ctrl`) — not melodee's name.
3. In a MIDI monitor:
   - a key: `90 35 64` (F3) … `90 4F 64` (G5), release `80 35 00`;
   - a button: `B0 28 7F` (FX) / `B0 28 00`. **If the CCs come out rotated by two**
     (FX arriving as `B0 30`), the panel table is wrong — that is exactly the bug
     RemoteFM1's protocol caught; report it with the label→CC you observed;
   - an encoder click: `B0 14 41` (CC 20, +1) / `B0 14 3F` (−1);
   - the MASTER pot: `B0 1B xx` (CC 27) plus `B0 07` / `B0 27` (14-bit pair).
4. LEDs: send `90 3C 64` from the host → key C4 lights. Send `B0 36 7F` → FX lights.

## First five minutes, in order

1. Does it boot and show the screen? (else: recovery above)
2. Does the port enumerate as "FM-1 ctrl"?
3. One key — correct note number?
4. One button — correct CC? (this is the panel-table check)
5. One encoder click — `B0 14 41`?
6. The MASTER pot — `B0 1B`?
7. Host note → key LED lights?

## Known gaps, so they are not mistaken for bugs

- **LEDs are on/off only.** `fm1_led_key(id, on)` drives the lit line; the HAL's
  dim/glow levels are not used yet.
- **No velocity.** Every key sends 100.
- **Panel calibration is not persisted** (`plat_store_save` is a stub), so the compiled
  `PANEL_DEFAULT` table runs. Correct for a stock FM-1; wrong only if the unit was
  calibrated differently by an earlier firmware.
- **No crash reporter.** `fm1_guard_enable()` is not called, so a fault ends in the
  watchdog reset rather than a screen of hex.
- The screen is static: it draws the title once and never updates.
