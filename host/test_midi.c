/* fm1-ctrl host tests — MIDI byte-stream contract (plan Tasks 1-2).
 *
 * Build+run via build.sh. Plain asserts, no framework.
 * Panel topology (real FM-1): 27 keys (notes 48..72), 14 buttons (CC 40..53),
 * 7 encoders (CC 20..26 in the melodee-native default map).
 */
#include <assert.h>
#include <stdio.h>
#include "midi_io.h"
#include "keyscan.h"
#include "encoders.h"
#include "leds.h"
#include "ctrl_glue.h"
#include "map.h"

static midi_msg3 g(int i) { return midi_cap_get(i); }

/* T12 stand-in for panel.c's label table: matrix 2 -> FX (label 0) */
static int shift1(int m) { return (m + 12) % 14; }

int main(void) {
    ctrl_init();     /* map_reset + all modules, as on the device */

    /* T1: chromatic from F3 (CTL-1 KEY0_NOTE): key idx 12 = note 65.
     * Debounce needs 2 stable frames. */
    keyscan_poll(1u << 12);
    assert(midi_cap_len() == 0);
    keyscan_poll(1u << 12);
    assert(midi_cap_len() == 1);
    midi_msg3 m = g(0);
    assert(m.b[0] == 0x90 && m.b[1] == 53 + 12 && m.b[2] == 0x64);
    keyscan_poll(0); keyscan_poll(0);
    assert(midi_cap_len() == 2);
    m = g(1);
    assert(m.b[0] == 0x80 && m.b[1] == 53 + 12 && m.b[2] == 0x00);

    /* T2: key idx 14 = note 67 (G4) */
    keyscan_poll(1u << 14); keyscan_poll(1u << 14);
    assert(midi_cap_len() == 3 && g(2).b[1] == 67 && g(2).b[0] == 0x90);
    keyscan_poll(0); keyscan_poll(0);  /* release before encoder tests */

    /* T3: encoder 0 (SELECT), one CW detent -> single CC20 +1 (0x41) on ch 1 */
    midi_cap_clear();
    const uint8_t cw[5]  = {0, 1, 3, 2, 0};
    uint16_t ab;   /* 16 bits: encoder fields live at bit positions 0..12 */
    for (int i = 0; i < 5; i++) { ab = cw[i]; encoders_poll(ab, 0); }
    assert(midi_cap_len() == 1);
    m = g(0);
    assert(m.b[0] == 0xB0 && m.b[1] == 20 && m.b[2] == 0x41);

    /* T3b: CCW detent -> 0x3F */
    const uint8_t ccw[4] = {2, 3, 1, 0};
    for (int i = 0; i < 4; i++) { ab = ccw[i]; encoders_poll(ab, 0); }
    assert(midi_cap_len() == 2);
    m = g(1);
    assert(m.b[0] == 0xB0 && m.b[1] == 20 && m.b[2] == 0x3F);

    /* T4: encoder 2 (PRESETS) -> CC 22 */
    midi_cap_clear();
    for (int i = 0; i < 5; i++) { ab = (uint16_t)(cw[i] << 4); encoders_poll(ab, 0); }
    assert(midi_cap_len() == 1 && g(0).b[1] == 22 && g(0).b[0] == 0xB0);

    /* T4b: encoder 6 (KNOB 4) -> CC 26 */
    midi_cap_clear();
    for (int i = 0; i < 5; i++) { ab = (uint16_t)(cw[i] << 12); encoders_poll(ab, 0); }
    assert(midi_cap_len() == 1 && g(0).b[1] == 26 && g(0).b[0] == 0xB0);

    /* T5: push+turn -> channel 2 (0xB1), same relative encoding */
    midi_cap_clear();
    for (int i = 0; i < 5; i++) { ab = cw[i]; encoders_poll(ab, 0x01); }
    assert(midi_cap_len() == 1 && g(0).b[0] == 0xB1 && g(0).b[2] == 0x41);

    /* T6: LED grid from MIDI-in CC 0..15; others ignored */
    midi_recv3(0xB0, 5, 0x0A);
    assert(leds_grid_get(5) == 0x0A);
    midi_recv3(0xB0, 5, 0x00);
    assert(leds_grid_get(5) == 0);
    midi_recv3(0xB0, 71, 64);
    assert(leds_grid_get(15) == 0);

    /* T7: key LED mirror on note on/off; unknown note is a no-op.
     * Key span is now F3..G5 = notes 53..79. */
    midi_recv3(0x90, 60, 100);
    assert(leds_key_get(60) == 1);
    midi_recv3(0x80, 60, 0);
    assert(leds_key_get(60) == 0);
    midi_recv3(0x90, 127, 100);
    assert(leds_key_get(127) == 0);

    /* T8: buttons — panel idx 32 (PLAY) -> CC 45, idx 37 (OCT-) -> CC 52;
     * press = 127, release = 0 */
    midi_cap_clear();
    const uint64_t play_btn = 1ull << (27 + 10);
    keyscan_poll(play_btn);
    assert(midi_cap_len() == 0);
    keyscan_poll(play_btn);
    assert(midi_cap_len() == 1);
    m = g(0);
    assert(m.b[0] == 0xB0 && m.b[1] == 50 && m.b[2] == 127);
    keyscan_poll(0); keyscan_poll(0);
    assert(midi_cap_len() == 2 && g(1).b[1] == 50 && g(1).b[2] == 0);

    /* T9: simultaneous key + button in one mask.
     * key idx 5 -> note 58 (53+5, A#3); button idx 13 (OCT+) -> CC 53. */
    midi_cap_clear();
    const uint64_t both = (1ull << 5) | (1ull << (27 + 13));
    keyscan_poll(both); keyscan_poll(both);
    assert(midi_cap_len() == 2);
    assert(g(0).b[0] == 0x90 && g(0).b[1] == 58);          /* key idx 5 = 58 */
    assert(g(1).b[0] == 0xB0 && g(1).b[1] == 53 && g(1).b[2] == 127); /* OCT+ */

    /* T10: the ctrl_glue facade drives the same bytes (portability boundary).
     * Fresh modules via ctrl_init, then a key + button + encoder + midi-in. */
    midi_cap_clear();
    ctrl_init();
    ctrl_frame_scan(1ull << 3); ctrl_frame_scan(1ull << 3);   /* key idx 3 = note 56 */
    ctrl_frame_scan(0); ctrl_frame_scan(0);
    ctrl_frame_enc(0, 0); ctrl_frame_enc(1, 0); ctrl_frame_enc(3, 0);
    ctrl_frame_enc(2, 0); ctrl_frame_enc(0, 0);               /* enc0 CW one detent */
    ctrl_midi_in3(0xB0, 5, 0x3C);   /* consumes: LED grid, no emit */
    assert(midi_cap_len() == 3);
    assert(g(0).b[0] == 0x90 && g(0).b[1] == 56 && g(0).b[2] == 0x64);
    assert(g(1).b[0] == 0x80 && g(1).b[1] == 56);
    assert(g(2).b[0] == 0xB0 && g(2).b[1] == 20 && g(2).b[2] == 0x41);
    assert(leds_grid_get(5) == 0x3C);

    /* T11: button-LED feedback (melodee.json outputs.btnLeds, CC 54..67).
     * CC 54 = button 0 (FX): nonzero lights it, zero clears. Out of range ignored. */
    ctrl_midi_in3(0xB0, 54, 0x7F);
    assert(leds_btn_get(0) == 1);
    ctrl_midi_in3(0xB0, 67, 1);
    assert(leds_btn_get(13) == 1);
    ctrl_midi_in3(0xB0, 54, 0);
    assert(leds_btn_get(0) == 0);
    ctrl_midi_in3(0xB0, 68, 0x7F);   /* beyond the 14-button window: no crash, no bit */
    assert(leds_btn_get(14) == 0);

    /* T12: panel label mapping (RemoteFM1 PROTOCOL section 4 order).
     * matrix 0 = OCT- , 1 = OCT+ , 2 = FX .. 13 = REC. A matrix id must emit the
     * CC of its printed label, not CC 40 + matrix id. */
    {
        midi_cap_clear();
        ctrl_init();
        keyscan_btn_map(shift1);
        ctrl_frame_scan((uint64_t)1 << (27 + 2));   /* matrix 2 = FX */
        ctrl_frame_scan((uint64_t)1 << (27 + 2));
        ctrl_frame_scan(0); ctrl_frame_scan(0);
        assert(midi_cap_len() == 2);
        assert(midi_cap_get(0).b[0] == 0xB0 && midi_cap_get(0).b[1] == 40 && midi_cap_get(0).b[2] == 127);
        keyscan_btn_map(0);                          /* back to identity for later tests */
    }

    /* T13: runtime map over SysEx (F0 7D 43 54 ...): set, get, reset. */
    {
        uint8_t req[64], ans[64];
        uint32_t n = 0, an;
        int i;
        req[n++] = 0x7D; req[n++] = 'C'; req[n++] = 'T'; req[n++] = 0x01;   /* SET */
        req[n++] = 7;
        for (i = 0; i < 7; i++) req[n++] = (uint8_t)(30 + i);              /* enc -> CC 30..36 */
        req[n++] = 14;
        for (i = 0; i < 14; i++) req[n++] = (uint8_t)(60 + i);             /* btn -> CC 60..73 */
        req[n++] = 74;                                                     /* btn LED base */
        req[n++] = 1;                                                      /* flags: mirror on, CC mode */
        assert(map_sysex(req, n) == 1);
        assert(map_encoder_cc(0) == 30 && map_encoder_cc(6) == 36);
        assert(map_btn_cc(0) == 60 && map_btn_cc(13) == 73);
        assert(map_btn_led0() == 74 && map_mirror() == 1 && map_btn_mode() == 0);

        an = map_sysex_build(ans);
        assert(ans[0] == 0x7D && ans[1] == 'C' && ans[2] == 'T' && ans[3] == 0x01);
        assert(ans[4] == 7 && ans[5] == 30 && ans[an - 1] == 1);

        /* button-as-note mode: matrix 2 (FX) must emit 0x90 60 127 */
        req[n - 1] = 3;                       /* mirror + notes */
        assert(map_sysex(req, n) == 1);
        assert(map_btn_mode() == 1);
        midi_cap_clear(); ctrl_init();        /* ctrl_init resets the map: re-apply */
        assert(map_sysex(req, n) == 1);
        ctrl_frame_scan((uint64_t)1 << (27 + 2));
        ctrl_frame_scan((uint64_t)1 << (27 + 2));
        ctrl_frame_scan(0); ctrl_frame_scan(0);
        assert(midi_cap_len() >= 1 && midi_cap_get(0).b[0] == 0x90 &&
               midi_cap_get(0).b[1] == 62 && midi_cap_get(0).b[2] == 127);

        req[3] = 0x03;                        /* RESET */
        assert(map_sysex(req, 4) == 1);
        assert(map_encoder_cc(0) == 20 && map_btn_cc(0) == 40);
        assert(map_sysex((const uint8_t *)"\x7d\x00", 2) == 0);   /* not ours */
    }


    /* T14: MASTER pot conditioning. The 14-bit CC pair exposes the ADC's low
     * three bits (CC39 = (adc&7)<<4), so a single count of noise used to move
     * the fine byte by 16 -- the 16/48 chatter heard on the panel. */
    {
        int32_t last = -1;
        /* first read always announces */
        assert(ctrl_master_accept(512, &last) == 1 && last == 512);
        /* noise below the dead band is silent, however long it goes on */
        assert(ctrl_master_accept(511, &last) == 0);
        assert(ctrl_master_accept(513, &last) == 0);
        assert(ctrl_master_accept(515, &last) == 0);      /* +3 */
        assert(ctrl_master_accept(507, &last) == 0);      /* -5 */
        assert(last == 512);                              /* nothing moved */
        /* a real turn passes the band and moves the reference */
        assert(ctrl_master_accept(519, &last) == 1 && last == 519);
        assert(ctrl_master_accept(521, &last) == 0);      /* new noise floor */
        /* exactly at the band edge it emits (>= dead band) */
        assert(ctrl_master_accept(519 + CTRL_MASTER_DEADBAND, &last) == 1);
        /* a conversion timeout never emits */
        assert(ctrl_master_accept(-1, &last) == 0);
        /* why the dead band has to cover the low 3 bits: CC39 = (adc & 7) << 4,
         * so 512 -> 0 but 514 -> 32 in the "fine" byte */
        assert(((512 & 7) << 4) == 0);
        assert(((514 & 7) << 4) == 32);
    }

    /* T15: label id != matrix id. This is the class of bug that made long-press
     * SAVE (label 7) open the diag page from GLO: fm1_in.buttons carries MATRIX
     * ids, so a label constant must go through panel.btn[] first. The panel table
     * is device-only, so pin the invariant that makes it matter: the default
     * calibration is not the identity map. */
    {
        /* PANEL_DEFAULT.btn[] = {2,3,4,5,6,7,8,9,10,11,12,13,0,1} */
        static const int def_btn[14] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1};
        unsigned i, differs = 0;
        for (i = 0; i < 14; i++)
            if (def_btn[i] != (int)i)
                differs++;
        assert(differs == 14);          /* every label maps elsewhere: never assume identity */
        assert(def_btn[7] == 9);        /* SAVE sits on matrix 9, GLO on 7 */
    }

    printf("ALL HOST TESTS PASSED\n");
    return 0;
}
