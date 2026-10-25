/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller
 *
 * fm1-ctrl's MIDI byte-stream contract, against the real app/app.c through a
 * fake platform. Ported from the v1 host tests (/workspace/fm1-ctrl-recovery/
 * v1-source/test_midi.c), keeping the cases that still say something and
 * dropping the ones that tested the old module wiring.
 *
 * What changed with the port: keyscan, encoders, leds and map are gone, so
 * debounce (T1's two-frame rule), label->matrix (T12, T15) and the SysEx map
 * (T13) have no code to test any more -- the platform owns the first two and
 * the map is compiled in. Their assertions moved to the map itself, which is
 * what v2 pins.
 *
 * Build and run: app/tests/run.sh
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fake_plat.h"
#include "../app.h"
#include "../app.c"             /* the app under test: the shipped source, not a copy */

/* ---- helpers ----
 * One "frame" is what the device does between two passes of the main loop: the
 * audio interrupt runs app_render (which drains MIDI in -- the platform's
 * idiom, and template/app.c does the same), then the main loop runs app_frame
 * (which scans the panel). Testing them in this order is what makes the LED
 * assertions below mean anything: on the device, a host message is only read
 * because the render was called. */
static void frames(int n)
{
    while (n-- > 0) {
        int32_t lr[16];
        fp.ms += 16;                /* the loop runs at ~60 Hz, and the screen is
                                     * redrawn on a 100 ms timer */
        app_render(lr, 8, 4096);
        app_frame();
    }
}

/* the render alone, for the cases that ask what the audio callback does */
static void render_once(void)
{
    int32_t lr[16];
    app_render(lr, 8, 4096);
}

/* boot the app the way the platform does, then let MASTER announce its first
 * read (it always does) and clear the capture: from here on every message is
 * one the test asked for. */
static void boot(void)
{
    fake_reset();
    /* app_init() does not reset its own state, and on the device it does not
     * need to: ctl lives in .bss and a boot starts it at zero. In the test the
     * process outlives many "boots", so a lit LED from the previous case would
     * survive into this one and make an assertion about *this* case pass for
     * the wrong reason. Clear it here, which is what .bss does on a device. */
    memset(&ctl, 0, sizeof ctl);
    /* the twenty keys on PA7 / PA8 are the only ones with a converter: keys 0..19.
     * On the device plat_analog_mask() says so; the fake is told once per boot. */
    fp.analog = 0x000FFFFFu;
    app_init();
    app_ui_init();
    frames(1);
    /* the MASTER window is MASTER_WIN frames deep: fill it with whatever the pot
     * is resting at, the way a device that has been on for a second looks */
    frames(MASTER_WIN);
    sent_clear();
}

static int find(uint32_t st, uint32_t d1)      /* index of the first matching sent message */
{
    int i;
    for (i = 0; i < sent_n; i++)
        if (sent[i].st == st && sent[i].d1 == d1)
            return i;
    return -1;
}

static int count(uint32_t st)                   /* how many with this status */
{
    int i, n = 0;
    for (i = 0; i < sent_n; i++)
        if (sent[i].st == st)
            n++;
    return n;
}

int main(void)
{
    int i;

    /* ================= T1: the key map is F3..G5 = notes 53..79 =============
     * Not 48..72, not 60 at key 0: DriftSeq's emulator decodes note 53 as key 0
     * (web/emu.js:184), so this is the baseline. */
    {
        boot();

        fp.keys = 1u << 12;                     /* key 12 = F3 + 12 = C4 */
        frames(1);
        assert(sent_n == 1);
        assert(sent[0].st == 0x90 && sent[0].d1 == 53 + 12 && sent[0].d2 == 100);
        assert(sent[0].to == (TO_USB | TO_BLE));   /* both links, always */

        fp.keys = 0;
        frames(1);
        assert(sent_n == 2);
        assert(sent[1].st == 0x80 && sent[1].d1 == 53 + 12 && sent[1].d2 == 0);
    }

    /* ================= T2: the whole key span, ends included ================
     * 27 keys, 53..79, and nothing outside. */
    {
        boot();

        fp.keys = 1u << 0;                      /* F3 = 53 */
        frames(1);
        assert(sent_n == 1 && sent[0].d1 == 53);

        boot();
        fp.keys = 1u << (NKEYS - 1);             /* G5 = 79 */
        frames(1);
        assert(sent_n == 1 && sent[0].d1 == 79);

        /* the platform masks to NKEYS bits, so a stray bit cannot invent a note */
        boot();
        fp.keys = 0xFFFFFFFFu;
        frames(1);
        assert(sent_n == NKEYS);                 /* exactly 27 note-ons, no more */
        for (i = 0; i < sent_n; i++)
            assert(sent[i].d1 >= 53 && sent[i].d1 <= 79);
    }

    /* ================= T3: velocity comes from the key's own sensing ========
     * touch_vel supplies it; zero falls back to 100 (a press must never be a
     * note-on with velocity 0, which is a note-off to every receiver). */
    {
        boot();
        fp.vel[3] = 127;
        fp.keys = 1u << 3;
        frames(1);
        assert(sent_n == 1 && sent[0].d2 == 127);

        boot();
        fp.vel[3] = 0;                           /* no reading */
        fp.keys = 1u << 3;
        frames(1);
        assert(sent_n == 1 && sent[0].d2 == 100);   /* the default, never 0 */
    }

    /* ================= T4: buttons are CC 40..53, 127 / 0 ================== */
    {
        boot();

        fp.buttons = 1u << B_PLAY;
        frames(1);
        assert(sent_n == 1);
        assert(sent[0].st == 0xB0 && sent[0].d1 == 40 + B_PLAY && sent[0].d2 == 127);

        fp.buttons = 0;
        frames(1);
        assert(sent_n == 2);
        assert(sent[1].st == 0xB0 && sent[1].d1 == 40 + B_PLAY && sent[1].d2 == 0);

        /* the ends of the range: FX is 40, OCT+ is 53 */
        boot();
        fp.buttons = 1u << B_FX;
        frames(1);
        assert(sent_n == 1 && sent[0].d1 == 40 && sent[0].d2 == 127);

        boot();
        fp.buttons = 1u << B_OCTUP;
        frames(1);
        assert(sent_n == 1 && sent[0].d1 == 53 && sent[0].d2 == 127);
    }

    /* ================= T5: buttons are CC, never 0xFA / 0xFC / MMC =========
     * The emulators drop every byte >= 0xF0 (melodee/Felucca index.html:439,472)
     * and no firmware in the ecosystem reads MMC, so transport is CC. */
    {
        boot();
        fp.buttons = (1u << B_PLAY) | (1u << B_REC);
        frames(1);
        assert(sent_n == 2);
        for (i = 0; i < sent_n; i++) {
            assert(sent[i].st == 0xB0);         /* a control change, nothing else */
            assert(sent[i].st < 0xF0);
        }
    }

    /* ================= T6: encoders are CC 20..26, relative 64+n ========== */
    {
        boot();

        fp.enc[EN_SELECT] = 1;                    /* one clockwise detent */
        frames(1);
        assert(sent_n == 1);
        assert(sent[0].st == 0xB0 && sent[0].d1 == 20 && sent[0].d2 == 64 + 1);

        fp.enc[EN_SELECT] = -2;                   /* two counter-clockwise */
        frames(1);
        assert(sent_n == 2);
        assert(sent[1].d1 == 20 && sent[1].d2 == 64 - 2);

        /* the far end of the encoder range: K4 is CC 26 */
        boot();
        fp.enc[EN_K4] = 3;
        frames(1);
        assert(sent_n == 1 && sent[0].d1 == 26 && sent[0].d2 == 64 + 3);
    }

    /* ================= T7: a fast turn splits at +-63 ======================
     * One CC carries 64 +- 63, so 100 detents in one frame is two messages:
     * +63 then +37, both on the same controller. */
    {
        boot();

        fp.enc[EN_ALGO] = 100;
        frames(1);
        assert(sent_n == 2);
        assert(sent[0].d1 == 21 && sent[0].d2 == 64 + 63);
        assert(sent[1].d1 == 21 && sent[1].d2 == 64 + 37);

        boot();
        fp.enc[EN_ALGO] = -100;
        frames(1);
        assert(sent_n == 2);
        assert(sent[0].d2 == 64 - 63 && sent[1].d2 == 64 - 37);
    }

    /* ================= T8: MASTER = CC27 + the 14-bit pair ================= */
    {
        boot();

        fp.master = 2048;                         /* mid travel */
        frames(MASTER_WIN);                       /* the window has to turn over */
        /* three messages: CC27, CC7 coarse, CC39 fine */
        assert(sent_n == 3);
        assert(sent[0].st == 0xB0 && sent[0].d1 == 27);
        assert(sent[1].st == 0xB0 && sent[1].d1 == 7);
        assert(sent[2].st == 0xB0 && sent[2].d1 == 39);
        /* 2048 << 2 = 8192; coarse = 8192 >> 7 = 64, fine = 8192 & 127 = 0 */
        assert(sent[0].d2 == 64 && sent[1].d2 == 64 && sent[2].d2 == 0);

        /* the ends: boot() announced MASTER at 0 and cleared the capture, so go
         * to the top first, then back to the bottom */
        boot();
        fp.master = 4096;                         /* the platform's maximum */
        frames(MASTER_WIN);
        assert(sent_n == 3);
        assert(sent[0].d2 == 127 && sent[1].d2 == 127 && sent[2].d2 == 127);

        sent_clear();
        fp.master = 0;
        frames(MASTER_WIN);
        assert(sent_n == 3);
        assert(sent[0].d2 == 0 && sent[1].d2 == 0 && sent[2].d2 == 0);

        /* the clamp: plat_master() promises 0..4096, but if a platform ever goes
         * past it the wire must not carry a value a receiver would misread
         * (14 bits is 16383; 5000 << 2 would be 20000). */
        sent_clear();
        fp.master = 5000;                         /* out of contract on purpose */
        frames(MASTER_WIN);
        assert(sent_n == 3);
        assert(sent[0].d2 == 127 && sent[1].d2 == 127 && sent[2].d2 == 127);
    }

    /* ================= T9: MASTER's dead band (the 16/48 chatter) ==========
     * The 14-bit pair publishes the low three bits (CC39 = (adc&7)<<4), so one
     * count of noise moved the fine byte by 16 -- measured on the panel as
     * 16/48 alternating. 4x oversampling plus a dead band of 6 counts. */
    {
        static const int32_t noise[] = {0, 1, -1, 2, -2, 1, -1, 2, -2, 0, 1, -1};
        int k, emitted;

        boot();
        fp.master = 2048;
        frames(MASTER_WIN);
        assert(sent_n == 3);                      /* the first read always announces */

        /* now sit still with +-2 counts of noise: 12 frames, no more messages */
        for (k = 0; k < (int)(sizeof noise / sizeof noise[0]); k++) {
            fp.master = (uint32_t)(2048 + noise[k]);
            frames(1);
        }
        emitted = sent_n - 3;
        assert(emitted == 0);                      /* was: a message per count (96 in 200) */

        /* why the dead band has to cover the low three bits */
        assert(((512 & 7) << 4) == 0);
        assert(((514 & 7) << 4) == 32);

        /* a real turn still passes */
        fp.master = 2048 + 40;
        frames(MASTER_WIN);
        assert(sent_n == 6);
    }

    /* ================= T9c: the dead band covers the pot's real noise ======
     * The reported fault: with nothing touching the knob the fine byte stepped
     * 8 / 36 / 88, i.e. adc's low five bits went 2 / 9 / 22 -- a swing of 20 raw
     * counts. The median alone would still follow a *sustained* 20-count shift
     * (two wild readings in a row outvote the rest), so the band has to cover it.
     * 24 raw counts is 0.6 % of travel and under 2 counts of the CC27 the player
     * actually turns. */
    {
        boot();

        fp.master = 1280;
        frames(MASTER_WIN);
        sent_clear();

        /* the three readings from the report, sustained: adc 1282 / 1289 / 1302.
         * All three are inside the band, so none may reach the wire. */
        fp.master = 1282;
        frames(MASTER_WIN * 2);
        assert(sent_n == 0);
        fp.master = 1289;
        frames(MASTER_WIN * 2);
        assert(sent_n == 0);
        fp.master = 1302;
        frames(MASTER_WIN * 2);
        assert(sent_n == 0);

        /* a move a hand would call a move still goes through. 32 counts is one
         * step of the CC27 the player sees (adc >> 5), and it must not be eaten. */
        fp.master = 1280 + 32;
        frames(MASTER_WIN);
        assert(sent_n == 3);
        assert(sent[0].d2 == ((1280 + 32) << 2) >> 7);
    }

    /* ================= T9b: a wild reading is ignored, not averaged ========
     * The pot does produce the odd reading far outside its own noise -- and the
     * panel showed 8 / 36 / 88 (adc's low five bits going 2 / 9 / 22) with
     * nothing touching the knob. A mean would spread one such reading over the
     * whole window; the median drops it. One frame in eight wild must therefore
     * change nothing at all, and the knob must still follow a real turn at once.
     */
    {
        boot();

        fp.master = 2048;
        frames(MASTER_WIN);
        assert(sent_n == 3);                      /* announced: a real move */
        sent_clear();

        /* every other frame produces a far-out reading: the median ignores them */
        fp.seq_n = MASTER_WIN;
        fp.seq[0] = 2048 + 400; fp.seq[1] = 2048;
        fp.seq[2] = 2048 - 400; fp.seq[3] = 2048;
        fp.seq[4] = 2048 + 400; fp.seq[5] = 2048;
        fp.seq[6] = 2048 - 400; fp.seq[7] = 2048;
        fp.seq_i = 0;
        frames(MASTER_WIN * 2);
        assert(sent_n == 0);                      /* 2048 throughout */

        /* a real turn is followed within the window, not averaged away */
        fp.seq_n = 0;
        fp.master = 2200;                         /* 152 counts: past the dead band */
        frames(MASTER_WIN);
        assert(sent_n == 3);
        assert(sent[0].d2 == (2200 << 2) >> 7);
    }

    /* ================= T10: the LEDs echo THIS unit, not the host ==========
     * app.c is included, not linked, so the test reads the app's own state. The
     * FM-1 shows what its own panel is doing; a host's note or CC is counted and
     * dropped, and must not light anything. */
    {
        boot();

        /* a key this unit holds lights that key, and only that key */
        fp.keys = 1u << (60 - KEY_NOTE0);
        frames(1);
        assert(ctl.on_keys == (1u << (60 - KEY_NOTE0)));

        /* a button it holds lights its label */
        fp.buttons = 1u << B_PLAY;
        frames(1);
        assert(ctl.on_btn == (1u << B_PLAY));

        /* and letting go clears both */
        fp.keys = 0;
        fp.buttons = 0;
        frames(1);
        assert(ctl.on_keys == 0 && ctl.on_btn == 0);
    }

    {
        boot();

        /* what the host sends back is drained and counted, and lights nothing:
         * the panel has one source of truth */
        fake_midi_in(0x90, 60, 100, PORT_USB);
        fake_midi_in(0xB0, 54, 0x7F, PORT_USB);
        frames(1);
        assert(sent_n == 0);                       /* input alone emits nothing */
        assert(ctl.got == 2);                      /* but it was seen */
        assert(ctl.on_keys == 0);                  /* and not obeyed */
        assert(ctl.on_btn == 0);

        /* the whole span the host used to drive is inert now */
        fake_midi_in(0x90, 53, 100, PORT_USB);
        fake_midi_in(0xB0, 67, 0x7F, PORT_USB);
        frames(1);
        assert(ctl.on_keys == 0 && ctl.on_btn == 0);
    }

    /* ================= T10b: the hardware is told what to show =============
     * A controller may keep perfect state and never write it to the panel: the
     * keys stay dark and every test above still passes. So assert the call. */
    {
        boot();

        fp.keys = 1u << (60 - KEY_NOTE0);          /* key C4 */
        fp.buttons = 1u << B_FX;                   /* button FX */
        frames(1);
        assert(fp.led_n > 0);                      /* the panel was written at all */
        assert(fp.led_k == (1u << (60 - KEY_NOTE0)));
        assert(fp.led_b == (1u << B_FX));

        /* and releasing is written too, not merely computed */
        fp.keys = 0;
        fp.buttons = 0;
        frames(1);
        assert(fp.led_k == 0);
        assert(fp.led_b == 0);

        /* nothing changed, so nothing is rewritten: keeping the state is not the
         * same as showing it, and the scan is 60 times a second */
        {
            int n = fp.led_n;
            frames(3);
            assert(fp.led_n == n);
        }
    }

    /* ================= T10c: pressure is polyphonic aftertouch, 0xA0 =======
     * The twenty keys on the analog rows report how hard they are held; the wire
     * carries it per key as 0xA0 note pressure. A press starts it at 0 (the note
     * on is the velocity), a change past the dead band sends, and a small wobble
     * does not. */
    {
        boot();

        /* key 3 down: note on, then aftertouch only once it has a pressure */
        fp.vel[3] = 90;
        fp.press[3] = 40;
        fp.down = 1u << 3;
        fp.keys = 1u << 3;
        frames(1);
        assert(count(0x90) == 1);
        assert(count(0xA0) == 1);
        assert(find(0xA0, 53 + 3) >= 0);
        assert(sent[find(0xA0, 53 + 3)].d2 == 40);

        /* same pressure, frame after frame: no more aftertouch */
        sent_clear();
        frames(30);
        assert(count(0xA0) == 0);

        /* a wobble inside the dead band stays off the wire */
        fp.press[3] = 41;
        frames(1);
        assert(count(0xA0) == 0);
        fp.press[3] = 40;
        frames(1);
        assert(count(0xA0) == 0);

        /* a real lean sends the new value */
        fp.press[3] = 60;
        frames(1);
        assert(count(0xA0) == 1);
        assert(sent[find(0xA0, 53 + 3)].d2 == 60);

        /* lifting: the reading is not a pressure, so it is not sent */
        sent_clear();
        fp.down = 0;                               /* the key has left its seat */
        fp.press[3] = 10;
        frames(1);
        assert(count(0xA0) == 0);

        /* released: the note off, and the state cleared for the next press */
        fp.keys = 0;
        frames(1);
        assert(count(0x80) == 1);
        assert(ctl.press[3] == 0);
    }

    /* ================= T10d: the top row has no pressure, and says so ======
     * The seven keys of the top row (keys 20..26, C#5..G5) have no converter of
     * their own: their velocity comes from a probe through the LED line and they
     * have no pressure at all. plat_analog_mask is what tells the app, and a
     * platform that (wrongly) reported a reading must still not put it on the
     * wire. */
    {
        boot();

        assert(fp.analog == 0x000FFFFFu);          /* 20 keys: 0..19 */
        for (i = 20; i < NKEYS; i++)
            assert(!((analog_mask >> i) & 1u));

        fp.vel[22] = 88;                           /* a top-row key, pressed */
        fp.press[22] = 99;                         /* a reading it cannot have */
        fp.down = 1u << 22;
        fp.keys = 1u << 22;
        frames(10);
        assert(count(0x90) == 1);                  /* the note, with its velocity */
        assert(sent[find(0x90, 53 + 22)].d2 == 88);
        assert(count(0xA0) == 0);                  /* and never an aftertouch */

        /* the analog ones still do */
        fp.vel[19] = 70;
        fp.press[19] = 55;                         /* key 19 = C5, the last analog key */
        fp.down |= 1u << 19;
        fp.keys |= 1u << 19;
        frames(1);
        assert(sent[find(0xA0, 53 + 19)].d2 == 55);
    }

    /* ================= T10e: the screen shows this unit ===================
     * The key display draws a pressure bar for the analog keys and a block at the
     * top for the top row, so the two kinds of press do not look alike. Drawn
     * from local state: a host message changes nothing. */
    {
        boot();

        fp.rect_n = 0;
        fp.rect_h_max = 0;
        fp.keys = 1u << 3;                         /* one analog key */
        fp.press[3] = 127;
        fp.down = 1u << 3;
        frames(8);                                 /* past the 100 ms redraw */
        assert(fp.rect_n > 0);                     /* something was filled */
        assert(fp.rect_h_max > 80);                /* a full bar, not a stub */

        /* half the pressure, half the bar */
        boot();
        fp.rect_h_max = 0;
        fp.keys = 1u << 3;
        fp.press[3] = 64;
        fp.down = 1u << 3;
        frames(8);
        assert(fp.rect_h_max > 30 && fp.rect_h_max < 60);

        /* a top-row key: its block is short and at the top, never a tall bar */
        boot();
        fp.rect_h_max = 0;
        fp.keys = 1u << 22;
        fp.press[22] = 127;                        /* would be a full bar if obeyed */
        fp.down = 1u << 22;
        frames(8);
        assert(fp.rect_h_max > 0 && fp.rect_h_max <= 12);

        /* and the host cannot paint the screen either */
        boot();
        fp.rect_n = 0;
        fake_midi_in(0x90, 60, 100, PORT_USB);
        fake_midi_in(0xB0, 54, 0x7F, PORT_USB);
        frames(1);
        /* the redraw is 10 Hz, so one frame may not have drawn yet; what must
         * hold is that no key was marked on */
        assert(ctl.on_keys == 0 && ctl.on_btn == 0);
    }

    /* ================= T10f: the frames between the 60 Hz ones count too ===
     * main_fm1.c runs app_frame at most every 16 ms and app_input_only in
     * between, which is most of the loop. Both must read the panel, or a key
     * held through a quiet moment is never seen. */
    {
        boot();

        fp.keys = 1u << 5;
        app_input_only();
        assert(ctl.keys == (1u << 5));             /* seen without app_frame */
        assert(ctl.on_keys == (1u << 5));
        assert(count(0x90) == 1);

        fp.keys = 0;
        app_input_only();
        assert(ctl.keys == 0);
        assert(count(0x80) == 1);
    }

    /* ...and they light the panel. A fresh boot has written nothing yet, so the
     * first held key must reach plat_leds on its own. */
    {
        boot();

        fp.keys = 1u << 9;
        app_input_only();
        assert(fp.led_k == (1u << 9));
        assert(fp.led_b == 0);
    }

    /* ...and they keep the pressure current. touch_poll is what moves a key's
     * velocity and pressure from the scan's events into the app's view, so a
     * frame that skips it would carry the previous reading. The fake's touch_poll
     * advances a value that only it can produce -- exactly what would be stale. */
    {
        boot();

        fp.keys = 1u << 5;
        fp.down = 1u << 5;
        fp.press[5] = 40;
        app_frame();                               /* the press, at 40 */
        sent_clear();

        fp.press[5] = 100;                         /* the key leans harder */
        fp.poll_raise = 7;                         /* touch_poll() adds this to every
                                                    * held key's reading, as the real
                                                    * one advances it */
        app_input_only();
        assert(count(0xA0) == 1);
        assert(sent[find(0xA0, 53 + 5)].d2 == 107);

        fp.poll_raise = 0;
    }

    /* ================= T11: MIDI in is drained, not accumulated ============
     * app_render drains the input ring; if it did not, the ring would fill and
     * the host's LEDs would stop arriving. */
    {
        boot();

        for (i = 0; i < 8; i++)
            fake_midi_in(0xB0, (uint32_t)(54 + (i & 13)), 0x7F, PORT_USB);
        {
            int32_t lr[16];
            app_render(lr, 8, 4096);
            for (i = 0; i < 16; i++)
                assert(lr[i] == 0);                /* a controller makes no sound */
        }
        /* the queue is empty: a second render finds nothing and stays silent */
        {
            int32_t lr[16];
            app_render(lr, 8, 4096);
            for (i = 0; i < 16; i++)
                assert(lr[i] == 0);
        }
        assert(sent_n == 0);
    }

    /* ================= T12: the app never transposes =======================
     * OCT- / OCT+ are plain buttons: the octave belongs to the host (RemoteFM1
     * PROTOCOL.md:108 does the same). Pressing them must not shift note numbers. */
    {
        boot();

        fp.buttons = 1u << B_OCTUP;
        frames(1);
        assert(sent_n == 1 && sent[0].d1 == 53 && sent[0].st == 0xB0);

        fp.buttons = 0;
        fp.keys = 1u << 0;
        frames(1);
        assert(find(0x90, 53) >= 0);               /* still F3: no shift */
        assert(find(0x90, 65) < 0);                /* and not an octave up */
    }

    /* ================= T13: a held control sends one event, not 60/s ======
     * app_frame runs at 60 Hz; a key held for a second must not repeat. */
    {
        boot();

        fp.keys = 1u << 5;
        frames(60);                                /* one second at 60 Hz */
        assert(count(0x90) == 1);

        fp.buttons = 1u << B_FX;
        frames(60);
        assert(count(0xB0) == 1);                  /* the one FX press */

        fp.enc[EN_K1] = 0;                         /* no turn */
        frames(60);
        assert(sent_n == 2);                       /* nothing new at all */
    }

    /* ================= T14: the map itself, as DriftSeq decodes it =========
     * The numbers DriftSeq's web/emu.js:184 reads. If someone changes a
     * constant in app.c, this is what breaks, and it should. */
    {
        assert(KEY_NOTE0 == 53);
        assert(ENC_CC0 == 20);
        assert(BTN_CC0 == 40);
        assert(BTN_LED0 == 54);
        assert(MASTER_CC == 27);
        assert(MASTER_MSB == 7);
        assert(MASTER_LSB == 39);
        /* the ranges must not overlap, or a receiver cannot tell them apart */
        assert(ENC_CC0 + NE - 1 == 26);
        assert(BTN_CC0 + NB - 1 == 53);
        assert(BTN_LED0 + NB - 1 == 67);
        assert(KEY_NOTE0 + NKEYS - 1 == 79);
        assert(26 < BTN_CC0);                      /* encoders below the buttons */
        assert(53 < BTN_LED0);                     /* buttons below their LEDs */
        assert(79 < 20 || 79 > 67);                /* notes clear of both CC ranges */
    }

    /* ================= T15: the panel's label ids are what we send =========
     * plat.h's B_* are printed-label ids and the platform owns the matrix map
     * (persisted, OBJ_PANEL). v1's bug -- long-press SAVE opening the diag page
     * from GLO -- came from confusing the two. Nothing to convert here, and a
     * test so it stays that way: B_SAVE must not be assumed equal to any
     * matrix id, only its label. */
    {
        assert(B_SAVE == 7);
        assert(B_GLO == 5);
        assert(B_SAVE != B_GLO);
        /* the button LEDs follow the same order as the buttons */
        assert(BTN_CC0 + B_SAVE < BTN_CC0 + NB);
    }

    printf("ALL HOST TESTS PASSED (%d cases)\n", 23);
    return 0;
}
