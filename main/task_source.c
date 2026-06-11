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
/* sig+script must fit in one esp-mqtt packet: honor MAX_SCRIPT_LEN with
   topic/header headroom. 32KB silently capped pushes below the
   documented limit (ssh_vt grew past it at T3b: "rejected: too large"
   while the sender said published). */
#define RX_BUF_SIZE    (MAX_SCRIPT_LEN + 2048)
#define TX_BUF_SIZE    2048          /* out = short status lines only */

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

/* P4c registry (design §4.5): the broker's retained messages on
 * <TASK_TOPIC>/apps/<name> ARE the store. (Re)connecting replays the
 * whole shelf; the byte-compare below makes that sync idempotent and
 * silent. Apps are only ever INSTALLED here — never run (publish ->
 * immediate replace stays a dev-topic behavior, §6). An empty retained
 * payload is the tombstone: uninstall. It cannot carry a signature
 * (that's how MQTT clears a retained message), accepted on the
 * LAN-only broker — worst case an attacker deletes, never installs. */
#define APPS_PREFIX CONFIG_MQJS_TASK_TOPIC "/apps/"

static void registry_rx(esp_mqtt_event_handle_t e)
{
    char name[25];
    size_t nlen = (size_t)e->topic_len - (sizeof(APPS_PREFIX) - 1);
    const char *p = e->topic + sizeof(APPS_PREFIX) - 1;
    if (nlen == 0 || nlen >= sizeof name) {
        ui_status_set_event("app rejected: bad name");
        return;
    }
    for (size_t i = 0; i < nlen; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            ui_status_set_event("app rejected: bad name");
            return;
        }
        name[i] = c;
    }
    name[nlen] = '\0';

    if (e->data_len == 0) { /* tombstone */
        if (storage_delete_app(name)) {
            char ev[48];
            snprintf(ev, sizeof ev, "uninstalled: %s", name);
            publish_status(ev);
            ui_status_set_event(ev);
        }
        return;
    }

    size_t n = (size_t)e->data_len;
    if (n <= SIG_LEN || n > MAX_SCRIPT_LEN) {
        publish_status("error: bad app length");
        return;
    }
    unsigned char *m = malloc(n + 1);
    if (!m)
        return;
    unsigned long long mlen = 0;
    if (crypto_sign_open(m, &mlen, (const unsigned char *)e->data, n,
                         MQJS_TASK_PUBKEY) != 0) {
        free(m);
        publish_status("error: bad app signature");
        ui_status_set_event("app rejected: bad signature");
        return;
    }
    m[mlen] = '\0';

    /* the in-file identity must match the shelf it was put on */
    if (mlen < 8 + nlen + 1 || memcmp(m, "// @app ", 8) != 0 ||
        memcmp(m + 8, name, nlen) != 0 ||
        (m[8 + nlen] != '\n' && m[8 + nlen] != '\r' && m[8 + nlen] != ' ')) {
        free(m);
        publish_status("error: @app/topic mismatch");
        ui_status_set_event("app rejected: @app mismatch");
        return;
    }

    size_t cur_len = 0;
    char *cur = storage_load_app(name, &cur_len);
    bool same = cur && cur_len == (size_t)mlen && !memcmp(cur, m, cur_len);
    bool existed = cur != NULL;
    free(cur);
    if (same) { /* retained replay on reconnect: nothing changed */
        free(m);
        return;
    }
    if (storage_save_app(name, (const char *)m, (size_t)mlen)) {
        char ev[48];
        snprintf(ev, sizeof ev, "%s: %s (%uB)",
                 existed ? "updated" : "installed", name, (unsigned)mlen);
        publish_status(ev);
        ui_status_set_event(ev);
    } else {
        publish_status("error: app save failed");
    }
    free(m);
}

static void ev_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ready, listening on %s", CONFIG_MQJS_TASK_TOPIC);
        esp_mqtt_client_subscribe(s_cli, CONFIG_MQJS_TASK_TOPIC, 1);
        /* P4c registry shelf: retained app payloads replay on every
           (re)connect = the store sync */
        esp_mqtt_client_subscribe(s_cli, APPS_PREFIX "+", 1);
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
            char ev0[48];
            snprintf(ev0, sizeof ev0, "rejected: too large (%dB)",
                     e->total_data_len);
            publish_status("error: too large");
            ui_status_set_event(ev0);
            break;
        }
        /* route by topic: registry shelf vs the dev task topic */
        if (e->topic_len > (int)(sizeof(APPS_PREFIX) - 1) &&
            memcmp(e->topic, APPS_PREFIX, sizeof(APPS_PREFIX) - 1) == 0) {
            registry_rx(e);
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

        /* P4b-lite install: a "// @app <name>" first line means
           "save to /littlefs/apps/, do NOT replace the running task"
           (the launcher lists and starts it). This separation is
           deliberate: store-delivered apps must never auto-run —
           publish->immediate-replace stays a dev-only behavior
           (launcher-multiapp-design §6). */
        if (mlen > 8 && memcmp(m, "// @app ", 8) == 0) {
            char name[25];
            size_t n = 0;
            const char *p = (const char *)m + 8;
            while (n < sizeof name - 1 &&
                   ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
                name[n++] = *p++;
            }
            name[n] = '\0';
            char ev2[48];
            if (n > 0 && storage_save_app(name, (const char *)m,
                                          (size_t)mlen)) {
                snprintf(ev2, sizeof ev2, "installed: %s (%uB)", name,
                         (unsigned)mlen);
                publish_status(ev2);
            } else {
                snprintf(ev2, sizeof ev2, "install failed: bad @app name");
                publish_status("error: bad @app name");
            }
            ui_status_set_event(ev2);
            free(m);
            break;
        }

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
        /* without this, out inherits buffer.size = a second 66KB block */
        .buffer.out_size = TX_BUF_SIZE,
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
