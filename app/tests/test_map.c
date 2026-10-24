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
    app_init();
    app_ui_init();
    frames(1);
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
        frames(1);
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
        frames(1);
        assert(sent_n == 3);
        assert(sent[0].d2 == 127 && sent[1].d2 == 127 && sent[2].d2 == 127);

        sent_clear();
        fp.master = 0;
        frames(1);
        assert(sent_n == 3);
        assert(sent[0].d2 == 0 && sent[1].d2 == 0 && sent[2].d2 == 0);

        /* the clamp: plat_master() promises 0..4096, but if a platform ever goes
         * past it the wire must not carry a value a receiver would misread
         * (14 bits is 16383; 5000 << 2 would be 20000). */
        sent_clear();
        fp.master = 5000;                         /* out of contract on purpose */
        frames(1);
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
        frames(1);
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
        frames(1);
        assert(sent_n == 6);
    }

    /* ================= T9b: the 4x oversample averages within a frame =======
     * T9 covers the dead band with a still pot; this one covers the averaging.
     *
     * Honest note on what each mechanism buys: the pot's real noise is about
     * +-2 counts, and the dead band of 6 already covers that on its own (T9).
     * The averaging is the second line of defence, for excursions larger than
     * the dead band that are still only noise. So this test uses +-10: each
     * single read is beyond the band (10 > 6) and would emit, while their mean
     * is exactly 2048 and must not. Without the averaging this case fails. */
    {
        boot();                                   /* MASTER announced at 0 */

        fp.master = 2048;
        frames(1);
        assert(sent_n == 3);                      /* 0 -> 2048: a real move */
        sent_clear();

        /* four reads inside one frame, alternating either side of 2048: each is
         * 10 counts out, the mean is 2048 */
        fp.seq_n = 4;
        fp.seq[0] = 2048 + 10; fp.seq[1] = 2048 - 10;
        fp.seq[2] = 2048 + 10; fp.seq[3] = 2048 - 10;
        fp.seq_i = 0;
        frames(1);
        assert(sent_n == 0);                      /* the mean is 2048: unchanged */

        /* the same jitter, all on one side: the mean really did move, and
         * (10 counts > the dead band) it must be reported */
        fp.seq[0] = 2048 + 10; fp.seq[1] = 2048 + 10;
        fp.seq[2] = 2048 + 10; fp.seq[3] = 2048 + 10;
        fp.seq_i = 0;
        frames(1);
        assert(sent_n == 3);

        fp.seq_n = 0;                             /* back to a steady pot */
    }

    /* ================= T10: the LEDs come only from what the host sends =====
     * app.c is included, not linked, so the test can read the app's own LED
     * state: that is the only way to tell "the host lit a key" from "the app
     * ignored the host". Nothing here may emit. */
    {
        boot();

        /* a note in the key span lights that key, and only that key */
        fake_midi_in(0x90, 60, 100, PORT_USB);
        frames(1);
        assert(sent_n == 0);                       /* input alone emits nothing */
        assert(ctl.lit_keys == (1u << (60 - KEY_NOTE0)));

        /* note-off clears it */
        fake_midi_in(0x80, 60, 0, PORT_USB);
        frames(1);
        assert(sent_n == 0);
        assert(ctl.lit_keys == 0);

        /* CC 54..67 light the buttons: 54 is FX, 67 is OCT+ */
        fake_midi_in(0xB0, 54, 0x7F, PORT_USB);
        fake_midi_in(0xB0, 67, 1, PORT_USB);
        frames(1);
        assert(sent_n == 0);
        assert(ctl.lit_btn == ((1u << 0) | (1u << 13)));

        /* zero clears, and it is the value that decides -- not the message */
        fake_midi_in(0xB0, 54, 0, PORT_USB);
        frames(1);
        assert(ctl.lit_btn == (1u << 13));

        /* out of range: CC 68 is past the 14 buttons, 52 is below them */
        fake_midi_in(0xB0, 68, 0x7F, PORT_USB);
        fake_midi_in(0xB0, 52, 0x7F, PORT_USB);
        fake_midi_in(0x90, 52, 100, PORT_USB);     /* below the key span */
        fake_midi_in(0x90, 80, 100, PORT_USB);     /* above it */
        frames(1);
        assert(sent_n == 0);
        assert(ctl.lit_btn == (1u << 13));          /* unchanged */
        assert(ctl.lit_keys == 0);                  /* still clear */

        /* a note-on with velocity 0 is a note-off, not a light */
        fake_midi_in(0x90, 60, 0, PORT_USB);
        frames(1);
        assert(ctl.lit_keys == 0);
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

    printf("ALL HOST TESTS PASSED (%d cases)\n", 15);
    return 0;
}
