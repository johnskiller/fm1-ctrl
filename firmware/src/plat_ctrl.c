/* fm1-ctrl's plat_fm1.c subset: the MIDI ring accessors over usb.c's statics.
 * Derived from x0x's app/plat_fm1.c (GPL-3.0-only, Hügelton Instruments). */
/* counters for the on-screen diagnostic (ctrl_main.c draws them):
 * tx = USB-MIDI event packets handed to the SIE, rx = packets taken from the
 * host. Nothing else counts MIDI, so a zero here means nothing was produced. */
volatile uint32_t ctrl_dbg_tx, ctrl_dbg_rx;

int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = midi_in_q[mi_r % MQ];
    mi_r++;
    ctrl_dbg_rx++;
    return 1;
}
void plat_midi_out(uint32_t pkt)
{
    midi_out_event(pkt);
    ctrl_dbg_tx++;
}
