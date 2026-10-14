#!/bin/sh
# fm1-ctrl host build: tests + simulator (firmware cross-build lands in Task 5)
set -e
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-cc}
SRC="firmware/keyscan.c firmware/encoders.c firmware/leds.c firmware/midi_io.c firmware/map.c"

$CC -O2 -Wall -Wextra -std=c99 -DMIDI_HOST -Ifirmware -o build/test_midi host/test_midi.c $SRC
$CC -O2 -Wall -Wextra -std=c99 -DMIDI_HOST -Ifirmware -o build/sim      host/sim.c      $SRC

build/test_midi
