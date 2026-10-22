# Which upstream we vendor, and how far behind we were (2026-10-15)

## Provenance

`firmware/hal/` and `firmware/src/{usb,ota,lcd,gfx,libc,panel}.c` come from
**`charlesvestal/fm1-x0x`** (MIT-adjacent GPL-3.0 tree, Hügelton Instruments), not from
Felucca directly. Verified by diff: 11 of 13 HAL headers were byte-identical to
`fm1-x0x` `HEAD` and none to Felucca 1.6.

`fm1-x0x` is itself a Felucca-lineage fork, so the lineage is
Felucca → x0x → fm1-ctrl.

## What was stale, and what it cost

Compared on 2026-10-15:

| file | our copy | upstream | verdict |
|---|---|---|---|
| `hal/fm1_input.h` | x0x 2026-10-10 | **Felucca 1.6.1 (2026-10-13)** | x0x behind; adopted Felucca |
| `hal/fm1_sys.h` | x0x (pre-#21) | x0x HEAD | **adopted** |
| `hal/fm1_cpu1.h` | x0x (pre-check-word) | x0x HEAD | adopted |
| `hal/fm1_led_anim.h` | absent | Felucca 1.6.1 | new dependency of `fm1_input.h` |
| `src/ota.c` | x0x (pre-`ota_try_ms`) | x0x HEAD | adopted |
| `src/usb.c` | x0x, `SXQ 64` | x0x HEAD, `SXQ 128` | adopted the size only |

### 1. `fm1_input.h`: the encoder fix we were missing

Felucca 1.6.1 rewrote two things, both user-visible on hardware:

- **Asymmetric debounce.** A press counts after `FM1_DEB_PRESS` = 2 frames (~1.1 ms);
  a release needs `FM1_DEB_RELEASE` = 8 frames (~9 ms). x0x's integrating debounce
  (8 frames each way) added ~1 ms of latency to every key and, worse, could end a note
  early when a contact chattered on release.
- **Encoder turning at speed.** From the Felucca header, verbatim: "the stock filter
  took a state only when two frames in a row saw it, and a turn faster than ~110
  clicks/s, ~60 with the transitions bunched, **lost whole clicks**; a flick counted
  next to nothing." Counted up to ~300 clicks/s now. Since our whole product is
  "every control emits, no gaps", silently dropping encoder clicks is the worst
  possible bug for us.

API check before swapping: `fm1_input_init`, `fm1_input_tick`, `fm1_enc_take`,
`fm1_led_key`, `fm1_in`, `FM1_NCOL/NKEY/NENC`, `FM1_KEYMAP`, `FM1_ENC` — all present,
so the swap needed no changes in our code. `fm1_input.h` now `#include`s
`fm1_led_anim.h`, which was vendored with it.

### 2. `fm1_sys.h`: the supply rails (this one is a brick-risk item)

x0x HEAD (`f295356`, fm1-x0x#21) added `fm1_vdc14_raise()` / `fm1_sysvdd_raise()`.
The boot loader leaves **VDC14 = 3 (1.40 V)** and **SYSVDD = 11 (1.26 V)**, while
M-VAVE's own firmware raises them to **7 (1.60 V)** and **15 (1.38 V)** as it starts,
at the same 360 MHz. x0x's comment: "under load (both cores, or WiFi in fm1-radio),
**some units read flash and SRAM wrong**" at the boot loader's levels.

We were running at the boot loader's levels with a flash driver and USB both under
load. Adopted: `fm1_cstart()` now raises both rails, VDC14 first (SYSVDD is regulated
from it), a level per 100 µs, matching x0x's order.

Confirmed present in the linked image: the disassembly of `fm1_cstart` calls
`fm1_p33_read`/`fm1__p33_op` on register 6 then 9 in a loop, incrementing up to 7 and
15, with a 2400-cycle delay between steps.

### 3. `fm1_cpu1.h` and `ota.c`

The cpu1 header gains a check word per job (Melodee's `keremimo/melodee#21`: a stale
function read as the next job). We halt core 1 at boot and never start it, so this is
parity rather than a fix.

`ota.c` gains `ota_try_ms` (a restore's first reads use a shorter wait) and
`ota_service()` returns the decoded frame length so the app can read its own SysEx
commands. We ignore the return value; taking the file keeps us from re-deriving those
patches later. `SXQ` 64 → 128 gives the SysEx queue room for a multi-packet reply,
which our map GET uses.

## How to repeat this check

```sh
git -C /tmp/fm1-x0x fetch --all
git -C /tmp/Felucca fetch --all
for f in firmware/hal/*.h; do
  diff -q "$f" "/tmp/fm1-x0x/$f" && diff -q "$f" <(git -C /tmp/Felucca show origin/main:$f)
done
```

Nine of the HAL headers (`adc`, `audio`, `cc`, `flash`, `gpio`, `guard`, `irq`,
`lcd_hw`, `time`, `timer`, `usb`) were identical to x0x HEAD and are still current;
`fm1_xip.h` and the asm files were not re-checked because nothing we build depends on
their changes.

## Still not done

- `gfx.c` / `lcd.c` / `libc.c` are byte-identical to x0x HEAD, so nothing to take.
- `panel.c` is deliberately trimmed (store stubs, no calibration persistence).
- No second upstream check has been done against `melodee` or `sloop`; if either has a
  HAL change that matters, it would show up as a diff in `fm1_input.h` again.
