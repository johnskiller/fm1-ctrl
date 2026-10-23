/* fm1-ctrl on-device glue: TIMER5 ISR, MIDI transport thunks, boot + main loop.
 * Included last by ctrl.c (unity root) — sees the platform's statics
 * (fm1_ms, usb_*, panel) as file-local symbols, like x0x's main_fm1.c.
 * App layer: Copyright (C) 2026 johnskiller, GPL-3.0-only.
 */
#include "midi_io.h"      /* our transport hook + midi_msg3 (from -Ifirmware) */
#include "keyscan.h"
#include "leds.h"
#include "ctrl_glue.h"
#include "map.h"
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

/* ---- map SysEx: F0 7D 43 54 .. F7 changes the runtime map (no flash: a power
 * cycle returns to the compiled-in default). Handled here, before ota_service(),
 * because ota_take() consumes any frame it is handed. A GET is answered with the
 * current map so a host page can verify what the unit is sending. ---- */
static uint8_t sx_reply[64];
static void ctrl_service_map_sysex(void)
{
    const uint8_t *p;
    uint32_t n, k;
    int rc;
    if (!ota_frame_get(&p, &n))
        return;
    if (!map_sysex_is_ours(p, n))
        return;                          /* ota.c's business */
    rc = map_sysex(p, n);
    ota_frame_done();
    if (rc != 1)
        return;
    if (n >= 4u && p[3] == 0x02) {       /* GET -> answer */
        k = map_sysex_build(sx_reply + 1);
        sx_reply[0] = 0xF0;
        sx_reply[1 + k] = 0xF7;
        ota_wire_send(sx_reply, k + 2u);
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
/* on-screen diagnostic counters (ctrl_diag_draw renders them): declared up here
 * because the pollers below increment them */
static uint32_t dbg_boot, dbg_scan, dbg_enc, dbg_key, dbg_btn;
volatile uint32_t dbg_irq;

static void ctrl_poll_encoders(void)
{
    uint32_t r;
    for (r = 0; r < NE; r++) {
        int e = (int)panel_matrix_enc(r);
        int32_t st;
        if (e < 0 || (uint32_t)e >= FM1_NENC)
            continue;                /* a corrupt calibration must not index enc_steps[] */
        st = fm1_enc_take((uint32_t)e);
        if (st) {
            encoders_step((int)r, (int)st * (int)panel_dir_enc(r), 0);
            dbg_enc++;
        }
    }
}

/* MASTER pot (PB6, SARADC ch4 -- NOT a hardware volume path: every FM-1 firmware
 * multiplies the mix by it in software; x0x reads it and does out *= vol in
 * engine.c, and nothing in fm1_audio.h writes a codec volume register).
 *
 * Sent two ways, both on channel 1:
 *   CC 27        adc >> 3   -- what the Felucca/Melodee web emus call MASTER
 *                             (web/emu/worklet.js: MASTER_CC = 27)
 *   CC 7 / 39    14-bit     -- RemoteFM1 PROTOCOL section 4, for DAWs
 * The value is the raw ADC, not a curve: the receiving firmware applies its own
 * (x0x: master_q12 = (adc>>3)^2), so squaring here would square it twice. */
static void ctrl_poll_master(void)
{
    static int32_t last = -1;
    int32_t adc = 0, v;
    uint32_t i;
    /* average several conversions: the pot's noise is +-2 counts and every single
     * one of them used to reach the wire (see ctrl_glue.h, CTRL_MASTER_*) */
    for (i = 0; i < CTRL_MASTER_OVERSAMPLE; i++) {
        v = fm1_adc_read(FM1_ADC_MASTER);
        if (v < 0)
            break;
        adc += v;
    }
    if (i == 0)
        return;                          /* no good conversion at all */
    adc /= (int32_t)i;
    if (!ctrl_master_accept(adc, &last))
        return;
    midi_emit3(0xB0, 27, (uint8_t)(adc >> 3));
    {
        uint32_t v14 = (uint32_t)adc << 4;
        midi_emit3(0xB0, 7,  (uint8_t)(v14 >> 7));
        midi_emit3(0xB0, 39, (uint8_t)(v14 & 0x7F));
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

extern void isr_timer5(void);        /* fm1_isr.S: FM1_ISR isr_timer5, fm1_timer5_irq */

/* ---- TIMER5 ISR: x0x's, minus audio/UAC ----
 * The name is load bearing: fm1_isr.S has `FM1_ISR isr_timer5, fm1_timer5_irq`, the
 * asm entry that saves psr/rets/reti and ends in csync/rti. fm1_timer5_start() must
 * be given isr_timer5 (the wrapper), and this body must be fm1_timer5_irq, or the
 * wrapper is unreferenced, --gc-sections drops it, and a timer tick jumps into a C
 * function that cannot return. That is what kept the first smoke build from ever
 * reaching its main loop. */
void fm1_timer5_irq(void)
{
    static uint32_t sub;
    uint8_t usb_due;
    fm1_timer5_ack();
    dbg_irq++;
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
/* .ram_text / .c1_text: code that must execute from RAM (the flash driver runs
 * while the flash is busy) -- linked in RAMTEXT at 0x01C00000, stored in the
 * image, copied down here. x0x's fm1_cstart does the same; without it the flash
 * driver, and with it the whole update path, runs from uninitialised RAM. */
extern const uint8_t _rt_load[], _c1_load[];
extern uint32_t _rt_start[], _rt_end[];        /* fm1_guard.h's declaration */
extern uint8_t _c1_start[], _c1_end[];


void fm1_cstart(void)
{
    uint32_t *d;
    fm1_cpu1_halt();
    fm1_time_init();
    fm1_reset_reason();
    fm1_wdt_arm(0x0D);
    fm1_irq_init();
    {   /* supplies to M-VAVE's own levels before anything loads them: the boot
         * loader leaves VDC14 3 / SYSVDD 11 (1.40 / 1.26 V), the stock firmware
         * runs 7 / 15 (1.60 / 1.38 V). x0x does this at the top of fm1_main with
         * its reason spelled out: under load some units read flash and SRAM
         * wrong at the lower levels (fm1-x0x#21, fm1_sys.h). Our flash driver
         * and USB both run under load. */
        uint32_t f = irq_save();        /* fm1_flash.h's, as x0x uses */
        fm1_vdc14_raise(7);
        fm1_sysvdd_raise(15);
        irq_restore(f);
    }
    for (d = (uint32_t *)_bss_start; d < (uint32_t *)_bss_end; d++)
        *d = 0;
    for (d = (uint32_t *)_pool_start; d < (uint32_t *)_pool_end; d++)
        *d = 0;
    {                                   /* .data: load -> start (x0x's word copy) */
        const uint8_t *s;
        for (s = _data_load, d = (uint32_t *)_data_start; (uint8_t *)d < _data_end; s += 4, d++)
            *d = *(const uint32_t *)s;
    }
    {   /* .ram_text + .c1_text -> RAMTEXT: REQUIRED, the flash driver lives there */
        const uint8_t *s;
        uint8_t *t;
        for (s = _rt_load, t = (uint8_t *)_rt_start; t < (uint8_t *)_rt_end; s++, t++)
            *t = *s;                       /* word-aligned: the linker guarantees it */
        for (s = _c1_load, t = (uint8_t *)_c1_start; t < (uint8_t *)_c1_end; s++, t++)
            *t = *s;
    }
    fm1_irq_enable_all();
    ctrl_main();
    for (;;)
        ;
}


/* ---- on-screen diagnostic (hold SAVE to see it) ------------------------
 * The first smoke build proved how expensive a blank screen is: the unit sat
 * on its splash with no main loop and told nobody. This page is the same idea
 * for the full firmware -- every layer that can be dead in silence has a line:
 *
 *   BOOT   main-loop iterations / 4          (0 = the loop never ran)
 *   IRQ    TIMER5 ticks / 1000               (0 = the ISR is not running)
 *   UP.CFG usb.up, then usb.config           (11 = the host enumerated us)
 *   SCAN   keyscan_poll frames               (0 = the panel matrix is silent)
 *   KEY    last raw key mask, hex            (press a key: this must change)
 *   BTN    last raw button mask, hex
 *   ENC    encoder detents taken
 *   TX     USB-MIDI packets out              (press a key: +2 note on/off)
 *   RX     packets in from the host          (host lights: must climb)
 *   LED    button+key LEDs currently lit
 *   MX     map checksum-ish: the 7 encoder CCs, hex
 */

static void dhex(char *b, uint32_t v, int digits)
{
    static const char d[] = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) {
        b[i] = d[v & 0xFu];
        v >>= 4;
    }
    b[digits] = 0;
}

static void dnum(char *b, uint32_t v)
{
    char t[12];
    int n = 0, i = 0;
    if (!v)
        t[n++] = '0';
    while (v) {
        t[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n)
        b[i++] = t[--n];
    b[i] = 0;
}

static void dline(uint32_t y, const char *label, const char *val, uint16_t c)
{
    char b[28];
    uint32_t i = 0;
    while (*label && i < 14u)
        b[i++] = *label++;
    while (*val && i < 27u)
        b[i++] = *val++;
    b[i] = 0;
    draw_text_box(4, y, 232, &FONT_S, b, c, 0);
}

static void ctrl_diag_draw(void)
{
    char v[12];
    uint32_t lit = 0, i;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 4, 240, &FONT_S, "fm1-ctrl DIAG", C_HI, 1);
    dnum(v, dbg_boot / 4u);          dline(24,  "BOOT ", v, C_WHITE);
    dnum(v, dbg_irq / 1000u);        dline(40,  "IRQ  ", v, C_WHITE);
    { char t[8]; t[0] = (char)('0' + (usb.up ? 1 : 0)); t[1] = (char)('0' + (usb.config ? 1 : 0)); t[2] = 0;
      dline(56, "UP.CFG ", t, usb.config ? C_HI : C_WHITE); }
    dnum(v, dbg_scan);               dline(72,  "SCAN ", v, C_WHITE);
    dhex(v, dbg_key, 7);             dline(88,  "KEY  ", v, C_WHITE);
    dhex(v, dbg_btn, 4);             dline(104, "BTN  ", v, C_WHITE);
    dnum(v, dbg_enc);                dline(120, "ENC  ", v, C_WHITE);
    dnum(v, ctrl_dbg_tx);            dline(136, "TX   ", v, C_HI);
    dnum(v, ctrl_dbg_rx);            dline(152, "RX   ", v, C_HI);
    for (i = 0; i < 27u; i++)
        lit += (uint32_t)leds_key_get(NOTE_KEY0 + (int)i);
    for (i = 0; i < 14u; i++)
        lit += (uint32_t)leds_btn_get((int)i);
    dnum(v, lit);                    dline(168, "LED  ", v, C_WHITE);
    { /* the runtime map: 7 encoder CCs, the fastest way to see a SysEx SET landed */
      char t[24];
      for (i = 0; i < 7u; i++)
          dhex(t + i * 3, map_encoder_cc((int)i), 2), t[i * 3 + 2] = ' ';
      t[21] = 0;
      dline(184, "MAP  ", t, C_GRAY);
    }
    {   /* uptime: the page only redraws if the loop is alive */
        char u[10], t[12];
        uint32_t k = 0;
        const char *p = "UP ";
        while (*p)
            t[k++] = *p++;
        dnum(u, fm1_ms / 1000u);
        for (i = 0; u[i]; i++)
            t[k++] = u[i];
        t[k++] = 's';
        t[k++] = ' ';
        t[k++] = ' ';
        t[k] = 0;
        dline(204, "", t, C_HI);
    }
    dline(222, "SAVE = BACK", "", C_GRAY);
}

/* boot-stage beacon: fm1_fault() prints it, so a crash during bring-up names the
 * exact call instead of leaving a garbage PC to argue about. */
volatile uint32_t ctrl_stage;
#define STAGE(n) do { ctrl_stage = (n); } while (0)

/* ---- main loop ---- */
static void ctrl_main(void)
{
    uint32_t t0;
    STAGE(1);
    lcd_init();
    STAGE(2);
    palette_set(1);
    draw_text_box(0, 96, 240, &FONT_M, "fm1-ctrl", C_HI, 1);
    draw_text_box(0, 134, 240, &FONT_S, "EVERY CONTROL EMITS", C_GRAY, 1);
    STAGE(3);
    fm1_input_init();
    STAGE(4);
    panel_init();
    STAGE(5);
    fm1_mailbox_clear();        /* stale update/boot state from a previous firmware: clear
                                 * it BEFORE lock_top() write-protects 0x01C7FD50.. */
    midi_transport_send = midi_transport_usb3;
    STAGE(6);
    fm1_adc_init();
    STAGE(7);
    ctrl_init();                /* module init first, then wire the hooks into it */
    STAGE(8);
    keyscan_btn_map(btn_matrix_to_label);
    STAGE(9);
    usb_start();
    STAGE(10);
    STAGE(11);                          /* next: the ISR starts firing */
    fm1_timer5_start(isr_timer5, 4);   /* the asm wrapper, not a C function */
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    STAGE(12);
    fm1_delay_ms(300);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_M, "fm1-ctrl", C_HI, 1);

    STAGE(13);                          /* main loop reached */
    t0 = fm1_ms;
    for (;;) {
        uint64_t raw = (uint64_t)fm1_in.notes | (uint64_t)fm1_in.buttons << 27;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        dbg_boot++;
        dbg_key = fm1_in.notes;
        dbg_btn = fm1_in.buttons;
        ctrl_frame_scan(raw);
        dbg_scan++;
        ctrl_poll_encoders();
        ctrl_service_map_sysex();       /* runtime map over SysEx (before ota_take) */
        ota_service();                  /* M-UPGRADE handshake: the installer must see us */
        if (usb.ota_req) {
            usb.ota_req = 0;
            ota_session();              /* commits and resets; on failure returns here */
        }
        ctrl_drain_midi_in();
        ctrl_leds_flush();
        if (fm1_ms - t0 >= 20u) {       /* <=50 Hz is plenty for a pot and the diag page */
            static uint32_t t_diag;
            t0 = fm1_ms;
            ctrl_poll_master();
            if (fm1_in.buttons & (1u << panel_matrix_btn(B_SAVE))) {   /* matrix id, not label */
                if (fm1_ms - t_diag >= 200u) {
                    t_diag = fm1_ms;
                    ctrl_diag_draw();
                }
            } else if (t_diag) {
                t_diag = 0;
                lcd_fill(0, 0, 240, 240, C_BLACK);
                draw_text_box(0, 110, 240, &FONT_M, "fm1-ctrl", C_HI, 1);
            }
        }
    }
}

