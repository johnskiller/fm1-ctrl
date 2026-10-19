#include "map.h"

/* runtime map: starts as the compiled-in default (map_data.c, generated from
 * docs/maps/<emu>.json by tools/map2c.py) and can be replaced over SysEx at runtime.
 * Nothing here touches flash: a power cycle returns to the compiled default. */

extern const uint8_t     MAP_ENC_CC[MAP_NENC];
extern const uint8_t     MAP_BTN_CC[MAP_NBTN];
extern const map_led_cfg_t MAP_LED;
extern const uint8_t     MAP_BTN_LED0;
extern const uint8_t     MAP_MIRROR;

static uint8_t rt_enc[MAP_NENC];
static uint8_t rt_btn[MAP_NBTN];
static uint8_t rt_btn_led0;
static uint8_t rt_mirror;
static uint8_t rt_btn_mode;      /* 0 = buttons send CC, 1 = buttons send notes */

void map_reset(void)
{
    int i;
    for (i = 0; i < MAP_NENC; i++) rt_enc[i] = MAP_ENC_CC[i];
    for (i = 0; i < MAP_NBTN; i++) rt_btn[i] = MAP_BTN_CC[i];
    rt_btn_led0 = MAP_BTN_LED0;
    rt_mirror   = MAP_MIRROR;
    rt_btn_mode = 0;
}

uint8_t map_encoder_cc(int idx) { return (idx >= 0 && idx < MAP_NENC) ? rt_enc[idx] : 0; }
uint8_t map_btn_cc(int i)       { return (i >= 0 && i < MAP_NBTN) ? rt_btn[i] : 0; }
map_led_cfg_t map_led(void)     { return MAP_LED; }
uint8_t map_btn_led0(void)      { return rt_btn_led0; }
uint8_t map_mirror(void)        { return rt_mirror; }
uint8_t map_btn_mode(void)      { return rt_btn_mode; }

/* ---- SysEx: F0 7D 43 54 <cmd> <body...> F7  ("CT" = ctrl; 0x7D = non-commercial)
 * The frame handed in excludes F0/F7.
 *
 *   0x01 SET   nenc, enc_cc[nenc], nbtn, btn_cc[nbtn], btn_led0, flags
 *              flags bit0: key-LED mirror, bit1: buttons send notes instead of CC
 *   0x02 GET   -> the same body as SET, echoing the current map
 *   0x03 RESET -> compiled default
 *
 * returns 1 = ours and applied, 0 = not ours, -1 = ours but malformed. */
#define SX_HDR0 0x7D
#define SX_HDR1 'C'
#define SX_HDR2 'T'
#define SX_SET 0x01
#define SX_GET 0x02
#define SX_RESET 0x03

int map_sysex_is_ours(const uint8_t *p, uint32_t n)
{
    return n >= 4u && p[0] == SX_HDR0 && p[1] == SX_HDR1 && p[2] == SX_HDR2;
}

int map_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t i, k;
    if (!map_sysex_is_ours(p, n))
        return 0;
    if (p[3] == SX_RESET) {
        map_reset();
        return 1;
    }
    if (p[3] == SX_GET) {
        return 1;                       /* the caller answers map_sysex_build() */
    }
    if (p[3] != SX_SET || n < 5u)
        return -1;
    k = 4u;
    if (k >= n) return -1;
    i = p[k++];                         /* nenc */
    if (i != MAP_NENC || k + i > n) return -1;
    { uint8_t t[MAP_NENC]; uint32_t j;
      for (j = 0; j < i; j++) t[j] = p[k + j];
      for (j = 0; j < i; j++) rt_enc[j] = t[j];
      k += i; }
    if (k >= n) return -1;
    i = p[k++];                         /* nbtn */
    if (i != MAP_NBTN || k + i > n) return -1;
    { uint8_t t[MAP_NBTN]; uint32_t j;
      for (j = 0; j < i; j++) t[j] = p[k + j];
      for (j = 0; j < i; j++) rt_btn[j] = t[j];
      k += i; }
    if (k + 2u > n) return -1;
    rt_btn_led0 = p[k++];
    rt_mirror   = p[k] & 1u;
    rt_btn_mode = (p[k] >> 1) & 1u;
    return 1;
}

/* build the GET answer body (without F0/F7); returns the length */
uint32_t map_sysex_build(uint8_t *out)
{
    uint32_t k = 0;
    int i;
    out[k++] = SX_HDR0; out[k++] = SX_HDR1; out[k++] = SX_HDR2; out[k++] = SX_SET;
    out[k++] = MAP_NENC;
    for (i = 0; i < MAP_NENC; i++) out[k++] = rt_enc[i];
    out[k++] = MAP_NBTN;
    for (i = 0; i < MAP_NBTN; i++) out[k++] = rt_btn[i];
    out[k++] = rt_btn_led0;
    out[k++] = (uint8_t)((rt_mirror ? 1u : 0u) | (rt_btn_mode << 1));
    return k;
}
