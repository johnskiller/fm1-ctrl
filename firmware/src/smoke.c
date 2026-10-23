/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-ctrl SMOKE build: proves the platform layer on hardware before our glue is
 * allowed near it. Build: tools/build.py --smoke
 *
 * The screen is the point of this build. It shows every state the USB bring-up can
 * get stuck in, live, so a photo of the panel says which layer failed:
 *
 *   BOOT n          main loop entered (n increments every 500 ms) -- if this is
 *                   stuck at 0 the main loop never ran
 *   IRQ  n          TIMER5 ticks counted in the ISR -- 0 means the ISR is not
 *                   firing or returns nowhere (the first flash's bug)
 *   UP/ON/CFG       usb.up (SIE alive), usb.config (host set our config: we are
 *                   ENUMERATED), and the address the host gave us
 *   SOF/FR          SOFs seen / frame number advancing: a real host is talking
 *   SETUP/RESET     control requests served / bus resets
 *   E0ERR           timeouts on EP0: a descriptor the host rejected
 *   TX/RX           MIDI packets out / in
 *   MS nn           fm1_ms -- proves TIMER4 and the ms conversion
 *
 * What it exercises, in the order a failure would show up:
 *   1. boot to a screen -- core, clocks, SPI panel
 *   2. the supply rails raised (fm1_vdc14_raise/fm1_sysvdd_raise)
 *   3. TIMER5 at 2 kHz, USB polled from its ISR
 *   4. enumeration as a class-compliant MIDI device ("FM-1 ctrl")
 *   5. timed MIDI out: a note every 2 s, cycling F3..G5, plus a CC sweep
 *   6. MIDI in: anything the host sends is echoed back
 *
 * It uses usb.c/ota.c/lcd.c/gfx.c and the HAL, and NOT keyscan/encoders/leds/
 * map/midi_io/ctrl_glue. If this build behaves, the platform is sound.
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
static void fm1_fault(const fm1_crash_t *c) { (void)c; }

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

/* ------------------------------------------------------------- ISR --- */
/* The asm entry fm1_isr.S (FM1_ISR isr_timer5, fm1_timer5_irq) saves psr/rets/reti
 * and ends in csync/rti. The C body MUST be named fm1_timer5_irq and the wrapper
 * MUST be what fm1_timer5_start() gets; otherwise the wrapper is unreferenced,
 * --gc-sections drops it, and a tick jumps into C code that cannot return. */
extern void isr_timer5(void);
volatile uint32_t dbg_irq, dbg_boot;

void fm1_timer5_irq(void)
{
    static uint32_t sub;
    fm1_timer5_ack();
    dbg_irq++;
    fm1_input_tick();
    if (sub % 5u == 0u)
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
extern uint32_t _rt_start[], _rt_end[];
extern uint8_t _c1_start[], _c1_end[];

void fm1_cstart(void)
{
    uint32_t *d;
    fm1_cpu1_halt();
    fm1_time_init();
    fm1_reset_reason();
    fm1_wdt_arm(0x0D);
    fm1_irq_init();
    {   /* the supplies, as x0x does it (fm1-x0x#21): docs/research/upstream-audit.md */
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

/* ---------------------------------------------------------- the screen --- */
static void hex4(char *b, uint32_t v)          /* 4 hex digits, no prefix */
{
    static const char d[] = "0123456789ABCDEF";
    int i;
    b[4] = 0;
    for (i = 3; i >= 0; i--) {
        b[i] = d[v & 0xFu];
        v >>= 4;
    }
}

static void line(uint32_t y, const char *label, uint32_t v)
{
    char b[24];
    uint32_t i = 0;
    while (*label && i < 12)
        b[i++] = *label++;
    if (v > 9999u) {
        hex4(b + i, v);
        i += 4;
    } else {
        char t[8];
        int n = 0;
        if (!v) {
            t[n++] = '0';
        } else {
            while (v) {
                t[n++] = (char)('0' + (v % 10u));
                v /= 10u;
            }
        }
        while (n--)
            b[i++] = t[n];
    }
    b[i] = 0;
    draw_text_box(6, y, 228, &FONT_S, b, C_WHITE, 0);
}

static void smoke_main(void)
{
    uint32_t t_tick, t_draw, note = 53, sweep = 0;

    lcd_init();
    palette_set(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 6, 240, &FONT_M, "fm1-ctrl", C_HI, 1);
    draw_text_box(0, 34, 240, &FONT_S, "SMOKE 9298", C_GRAY, 1);

    fm1_input_init();
    fm1_mailbox_clear();        /* before lock_top() write-protects 0x01C7FD50.. */
    usb_start();
    fm1_timer5_start(isr_timer5, 4);    /* the asm wrapper */
    fm1_guard_lock_top();
    fm1_irq_enable_all();

    t_tick = fm1_ms;
    t_draw = fm1_ms;
    for (;;) {
        uint32_t pkt;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        dbg_boot++;
        /* MIDI in -> echo straight back: proves both directions, no app logic */
        while (plat_midi_in(&pkt)) {
            uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16), d2 = (uint8_t)(pkt >> 24);
            if ((st & 0xF0u) == 0x90u || (st & 0xF0u) == 0x80u || (st & 0xF0u) == 0xB0u)
                smoke_send(st, d1, d2);
        }
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            ota_session();
        }
        if (fm1_ms - t_tick >= 2000u) {         /* the timed event */
            t_tick = fm1_ms;
            smoke_send(0x90, (uint8_t)note, 100);
            smoke_send(0x80, (uint8_t)note, 0);
            smoke_send(0xB0, 20, (uint8_t)(sweep++ & 1u ? 0x41u : 0x3Fu));
            note = note >= 79u ? 53u : note + 1u;
        }
        if (fm1_ms - t_draw >= 250u) {          /* the live state */
            t_draw = fm1_ms;
            {   /* heartbeat: the dot moves only if the main loop runs */
                static uint8_t ph;
                lcd_fill(200, 34, 8, 8, C_BLACK);
                lcd_fill(200 + (ph ? 0 : 4), 34, 4, 4, C_HI);
                ph ^= 1;
            }
            line(56, "BOOT ", dbg_boot / 4u);           /* /4: 1 per second */
            line(74, "IRQ  ", dbg_irq / 1000u);         /* /1000: kticks */
            line(92, "UP.CFG ", (uint32_t)usb.up * 10u + usb.config);
            line(110, "ADDR ", usb.has_pend_addr ? usb.pend_addr : usb.config ? 1u : 0u);
            line(128, "SOF  ", usb.sof_seen);
            line(146, "SETUP", usb.setups);
            line(164, "RESET", usb.resets);
            line(182, "E0ERR", usb.timeouts);
            line(200, "TX   ", usb.tx_pkts);
            line(218, "RX   ", usb.rx_pkts);
            {   /* ms since boot: proves TIMER4 and the ms conversion */
                char t[16];
                uint32_t v = fm1_ms / 1000u, i = 0, n = 0;
                char s[8];
                t[i++] = 'T'; t[i++] = ' ';
                if (!v) s[n++] = '0';
                while (v) { s[n++] = (char)('0' + v % 10u); v /= 10u; }
                while (n) t[i++] = s[--n];
                t[i++] = 's'; t[i] = 0;
                draw_text_box(6, 236, 228, &FONT_S, t, C_GRAY, 0);
            }
        }
    }
}
