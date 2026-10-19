# RemoteFM1 — what to take, what to skip (2026-10-15)

Upstream: <https://github.com/Quixotic7/RemoteFM1>, installer
<https://quixotic7.github.io/RemoteFM1/installer/>. GPL-3.0-only, built on TransitFM1 →
Felucca platform, M1 on one unit (2026-10-18). Read: `README.md`, `PLAN.md`,
`docs/PROTOCOL.md`, `docs/SAFETY.md`, `web/installer/{fm1ota.js,fm1pkg.js}`.

## What it is

The FM-1 becomes a **USB terminal**: every key/button/encoder/MASTER goes up as plain
MIDI; the host sends LEDs (5 levels) and the whole 240×240 screen back as SysEx or a
vendor bulk pipe. A firmware compiled as an `.fm1core` runs on the computer and uses the
real panel. It is the same "paper-thin controller" idea as fm1-ctrl, one level further:
RemoteFM1 also takes over the screen and adds a host protocol.

**Same goal for the input half, so its protocol is authoritative on the panel wiring.**

## Confirmed and adopted

### 1. Button/encoder matrix numbering (was a live bug in fm1-ctrl)

`docs/PROTOCOL.md` §4 gives the matrix ids, cross-checked against `firmware/hal/fm1_input.h`
and `panel.c`:

| matrix button id | label |
|---|---|
| 0 | OCT− |
| 1 | OCT+ |
| 2..13 | FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC |

fm1-ctrl emitted `MAP_BTN_CC[matrix_id]`, i.e. CC 40 for matrix 0 → **every button's CC
was rotated by two on hardware** (OCT− sent FX's CC). Fixed by
`keyscan_btn_map()` + `panel_matrix_btn()` (panel.c's `PANEL_DEFAULT` table, until then
dead code). Host tests keep raw matrix ids; the device glue installs the translation.

Encoders: matrix order is not label order either — `PANEL_DEFAULT.enc = {0,1,6,2,3,4,5}`
maps SELECT/ALGO/PRESETS/K1..K4 onto matrix 0,1,6,2,3,4,5. Adopted via
`panel_matrix_enc()` / `panel_dir_enc()`.

### 2. MASTER pot: 14-bit CC 7 / 39

`PROTOCOL.md` §4: `BE 07 v14>>7` then `BE 27 v14&0x7F`, `v14 = adc << 4`, sent on a
change of ≥ 2 counts. fm1-ctrl never read the pot at all. Adopted verbatim in
`ctrl_poll_master()` (≤ 50 Hz). Note `BE` = channel 15 (0-based 14) — RemoteFM1 puts
keys on channel 16 (0x9F) and buttons on channel 15 (0x9E); fm1-ctrl keeps everything on
channel 1 for melodee compatibility, so only the *controller numbers* are borrowed.

### 3. Encoder delta splitting

"A turn of more than 63 detents is split into several messages of at most ±63, so
nothing is lost." Adopted in `encoders_step()`.

### 4. The OTA lesson that matters most (a real brick risk in fm1-ctrl)

`PLAN.md` §10 M0 log and `docs/SAFETY.md` document TransitFM1 firmware that:
- did not drain its MIDI-in ring → after ~40 DAW events EP1 OUT NAKed forever and the
  updater's SysEx was shut out;
- stopped in step 1 after 2 header reads, leaving the unit in the chip's ROM boot
  (`WL80UBOOT1.00`).

fm1-ctrl had the same class of defect, found by auditing our own port against this
document: **`FELUCCA_OTA` was `#define`d *after* `#include "usb.c"`**, so usb.c compiled
with the OTA path out — no M-UPGRADE handler, `ota_frame_get` undefined — while our main
loop never called `ota_service()` either. A unit flashed with that build would have
answered no installer: unrecoverable without the unbricker. Both fixed
(`ctrl.c` macro moved above the include, `ctrl_main.c` services `ota_service()` +
`usb.ota_req` + drains the ring every loop).

### 5. Identity/port discovery for an installer

RemoteFM1 answers installers as the official V15 (`FM-1_015`) and is found by **HELLO,
never by port name** (CoreMIDI keeps the first firmware's name). fm1-ctrl answers
`FM-1_9299`, a 9xx Felucca-family identity, which is the right band — see
`fm1ota.js:classifyFirmware`: `9xx` identities are told apart by the INFO text
(`^felucca`, `^melodee`, `^choralroot`). Our firmware answers the INFO request only when
the backup protocol path is present*; worth checking on hardware that
`classifyFirmware` would not call us "an unknown Felucca-based firmware".

\* `ota.c` answers cmd `0x11` (identity) and `0x30` (read) — the `F0 7D 46 4C 01 F7` INFO
of Felucca/ChoralRoot is a different vendor id and is **not** answered. If a Felucca-family
installer refuses us, printing `FELUCCA v1.0` from `ota_reply_identity`'s sibling would
fix it. Not needed for M-UPGRADE, which is what `fm1_install.py` uses.

## What to skip

- **The whole screen/tile/transport layer** (`TILES`, `FRAME_ACK`, PAL/RLE/COPY, vendor
  bulk EP3, WebUSB). fm1-ctrl's screen stays local; melodee owns its own UI.
- **Session/keepalive protocol** (`HELLO`/`SESSION`/`PANEL_R`). Only useful once a host
  runs the firmware; our host is an emu page, not a core runner.
- **Their LED model** (11 columns × 4-5 levels packed per column). We drive the same
  hardware through Felucca's `fm1_led_key(id, on)` helpers, which is less code.
- **Their build system** (Docker, TransitFM1 base). We already cross-compile natively
  with the JieLi pi32v2 toolchain.

## Worth stealing later

- **`fm1pkg.js` / `fm1ota.js` as a browser installer.** They are dependency-free ES
  modules that read `.fwsc`, verify identity, drive both update steps over WebMIDI, and
  resume a cut-off install. GPL-3.0-compatible with our tree. A `fm1-ctrl` install page
  on GitHub Pages would then need no Python and no cable gymnastics — and our package
  already parses: `new Package(ctrl.fwsc).product === "FM-1_9299"`, `validate` ok.
- **Their safety framing** (`docs/SAFETY.md` cut-point test matrix: cable pull, OCT− at
  power-on, loader identity) as the checklist for our first flash.
- **`OTA` resume UX**: "the device stays in update mode until it is finished; replug and
  press Install again". Our `ota_session()` already retries, but this is the text our
  README should carry.

## License note

RemoteFM1 is GPL-3.0-only and ours is GPL-3.0-or-later (README) with a stale MIT
`LICENSE` file at the repo root. Copying code from them is fine under GPL-3.0-or-later,
but the root `LICENSE` should say GPL-3.0 to match the vendored Felucca files (37 of
them). Documentation facts (protocol tables, CC numbers) are not copyrightable and were
used as a cross-check, not copied.
