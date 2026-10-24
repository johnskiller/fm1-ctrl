# Porting onto fm1-base: what it took, and what DriftSeq will meet

Written while porting fm1-ctrl (`/tmp/fm1-base/ctrl/`) from the x0x-vendored HAL to
[fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0, felucca-bsp inside).
Measured 2026-10-24. Everything below is from that build, not from the README.

## The short version

The app is **one file**. `plat.h` already owns the panel, the encoders, the supply
rails, the screen, USB-MIDI, Bluetooth MIDI and the update path, so a controller
firmware is ~330 lines and its whole job is `plat_*()` in, `plat_midi_send()` out.

What fm1-ctrl had to rewrite: **all of it.** Its `ctrl_glue.c` re-implemented the
key scan and the encoder decode against `fm1_in`, which fm1-base does not expose.
Both are gone; they are `plat_keys()` and `plat_enc()` now.

## Real numbers

| | fm1-ctrl on its own HAL | fm1-ctrl on fm1-base |
|---|---|---|
| app source | `ctrl.c` + `ctrl_glue.c` + `keyscan.c` + `encoders.c` + `leds.c` + `map.c` + `plat_ctrl.c` + `usb.c` + `ota.c` (~4000 lines, most vendored) | **`app.c`, 330 lines** |
| app image | 35,196 B | **210,996 B** (the platform is linked in) |
| package | 609,636 B | **611,496 B** |
| Bluetooth MIDI | none | **`ble = true` in fm1.toml, nothing else** |
| panel calibration kept | no (`store` stub -1) | **yes, `OBJ_PANEL` at 0xDC000** |
| host simulator | own | **`--host`, a script language** |
| browser emulator | own wasm | **`--web`, same app** |

The image is 6× larger and that is the point: the platform brings the screen, the
fonts, the screen reader, the USB descriptors, the supply-rail bring-up and the
loader. Measured build time on this machine: ~40 s device, ~8 s host.

## The contract (what an app must supply)

`app.h` declares it; seven functions, no more:

```c
void app_init(void);                                    /* boot: load your settings */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio ISR */
void app_panic(void);                                   /* silence before an update */
void app_ui_init(void);                                 /* the first screen */
void app_frame(void);                                   /* ~60 Hz: input, maybe a redraw */
void app_input_only(void);                              /* between frames: input only */
void app_say(const char *a, const char *b);             /* screen reader */
```

A controller writes zeros into `app_render`'s buffer and does its panel work in
`app_frame`. `plat_midi_in()` must be drained from `app_render` (the audio ISR) or
the USB/BLE input rings fill.

## Pitfalls, in the order they bit

1. **`gfx_*` does not exist.** Drawing is `cv_begin(w, h, bg)` → `cv_text(x, y,
   &FONT_*, s, colour)` → `cv_blit(0, y)`, and the screen is drawn in horizontal
   bands. `FONT_B/XS/S/M/L` are globals from `device/gfx.c`, in scope because
   `unity.c` includes it. `C_WHITE/C_HI/C_GRAY/C_DIM/C_LINE/C_BLACK` likewise.
2. **`OBJ_*` must be declared once, in `app.h`.** `app.c` includes `app.h`; a second
   `enum { OBJ_CTRL, ... }` in the `.c` is a redeclaration error.
3. **`APP_NAME`/`APP_SPLASH_RGB`/`APP_BLOCK`/`APP_TOUCH` belong in `app.h`**, not in
   the `.c` — `unity.c` includes `app.h` before the config, and `fm1b_config.h`
   reads them from there.
4. **`words.tsv` is required** even if the app says nothing: `tools/build.py` reads
   `words` from `fm1.toml` and fails with a bare `FileNotFoundError` if the file is
   absent. Copy the template's.
5. **`plat_master()` is 0..4096**, not 0..1023 and not the raw ADC. Two `>>`-based
   conversions that assumed 10 bits are wrong here.
6. **`plat_store_load()` returns the byte count**, so `!= sizeof(set)` is the only
   safe test (a shorter object must not be trusted).
7. **`touch_poll()` must run before `touch_vel()`** in the same frame, or the
   velocity read is the previous frame's. `touch_on(1)` in `app_ui_init`.
8. **`plat_midi_send(pkt, TO_USB | TO_BLE)`** is how one message reaches both links.
   `TO_BLE` = `TO_BLE_HOST | TO_BLE_DEV` (the computer, and a BLE device the FM-1
   connected to). A `plat_midi_out(pkt)` exists but is USB only.
9. **Encoder turn splitting is still the app's job** if it sends relative CC: one CC
   carries ±63, `plat_enc` can report more detents than that at once.
10. **`id_prefix` picks the package identity.** fm1-ctrl is `FM-1_93` (dev
    `FM-1_9300`); the loader compares identities, so do not reuse another firmware's.

## Verifying without the hardware

The host simulator takes a script and writes `midi_out.txt` — the actual bytes.
That is the cheapest proof the map is right:

```
tools/build.py ctrl --host
ctrl/build/host/ctrl_host ctrl/tests/map.txt ctrl/build/host
cat ctrl/build/host/midi_out.txt
```

For fm1-ctrl that printed, and this is the whole panel in eleven lines:

```
1 ms:   B0 1B 40   CC27 = 64      MASTER's first read
1 ms:   B0 07 40   CC7  coarse
1 ms:   B0 27 00   CC39 fine
601 ms: 90 35 64   note 0x35 = 53 = F3, velocity 100
653 ms: 80 35 00   note off
705 ms: B0 28 7F   CC40 = FX down
737 ms: B0 28 00   CC40 = FX up
769 ms: B0 14 41   CC20 = SELECT, 64+1
1039 ms: B0 14 3F  CC20, 64-1
```

The script language: `wait <ms>`, `shot <png>`, `key <k> down|up`, `press <B_*>`,
`release <B_*>`, `tap <B_*>`, `turn <EN_*> <n>`, `expect <what> <value>` (the app's
own `app_expect` supplies the values).

## For DriftSeq specifically

DriftSeq is not a controller: it has a sequencer, an audio engine and a screen of
its own, so the port is larger, but the shape is the same.

**What DriftSeq already has that fm1-base also provides — drop its copy:**

| DriftSeq has | fm1-base gives |
|---|---|
| `firmware/hal/*` (fm1_input, fm1_adc, fm1_lcd_hw, fm1_timer, ...) | `bsp/` (felucca-bsp) |
| `firmware/src/app_main.c` boot sequence, `.ram_text` copy, rail raise | `device/main_fm1.c` |
| `firmware/app.ld`, `crt0.S` | `device/app.ld` |
| `usb.c`, `ota.c` | `device/unity.c` + felucca-bsp |
| `panel.c` calibration | `plat_*` + `OBJ_PANEL` (persisted — DriftSeq's is not) |
| its own WebMIDI-in in `web/emu.js` | keep: fm1-base's emu has no MIDI input |

**The three real ports of work:**

1. **`ds_tick` must move into `app_render`.** DriftSeq's engine is advanced by
   `ds_tick(&g, dt)` from a 1 ms timer today. On fm1-base the audio call is
   `app_render(lr, n, master_q12)` with `n` frames per block; the tick has to be
   driven from there, and `app_frame` keeps the UI. That is a real restructure of the
   timing, not a rename.
2. **The screen must be redrawn through `cv_*` bands** (see pitfall 1). DriftSeq
   draws to its own framebuffer in `web/ui_web.c`; on the device that becomes
   `cv_begin/cv_text/cv_blit` per band, or `lcd_blit` if it keeps its own buffer.
3. **`fm1_in.buttons` reads must become `plat_buttons()`**, and the label→matrix
   conversion disappears (the platform's `panel.btn[]` already does it, and it is
   persisted).

**What DriftSeq gains and fm1-ctrl cannot test for it:** Bluetooth MIDI (the FM-1 as
a BLE peripheral *and* central), the persisted panel calibration, the screen reader,
backup/restore, and a browser emulator built from the same source by `--web`.

**Licence.** fm1-base is MPL-2.0 file by file; DriftSeq is GPL-3.0. MPL-2.0 code may
be combined into a GPL-3.0 work (MPL is GPL-compatible in that direction), but the
MPL files DriftSeq modifies must have their changes published under MPL-2.0. Keep
DriftSeq's own files GPL-3.0 and state the mix in `LICENSING.md`; do not relicense
felucca-bsp's files.

## Open questions (not answered by this port)

- The emu still has no WebMIDI input: `web/emu/worklet.js` handles `load`, `buttons`,
  `keys`, `enc`, `master`, `sysex`, `app` — no `midi`. Driving it from the real FM-1
  needs the sink exposed (`window.__fm1send = send`) exactly as with x0x.
- `plat_master()` is 0..4096 here, 0..1023 in melodee/Felucca's emu and 0..127 in
  DriftSeq's `masterVol`. The conversion belongs in the shim, one profile per host.

## Two things the tests taught, after the port

Both were found by writing the contract tests against the real `app.c` on a fake
platform (15 cases, `sh app/tests/run.sh`), then mutating the source to see whether
the tests noticed. 18 mutations, 18 caught -- but two of them only after the tests
were improved, and both are things DriftSeq will meet:

**A "frame" is not `app_frame`.** The platform's idiom is to drain `plat_midi_in`
from the audio interrupt, in `app_render` (`template/app.c:45` does exactly this).
A test that calls only `app_frame` never sees a host message, so anything you check
about LEDs or MIDI-in will pass for the wrong reason. Test `app_render` then
`app_frame`, in that order.

**Defences need to be told apart before you test them.** MASTER has a dead band of 6
counts *and* 4x oversampling. Measured: the pot's +-2 counts are already covered by
the band alone, so a test using +-2 passes even with the oversampling deleted --
the averaging only earns its place against excursions past the band (the test that
catches its removal uses +-10, where each single read would emit but the mean is
still 2048). If you have two mechanisms, know which one each test is holding up.

Also: an app that is *included* by its test rather than linked lets the test read the
state it keeps (`ctl.lit_keys`, `ctl.lit_btn`), which is the only way to tell "the
host lit a key" from "the app ignored the host". A linked app can only be observed
through `plat_*`, and LED state has no such window.
