/* fm1-ctrl control map -- generated interface (see tools/map2c.py).
 *
 * firmware/map_data.c is GENERATED from a docs/maps JSON file. These accessors
 * hide whether a value came from the map or from a built-in default.
 */
#ifndef MAP_H
#define MAP_H
#include <stdint.h>

typedef struct { uint8_t present, lo, hi; } map_led_cfg_t;

uint8_t map_encoder_cc(int idx);      /* encoder i -> CC number            */
uint8_t map_btn_note(int i);          /* transport PLAY/STOP/REC note      */
map_led_cfg_t map_led(void);          /* LED grid CC range (present flag)  */
uint8_t map_mirror(void);             /* key-LED note mirror enabled       */

#endif
