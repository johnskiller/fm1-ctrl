/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-ctrl in the browser: the firmware's own modules (keyscan, encoders, leds,
 * map, midi_io, ctrl_glue) compiled to WebAssembly, after Felucca's web/emu and
 * melodee's web/emu/melodee_web.c (Hugelton Instruments / keremimo, GPL-3.0).
 * No USB, no screen, no audio: the page feeds key/button/encoder edges in, drains
 * the emitted MIDI bytes, and feeds WebMIDI-in back the way usb.c feeds midi_recv3.
 * Nothing of the firmware changes.
 *   web/emu/build.sh -> build/emu/{fm1ctrl.wasm, index.html} */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define EXPORT __attribute__((used, visibility("default")))

/* firmware API (defined by the includes below) */
#include "keyscan.h"     /* NKEYS NBUTTONS */
#include "encoders.h"    /* NENC */
#define NOTE_KEY0 53     /* key idx 0 = F3 (mirrors keyscan.c's private define) */
uint8_t leds_grid_get(int cc);
uint8_t leds_key_get(int note);
void midi_recv3(uint8_t st, uint8_t d1, uint8_t d2);
void midi_transport_send_fn(uint8_t st, uint8_t d1, uint8_t d2);

static uint32_t fm1_ms;
EXPORT uint32_t web_now_ms(void) { return fm1_ms; }

/* ---- input state, by id: buttons bit b = label b (0..13); keys bit k = 0..26 ---- */
static uint64_t web_key_mask, web_btn_mask;
EXPORT void web_key(uint32_t k, int down)
{
    if (k >= NKEYS) return;
    uint64_t bit = (uint64_t)1 << k;
    if (down) web_key_mask |= bit; else web_key_mask &= ~bit;
}
EXPORT void web_button(uint32_t b, int down)
{
    if (b >= NBUTTONS) return;
    uint64_t bit = (uint64_t)1 << (NKEYS + b);
    if (down) web_btn_mask |= bit; else web_btn_mask &= ~bit;
}
EXPORT uint64_t web_scan_mask(void) { return web_key_mask | web_btn_mask; }

/* ---- encoders: signed steps by role 0..6; each step becomes 4 quadrature
 * transitions spread over 4 ticks, exactly what a real detent produces ---- */
enum { FM1_NENC = 7 };
static int32_t web_enc_steps[FM1_NENC];
static uint8_t web_enc_q[FM1_NENC];       /* current gray state */
static const uint8_t NEXT_CW[4]  = {1, 3, 0, 2};   /* 00->01->11->10->00 */
static const uint8_t NEXT_CCW[4] = {2, 0, 3, 1};
EXPORT void web_enc(uint32_t role, int32_t steps)
{
    if (role < FM1_NENC)
        web_enc_steps[role] += steps * 4;   /* 1 detent = 4 quadrature transitions */
}
/* one pending quadrature transition per tick, per encoder */
static void enc_quadrature_tick(uint16_t *ab, uint8_t *push)
{
    int i;
    for (i = 0; i < NENC; i++) {
        if (web_enc_steps[i] == 0) continue;
        int dir = web_enc_steps[i] > 0 ? +1 : -1;
        uint8_t q = dir > 0 ? NEXT_CW[web_enc_q[i]] : NEXT_CCW[web_enc_q[i]];
        web_enc_q[i] = q;
        *ab |= (uint16_t)q << (i * 2);
        web_enc_steps[i] -= dir;
    }
}

/* ---- the emitted bytes (device -> host): ring the page drains ---- */
#define OUTQ 256u
static uint8_t out_q[OUTQ][3];
static uint32_t out_w, out_r;
static void q_out(uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t i = out_w % OUTQ;
    out_q[i][0] = st; out_q[i][1] = d1; out_q[i][2] = d2;
    out_w++;
}
EXPORT uint32_t web_out_len(void) { return out_w - out_r; }
/* one message, packed st|d1<<8|d2<<16; -1 when empty (no pointer marshalling) */
EXPORT int32_t web_out_get(void)
{
    if (out_w == out_r) return -1;
    const uint8_t *m = out_q[out_r % OUTQ];
    out_r++;
    return (int32_t)m[0] | ((int32_t)m[1] << 8) | ((int32_t)m[2] << 16);
}

/* ---- MIDI-in: the page hands bytes over; the firmware routes them ---- */
EXPORT void web_midi_in(uint8_t st, uint8_t d1, uint8_t d2) { midi_recv3(st, d1, d2); }

/* ---- the LEDs, as midi_io routes them ---- */
static uint8_t  web_grid[16];
static uint32_t web_keyled;
/* live readback from leds.c (the latched copies stay for the tick path) */
EXPORT uint32_t web_grid_row(uint32_t r) { return r < 16u ? leds_grid_get((int)r) : 0; }
EXPORT uint32_t web_btn_leds(void)
{
    extern uint8_t leds_btn_get(int i);
    uint32_t k, m = 0;
    for (k = 0; k < NBUTTONS; k++)
        m |= (uint32_t)leds_btn_get((int)k) << k;
    return m;
}

EXPORT uint32_t web_lit_keys(void)
{
    uint32_t k, m = 0;
    for (k = 0; k < NKEYS; k++)
        m |= (uint32_t)leds_key_get(NOTE_KEY0 + (int)k) << k;
    return m;
}

/* ---- the firmware itself, host-test shape ---- */
static void transport_hook(uint8_t st, uint8_t d1, uint8_t d2) { q_out(st, d1, d2); }

#include "../../firmware/keyscan.c"
#include "../../firmware/encoders.c"
#include "../../firmware/leds.c"
#include "../../firmware/midi_io.c"
#include "../../firmware/map.c"
#include "../../firmware/map_data.c"
#include "../../firmware/ctrl_glue.c"

/* ---- one tick of the device: 2 keyscan frames (sim.c's convention), 1 encoder
 * quadrature step, the LED state latched out of leds.c for the page ---- */
EXPORT void web_tick(void)
{
    uint16_t ab = 0;
    uint8_t push = 0;
    enc_quadrature_tick(&ab, &push);
    fm1_ms++;
    keyscan_poll(web_key_mask | web_btn_mask);   /* debounce frame 1 */
    keyscan_poll(web_key_mask | web_btn_mask);   /* debounce frame 2 -> emit */
    encoders_poll(ab, push);
    { uint32_t k;
      web_keyled = 0;
      for (k = 0; k < NKEYS; k++)
          web_keyled |= (uint32_t)leds_key_get(NOTE_KEY0 + k) << k;
      for (k = 0; k < 16u; k++)
          web_grid[k] = leds_grid_get((uint8_t)k); }
}

static int web_booted;
EXPORT void web_boot(void)
{
    if (web_booted) return;
    web_booted = 1;
    midi_transport_send = transport_hook;
    ctrl_init();
}
