/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-ctrl SMOKE build: proves the platform layer on hardware before our glue is
 * allowed near it. Build: tools/build.py --smoke
 *
 * What it exercises, in the order a failure would show up:
 *   1. boot to a screen (lcd_init + text) -- core, clocks, SPI panel
 *   2. the supply rails raised (fm1_vdc14_raise/fm1_sysvdd_raise in fm1_cstart)
 *   3. USB enumeration as a class-compliant MIDI device ("FM-1 ctrl")
 *   4. timed MIDI out: a note every 2 s, cycling F3..G5, plus a CC sweep
 *   5. MIDI in: any message from the host is echoed back (so a host can prove
 *      both directions without our map/keyscan/LED code existing)
 *
 * It uses usb.c/ota.c/lcd.c/gfx.c and the HAL, and NOT keyscan/encoders/leds/
 * map/midi_io/ctrl_glue. If this build behaves, the platform is sound and any
 * later failure is ours; if it does not, nothing of ours is on the unit yet.
 */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_flash.h"
#include "fm1_cpu1.h"
#include "fm1_input.h"

#include "libc.c"
#include "lcd.c"
#include "gfx.c"

static volatile uint32_t fm1_ms;
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

#define FELUCCA_OTA 1             /* must precede usb.c: it gates the SysEx/OTA path */
#include "usb.c"
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_9298"    /* smoke identity: the installer accepts 9xx */
#endif
#include "ota.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return FL_FAR(fl_read_ram)(off, p, n); }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm)
{
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
static void fm1_fault(const fm1_crash_t *c) { (void)c; fm1_reboot(); }

/* plat.h's MIDI contract, straight onto this TU's usb.c statics */
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = midi_in_q[mi_r % MQ];
    mi_r++;
    return 1;
}
void plat_midi_out(uint32_t pkt) { midi_out_event(pkt); }

/* ---- transport: one 3-byte MIDI message -> a USB-MIDI event packet ---- */
static void smoke_send(uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t cin = ((st & 0xF0u) == 0x90u || (st & 0xF0u) == 0x80u) ? 0x9u : 0xBu;
    plat_midi_out((uint32_t)cin | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}

static void isr_timer5(void);
static void smoke_timer5_irq(void)
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
    {                                   /* fm1_ms */
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

static void smoke_main(void);

extern uint8_t _bss_start[], _bss_end[], _pool_start[], _pool_end[];
extern const uint8_t _data_load[], _data_start[], _data_end[];
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
    {   /* the supplies, as x0x does it (fm1-x0x#21): see docs/research/upstream-audit.md */
        uint32_t f = irq_save();
        fm1_vdc14_raise(7);
        fm1_sysvdd_raise(15);
        irq_restore(f);
    }
    for (d = (uint32_t *)_bss_start; d < (uint32_t *)_bss_end; d++)
        *d = 0;
    for (d = (uint32_t *)_pool_start; d < (uint32_t *)_pool_end; d++)
        *d = 0;
    {   /* .data, .ram_text, .c1_text: load -> start */
        const uint8_t *s;
        uint8_t *t;
        for (s = _data_load, d = (uint32_t *)_data_start; (uint8_t *)d < _data_end; s += 4, d++)
            *d = *(const uint32_t *)s;
        for (s = _rt_load, t = (uint8_t *)_rt_start; t < (uint8_t *)_rt_end; s++, t++)
            *t = *s;
        for (s = _c1_load, t = (uint8_t *)_c1_start; t < (uint8_t *)_c1_end; s++, t++)
            *t = *s;
    }
    fm1_irq_enable_all();
    smoke_main();
    for (;;)
        ;
}

static void smoke_main(void)
{
    uint32_t t0, tick = 0;
    uint8_t note = 53;

    lcd_init();
    palette_set(1);
    draw_text_box(0, 60, 240, &FONT_M, "fm1-ctrl", C_HI, 1);
    draw_text_box(0, 96, 240, &FONT_S, "SMOKE BUILD", C_WHITE, 1);
    draw_text_box(0, 120, 240, &FONT_S, "NO PANEL CODE", C_GRAY, 1);
    draw_text_box(0, 144, 240, &FONT_S, "WATCH MIDI OUT", C_GRAY, 1);

    fm1_input_init();
    fm1_mailbox_clear();        /* before lock_top() write-protects 0x01C7FD50.. */
    usb_start();
    fm1_timer5_start(smoke_timer5_irq, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();

    t0 = fm1_ms;
    for (;;) {
        uint32_t pkt;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        /* MIDI in -> echo straight back: proves both directions, no app logic */
        while (plat_midi_in(&pkt)) {
            uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16), d2 = (uint8_t)(pkt >> 24);
            if ((st & 0xF0u) == 0x90u || (st & 0xF0u) == 0x80u ||
                (st & 0xF0u) == 0xB0u)
                smoke_send(st, d1, d2);
        }
        /* OTA: the installer must be able to see and replace this build */
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            ota_session();
        }
        if (fm1_ms - t0 >= 2000u) {
            t0 = fm1_ms;
            /* a note, then its release, cycling the key span; then a CC sweep */
            smoke_send(0x90, note, 100);
            smoke_send(0x80, note, 0);
            smoke_send(0xB0, 20, (uint8_t)(tick % 2 ? 0x41 : 0x3F));
            note = (uint8_t)(note >= 79 ? 53 : note + 1);
            tick++;
            draw_text_box(0, 180, 240, &FONT_S, tick & 1 ? "TICK" : "TACK", C_GRAY, 1);
        }
    }
}
