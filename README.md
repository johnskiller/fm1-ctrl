# fm1-ctrl

A **thin, transparent MIDI controller** for the M-VAVE FM-1: every control sends its
own event, unchanged, and the mapping lives in the receiving host (a web emulator, a
DAW, or a map pushed over SysEx). The firmware has no business logic.

**Status: ported to [fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0),
which brings Bluetooth MIDI.** The app is one file, `app/app.c`.

## The map

| control | message |
| --- | --- |
| 27 keys (F3..G5) | Note On/Off, note **53..79**, velocity from the key's own sensing |
| 7 encoders | CC **20..26**, relative, `64 + detents` (split at ±63) |
| 14 buttons | CC **40..53**, `127` down / `0` up, labels FX..OCT+ |
| MASTER | CC **27** (7-bit) plus the 14-bit pair CC **7** / CC **39** |
| LEDs in | notes 53..79 → the keys, CC 54..67 → the button LEDs |

Buttons are sent as **plain CC**, not `0xFA`/`0xFC` and not MMC. Measured reasons:
the FM-1 emulators drop every byte `>= 0xF0` (`melodee`/`Felucca`
`index.html:439,472` — `if (d[0] >= 0xF0) return;`), and no firmware in the ecosystem
reads MMC. [DriftSeq](https://github.com/johnskiller/driftseq) implements this exact
map on purpose (`web/emu.js:184`), so it is the baseline this firmware must not break.

## Build

```sh
git submodule update --init --recursive        # the platform
export AC79_SDK=/path/to/AC79_AIoT_SDK         # only for Bluetooth
python3 platform/tools/build.py app            # firmware + package
python3 platform/tools/build.py app --host     # the host simulator
python3 platform/tools/build.py app --web      # the browser emulator
```

Output: `app/build/ctrl.bin`, `app/build/ctrl.fwsc` (identity `FM-1_9300` in a dev
build).

## Verify without the hardware

The host simulator writes the actual bytes it sent:

```sh
python3 platform/tools/build.py app --host
app/build/host/ctrl_host app/tests/map.txt app/build/host
cat app/build/host/midi_out.txt
```

That is the whole panel in eleven lines — see `platform/README.md` for the script
language (`wait`, `key`, `press`, `turn`, `shot`, `expect`).

## Repository shape

```
app/                    the firmware: app.c, app.h, fm1.toml, words.tsv, tests/
platform/               fm1-base (submodule): bsp/, device/, include/, tools/, web/
tools/                  this project's own scripts (map generation, checks)
web/                    fm1-shim.js: drives a third-party emulator from the real FM-1
docs/                   specs, maps, research notes
```

Everything under `platform/` is MPL-2.0 and belongs to its authors; our own files are
MPL-2.0 too. **No GPL-3.0 code is in this tree** — see `LICENSING.md` for how that
came about and what it replaced.

## What it does not do

- It does not drive other firmwares' browser emulators by itself. Their worklets accept
  `{type:"buttons"|"keys"|"enc"|"master"}` and **have no MIDI input**; `web/fm1-shim.js`
  is the translation layer, and it needs the page to expose its `send()`.
- It does not make sound. A controller sends events.
- Its own screen is a status page (counters, encoder positions, what the host lit).

## Research notes

- `docs/research/porting-to-fm1-base.md` — what the port took, the ten pitfalls, and
  what DriftSeq will meet doing the same.
- `docs/research/timer5-isr-trap.md` — the TIMER5 wrapper trap of the old tree.
- `docs/research/upstream-audit.md` — the old tree's code lineage.

## Licence

MPL-2.0. See `LICENSING.md`.
