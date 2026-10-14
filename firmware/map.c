#include "map.h"

/* accessors over the generated data in map_data.c (tools/map2c.py) */

uint8_t map_encoder_cc(int idx) {
    extern const uint8_t MAP_ENC_CC[8];
    return (idx >= 0 && idx < 8) ? MAP_ENC_CC[idx] : 0;
}

uint8_t map_btn_note(int i) {
    extern const uint8_t MAP_BTN_NOTE[3];
    return (i >= 0 && i < 3) ? MAP_BTN_NOTE[i] : 0;
}

map_led_cfg_t map_led(void) {
    extern const map_led_cfg_t MAP_LED;
    return MAP_LED;
}

uint8_t map_mirror(void) {
    extern const uint8_t MAP_MIRROR;
    return MAP_MIRROR;
}
