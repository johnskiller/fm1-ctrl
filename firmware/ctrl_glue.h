/* fm1-ctrl glue — the frame functions that run on-device, in a host-testable
 * form. No HAL includes here: on the FM-1, ctrl.c's unity root feeds these
 * from fm1_in.* / plat_midi_* (see docs/research/usb-midi.md §6).
 */
#ifndef CTRL_GLUE_H
#define CTRL_GLUE_H
#include <stdint.h>

void ctrl_init(void);
/* one scan frame: raw bit i = scan point i closed (27 keys, then 14 buttons) */
void ctrl_frame_scan(uint64_t raw);
/* one encoder frame: ab = 2 bits per encoder, push = bit per encoder */
void ctrl_frame_enc(uint16_t ab, uint8_t push);
/* one MIDI-in message from the host (LED feedback path) */
void ctrl_midi_in3(uint8_t st, uint8_t d1, uint8_t d2);
/* device: copy leds.c state into the panel's LED matrix (weak no-op on host) */
void ctrl_leds_flush(void);

/* MASTER pot conditioning, host-testable.
 *
 * The pot is a 10-bit conversion but the 14-bit CC pair exposes its low three
 * bits: CC39 = (adc & 7) << 4, so ONE count of ADC noise moves the "fine" byte
 * by 16 -- the chatter (16 / 48) seen on the panel. Two counters:
 *
 *   CTRL_MASTER_OVERSAMPLE  reads averaged per poll (noise / sqrt(n))
 *   CTRL_MASTER_DEADBAND    hysteresis in 10-bit counts: below this, no event
 *
 * 6 counts is 0.6 % of the pot's travel -- under what a hand can play, over the
 * +-2 counts the conversion itself wanders. ctrl_master_accept() returns 1 when
 * the value should be emitted, and remembers it in *last (-1 = nothing yet). */
#define CTRL_MASTER_OVERSAMPLE 4
#define CTRL_MASTER_DEADBAND   6

int ctrl_master_accept(int32_t adc, int32_t *last);

#endif
