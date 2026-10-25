/* SPDX-License-Identifier: MPL-2.0
 * fm1-ctrl — a thin, transparent USB + Bluetooth MIDI controller for the M-VAVE FM-1.
 *
 * Every control sends its own event, unchanged; nothing about the panel is
 * interpreted here. The mapping lives in the receiving host (a web emulator, a
 * DAW, or a map pushed over SysEx), so this firmware has no business logic:
 *
 *   keys     note 53..79   (the FM-1's own layout)   velocity from the key's sensing
 *   pressure 0xA0 note p                              the 20 keys on the analog rows only
 *   encoders CC 20..26                                relative, 64 + detents
 *   buttons  CC 40..53                                labels FX..OCT+, 127 on / 0 off
 *   MASTER   CC 27 (7-bit) + CC 7 / CC 39             the 14-bit pair a DAW reads
 *
 * The panel echoes THIS unit: the keys and buttons it is holding light their
 * LEDs and their place on the screen, and a held key shows the pressure it
 * reports. Feedback from the host is counted and dropped -- one FM-1, one
 * source of truth for what the panel shows.
 *
 * On felucca-bsp via fm1-base: the platform owns the panel, the encoders, the
 * supply rails, the screen, USB and Bluetooth MIDI, and the update path, so this
 * file is the whole app.
 */
#include "fastmath.h"

#define CTRL_MAGIC 0x4354524Cu           /* "CTRL" */

/* the panel's own numbers: the map every receiving host is written against */
#define KEY_NOTE0   53u                  /* note 53: F3 when middle C is 60 */
#define ENC_CC0     20u
#define BTN_CC0     40u
#define BTN_LED0    54u                  /* the host's LED CCs (still counted, no longer drawn) */
#define MASTER_CC   27u
#define MASTER_MSB  7u
#define MASTER_LSB  39u
#define KEY_VEL_DEF 100u
/* pressure: the reading's own noise is a count or two, and it is smoothed over
 * ~5 ms steps, so a dead band of 3 keeps a resting finger off the wire */
#define KEY_AFT     3u

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
/* seven to a row leaves 34 px a column: three characters is what fits */
static const char *const ENC_SNAME[NE] = {"SEL", "ALG", "PRE", "K1", "K2", "K3", "K4"};

typedef struct {
    uint32_t magic;
    uint8_t ble_off;
    uint8_t rsv[3];
} settings_t;
static settings_t set;

static struct {
    uint32_t keys, btn;                  /* held at the last look: what this unit plays */
    uint32_t on_keys, on_btn;            /* what the panel is showing */
    uint32_t led_keys, led_btn;          /* what plat_leds was last told */
    uint8_t  press[NKEYS];               /* the pressure each key reports now, 0..127 */
    uint8_t  press_sent[NKEYS];          /* the pressure the wire last carried */
    int32_t  turns[NE];                  /* detents since boot, per encoder */
    int32_t  master_last;                /* -1: nothing emitted yet */
    uint32_t sent, got;                  /* messages out, messages in */
} ctl;

static uint32_t analog_mask;             /* bit k: key k can report pressure (PA7 / PA8) */

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
                ctl.press_sent[i] = 0;              /* the aftertouch starts over */
            } else {
                out3(0x80u, KEY_NOTE0 + i, 0u);
                ctl.press[i] = 0;
                ctl.press_sent[i] = 0;
            }
        }
    ctl.keys = now;
    ctl.on_keys = now;
}

/* The twenty keys on the analog rows keep reporting how hard they are held; the
 * seven of the top row have no converter of their own and must never send one.
 * Polyphonic (0xA0 note pressure), because the reading is per key. */
static void scan_pressure(void)
{
    uint32_t i;
    for (i = 0; i < NKEYS; i++) {
        uint32_t p;
        int32_t d;
        if (!(analog_mask >> i & 1u))
            continue;                           /* nothing to measure */
        if (!(ctl.keys >> i & 1u)) {
            ctl.press[i] = 0;
            continue;
        }
        p = touch_pressure(i);
        ctl.press[i] = (uint8_t)p;
        if (!touch_down(i))
            continue;                           /* lifting: a lift is not a pressure */
        d = (int32_t)p - (int32_t)ctl.press_sent[i];
        if (d < 0) d = -d;
        if (d < (int32_t)KEY_AFT)
            continue;
        ctl.press_sent[i] = (uint8_t)p;
        out3(0xA0u, KEY_NOTE0 + i, p);
    }
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
    ctl.on_btn = now;
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
    v14 = (uint32_t)adc << 2;                    /* 0..4096 -> 0..16384 */
    /* 4096 << 2 is 16384, one past 14 bits: this is the only clamp needed, and
     * it also covers a platform that one day returns more than its contract. */
    if (v14 > 16383u) v14 = 16383u;
    out3(0xB0u, MASTER_CC, v14 >> 7);            /* CC27: 7-bit, the emulators' own knob */
    out3(0xB0u, MASTER_MSB, v14 >> 7);           /* CC7:  coarse */
    out3(0xB0u, MASTER_LSB, v14 & 0x7Fu);        /* CC39: fine */
}

/* --------------------------------------------------------------- host -> in --- */
/* Drained and counted, never acted on: the panel shows this unit, not the host.
 * Draining still matters -- a full ring stops the link's own bookkeeping. */
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        (void)pkt;
        (void)port;
        ctl.got++;
    }
}

/* ---------------------------------------------------------------- the LEDs --- */
/* Written when they change, not every frame: keeping the state is not the same
 * as showing it. */
static void write_leds(void)
{
    if (ctl.on_keys == ctl.led_keys && ctl.on_btn == ctl.led_btn)
        return;
    ctl.led_keys = ctl.on_keys;
    ctl.led_btn = ctl.on_btn;
    plat_leds(ctl.led_btn, ctl.led_keys);
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

    /* the fourteen buttons: lit while this unit holds them */
    cv_begin(240, 40, C_BLACK);
    for (i = 0; i < NB; i++) {
        int32_t x = 4 + (int32_t)(i % 7u) * 34, y = (int32_t)(i / 7u) * 20;
        int on = (ctl.on_btn >> i) & 1u;
        cv_text(x, y, &FONT_XS, BTN_NAME[i], on ? C_WHITE : C_DIM);
    }
    cv_blit(0, 32);

    /* the encoders: a detent count each, added up since boot */
    cv_begin(240, 32, C_BLACK);
    for (i = 0; i < NE; i++) {
        int32_t x = 4 + (int32_t)i * 34;
        cv_text(x, 0, &FONT_XS, ENC_SNAME[i], C_GRAY);
        num_text(ctl.turns[i], t);
        cv_text(x, 13, &FONT_XS, t, C_HI);
    }
    cv_blit(0, 72);

    /* the keys, a column each. The twenty on the analog rows grow a bar with the
     * pressure they report; the seven of the top row have no converter of their
     * own, so while held they carry a block at the top -- a different place for a
     * different thing, not a bar pretending to be a reading. */
    cv_begin(240, 136, C_BLACK);
    for (i = 0; i < NKEYS; i++) {
        int32_t x = 4 + (int32_t)i * 8;
        int on = (ctl.on_keys >> i) & 1u;
        if (analog_mask >> i & 1u) {
            int32_t h = on ? (int32_t)ctl.press[i] * 88 / 127 : 0;
            if (on && h < 3) h = 3;              /* a press is never invisible */
            if (h) cv_rect(x, 96 - h, 6, h, C_HI);
            else   cv_line(x, 95, x + 5, 95, C_LINE);
        } else {
            if (on) cv_rect(x, 10, 6, 8, C_HI);
            else    cv_line(x, 95, x + 5, 95, C_LINE);
        }
    }
    cv_blit(0, 104);
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
    midi_in();                           /* the ring: drained, counted, not obeyed */
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;                       /* a controller makes no sound */
}

void app_panic(void) { }

void app_ui_init(void)
{
    touch_on(1);                         /* the keys' velocity and pressure readings on */
    analog_mask = plat_analog_mask();    /* which keys can report a pressure at all */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw();
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    touch_poll();                        /* first: touch_vel is this frame's */
    scan_keys();
    scan_pressure();
    scan_buttons();
    scan_encoders();
    scan_master();
    write_leds();
    if (now - redraw_at >= 100u) {       /* the screen is a status page, 10 Hz */
        redraw_at = now;
        draw();
    }
}

void app_input_only(void)
{
    touch_poll();                        /* the frames between the 60 Hz ones still
                                          * advance the pressure and the keys */
    scan_keys();
    scan_pressure();
    scan_buttons();
    scan_encoders();
    write_leds();
}

void app_say(const char *a, const char *b) { (void)a; (void)b; }
