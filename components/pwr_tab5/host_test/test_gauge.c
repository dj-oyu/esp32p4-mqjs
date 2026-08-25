/*
 * esp32p4-mqjs — host test for the Tab5 fuel gauge (pwr_gauge.c).
 * Copyright (C) 2026 dj-oyu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs on the build machine, no IDF, no device:
 *
 *   wsl gcc -O2 -o /tmp/test_gauge components/pwr_tab5/host_test/test_gauge.c \
 *           components/pwr_tab5/pwr_gauge.c -I components/pwr_tab5 -lm && /tmp/test_gauge
 *
 * The simulated pack is the gauge's own OCV curve plus a series resistance,
 * which is circular for the curve itself (the test cannot prove the curve is
 * right — only the trace from a real pack can) but is exactly right for what
 * is under test here: does the estimator recover R, does OCV stay put while
 * the terminal voltage swings, do the anchors fire, does the display stay
 * honest. Every check below fails if you break the thing it names — see
 * docs/battery-power-design.md for the sabotage log.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pwr_gauge.h"

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __func__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* ---- a pack simulator ------------------------------------------- */
typedef struct {
    double mah;      /* charge in the pack   */
    double cap;      /* true capacity        */
    double r;        /* true series R, ohms  */
} sim_t;

static void sim_init(sim_t *s, double cap, double r, double soc)
{
    s->cap = cap;
    s->r = r;
    s->mah = cap * soc / 100.0;
}

/* Advance the pack by dt seconds at `ma` (+ = charging) and return the
   terminal voltage in mV. */
static int32_t sim_step(sim_t *s, double ma, double dt_s)
{
    s->mah += ma * dt_s / 3600.0;
    if (s->mah < 0)
        s->mah = 0;
    if (s->mah > s->cap)
        s->mah = s->cap;
    double soc = s->mah * 100.0 / s->cap;
    double ocv = pwr_gauge_soc_to_ocv((int32_t)(soc + 0.5));
    return (int32_t)(ocv + ma / 1000.0 * s->r * 1000.0 + 0.5);
}

/* ---- 1. the curve ------------------------------------------------ */
static void test_curve(void)
{
    CHECK(pwr_gauge_ocv_to_soc(9000) == 100, "above the top clamps to 100");
    CHECK(pwr_gauge_ocv_to_soc(5000) == 0, "below the bottom clamps to 0");

    int32_t prev = -1;
    for (int mv = 6000; mv <= 8600; mv += 10) {
        int32_t s = pwr_gauge_ocv_to_soc(mv);
        CHECK(s >= 0 && s <= 100, "soc in range at %d mV (got %d)", mv, s);
        CHECK(s >= prev, "curve must be monotone (%d mV: %d after %d)", mv, s,
              prev);
        prev = s;
    }
    /* round trip within the resolution the table can carry */
    for (int soc = 5; soc <= 95; soc += 5) {
        int32_t mv = pwr_gauge_soc_to_ocv(soc);
        int32_t back = pwr_gauge_ocv_to_soc(mv);
        CHECK(labs((long)back - soc) <= 1, "round trip %d -> %d mV -> %d", soc,
              mv, back);
    }
}

/* ---- 2. IR compensation ------------------------------------------ */
/*
 * A pack sitting at a fixed SoC while the load steps between 300 mA and
 * 1200 mA (backlight + Wi-Fi + camera, roughly). The terminal voltage moves
 * by I*R = 180 mV; the whole point of the estimator is that the OCV it
 * reports does not.
 */
static void test_ir_compensation(void)
{
    sim_t sim;
    sim_init(&sim, 2000.0, 0.20, 60.0);
    pwr_gauge_t g;
    pwr_gauge_init(&g, 2000, 60);

    double ocv_min = 1e9, ocv_max = -1e9, v_min = 1e9, v_max = -1e9;
    for (int i = 0; i < 900; i++) {
        double ma = (i / 30) % 2 ? -1200.0 : -300.0; /* 30 s per step */
        int32_t mv = sim_step(&sim, ma, 1.0);
        pwr_gauge_step(&g, mv, (int32_t)(ma * 10), 1000, 0);
        if (i > 300) { /* after the estimator has seen a few steps */
            if (g.ocv_v < ocv_min) ocv_min = g.ocv_v;
            if (g.ocv_v > ocv_max) ocv_max = g.ocv_v;
            if (mv < v_min) v_min = mv;
            if (mv > v_max) v_max = mv;
        }
    }
    printf("  R learned %.3f ohm (true 0.200), terminal swing %.0f mV, "
           "OCV swing %.0f mV\n",
           g.r_ohm, v_max - v_min, (ocv_max - ocv_min) * 1000.0);
    CHECK(fabsf(g.r_ohm - 0.20f) < 0.05f, "R should converge near 0.20 (got %.3f)",
          g.r_ohm);
    CHECK(v_max - v_min > 150.0, "the simulated load step must be visible");
    /* The pack really does drain over 15 minutes, so OCV is allowed to move
       a little; it must move far less than the terminal voltage. */
    CHECK((ocv_max - ocv_min) * 1000.0 < (v_max - v_min) / 3.0,
          "IR compensation must flatten the swing");
}

/* ---- 3. the rest anchor ------------------------------------------ */
static void test_rest_anchor(void)
{
    pwr_gauge_t g;
    pwr_gauge_init(&g, 2000, -1);

    /* Idle at a pack voltage that means 50% on the curve. */
    int32_t mv = pwr_gauge_soc_to_ocv(50);
    pwr_anchor_t hit = PWR_ANCHOR_NONE;
    for (int i = 0; i < 120 && hit == PWR_ANCHOR_NONE; i++)
        hit = pwr_gauge_step(&g, mv, -100 /* 10 mA */, 1000, 0);
    CHECK(hit == PWR_ANCHOR_REST, "a quiet minute must anchor on the curve");
    CHECK(labs(g.soc - 50) <= 2, "anchored SoC should be ~50 (got %d)", g.soc);
    CHECK(fabsf(g.mah - 1000.0f) < 60.0f, "integrator seeded (got %.0f mAh)",
          g.mah);

    /* A load that is NOT rest must not anchor. */
    g.rest_n = 0;
    hit = PWR_ANCHOR_NONE;
    for (int i = 0; i < 200 && hit == PWR_ANCHOR_NONE; i++)
        hit = pwr_gauge_step(&g, mv - 100, -6000 /* 600 mA */, 1000, 0);
    CHECK(hit != PWR_ANCHOR_REST, "600 mA is not rest");
}

/* ---- 4. the full anchor and capacity learning -------------------- */
static void test_full_anchor(void)
{
    pwr_gauge_t g;
    pwr_gauge_init(&g, 2000, -1);

    /* Anchor low first, so the charge that follows spans enough SoC for the
       capacity estimate to be allowed to run. */
    int32_t low = pwr_gauge_soc_to_ocv(20);
    for (int i = 0; i < 70; i++)
        pwr_gauge_step(&g, low, 0, 1000, 0);
    CHECK(g.soc_at_anchor == 20 || g.soc_at_anchor == 19 ||
              g.soc_at_anchor == 21,
          "low anchor (got %d)", g.soc_at_anchor);

    /* Charge at 1 A for the coulombs a 2500 mAh pack would take from 20% to
       100% (2000 mAh), then taper. */
    for (int i = 0; i < 7200; i++)
        pwr_gauge_step(&g, 8000, 10000 /* 1000 mA */, 1000,
                       PWR_GAUGE_F_CHARGE_EN | PWR_GAUGE_F_USB);
    pwr_anchor_t hit = PWR_ANCHOR_NONE;
    for (int i = 0; i < 120 && hit == PWR_ANCHOR_NONE; i++)
        hit = pwr_gauge_step(&g, 8250, 500 /* 50 mA taper */, 1000,
                             PWR_GAUGE_F_CHARGE_EN | PWR_GAUGE_F_USB);
    CHECK(hit == PWR_ANCHOR_FULL, "termination must anchor at 100%%");
    CHECK(g.soc == 100, "SoC is 100 after termination (got %d)", g.soc);
    printf("  capacity learned %.0f mAh (fed 2000 mAh over 80%% of a pack)\n",
           g.cap_mah);
    CHECK(g.cap_mah > 2100.0f, "capacity must move toward the measured value "
                               "(got %.0f)", g.cap_mah);

    /* Taper alone, with CHG_EN off, is not termination. */
    pwr_gauge_t h;
    pwr_gauge_init(&h, 2000, 50);
    hit = PWR_ANCHOR_NONE;
    for (int i = 0; i < 200 && hit == PWR_ANCHOR_NONE; i++)
        hit = pwr_gauge_step(&h, 8250, 500, 1000, 0);
    CHECK(hit != PWR_ANCHOR_FULL, "no CHG_EN, no full anchor");
}

/* ---- 5. a whole discharge ---------------------------------------- */
/*
 * Six hours from full to the shutdown region at a load that steps around,
 * which is the case the display smoothing exists for.
 */
static void test_discharge_run(void)
{
    sim_t sim;
    sim_init(&sim, 2000.0, 0.20, 100.0);
    pwr_gauge_t g;
    pwr_gauge_init(&g, 2000, 100);

    int32_t prev_disp = 100;
    int rises = 0, eta_seen = 0;
    int32_t mv = 0;
    for (int i = 0; i < 6 * 3600; i++) {
        /* 250 mA base with a 900 mA burst every other minute */
        double ma = ((i / 60) % 2) ? -900.0 : -250.0;
        mv = sim_step(&sim, ma, 1.0);
        if (mv < 6300)
            break;
        pwr_gauge_step(&g, mv, (int32_t)(ma * 10), 1000, 0);
        if (g.disp > prev_disp)
            rises++;
        prev_disp = g.disp;
        if (g.eta_min > 0)
            eta_seen++;
    }
    printf("  ended at %d mV, disp %d%%, %.0f mAh left, eta %d min\n", mv,
           g.disp, g.mah, g.eta_min);
    CHECK(rises == 0, "the display must never rise while discharging (%d rises)",
          rises);
    CHECK(g.disp < 20, "six hours of load must land low (got %d%%)", g.disp);
    CHECK(eta_seen > 1000, "a time estimate must be available while running");
}

/* ---- 6. no pack installed ---------------------------------------- */
static void test_no_battery(void)
{
    pwr_gauge_t g;
    pwr_gauge_init(&g, 2000, -1);
    for (int i = 0; i < 10; i++)
        pwr_gauge_step(&g, 5000, 0, 1000, PWR_GAUGE_F_USB);
    CHECK(g.no_batt, "5.0 V on the pack node cannot be a 2S pack");
    CHECK(g.soc < 0, "no pack means no percentage (got %d)", g.soc);

    /* and it recovers when one is plugged in */
    for (int i = 0; i < 10; i++)
        pwr_gauge_step(&g, 7600, 0, 1000, PWR_GAUGE_F_USB);
    CHECK(!g.no_batt, "a pack that appears must be picked up");
    CHECK(g.soc >= 0, "and must produce a percentage again");
}

/* ---- 7. a stalled sampler must not integrate an hour -------------- */
static void test_dt_clamp(void)
{
    pwr_gauge_t g;
    pwr_gauge_init(&g, 2000, 50);
    float before = g.mah;
    pwr_gauge_step(&g, 7500, -20000 /* 2 A */, 3600000u /* an hour */, 0);
    printf("  one stalled sample at 2 A: %.1f -> %.1f mAh\n", before, g.mah);
    CHECK(before - g.mah < 10.0f,
          "a 1 h gap must integrate at most the clamp (lost %.1f mAh)",
          before - g.mah);
}

int main(void)
{
    printf("pwr_gauge host test\n");
    printf("[1] OCV curve\n");           test_curve();
    printf("[2] IR compensation\n");     test_ir_compensation();
    printf("[3] rest anchor\n");         test_rest_anchor();
    printf("[4] full anchor\n");         test_full_anchor();
    printf("[5] discharge run\n");       test_discharge_run();
    printf("[6] no battery\n");          test_no_battery();
    printf("[7] dt clamp\n");            test_dt_clamp();

    if (g_fail) {
        printf("\n%d CHECK(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
