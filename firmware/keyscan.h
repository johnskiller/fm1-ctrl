/* fm1-ctrl key scan — pure logic, no I/O (host-tested per plan Task 1).
 *
 * Key index map (spec §4):
 *   idx 0..13  bottom row, notes 48..61 (C3..B3)
 *   idx 14..26 top row,    notes 60..72 (C4..A4)
 *   idx 27..29 transport PLAY/STOP/REC -> notes 36/37/38
 * Velocity fixed 100 (0x64) in v1 (no pressure sensing).
 */
#ifndef KEYSCAN_H
#define KEYSCAN_H
#include <stdint.h>

void keyscan_init(void);
/* raw_mask bit i = key i contact closed this scan frame (raw, pre-debounce) */
void keyscan_poll(uint32_t raw_mask);

#endif
