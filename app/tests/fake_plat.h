/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller
 *
 * A fake platform: enough of plat.h, gfx and touch to run app/app.c on a
 * computer. The whole point is that the app under test is the real app.c --
 * not a copy of its logic -- so a test that passes here is a claim about the
 * firmware that ships.
 */
#pragma once
#include <stdint.h>
#include "plat.h"

/* ---- what device/gfx.c would give us (only what app.c names) ---- */
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
#define C_LINE  0x2222u
#define C_DIM   0x4A4Au
#define C_GRAY  0x9696u
#define C_HI    0x07E4u

typedef struct { int h; } felucca_font_t;
extern const felucca_font_t FONT_XS, FONT_B;

void cv_begin(uint32_t w, uint32_t h, uint16_t bg);
int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c);
void cv_blit(uint32_t x, uint32_t y);
int32_t text_w(const felucca_font_t *f, const char *s);
void lcd_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);

/* ---- lib/touch.h: the keys' velocity and pressure ---- */
void touch_on(int on);
void touch_poll(void);
uint32_t touch_vel(uint32_t key);

/* ---- the fake panel and the capture ---- */
typedef struct {
    uint32_t keys;          /* held, bit k */
    uint32_t buttons;       /* held, bit B_* */
    int32_t  enc[NE];       /* detents to report on the next plat_enc call */
    uint32_t master;        /* 0..4096 */
    uint32_t ms;
    uint32_t vel[NKEYS];    /* what touch_vel returns (0: the app's default) */
    int      ble_connected;

    /* When set, plat_master returns these in turn instead of `master`: the pot
     * jittering *within* one frame, which is what the app's oversampling is
     * there to average out. */
    uint32_t seq[16];
    int      seq_n, seq_i;
} fake_panel_t;

extern fake_panel_t fp;

/* a captured MIDI packet: status, data 1, data 2, and where it was sent */
typedef struct { uint32_t st, d1, d2, to; } sent_t;
#define SENT_MAX 256
extern sent_t sent[SENT_MAX];
extern int sent_n;
void sent_clear(void);

/* queue a message for plat_midi_in to hand to the app */
void fake_midi_in(uint32_t st, uint32_t d1, uint32_t d2, int port);

/* the fake platform's reset, between test cases */
void fake_reset(void);
