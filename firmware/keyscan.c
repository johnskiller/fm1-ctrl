#include "keyscan.h"
#include "midi_io.h"
#include <stdbool.h>

#define NKEYS     27
#define NOTE_BOT  48  /* idx 0..13  = C3..B3 */
#define NOTE_TOP  60  /* idx 14..26 = C4..A4 */
#define BTN_BASE  27  /* idx 27..29 = PLAY/STOP/REC */
#define BTN_NOTE0 36
#define VEL       100

#define NSTATE (NKEYS + 3)

static bool     held[NSTATE];
static uint32_t curr_raw;

static uint8_t key_note(int i) {
    if (i >= BTN_BASE) return BTN_NOTE0 + (i - BTN_BASE);
    if (i < 14)        return NOTE_BOT + i;
    return NOTE_TOP + (i - 14);
}

void keyscan_init(void) {
    for (int i = 0; i < NSTATE; i++) held[i] = false;
    curr_raw = 0;
}

void keyscan_poll(uint32_t raw_mask) {
    /* 2-frame consensus: an edge is accepted only when this frame and the
     * previous frame agree — a single-frame glitch is dropped entirely. */
    for (int i = 0; i < NSTATE; i++) {
        bool now  = (raw_mask >> i) & 1u;
        bool prev = (curr_raw >> i) & 1u;
        if (now == prev && held[i] != now) {
            held[i] = now;
            uint8_t n = key_note(i);
            midi_emit3(now ? 0x90 : 0x80, n, now ? VEL : 0x00);
        }
    }
    curr_raw = raw_mask;
}
