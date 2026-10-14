/* fm1-ctrl control map -- compiled-in projection of a docs/maps JSON file.
 *
 * v1: one map baked at build time (tools/map2c.py generates this file from the
 * JSON in Task 4; until then the melodee values are hand-copied here).
 * Runtime map switching is out of scope (spec §2).
 */
#ifndef MAP_H
#define MAP_H
#include <stdint.h>

uint8_t map_encoder_cc(int idx);   /* encoder i -> CC number */

#endif
