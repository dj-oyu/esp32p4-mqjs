#include "tailscale_adapter.h"
#include "system_vault.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(CONFIG_MQJS_TAILSCALE) && defined(ESP_PLATFORM)

#include "microlink.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <time.h>

static const char *TAG = "ts_adapter";

#define TS_MAX_RETRIES   5
#define TS_MAX_NTP_WAITS 18   /* ~180s: first NTP sync over a fresh link is slow */
#define TS_TICK_US     (10 * 1000 * 1000)   /* 10s connect-watchdog tick */
#define TS_DEVICE_NAME "m5stack-tab5"

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_watchdog;
static microlink_t *s_ml;
static char *s_session_key;          /* heap-owned, alive for the session */
static char s_state[20] = "not-configured";
static char s_detail[64];
static int s_retries;
static int s_ntp_waits;              /* ticks spent waiting for the clock */
static bool s_connected_once;
static bool s_net_up;
static bool s_starting;              /* armed: waiting for time sync / connecting */
static bool s_skip_autostart;        /* set when a prior boot reset mid-connect */

/* Crash-loop guard in RTC memory: survives a watchdog/SW reset, is random on a
   cold (power-on) boot. We "arm" it right before starting microlink and clear
   it on a successful connect. A boot that finds it still armed means the
   previous boot reset while connecting. We count CONSECUTIVE such resets and
   only skip auto-start after TS_GUARD_MAX_FAILS of them — so a one-off reset
   (e.g. a reflash's watchdog-reset, or a transient) still retries, while a real
   connect-crash boot-loop is still broken. A successful connect, or an explicit
   user enable/reauth, resets the counter. Bump TS_GUARD_MAGIC whenever the set
   of RTC guard vars changes so a firmware update starts from a clean count. */
#define TS_GUARD_MAGIC      0x7a11c0dfu   /* bumped: added s_guard_fails */
#define TS_GUARD_ARMED      0xa5a5a5a5u
#define TS_GUARD_MAX_FAILS  3
static RTC_NOINIT_ATTR uint32_t s_guard_magic;
static RTC_NOINIT_ATTR uint32_t s_guard_state;
static RTC_NOINIT_ATTR uint32_t s_guard_fails;   /* consecutive mid-connect resets */

static bool time_is_valid(void)
{
    return time(NULL) > 1700000000;  /* ~2023-11; SNTP has set the clock */
}

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
        s_guard_state = 0;   /* connected OK -> disarm the crash-loop guard */
        s_guard_fails = 0;   /* and reset the consecutive-reset counter */
        /* keep the watchdog ticking: its connected branch logs heap (measurement) */
        if (s_watchdog) {
            esp_timer_stop(s_watchdog);
            esp_timer_start_periodic(s_watchdog, TS_TICK_US);
        }
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

/* caller holds the lock; time must be valid and a key present in the vault */
static void start_microlink(void)
{
    if (s_ml)
        return;
    s_guard_state = TS_GUARD_ARMED;  /* arm: about to run risky microlink network */
    /* internal-RAM watermark: esp_hosted's SDIO RX path needs internal DMA RAM;
       a small largest-block here is what makes the handshake-time alloc fail. */
    ESP_LOGW(TAG, "start: INTERNAL free=%u largest=%u | DMA free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    s_retries = 0;   /* fresh budget for the connect phase */
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

/* connect watchdog: bounds the time-sync wait (before microlink starts) AND
 * microlink's connect attempts (it auto-reconnects forever and can't flag auth
 * rejection). Success is driven by callbacks, not this tick. */
static void watchdog_tick(void *arg)
{
    (void)arg;
    lock();
    if (!strcmp(s_state, "connected")) {
        /* connected: log heap periodically (measurement) instead of stopping */
        ESP_LOGW(TAG, "connected heap: INTERNAL free=%u largest=%u | DMA free=%u "
                      "largest=%u | PSRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        unlock();
        return;
    }
    /* Not connected. Two distinct phases with separate budgets. */
    if (s_starting && !s_ml) {
        /* NTP-wait: poll the clock directly — don't depend only on the SNTP
         * sync_cb (it can be missed, and the first sync over a fresh link is
         * slow). Start microlink the moment the clock is valid; be patient
         * before giving up (TS_MAX_NTP_WAITS, separate from connect retries). */
        if (time_is_valid()) {
            start_microlink();
            unlock();
            return;
        }
        if (++s_ntp_waits >= TS_MAX_NTP_WAITS) {
            stop_session();
            set_status("error", "時刻同期できません（ネットワーク確認）");
            ESP_LOGW(TAG, "tailnet time-sync gave up after %d ticks", TS_MAX_NTP_WAITS);
        } else {
            char d[64];
            snprintf(d, sizeof d, "時刻同期中… (%d)", s_ntp_waits);
            set_status("connecting", d);
        }
        unlock();
        return;
    }
    /* Connect-retry: microlink is running, waiting to reach CONNECTED. */
    s_retries++;
    if (s_retries >= TS_MAX_RETRIES) {
        stop_session();
        char d[64];
        snprintf(d, sizeof d, "接続失敗（%d回）auth key/接続先を確認", TS_MAX_RETRIES);
        set_status("error", d);
        ESP_LOGW(TAG, "tailnet connect gave up after %d ticks", TS_MAX_RETRIES);
    } else {
        char d[64];
        snprintf(d, sizeof d, "接続中… 試行%d回", s_retries);
        set_status("connecting", d);
    }
    unlock();
}

/* Wi-Fi SNTP sync callback chain (wifi_set_time_sync_cb). Start microlink once
 * the clock is valid and we're armed — the callback-driven start (no polling).
 * Public: called from wifi.c's sntp_synced(). */
void tailscale_adapter_on_time_synced(void)
{
    lock();
    if (s_starting && !s_ml && time_is_valid())
        start_microlink();
    unlock();
}

/* caller holds the lock: arm the session. wifi.c owns SNTP; microlink starts
 * now if time is already valid, else on the next time-sync callback. */
/* TEMPORARILY GATED (2026-06-15): microlink's connect handshake exhausts
   esp_hosted's SDIO DMA RX buffer pool under load — `assert(*buf)` in
   sdio_drv.c after a ~4.6KB _h_malloc_align fails (internal DMA RAM is starved
   by microlink tasks + camera + LVGL + mqjs). That crash + boot auto-start =
   a boot loop. Everything up to "Sending Noise handshake" works (crypto, key,
   SNTP, STUN, controlplane TCP), so this is purely an internal-RAM budget
   issue. Set TS_CONNECT_ENABLED=1 once internal RAM headroom is fixed. */
#define TS_CONNECT_ENABLED 1

static void begin_connect(void)
{
    if (s_ml || s_starting)
        return;
#if !TS_CONNECT_ENABLED
    set_status("error", "一時無効（SDIO RAM調整中）");
    return;
#else
    s_starting = true;
    s_retries = 0;
    s_ntp_waits = 0;
    if (s_watchdog)
        esp_timer_start_periodic(s_watchdog, TS_TICK_US);
    if (time_is_valid())
        start_microlink();
    else
        set_status("connecting", "時刻同期中…");
#endif
}

/* reflect "key present + enabled + network up" into the idle state strings.
 * Armed but Wi-Fi not up yet (failure case: Wi-Fi never connects) shows a
 * network-wait rather than a misleading "connecting". */
static void refresh_idle_status(void)
{
    if (!system_vault_tailscale_has())
        set_status("not-configured", "未設定");
    else if (!system_vault_tailscale_enabled())
        set_status("disabled", "オフ");
    else if (!s_net_up)
        set_status("connecting", "ネットワーク待ち");
    else
        set_status("connecting", "接続準備中");
}

void tailscale_adapter_init(void)
{
    if (s_guard_magic != TS_GUARD_MAGIC) {   /* cold boot / fw update: RTC random */
        s_guard_magic = TS_GUARD_MAGIC;
        s_guard_state = 0;
        s_guard_fails = 0;
    } else if (s_guard_state == TS_GUARD_ARMED) {
        s_guard_fails++;             /* last boot armed but never disarmed */
    } else {
        s_guard_fails = 0;           /* last boot ended cleanly */
    }
    s_guard_state = 0;               /* start this boot disarmed */
    s_skip_autostart = (s_guard_fails >= TS_GUARD_MAX_FAILS);
    if (s_skip_autostart)
        ESP_LOGW(TAG, "%u consecutive resets mid-connect -> skipping auto-start "
                      "(power-cycle or re-enable to retry)", (unsigned)s_guard_fails);
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
    /* measurement (fires even when connect is gated): the internal/DMA headroom
       available to esp_hosted's SDIO RX path once the network + apps are up. */
    ESP_LOGW(TAG, "net-up heap: INTERNAL free=%u largest=%u | DMA free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    if (system_vault_tailscale_has() && system_vault_tailscale_enabled()) {
        if (s_skip_autostart)   /* prior boot reset mid-connect: don't loop */
            set_status("error", "前回接続中にリセット。手動で再試行/電源入れ直し");
        else
            begin_connect();
    } else {
        refresh_idle_status();
    }
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
    s_skip_autostart = false;   /* explicit user action: clear the crash guard */
    s_guard_fails = 0;
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
    s_skip_autostart = false;   /* new key = explicit retry: clear the crash guard */
    s_guard_fails = 0;
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
void tailscale_adapter_on_time_synced(void) {}
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
