# Controlling another firmware's web/emu — how far it really goes

The pitch: put the real FM-1 under your hands and play a firmware that runs in the
browser, without flashing the unit. This page is the honest map of where that works,
where it needs a shim, and where it cannot work at all.

Checked 2026-10-15 against the actual sources:

| firmware | web emu | consumes notes | consumes CC | buttons reach the UI | verdict |
|---|---|---|---|---|---|
| **melodee** | `webapp/simulator` (wasm) | yes | CC 20–26 knobs, CC 27 master | no — `{type:"buttons"}` only | knobs + keys + master **work today**; buttons need the shim |
| **Felucca** | `web/emu` (wasm) | yes | CC 20–26, CC 27 | no | same as melodee (shared `worklet.js`) |
| **Sloop** | installer only, no emu page | notes | **no CC at all** | no | firmware `seq.c` drops every non-note message |
| **fm1-ctrl on itself** | `emu.html` (this repo) | yes | all, mapped | yes | the reference |

## What the shared worklet gives us for free

`web/emu/worklet.js` in both Felucca and Melodee:

```js
export const KNOB_CC = 20, MASTER_CC = 27, KNOB_MODES = ["abs","bin","twos","sign"];
```

and the page's MIDI handler:

```js
const k = knobsOn && (d[0] & 0xF0) === 0xB0 ? midiKnob(knobCC, d[1], d[2] || 0, knobMode) : null;
if (!k) send({ type: "midi", data: [d[0], d[1], d[2] || 0] });   // to the synth
else if (k.master !== undefined) { setMaster(k.master); ... }
else turn(knobs.find((x) => x.role === k.role), k.n);
```

So with the "knobs" box ticked, **our CC 20–26 and CC 27 drive those pages' knobs
directly** — that is the default map in `firmware/map_data.c` (generated from
`docs/maps/melodee.json`, byte-identical to `docs/maps/felucca.json`). No shim, no
reflash of the emulator, nothing to install.

Keys go through as notes and reach the firmware (the page forwards them to the
worklet as `{type:"midi"}`), so playing the real keyboard makes the emulated firmware
sound.

## What does not work: buttons

Both pages keep their panel buttons on a **different channel from MIDI**:

```js
send({ type: "buttons", mask });      // bit per panel button, in the page's own order
```

That message is produced by clicking the on-screen panel. The WebMIDI handler never
produces it, and neither firmware's MIDI-in accepts a CC for a panel button. So
pressing FX on the real unit changes nothing in those pages.

This is a UI-decoupling problem, not a mapping problem: the information is simply not
in the MIDI stream. Two ways out, in order of effort:

1. **`web/fm1-shim.js` (shipped here).** Load it into the emu page; it takes the FM-1's
   MIDI and calls the page's own `send()` with `{type:"buttons"|"keys"|"enc"|"master"}`,
   i.e. the exact messages the on-screen panel makes. The firmware cannot tell the
   difference. Needs one `setSink(...)` line if the page keeps `send` in a closure
   (both of these do — `node.port.postMessage` is the same channel).
2. **Fork the page** and add a `case 0xB0:` for the panel buttons. Ten lines, but it
   means maintaining a fork per firmware.

The shim is the general answer, and it is also what makes the *next* firmware cheap:
any page that has a keyboard/knob/button message API can be driven the same way.

## Runtime maps over SysEx (shipped, `map.c`)

An emu whose CC numbers differ needs a new map, and re-flashing the unit to change a
map is not acceptable. So the map is writable at runtime:

```
F0 7D 43 54 01 <nenc> <enc CCs…> <nbtn> <btn CCs…> <btnLed0> <flags> F7   SET
F0 7D 43 54 02 F7                                                        GET  (echoes the map)
F0 7D 43 54 03 F7                                                        RESET (compiled default)
```

- `flags` bit0 = key-LED mirror on, bit1 = buttons send **notes** instead of CC
  (the RemoteFM1 convention: matrix ids as note numbers).
- **Not persisted.** A power cycle returns to the map compiled in from
  `docs/maps/<emu>.json`. That is deliberate: flash is the thing that bricks units.
- 0x7D is the non-commercial SysEx id; `43 54` = "CT". No other FM-1 firmware uses it,
  and ours ignores everything else, so sharing a bus with `fm1ota.js` traffic is safe
  (`ctrl_service_map_sysex()` checks the header before `ota_take()` sees the frame).

Verified in the host tests (T13: set/get/reset, button-as-note) and in the wasm
(`web/emu/emu_test.mjs` + the map test): remap encoder 0 to CC 30, press button 0,
and the byte stream really is `B0 3C 7F`.

## Where the limits are, plainly

- **A firmware whose panel buttons never produce a message** cannot be driven from
  outside, by us or by anyone. Sloop's `seq.c` accepts notes only; its web editor
  talks to the device over SysEx, not over panel MIDI. That needs a fork of the
  firmware, which is out of scope here.
- **The screen**: we do not send pixels. The emu page already draws its own screen, so
  this costs nothing for UI work — but the *feel* of the real 240×240 panel is not part
  of what you can evaluate this way. RemoteFM1 solves that with a tile protocol; we
  deliberately do not.
- **Velocity**: the FM-1's key rows are analog and the Felucca HAL has the sense data,
  but `keyscan.c` sends a fixed 100 in v1. If a firmware's UI cares about velocity
  (melodee's does), that is the next thing to wire.

## Why this is worth building even though RemoteFM1 exists

RemoteFM1 already runs a firmware on the host and takes the panel over — including the
screen — but its input is a private protocol: notes on channel 16, buttons on channel
15, encoders on CC 16–22, and the host must be its own `libremotefm1`. Its own README
puts it well: "the screen reads REMOTE FM1 / WAITING FOR HOST" without a host.

Ours stays a **plain USB MIDI controller with a configurable map**, which buys three
things RemoteFM1 cannot:

1. It works with **the emulator that already exists** — the melodee simulator you have
   open in Chrome, no host binary, no protocol.
2. It works **with no computer at all**: a DAW, an iPad, a hardware synth.
3. The map lives in JSON and can be pushed **over SysEx** at runtime, so covering a new
   firmware does not need a new firmware build, let alone a flash.
