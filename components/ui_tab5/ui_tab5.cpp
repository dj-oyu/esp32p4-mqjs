/*
 * Tab5 on-device UI, Phase 0: panel bring-up + LVGL Hello World.
 *
 * Hardware values mirror the M5Tab5-UserDemo BSP (m5stack_tab5.c):
 *  - 5" 720x1280 MIPI-DSI panel, ILI9881C controller, 2 lanes @730Mbps,
 *    DPI 60MHz RGB565, DPHY powered from on-chip LDO channel 3 @2.5V
 *  - LCD_RST (P4) and TP_RST (P5) sit on a PI4IOE5V6408 IO expander at
 *    0x43 on the internal I2C bus (SDA=G31 SCL=G32) — the 0x44 expander
 *    (C6 power) is handled separately by main/board_tab5.c
 *  - backlight is LEDC PWM on GPIO22 (12bit @5kHz)
 *
 * Newer Tab5 units ship an ST7123/ST7121 panel instead; the variant is
 * identified by which touch controller answers on I2C (GT911 -> ILI9881C,
 * 0x55 -> ST712x). Only ILI9881C is implemented; ST712x units log an
 * error and the UI stays off (firmware keeps running headless).
 *
 * The LVGL tick/timer task is owned by esp_lvgl_port, pinned to Core 1
 * at low priority so it never starves js_task (Core 0, Phase 1 will pin
 * that side explicitly).
 */
#include "sdkconfig.h"
#if CONFIG_MQJS_TAB5_UI

#include <string.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_tab5.h"

#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"

#include "ili9881_init_data.inc"
#include "st7123_init_data.inc"

/* Noto Sans CJK JP subset, fonts/font_noto_jp_20_4.c (compiled as C).
 * The hiz8 min TTF it was generated from has no glyph for U+0020 space
 * (or U+0022) — they render as tofu. ui_font() returns a mutable copy
 * with Montserrat 20 as fallback to fill the ASCII gaps. */
extern "C" {
LV_FONT_DECLARE(font_noto_jp_20_4);
}

static const lv_font_t *ui_font(void)
{
    static lv_font_t jp;
    if (!jp.line_height) {
        jp = font_noto_jp_20_4;
        jp.fallback = &lv_font_montserrat_20;
    }
    return &jp;
}

static const char *TAG = "ui_tab5";

/*
 * esp-hosted 2.x runs its full transport init from a pre-scheduler C
 * constructor. At that point only the small early heap exists (no PSRAM,
 * ~110KB internal DMA RAM) and its two SDIO mempools need ~90KB of it —
 * this component's extra .bss (LVGL et al.) pushed that over the edge:
 * boot died in "assert failed: sdio_mempool_create ... (buf_mp_g)".
 *
 * Fix: wrap esp_hosted_init (-Wl,--wrap, only when MQJS_TAB5_UI=y) so
 * the constructor-time call becomes a no-op; main/wifi.c calls it again
 * once the scheduler is up (esp_hosted_init_done makes that the real,
 * one-and-only init). Side benefit: on Tab5 the C6 is power-gated off
 * until board_tab5_power_init(), so constructor-time init was always
 * too early on this board.
 */
extern "C" int __real_esp_hosted_init(void);
extern "C" int __wrap_esp_hosted_init(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        ESP_EARLY_LOGW(TAG, "deferring esp_hosted_init to after startup");
        return ESP_OK;
    }
    return __real_esp_hosted_init();
}

/* --- Tab5 display constants (M5Tab5-UserDemo BSP) --- */
#define UI_LCD_H_RES        720
#define UI_LCD_V_RES        1280
#define UI_DSI_LANES        2
#define UI_DPHY_LDO_CHAN    3
#define UI_DPHY_LDO_MV      2500
#define UI_BACKLIGHT_GPIO   22
#define UI_LVGL_BUF_LINES   50

/* Tab5 shipped with different panels over time; the variant is identified
 * by which touch controller answers on the internal I2C bus
 * (GT911 -> ILI9881C, 0x55 + fw version -> ST7121/ST7123). */
typedef enum {
    UI_PANEL_NONE = 0,
    UI_PANEL_ILI9881C, /* 730Mbps lanes, DPI 60MHz */
    UI_PANEL_ST7121,   /* 965Mbps lanes, DPI 70MHz */
    UI_PANEL_ST7123,
} ui_panel_variant_t;

/* internal I2C bus */
#define UI_I2C_PORT         0
#define UI_I2C_SDA          31
#define UI_I2C_SCL          32
#define UI_PI4IOE1_ADDR     0x43 /* P4=LCD_RST, P5=TP_RST */
#define UI_GT911_ADDR       0x5D /* primary GT911 address */
#define UI_GT911_ADDR_BKP   0x14
#define UI_ST7123_TP_ADDR   0x55 /* touch of the ST712x panel variant */

/* Touch fw version register 0x0000 on the 0x55 controller tells the
 * ST712x flavour apart: 1 = ST7121, 3 (or anything else) = ST7123. */
static ui_panel_variant_t st712x_flavour(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = UI_ST7123_TP_ADDR;
    dev_cfg.scl_speed_hz = 100000;

    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK)
        return UI_PANEL_ST7123;

    const uint8_t reg[2] = { 0x00, 0x00 };
    uint8_t fw = 0xFF;
    esp_err_t err = i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 100);
    i2c_master_bus_rm_device(dev);

    ESP_LOGI(TAG, "ST712x touch fw version: %u (%s)", fw,
             esp_err_to_name(err));
    return (err == ESP_OK && fw == 1) ? UI_PANEL_ST7121 : UI_PANEL_ST7123;
}

/*
 * Release LCD/TP reset via the 0x43 expander and sniff which touch
 * controller answers (tells us the panel variant). The bus is created
 * and deleted again so JS i2c.setup(0, ...) can claim the port later.
 */
static ui_panel_variant_t panel_reset_and_detect(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = UI_I2C_PORT;
    bus_cfg.sda_io_num = (gpio_num_t)UI_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)UI_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus for IO expander failed");
        return UI_PANEL_NONE;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = UI_PI4IOE1_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander 0x43 not reachable");
        i2c_del_master_bus(bus);
        return UI_PANEL_NONE;
    }

    /* register writes mirror bsp_io_expander_pi4ioe_init() (0x43 half):
       P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST, P5 TP_RST, P6 CAM_RST high */
    static const uint8_t seq[][2] = {
        { 0x01, 0xFF },        /* chip reset */
        { 0x03, 0b01111111 },  /* IO direction (1=output) */
        { 0x07, 0b00000000 },  /* high-impedance off for used pins */
        { 0x0D, 0b01111111 },  /* pull select */
        { 0x0B, 0b01111111 },  /* pull enable */
        { 0x05, 0b01110110 },  /* OUT: LCD_RST/TP_RST released high */
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]) && err == ESP_OK; i++)
        err = i2c_master_transmit(dev, seq[i], 2, 50);
    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO expander 0x43 init failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(bus);
        return UI_PANEL_NONE;
    }

    /* give the touch controller time to boot out of reset, then probe */
    vTaskDelay(pdMS_TO_TICKS(100));
    ui_panel_variant_t variant;
    if (i2c_master_probe(bus, UI_GT911_ADDR, 50) == ESP_OK ||
        i2c_master_probe(bus, UI_GT911_ADDR_BKP, 50) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 found -> ILI9881C panel variant");
        variant = UI_PANEL_ILI9881C;
    } else if (i2c_master_probe(bus, UI_ST7123_TP_ADDR, 50) == ESP_OK) {
        variant = st712x_flavour(bus);
        ESP_LOGI(TAG, "touch @0x55 -> %s panel variant",
                 variant == UI_PANEL_ST7121 ? "ST7121" : "ST7123");
    } else {
        ESP_LOGW(TAG, "no touch controller answered, assuming ILI9881C");
        variant = UI_PANEL_ILI9881C;
    }
    i2c_del_master_bus(bus);
    return variant;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_12_BIT;
    timer_cfg.timer_num = LEDC_TIMER_0;
    timer_cfg.freq_hz = 5000;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK)
        return err;

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num = UI_BACKLIGHT_GPIO;
    ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg.channel = LEDC_CHANNEL_1;
    ch_cfg.timer_sel = LEDC_TIMER_0;
    ch_cfg.duty = 0; /* stay dark until the first frame is up */
    ch_cfg.hpoint = 0;
    return ledc_channel_config(&ch_cfg);
}

static void backlight_set(int percent)
{
    uint32_t duty = (4095u * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static esp_err_t display_init(ui_panel_variant_t variant,
                              esp_lcd_panel_io_handle_t *out_io,
                              esp_lcd_panel_handle_t *out_panel)
{
    bool ili9881c = (variant == UI_PANEL_ILI9881C);
    bool st7121 = (variant == UI_PANEL_ST7121);

    /* power the MIPI DPHY from the on-chip LDO */
    static esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = UI_DPHY_LDO_CHAN;
    ldo_cfg.voltage_mv = UI_DPHY_LDO_MV;
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DPHY LDO failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = 0;
    bus_cfg.num_data_lanes = UI_DSI_LANES;
    bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = ili9881c ? 730 : 965;
    err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits = 8;
    dbi_cfg.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DBI io failed: %s", esp_err_to_name(err));
        return err;
    }

    /* timings per variant, straight from the UserDemo BSP */
    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = ili9881c ? 60 : 70;
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.num_fbs = 1;
    dpi_cfg.video_timing.h_size = UI_LCD_H_RES;
    dpi_cfg.video_timing.v_size = UI_LCD_V_RES;
    if (ili9881c) {
        dpi_cfg.video_timing.hsync_pulse_width = 40;
        dpi_cfg.video_timing.hsync_back_porch = 140;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = 4;
        dpi_cfg.video_timing.vsync_back_porch = 20;
        dpi_cfg.video_timing.vsync_front_porch = 20;
    } else {
        dpi_cfg.video_timing.hsync_pulse_width = 2;
        dpi_cfg.video_timing.hsync_back_porch = 40;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = st7121 ? 20 : 2;
        dpi_cfg.video_timing.vsync_back_porch = st7121 ? 24 : 8;
        dpi_cfg.video_timing.vsync_front_porch = st7121 ? 200 : 220;
    }

    esp_lcd_panel_dev_config_t dev_cfg = {};
    dev_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev_cfg.reset_gpio_num = GPIO_NUM_NC; /* reset is on the IO expander */

    esp_lcd_panel_handle_t panel = NULL;
    if (ili9881c) {
        ili9881c_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = tab5_lcd_ili9881c_specific_init_code_default;
        vendor_cfg.init_cmds_size = sizeof(tab5_lcd_ili9881c_specific_init_code_default) /
                                    sizeof(tab5_lcd_ili9881c_specific_init_code_default[0]);
        vendor_cfg.mipi_config.dsi_bus = dsi_bus;
        vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
        vendor_cfg.mipi_config.lane_num = UI_DSI_LANES;
        dev_cfg.bits_per_pixel = 16;
        dev_cfg.vendor_config = &vendor_cfg;
        err = esp_lcd_new_panel_ili9881c(io, &dev_cfg, &panel);
    } else if (st7121) {
        st7121_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = NULL; /* driver-internal defaults */
        vendor_cfg.init_cmds_size = 0;
        vendor_cfg.mipi_config.dsi_bus = dsi_bus;
        vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
        dev_cfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        dev_cfg.bits_per_pixel = 24;
        dev_cfg.vendor_config = &vendor_cfg;
        err = esp_lcd_new_panel_st7121(io, &dev_cfg, &panel);
    } else {
        st7123_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = st7123_vendor_specific_init_default;
        vendor_cfg.init_cmds_size = sizeof(st7123_vendor_specific_init_default) /
                                    sizeof(st7123_vendor_specific_init_default[0]);
        vendor_cfg.mipi_config.dsi_bus = dsi_bus;
        vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
        vendor_cfg.mipi_config.lane_num = UI_DSI_LANES;
        dev_cfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        dev_cfg.bits_per_pixel = 24;
        dev_cfg.vendor_config = &vendor_cfg;
        err = esp_lcd_new_panel_st7123(io, &dev_cfg, &panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel create failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    /* IDF 6: dma2d blit is a runtime switch, not a config flag */
    esp_lcd_dpi_panel_enable_dma2d(panel);

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}

extern "C" void ui_tab5_start(void)
{
    ui_panel_variant_t variant = panel_reset_and_detect();
    if (variant == UI_PANEL_NONE)
        return;
    if (backlight_init() != ESP_OK)
        ESP_LOGW(TAG, "backlight init failed, continuing");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    if (display_init(variant, &io, &panel) != ESP_OK)
        return;

    /* LVGL task on Core 1, low priority (js_task runs on Core 0) */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    port_cfg.task_priority = 4;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return;
    }

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = io;
    disp_cfg.panel_handle = panel;
    disp_cfg.buffer_size = UI_LCD_H_RES * UI_LVGL_BUF_LINES;
    disp_cfg.double_buffer = false;
    disp_cfg.hres = UI_LCD_H_RES;
    disp_cfg.vres = UI_LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = true;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = false;

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return;
    }

    /* Phase 0 acceptance: a Japanese label on screen */
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_label_set_text(label, "こんにちは世界 - mqjs Tab5 UI Phase 0");
    lv_obj_set_style_text_color(label, lv_color_hex(0xE0E6EA), 0);
    lv_obj_center(label);
    lvgl_port_unlock();

    backlight_set(100);
    ESP_LOGI(TAG, "UI up (%dx%d)", UI_LCD_H_RES, UI_LCD_V_RES);
}

/* Phase 1 will route these into the console ring / status snapshot */
extern "C" void ui_tab5_log(const char *line, size_t n)
{
    (void)line;
    (void)n;
}

extern "C" void ui_tab5_set_status(const ui_status_t *st)
{
    (void)st;
}

#endif /* CONFIG_MQJS_TAB5_UI */
