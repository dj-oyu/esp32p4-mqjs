/*
 * net_mqtt.c - WiFi STA + MQTT client for JS script distribution.
 *
 * The ESP32-P4 has no radio: esp_wifi_remote forwards the esp_wifi
 * calls below to the Stamp-AddOn C6 running the esp-hosted slave
 * firmware, so this file looks like ordinary WiFi code.
 *
 * MQTT payloads larger than the client buffer arrive fragmented
 * (MQTT_EVENT_DATA with current_data_offset/total_data_len); they are
 * reassembled here before being handed to the script runner.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "net_mqtt.h"
#include "script_runner.h"
#include "script_store.h"

static const char *TAG = "net";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_mqtt_connected;
static bool s_mqtt_started;

static char s_dev_id[13];          /* efuse MAC as 12 hex chars */
static char s_topic_script[64];
static char s_topic_cmd[64];
static char s_topic_status[64];

/* reassembly buffer for fragmented publishes */
enum { RX_NONE = 0, RX_SCRIPT, RX_CMD };
static char *s_rx;
static int s_rx_total;
static int s_rx_kind;

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

void net_mqtt_publish_status(const char *state, int rc)
{
    if (!s_client || !s_mqtt_connected)
        return;
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"state\":\"%s\",\"rc\":%d,\"heap\":%u}",
             state, rc, (unsigned)esp_get_free_heap_size());
    esp_mqtt_client_publish(s_client, s_topic_status, msg, 0, 1, 1);
}

/* ------------------------------------------------------------------ */
/* incoming data                                                       */
/* ------------------------------------------------------------------ */

static bool topic_is(esp_mqtt_event_handle_t ev, const char *topic)
{
    size_t len = strlen(topic);
    return ev->topic_len == len && memcmp(ev->topic, topic, len) == 0;
}

static void rx_reset(void)
{
    free(s_rx);
    s_rx = NULL;
    s_rx_kind = RX_NONE;
    s_rx_total = 0;
}

static void handle_cmd(const char *cmd)
{
    ESP_LOGI(TAG, "command: %s", cmd);
    if (strcmp(cmd, "restart") == 0) {
        script_runner_control(SCRIPT_CMD_RESTART);
    } else if (strcmp(cmd, "stop") == 0) {
        script_runner_control(SCRIPT_CMD_STOP);
    } else if (strcmp(cmd, "clear") == 0) {
        script_store_erase();
        script_runner_control(SCRIPT_CMD_CLEAR);
    } else {
        ESP_LOGW(TAG, "unknown command '%s'", cmd);
        net_mqtt_publish_status("bad-command", -1);
    }
}

static void handle_data(esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset == 0) {
        /* first (maybe only) fragment: topic is only valid here */
        rx_reset();
        int kind = RX_NONE;
        if (topic_is(ev, s_topic_script))
            kind = RX_SCRIPT;
        else if (topic_is(ev, s_topic_cmd))
            kind = RX_CMD;
        if (kind == RX_NONE)
            return;

        if (ev->total_data_len > MQJS_SCRIPT_MAX) {
            ESP_LOGE(TAG, "payload too large (%d > %d)",
                     ev->total_data_len, MQJS_SCRIPT_MAX);
            net_mqtt_publish_status("too-large", -1);
            return;
        }
        s_rx = script_psram_alloc((size_t)ev->total_data_len + 1);
        if (!s_rx) {
            ESP_LOGE(TAG, "no memory for %d byte payload", ev->total_data_len);
            return;
        }
        s_rx_kind = kind;
        s_rx_total = ev->total_data_len;
    }

    if (s_rx_kind == RX_NONE)
        return;                      /* ignored or failed topic */

    memcpy(s_rx + ev->current_data_offset, ev->data, ev->data_len);
    if (ev->current_data_offset + ev->data_len < s_rx_total)
        return;                      /* more fragments coming */

    s_rx[s_rx_total] = '\0';

    if (s_rx_kind == RX_CMD) {
        handle_cmd(s_rx);
        rx_reset();
        return;
    }

    /* RX_SCRIPT */
    if (s_rx_total == 0) {
        /* empty retained message = wipe */
        ESP_LOGI(TAG, "empty script payload -> clearing stored script");
        script_store_erase();
        script_runner_control(SCRIPT_CMD_CLEAR);
        rx_reset();
        return;
    }

    ESP_LOGI(TAG, "received script (%d bytes)", s_rx_total);
    script_store_save(s_rx, (size_t)s_rx_total);
    script_runner_submit(s_rx, (size_t)s_rx_total);  /* ownership moves */
    s_rx = NULL;
    s_rx_kind = RX_NONE;
    s_rx_total = 0;
}

/* ------------------------------------------------------------------ */
/* mqtt client                                                         */
/* ------------------------------------------------------------------ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "mqtt connected; subscribing %s", s_topic_script);
        esp_mqtt_client_subscribe(s_client, s_topic_script, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        net_mqtt_publish_status("online", 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        rx_reset();
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_mqtt_started)
        return;

    char client_id[32];
    snprintf(client_id, sizeof(client_id), "mqjs-%s", s_dev_id);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQJS_MQTT_BROKER_URI,
        .credentials.client_id = client_id,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "{\"state\":\"offline\",\"rc\":0}",
            .qos = 1,
            .retain = 1,
        },
        /* big enough that typical scripts arrive in one piece;
           larger ones are reassembled in handle_data() anyway */
        .buffer.size = 8192,
    };
#if defined(CONFIG_MQJS_MQTT_USERNAME)
    if (CONFIG_MQJS_MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = CONFIG_MQJS_MQTT_USERNAME;
        cfg.credentials.authentication.password = CONFIG_MQJS_MQTT_PASSWORD;
    }
#endif

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "mqtt client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    s_mqtt_started = true;
}

/* ------------------------------------------------------------------ */
/* wifi                                                                */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        mqtt_start();
    }
}

void net_mqtt_start(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_dev_id, sizeof(s_dev_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_topic_script, sizeof(s_topic_script), "%s/%s/script",
             CONFIG_MQJS_TOPIC_PREFIX, s_dev_id);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/%s/cmd",
             CONFIG_MQJS_TOPIC_PREFIX, s_dev_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status",
             CONFIG_MQJS_TOPIC_PREFIX, s_dev_id);
    ESP_LOGI(TAG, "device id: %s (script topic: %s)", s_dev_id, s_topic_script);

    if (CONFIG_MQJS_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "MQJS_WIFI_SSID empty: network disabled, "
                      "set it via menuconfig to enable MQTT distribution");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_MQJS_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_MQJS_WIFI_PASSWORD,
            sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}
