#include "leds.h"

#define GRID_ROWS 16

/* physical key+button note span (keys F3..G5 = notes 53..79) */
#define NOTE_MIN 53
#define NOTE_MAX 79

static uint8_t grid[GRID_ROWS];
static uint8_t keyled[128];

void leds_init(void) {
    for (int i = 0; i < GRID_ROWS; i++) grid[i] = 0;
    for (int i = 0; i < 128; i++) keyled[i] = 0;
}

void leds_on_cc(uint8_t cc, uint8_t val) {
    if (cc < GRID_ROWS) grid[cc] = val;
}

void leds_on_note(uint8_t note, int on) {
    if (note >= NOTE_MIN && note <= NOTE_MAX) keyled[note] = on ? 1 : 0;
}

uint8_t leds_grid_get(int cc) { return (cc >= 0 && cc < GRID_ROWS) ? grid[cc] : 0; }
uint8_t leds_key_get(int n)   { return (n >= 0 && n < 128) ? keyled[n] : 0; }
