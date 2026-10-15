# FM-1 control map convention v0.2

One JSON file per emulator declares how the FM-1's controls map to that emu's
page controls and how emu state feeds the FM-1's LEDs back. Shims are generic —
they only read maps. Maps are static files, hostable anywhere (the fm1 hub can
serve them).

## Panel topology (real FM-1, Felucca-lineage panel.c)

- **27 keys**: bottom row = notes 48..61 (C3..B3), top row = notes 60..72 (C4..A4)
- **7 encoders**: SELECT, ALGORITHM, PRESETS, KNOB 1..4
- **14 buttons** (panel order): FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP,
  SEQ, PLAY, REC, OCT−, OCT+

## File shape

```json
{
  "version": "0.2",
  "emu": "melodee",
  "url": "https://johnskiller.github.io/melodee/webapp/simulator/",
  "native": true,
  "inputs": {
    "notes": {},
    "ccs":   { "20": "knob:0", "21": "knob:1", "22": "knob:2",
               "23": "knob:3", "24": "knob:4", "25": "knob:5", "26": "knob:6",
               "40": "btn:0", "41": "btn:1", "...": "btn:13 -> CC 53" }
  },
  "outputs": {
    "ledGrid":  { "cc": [0, 15],  "mode": "bitmap" },
    "keyLeds":  { "noteOn": "mirror" },
    "btnLeds":  { "cc": [54, 67], "mode": "one-cc-per-button", "order": "panel" }
  }
}
```

## Rules

- `inputs.ccs`: CC number → `knob:<0..6>` (encoder role) or `btn:<0..13>`
  (panel-order button). Buttons send **127 on press, 0 on release**.
- `inputs.notes`: MIDI note number → `key:<n>` (rare; keys already ARE notes).
- Encoders emit **relative CC, MCU-style**: 64+delta (0x41=+1, 0x3F=−1, one
  detent = ±1). Emus whose knobs take absolute values should use a mode like
  melodee's `midiKnob(abs)` — value − last — which digests relative natively.
  Push+turn duplicates the stream on channel 2 (0xB1) as a second layer.
- `native: true` = the emu consumes Web MIDI directly; with this map baked into
  the firmware, NO shim is needed (melodee's simulator is the first native case).
- `outputs.ledGrid`: a CC range carries the LED bitmap (CC 0–15 = 16 rows ×
  8 bits = 128 cells; FM-1 grid is 5×7 + keys, plenty).
- `outputs.keyLeds: "mirror"` = note on/off lights the physical key LED.
- `outputs.btnLeds`: one CC per button, CC 54..67 in panel order (value != 0 = on).
  With grid (0..15) + key mirror (notes 53..79) + buttons (54..67) the emu can
  reach all 41 physical LEDs: 27 keys + 12 command buttons + OCT-/OCT+.
- Anything absent from the map is ignored by the shim — partial maps are fine.

## Firmware side

The firmware does NOT parse JSON — `tools/map2c.py` compiles a map into const C
tables (`firmware/map_data.c`) at build time. v1 ships with exactly one
compiled-in map (selected by a build flag); runtime map switching is out of scope.
