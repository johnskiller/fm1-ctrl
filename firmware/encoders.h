/* fm1-ctrl encoders — 8 rotary encoders, quadrature decode -> relative CC.
 *
 * ab:   2 bits per encoder (A|B pair), encoder i in bits [2i, 2i+1]
 * push: bit i = encoder i switch held
 *
 * One mechanical detent = one full quadrature cycle (4 valid transitions).
 * turn        -> CC ch1 (0xB0), value 64+delta (MCU-style: 0x41=+1, 0x3F=-1)
 * push + turn -> same relative CC on ch2 (0xB1) — a second layer for the shim
 *                (emu knobs are drag-relative anyway; a fake absolute value
 *                would drift — spec §2 revised 2026-10-14)
 * CC numbers come from the compiled-in map (map.c).
 */
#ifndef ENCODERS_H
#define ENCODERS_H
#include <stdint.h>

void encoders_init(void);
void encoders_poll(uint16_t ab, uint8_t push);

#endif
