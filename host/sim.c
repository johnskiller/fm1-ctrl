/* fm1-ctrl host simulator (plan Task 1) — drives the same modules the
 * firmware will, prints the MIDI byte stream the device would send.
 *
 * Commands (whitespace separated, read from stdin):
 *   k IDX     key press           (IDX 0..29)
 *   K IDX     key release
 *   s HEXMASK set full scan mask  (e.g. `s 1000` = key 12)
 *   e ENC N   turn encoder ENC by N detents (N negative = CCW)
 *   E ENC     toggle encoder push
 *   m ST D1 D2  inject MIDI-in bytes (decimal, e.g. `m 176 5 10`)
 *   L         print LED state
 *   q         quit
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "midi_io.h"
#include "keyscan.h"
#include "encoders.h"
#include "leds.h"

static uint64_t mask;
static uint8_t  push;

static long rd(const char *fmt) { long v = 0; if (scanf(fmt, &v) != 1) exit(1); return v; }

static void flush_midi(void) {
    for (int i = 0; i < midi_cap_len(); i++) {
        midi_msg3 m = midi_cap_get(i);
        printf("MIDI %02X %02X %02X\n", m.b[0], m.b[1], m.b[2]);
    }
    midi_cap_clear();
}

static void turn(int enc, int n) {
    /* both sequences start and end at phase 0, so consecutive commands work */
    static const uint8_t seq[2][4] = { {1, 3, 2, 0}, {2, 3, 1, 0} };
    int dir = n >= 0 ? 0 : 1;
    for (int d = 0; d < (n < 0 ? -n : n); d++)
        for (int s = 0; s < 4; s++)
            encoders_poll((uint16_t)(seq[dir][s] << (enc * 2)), push);
}

int main(void) {
    keyscan_init(); encoders_init(); leds_init(); midi_io_init();
    char cmd[8];
    long a = 0, b = 0, c = 0;
    while (scanf("%7s", cmd) == 1) {
        if      (!strcmp(cmd, "q")) break;
        else if (!strcmp(cmd, "k")) { a = rd("%ld"); mask |=  1ull << a; keyscan_poll(mask); keyscan_poll(mask); }
        else if (!strcmp(cmd, "K")) { a = rd("%ld"); mask &= ~(1ull << a); keyscan_poll(mask); keyscan_poll(mask); }
        else if (!strcmp(cmd, "s")) { a = rd("%lx"); mask = (uint64_t)a; keyscan_poll(mask); keyscan_poll(mask); }
        else if (!strcmp(cmd, "e")) { a = rd("%ld"); b = rd("%ld"); turn((int)a, (int)b); }
        else if (!strcmp(cmd, "E")) { a = rd("%ld"); push ^= (uint8_t)(1u << a); }
        else if (!strcmp(cmd, "m")) { a = rd("%ld"); b = rd("%ld"); c = rd("%ld");
                                      midi_recv3((uint8_t)a, (uint8_t)b, (uint8_t)c); }
        else if (!strcmp(cmd, "L")) {
            printf("GRID ");
            for (int i = 0; i < 16; i++) printf("%02X", leds_grid_get(i));
            printf("\nKEYS");
            for (int n = 36; n <= 72; n++) if (leds_key_get(n)) printf(" %d", n);
            printf("\n");
            continue; /* no midi to flush */
        }
        flush_midi();
    }
    return 0;
}
