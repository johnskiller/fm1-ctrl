/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-ctrl: the thin transparent controller firmware for the M-VAVE FM-1.
 * One compilation unit, Felucca's layout (x0x.c is the template):
 * platform sources are #included in a strict order; our app is the thin
 * glue in ctrl_glue.c + the modules it drives (host-tested, see
 * docs/research/usb-midi.md §6).
 *
 * fm1-ctrl difference vs x0x: no DSP, no sequencer, no storage — every
 * key/button/encoder action goes out raw as USB MIDI; host MIDI-in drives
 * the LEDs. App layer Copyright (C) 2026 johnskiller.
 */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_flash.h"
#include "fm1_perf.h"

#include "libc.c"
#include "lcd.c"
#include "gfx.c"

static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4, TIMER5 ISR) */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

#define FELUCCA_OTA 1             /* M-UPGRADE entry: keeps the web installer working.
                                   * MUST precede usb.c: it gates the SysEx/OTA path. */
#include "usb.c"                  /* FELUCCA_UAC=0 FELUCCA_CDC=0: pure MIDI */
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_9299"    /* fm1-ctrl dev identity (sloop _9xxxx, x0x _9, CTL-1 its own) */
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
    return fl_write(off, p, n);   /* ROM-callable wrapper */
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return FL_FAR(fl_read_ram)(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u) {
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_HI, 1);
        return;
    }
    draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_HI, 1);
    fm1_delay_ms(1500);
}
static void ota_commit(const uint8_t *parm)
{
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
/* fm1_irq.h requires the app to supply the fault reporter */
static void fm1_fault(const fm1_crash_t *c)
{
    char b[9];
    uint32_t i;
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "fm1-ctrl CRASH", C_WHITE, 1);
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(c->pc >> (28u - 4u * i)) & 15u];
    b[8] = 0;
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    { uint32_t t0 = fm1_ticks();
      while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US) ; }
    fm1_reboot();
}

#include "plat_ctrl.h"            /* panel enums + store stubs (see header note) */
#include "panel.c"                /* physical panel map (calibration table) */
#include "plat_ctrl.c"            /* midi ring accessors (usb.c statics above) */
#include "../keyscan.c"           /* our host-tested modules, into the unity root */
#include "../encoders.c"
#include "../leds.c"
#include "../midi_io.c"
#include "../map.c"
#include "../map_data.c"
#include "ctrl_glue.c"
#include "ctrl_main.c"            /* boot, TIMER5 ISR, main loop */
