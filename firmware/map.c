#include "map.h"

/* docs/maps/melodee.json inputs.ccs (hand-copied until map2c.py lands) */
static const uint8_t ENC_CC[8] = { 71, 74, 84, 7, 1, 91, 93, 10 };

uint8_t map_encoder_cc(int idx) {
    return (idx >= 0 && idx < 8) ? ENC_CC[idx] : 0;
}
