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

/* fired once, on the first IP: brings up the only network-dependent
   service (MQTT task delivery). Everything else — JS mqtt/http/ssh — opens
   its own connection on demand and self-retries, so nothing else needs to
   gate boot on the link. */
static void (*s_on_got_ip)(void);
static bool s_ip_announced;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, reconnecting");
        ui_status_set_net(false, NULL);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        ui_status_set_net(true, ip);
        if (!s_ip_announced && s_on_got_ip) {
            s_ip_announced = true;
            s_on_got_ip();   /* e.g. task_source_start() — runs in the event
                                task; esp_mqtt_client_start is non-blocking */
        }
    }
}

/* Non-blocking: brings the link up and returns. The connect itself is
   asynchronous (esp_wifi_connect runs off WIFI_EVENT_STA_START), so app_main
   is never held waiting for an IP — on_got_ip fires once the link is up and
   starts the network-dependent service. The only unavoidable cost here is
   esp_hosted_init bringing up the SDIO transport to the C6 (~1s), which is
   link setup, not the connect. */
void wifi_start(void (*on_got_ip)(void))
{
    s_on_got_ip = on_got_ip;

    if (CONFIG_MQJS_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "no SSID set (menuconfig: mqjs platform), WiFi disabled");
        return;
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
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_ERR_INVALID_STATE)   /* already created is fine */
        ESP_ERROR_CHECK(le);
    esp_netif_create_default_wifi_sta();

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

    ESP_LOGI(TAG, "connecting to \"%s\" in background...", CONFIG_MQJS_WIFI_SSID);
}
