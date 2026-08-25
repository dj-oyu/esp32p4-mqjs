/*
 * esp32p4-mqjs — Tab5 battery fuel gauge (pure math). See pwr_gauge.h.
 * Copyright (C) 2026 dj-oyu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pwr_gauge.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The OCV curve                                                       */
/* ------------------------------------------------------------------ */
/*
 * PROVISIONAL. These are the resting terminal voltages of a healthy 2S
 * Li-ion pack, doubled from the usual per-cell curve and pulled down at the
 * bottom to match what M5 documents for this device: full 8.23 V down to a
 * 6.0 V shutdown threshold, about 6 hours. They are NOT measured on an
 * NP-F550.
 *
 * TO REPLACE THEM WITH REAL ONES: run examples/battery_trace.js on a full
 * pack until the device shuts itself down (~6 h, hands-off, publishes a
 * sample every 10 s over MQTT), then feed the log to tools/battery_fit.py,
 * which prints this table with the measured numbers.
 * docs/battery-power-design.md has the procedure.
 *
 * Two things make the provisional curve survivable in the meantime: the
 * protection ladder thresholds on OCV in millivolts, not on the percentage
 * derived from this table, and the full-charge anchor re-learns capacity
 * from coulombs regardless of what the curve claims.
 */
static const struct { int16_t mv; int8_t soc; } s_ocv[] = {
    { 8300, 100 }, { 8200, 95 }, { 8100, 90 }, { 7950, 80 }, { 7820, 70 },
    { 7700,  60 }, { 7590, 50 }, { 7490, 40 }, { 7390, 30 }, { 7260, 20 },
    { 7120,  12 }, { 6950,  7 }, { 6700,  3 }, { 6300,  0 },
};
#define OCV_N ((int)(sizeof s_ocv / sizeof s_ocv[0]))

int32_t pwr_gauge_ocv_to_soc(int32_t ocv_mv)
{
    if (ocv_mv >= s_ocv[0].mv)
        return 100;
    for (int i = 1; i < OCV_N; i++) {
        if (ocv_mv >= s_ocv[i].mv) {
            int32_t dv = s_ocv[i - 1].mv - s_ocv[i].mv;
            int32_t ds = s_ocv[i - 1].soc - s_ocv[i].soc;
            return s_ocv[i].soc + ((ocv_mv - s_ocv[i].mv) * ds + dv / 2) / dv;
        }
    }
    return 0;
}

int32_t pwr_gauge_soc_to_ocv(int32_t soc)
{
    if (soc >= 100)
        return s_ocv[0].mv;
    if (soc <= 0)
        return s_ocv[OCV_N - 1].mv;
    for (int i = 1; i < OCV_N; i++) {
        if (soc >= s_ocv[i].soc) {
            int32_t ds = s_ocv[i - 1].soc - s_ocv[i].soc;
            int32_t dv = s_ocv[i - 1].mv - s_ocv[i].mv;
            return s_ocv[i].mv + ((soc - s_ocv[i].soc) * dv + ds / 2) / ds;
        }
    }
    return s_ocv[OCV_N - 1].mv;
}

/* ------------------------------------------------------------------ */
/* Tuning                                                              */
/* ------------------------------------------------------------------ */
#define RLS_LAMBDA    0.998f  /* ~500 s memory at 1 Hz                  */
#define R_DEFAULT     0.20f   /* NP-F550-class 2S pack, before learning */
#define R_MIN         0.05f
#define R_MAX         0.80f
#define OCV_MIN_V     5.5f
#define OCV_MAX_V     8.7f
#define P_MAX         10.0f   /* covariance bound: no runaway gain      */
#define EXCITE_A      0.05f   /* current step that counts as excitation */
#define EXCITE_TRUST  20      /* steps before the learned R is used     */

#define REST_MA_X10   500     /* under 50 mA in either direction = rest */
#define REST_SAMPLES  60
#define FULL_MV       8100    /* CV plateau reached                     */
#define FULL_MA_X10   1000    /* taper below 100 mA (about C/20)        */
#define FULL_SAMPLES  60
#define LEARN_MIN_DSOC 40     /* shorter intervals learn only noise     */
#define CAP_MIN_MAH   1000.0f
#define CAP_MAX_MAH   3200.0f

#define DISP_STEP_MS  3000    /* at most 1 point per 3 s                */
#define DISP_SNAP     5       /* ... unless we are more than 5 out      */
#define DT_MAX_MS     10000u  /* a scheduling hiccup integrates no more */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pwr_gauge_init(pwr_gauge_t *g, int32_t cap_mah, int32_t saved_soc)
{
    memset(g, 0, sizeof *g);
    g->cap_mah = (cap_mah >= (int32_t)CAP_MIN_MAH &&
                  cap_mah <= (int32_t)CAP_MAX_MAH)
                     ? (float)cap_mah
                     : (float)PWR_CAP_NOMINAL_MAH;
    g->r_ohm = R_DEFAULT;
    g->th0 = 7.6f;          /* mid-pack; the first samples correct it   */
    g->th1 = R_DEFAULT;
    g->p00 = 1.0f;
    g->p11 = 1.0f;
    g->soc = -1;
    g->disp = -1;
    g->eta_min = -1;
    g->soc_at_anchor = -1;
    if (saved_soc >= 0 && saved_soc <= 100) {
        g->soc = saved_soc;
        g->disp = saved_soc;
        g->mah = g->cap_mah * (float)saved_soc / 100.0f;
    }
}

/* One recursive-least-squares update of theta = [OCV, R] against
   V = OCV + I*R. Guarded twice, because the regressor [1, I] is a fixed
   direction whenever the load is constant, and plain RLS-with-forgetting
   would then inflate P without bound in the unexcited direction:
     - the covariance diagonal is capped (bounded gain), and
     - theta is clamped to the physically possible range afterwards.
   Both are cheap; neither can turn a good sample into a bad R. */
static void rls_update(pwr_gauge_t *g, float v, float i_a)
{
    const float x0 = 1.0f, x1 = i_a;

    float px0 = g->p00 * x0 + g->p01 * x1;
    float px1 = g->p10 * x0 + g->p11 * x1;
    float denom = RLS_LAMBDA + x0 * px0 + x1 * px1;
    if (!(denom > 1e-6f))
        return;
    float k0 = px0 / denom, k1 = px1 / denom;

    float err = v - (g->th0 * x0 + g->th1 * x1);
    g->th0 += k0 * err;
    g->th1 += k1 * err;

    /* xT * P, as a row vector */
    float xp0 = x0 * g->p00 + x1 * g->p10;
    float xp1 = x0 * g->p01 + x1 * g->p11;
    g->p00 = (g->p00 - k0 * xp0) / RLS_LAMBDA;
    g->p01 = (g->p01 - k0 * xp1) / RLS_LAMBDA;
    g->p10 = (g->p10 - k1 * xp0) / RLS_LAMBDA;
    g->p11 = (g->p11 - k1 * xp1) / RLS_LAMBDA;
    g->p00 = clampf(g->p00, 1e-6f, P_MAX);
    g->p11 = clampf(g->p11, 1e-6f, P_MAX);

    g->th0 = clampf(g->th0, OCV_MIN_V, OCV_MAX_V);
    g->th1 = clampf(g->th1, R_MIN, R_MAX);
}

pwr_anchor_t pwr_gauge_step(pwr_gauge_t *g, int32_t mv, int32_t ma_x10,
                            uint32_t dt_ms, uint32_t flags)
{
    if (dt_ms > DT_MAX_MS)
        dt_ms = DT_MAX_MS;
    g->mv = mv;
    g->ma_x10 = ma_x10;

    /* No pack installed (USB-only operation): report nothing rather than a
       number derived from the buck output. Every consumer treats soc < 0 as
       unknown, and the protection ladder never fires. */
    if (mv < PWR_NO_BATT_MV) {
        g->no_batt = true;
        g->soc = -1;
        g->disp = -1;
        g->eta_min = -1;
        g->ocv_v = (float)mv / 1000.0f;
        return PWR_ANCHOR_NONE;
    }
    g->no_batt = false;

    const float v = (float)mv / 1000.0f;
    const float ma = (float)ma_x10 / 10.0f;
    const float i_a = ma / 1000.0f;

    /* --- 1. IR compensation ------------------------------------- */
    float prev_i = g->ma_avg / 1000.0f;
    if (fabsf(i_a - prev_i) > EXCITE_A && g->excite_n < 1000000)
        g->excite_n++;
    rls_update(g, v, i_a);
    if (g->excite_n >= EXCITE_TRUST)
        g->r_ohm = g->th1;

    /* Instantaneous, not theta[0]: the protection ladder must react to a
       sagging pack within a sample, and theta[0] is deliberately slow. A
       short EMA takes the ADC noise off without adding lag that matters. */
    float ocv_inst = clampf(v - i_a * g->r_ohm, OCV_MIN_V, OCV_MAX_V);
    if (g->ocv_v <= 0.0f) {
        g->ocv_v = ocv_inst;
    } else {
        float a = (float)dt_ms / (10000.0f + (float)dt_ms);
        g->ocv_v += (ocv_inst - g->ocv_v) * a;
    }

    /* --- 2. coulomb counting ------------------------------------ */
    g->mah += ma * (float)dt_ms / 3600000.0f;
    g->mah = clampf(g->mah, 0.0f, g->cap_mah * 1.2f);

    /* current EMA for the time estimate (tau about 5 min) */
    {
        float a = (float)dt_ms / (300000.0f + (float)dt_ms);
        g->ma_avg += (ma - g->ma_avg) * a;
    }

    /* --- 3. anchors --------------------------------------------- */
    pwr_anchor_t anchor = PWR_ANCHOR_NONE;

    if (ma_x10 > -REST_MA_X10 && ma_x10 < REST_MA_X10)
        g->rest_n++;
    else
        g->rest_n = 0;

    bool tapering = (flags & PWR_GAUGE_F_CHARGE_EN) && mv >= FULL_MV &&
                    ma_x10 > 0 && ma_x10 < FULL_MA_X10;
    if (tapering)
        g->full_n++;
    else
        g->full_n = 0;

    if (g->full_n >= FULL_SAMPLES) {
        /* Charge terminated. This is the one moment the absolute SoC is
           known without trusting any curve, so it is also where capacity
           is learned: coulombs counted since the last anchor, divided by
           the SoC span they covered. */
        if (g->soc_at_anchor >= 0 &&
            (100 - g->soc_at_anchor) >= LEARN_MIN_DSOC) {
            float dmah = g->mah - g->mah_at_anchor;
            float dsoc = (float)(100 - g->soc_at_anchor);
            if (dmah > 0.0f) {
                float cap = dmah * 100.0f / dsoc;
                if (cap >= CAP_MIN_MAH && cap <= CAP_MAX_MAH)
                    g->cap_mah += (cap - g->cap_mah) * 0.5f;
            }
        }
        g->soc = 100;
        g->mah = g->cap_mah;
        g->soc_at_anchor = 100;
        g->mah_at_anchor = g->mah;
        g->full_n = 0;
        anchor = PWR_ANCHOR_FULL;
    } else if (g->rest_n >= REST_SAMPLES) {
        int32_t s = pwr_gauge_ocv_to_soc((int32_t)(g->ocv_v * 1000.0f));
        g->soc = s;
        g->mah = g->cap_mah * (float)s / 100.0f;
        g->soc_at_anchor = s;
        g->mah_at_anchor = g->mah;
        g->rest_n = 0;
        anchor = PWR_ANCHOR_REST;
    } else if (g->soc >= 0) {
        /* between anchors the integrator carries it */
        int32_t s = (int32_t)(g->mah * 100.0f / g->cap_mah + 0.5f);
        g->soc = s < 0 ? 0 : (s > 100 ? 100 : s);
    } else {
        /* Cold start with no saved value and no anchor yet: the OCV table
           is all we have. It is wrong under load, which is why it is only
           the seed and the first rest anchor replaces it. */
        int32_t s = pwr_gauge_ocv_to_soc((int32_t)(g->ocv_v * 1000.0f));
        g->soc = s;
        g->mah = g->cap_mah * (float)s / 100.0f;
    }

    /* --- 4. display smoothing ----------------------------------- */
    if (g->disp < 0 || anchor != PWR_ANCHOR_NONE) {
        g->disp = g->soc;
        g->disp_hold_ms = 0;
    } else {
        int32_t d = g->soc - g->disp;
        if (d > DISP_SNAP || d < -DISP_SNAP) {
            g->disp = g->soc; /* too far out to walk there politely */
            g->disp_hold_ms = 0;
        } else if (d != 0) {
            g->disp_hold_ms += dt_ms;
            if (g->disp_hold_ms >= DISP_STEP_MS) {
                g->disp_hold_ms = 0;
                /* Direction gate: a percentage that goes UP while the pack
                   is discharging reads as a broken gauge even when the
                   estimator has a good reason. Anchors are the exception,
                   handled above. */
                bool charging = ma_x10 > 100;
                if (d > 0 && charging)
                    g->disp++;
                else if (d < 0 && !charging)
                    g->disp--;
            }
        }
    }

    /* --- 5. time estimate --------------------------------------- */
    if (g->ma_avg < -20.0f)
        g->eta_min = (int32_t)(g->mah * 60.0f / -g->ma_avg);
    else if (g->ma_avg > 20.0f)
        g->eta_min = (int32_t)((g->cap_mah - g->mah) * 60.0f / g->ma_avg);
    else
        g->eta_min = -1;
    if (g->eta_min > 5999)
        g->eta_min = 5999;

    return anchor;
}
