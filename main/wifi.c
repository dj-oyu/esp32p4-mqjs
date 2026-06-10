/*
 * WiFi station bring-up for Stamp-P4 + Stamp-AddOn C6.
 *
 * The P4 has no radio; esp_wifi_remote forwards the regular esp_wifi_* API
 * over SDIO (esp-hosted) to the C6, so this file looks exactly like the
 * stock IDF station example. SDIO pins live in sdkconfig.defaults,
 * credentials in menuconfig -> "mqjs platform".
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "ui_status.h"
#include "wifi.h"

static const char *TAG = "wifi";

/* esp-hosted public API (component header not on main's include path).
 * Idempotent: with the Tab5 UI build, components/ui_tab5 defers the
 * constructor-time call (early heap too small for the SDIO mempools, C6
 * still power-gated), so this is where the transport really comes up.
 * On Stamp builds the constructor already did the work and this no-ops. */
int esp_hosted_init(void);

static EventGroupHandle_t s_evt;
#define GOT_IP_BIT BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_evt, GOT_IP_BIT);
        ESP_LOGW(TAG, "disconnected, reconnecting");
        ui_status_set_net(false, NULL);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        ui_status_set_net(true, ip);
        xEventGroupSetBits(s_evt, GOT_IP_BIT);
    }
}

bool wifi_start_and_wait(uint32_t timeout_ms)
{
    if (CONFIG_MQJS_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "no SSID set (menuconfig: mqjs platform), WiFi disabled");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_hosted_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_evt = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_MQJS_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_MQJS_WIFI_PASSWORD, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* wall clock for JS Date (and the Tab5 clock demo): background SNTP,
       syncs whenever the link is up and re-syncs periodically */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    ESP_LOGI(TAG, "connecting to \"%s\"...", CONFIG_MQJS_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_evt, GOT_IP_BIT, pdFALSE, pdFALSE,
                                           timeout_ms ? pdMS_TO_TICKS(timeout_ms)
                                                      : portMAX_DELAY);
    if (!(bits & GOT_IP_BIT)) {
        ESP_LOGW(TAG, "no IP after %lu ms (keeps retrying in background)",
                 (unsigned long)timeout_ms);
        return false;
    }
    return true;
}
