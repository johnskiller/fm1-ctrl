#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-ctrl in the browser: the firmware's own modules compiled to WebAssembly with
# Emscripten, after Felucca's web/emu/build.sh (Hugelton Instruments, GPL-3.0) and
# melodee's web/emu/build.sh (keremimo).
#   web/emu/build.sh  ->  build/emu/{fm1ctrl.wasm, index.html}
set -e
cd "$(dirname "$0")/../.."
command -v emcc >/dev/null 2>&1 || { echo "emu: no emcc (Emscripten); source emsdk_env.sh"; exit 1; }
mkdir -p build/emu
# freestanding: no USB, no OTA, no screen. The page is the panel.
# shellcheck disable=SC2086
emcc ${EMU_OPT:--O2} -std=gnu99 -w \
    -Ibuild/emu -Ifirmware \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=16777216 -sFILESYSTEM=0 \
    -sEXPORTED_RUNTIME_METHODS=[] -sALLOW_TABLE_GROWTH=0 \
    -o build/emu/fm1ctrl.wasm web/emu/emu_web.c
cp web/emu/index.html build/emu/index.html
echo "emu: build/emu ($(wc -c < build/emu/fm1ctrl.wasm | tr -d ' ') B wasm)"
