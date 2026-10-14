#include "keyscan.h"
#include "midi_io.h"
#include "map.h"
#include <stdbool.h>

#define NOTE_BOT  48  /* key idx 0..13  = C3..B3 */
#define NOTE_TOP  60  /* key idx 14..26 = C4..A4 */
#define BTN_CC0   40  /* default CC for button 0 (map may override) */
#define VEL       100

static bool      held[NSTATE];
static uint64_t  curr_raw;

void keyscan_init(void) {
    for (int i = 0; i < NSTATE; i++) held[i] = false;
    curr_raw = 0;
}

void keyscan_poll(uint64_t raw_mask) {
    /* 2-frame consensus: an edge is accepted only when this frame and the
     * previous frame agree — a single-frame glitch is dropped entirely. */
    for (int i = 0; i < NSTATE; i++) {
        bool now  = (raw_mask >> i) & 1u;
        bool prev = (curr_raw >> i) & 1u;
        if (now != prev || held[i] == now) continue;
        held[i] = now;
        if (i < NKEYS) {
            uint8_t n = (uint8_t)(i < 14 ? NOTE_BOT + i : NOTE_TOP + (i - 14));
            midi_emit3(now ? 0x90 : 0x80, n, now ? VEL : 0x00);
        } else {
            uint8_t cc = map_btn_cc(i - NKEYS);
            midi_emit3(0xB0, cc, now ? 127 : 0);
        }
    }
    curr_raw = raw_mask;
}
