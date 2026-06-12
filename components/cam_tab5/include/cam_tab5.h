/* Tab5 camera barcode scanner (SC2356 over MIPI-CSI via esp_video).
 *
 * One scan at a time: a low-priority task captures RGB565 720p frames
 * and runs the EAN-13 scanline decoder over a grid of rows + columns
 * until a code (optionally prefix-filtered, e.g. "97" for ISBN
 * Bookland 978/979) is found or the timeout expires. The callback
 * fires exactly once, from the scan task — marshal to your own
 * context (mqjs posts an event).
 *
 * The header stays IDF-type-free so non-camera builds (Stamp, PC) can
 * include it; everything is stubbed when CONFIG_MQJS_CAMERA is off.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cam_tab5_cb_t)(const char *code13_or_null, void *arg);

/* SCCB rides the touch controller's I2C bus (port 1, SDA31/SCL32) —
 * pass ui_tab5's bus handle once at boot, before the first scan. */
void cam_tab5_set_i2c(void *i2c_master_bus_handle);

/* false = busy / camera unavailable (see cam_tab5_status()). prefix
 * may be NULL (accept any EAN-13). */
bool cam_tab5_scan_start(uint32_t timeout_ms, const char *prefix,
                         cam_tab5_cb_t cb, void *arg);

void cam_tab5_cancel(void);

/* last init/scan state, for remote diagnosis (no serial on Tab5) */
const char *cam_tab5_status(void);

#ifdef __cplusplus
}
#endif
