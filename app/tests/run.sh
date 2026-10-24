# SPDX-License-Identifier: MPL-2.0
# fm1-ctrl's contract tests: the real app/app.c on a fake platform.
#   sh app/tests/run.sh
set -e
cd "$(dirname "$0")/../.."

CC=${CC:-cc}
OUT=app/tests/build
mkdir -p "$OUT"

# app.c includes fastmath.h from the platform; nothing else of the platform is
# linked -- that is the point: the app must stand on plat.h alone.
$CC -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -I platform/include \
    -o "$OUT/test_map" \
    app/tests/test_map.c app/tests/fake_plat.c

"$OUT/test_map"
