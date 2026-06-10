/*
 * Receive replacement JS tasks over MQTT (see task_source.h).
 *
 * SECURITY: there is no signature verification yet (roadmap: Ed25519 +
 * trusted source). Anyone who can publish to the topic runs code on the
 * device, so on a public broker the topic name is the only secret.
 * The JS watchdog still bounds what a hostile script can do per run.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "mqjs_runtime.h"
#include "task_source.h"

#define MAX_SCRIPT_LEN (64 * 1024)
#define RX_BUF_SIZE    (32 * 1024)   /* scripts must fit in one packet */

static const char *TAG = "task_src";

static esp_mqtt_client_handle_t s_cli;
static SemaphoreHandle_t s_lock;
static char *s_pending;

static void publish_status(const char *msg)
{
    esp_mqtt_client_publish(s_cli, CONFIG_MQJS_TASK_TOPIC "/status",
                            msg, 0, 0, 0);
}

static void ev_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ready, listening on %s", CONFIG_MQJS_TASK_TOPIC);
        esp_mqtt_client_subscribe(s_cli, CONFIG_MQJS_TASK_TOPIC, 1);
        publish_status("ready");
        break;

    case MQTT_EVENT_DATA: {
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) {
            ESP_LOGW(TAG, "script does not fit the rx buffer (%d bytes), ignored",
                     e->total_data_len);
            publish_status("error: too large");
            break;
        }
        if (e->data_len == 0 || e->data_len > MAX_SCRIPT_LEN)
            break;
        char *buf = malloc((size_t)e->data_len + 1);
        if (!buf) {
            publish_status("error: no memory");
            break;
        }
        memcpy(buf, e->data, e->data_len);
        buf[e->data_len] = '\0';

        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_pending);            /* superseded before it ever ran */
        s_pending = buf;
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "new task received (%d bytes), stopping current script",
                 e->data_len);
        publish_status("accepted");
        mqjs_runtime_stop();
        break;
    }
    default:
        break;
    }
}

void task_source_start(void)
{
    if (CONFIG_MQJS_TASK_TOPIC[0] == '\0') {
        ESP_LOGI(TAG, "disabled (no topic configured)");
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQJS_TASK_BROKER,
        .buffer.size = RX_BUF_SIZE,
    };
    s_cli = esp_mqtt_client_init(&cfg);
    if (!s_cli) {
        ESP_LOGE(TAG, "mqtt init failed");
        return;
    }
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, ev_cb, NULL);
    esp_mqtt_client_start(s_cli);
}

char *task_source_take(void)
{
    if (!s_lock)
        return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *p = s_pending;
    s_pending = NULL;
    xSemaphoreGive(s_lock);
    return p;
}
