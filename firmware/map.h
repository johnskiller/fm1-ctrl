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
uint8_t map_btn_led0(void);           /* button-LED feedback base CC        */
uint8_t map_btn_mode(void);           /* 0 = buttons send CC, 1 = notes      */

/* runtime map, replaced over SysEx (F0 7D 43 54 ...): not persisted, a power
 * cycle returns to the compiled default from map_data.c */
void     map_reset(void);
int      map_sysex_is_ours(const uint8_t *p, uint32_t n);
int      map_sysex(const uint8_t *p, uint32_t n);        /* 1 ours, 0 not, -1 bad */
uint32_t map_sysex_build(uint8_t *out);                  /* GET answer, no F0/F7 */

#endif
