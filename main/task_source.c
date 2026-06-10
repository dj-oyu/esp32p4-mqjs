/*
 * Receive replacement JS tasks over MQTT (see task_source.h).
 *
 * Payload wire format: Ed25519 signature(64 bytes) || JS source. Only
 * payloads that verify against the firmware's embedded public key
 * (task_pubkey.h) are accepted; verified tasks are persisted to LittleFS
 * and replace the running script. The JS watchdog still bounds each run.
 *
 * SECURITY: flash is trusted - a persisted task is not re-verified on
 * boot. Key rotation requires re-flashing (regenerate task_pubkey.h).
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "mqjs_runtime.h"
#include "storage.h"
#include "task_pubkey.h"
#include "task_source.h"
#include "tweetnacl.h"
#include "ui_status.h"

#define SIG_LEN        64
#define MAX_SCRIPT_LEN (64 * 1024)
#define RX_BUF_SIZE    (32 * 1024)   /* sig+script must fit in one packet */

static const char *TAG = "task_src";

static esp_mqtt_client_handle_t s_cli;
static SemaphoreHandle_t s_lock;
static char *s_pending;
static size_t s_pending_len;

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
        ui_status_set_mqtt(true);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ui_status_set_mqtt(false);
        break;

    case MQTT_EVENT_DATA: {
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) {
            ESP_LOGW(TAG, "payload does not fit the rx buffer (%d bytes), ignored",
                     e->total_data_len);
            publish_status("error: too large");
            ui_status_set_event("rejected: too large");
            break;
        }
        size_t n = (size_t)e->data_len;
        if (n <= SIG_LEN || n > MAX_SCRIPT_LEN) {
            publish_status("error: bad length");
            ui_status_set_event("rejected: bad length");
            break;
        }

        /* crypto_sign_open writes the message into a buffer of size n;
           the recovered script is its first (n - 64) bytes. */
        unsigned char *m = malloc(n + 1);
        if (!m) {
            publish_status("error: no memory");
            break;
        }
        unsigned long long mlen = 0;
        int rc = crypto_sign_open(m, &mlen, (const unsigned char *)e->data,
                                  n, MQJS_TASK_PUBKEY);
        if (rc != 0) {
            free(m);
            ESP_LOGW(TAG, "signature verification failed, rejected");
            publish_status("error: bad signature");
            ui_status_set_event("rejected: bad signature");
            break;
        }
        m[mlen] = '\0';

        storage_save_task((const char *)m, (size_t)mlen);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_pending);            /* superseded before it ever ran */
        s_pending = (char *)m;
        s_pending_len = (size_t)mlen;
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "verified task accepted (%llu bytes), stopping current script",
                 mlen);
        publish_status("accepted");
        char ev[48];
        snprintf(ev, sizeof ev, "accepted (%uB)", (unsigned)mlen);
        ui_status_set_event(ev);
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
        /* Ed25519 verify runs in the mqtt event task; give it headroom */
        .task.stack_size = 8192,
    };
    s_cli = esp_mqtt_client_init(&cfg);
    if (!s_cli) {
        ESP_LOGE(TAG, "mqtt init failed");
        return;
    }
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, ev_cb, NULL);
    esp_mqtt_client_start(s_cli);
}

char *task_source_take(size_t *len)
{
    if (!s_lock) {
        *len = 0;
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *p = s_pending;
    *len = s_pending_len;
    s_pending = NULL;
    s_pending_len = 0;
    xSemaphoreGive(s_lock);
    return p;
}
