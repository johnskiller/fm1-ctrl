#!/bin/sh
# fm1-ctrl build: map2c -> host tests + simulator (firmware cross-build = Task 5)
set -e
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-cc}
MODS="firmware/keyscan.c firmware/encoders.c firmware/leds.c firmware/midi_io.c firmware/map.c firmware/map_data.c"

# map data is generated from the JSON map (single source of truth with the shim)
python3 tools/map2c.py docs/maps/melodee.json -o firmware/map_data.c

$CC -O2 -Wall -Wextra -std=c99 -DMIDI_HOST -Ifirmware -o build/test_midi host/test_midi.c $MODS
$CC -O2 -Wall -Wextra -std=c99 -DMIDI_HOST -Ifirmware -o build/sim      host/sim.c      $MODS

build/test_midi
