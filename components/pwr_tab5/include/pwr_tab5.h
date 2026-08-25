/*
 * esp32p4-mqjs — M5Stack Tab5 battery: charge control, fuel gauge, protection.
 * Copyright (C) 2026 dj-oyu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * THE HARDWARE (docs/battery-power-design.md has the sources).
 *
 *   pack     NP-F550-compatible, 2S, 7.4 V / 2000 mAh, removable
 *   monitor  INA226 at 0x41 on the internal I2C bus, 5 mOhm shunt
 *   charger  IP2326, gated by the PI4IOE5V6408 expander at 0x44:
 *              P7 CHG_EN   1 = charging allowed
 *              P5 QC_EN    0 = quick charge allowed (active low)
 *              P4 PWROFF   1->0 pulse asks the hardware to cut power
 *              P3 USB5V_EN USB-A host power
 *              P6 (input)  charger / USB-C status
 *            EXT 5V is P2 of the OTHER expander, at 0x43.
 *
 * TWO FACTS ABOUT THIS DEVICE SHAPE EVERYTHING BELOW.
 *
 *   1. The Tab5 charges ONLY while it is powered on and firmware has
 *      asserted CHG_EN. M5's own docs say so, and the factory firmware sets
 *      the bit in its HAL init. A shutdown is therefore not a safe default:
 *      turning off with a charger attached stops the charging too.
 *   2. Below 6.0 V the pack's protection latches and the documented recovery
 *      is to REMOVE THE BATTERY and reset the device. Nothing else in this
 *      firmware has a failure mode that needs a screwdriver, so getting the
 *      device to shut itself down before that point is the single most
 *      valuable thing this component does.
 *
 * OWNERSHIP. One task samples the INA226, runs the gauge, applies policy and
 * owns every write to the 0x44 expander (read-modify-write under a mutex).
 * Callers never touch I2C; they call the functions here, which are safe from
 * any task. This is the same single-owner shape microlink's lifecycle task
 * ended up with, for the same reason: the alternative is two writers racing
 * on one output register.
 *
 * With CONFIG_MQJS_TAB5_BATTERY=n every entry point is a no-op inline stub,
 * so main/ needs no ifdefs and Stamp builds carry none of it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#else
/* The PC runner (tools/run_pc.c) compiles the runtime and therefore this
   header. It gets the types and the stubs; there is no battery to read. */
#undef CONFIG_MQJS_TAB5_BATTERY
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWR_BATT_UNKNOWN = 0,
    PWR_BATT_NONE,        /* running on USB, no pack installed        */
    PWR_BATT_DISCHARGING,
    PWR_BATT_CHARGING,
    PWR_BATT_FULL,        /* charge terminated                        */
    PWR_BATT_LIMITED,     /* the charge ceiling is holding CHG_EN off */
} pwr_batt_state_t;

/* Load-shed / protection tiers. The ladder runs on a charger as well --
   shedding load so the charger wins against the load is the right answer to
   "nearly empty but plugged in". Only the last rung is charger-gated: the
   Tab5 charges only while powered on (fact 1 above), so a shutdown while
   charging would strand the pack instead of saving it. */
typedef enum {
    PWR_TIER_OK = 0,
    PWR_TIER_LOW,      /* notify, shorten the screen timeouts        */
    PWR_TIER_SHED,     /* external 5 V rails off                     */
    PWR_TIER_CRITICAL, /* screen down hard, apps warned              */
    PWR_TIER_SHUTDOWN, /* countdown is running                       */
} pwr_tier_t;

typedef struct {
    int32_t mv;        /* pack terminal voltage, mV                   */
    int32_t ma;        /* pack current, mA, + = into the battery      */
    int32_t ocv_mv;    /* IR-compensated open-circuit voltage, mV     */
    int32_t mohm;      /* learned pack resistance, mOhm               */
    int32_t pct;       /* displayed charge 0..100, -1 = unknown       */
    int32_t mah;       /* remaining charge estimate                   */
    int32_t cap_mah;   /* full-charge capacity (learned)              */
    int32_t eta_min;   /* minutes to empty or to full, -1 = unknown   */
    uint8_t state;     /* pwr_batt_state_t                            */
    uint8_t tier;      /* pwr_tier_t                                  */
    uint8_t limit_pct; /* charge ceiling policy, 100 = charge to full */
    uint8_t in_sta;    /* raw 0x44 input register (P6 = charger bit)  */
    bool    usb;       /* charger present                             */
    bool    charge_en; /* CHG_EN as this component last drove it      */
    bool    qc_en;     /* QC_EN (true = quick charge allowed)         */
    bool    present;   /* the INA226 answered                         */
    uint32_t samples;  /* samples taken since boot                    */
} pwr_batt_t;

#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_BATTERY

/*
 * Assert QC_EN then CHG_EN on the expander the caller already has a bus for.
 * Called from board_tab5_power_init() during early boot, long before the UI
 * (and therefore the shared I2C bus) exists — charging must not wait for a
 * display. Order and the 50 ms gap mirror the factory firmware's HAL init.
 */
void pwr_tab5_early_charge_on(void *i2c_bus_handle);

/* Start the owner task. Needs ui_tab5_i2c_bus(), so call it from the UI-ready
   hook, not from app_main. Idempotent; logs and gives up if the INA226 does
   not answer (every getter then reports state = UNKNOWN). */
void pwr_tab5_start(void);

/* Latest snapshot. Returns false (and zeroes *out) before the first sample. */
bool pwr_tab5_get(pwr_batt_t *out);

/* Charge current selector, exactly the three the hardware offers:
   0 = off, 500 = CHG_EN with QC off, 1000 = CHG_EN with QC on.
   This is the manual override; the ceiling policy may still hold CHG_EN
   off above the limit. */
void pwr_tab5_set_charge_ma(int ma);
int  pwr_tab5_charge_ma(void);

/* Charge ceiling for pack longevity: 100 (charge to full), 90 or 80.
   Stopping at the ceiling resumes at ceiling-5 with a 120 s minimum dwell,
   so the charger is never toggled at a rate it could notice. Persisted. */
void pwr_tab5_set_limit(int pct);
int  pwr_tab5_limit(void);
/* "Full charge just this once" — ignores the ceiling until the next charge
   termination or until the charger is unplugged. */
void pwr_tab5_full_charge_once(void);

/* External 5 V rails (also what the shed tier switches off). */
void pwr_tab5_set_usb5v(bool on);   /* USB-A host power, 0x44 P3    */
void pwr_tab5_set_ext5v(bool on);   /* M5-Bus / Grove 5 V, 0x43 P2  */

/* Ask the hardware to cut power (0x44 P4 pulse). Does not save anything by
   itself — go through the shutdown callback if apps need to persist. */
void pwr_tab5_power_off(void);

/* The current tier, for the screen power-state machine (mqjs_power polls
   this to shorten its timeouts on a low battery). Safe from any task. */
int pwr_tab5_tier(void);

/*
 * Last words before an over-discharge shutdown. Called once from the owner
 * task when the ladder reaches PWR_TIER_SHUTDOWN, with the grace period in
 * seconds. The callback should hand the news to whatever needs to persist
 * (the JS apps' onStop, the LP black box) and call pwr_tab5_shutdown_ack()
 * when done; power is cut on the ack or when the grace expires, whichever
 * comes first. A charger appearing during the countdown aborts it.
 */
void pwr_tab5_set_shutdown_cb(void (*fn)(const char *reason, int grace_s));
void pwr_tab5_shutdown_ack(void);

/* One-line user-facing notices ("バッテリー残り 15%" ...). Wired to the
   status bar in main; NULL disables. */
void pwr_tab5_set_notify_cb(void (*fn)(const char *text));

/* Called once per sample (1 Hz) from the owner task with the fresh snapshot.
   main uses it to feed the status bar. Runs on the battery task: keep it
   short, and do not call back into this component. */
void pwr_tab5_set_sample_cb(void (*fn)(const pwr_batt_t *b));

/*
 * Current-sign override for bring-up. The INA226's orientation on this board
 * is not documented anywhere we can check, and the factory UI infers charging
 * from "current >= 0". Default +1 follows that; -1 flips it. Also settable at
 * build time (CONFIG_MQJS_TAB5_BATTERY_INVERT_CURRENT) so a verified board
 * needs no runtime call.
 */
void pwr_tab5_set_current_sign(int sign);

#else /* no battery support in this build */

static inline void pwr_tab5_early_charge_on(void *b) { (void)b; }
static inline void pwr_tab5_start(void) {}
static inline bool pwr_tab5_get(pwr_batt_t *out)
{
    if (out) {
        pwr_batt_t z = { 0 };
        z.pct = -1;
        z.eta_min = -1;
        *out = z;
    }
    return false;
}
static inline void pwr_tab5_set_charge_ma(int ma) { (void)ma; }
static inline int  pwr_tab5_charge_ma(void) { return 0; }
static inline void pwr_tab5_set_limit(int pct) { (void)pct; }
static inline int  pwr_tab5_limit(void) { return 100; }
static inline void pwr_tab5_full_charge_once(void) {}
static inline void pwr_tab5_set_usb5v(bool on) { (void)on; }
static inline void pwr_tab5_set_ext5v(bool on) { (void)on; }
static inline void pwr_tab5_power_off(void) {}
static inline int  pwr_tab5_tier(void) { return 0; }
static inline void pwr_tab5_set_shutdown_cb(void (*fn)(const char *, int))
{
    (void)fn;
}
static inline void pwr_tab5_shutdown_ack(void) {}
static inline void pwr_tab5_set_notify_cb(void (*fn)(const char *)) { (void)fn; }
static inline void pwr_tab5_set_sample_cb(void (*fn)(const pwr_batt_t *))
{
    (void)fn;
}
static inline void pwr_tab5_set_current_sign(int sign) { (void)sign; }

#endif /* battery support */

#ifdef __cplusplus
}
#endif
