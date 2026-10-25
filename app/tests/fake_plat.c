/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller
 *
 * The fake platform's implementation. Deliberately dull: counters, a queue, no
 * behaviour that could make a failing app look right.
 */
#include <string.h>
#include "fake_plat.h"

fake_panel_t fp;

const felucca_font_t FONT_XS = {8}, FONT_B = {16};

sent_t sent[SENT_MAX];
int sent_n;

/* the input queue: what the host would send us */
#define IN_MAX 32
static struct { uint32_t st, d1, d2; int port; } inq[IN_MAX];
static int inq_n, inq_i;

static int touch_is_on;
static uint32_t blits;

void sent_clear(void) { sent_n = 0; }

void fake_midi_in(uint32_t st, uint32_t d1, uint32_t d2, int port)
{
    if (inq_n < IN_MAX) {
        inq[inq_n].st = st;
        inq[inq_n].d1 = d1;
        inq[inq_n].d2 = d2;
        inq[inq_n].port = port;
        inq_n++;
    }
}

void fake_reset(void)
{
    memset(&fp, 0, sizeof fp);
    sent_n = 0;
    inq_n = inq_i = 0;
    touch_is_on = 0;
    blits = 0;
}

/* ---- device/gfx.c's API, without a screen ---- */
void cv_begin(uint32_t w, uint32_t h, uint16_t bg) { (void)w; (void)h; (void)bg; }
int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    (void)y; (void)f; (void)c;
    /* the app measures with text_w and lays out from its return: keep it honest */
    return x + (int32_t)strlen(s) * 6;
}
void cv_blit(uint32_t x, uint32_t y) { (void)x; (void)y; blits++; }
void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    (void)x; (void)y; (void)w; (void)c;
    fp.rect_n++;
    if (h > fp.rect_h_max)
        fp.rect_h_max = h;
}
void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)c;
    fp.line_n++;
}
int32_t text_w(const felucca_font_t *f, const char *s) { (void)f; return (int32_t)strlen(s) * 6; }
void lcd_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }

/* ---- lib/touch.h ---- */
void touch_on(int on) { touch_is_on = on ? 1 : 0; }
void touch_poll(void)
{
    uint32_t i;
    for (i = 0; i < NKEYS && fp.poll_raise; i++)
        if ((fp.keys >> i & 1u) && fp.press[i] + (uint32_t)fp.poll_raise <= 127u)
            fp.press[i] = (uint8_t)(fp.press[i] + fp.poll_raise);
}
uint32_t touch_vel(uint32_t key) { return key < NKEYS ? fp.vel[key] : 0; }
uint8_t touch_pressure(uint32_t key) { return key < NKEYS ? fp.press[key] : 0; }
int touch_down(uint32_t key) { return key < NKEYS && ((fp.down >> key) & 1u); }

/* ---- plat.h ---- */
uint32_t plat_ms(void) { return fp.ms; }
uint32_t plat_buttons(void) { return fp.buttons; }
uint32_t plat_keys(void) { return fp.keys & ((1u << NKEYS) - 1u); }

int32_t plat_enc(int role)
{
    int32_t d;
    if (role < 0 || role >= NE)
        return 0;
    d = fp.enc[role];           /* detents since the last call: report, then forget */
    fp.enc[role] = 0;
    return d;
}

uint32_t plat_analog_mask(void) { return fp.analog; }

uint32_t plat_master(void)
{
    if (fp.seq_n > 0) {              /* the pot jittering inside one frame */
        uint32_t v = fp.seq[fp.seq_i % fp.seq_n];
        fp.seq_i++;
        return v;
    }
    return fp.master;
}
int plat_battery(void) { return 4; }

void plat_lights(uint32_t b, uint32_t k, uint32_t db, uint32_t dk, int glow)
{ (void)db; (void)dk; fp.lights_b = b; fp.lights_k = k; fp.glow = glow; fp.lights_n++; }
void plat_lights_mid(uint32_t b, uint32_t k) { (void)b; (void)k; }
void plat_leds(uint32_t b, uint32_t k) { fp.led_b = b; fp.led_k = k; fp.led_n++; }
void plat_glow(int on) { fp.glow = on; }
void plat_play_red(int r) { fp.play_red = r; }

int plat_midi_in(uint32_t *pkt, int *port)
{
    uint32_t st, d1, d2;
    int p;
    if (inq_i >= inq_n)
        return 0;
    st = inq[inq_i].st; d1 = inq[inq_i].d1; d2 = inq[inq_i].d2; p = inq[inq_i].port;
    inq_i++;
    *pkt = 0x03u | st << 8 | d1 << 16 | d2 << 24;
    if (port) *port = p;
    return 1;
}
uint32_t plat_midi_in_from(void) { return TO_USB; }

void plat_midi_send(uint32_t pkt, uint32_t to)
{
    if (sent_n < SENT_MAX) {
        sent[sent_n].st = (pkt >> 8) & 0xFFu;
        sent[sent_n].d1 = (pkt >> 16) & 0x7Fu;
        sent[sent_n].d2 = (pkt >> 24) & 0x7Fu;
        sent[sent_n].to = to;
        sent_n++;
    }
}
void plat_midi_out(uint32_t pkt) { plat_midi_send(pkt, TO_USB); }
uint32_t plat_midi_out_room(uint32_t to) { (void)to; return 64; }
uint32_t plat_midi_out_link(void) { return 1; }
uint32_t plat_midi_count(int port, int out) { (void)port; (void)out; return (uint32_t)sent_n; }
uint32_t plat_midi_ago(int port, int out) { (void)port; (void)out; return 0; }

int plat_ble_state(void) { return fp.ble_connected ? BLE_READY : BLE_STARTING; }
int plat_ble_connected(void) { return fp.ble_connected; }
int plat_ble_linked(void) { return fp.ble_connected; }
void plat_ble_start(void) { }
uint32_t plat_ble_log_total(void) { return 0; }
const char *plat_ble_log_line(int back) { (void)back; return 0; }
void plat_ble_scan(int on) { (void)on; }
int plat_ble_found(int i, char *name, int *rssi) { (void)i; (void)name; (void)rssi; return 0; }
int plat_ble_connect(int i) { (void)i; return 0; }
void plat_ble_disconnect(void) { }
int plat_ble_device(void) { return BLE_DEV_OFF; }
void plat_ble_enable(int on) { (void)on; }

int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    (void)obj; (void)dst; (void)max;
    return -1;                   /* no settings: the app takes its defaults */
}
int plat_store_head(uint32_t obj, void *dst, uint32_t n) { (void)obj; (void)dst; (void)n; return -1; }
int plat_store_size(uint32_t obj) { (void)obj; return -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { (void)obj; (void)src; (void)len; return 0; }

uint32_t plat_lock(void) { return 0; }
void plat_unlock(uint32_t f) { (void)f; }
void plat_yield(void) { }
void plat_log(const char *line) { (void)line; }
uint32_t plat_cpu_pct(void) { return 0; }
uint32_t plat_xruns(void) { return 0; }
uint32_t plat_ticks24(void) { return 0; }
void plat_usb_audio_full(int on) { (void)on; }
uint32_t blits_seen(void) { return blits; }
