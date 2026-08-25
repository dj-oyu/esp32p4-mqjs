/*
 * esp32p4-mqjs — Tab5 battery fuel gauge (pure math, no IDF, host-testable).
 * Copyright (C) 2026 dj-oyu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * WHAT THIS IS. Voltage -> state-of-charge for the Tab5's 2S NP-F550 pack,
 * good enough to drive a battery icon, a "残り 2h10m" line and — the part
 * that actually matters — an over-discharge shutdown that fires before the
 * pack's own protection FET latches at 6.0 V (docs/battery-power-design.md
 * §1: recovering from that needs the battery physically removed).
 *
 * WHY NOT A PLAIN LINEAR MAP. M5Unified maps 3.30-4.15 V/cell linearly and
 * ESPHome's template maps 6.0-8.23 V pack; both read the terminal voltage,
 * which on this device swings ~300 mV as the camera, Wi-Fi and backlight
 * come and go. A percentage that drops 8 points when a scan starts is not a
 * percentage. So three layers, in order of how much they can be trusted:
 *
 *   1. IR compensation. The INA226 gives voltage AND current from the same
 *      node, so the pack's internal resistance is observable: V = OCV + I*R
 *      (I positive = into the battery). Recursive least squares learns R
 *      from the load steps the device makes anyway, and OCV = V - I*R is
 *      then a load-independent voltage. THIS is what the protection ladder
 *      thresholds on — never the raw terminal voltage.
 *   2. Coulomb counting. Integrating current is exact over minutes and
 *      drifts over hours; it carries SoC between anchors and is what makes
 *      the display monotone under a changing load.
 *   3. Anchors. Three moments where the true SoC is known and the integrator
 *      is reset: at rest (OCV table), at charge termination (100% + capacity
 *      learning), and at boot from a saved value.
 *
 * WHAT IT IS NOT. Not a coulomb counter IC replacement: no temperature
 * compensation (the pack has no thermistor we can reach), no cycle-aged OCV
 * curve, no per-cell anything (the INA226 sees the pack, not the two cells).
 * Accuracy target is "the icon is honest and the shutdown is early", not 1%.
 *
 * FLOAT. ESP32-P4 is RV32IMAFC — single-precision FP is a hardware
 * instruction, and the RLS update is 20 flops once per second. Fixed point
 * would buy nothing here and cost the reader.
 *
 * THREADING. None. Every function is a pure transform of the struct the
 * caller owns; pwr_tab5.c calls them from its one owner task.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for pwr_gauge_step(): what the charge hardware is doing. */
#define PWR_GAUGE_F_CHARGE_EN (1u << 0) /* CHG_EN asserted by policy   */
#define PWR_GAUGE_F_USB       (1u << 1) /* charger present (expander)  */

/* Why the integrator was reset on this step (0 = it was not). */
typedef enum {
    PWR_ANCHOR_NONE = 0,
    PWR_ANCHOR_REST,  /* current ~0 long enough: OCV table is truth     */
    PWR_ANCHOR_FULL,  /* charge terminated: 100% (and capacity learned) */
} pwr_anchor_t;

/* Nominal pack: NP-F550, 7.4 V / 2000 mAh (docs.m5stack.com/en/core/Tab5). */
#define PWR_CAP_NOMINAL_MAH 2000
/* Below this the node cannot be a 2S pack (protection would have latched),
   so it is USB-only operation with no battery installed. */
#define PWR_NO_BATT_MV 5500

typedef struct {
    /* ---- last sample, as handed in ---- */
    int32_t mv;         /* pack terminal voltage, mV                     */
    int32_t ma_x10;     /* pack current, 0.1 mA, + = into the battery    */

    /* ---- estimates ---- */
    float r_ohm;        /* learned pack resistance                       */
    float ocv_v;        /* IR-compensated open-circuit voltage           */
    float mah;          /* remaining charge                              */
    float cap_mah;      /* full-charge capacity (learned, else nominal)  */
    float ma_avg;       /* EMA of current, mA, for the time estimate     */

    int32_t soc;        /* true SoC 0..100, -1 = not established yet     */
    int32_t disp;       /* smoothed SoC for display, -1 = unknown        */
    int32_t eta_min;    /* minutes to empty (discharge) / full (charge)  */
    bool    no_batt;    /* running on USB with no pack installed         */

    /* ---- RLS state (theta = [ocv_v, r_ohm]) ---- */
    float p00, p01, p10, p11;
    float th0, th1;
    int32_t excite_n;   /* samples with real current excitation seen     */

    /* ---- anchor bookkeeping ---- */
    int32_t rest_n;     /* consecutive near-zero-current samples         */
    int32_t full_n;     /* consecutive charge-taper samples              */
    float   mah_at_anchor;   /* integrator value when the last anchor hit */
    int32_t soc_at_anchor;   /* SoC that anchor established, -1 = none    */
    uint32_t disp_hold_ms;   /* time since the display last stepped       */
} pwr_gauge_t;

/*
 * Start the gauge. `cap_mah` is the learned capacity from NVS (0 = use the
 * nominal), `saved_soc` the SoC persisted at the last clean shutdown
 * (-1 = none). A saved SoC is a weak anchor: it seeds the integrator so the
 * first minutes have a number, and the first rest or full anchor overrides
 * it. It deliberately does NOT set soc_at_anchor — a saved value must never
 * become the reference leg of a capacity-learning interval.
 */
void pwr_gauge_init(pwr_gauge_t *g, int32_t cap_mah, int32_t saved_soc);

/*
 * Fold one sample in. `dt_ms` is the interval since the previous call
 * (clamped internally: a scheduling hiccup must not integrate an hour of
 * charge). Returns the anchor that fired, if any.
 */
pwr_anchor_t pwr_gauge_step(pwr_gauge_t *g, int32_t mv, int32_t ma_x10,
                            uint32_t dt_ms, uint32_t flags);

/* The OCV -> SoC curve, exposed for the host test and the trace fitter.
   Input is PACK millivolts (2S), output 0..100. */
int32_t pwr_gauge_ocv_to_soc(int32_t ocv_mv);
/* The inverse, for seeding the integrator from a SoC. */
int32_t pwr_gauge_soc_to_ocv(int32_t soc);

#ifdef __cplusplus
}
#endif
