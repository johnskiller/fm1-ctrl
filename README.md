# fm1-ctrl

**Not just a MIDI controller: a way to play a firmware's web emulator with the real
panel.** Every key, button and encoder goes out as plain MIDI, a JSON map says what it
means, and the map can be pushed over SysEx at runtime. That is enough to drive the
melodee and Felucca emulators you can already open in Chrome — knobs, keys and master
today, buttons with `web/fm1-shim.js` — so a firmware's UI can be evaluated under real
hands before anything is flashed. See [docs/web-emu-control.md](docs/web-emu-control.md)
for exactly what works per firmware, and where the limits are.

Every FM-1 firmware ships a web emulator that is mouse-only: 27 tiny buttons,
draggable knobs, zero physicality. The FM-1 hardware is the opposite — a great
input device with nothing to play. Flip it: a **thin controller firmware** turns
the physical FM-1 into the input device for ANY firmware's web emulator, with
LED/screen state driven back from the emulator over Web MIDI.

```
FM-1 keys/encoders/buttons ──USB MIDI──▶ browser shim ──▶ emu page controls
FM-1 LEDs/screen ◀──USB MIDI── browser shim ◀── emu state (steps, values)
```

- **Thinnest firmware in the ecosystem**: scan → USB MIDI out, feedback → LEDs/screen.
  No DSP, no storage, no sequencer. (~1–2 days)
- **Thick vs thin**: CTL-1 (charlesvestal/fm1-ctl) already exists but is the
  *thick* kind — its own brain (scale/chords, CC banks, arp, Euclidean drums,
  onboard GM). fm1-ctrl is the *thin* kind: a transparent front-panel for any
  emu — every key/knob/button action goes out raw as MIDI, mapping lives in the
  emu side (JSON map), zero logic on the FM-1.
- **One generic shim + one JSON map per emulator** (~half day each). The JSON
  map convention (`docs/map-convention.md`) is the shared piece — maps are static
  files the hub can host. Emus that already consume Web MIDI natively (melodee's
  simulator does) need no shim at all — the map just bakes into the firmware.
- **DriftSeq's skeleton**: the keyscan/LED/USB-MIDI layer is exactly what idea 3
  (DriftSeq, master MIDI sequencer) grows on. Doubles as idea 4's
  MIDI-in→audio latency test rig.

## Status

Paper-first, following the proven Polly pipeline: **spec → host simulator →
shim → firmware**. See `docs/specs/`.

Progress (2026-10-14):

- ✅ spec v1 + map-convention v0.1 + melodee map + implementation plan
- ✅ Tasks 1–2: host-tested C modules (keyscan/encoders/leds/midi_io) + terminal
  host sim — `sh build.sh` → `ALL HOST TESTS PASSED`
- ✅ Task 4: `tools/map2c.py` (JSON map → `firmware/map_data.c`, idempotent)
- 🔄 Task 3: generic web shim + melodee bridge (node tests)
- 🔄 Task 5 prep: JieLi USB-MIDI investigation (`docs/research/usb-midi.md`)
- ⬜ Task 5: firmware port · ⬜ Task 6: on-device validation + latency

## Layout

```
docs/specs/           dated design docs (v1 scope lives here)
docs/map-convention.md  the JSON control-map convention (v0.1)
docs/maps/            per-emu maps (melodee first)
host/                 host UI simulator (pre-firmware iteration)
firmware/             the controller firmware (pure C, host-tested)
web/                  generic shim JS + per-emu glue
```

## License

GPL-3.0-or-later — matches the FM-1 firmware ecosystem (Felucca lineage).


**Try it without hardware:** open `web/panel-emu.html` — click keys/buttons/encoders, watch the exact MIDI bytes the firmware would send, plus the LED feedback loopback.
