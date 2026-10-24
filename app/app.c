/* SPDX-License-Identifier: MPL-2.0
 * fm1-ctrl — a thin, transparent USB + Bluetooth MIDI controller for the M-VAVE FM-1.
 *
 * Every control sends its own event, unchanged; nothing about the panel is
 * interpreted here. The mapping lives in the receiving host (a web emulator, a
 * DAW, or a map pushed over SysEx), so this firmware has no business logic:
 *
 *   keys     note 53..79   (F3 .. G5)      velocity from the key's own sensing
 *   encoders CC 20..26                     relative, 64 + detents
 *   buttons  CC 40..53                     labels FX..OCT+, 127 on / 0 off
 *   MASTER   CC 27 (7-bit) + CC 7 / CC 39  (the 14-bit pair a DAW reads)
 *
 * LEDs come only from what the host sends back: notes 53..79 light the keys,
 * CC 54..67 the button LEDs. No local state is invented.
 *
 * On felucca-bsp via fm1-base: the platform owns the panel, the encoders, the
 * supply rails, the screen, USB and Bluetooth MIDI, and the update path, so this
 * file is the whole app.
 */
#include "fastmath.h"

#define CTRL_MAGIC 0x4354524Cu           /* "CTRL" */

/* the panel's own numbers: the map every receiving host is written against */
#define KEY_NOTE0   53u                  /* F3 */
#define ENC_CC0     20u
#define BTN_CC0     40u
#define BTN_LED0    54u
#define MASTER_CC   27u
#define MASTER_MSB  7u
#define MASTER_LSB  39u
#define KEY_VEL_DEF 100u

/* MASTER: the pot wanders +-2 counts and every one used to reach the wire. The
 * 14-bit pair also publishes the low three bits (CC39 = (adc&7)<<4), so ONE count
 * of noise moved the "fine" byte by 16 -- the 16/48 chatter heard on the panel.
 * Average first, then a dead band of 6 counts: 0.6 % of travel, under a hand's
 * resolution, over the conversion's own noise. */
#define MASTER_OVERSAMPLE 4
#define MASTER_DEADBAND   6

static const char *const BTN_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                         "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_NAME[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};

typedef struct {
    uint32_t magic;
    uint8_t ble_off;
    uint8_t rsv[3];
} settings_t;
static settings_t set;

static struct {
    uint32_t keys, btn;                  /* held at the last look */
    int32_t  turns[NE];                  /* detents since boot, per encoder */
    int32_t  master_last;                /* -1: nothing emitted yet */
    uint32_t sent, got;                  /* messages out, messages in */
    uint32_t lit_keys, lit_btn;          /* the LEDs the host asked for */
} ctl;

int ctrl_ble_off(void) { return set.ble_off; }

/* ------------------------------------------------------------------- MIDI --- */
/* One message to both links: a computer over USB, and Bluetooth. */
static void out3(uint32_t st, uint32_t d1, uint32_t d2)
{
    plat_midi_send(0x03u | st << 8 | d1 << 16 | d2 << 24, TO_USB | TO_BLE);
    ctl.sent++;
}

/* ------------------------------------------------------------- panel -> out --- */
static void scan_keys(void)
{
    uint32_t now = plat_keys() & ((1u << NKEYS) - 1u), i, changed = now ^ ctl.keys;
    if (!changed)
        return;
    for (i = 0; i < NKEYS; i++)
        if (changed >> i & 1u) {
            if (now >> i & 1u) {
                uint32_t vel = touch_vel(i);        /* the key's own sensing */
                out3(0x90u, KEY_NOTE0 + i, vel ? vel : KEY_VEL_DEF);
            } else {
                out3(0x80u, KEY_NOTE0 + i, 0u);
            }
        }
    ctl.keys = now;
}

static void scan_buttons(void)
{
    uint32_t now = plat_buttons() & ((1u << NB) - 1u), i, changed = now ^ ctl.btn;
    if (!changed)
        return;
    for (i = 0; i < NB; i++)
        if (changed >> i & 1u)
            out3(0xB0u, BTN_CC0 + i, (now >> i & 1u) ? 127u : 0u);
    ctl.btn = now;
}

static void scan_encoders(void)
{
    uint32_t i;
    for (i = 0; i < NE; i++) {
        int32_t d = plat_enc((int)i), left = d < 0 ? -d : d, sign = d < 0 ? -1 : 1;
        ctl.turns[i] += d;
        /* a fast turn is many detents at once; one CC carries +-63, so split */
        while (left > 0) {
            int32_t n = left > 63 ? 63 : left;
            out3(0xB0u, ENC_CC0 + i, (uint32_t)(64 + sign * n));
            left -= n;
        }
    }
}

static void scan_master(void)
{
    uint32_t i, v14;
    int32_t adc = 0, d;

    for (i = 0; i < MASTER_OVERSAMPLE; i++)
        adc += (int32_t)plat_master();
    adc /= (int32_t)MASTER_OVERSAMPLE;

    if (ctl.master_last < 0) {
        ctl.master_last = adc;                   /* the first read always announces */
    } else {
        d = adc - ctl.master_last;
        if (d < MASTER_DEADBAND && d > -MASTER_DEADBAND)
            return;
        ctl.master_last = adc;
    }
    if (adc < 0) adc = 0;
    if (adc > 4096) adc = 4096;
    v14 = (uint32_t)adc << 2;                    /* 0..4096 -> 0..16384 */
    if (v14 > 16383u) v14 = 16383u;
    out3(0xB0u, MASTER_CC, v14 >> 7);            /* CC27: 7-bit, the emulators' own knob */
    out3(0xB0u, MASTER_MSB, v14 >> 7);           /* CC7:  coarse */
    out3(0xB0u, MASTER_LSB, v14 & 0x7Fu);        /* CC39: fine */
}

/* ------------------------------------------------------------- host -> LEDs --- */
/* Nothing decides anything here: a note or CC that says "light this" lights it. */
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        ctl.got++;
        if ((st & 0xF0u) == 0x90u || (st & 0xF0u) == 0x80u) {
            if (d1 >= KEY_NOTE0 && d1 < KEY_NOTE0 + NKEYS) {          /* the keys */
                uint32_t bit = 1u << (d1 - KEY_NOTE0);
                if ((st & 0xF0u) == 0x90u && d2) ctl.lit_keys |= bit; else ctl.lit_keys &= ~bit;
                continue;
            }
        }
        if ((st & 0xF0u) == 0xB0u && d1 >= BTN_LED0 && d1 < BTN_LED0 + NB) {
            uint32_t bit = 1u << (d1 - BTN_LED0);                     /* the buttons */
            if (d2) ctl.lit_btn |= bit; else ctl.lit_btn &= ~bit;
        }
    }
}

/* --------------------------------------------------------------------- UI --- */
static uint32_t redraw_at;

static void num_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void text_cat(char *d, const char *s)
{
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}

static void draw(void)
{
    char t[24], line[40];
    uint32_t i;
    int32_t y;

    /* the header: the name, the Bluetooth state, the counters -- a glance says
     * whether both links are alive */
    cv_begin(240, 32, C_BLACK);
    {
        int32_t x = cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE);
#if FM1B_BLE
        x = cv_text(x + 8, 10, &FONT_XS, "BLE", plat_ble_connected() ? C_HI : C_DIM);
#endif
#if FM1B_BLE_CENTRAL
        x = cv_text(x + 6, 10, &FONT_XS, "DEV", plat_ble_device() == BLE_DEV_READY ? C_HI : C_DIM);
#endif
        (void)x;
        line[0] = 0;
        text_cat(line, "OUT ");
        num_text((int32_t)ctl.sent, t);
        text_cat(line, t);
        text_cat(line, "  IN ");
        num_text((int32_t)ctl.got, t);
        text_cat(line, t);
        cv_text(230 - text_w(&FONT_XS, line), 10, &FONT_XS, line, C_GRAY);
    }
    cv_blit(0, 0);

    /* the buttons, lit while the host lights them, and the encoders' counts */
    cv_begin(240, 132, C_BLACK);
    for (i = 0; i < NB; i++) {
        int32_t x = 4 + (int32_t)(i % 4u) * 58, yy = (int32_t)(i / 4u) * 21;
        int lit = (ctl.lit_btn >> i) & 1u;
        cv_text(x, yy, &FONT_XS, BTN_NAME[i], lit ? C_WHITE : C_DIM);
    }
    for (i = 0; i < NE; i++) {
        int32_t x = 4 + (int32_t)(i % 4u) * 58;
        y = 88 + (int32_t)(i / 4u) * 22;
        cv_text(x, y, &FONT_XS, ENC_NAME[i], C_GRAY);
        num_text(ctl.turns[i], t);
        cv_text(x, y + 12, &FONT_XS, t, C_HI);
    }
    cv_blit(0, 36);

    /* the keys the host lights */
    cv_begin(240, 60, C_BLACK);
    for (i = 0; i < NKEYS; i++) {
        int32_t x = 4 + (int32_t)i * 8;
        int lit = (ctl.lit_keys >> i) & 1u;
        cv_text(x, 4, &FONT_XS, lit ? "*" : ".", lit ? C_HI : C_LINE);
    }
    cv_blit(0, 180);
}

/* ---------------------------------------------------------------- the app --- */
void app_init(void)
{
    if (plat_store_load(OBJ_CTRL, &set, sizeof set) != (int)sizeof set || set.magic != CTRL_MAGIC) {
        set.magic = CTRL_MAGIC;
        set.ble_off = 0;
    }
    ctl.master_last = -1;
    ctl.keys = plat_keys();
    ctl.btn = plat_buttons();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    (void)master_q12;
    midi_in();                           /* the LEDs: what the host sent back */
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;                       /* a controller makes no sound */
}

void app_panic(void) { }

void app_ui_init(void)
{
    touch_on(1);                         /* the keys' velocity and pressure readings on */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw();
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    touch_poll();                        /* first: touch_vel is this frame's */
    scan_keys();
    scan_buttons();
    scan_encoders();
    scan_master();
    if (now - redraw_at >= 100u) {       /* the screen is a status page, 10 Hz */
        redraw_at = now;
        draw();
    }
}

void app_input_only(void)
{
    scan_keys();
    scan_buttons();
    scan_encoders();
}

void app_say(const char *a, const char *b) { (void)a; (void)b; }
