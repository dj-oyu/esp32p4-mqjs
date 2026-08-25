/*
 * esp32p4-mqjs — M5Stack Tab5 battery driver + policy. See pwr_tab5.h.
 * Copyright (C) 2026 dj-oyu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pwr_tab5.h"

#if CONFIG_MQJS_TAB5_BATTERY

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pwr_gauge.h"
#include "ui_tab5.h"

static const char *TAG = "batt";

/* ------------------------------------------------------------------ */
/* INA226 (0x41 on the internal bus, 5 mOhm shunt)                     */
/* ------------------------------------------------------------------ */
/*
 * We deliberately do NOT program the calibration register and do NOT read
 * the CURRENT/POWER registers. The datasheet's current path exists to spare
 * a host the division; here it only adds a quantisation step (the factory
 * firmware's calibrate(0.005, 8.192) rounds the current LSB up to 0.3 mA)
 * and a register that can silently disagree with the shunt reading. The
 * shunt voltage is exact and the arithmetic is one shift:
 *
 *     2.5 uV per LSB / 5 mOhm = 0.5 mA per LSB
 *
 * so raw * 5 is the current in 0.1 mA units, with no calibration state to
 * get out of sync. Bus voltage is 1.25 mV per LSB = raw * 5 / 4 in mV.
 */
#define INA226_ADDR      0x41
#define INA226_REG_SHUNT 0x01
#define INA226_REG_BUS   0x02
#define INA226_REG_CONF  0x00
#define INA226_REG_ID    0xFF /* die id, 0x2260 */

/* AVG=16, Vbus 1.1 ms, Vshunt 1.1 ms, shunt+bus continuous. Bits 14:12 are
   the reserved '100' pattern the datasheet's reset value carries. One
   conversion pair is 16 * 2 * 1.1 ms = 35 ms, comfortably inside our 1 Hz. */
#define INA226_CONF_VALUE 0x4527

/* ------------------------------------------------------------------ */
/* PI4IOE5V6408 expanders                                              */
/* ------------------------------------------------------------------ */
#define EXP1_ADDR 0x43 /* LCD/TP/CAM reset, SPK_EN, EXT5V_EN */
#define EXP2_ADDR 0x44 /* WLAN, USB5V, PWROFF, QC, CHG       */

#define PI4IO_REG_OUT    0x05
#define PI4IO_REG_IN_STA 0x0F

#define EXP2_BIT_WLAN   0
#define EXP2_BIT_USB5V  3
#define EXP2_BIT_PWROFF 4
#define EXP2_BIT_QC     5 /* active low: 0 = quick charge allowed */
#define EXP2_BIT_CHGSTA 6 /* input                                */
#define EXP2_BIT_CHG    7 /* active high                          */
#define EXP1_BIT_EXT5V  2

#define I2C_TMO_MS 50

/* ------------------------------------------------------------------ */
/* Policy tuning                                                       */
/* ------------------------------------------------------------------ */
#define SAMPLE_MS       1000
#define CEILING_DWELL_MS 120000 /* the charger must never be chattered */
#define CEILING_RESUME   5      /* resume charging at limit - 5 points */

/* Ladder thresholds. Each tier fires on EITHER the percentage or the
   IR-compensated voltage, whichever is worse: the percentage can be wrong
   after a cold boot with no anchor, and the voltage cannot. */
#define T_LOW_PCT   25
#define T_LOW_MV    7350
#define T_SHED_PCT  15
#define T_SHED_MV   7100
#define T_CRIT_PCT   8
#define T_CRIT_MV   6850
#define T_DOWN_PCT   4
#define T_DOWN_MV   6600

#define TIER_UP_N    5  /* consecutive samples to raise a tier   */
#define TIER_DOWN_N 30  /* ... and to lower one again (no flap)  */
#define SHUTDOWN_N  10  /* the last tier wants ten in a row      */

#define NVS_NS      "mqjs_pwr"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static i2c_master_dev_handle_t s_ina, s_exp1, s_exp2;
static SemaphoreHandle_t s_io_mtx;   /* serialises expander read-modify-write */
static SemaphoreHandle_t s_snap_mtx;
static pwr_batt_t s_snap;
static pwr_gauge_t s_gauge;
static bool s_running;

static volatile int  s_tier;          /* published for mqjs_power         */
static int  s_charge_ma = 1000;       /* manual selector, default 1 A     */
static int  s_limit_pct = 100;
static bool s_limit_once;
static bool s_ceiling_hold;
static int64_t s_last_chg_change_ms;
static int  s_sign = 1;

static void (*s_shutdown_cb)(const char *reason, int grace_s);
static void (*s_notify_cb)(const char *text);
static void (*s_sample_cb)(const pwr_batt_t *b);
static volatile bool s_shutdown_ack;

static nvs_handle_t s_nvs;
static bool s_nvs_ok;

/* ------------------------------------------------------------------ */
/* I2C helpers                                                         */
/* ------------------------------------------------------------------ */
static bool reg16_read(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, rx, 2, I2C_TMO_MS) != ESP_OK)
        return false;
    *out = (uint16_t)((rx[0] << 8) | rx[1]);
    return true;
}

static bool reg16_write(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t tx[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    return i2c_master_transmit(dev, tx, 3, I2C_TMO_MS) == ESP_OK;
}

static bool reg8_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1, I2C_TMO_MS) ==
           ESP_OK;
}

/* Read-modify-write one output bit of an expander. The read is not optional
   and a cached shadow is not enough: board_tab5.c writes this register
   during early boot and the two expanders carry unrelated rails, so a
   blind write would be a way to switch off the C6 by accident. */
static bool exp_set_bit(i2c_master_dev_handle_t dev, int bit, bool on)
{
    if (!dev || !s_io_mtx)
        return false; /* called before pwr_tab5_start(), or it gave up */
    bool ok = false;
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    uint8_t cur;
    if (reg8_read(dev, PI4IO_REG_OUT, &cur)) {
        uint8_t next = on ? (uint8_t)(cur | (1u << bit))
                          : (uint8_t)(cur & ~(1u << bit));
        uint8_t tx[2] = { PI4IO_REG_OUT, next };
        ok = i2c_master_transmit(dev, tx, 2, I2C_TMO_MS) == ESP_OK;
    }
    xSemaphoreGive(s_io_mtx);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Early boot: turn charging on before anything else exists            */
/* ------------------------------------------------------------------ */
void pwr_tab5_early_charge_on(void *bus_handle)
{
    if (!bus_handle)
        return;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP2_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device((i2c_master_bus_handle_t)bus_handle, &cfg,
                                  &dev) != ESP_OK) {
        ESP_LOGE(TAG, "expander 0x44 unreachable, charging stays off");
        return;
    }
    uint8_t cur = 0;
    if (!reg8_read(dev, PI4IO_REG_OUT, &cur)) {
        i2c_master_bus_rm_device(dev);
        ESP_LOGE(TAG, "expander read failed, charging stays off");
        return;
    }
    /* QC first, then a settling gap, then CHG_EN — the order the factory
       firmware's HAL init uses (setChargeQcEnable, delay(50), setCharge-
       Enable). QC is active low. */
    uint8_t qc = (uint8_t)(cur & ~(1u << EXP2_BIT_QC));
    uint8_t tx[2] = { PI4IO_REG_OUT, qc };
    i2c_master_transmit(dev, tx, 2, I2C_TMO_MS);
    vTaskDelay(pdMS_TO_TICKS(50));
    tx[1] = (uint8_t)(qc | (1u << EXP2_BIT_CHG));
    esp_err_t err = i2c_master_transmit(dev, tx, 2, I2C_TMO_MS);
    i2c_master_bus_rm_device(dev);
    ESP_LOGI(TAG, "charging %s (CHG_EN + QC on expander 0x44: 0x%02X -> 0x%02X)",
             err == ESP_OK ? "enabled" : "ENABLE FAILED", cur, tx[1]);
}

/* ------------------------------------------------------------------ */
/* NVS                                                                 */
/* ------------------------------------------------------------------ */
static void nvs_load(int32_t *cap_mah, int32_t *saved_soc)
{
    *cap_mah = 0;
    *saved_soc = -1;
    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK)
        return;
    s_nvs_ok = true;
    int32_t v;
    if (nvs_get_i32(s_nvs, "limit", &v) == ESP_OK && v >= 50 && v <= 100)
        s_limit_pct = v;
    if (nvs_get_i32(s_nvs, "cap", &v) == ESP_OK)
        *cap_mah = v;
    if (nvs_get_i32(s_nvs, "soc", &v) == ESP_OK && v >= 0 && v <= 100)
        *saved_soc = v;
}

static void nvs_save_i32(const char *key, int32_t v)
{
    if (!s_nvs_ok)
        return;
    if (nvs_set_i32(s_nvs, key, v) == ESP_OK)
        nvs_commit(s_nvs);
}

/* ------------------------------------------------------------------ */
/* Charge control                                                      */
/* ------------------------------------------------------------------ */
static void apply_charge(bool want_charge, bool want_qc)
{
    exp_set_bit(s_exp2, EXP2_BIT_QC, !want_qc); /* active low */
    exp_set_bit(s_exp2, EXP2_BIT_CHG, want_charge);
    s_last_chg_change_ms = esp_timer_get_time() / 1000;
}

void pwr_tab5_set_charge_ma(int ma)
{
    s_charge_ma = (ma <= 0) ? 0 : (ma >= 1000 ? 1000 : 500);
    if (s_running)
        apply_charge(s_charge_ma > 0 && !s_ceiling_hold, s_charge_ma >= 1000);
    ESP_LOGI(TAG, "charge selector = %d mA", s_charge_ma);
}

int pwr_tab5_charge_ma(void) { return s_charge_ma; }

void pwr_tab5_set_limit(int pct)
{
    if (pct < 50)
        pct = 50;
    if (pct > 100)
        pct = 100;
    s_limit_pct = pct;
    s_limit_once = false;
    nvs_save_i32("limit", pct);
    ESP_LOGI(TAG, "charge ceiling = %d%%", pct);
}

int pwr_tab5_limit(void) { return s_limit_pct; }

void pwr_tab5_full_charge_once(void)
{
    s_limit_once = true;
    s_ceiling_hold = false;
    if (s_running)
        apply_charge(s_charge_ma > 0, s_charge_ma >= 1000);
    ESP_LOGI(TAG, "ceiling suspended for this charge");
}

void pwr_tab5_set_usb5v(bool on) { exp_set_bit(s_exp2, EXP2_BIT_USB5V, on); }
void pwr_tab5_set_ext5v(bool on) { exp_set_bit(s_exp1, EXP1_BIT_EXT5V, on); }

void pwr_tab5_power_off(void)
{
    ESP_LOGW(TAG, "power off");
    /* Three pulses, as the factory BSP does: the hardware latch is edge
       triggered and a single edge has been seen to be missed. */
    for (int i = 0; i < 3; i++) {
        exp_set_bit(s_exp2, EXP2_BIT_PWROFF, true);
        vTaskDelay(pdMS_TO_TICKS(100));
        exp_set_bit(s_exp2, EXP2_BIT_PWROFF, false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int pwr_tab5_tier(void) { return s_tier; }

void pwr_tab5_set_shutdown_cb(void (*fn)(const char *, int)) { s_shutdown_cb = fn; }
void pwr_tab5_shutdown_ack(void) { s_shutdown_ack = true; }
void pwr_tab5_set_notify_cb(void (*fn)(const char *)) { s_notify_cb = fn; }
void pwr_tab5_set_sample_cb(void (*fn)(const pwr_batt_t *)) { s_sample_cb = fn; }
void pwr_tab5_set_current_sign(int sign) { s_sign = sign < 0 ? -1 : 1; }

static void notify(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void notify(const char *fmt, ...)
{
    if (!s_notify_cb)
        return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s_notify_cb(buf);
}

/* ------------------------------------------------------------------ */
/* The owner task                                                      */
/* ------------------------------------------------------------------ */
static bool ina_present(void)
{
    uint16_t id = 0;
    return reg16_read(s_ina, INA226_REG_ID, &id) && id == 0x2260;
}

/* One sample. Returns false when the monitor stopped answering. */
static bool sample(int32_t *mv, int32_t *ma_x10, uint8_t *in_sta)
{
    uint16_t bus = 0, shunt = 0;
    if (!reg16_read(s_ina, INA226_REG_BUS, &bus))
        return false;
    if (!reg16_read(s_ina, INA226_REG_SHUNT, &shunt))
        return false;
    *mv = ((int32_t)bus * 5) / 4;                    /* 1.25 mV per LSB   */
    *ma_x10 = (int32_t)(int16_t)shunt * 5 * s_sign;  /* 0.5 mA per LSB    */
    *in_sta = 0;
    reg8_read(s_exp2, PI4IO_REG_IN_STA, in_sta);
    return true;
}

/* The tier ladder, with separate confirm counts up and down. Returns the
   tier this sample argues for; the caller applies the hysteresis. */
static int tier_for(int32_t pct, int32_t ocv_mv)
{
    if ((pct >= 0 && pct <= T_DOWN_PCT) || ocv_mv <= T_DOWN_MV)
        return PWR_TIER_SHUTDOWN;
    if ((pct >= 0 && pct <= T_CRIT_PCT) || ocv_mv <= T_CRIT_MV)
        return PWR_TIER_CRITICAL;
    if ((pct >= 0 && pct <= T_SHED_PCT) || ocv_mv <= T_SHED_MV)
        return PWR_TIER_SHED;
    if ((pct >= 0 && pct <= T_LOW_PCT) || ocv_mv <= T_LOW_MV)
        return PWR_TIER_LOW;
    return PWR_TIER_OK;
}

static void enter_tier(int tier)
{
    switch (tier) {
    case PWR_TIER_LOW:
        notify("バッテリー残りわずかです");
        break;
    case PWR_TIER_SHED:
        /* The two rails the device can live without: USB-A host power and
           the M5-Bus / Grove 5 V. Both are pure loss when nothing is
           plugged into them, and neither is load-bearing for the UI. */
        pwr_tab5_set_usb5v(false);
        pwr_tab5_set_ext5v(false);
        notify("省電力: 外部5V出力を停止しました");
        break;
    case PWR_TIER_CRITICAL:
        notify("バッテリー切れが近づいています");
        break;
    default:
        break;
    }
}

static void leave_tier(int from)
{
    if (from >= PWR_TIER_SHED) {
        pwr_tab5_set_usb5v(true);
        pwr_tab5_set_ext5v(true);
        notify("外部5V出力を復帰しました");
    }
}

static void pwr_task(void *arg)
{
    (void)arg;
    int64_t prev_ms = esp_timer_get_time() / 1000;
    int tier_cand = PWR_TIER_OK, tier_n = 0;
    int64_t shutdown_at_ms = 0; /* 0 = no countdown running */
    int cut_tries = 0;          /* pulses sent that did not take          */
    const int grace_s = CONFIG_MQJS_TAB5_BATTERY_GRACE_S;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SAMPLE_MS));

        int64_t now_ms = esp_timer_get_time() / 1000;
        uint32_t dt = (uint32_t)(now_ms - prev_ms);
        prev_ms = now_ms;

        int32_t mv, ma_x10;
        uint8_t in_sta;
        if (!sample(&mv, &ma_x10, &in_sta)) {
            ESP_LOGW(TAG, "INA226 stopped answering");
            xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
            s_snap.present = false;
            s_snap.state = PWR_BATT_UNKNOWN;
            xSemaphoreGive(s_snap_mtx);
            continue;
        }

        /*
         * Charger presence. The expander's P6 is named CHG_STAT by M5Unified
         * and usb_c_detect by the BSP, and nothing we can read says which
         * polarity means "attached" — so it is reported raw in the snapshot
         * for bring-up and only used as a hint here. What actually decides
         * is current flowing INTO the pack, which is unambiguous once the
         * sign convention is pinned (pwr_tab5_set_current_sign).
         */
        bool charging_now = ma_x10 > 500; /* > 50 mA into the pack */
        bool usb = charging_now || ((in_sta & (1u << EXP2_BIT_CHGSTA)) != 0);

        uint32_t flags = 0;
        if (s_charge_ma > 0 && !s_ceiling_hold)
            flags |= PWR_GAUGE_F_CHARGE_EN;
        if (usb)
            flags |= PWR_GAUGE_F_USB;

        pwr_anchor_t anchor = pwr_gauge_step(&s_gauge, mv, ma_x10, dt, flags);
        int32_t pct = s_gauge.disp;
        int32_t ocv_mv = (int32_t)(s_gauge.ocv_v * 1000.0f);

        if (anchor == PWR_ANCHOR_FULL) {
            nvs_save_i32("cap", (int32_t)s_gauge.cap_mah);
            s_limit_once = false; /* one-shot ceiling override is spent */
        }

        /* ---- charge ceiling ------------------------------------ */
        bool ceiling = false;
        if (!s_limit_once && s_limit_pct < 100 && pct >= 0) {
            if (s_ceiling_hold)
                ceiling = pct > s_limit_pct - CEILING_RESUME;
            else
                ceiling = pct >= s_limit_pct;
        }
        if (ceiling != s_ceiling_hold &&
            now_ms - s_last_chg_change_ms >= CEILING_DWELL_MS) {
            s_ceiling_hold = ceiling;
            apply_charge(s_charge_ma > 0 && !s_ceiling_hold,
                         s_charge_ma >= 1000);
            if (ceiling)
                notify("充電を%d%%で停止しました", s_limit_pct);
            else
                notify("充電を再開しました");
        }

        /* ---- the ladder ---------------------------------------- */
        /*
         * The ladder runs on a charger too -- shedding the external 5 V
         * rails and letting the screen go dark while a nearly-empty pack
         * charges is exactly the right answer, because it lets the charger
         * win against the load. What a charger DOES forbid is the last
         * rung: the Tab5 charges only while powered on, so switching off
         * to save a pack that is being refilled would strand it.
         */
        int want = s_gauge.no_batt ? PWR_TIER_OK : tier_for(pct, ocv_mv);
        if (usb && want >= PWR_TIER_SHUTDOWN)
            want = PWR_TIER_CRITICAL;
        if (want == tier_cand) {
            if (tier_n < 1000)
                tier_n++;
        } else {
            tier_cand = want;
            tier_n = 1;
        }
        int need = (want == PWR_TIER_SHUTDOWN) ? SHUTDOWN_N
                   : (want > s_tier)           ? TIER_UP_N
                                               : TIER_DOWN_N;
        if (want != s_tier && tier_n >= need) {
            int from = s_tier;
            s_tier = want;
            if (want > from)
                enter_tier(want);
            else
                leave_tier(from);
            ESP_LOGW(TAG, "tier %d -> %d (%ld%% ocv %ld mV)", from, want,
                     (long)pct, (long)ocv_mv);
        }

        /* ---- over-discharge shutdown --------------------------- */
        if (s_tier == PWR_TIER_SHUTDOWN && !shutdown_at_ms && !cut_tries) {
            shutdown_at_ms = now_ms + grace_s * 1000;
            s_shutdown_ack = false;
            notify("電池切れ: %d秒後に電源を切ります", grace_s);
            ESP_LOGE(TAG, "over-discharge shutdown armed (%ld mV ocv, %ld%%)",
                     (long)ocv_mv, (long)pct);
            if (s_shutdown_cb)
                s_shutdown_cb("battery", grace_s);
        } else if (shutdown_at_ms && (usb || s_tier < PWR_TIER_SHUTDOWN)) {
            shutdown_at_ms = 0;
            cut_tries = 0;
            notify("充電を検出: シャットダウンを中止しました");
            ESP_LOGW(TAG, "shutdown aborted (charger appeared)");
        } else if (shutdown_at_ms &&
                   (s_shutdown_ack || now_ms >= shutdown_at_ms)) {
            nvs_save_i32("soc", pct < 0 ? 0 : pct);
            ESP_LOGE(TAG, "cutting power (%s)",
                     s_shutdown_ack ? "apps saved" : "grace expired");
            pwr_tab5_power_off();
            /* Still here, so the pulse did not take. Do NOT replay the
               countdown: the apps have already had their last words and the
               user has already seen the notice. Just try the pulse again in
               a minute, quietly, until the pack or the hardware gives. */
            cut_tries++;
            shutdown_at_ms = now_ms + 60000;
            if (cut_tries == 1)
                ESP_LOGE(TAG, "power did not cut; retrying every 60 s");
        }

        /* ---- publish ------------------------------------------- */
        xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
        s_snap.mv = mv;
        s_snap.ma = ma_x10 / 10;
        s_snap.ocv_mv = ocv_mv;
        s_snap.mohm = (int32_t)(s_gauge.r_ohm * 1000.0f);
        s_snap.pct = s_gauge.no_batt ? -1 : pct;
        s_snap.mah = (int32_t)s_gauge.mah;
        s_snap.cap_mah = (int32_t)s_gauge.cap_mah;
        s_snap.eta_min = s_gauge.eta_min;
        s_snap.tier = (uint8_t)s_tier;
        s_snap.limit_pct = (uint8_t)s_limit_pct;
        s_snap.in_sta = in_sta;
        s_snap.usb = usb;
        s_snap.charge_en = s_charge_ma > 0 && !s_ceiling_hold;
        s_snap.qc_en = s_charge_ma >= 1000;
        s_snap.present = true;
        s_snap.samples++;
        s_snap.state = s_gauge.no_batt          ? PWR_BATT_NONE
                       : s_ceiling_hold         ? PWR_BATT_LIMITED
                       : (anchor == PWR_ANCHOR_FULL || (usb && pct >= 100))
                           ? PWR_BATT_FULL
                       : charging_now ? PWR_BATT_CHARGING
                                      : PWR_BATT_DISCHARGING;
        pwr_batt_t pub = s_snap;
        xSemaphoreGive(s_snap_mtx);

        if (s_sample_cb)
            s_sample_cb(&pub);
    }
}

bool pwr_tab5_get(pwr_batt_t *out)
{
    if (!out)
        return false;
    if (!s_snap_mtx || !s_running) {
        memset(out, 0, sizeof *out);
        out->pct = -1;
        out->eta_min = -1;
        return false;
    }
    xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_snap_mtx);
    return out->present;
}

void pwr_tab5_start(void)
{
    if (s_running)
        return;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)ui_tab5_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "no internal I2C bus; battery monitoring disabled");
        return;
    }
    s_io_mtx = xSemaphoreCreateMutex();
    s_snap_mtx = xSemaphoreCreateMutex();
    if (!s_io_mtx || !s_snap_mtx)
        return;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000,
    };
    cfg.device_address = INA226_ADDR;
    if (i2c_master_bus_add_device(bus, &cfg, &s_ina) != ESP_OK) {
        ESP_LOGE(TAG, "INA226 not addressable");
        return;
    }
    cfg.device_address = EXP1_ADDR;
    i2c_master_bus_add_device(bus, &cfg, &s_exp1);
    cfg.device_address = EXP2_ADDR;
    i2c_master_bus_add_device(bus, &cfg, &s_exp2);

    /* The die id is a sanity line in the log, not a gate: a board that
       answers at 0x41 and accepts the config register is good enough, and
       refusing to monitor the battery because a vendor shipped a different
       marking would be the worse failure. The config write IS the gate. */
    if (!ina_present())
        ESP_LOGW(TAG, "INA226 die id mismatch at 0x%02X (continuing)",
                 INA226_ADDR);
    if (!reg16_write(s_ina, INA226_REG_CONF, INA226_CONF_VALUE)) {
        ESP_LOGE(TAG, "INA226 config write failed; battery monitoring off");
        return;
    }

#if CONFIG_MQJS_TAB5_BATTERY_INVERT_CURRENT
    s_sign = -1;
#endif

    int32_t cap_mah, saved_soc;
    nvs_load(&cap_mah, &saved_soc);
    pwr_gauge_init(&s_gauge, cap_mah, saved_soc);

    /* Re-assert the charge selector on the live handles: early boot set the
       bits through a bus that has since been deleted, and the ceiling policy
       has not run yet. */
    apply_charge(s_charge_ma > 0, s_charge_ma >= 1000);

    s_running = true;
    xTaskCreatePinnedToCore(pwr_task, "batt", 3584, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "battery monitor up (cap %ld mAh, saved soc %ld, limit %d%%, "
                  "current sign %+d)",
             (long)s_gauge.cap_mah, (long)saved_soc, s_limit_pct, s_sign);
}

#endif /* CONFIG_MQJS_TAB5_BATTERY */
