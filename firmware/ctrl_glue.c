#include "ctrl_glue.h"
#include "keyscan.h"
#include "encoders.h"
#include "leds.h"
#include "midi_io.h"
#include "map.h"

void ctrl_init(void) {
    map_reset();
    keyscan_init(); encoders_init(); leds_init(); midi_io_init();
}

void ctrl_frame_scan(uint64_t raw) { keyscan_poll(raw); }
void ctrl_frame_enc(uint16_t ab, uint8_t push) { encoders_poll(ab, push); }
void ctrl_midi_in3(uint8_t st, uint8_t d1, uint8_t d2) { midi_recv3(st, d1, d2); }


int ctrl_master_accept(int32_t adc, int32_t *last)
{
    int32_t d;
    if (adc < 0)                       /* conversion timeout: keep the old value */
        return 0;
    if (*last < 0) {                   /* first good read: always announce it */
        *last = adc;
        return 1;
    }
    d = adc - *last;
    if (d < CTRL_MASTER_DEADBAND && d > -CTRL_MASTER_DEADBAND)
        return 0;
    *last = adc;
    return 1;
}
