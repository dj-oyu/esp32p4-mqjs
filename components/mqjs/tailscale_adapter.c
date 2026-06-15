#include "tailscale_adapter.h"
#include "system_vault.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(CONFIG_MQJS_TAILSCALE) && defined(ESP_PLATFORM)

#include "microlink.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ts_adapter";

#define TS_MAX_RETRIES 5
#define TS_TICK_US     (10 * 1000 * 1000)   /* 10s connect-watchdog tick */
#define TS_DEVICE_NAME "m5stack-tab5"

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_watchdog;
static microlink_t *s_ml;
static char *s_session_key;          /* heap-owned, alive for the session */
static char s_state[20] = "not-configured";
static char s_detail[64];
static int s_retries;
static bool s_connected_once;
static bool s_net_up;
static bool s_time_synced;
static bool s_sntp_started;
static bool s_starting;              /* connect sequence in flight */

static void lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

static void set_status(const char *state, const char *detail)
{
    snprintf(s_state, sizeof s_state, "%s", state);
    snprintf(s_detail, sizeof s_detail, "%s", detail ? detail : "");
}

/* caller holds the lock */
static void stop_session(void)
{
    if (s_watchdog)
        esp_timer_stop(s_watchdog);
    if (s_ml) {
        microlink_stop(s_ml);
        microlink_destroy(s_ml);   /* stops using the auth key */
        s_ml = NULL;
    }
    if (s_session_key) {           /* zero the secret only after destroy */
        memset(s_session_key, 0, SYSTEM_VAULT_TS_AUTH_MAX + 1);
        free(s_session_key);
        s_session_key = NULL;
    }
    s_connected_once = false;
    s_starting = false;
}

static void on_ml_state(microlink_t *ml, microlink_state_t st, void *ud)
{
    (void)ml; (void)ud;
    lock();
    if (st == ML_STATE_CONNECTED) {
        s_connected_once = true;
        s_retries = 0;
        s_starting = false;
        if (s_watchdog)
            esp_timer_stop(s_watchdog);
        char ip[16] = "";
        if (s_ml)
            microlink_ip_to_str(microlink_get_vpn_ip(s_ml), ip);
        char d[64];
        snprintf(d, sizeof d, "接続済み %s", ip);
        set_status("connected", d);
        ESP_LOGI(TAG, "tailnet connected: %s", ip);
    } else if (st == ML_STATE_RECONNECTING && s_connected_once) {
        set_status("connecting", "再接続中");
    }
    unlock();
}

/* caller holds the lock; key must already be in the vault + s_time_synced */
static void start_microlink(void)
{
    s_session_key = malloc(SYSTEM_VAULT_TS_AUTH_MAX + 1);
    if (!s_session_key ||
        !system_vault_tailscale_read(s_session_key, SYSTEM_VAULT_TS_AUTH_MAX + 1)) {
        if (s_session_key) { free(s_session_key); s_session_key = NULL; }
        set_status("not-configured", "");
        s_starting = false;
        return;
    }
    microlink_config_t cfg = {
        .auth_key = s_session_key,    /* pointer kept by microlink for the session */
        .device_name = TS_DEVICE_NAME,
        .enable_derp = true,          /* fallback only; direct path preferred */
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = 4,
    };
    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        stop_session();
        set_status("error", "初期化に失敗しました");
        return;
    }
    microlink_set_state_callback(s_ml, on_ml_state, NULL);
    if (microlink_start(s_ml) != ESP_OK) {
        stop_session();
        set_status("error", "起動に失敗しました");
        return;
    }
    set_status("connecting", "接続中…");
}

/* periodic connect-watchdog: microlink never reports "auth rejected" distinctly
 * and auto-reconnects forever, so bound the initial connect ourselves. */
static void watchdog_tick(void *arg)
{
    (void)arg;
    lock();
    if (!strcmp(s_state, "connected")) {
        esp_timer_stop(s_watchdog);
        unlock();
        return;
    }
    s_retries++;
    if (s_retries >= TS_MAX_RETRIES) {
        stop_session();
        char d[64];
        snprintf(d, sizeof d, "接続失敗（%d回試行）auth keyを確認", TS_MAX_RETRIES);
        set_status("error", d);
        ESP_LOGW(TAG, "tailnet connect gave up after %d tries", TS_MAX_RETRIES);
    } else {
        char d[64];
        snprintf(d, sizeof d, "接続中… 試行%d回", s_retries);
        set_status("connecting", d);
    }
    unlock();
}

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    lock();
    s_time_synced = true;
    if (s_starting && !s_ml)
        start_microlink();
    unlock();
}

/* caller holds the lock: begin the connect sequence (NTP then microlink). */
static void begin_connect(void)
{
    if (s_ml || s_starting)
        return;
    s_starting = true;
    s_retries = 0;
    set_status("connecting", "時刻同期中…");
    if (s_watchdog)
        esp_timer_start_periodic(s_watchdog, TS_TICK_US);
    if (s_time_synced) {
        start_microlink();
        return;
    }
    if (!s_sntp_started) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        c.sync_cb = on_sntp_sync;
        if (esp_netif_sntp_init(&c) == ESP_OK)
            s_sntp_started = true;
    }
}

/* reflect "key present + enabled" into the idle state strings */
static void refresh_idle_status(void)
{
    if (!system_vault_tailscale_has())
        set_status("not-configured", "未設定");
    else if (!system_vault_tailscale_enabled())
        set_status("disabled", "オフ");
}

void tailscale_adapter_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    const esp_timer_create_args_t a = {
        .callback = watchdog_tick, .name = "ts_wd",
    };
    esp_timer_create(&a, &s_watchdog);
    lock();
    refresh_idle_status();
    unlock();
}

void tailscale_adapter_on_net_up(void)
{
    lock();
    s_net_up = true;
    if (system_vault_tailscale_has() && system_vault_tailscale_enabled())
        begin_connect();
    else
        refresh_idle_status();
    unlock();
}

void tailscale_adapter_get_status(tailscale_status_t *out)
{
    lock();
    snprintf(out->state, sizeof out->state, "%s", s_state);
    snprintf(out->detail, sizeof out->detail, "%s", s_detail);
    out->retries = s_retries;
    out->configured = system_vault_tailscale_has();
    out->enabled = system_vault_tailscale_enabled();
    if (s_ml && !strcmp(s_state, "connected")) {
        microlink_ip_to_str(microlink_get_vpn_ip(s_ml), out->ip);
        out->peers = microlink_get_peer_count(s_ml);
    } else {
        out->ip[0] = '\0';
        out->peers = 0;
    }
    unlock();
}

bool tailscale_adapter_enable(void)
{
    if (!system_vault_tailscale_set_enabled(true))
        return false;
    lock();
    if (system_vault_tailscale_has() && s_net_up)
        begin_connect();
    else
        refresh_idle_status();
    unlock();
    return true;
}

bool tailscale_adapter_disable(void)
{
    if (!system_vault_tailscale_set_enabled(false))
        return false;
    lock();
    stop_session();
    set_status("disabled", "オフ");
    unlock();
    return true;
}

void tailscale_adapter_reauth(void)
{
    lock();
    stop_session();
    if (system_vault_tailscale_has() && system_vault_tailscale_enabled() && s_net_up)
        begin_connect();
    else
        refresh_idle_status();
    unlock();
}

void tailscale_adapter_forget(void)
{
    lock();
    stop_session();
    set_status("not-configured", "未設定");
    unlock();
}

#else  /* CONFIG_MQJS_TAILSCALE off or host build: no-op stubs */

void tailscale_adapter_init(void) {}
void tailscale_adapter_on_net_up(void) {}
void tailscale_adapter_get_status(tailscale_status_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->state, sizeof out->state, "not-configured");
}
bool tailscale_adapter_enable(void) { return false; }
bool tailscale_adapter_disable(void) { return false; }
void tailscale_adapter_reauth(void) {}
void tailscale_adapter_forget(void) {}

#endif
