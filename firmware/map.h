/* fm1-ctrl control map -- generated interface (see tools/map2c.py).
 *
 * firmware/map_data.c is GENERATED from a docs/maps JSON file. These accessors
 * hide whether a value came from the map or from a built-in default.
 */
#ifndef MAP_H
#define MAP_H
#include <stdint.h>

#define MAP_NENC 7
#define MAP_NBTN 14

typedef struct { uint8_t present, lo, hi; } map_led_cfg_t;

uint8_t map_encoder_cc(int idx);      /* encoder i -> CC number (0..6)      */
uint8_t map_btn_cc(int i);            /* button i -> CC number (0..13)      */
map_led_cfg_t map_led(void);          /* LED grid CC range (present flag)   */
uint8_t map_mirror(void);             /* key-LED note mirror enabled        */

#endif
