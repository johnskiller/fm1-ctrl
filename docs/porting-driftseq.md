# Porting DriftSeq onto fm1-base

Written from a finished port of the opposite kind — fm1-ctrl is a *thin controller*
(no engine, no sequencer, no audio) that moved onto fm1-base and has been running
on hardware since. DriftSeq is the hard direction: it has an engine, a screen
language of its own, and 8,718 lines of firmware of which fm1-base already contains
a better version of roughly half. What follows is what is measured, what is a judgement call, and what will
have to be rewritten rather than moved.

Numbers are from this machine, `2026-10-25`. Reproduce them, do not trust them.

---

## 0. First, the thing that changes the shape of the port

**DriftSeq has no audio.** Its own header says so (`firmware/src/app_main.c:17`,
"no audio on this firmware"), and `ds_tick()` is driven from the TIMER5 interrupt
at 1 ms. Its MIDI goes out through `midi_transport_send`.

fm1-base's app contract is built around `app_render(int32_t *lr, uint32_t n,
uint32_t master_q12)` — the audio interrupt, where a synth does its DSP and where
**every** app is expected to drain `plat_midi_in()`.

So DriftSeq's `app_render` writes zeros and drains MIDI, and the sequencer stays on
a timer. That is legal — `template/app.c` does exactly this for the buffer — but it
means:

- **Do not** move `ds_tick` into `app_render` "because that is where time comes
  from". With no audio, `n` is whatever the platform renders, and the platform's
  render is silent; hanging the sequencer's clock on it couples musical time to an
  audio path DriftSeq does not use. Keep the timer, or use `plat_ms()`.
- The tempting refactor — "render audio, put the tick there like x0x does" — turns
  a working sequencer into one whose tempo depends on a codec. Resist it until
  there is an actual synth to schedule.

If DriftSeq ever gains a synth, this inverts: the tick moves into `app_render` and
the timer goes. Plan the interface so that change is one call site, not a rewrite.

---

## 1. The judgement table

Every component DriftSeq has, against what fm1-base gives. "Shim" = keep
DriftSeq's file, route it through `plat_*`. "Fork" = fm1-base cannot host it as-is.

### Platform plumbing — delete DriftSeq's copy

| DriftSeq | fm1-base | verdict |
|---|---|---|
| `firmware/hal/fm1_input.h` (370 lines) | `bsp/boards/fm1/fm1_input.h` (791) | **drop** — the bsp one is a superset: analog key sensing, probe, drive, all of what DriftSeq's has plus the parts fm1-ctrl needed |
| `firmware/hal/fm1_adc.h`, `fm1_audio.h`, `fm1_lcd_hw.h` | same names in `bsp/boards/fm1/` | **drop** — different, and the bsp's is the maintained one |
| `firmware/hal/fm1_cpu1.{h,S}`, `fm1_timer.h`, `fm1_time.h`, `fm1_gpio.h`, `fm1_irq.h`, `fm1_sys.h`, `fm1_guard.h`, `fm1_flash.h`, `fm1_perf.h`, `fm1_uart.h`, `fm1_xip.h` | `bsp/chip/`, `bsp/boards/fm1/`, `device/main_fm1.c` | **drop** — no equivalent file, but the capability moved into the platform (`plat_ms`, `plat_cpu2_*`, `plat_ticks24`) |
| `firmware/app.ld`, `crt0.S` | `device/app.ld` + `bsp/chip/crt0` | **drop** |
| `firmware/src/usb.c` (1,078 lines) | `device/unity.c` + felucca-bsp | **drop** — this is the single biggest deletion |
| `firmware/src/ota.c` (396) | `device/unity.c` (update path, `ota_session`) | **drop** |
| `firmware/src/panel.c` (109) | `plat_*` + `OBJ_PANEL` | **drop, and gain** — the calibration becomes persisted; DriftSeq's is not |
| `firmware/src/lcd.c` (134) | `device/lcd.c` | **drop** |
| `firmware/src/gfx.c` (5 — a wrapper) | `device/gfx.c` (`cv_*`) | **drop the wrapper, adapt the callers** (see §2) |
| `firmware/src/libc.c`, `libc_fmt.h` | bsp's | **drop** |
| `firmware/keyscan.c` (35), `encoders.c` (37), `leds.c` (32) | `plat_keys()`, `plat_enc()`, `plat_leds()` | **drop** |
| `firmware/ctrl_glue.c` (13), `firmware/src/plat_ctrl.c` (11) + `.h` (20) | `plat.h` | **drop** |
| `firmware/gen_font_pregen.h`, `felucca_font.h` | `assets/fonts` + `tools/gen_font.py` | **drop, and gain** — fonts generated at build time |

Summing those files: **2,541 lines** gone before any DriftSeq logic moves. (Not
counting the 1,877-line `gen_font_pregen.h`, which is a pregenerated font table
rather than plumbing — fm1-base regenerates its own at build time, so it goes too,
but it was never logic.)

### DriftSeq's own code — moves, with the noted changes

| DriftSeq | effort | notes |
|---|---|---|
| `firmware/map.c/h` (19) | **trivial** | pure data; keep |
| `firmware/midi_io.c` (40) | **small** | `midi_transport_send` becomes `plat_midi_send(pkt, TO_USB \| TO_BLE)`. Note the packet packing differs (see §3) |
| `host/driftseq.c` (the engine, golden-pinned) | **none** | it is already pure: `ds_tick(state, dt)` emits bytes into `state->out`. It does not know the hardware exists. Keep it byte-identical or the golden hash breaks |
| `firmware/src/ui_*.c` — 7 files, 1,973 lines (`ui.c` 414, `ui_screens.c` 744, `ui_edit.c` 561, `ui_text.c` 98, `ui_glue.c` 71, `ui_input.c` 50, `ui_tables.c` 35) | **large** | see §2 |
| `firmware/src/app_main.c` | **rewritten** | the boot sequence and the main loop are the platform's now |
| `firmware/src/app.c` (the unity root) | **rewritten** | fm1-base's unity root is `device/unity.c`; DriftSeq's becomes the seven app hooks in `app.c`: `app_init`, `app_ui_init`, `app_frame`, `app_input_only`, `app_render`, `app_panic`, `app_say` |
| **`fm1.toml`** | **new file** | DriftSeq has none — it builds with a `Makefile`. `fm1.toml` is now the build description: `name`, `id_prefix`, `ble`, `wifi`, `cpu2`. Written by hand, ~8 lines |
| `web/emu.js` + the WebMIDI input | **keep** | fm1-base's emulator has **no MIDI input**; this is DriftSeq's to keep |
| `host/test_golden.c`, `test_engine.c`, `test_euclid.c` | **keep as-is** | they test the engine, not the platform — they should still pass untouched, which is the best evidence the port is faithful |

### What fm1-base gives that DriftSeq has no code for

- **Bluetooth MIDI** — `ble = true` in `fm1.toml`, nothing else. Peripheral *and*
  central (the FM-1 can connect to a BLE keyboard).
- **Persisted panel calibration** (`OBJ_PANEL`), which DriftSeq currently loses.
- **The screen reader** — `app_say(const char *a, const char *b)` is a hook DriftSeq
  must define (empty is legal); `words.tsv` is the pronunciation table.
- **Backup and restore** over SysEx — `app_backup_playing()`,
  `app_backup_flush()`, `app_backup_hold(int)`, `app_backup_restored()` are hooks the
  platform calls; DriftSeq must supply them or its state will not survive a restore.
- **A host simulator** (`--host`) that prints the real MIDI bytes, and
  **`--web`**, which builds the browser emulator from the same `app.c`.
- **The second core** as a job interface (`plat_cpu2_run/wait`) — but read
  `docs/base-and-mcu.md` before planning around it: it has a documented hardware
  defect and is off by default.

---

## 2. The screen: the real work

DriftSeq draws into its own framebuffer through seven `ui_*.c` files — 1,973 lines
of layout that assume it can write anywhere and blit when it likes.

fm1-base draws in **horizontal bands**:

```c
cv_begin(240, h, C_BLACK);          /* a band h rows tall, black background   */
cv_text(x, y, &FONT_XS, s, C_HI);   /* returns the pen x                      */
cv_rect(x, y, w, h, C_HI);          /* filled                                 */
cv_line(x0, y0, x1, y1, C_LINE);
cv_blit(0, y);                      /* push the band to the panel at row y    */
```

There is no framebuffer to keep and no partial redraw. Three ways to go:

1. **Band-per-region** (what fm1-ctrl does, 420 lines total). Split the screen into
   the regions that change independently and redraw only the dirty ones. Cheapest
   in RAM, most work in restructuring.
2. **A framebuffer does not fit, and the blit is not exported.**
   `device/lcd.c:104` has `lcd_blit(x, y, w, h, px)` — but it is **`static`**, so an
   app cannot call it; the only path to the panel is `cv_blit`. And a 240×240
   `uint16_t` buffer is 115,200 B against a 98,304 B `.data + .bss` cap with ~65 KiB
   already spoken for. The `.pool` region (336 KiB at `0x1C20000`) is the one place a
   framebuffer could live, but it is shared, uncached, and fm1-ctrl's experience is
   that touching it costs more than it saves. **Treat "keep the framebuffer" as off
   the table**; if it must be tried, it is a platform change (exporting a blit), which
   breaks the unmodified-submodule property in §7.
3. **A band adapter**: keep DriftSeq's drawing calls, implement them over `cv_*`
   with an implicit `cv_begin`/`cv_blit` per region the caller declares. Mechanical,
   but it hides the cost (every region is a full SPI transfer) and DriftSeq's UI
   will need a dirty-region model anyway for 10 Hz refresh.

**The number that decides this**: a full 240×240 refresh is 115,200 bytes of 16-bit
pixels. At the documented 60 MHz SPI clock (`LCD_BAUD 0`, ~44 MHz effective) that is
**20.9 ms per full frame** — 21 % of a 100 ms redraw spent on the transfer alone, and
the redraw cannot overlap it. Whichever route is taken, DriftSeq needs a dirty-region
model: 1,973 lines of "draw it all" cannot run at a useful rate.

Fonts (from `build/gen/felucca_font.h`, `{height, first, last, ...}`):
`FONT_T` 12 (ASCII only), `FONT_XS` 15, `FONT_S` 16, `FONT_B` 16, `FONT_M` 24,
`FONT_L` 32. **`FONT_M` and `FONT_L` stop at `0x5F` (95)** — no lowercase, no
characters past `_`. Only `FONT_T/XS/S/B` cover to `0x7E`. If DriftSeq's screens
use lowercase at large sizes, they will render as blanks, silently. `APP_BIG_FONT` defaults to
`FONT_L` (32 px); overriding it with `FONT_M` saves 20 KB per `fm1b_config.h`. Faces are `BarlowSemiCondensed` (Medium /
SemiBold / Bold) plus a Terminus mono set, generated at build time by
`tools/gen_font.py` — so swapping in a face that matches DriftSeq's existing design
is a build option, not a fork.

---

## 3. Details that will bite, in the order they bit fm1-ctrl

1. **`gfx_*` does not exist.** It is `cv_*` (§2). `C_WHITE/C_HI/C_GRAY/C_DIM/
   C_LINE/C_BLACK` are in scope because `device/unity.c` includes `device/gfx.c`
   before `app.c`.
2. **`OBJ_*` is declared once, in `app.h`.** A second enum in `app.c` is a
   redeclaration error.
3. **`APP_NAME`, `APP_BLOCK`, `APP_TOUCH`, `APP_SPLASH_RGB`, `APP_BIG_FONT`,
   `APP_PALETTE`, `APP_GAIN`, `APP_TAGLINE` all belong in `app.h`.** `fm1b_config.h`
   supplies defaults under `#ifndef` for the cosmetic ones (so the template builds),
   but `APP_BLOCK`/`APP_TOUCH`/`APP_STORE_MAP` are the app's declaration — the
   platform reads them, it does not default them. `APP_BIG_FONT` defaults to
   **`FONT_L`**; `FONT_M` saves 20 KB if the design allows it.
4. **`words.tsv` must exist**, even if empty of meaning: `tools/build.py` fails with
   a bare `FileNotFoundError` if it is missing.
5. **`plat_master()` is 0..4096**, not 0..1023 (melodee/Felucca) and not 0..127
   (DriftSeq's `masterVol`). The conversion belongs in the shim, one profile per
   host. fm1-ctrl's: CC27 = `adc >> 5`, CC39 = `(adc & 31) << 2`.
6. **`plat_store_load()` returns a byte count**, so `!= sizeof(set)` is the only
   safe test.
7. **`plat_midi_send(pkt, to)` takes a packed packet, not three bytes.**
   `pkt = 0x03 | status<<8 | d1<<16 | d2<<24`, with `0x03` the USB-MIDI cable/CIN
   byte. `TO_USB | TO_BLE` reaches both links; `TO_BLE` is `TO_BLE_HOST |
   TO_BLE_DEV`. DriftSeq's `midi_emit3(st, d1, d2)` is a two-line adapter.
8. **`touch_vel()` reads a cache; `touch_poll()` is what fills it.** `touch_poll()`
   drains the touch ISR's event queue and calls `touch_update()` (the pressure
   filter); `touch_vel()` then returns `touch_v[key]` — a value from the last poll.
   Call `touch_poll()` once per input pass, then read. In fm1-ctrl this was missed in
   `app_input_only()`, which is most of the loop: the pressure only advanced on the
   16 ms frames and froze in between. **Poll in both paths.**
9. **Encoder splitting is still yours** if you send relative CC: one CC carries ±63
   and `plat_enc()` can report more detents than that in one frame.
10. **`id_prefix` picks the package identity, and the loader compares it.**
    `PRODUCT = prefix + "00"` (`build.py` main), so `id_prefix = "FM-1_91"` yields
    `FM-1_9100`, and `--release X.Y[.Z]` appends its own digits. DriftSeq's current
    `FELUCCA_ID` is `"FM-1_91001"` — six digits after the underscore, which no
    `id_prefix` can produce (they are digits-only plus a trailing `00`). Either accept
    the new identity (`FM-1_9100`) or pick a distinct prefix; do **not** assume the
    old string survives. Firmware identity and update-tool matching are the two things
    that brick a device if they disagree.

---

## 4. The two faults fm1-ctrl shipped, so DriftSeq does not

Both were found on hardware, both are about the platform's two update paths, and
both look correct in review.

**A frame is not `app_frame`.** `main_fm1.c` runs `app_frame()` at most every 16 ms
and `app_input_only()` in between — and `app_input_only` is *most of the loop*.
Anything that keeps state per frame (velocity, pressure, a held-key read) must be
advanced in **both**. fm1-ctrl read the keys' pressure only in `app_frame` and the
aftertouch froze between frames. A test that calls only `app_frame` cannot see it.

**The platform already filters the pot; do not filter it twice, and do not filter it
in the wrong axis.** fm1-ctrl read `plat_master()` four times per frame, believing
it averaged the conversion. It does not: the platform smooths the pot in the main
loop, once per loop pass, so all four reads return the same `master_pos`. The pot's
real noise is ±20 raw counts and the dead band was 6, so the fine byte danced
between 8/36/88 with nothing touching the knob. What fixed it: a **median over
frames** (8), compared against **what the wire last carried** rather than the
previous frame, with a dead band of 24 — above the noise, below one CC27 step (32).

Generalise: before adding a filter, find out what the layer below already does, and
what its update rate is.

---

## 5. Order of work that keeps the tree green

fm1-ctrl's order, which worked:

1. **Build the template unchanged.** `python3 platform/tools/build.py
   platform/template` — the argument is the app's **directory** (it reads
   `<dir>/fm1.toml`), not a target name. If this fails, nothing else matters.
2. **Write the contract tests first, against the engine, not the screen.** DriftSeq
   already has them (`host/test_engine.c`, `test_golden.c`) and they must keep
   passing untouched. That is the regression net for the whole port.
3. **Port `ds_tick` last, not first.** Get a trivial `app.c` running: keys → notes,
   encoders → CC, screen blank. Flash it. Then hang the engine on it.
4. **Do not touch `platform/`.** It is a submodule; keeping it unmodified means
   `git submodule update` always works and the MPL-2.0 "publish your changes"
   obligation never triggers.
5. **Test mutation, not just tests.** After the suite is green, break the source on
   purpose — flip a constant, delete a call — and check the suite notices. Of
   fm1-ctrl's 12 mutations, 3 were initially missed and each exposed a test that
   asserted the wrong thing (it checked computed state, not what was drawn).

---

## 6. Verifying without the hardware

The host simulator writes the real bytes:

```
python3 platform/tools/build.py --host app
app/build/host/ctrl_host app/tests/map.txt app/build/host
cat app/build/host/midi_out.txt
```

Script language: `wait <ms>`, `shot <png>`, `key <k> down|up [VEL [PRESS]]`,
`press <B_*>`, `release <B_*>`, `tap <B_*>`, `turn <EN_*> <n>`, `master <v>`,
`expect <what> <value>`.

This is how fm1-ctrl found that its aftertouch was right and its LEDs were not:
`midi_out.txt` showed `A0 35 0D … A0 35 34` for the pressure (correct) while
`plat_leds()` was never called at all (the hardware stayed dark). **The bytes prove
the protocol; they cannot prove the panel.** For the panel, `shot` + the pixel
statistics, and then the hardware.

---

## 7. Licence

fm1-base is MPL-2.0 file by file — with `device/announce.c` and
`tools/gen_words.py` under MIT, and three shim files under Apache-2.0. DriftSeq is
GPL-3.0-only.

- MPL-2.0 files may be combined into a GPL-3.0 work. That direction is fine.
- MPL files DriftSeq **modifies** must have those modifications published under
  MPL-2.0. Files it only uses keep their licence.
- If `platform/` stays an unmodified submodule, no modification obligation
  triggers at all. This is the strongest reason to keep it untouched (§5.4).
- Keep DriftSeq's own files GPL-3.0 and say so in `LICENSING.md`. Do not relicense
  felucca-bsp's files.
- The one GPL tail in fm1-base is the optional browser emulator voice
  (`tools/build.py` copies eSpeak NG only if a `fm1-reader` checkout with
  `espeak.wasm` is present). Absent that directory, it never enters the build — but
  if DriftSeq publishes `--web` output, it is worth checking what was shipped.

---

## 8. Honest summary

- **Deletions**: 2,541 lines of platform plumbing, unambiguously a gain.
- **Rewrites**: the unity root, the boot path, the main loop, `app.c` — a week.
- **The hard part**: 1,973 lines of screen code against a banded API with no
  framebuffer and 20.9 ms per full refresh. This is where the schedule goes, and
  no amount of testing helps until the dirty-region model exists.
- **Free**: Bluetooth MIDI, persisted calibration, backup/restore, `--web`, the
  screen reader, and a host simulator with real bytes.
- **Unchanged and load-bearing**: the engine. If `ds_tick` comes through the port
  byte-identical and `test_golden` still passes, the port did not change the music.

---

## Appendix: what the two firmwares are, so the comparison is fair

| | fm1-ctrl | DriftSeq |
|---|---|---|
| what it is | controller: MIDI out, nothing else | sequencer: engine + screens, MIDI out |
| `app.c` | 420 lines | 119 + 212, plus 1,973 in `ui_*` |
| app image | 212,180 B | — |
| RAM `.data+.bss` | 65,168 / 98,304 B | — |
| audio | none (`app_render` writes zeros) | none |
| test suite | 23 contract cases, 12 mutations | engine golden + 3 host tests |
| on hardware | yes | yes |

The asymmetry to keep in mind: fm1-ctrl's port was easy because a controller has no
state worth losing. DriftSeq's port is hard because everything it draws is state.
