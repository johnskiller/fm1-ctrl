# fm1-ctrl implementation plan

> **For Hermes:** Use subagent-driven-development to implement this plan task-by-task.

**Goal:** Turn the physical FM-1 into the input device for web emulators — thin
controller firmware (scan → USB MIDI out, LED feedback in) + one generic web
shim reading a per-emu JSON map. First end-to-end target: melodee's own emulator.

**Architecture:** Paper-first (Polly's proven pipeline): host UI simulator first
using the SAME keyscan/encoders/led/midi modules the firmware will use, then the
web shim, then firmware on the JieLi SDK. Pure C, host-tested — module logic
runs and is verified on host before it ever touches the device.

**Tech stack:** C99 (host sim compiles with cc; firmware with the pi32v2
toolchain), Web MIDI API (Chromium), no dependencies on the host side.

**Working tree:** `/home/john/.hermes/workspace/fm1-ctrl`
**Spec:** `docs/specs/2026-10-14-controller-v1.md` (v1 scope, acceptance
criteria) · **Map convention:** `docs/map-convention.md` v0.1
**Reference code:** Felucca web/emu shim architecture (hugelton/Felucca
`web/emu/` — shim + worklet + index.html + build.sh), sloop/melodee HAL for
keyscan patterns. `/tmp/melodee` has the melodee tree if HAL patterns are needed.

---

## Task 0: Repo skeleton (DONE — this commit)

- [x] README (positioning + layout)
- [x] `docs/specs/2026-10-14-controller-v1.md`
- [x] `docs/map-convention.md` v0.1 + `docs/maps/melodee.json`
- [x] LICENSE, directory skeleton

## Task 1: Host simulator skeleton — `host/sim.c` + module stubs

**Objective:** A terminal/SDL host binary that instantiates the same modules the
firmware will (keyscan, encoders, leds, midi_io) and prints every MIDI byte the
"device" would emit.

**Files:**
- Create: `host/sim.c` (main loop, key/encoder injection from stdin)
- Create: `firmware/keyscan.c/h`, `firmware/encoders.c/h`, `firmware/leds.c/h`,
  `firmware/midi_io.c/h`, `firmware/map.c/h` (compiled-in map table)

**Steps:**
1. Write failing host test: injecting key C4 press emits bytes
   `90 3C 64`; release emits `80 3C 00` (test in `host/test_midi.c`, plain
   assert, `cc -O2 -Wall`).
2. Implement `keyscan.c` as pure function: key matrix state → note on/off
   events (no I/O; debouncing via 2-sample consensus).
3. `midi_io.c` has a compile-time backend: `MIDI_HOST` (ring buffer, host sim
   reads it) vs `MIDI_USB` (JieLi SDK, later task).
4. Run: `cc -O2 -Wall -o /tmp/fm1sim host/sim.c firmware/*.c -DMIDI_HOST && echo ok`
5. Commit: `feat: host sim skeleton + keyscan/midi modules (host-tested)`

## Task 2: Encoders + LED feedback on host

**Objective:** Encoders emit relative CC; LED grid state driven from MIDI in
renders as ASCII art in the host sim (same code path the firmware LED driver
will call).

**Files:**
- Modify: `firmware/encoders.c`, `firmware/leds.c`, `host/sim.c`
- Test: extend `host/test_midi.c` — encoder turn emits `B0 47 41` style CC with
  relative sign; MIDI-in CC 0–15 sets grid bits; assert LED state.

**Steps:**
1. Failing test → implement quadrature decode (2-bit gray → +1/-1/0) and
   relative-CC emit (slop +1 / -63, standard MCU-style two's-complement).
2. Failing test → implement `leds_apply_cc(cc, value)` writing into a
   5×7+27 grid byte array; host sim prints it after each event batch.
3. Run test binary, expect PASS; commit `feat: encoders + LED grid feedback`.

## Task 3: Generic web shim + melodee map

**Objective:** A drop-in JS shim (`web/shim.js` + per-emu glue) that reads a
JSON map and bridges Web MIDI ↔ emu page controls, verified against melodee's
published emulator.

**Files:**
- Create: `web/shim.js` (generic: parse map, requestMIDIAccess, route events)
- Create: `web/melodee-bridge.js` (selectors for melodee's page controls —
  knob elements, note→synth key mapping)
- Create: `web/index.html` (launcher: pick emu, install shim, status)
- Test: `web/emu_test.mjs` — node-side: mock a map + MIDI events, assert the
  right DOM events would fire (jsdom or plain stubs, no browser needed).

**Steps:**
1. Failing node test: shim with melodee map routes CC 71 → knob 0 setter.
2. Implement shim (~200 lines, no framework).
3. Manual browser check against
   https://johnskiller.github.io/melodee/webapp/simulator/ — knob turns on page.
4. Commit `feat: generic web shim + melodee bridge`.

## Task 4: map2c compiler

**Objective:** `tools/map2c.py` compiles a JSON map into `map.c`'s const table
so firmware and shim share one source of truth.

**Files:**
- Create: `tools/map2c.py`
- Modify: `firmware/map.c` (generated), build invokes the tool.

**Steps:**
1. Test: run map2c on `docs/maps/melodee.json`, diff against golden `map.c`.
2. Implement; wire into a `build.sh` step.
3. Commit `feat: map2c — JSON map to const C table`.

## Task 5: JieLi firmware port

**Objective:** Same modules compiled for the FM-1 (pi32v2), USB MIDI in/out
live, LEDs driven from MIDI in. Build with `JIELI_TOOLCHAIN` + `AC79_SDK` set.

**Files:**
- Create: `firmware/main_fm1.c` (SDK entry, HAL init, main loop)
- Modify: `firmware/midi_io.c` (MIDI_USB backend via SDK USB device MIDI)

**Steps:**
1. Study sloop/melodee USB-MIDI init (both proved UAC+MIDI paths) — copy the
   descriptor handling pattern for a MIDI-only config.
2. Port modules (they must compile unchanged — that's the point of host-first).
3. Build → `fm1-ctrl.fwsc`, report flash headroom in the PR/release notes.
4. Commit `feat: FM-1 firmware port`.

## Task 6: On-device validation + latency measurement

**Objective:** John's FM-1 plays melodee's emulator; LEDs track playback;
MIDI-in→audio latency measured (idea 4's test rig deliverable).

**Steps:**
1. Flash fm1-ctrl.fwsc (hub installer or fm1pkg).
2. Chromium + shim page + melodee emu: play keys → sound; press play on emu →
   FM-1 LEDs walk.
3. Latency rig: record key press (contact) vs audio onset, report ms
   (n≥20, median). Record in `docs/latency.md`.
4. Tag `v0.1.0`, post maps to hub catalogue.

---

## Out of scope (explicit)

Sequencer/GM/storage (→ DriftSeq grows on this), runtime map switching,
screen value display, MIDI DIN, non-Chromium browsers.

## Risks / open questions

- FM-1 USB MIDI **in** (host→device) support in the Felucca-lineage stack is
  unverified — Task 5's step 1 resolves it; fallback = one-way controller.
- melodee emu knob elements may be canvas, not DOM — Task 3 step 3 finds out;
  fallback = dispatch synthetic pointer events at element coordinates.
