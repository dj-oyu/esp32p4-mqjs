/*
 * Platform-owned write-only credential storage.
 *
 * JS can only reach purpose-built setters/status functions guarded by the
 * immutable system-app bit. Native services use these read functions and must
 * zero their caller-owned buffers after use.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_VAULT_WIFI_SSID_MAX 32
#define SYSTEM_VAULT_WIFI_PASS_MAX 64
#define SYSTEM_VAULT_TS_AUTH_MAX   255

bool system_vault_wifi_set(const char *ssid, const char *password);
bool system_vault_wifi_has(void);
bool system_vault_wifi_ssid(char *dst, size_t cap);
bool system_vault_wifi_read(char *ssid, size_t ssid_cap,
                            char *password, size_t password_cap);
bool system_vault_wifi_forget(void);

bool system_vault_tailscale_set(const char *auth_key);
bool system_vault_tailscale_has(void);
bool system_vault_tailscale_read(char *dst, size_t cap);
bool system_vault_tailscale_forget(void);

#ifdef __cplusplus
}
#endif
