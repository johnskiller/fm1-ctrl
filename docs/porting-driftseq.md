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
| `firmware/src/gfx.c` (5) + `gfx_body.h` (243) | `device/gfx.c` (206) — **the same renderer**, 88.6 % identical | **drop both**; add `cv_disc`/`isqrt_i` in the app if used, set `APP_CANVAS_PX (240u*224u)` in `app.h` (§2) |
| `firmware/src/libc.c`, `libc_fmt.h` | bsp's | **drop** |
| `firmware/keyscan.c` (35), `encoders.c` (37), `leds.c` (32) | `plat_keys()`, `plat_enc()`, `plat_leds()` | **drop** |
| `firmware/ctrl_glue.c` (13), `firmware/src/plat_ctrl.c` (11) + `.h` (20) | `plat.h` | **drop** |
| `firmware/gen_font_pregen.h` (1,877), `src/felucca_font.h` (5) | `assets/fonts` + `tools/gen_font.py` | **drop, and gain** — fonts generated at build time |

Summing those files: **2,793 lines** gone before any DriftSeq logic moves. (Not
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

## 2. The screen: DriftSeq already has fm1-base's renderer

**Correction to an earlier draft of this document**, which claimed fm1-base has no
framebuffer. It does. `device/gfx.c` holds `cv_px[]` — a real off-screen canvas —
and it is in `.pool`, not `.bss`. The "banded" appearance comes from how apps *use*
it, not from a limitation.

The important finding for DriftSeq: **`firmware/src/gfx_body.h` is the same renderer.**
243 lines against fm1-base's 206, `difflib` similarity **88.6 %**, in three
contiguous runs (116, 69 and 9 lines). Same `cv_px[]`, same `.pool` section, same
`cv_begin` / `cv_band` / `cv_rect` / `cv_line` / `cv_text` / `cv_blit` /
`cv_blit_from` / `draw_text_box` / `swap16` / palettes. It is an older copy of the
same file, forked before fm1-base picked it up.

The complete difference:

| | DriftSeq `gfx_body.h` | fm1-base `gfx.c` |
|---|---|---|
| lines | 243 | 206 |
| canvas size | `CV_MAX = 240 * CV_ROWS`, `CV_ROWS` 224 → 105 KiB (its own comment: "full 240x224 main area per frame (X0X bands <=144)") | `CV_MAX = APP_CANVAS_PX`, default `240*144` → 67.5 KiB, **overridable from `app.h`** |
| `isqrt_i()` | present | absent |
| `cv_disc()` | present | absent |
| font typedef | in `gfx.c` glue, body has none | in `gfx.c` |

So the port for the renderer is: **delete `gfx_body.h` and `gfx.c`, and add two
functions to the platform copy** — or, better, carry `cv_disc` in DriftSeq's app code,
since a disc is not a platform primitive. `APP_CANVAS_PX` in `app.h` is the supported knob
for the canvas size; DriftSeq wants 224 rows (`APP_CANVAS_PX (240u * 224u)`) and
fm1-ctrl wanted 136, both legal. DriftSeq's own header already does exactly this under
a different name — the port is a rename, not a redesign.

A note on why fm1-base's default is 144 and not 240: the default is X0X's main area,
not a RAM limit. Measured on both trees:

| | fm1-ctrl | DriftSeq |
|---|---|---|
| `.pool` used | 93,704 B (canvas 69,120 + 3 BLE task stacks at 8 KiB) | **107,520 B** — the canvas alone, exact |
| `.pool` of | 344,064 B | 344,064 B |
| spare | 250,360 B | 236,544 B |
| `.data+.bss` | 65,168 / 98,304 B | 48,024 / 98,304 B |

DriftSeq's `.pool` is *entirely* canvas and it still has 236 KB spare; its `.bss` is
half empty. Its build has no Bluetooth stacks in the pool, so a BLE build adds 24 KiB
there (fm1-ctrl's does). RAM is not the constraint on either firmware. `.data+.bss` is
the 96 KiB cap, and Bluetooth's 24 KiB heap lives there on a BLE build — the canvas is
deliberately in `.pool` to stay out of it.

### What actually costs time

Two real constraints, neither of which is "no framebuffer":

1. **`lcd_blit` is `static`** (`device/lcd.c:104`) and the only app-visible path to the
   panel is `cv_blit` → `lcd_blit`. That is fine — DriftSeq's `cv_blit` calls
   `lcd_blit` the same way — but it means an app cannot blit a region from its own
   buffer at an arbitrary moment; it must be `cv_begin`/draw/`cv_blit`.

2. **A full-frame transfer dominates the budget.** DriftSeq's frame is 240x224 —
   107,520 B, which is **19.6 ms** at ~44 MHz effective (`LCD_BAUD 0`, 60 MHz
   documented). A full 240x240 would be 21.0 ms. The arithmetic checks out against
   `lcd.c`'s own hardware measurement: 240x72 at `LCD_BAUD 4` (12 MHz) is 23.0 ms by
   formula and "23 ms" in the comment.

   fm1-base's `lcd_data` leaves the DMA running and `lcd_sync()` waits only when the
   buffer is touched again, so the main loop does work during the tail of a frame — but
   the *next* draw into `cv_px` blocks. At 10 Hz the transfer alone is 20 % of the
   period; at 60 Hz it is 117 % and impossible.

   This is why X0X draws in bands: `cv_band(y0, h)` confines drawing to rows
   `y0..y0+h-1` of one canvas, so several bands can be composed before a single
   `cv_blit`, and a dirty band alone can be `cv_blit_from`-ed. It is an optimisation
   for the transfer, not a shape imposed on the app.

So the dirty-region model is still needed — but for the SPI budget, not for RAM.
DriftSeq's `ui.c:48` does `cv_begin(240, 224, C_BLACK)` every frame: one full-canvas
pass. At 19.6 ms a frame that caps the firmware near 50 Hz *before* any drawing logic
runs, and 1,973 lines of UI will not fit in what is left. The fix is the one X0X already
demonstrates in this same file: `cv_band` to compose several bands into one canvas, and
`cv_blit_from` to send only the rows that changed.

### Fonts

`FONT_T` 12 (ASCII only), `FONT_XS` 15, `FONT_S` 16, `FONT_B` 16, `FONT_M` 24,
`FONT_L` 32, from `build/gen/felucca_font.h` as `{height, pad, first, last, ...}`.
**`FONT_M` and `FONT_L` stop at `0x5F`** — `glyph()` folds lowercase to uppercase when
`f->last < 'a'`, so large text renders as capitals, not blanks. Anything outside
`first..last`, and C1 controls, draw as `'?'`. `APP_BIG_FONT` defaults to `FONT_L`;
`FONT_M` saves 20 KB. Faces are `BarlowSemiCondensed` (Medium / SemiBold / Bold) plus a
Terminus mono set, generated at build time by `tools/gen_font.py` — swapping in a face
that matches DriftSeq's design is a build option, not a fork.

DriftSeq's `gen_font_pregen.h` (1,877 lines) is a *vendored pregenerated* copy of the
same tables; the real build regenerates it, and the wasm UI build includes it directly.
On fm1-base it goes away entirely.

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

- **Deletions**: 2,793 lines of platform plumbing — including the renderer, which is
  the same file — unambiguously a gain.
- **Rewrites**: the unity root, the boot path, the main loop, `app.c` — a week.
- **Easy, unexpectedly**: the renderer is *already ported*. `gfx_body.h` is an older
  copy of fm1-base's `gfx.c` (88.6 % identical, same `cv_px[]` in `.pool`); DriftSeq
  has two extra functions (`isqrt_i`, `cv_disc`) and a larger canvas, both of which
  move over as an `APP_CANVAS_PX` define and 20 lines of app code.
- **The hard part**: not the drawing API — the **SPI budget**. DriftSeq's 240x224
  frame is 19.6 ms of transfer and `ui.c:48` redraws the whole canvas every frame. Its
  1,973 lines of UI need a dirty-region model (`cv_band` + `cv_blit_from`, already in
  the same file) to fit, and no allocation strategy changes that.
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
