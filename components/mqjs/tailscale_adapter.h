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
    char detail[64];  /* human-readable, never contains secrets */
    int retries;      /* connect attempts so far (live during connecting) */
    char ip[16];      /* Tailscale VPN IP "100.x.y.z", "" if none */
    int peers;        /* active peer count when connected */
    bool configured;  /* vault holds an auth key */
    bool enabled;     /* user on/off preference */
} tailscale_status_t;

/* Call once at boot, before wifi_start(). */
void tailscale_adapter_init(void);

/* Hook from on_net_up() (got-IP event context). Begins the NTP -> microlink
 * chain when a key is present and enabled; otherwise no-op. */
void tailscale_adapter_on_net_up(void);

/* Snapshot the current status (copied; safe to read any time). */
void tailscale_adapter_get_status(tailscale_status_t *out);

/* User on/off. enable() persists the flag and starts if the network is up;
 * disable() persists and tears down the session (keeps the key). */
bool tailscale_adapter_enable(void);
bool tailscale_adapter_disable(void);

/* Re-start the session after the auth key changed (tailscaleSet). */
void tailscale_adapter_reauth(void);

/* Stop the session and clear the vault (tailscaleForget). */
void tailscale_adapter_forget(void);

#ifdef __cplusplus
}
#endif
