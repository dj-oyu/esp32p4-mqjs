/*
 * M5Stack Tab5 board bring-up: the ESP32-C6 (esp-hosted slave) sits
 * behind a PI4IOE5V6408 IO expander (addr 0x44 on the internal I2C bus,
 * SDA=G31 SCL=G32). Its P0 (WLAN_PWR_EN) must be driven high before the
 * SDIO transport can find the C6. Register values mirror
 * bsp_io_expander_pi4ioe_init() in the M5Tab5-UserDemo BSP
 * (P0=WLAN_PWR_EN, P3=USB5V_EN high; P7=CHG_EN left low).
 *
 * The I2C bus is created, used and deleted again so JS i2c.setup(0,...)
 * can claim the port afterwards.
 */
#include "sdkconfig.h"

#if CONFIG_MQJS_TAB5_POWER

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_tab5.h"

#define PI4IOE2_ADDR 0x44

static const char *TAG = "tab5";

void board_tab5_power_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = 31,
        .scl_io_num = 32,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus for IO expander failed");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PI4IOE2_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not reachable");
        i2c_del_master_bus(bus);
        return;
    }

    static const uint8_t seq[][2] = {
        { 0x01, 0xFF },        /* chip reset */
        { 0x03, 0b10111001 },  /* IO direction (1=output) */
        { 0x07, 0b00000110 },  /* high-impedance off for used pins */
        { 0x0D, 0b10111001 },  /* pull select */
        { 0x0B, 0b11111001 },  /* pull enable */
        { 0x05, 0b00001001 },  /* OUT: P0 WLAN_PWR_EN=1, P3 USB5V_EN=1 */
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]) && err == ESP_OK; i++)
        err = i2c_master_transmit(dev, seq[i], 2, 50);

    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO expander init failed: %s", esp_err_to_name(err));
        return;
    }
    /* give the C6 a moment to come out of power-on before esp-hosted
       starts poking the SDIO bus */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "WLAN power enabled (PI4IOE@0x44 P0)");
}

#endif /* CONFIG_MQJS_TAB5_POWER */
