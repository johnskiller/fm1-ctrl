/* SPDX-License-Identifier: MPL-2.0
 * fm1-ctrl's half of the contract with the platform (device/unity.c,
 * host/plat_host.c). The platform owns the panel; this app only reports. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FM1-CTRL"
#define APP_SPLASH_RGB RGB(24, 168, 200)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h) */
#define APP_BLE_AT_BOOT (!ctrl_ble_off())
int ctrl_ble_off(void);

/* the store: our settings, the platform's panel calibration, Bluetooth's pairing */
enum { OBJ_CTRL, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
