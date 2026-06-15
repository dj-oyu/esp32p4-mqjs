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
#include "freertos/queue.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "ts_adapter";

#define TS_MAX_RETRIES   5
#define TS_MAX_NTP_WAITS 18   /* ~180s: first NTP sync over a fresh link is slow */
#define TS_TICK_US     (10 * 1000 * 1000)   /* 10s connect-watchdog tick */
#define TS_DEVICE_NAME "m5stack-tab5"

/* Connection state: the fixed set the UI/JS reflects. set_status() takes this
 * enum (type-safe; no stringly-typed strcmp), while the freeform detail stays a
 * string. ts_state_str() maps to the JS-facing string at the get_status edge. */
typedef enum {
    TS_ST_NOT_CONFIGURED,
    TS_ST_DISABLED,
    TS_ST_CONNECTING,
    TS_ST_CONNECTED,
    TS_ST_ERROR,
} ts_state_t;

/* Lifecycle commands posted to the single-owner lifecycle_task. */
typedef enum {
    TS_LIFECYCLE_CMD_START,
    TS_LIFECYCLE_CMD_STOP,
    TS_LIFECYCLE_CMD_REAUTH,
    TS_LIFECYCLE_CMD_FORGET,
    TS_LIFECYCLE_CMD_SUSPEND,   /* external exclusive user (camera): stop, stay armed */
    TS_LIFECYCLE_CMD_RESUME,    /* exclusive user done: re-arm if still enabled */
} ts_lifecycle_cmd_t;

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_watchdog;
static microlink_t *s_ml;
static char *s_session_key;          /* heap-owned, alive for the session */
static ts_state_t s_state = TS_ST_NOT_CONFIGURED;
static char s_detail[64];
static int s_retries;
static int s_ntp_waits;              /* ticks spent waiting for the clock */
static bool s_connected_once;
static bool s_net_up;
static bool s_starting;              /* armed: waiting for time sync / connecting */
static bool s_skip_autostart;        /* set when a prior boot reset mid-connect */
static bool s_suspended;             /* external exclusive user (camera) owns the radio */
static SemaphoreHandle_t s_suspend_done; /* SUSPEND stop-completion -> suspend() */

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

static const char *ts_state_str(ts_state_t s)
{
    switch (s) {
    case TS_ST_DISABLED:   return "disabled";
    case TS_ST_CONNECTING: return "connecting";
    case TS_ST_CONNECTED:  return "connected";
    case TS_ST_ERROR:      return "error";
    case TS_ST_NOT_CONFIGURED:
    default:               return "not-configured";
    }
}

static void set_status(ts_state_t state, const char *detail)
{
    s_state = state;
    snprintf(s_detail, sizeof s_detail, "%s", detail ? detail : "");
}

/* caller holds the lock */
static void stop_session(void)
{
    if (s_watchdog)
        esp_timer_stop(s_watchdog);
    if (s_ml) {
        /* destroy stops internally (joins the workers, then frees) — do NOT
         * call microlink_stop() separately or the worker join runs twice. */
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
        set_status(TS_ST_CONNECTED, d);
        ESP_LOGI(TAG, "tailnet connected: %s", ip);
    } else if (st == ML_STATE_RECONNECTING && s_connected_once) {
        set_status(TS_ST_CONNECTING, "再接続中");
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
        set_status(TS_ST_NOT_CONFIGURED, "");
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
        set_status(TS_ST_ERROR, "初期化に失敗しました");
        return;
    }
    microlink_set_state_callback(s_ml, on_ml_state, NULL);
    if (microlink_start(s_ml) != ESP_OK) {
        stop_session();
        set_status(TS_ST_ERROR, "起動に失敗しました");
        return;
    }
    set_status(TS_ST_CONNECTING, "接続中…");
}

/* ---- lifecycle command queue: single owner of microlink ------------------
 * Callbacks (the SNTP sync_cb runs in lwIP's tcpip_thread; the connect
 * watchdog runs in the esp_timer task) and the public API must NOT run
 * start_microlink()/stop_session() themselves: start_microlink opens lwIP
 * sockets, and lwIP's socket API self-deadlocks if called from tcpip_thread.
 * Instead every path POSTs a command (non-blocking, any context) and the
 * resident lifecycle_task is the ONLY code that drives microlink — so the
 * blocking lwIP work always runs in a plain task context, and all START/STOP/
 * REAUTH/FORGET transitions are serialized through one owner. */
static QueueHandle_t s_lifecycle_queue;
static void begin_connect(void);          /* fwd: REAUTH re-arms */
static void refresh_idle_status(void);    /* fwd: STOP reflects idle state */

static bool lifecycle_post_cmd(ts_lifecycle_cmd_t cmd)   /* safe from any context */
{
    return s_lifecycle_queue &&
           xQueueSend(s_lifecycle_queue, &cmd, 0) == pdTRUE;
}

static void lifecycle_handle_cmd(ts_lifecycle_cmd_t cmd) /* lifecycle_task only */
{
    lock();
    switch (cmd) {
    case TS_LIFECYCLE_CMD_START:   /* launch microlink once armed + clock valid */
        if (!s_suspended && !s_ml && s_starting && time_is_valid())
            start_microlink();
        break;
    case TS_LIFECYCLE_CMD_STOP:    /* user turned it off */
        stop_session();
        refresh_idle_status();
        break;
    case TS_LIFECYCLE_CMD_SUSPEND: { /* camera scan owns the radio: stop, stay armed */
        bool had_session = (s_state == TS_ST_CONNECTED || s_state == TS_ST_CONNECTING);
        s_suspended = true;
        stop_session();            /* synchronous: tasks/sockets/DMA down on return */
        if (had_session)           /* only override status if there was one to pause */
            set_status(TS_ST_CONNECTING, "カメラ起動のため一時停止");
        else
            refresh_idle_status(); /* disabled / not-configured: stay honest */
        if (s_suspend_done)
            xSemaphoreGive(s_suspend_done);  /* observable stop-completion */
        break;
    }
    case TS_LIFECYCLE_CMD_RESUME:  /* camera done: re-arm if still wanted */
        s_suspended = false;
        if (system_vault_tailscale_has() && system_vault_tailscale_enabled() &&
            s_net_up && !s_skip_autostart)
            begin_connect();
        else
            refresh_idle_status();
        break;
    case TS_LIFECYCLE_CMD_REAUTH:  /* new key: tear down, then re-arm */
        stop_session();
        if (system_vault_tailscale_has() &&
            system_vault_tailscale_enabled() && s_net_up)
            begin_connect();
        else
            refresh_idle_status();
        break;
    case TS_LIFECYCLE_CMD_FORGET:  /* credentials cleared */
        stop_session();
        set_status(TS_ST_NOT_CONFIGURED, "未設定");
        break;
    }
    unlock();
}

static void lifecycle_task(void *arg)
{
    (void)arg;
    ts_lifecycle_cmd_t cmd;
    for (;;)
        if (xQueueReceive(s_lifecycle_queue, &cmd, portMAX_DELAY) == pdTRUE)
            lifecycle_handle_cmd(cmd);
}

/* connect watchdog: bounds the time-sync wait (before microlink starts) AND
 * microlink's connect attempts (it auto-reconnects forever and can't flag auth
 * rejection). Success is driven by callbacks, not this tick. */
static void watchdog_tick(void *arg)
{
    (void)arg;
    lock();
    if (s_suspended) {   /* camera owns the radio: the timer is stopped, ignore strays */
        unlock();
        return;
    }
    if (s_state == TS_ST_CONNECTED) {
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
            lifecycle_post_cmd(TS_LIFECYCLE_CMD_START);
            unlock();
            return;
        }
        if (++s_ntp_waits >= TS_MAX_NTP_WAITS) {
            stop_session();
            set_status(TS_ST_ERROR, "時刻同期できません（ネットワーク確認）");
            ESP_LOGW(TAG, "tailnet time-sync gave up after %d ticks", TS_MAX_NTP_WAITS);
        } else {
            char d[64];
            snprintf(d, sizeof d, "時刻同期中… (%d)", s_ntp_waits);
            set_status(TS_ST_CONNECTING, d);
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
        set_status(TS_ST_ERROR, d);
        ESP_LOGW(TAG, "tailnet connect gave up after %d ticks", TS_MAX_RETRIES);
    } else {
        char d[64];
        snprintf(d, sizeof d, "接続中… 試行%d回", s_retries);
        set_status(TS_ST_CONNECTING, d);
    }
    unlock();
}

/* Wi-Fi SNTP sync callback chain (wifi_set_time_sync_cb). Start microlink once
 * the clock is valid and we're armed — the callback-driven start (no polling).
 * Public: called from wifi.c's sntp_synced(). */
void tailscale_adapter_on_time_synced(void)
{
    lock();
    /* runs in lwIP tcpip_thread — must NOT touch lwIP sockets here; just post
       a command for the lifecycle task to start microlink in a safe context. */
    if (!s_suspended && s_starting && !s_ml && time_is_valid())
        lifecycle_post_cmd(TS_LIFECYCLE_CMD_START);
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
    if (s_ml || s_starting || s_suspended)
        return;
#if !TS_CONNECT_ENABLED
    set_status(TS_ST_ERROR, "一時無効（SDIO RAM調整中）");
    return;
#else
    s_starting = true;
    s_retries = 0;
    s_ntp_waits = 0;
    if (s_watchdog)
        esp_timer_start_periodic(s_watchdog, TS_TICK_US);
    if (time_is_valid())
        lifecycle_post_cmd(TS_LIFECYCLE_CMD_START);
    else
        set_status(TS_ST_CONNECTING, "時刻同期中…");
#endif
}

/* reflect "key present + enabled + network up" into the idle state strings.
 * Armed but Wi-Fi not up yet (failure case: Wi-Fi never connects) shows a
 * network-wait rather than a misleading "connecting". */
static void refresh_idle_status(void)
{
    if (!system_vault_tailscale_has())
        set_status(TS_ST_NOT_CONFIGURED, "未設定");
    else if (!system_vault_tailscale_enabled())
        set_status(TS_ST_DISABLED, "オフ");
    else if (!s_net_up)
        set_status(TS_ST_CONNECTING, "ネットワーク待ち");
    else
        set_status(TS_ST_CONNECTING, "接続準備中");
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
    s_suspend_done = xSemaphoreCreateBinary();
    const esp_timer_create_args_t a = {
        .callback = watchdog_tick, .name = "ts_wd",
    };
    esp_timer_create(&a, &s_watchdog);
    /* single-owner lifecycle: all microlink start/stop runs on this task, so
       lwIP sockets never get opened from a callback (tcpip_thread) context. */
    s_lifecycle_queue = xQueueCreate(4, sizeof(ts_lifecycle_cmd_t));
    xTaskCreate(lifecycle_task, "ts_lifecycle", 4096, NULL, 5, NULL);
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
            set_status(TS_ST_ERROR, "前回接続中にリセット。手動で再試行/電源入れ直し");
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
    snprintf(out->state, sizeof out->state, "%s", ts_state_str(s_state));
    snprintf(out->detail, sizeof out->detail, "%s", s_detail);
    out->retries = s_retries;
    out->configured = system_vault_tailscale_has();
    out->enabled = system_vault_tailscale_enabled();
    if (s_ml && s_state == TS_ST_CONNECTED) {
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
    /* teardown (microlink_stop/destroy) runs on the lifecycle task */
    lifecycle_post_cmd(TS_LIFECYCLE_CMD_STOP);
    return true;
}

void tailscale_adapter_reauth(void)
{
    lock();
    s_skip_autostart = false;   /* new key = explicit retry: clear the crash guard */
    s_guard_fails = 0;
    unlock();
    lifecycle_post_cmd(TS_LIFECYCLE_CMD_REAUTH);
}

void tailscale_adapter_forget(void)
{
    lifecycle_post_cmd(TS_LIFECYCLE_CMD_FORGET);
}

/* Camera scan exclusion. Posts SUSPEND and BLOCKS (bounded) on the lifecycle
 * task's stop-completion: when this returns, microlink's tasks/sockets/DMA are
 * down (or there was no session) and auto-start is inhibited until resume().
 * Runs on the camera owner task — never inside the lifecycle task — so there is
 * no self-wait. The persisted enabled flag / crash guard are untouched. */
void tailscale_adapter_suspend(void)
{
    if (!s_lifecycle_queue || !s_suspend_done)
        return;                              /* not initialized: nothing to stop */
    xSemaphoreTake(s_suspend_done, 0);       /* drop any stale completion */
    if (!lifecycle_post_cmd(TS_LIFECYCLE_CMD_SUSPEND))
        return;
    /* bounded wait: the SUSPEND handler runs stop_session() synchronously, but a
       START already queued ahead of it (start_microlink opens lwIP sockets) can
       delay processing — 8s covers that. A timeout only means slightly noisier
       contention for this scan, never a hang. */
    if (xSemaphoreTake(s_suspend_done, pdMS_TO_TICKS(8000)) != pdTRUE)
        ESP_LOGW(TAG, "suspend: stop-completion timed out");
}

void tailscale_adapter_resume(void)
{
    lifecycle_post_cmd(TS_LIFECYCLE_CMD_RESUME);
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
void tailscale_adapter_suspend(void) {}
void tailscale_adapter_resume(void) {}
void tailscale_adapter_reauth(void) {}
void tailscale_adapter_forget(void) {}

#endif
