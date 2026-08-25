/*
 * Tailscale (microlink) lifecycle adapter.
 *
 * Bridges the platform-owned System Vault (auth key + on/off flag) to the
 * vendored microlink client. Startup is driven from the Wi-Fi got-IP callback
 * chain (no polling): on_net_up -> NTP sync -> microlink_start. A bounded
 * connect watchdog stops the session on repeated failure so a missing/invalid
 * auth key never spins forever (microlink itself auto-reconnects ~5-10s and
 * does not distinguish auth rejection from a transient network drop).
 *
 * Secret hygiene: microlink_init keeps the caller's auth_key POINTER (shallow
 * config copy, no strdup), so the key must outlive the session. The adapter
 * owns a heap copy for exactly the active session and zeroes+frees it on stop.
 *
 * All functions are safe to call when CONFIG_MQJS_TAILSCALE is off (no-op
 * stubs returning a "not-configured" status), so Stamp builds are unaffected.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char state[20];   /* not-configured|disabled|connecting|connected|error */
    /* human-readable, never contains secrets. 128 because the details are
       Japanese: the longest ("前回接続中にリセット。手動で再試行/電源入れ直し")
       is 70 UTF-8 bytes, so 64 truncated it mid-sequence — the UI drew a
       replacement glyph at the end. -O2 caught this; -Og did not. */
    char detail[128];
    int retries;      /* connect attempts so far (live during connecting) */
    char ip[16];      /* Tailscale VPN IP "100.x.y.z", "" if none */
    int peers;        /* active peer count when connected */
    bool configured;  /* vault holds an auth key */
    bool enabled;     /* user on/off preference */
} tailscale_status_t;

/* Call once at boot, before wifi_start(). */
void tailscale_adapter_init(void);

/* Hook from on_net_up() (got-IP event context). Arms the session when a key is
 * present and enabled; otherwise no-op. The actual microlink start waits for
 * time sync (TAI64N), driven by tailscale_adapter_on_time_synced(). */
void tailscale_adapter_on_net_up(void);

/* Hook from the Wi-Fi SNTP sync callback (wifi_set_time_sync_cb). Starts
 * microlink once the wall clock is valid and the session is armed. */
void tailscale_adapter_on_time_synced(void);

/* Snapshot the current status (copied; safe to read any time). */
/* Called from the microlink state task each time the tailnet reaches
   CONNECTED. Keep it short and non-blocking; it runs under the adapter lock,
   so it must not call back into this component. */
void tailscale_adapter_set_connected_cb(void (*fn)(void));

void tailscale_adapter_get_status(tailscale_status_t *out);

/* User on/off. enable() persists the flag and starts if the network is up;
 * disable() persists and tears down the session (keeps the key). */
bool tailscale_adapter_enable(void);
bool tailscale_adapter_disable(void);

/* Temporary network exclusion for an external exclusive user (the camera
 * scanner). suspend() stops the live microlink session WITHOUT touching the
 * user's persisted enabled flag or the crash-loop guard, and BLOCKS (bounded)
 * until the session is observably stopped — the stop-completion point the
 * camera owner needs before relying on the exclusion. resume() re-arms the
 * session if it is still configured + enabled + the network is up. One
 * exclusive user (no nesting); both are idempotent and safe to call when
 * Tailscale is not configured/enabled (suspend still blocks auto-start for the
 * window and returns promptly). One-way dependency: the camera owner waits on
 * this adapter; this adapter never waits on the camera. */
void tailscale_adapter_suspend(void);
void tailscale_adapter_resume(void);

/* Re-start the session after the auth key changed (tailscaleSet). */
void tailscale_adapter_reauth(void);

/* Stop the session and clear the vault (tailscaleForget). */
void tailscale_adapter_forget(void);

#ifdef __cplusplus
}
#endif
