# FM-1 control map convention v0.1

One JSON file per emulator declares how the FM-1's controls map to that emu's
page controls and how emu state feeds the FM-1's LEDs back. Shims are generic —
they only read maps. Maps are static files, hostable anywhere (the fm1 hub can
serve them).

## File shape

```json
{
  "version": "0.1",
  "emu": "melodee",
  "url": "https://johnskiller.github.io/melodee/webapp/simulator/",
  "inputs": {
    "notes":  { "36": "button:a",  "37": "button:b", "38": "button:c" },
    "ccs":    { "71": "knob:0", "74": "knob:1", "84": "knob:2",
                 "7": "knob:3", "1": "knob:4", "91": "knob:5",
                "93": "knob:6", "10": "knob:7" }
  },
  "outputs": {
    "ledGrid":  { "cc": [0, 15],  "mode": "bitmap", "note": "CC n value bit n-0" },
    "keyLeds":  { "noteOn": "mirror" }
  }
}
```

## Rules

- `inputs.notes`: MIDI note number (string key) → target selector
  `button:<id>` or `key:<id>` (emu-page element ids).
- `inputs.ccs`: CC number → `knob:<id>` / `button:<id>` (momentary).
- `outputs.ledGrid`: a CC range carries the LED bitmap (CC 0–15 = up to
  16 bits × 16 CCs = 256 cells; FM-1 grid is 5×7 + keys, plenty).
- `outputs.keyLeds: "mirror"` = note on/off lights the physical key LED.
- Anything absent from the map is ignored by the shim — partial maps are fine.
- Default map (when an emu has none): spec §4's built-in assignment; knobs only.

## Firmware side

The firmware does NOT parse JSON — `tools/map2c.py` compiles a map into a const
C table (`map.c`) at build time. v1 ships with exactly one compiled-in map
(selected by a build flag); runtime map switching is out of scope.
