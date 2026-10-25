# fm1-base and the FM-1's hardware: what a firmware can actually use

Written 2026-10-24 against `charlesvestal/fm1-base` @ `0944289` and
`hugelton/felucca-bsp` @ `b413cd8`. Every number below was read out of that source, not
out of the README; where the two disagree, the source wins and the disagreement is
noted. Anything marked **unverified** has not run on a device.

Read this before designing an engine: the machine is more capable than the FM-1's
stock firmware suggests, and less symmetric than "dual core with a DSP" implies.

---

## 1. The chip

| | |
| --- | --- |
| SoC | **JieLi AC79** (WL82 core), AIoT part |
| CPU | **32-bit RISC at 360 MHz**, single core plus one worker core |
| ISA / tuning | `-mcpu=r3`, `-mfprev1` — JieLi's own RISC, not ARM or Xtensa |
| FPU | **single precision only**, and **no fused multiply-add** (`-ffp-contract=off` across the whole build, so the host simulator computes bit-identical samples) |
| `double` | **not supported in practice.** `tools/build.py` fails the build if a soft-double routine gets linked in; a single `double` is a build error |
| Flash | 1 MiB (`FLASH_SIZE = 0x93000` for the package), XIP-mapped at `0x02000120` |
| RAM | ~400 KiB total, split (see §4) |
| External RAM | none |
| ADC | 10-bit SAR, four channels used (see §6) |
| Audio out | **ALNK0 I²S → external codec**, DMA, double-buffered |
| Screen | **ST7789-class 240×240** over SPI1 |

The clock is worth knowing about because it is not stable across bootloaders: under
M-VAVE's own bootloader the CPU runs at **360 MHz** (PLL1 = 540 MHz, system clock = 2/3
of it), but under m-bootloader 1.2 the same unit came up at **320 MHz**, which made
every firmware 11 % slower *and* dragged the peripherals' clock from 60 to 53 MHz.
`fm1_clock_stock()` reads the PLL registers at boot and restores M-VAVE's values, so a
build does not inherit whatever the loader left. Measured with a one-instruction loop
against TIMER4's 24 MHz crystal.

The SDK's own maximum for the system clock is 320 MHz; 360 is what M-VAVE ship. There
is no supported path above it (`bsp/docs/EXPERIMENTAL.md`, "Clock up to 320 MHz": the
plan is to measure, read the SDK's sequence, try one step with a fallback).

---

## 2. There is no DSP core — say it plainly

The FM-1's audio capability is **CPU0 doing floating-point arithmetic in an interrupt**,
not a separate DSP block. This matters more than any instruction list:

- There is no dedicated DSP, no hardware MAC array, no audio accelerator to offload to.
- The FPU is scalar single-precision. `fastmath.h` exists precisely because there is no
  libm and no vector unit: `fm_sinf`, `fm_cosf`, `fm_tanhf`, `fm_exp2f`, `fm_powf`,
  `fm_sqrtf`, `fm_rsqrtf`, `fm_db2lin`, `fm_sin_pi`, `fm_wrap_pi`, `fm_floorf`,
  `fm_clampf` — all `static inline`, branch-light, written to hit stated error bounds
  (e.g. `fm_sinf` abs 1e-6 on [-100, 100]; `fm_powf` rel 1e-5) and verified against libm
  by `tests/fastmath_test.c`.
- `csync` appears in the tree only as a **cache synchronisation barrier between the two
  cores**, not as a DSP primitive. Anyone reading the register names (`FM1_C1_CON`,
  `FM1_C1_EMU_CON`) should not mistake the "EMU" there for emulation of a DSP.

So a "DSP-heavy" feature is really a budget question: how much of a 5.8 ms block can
you spend in the audio ISR.

### The audio path, exactly

```
app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
    ↓ called from render_block() in the ALNK0 interrupt
abuf[2][256 frames]  (two halves, int32 L/R, 24-bit left-justified)
    ↓ DMA
ALNK0 I²S → external codec → speaker / headphones
```

Facts worth designing against:

| | |
| --- | --- |
| Sample rate | **44.1 kHz**, `FS 44100` in `device/main_fm1.c`. Note what `FS` actually is: a constant used for the **deadline arithmetic** (`budget = HALF_FRAMES * 1e6 / FS`). The rate itself is set by ALNK0 and the codec, so changing `FS` alone changes the budget and not the pitch. 44.1 kHz is the value the platform has been run at; other rates are unverified |
| Block | `APP_BLOCK = 256` frames by default → **5.8 ms** per half buffer |
| Buffer | `abuf[2 * 256 * 2]` int32: two halves, L/R interleaved |
| Deadline | 5.8 ms per block. The platform measures it: `fm1b_dbg.max_us`, and an EMA gives `plat_cpu_pct()`; over 90 % of the budget calls `app_overload()` so an app can shed voices |
| Overrun | `plat_xruns()` counts the DMA running past the render (`fm1b_dbg.late`) |
| Thread | the render **is** an interrupt (ALNK0, priority 3). No locks; `plat_lock()` exists for the timer-shared data |
| Latency | one block: ~5.8 ms, plus the codec's own |
| Format | int32 in, and int32 out: the app writes full-scale integers, not floats |
| Channels | **two**, L and R independently addressable (`lr[2*i]`, `lr[2*i+1]`). The template writes the same sample to both; a stereo engine does not have to |
| Gain | `master_q12` arrives per block, already converted by `APP_GAIN(m)` (default `m*m>>12`, i.e. the square — a taper, not linear) |

### What the render must not do

`plat_midi_in` is drained **from the render** (`template/app.c:70`), which is the
platform's idiom: the MIDI ring is read in the audio interrupt, not the main loop. The
consequence for tests and for DriftSeq's port is in
`docs/research/porting-to-fm1-base.md` — a test that only calls `app_frame` never sees a
host message, and a test that only calls `app_render` never sees a key.

The render also must not touch flash: `radio_quiet` routes the app to `app_quiet()`
while the radio calibrates, because a fetch during the radio's trim can come back
corrupted (a CTL-1 crashed so, 0.6 s into each start).

---

## 3. The second core: what it is, and the hardware bug you must plan for

This is the part to read carefully, because the honest answer is **"a worker with a
known defect on some units"**, not "a second DSP".

### Mechanical facts

| | |
| --- | --- |
| What it is | CPU1, an identical core, driven as a **one-job-at-a-time worker** for CPU0 |
| Job model | `plat_cpu2_run(fn, arg)` → CPU0 does its own share → `plat_cpu2_wait()`. **One job outstanding, ever** |
| Stable stack | **2816 B** (`FM1_CPU1_USTACK_WORDS`), with a fill mark so `plat_cpu2_stack()` reports the deepest use |
| Timeout | **8000 µs** (`FM1_CPU1_TIMEOUT_US`) — a job not back by then is a failure, not a slow job |
| When it may start | **once, in `app_init`, before the audio and the timers.** Not later. This is the start that works (Melodee's and X0X's finding) |
| Where its loop lives | `.c1_text`, copied to RAM at boot, so an **idle CPU1 never fetches from flash**. The *job* runs from flash, so CPU0 may only turn flash off with no job outstanding |
| Sharing | each word written by one core, `csync` around each; CPU0's words first, CPU1's on their own cache line |
| Capability | it can run any C function. It is not restricted to audio, but audio is what it is for |

### The defect

On **some FM-1 units** CPU1 intermittently reads shared RAM wrong (a 0x00200000 where a
0 holds), and more often under load. Measured by Jangada; reported as
`keremimo/melodee#17` and `charlesvestal/fm1-x0x#10`. The platform's response is a set
of defences rather than a fix:

- every job carries a **check word** (function, argument, job number); a mismatch means
  the job is rejected, never run twice;
- a misread, a CPU1 fault, a late job, or a starter that does not answer is a
  **failure**;
- two policies (`FM1_CPU1_FALLBACK`):
  - **0 (the default):** the failure goes into the crash record (`vec 0x100 +
    FM1_CPU1_*`) and **the FM-1 restarts**. Two failed starts at boot → safe mode.
  - **1:** CPU1 is retired, held, and **barred until power-off** — one core from then on,
    with nothing written to flash on a unit that may be unstable. A new build
    (`FM1B_BUILD`) gives it two cores again.
- `plat_cpu2_barred()` / `plat_cpu2_gave_up()` tell the app which happened.

### Status — read this before you rely on it

| | |
| --- | --- |
| Proven | the start, the guards, the hold, the misread detection: **Melodee and X0X run this on devices** |
| **Not proven** | **this file, as a whole, has not run on a device.** `bsp/docs/EXPERIMENTAL.md` says so explicitly: "This file as a whole has not run on a device yet." The first test is `hwtest-exp`, `k`, then `p` a few times |
| Downstream users | X0X, NuSaw, SPH-16, LOOM-8, Melodee |
| fm1-ctrl | **does not use it.** A controller has nothing to compute in parallel |

So: if you plan an engine that *needs* the second core, that is a design risk on top of
the usual one. Plan the single-core path first, and treat two cores as an accelerator
that may be barred for the whole session on a given unit.

### Practical shape for a DSP-heavy app

The pattern the downstream firmwares use is a **split render**:

```c
void app_render(int32_t *lr, uint32_t n, uint32_t gain)
{
    if (plat_cpu2_on() && plat_cpu2_run(voice_a_half, arg)) {
        voice_b_half(arg);                  /* CPU0's own share */
        if (plat_cpu2_wait() < 0)           /* retired: the job was dropped */
            voice_a_half(arg);              /* do it here instead */
    } else {
        voice_a_half(arg);
        voice_b_half(arg);                  /* one core */
    }
}
```

Two rules that are not optional: **never** turn flash off under an outstanding job, and
**always** be able to do the dropped job on CPU0 (`plat_cpu2_wait()` returning -1 means
exactly that). The host simulator runs jobs immediately and in order, so the same code
is testable on a laptop.

---

## 4. Memory

```ld
XIP     ORIGIN = 0x02000120  LENGTH = 0x8DFBC   /* code + rodata, runs from flash */
RAMTEXT ORIGIN = 0x01C00000  LENGTH = 0x8000    /* RAM-resident code (.ram_text, .c1_text) */
RAM     ORIGIN = 0x01C08000  LENGTH = 96K       /* .data + .bss */
POOL    ORIGIN = 0x01C20000  LENGTH = 0x54000   /* 336 KiB: big zeroed buffers, .pool */
NOINIT  ORIGIN = 0x01C7C000  LENGTH = 0x3D50    /* survives a reset (counters, bars) */
```

| | |
| --- | --- |
| Code space | `APP_SLOT = 0x8DFBC` ≈ **567 KiB** of the 1 MiB flash |
| `.data` + `.bss` | 96 KiB — this is the tight one. fm1-ctrl uses 9,772 B of it |
| `.pool` | **336 KiB**, zero-initialised at boot, for big arrays. Declare with `PLAT_POOL` |
| `.noinit` | 15.3 KiB, survives reset |
| Flash data area | `0x97000..0xDFFFF` (`plat_data_*`), plus `PLAT_XDATA0` 16 KiB and `PLAT_XDATA1` 92 KiB — **but** the update loader looks for a record in bytes `0xF00..0xF07` of every sector there, so an app keeping big data must leave those erased or guarantee they never read as `"AT"` at 0xF06 |
| Store objects | `PLAT_STORE_MAX = 3840` B per object, A/B sectors with CRC, torn-write safe; `OBJ_PANEL` is the platform's (panel calibration) |

The 96 KiB of `.data`/`.bss` is where a naive engine dies. Sample memory, delay lines and
big wavetables belong in `.pool`; per-voice state belongs in `.bss` and must be counted.

---

## 5. What fm1-base gives you above the hardware

`plat.h` is the contract, in groups, and a firmware compiles in only the groups its
`fm1.toml` asks for. The app sees nothing else — `tools/build.py` enforces that every
hardware register access lives in `bsp/`, so `device/`, `include/` and the app are clean
of bare pointer writes.

**Count note:** `DESIGN.md:42` says 68 functions; `include/plat.h` at this commit
declares **75** `plat_*` functions. The header is the truth and the doc has drifted —
count it yourself before quoting a number.

| Group | What you get |
| --- | --- |
| core | `plat_ms`, `plat_buttons` (bit `B_*`), `plat_keys` (bit 0..26), `plat_enc` (detents), `plat_master` (**0..4096**), `plat_lights`/`plat_leds`, `plat_glow`, `plat_play_red`, storage, `plat_cpu_pct`, `plat_xruns` |
| MIDI | `plat_midi_in`/`send`, `TO_USB`/`TO_BLE`/`TO_TRS`, `plat_midi3`, SysEx, `plat_midi_out_room` (per-link queue depth, for a sequencer's note-offs), `plat_midi_in_from` (for thru), `plat_midi_out_link` (new USB connection → re-send programs) |
| second core | `plat_cpu2_*` (§3) |
| analog keys | `plat_sense`, `plat_analog_mask`, `touch_*` (lib/touch.h): velocity on 20 keys, aftertouch on 20, velocity-only probe on the top 7 |
| profiling | `plat_ticks24` (24 ticks/µs), `plat_cpu_pct`, `plat_xruns` |
| power | `plat_battery` (0..3, 4 = USB host, -1 unread) |
| Bluetooth | `plat_ble_*`: the FM-1 as a BLE **peripheral** and, with `ble_central`, also as a **central** to a keyboard |
| WiFi | `plat_wifi_*` on lwIP with sockets. **Bluetooth or WiFi, never both** — each fills what the chip has |
| USB audio | in (record the FM-1) and out (play through it), each switchable off |

The app supplies seven functions: `app_init`, `app_render`, `app_panic`,
`app_ui_init`, `app_frame` (~60 Hz), `app_input_only`, `app_say` (screen reader).
Optional hooks: `app_tick` (1 kHz, in the timer ISR — a clock or sequencer),
`app_boot`, `app_loop`, `app_yield`, `app_overload`, `app_splash`,
`app_quiet`, `app_key_sample`, `app_sysex`, `app_usb_cmd`, `app_redraw`,
`app_cstart`, `app_fault`, and the backup quartet.

### Panel, screen, MIDI — the measured numbers

| | |
| --- | --- |
| Keys | **27**, F3..G5. Scan: 11-column × 6-row diode matrix behind two 74HC595, **TIMER5 at 10 kHz**, one column per tick, full frame ~0.6 ms. Press needs 2 frames closed (1.1–2.2 ms); release needs ~9 ms open |
| Encoders | **7**, stock quadrature decoder, one detent = one full quadrature cycle, counted up to ~300 clicks/s (the stock filter lost clicks above ~110) |
| Buttons | **14** + PLAY's second (green) LED |
| LEDs | per-key and per-button, plus `plat_lights_mid` (a third brightness level) and a glow level for unlit buttons |
| MASTER | 10-bit ADC (PB6, channel 4) → `plat_master()` **0..4096** |
| Screen | 240×240 ST7789. SPI1 at `LCD_BAUD 0` ≈ 60 MHz (~44 MHz effective, measured). **The FM-1 has no TE line**, so tearing is a real constraint: the panel's 16.7 ms refresh versus a strip transfer |
| MIDI in | USB, the TRS jack (in only — the FM-1 has no MIDI out jack), Bluetooth |
| MIDI out | USB and Bluetooth (no TRS out) |

The 24 MHz crystal drives TIMER4, which is the timing reference for everything
(`plat_ticks24`: 24 ticks per microsecond, wraps every 179 s — spans are unsigned
differences).

---

## 6. Analog input the stock firmware does not use

Two of the key-matrix rows are wired to the SARADC (**PA7 = ch0, PA8 = ch1**), which is
how `lib/touch.c` gets **velocity and pressure** out of a keyboard that was never
designed to report either:

- the **20 keys F3..C5** are on the analog rows: real velocity on press, real pressure
  while held;
- the **7 top-row keys C#5..G5** have no converter: velocity only, from a probe through
  the LED line, and no pressure;
- off by default; `touch_on(1)` takes the keys over (let every key go first).

One converter, owned by the input tick: the main loop must not call `fm1_adc_read`
while the scan runs. A conversion made short enough to fit between two 100 µs ticks is
a deliberate change from the SDK's timing.

This is the single most under-used capability for a groovebox: 20 keys with
velocity **and** aftertouch is not what an FM-1 normally feels like.

---

## 7. Build-time rules that shape the design

`tools/build.py` refuses a build that breaks these, so they are not style advice:

| Rule | Why |
| --- | --- |
| **No `double`** | the FPU is single precision; a soft-double routine in the image is a hard error (JieLi's radio setup is the one noted exception) |
| **No register access outside `bsp/`** | `device/`, `include/` and the app must go through `plat.h` |
| **No fused multiply-add** | `-ffp-contract=off` everywhere, so the host simulator and the device compute identical samples |
| **App ≤ 567 KiB** | `APP_SLOT`; a bigger image is a build error, not a truncated package |

---

## 8. Read this part before designing anything

1. **There is no DSP.** Audio is CPU0 in an interrupt, 5.8 ms per block, single-precision
   scalar. Budget in microseconds, not in "DSP cycles".
2. **The second core is a worker with a defect on some units, and this implementation is
   unverified on hardware.** Build the one-core path first. Two cores is an accelerator
   that may be barred for the session.
3. **96 KiB of `.data`/`.bss` is the real ceiling.** Big buffers go in `.pool` (336 KiB).
4. **Bluetooth or WiFi, choose one.** They do not coexist.
5. **44.1 kHz, 256-frame halves, int32 out.** No float on the wire.
6. **Velocity and aftertouch already exist on 20 keys** — no hardware mod needed.
7. **`plat_master()` is 0..4096**, not 0..1023 and not 0..127. Every emulator in the
   ecosystem disagrees about this range; convert in the app or the shim, never assume.
8. **The host simulator runs the same app.** Anything that cannot be tested there
   (Bluetooth, the second core's quirks, the analog rows) is where the device-only risk
   is concentrated.

---

## Sources

| Claim | Where |
| --- | --- |
| 360 MHz, PLL values, the 320 MHz loader case | `bsp/chip/fm1_clock.h` |
| No FMA, single-precision FPU, no `double` | `platform/tools/build.py:106-107`, `:624-631` |
| 44.1 kHz, 256-frame halves, the ISR and its deadline | `platform/device/main_fm1.c:12-16`, `render_block`, `fm1_alnk0_irq` |
| Second core: start, check word, defect, fallback, 2816 B, 8 ms | `bsp/chip/fm1_cpu1.h` |
| Second core unverified on a device | `bsp/docs/EXPERIMENTAL.md` |
| Memory map | `platform/device/app.ld` |
| 567 KiB slot, 1 MiB flash | `bsp/tools/fm1pkg.py:30-31` |
| Panel scan, debounce, encoders, analog rows | `bsp/boards/fm1/fm1_input.h`, `fm1_adc.h` |
| Screen, no TE line, SPI baud | `platform/device/lcd.c`, `bsp/boards/fm1/fm1_lcd_hw.h` |
| Velocity and pressure per key | `platform/lib/touch.h` |
| Audio DMA, I²S, codec lines | `bsp/boards/fm1/fm1_audio.h` |
| `plat.h` groups | `platform/include/plat.h`, `platform/DESIGN.md:45-56` |
