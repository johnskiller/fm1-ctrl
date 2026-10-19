/* fm1-ctrl on-device glue: TIMER5 ISR, MIDI transport thunks, boot + main loop.
 * Included last by ctrl.c (unity root) — sees the platform's statics
 * (fm1_ms, usb_*, panel) as file-local symbols, like x0x's main_fm1.c.
 * App layer: Copyright (C) 2026 johnskiller, GPL-3.0-only.
 */
#include "midi_io.h"      /* our transport hook + midi_msg3 (from -Ifirmware) */
#include "keyscan.h"
#include "leds.h"
#include "ctrl_glue.h"
#include "fm1_cpu1.h"
#include "fm1_adc.h"      /* MASTER pot (ADC ch4), RemoteFM1 PROTOCOL section 4 */

/* ---- MIDI transport: 3-byte app events <-> USB-MIDI event packets ----
 * Felucca's rings carry 4-byte USB-MIDI packets, byte 0 = cable/CIN.
 * CIN 0x9 = note, 0xB = CC/other channel messages, cable 0. */
static void midi_transport_usb3(uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t cin = ((st & 0xF0u) == 0x90u || (st & 0xF0u) == 0x80u) ? 0x9u : 0xBu;
    plat_midi_out((uint32_t)cin | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}

/* host -> device (LED feedback): drain plat_midi_in in the main loop */
static void ctrl_drain_midi_in(void)
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16), d2 = (uint8_t)(pkt >> 24);
        ctrl_midi_in3(st, d1, d2);
    }
}

/* ---- panel tables: matrix id <-> label id (panel.c), used for buttons, encoders
 * and LEDs. RemoteFM1 PROTOCOL section 4 numbers the matrix 0 = OCT-, 1 = OCT+,
 * 2..13 = FX..REC; our map/pages are in printed-label order. ---- */
static int btn_matrix_to_label(int m)
{
    uint32_t i;
    for (i = 0; i < NB; i++)
        if (panel.btn[i] == m)
            return (int)i;
    return m;                        /* no calibration entry: pass through */
}
/* encoders: the HAL already counts detents (fm1_enc_take); role order is the map's
 * (SELECT ALGO PRESETS K1..K4). applied dir so clockwise is always +. */
static void ctrl_poll_encoders(void)
{
    uint32_t r;
    for (r = 0; r < NE; r++) {
        int e = (int)panel_matrix_enc(r);
        int32_t st = fm1_enc_take((uint32_t)e);
        if (st)
            encoders_step((int)r, (int)st * (int)panel_dir_enc(r), 0);
    }
}

/* MASTER pot: 14-bit CC pair 7 (msb) / 39 (lsb), on a >=2 count change, <=200 Hz
 * (RemoteFM1 PROTOCOL section 4; melodee takes CC 27 as its own master, kept
 * separate on purpose -- this is the physical pot, always sent). */
static void ctrl_poll_master(void)
{
    static int32_t last = -1;
    int32_t adc = fm1_adc_read(FM1_ADC_MASTER);
    if (adc < 0)
        return;
    if (last >= 0 && adc - last < 2 && last - adc < 2)
        return;
    last = adc;
    {
        uint32_t v14 = (uint32_t)adc << 4;
        midi_emit3(0xBE, 7,  (uint8_t)(v14 >> 7));
        midi_emit3(0xBE, 39, (uint8_t)(v14 & 0x7F));
    }
}

/* LED feedback to the physical panel: leds.c state -> HAL matrix. The HAL takes
 * matrix ids (buttons 0..13, note keys 14..40) and its own dim/lit scan. */
void ctrl_leds_flush(void)
{
    uint32_t i;
    static uint32_t prev_key, prev_btn;
    uint32_t key = 0, btn = 0;
    for (i = 0; i < 27u; i++)
        key |= (uint32_t)leds_key_get(NOTE_KEY0 + (int)i) << i;
    for (i = 0; i < 14u; i++)
        btn |= (uint32_t)leds_btn_get((int)i) << i;
    if (key == prev_key && btn == prev_btn)
        return;                      /* cheap: only on change */
    prev_key = key; prev_btn = btn;
    for (i = 0; i < 14u; i++)
        fm1_led_key(panel_matrix_btn((uint32_t)i), (btn >> i) & 1u);
    for (i = 0; i < 27u; i++)
        fm1_led_key(14u + i, (key >> i) & 1u);
}

/* ---- TIMER5 ISR: x0x's, minus audio/UAC ---- */
static void isr_timer5(void);
static void ctrl_timer5_irq(void)
{
    static uint32_t sub;
    uint8_t usb_due;
    fm1_timer5_ack();
    fm1_input_tick();
    usb_due = (sub % 5u) == 0u;
    if (usb_due)
        usb_poll();
    if (++sub == 10u)
        sub = 0;
    {                                   /* fm1_ms, as x0x keeps it */
        static uint32_t last, acc;
        uint32_t now = fm1_ticks();
        acc += now - last;
        last = now;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
}

/* ---- boot: x0x's fm1_cstart, minus CPU1/storage/panel-setup extras ---- */
static void ctrl_main(void);
extern uint8_t _bss_start[], _bss_end[], _pool_start[], _pool_end[];
extern const uint8_t _data_load[], _data_start[], _data_end[];

void fm1_cstart(void)
{
    uint32_t *d;
    fm1_cpu1_halt();
    fm1_time_init();
    fm1_reset_reason();
    fm1_wdt_arm(0x0D);
    fm1_irq_init();
    for (d = (uint32_t *)_bss_start; d < (uint32_t *)_bss_end; d++)
        *d = 0;
    for (d = (uint32_t *)_pool_start; d < (uint32_t *)_pool_end; d++)
        *d = 0;
    {                                   /* .data: load -> start (x0x's word copy) */
        const uint8_t *s;
        for (s = _data_load, d = (uint32_t *)_data_start; (uint8_t *)d < _data_end; s += 4, d++)
            *d = *(const uint32_t *)s;
    }
    fm1_irq_enable_all();
    ctrl_main();
    for (;;)
        ;
}

/* ---- main loop ---- */
static void ctrl_main(void)
{
    uint32_t t0;
    lcd_init();
    palette_set(1);
    draw_text_box(0, 96, 240, &FONT_M, "fm1-ctrl", C_HI, 1);
    draw_text_box(0, 134, 240, &FONT_S, "EVERY CONTROL EMITS", C_GRAY, 1);
    fm1_input_init();
    panel_init();
    midi_transport_send = midi_transport_usb3;
    keyscan_btn_map(btn_matrix_to_label);
    fm1_adc_init();
    ctrl_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(300);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_M, "fm1-ctrl", C_HI, 1);

    t0 = fm1_ms;
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        ctrl_frame_scan((uint64_t)fm1_in.notes | (uint64_t)fm1_in.buttons << 27);
        ctrl_poll_encoders();
        ota_service();                  /* M-UPGRADE handshake: the installer must see us */
        if (usb.ota_req) {
            usb.ota_req = 0;
            ota_session();              /* commits and resets; on failure returns here */
        }
        ctrl_drain_midi_in();
        ctrl_leds_flush();
        if (fm1_ms - t0 >= 20u) {       /* MASTER pot: <=50 Hz is plenty for a pot */
            t0 = fm1_ms;
            ctrl_poll_master();
        }
    }
}

static void isr_timer5(void) { ctrl_timer5_irq(); }
