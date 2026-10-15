#include "map.h"

/* accessors over the generated data in map_data.c (tools/map2c.py) */

uint8_t map_encoder_cc(int idx) {
    extern const uint8_t MAP_ENC_CC[MAP_NENC];
    return (idx >= 0 && idx < MAP_NENC) ? MAP_ENC_CC[idx] : 0;
}

uint8_t map_btn_cc(int i) {
    extern const uint8_t MAP_BTN_CC[MAP_NBTN];
    return (i >= 0 && i < MAP_NBTN) ? MAP_BTN_CC[i] : 0;
}

map_led_cfg_t map_led(void) {
    extern const map_led_cfg_t MAP_LED;
    return MAP_LED;
}

uint8_t map_btn_led0(void) {
    extern const uint8_t MAP_BTN_LED0;
    return MAP_BTN_LED0;
}

uint8_t map_mirror(void) {
    extern const uint8_t MAP_MIRROR;
    return MAP_MIRROR;
}
