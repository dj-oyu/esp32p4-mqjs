#include "system_vault.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"

#define NS "mqjs_sysvault"
#define K_WIFI      "wifi"
#define K_TS_AUTH   "ts_auth"
#define K_TS_ENABLED "ts_on"

typedef struct {
    uint8_t version;
    char ssid[SYSTEM_VAULT_WIFI_SSID_MAX + 1];
    char password[SYSTEM_VAULT_WIFI_PASS_MAX + 1];
} wifi_credential_t;

static nvs_handle_t s_handle;
static bool s_open;

static bool vault_open(void)
{
    if (s_open)
        return true;
    if (nvs_open(NS, NVS_READWRITE, &s_handle) != ESP_OK) {
        nvs_flash_init();
        if (nvs_open(NS, NVS_READWRITE, &s_handle) != ESP_OK)
            return false;
    }
    s_open = true;
    return true;
}

static bool get_string(const char *key, char *dst, size_t cap)
{
    if (!cap || !vault_open())
        return false;
    size_t len = cap;
    if (nvs_get_str(s_handle, key, dst, &len) != ESP_OK) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

static bool has_string(const char *key)
{
    if (!vault_open())
        return false;
    size_t len = 0;
    return nvs_get_str(s_handle, key, NULL, &len) == ESP_OK;
}

static bool erase_key(const char *key)
{
    if (!vault_open())
        return false;
    esp_err_t err = nvs_erase_key(s_handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return false;
    return nvs_commit(s_handle) == ESP_OK;
}

static bool wifi_get(wifi_credential_t *out)
{
    size_t len = sizeof *out;
    if (!vault_open() ||
        nvs_get_blob(s_handle, K_WIFI, out, &len) != ESP_OK ||
        len != sizeof *out || out->version != 1 ||
        out->ssid[SYSTEM_VAULT_WIFI_SSID_MAX] != '\0' ||
        out->password[SYSTEM_VAULT_WIFI_PASS_MAX] != '\0' ||
        out->ssid[0] == '\0') {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}
#else
static char s_wifi_ssid[SYSTEM_VAULT_WIFI_SSID_MAX + 1];
static char s_wifi_pass[SYSTEM_VAULT_WIFI_PASS_MAX + 1];
static char s_ts_auth[SYSTEM_VAULT_TS_AUTH_MAX + 1];
static bool s_ts_enabled = true;  /* default ON */

static bool copy_if_set(const char *src, char *dst, size_t cap)
{
    if (!src[0] || !cap)
        return false;
    snprintf(dst, cap, "%s", src);
    return true;
}
#endif

static bool valid_len(const char *value, size_t max, bool allow_empty)
{
    if (!value)
        return false;
    size_t len = strlen(value);
    return (allow_empty || len > 0) && len <= max;
}

bool system_vault_wifi_set(const char *ssid, const char *password)
{
    if (!valid_len(ssid, SYSTEM_VAULT_WIFI_SSID_MAX, false) ||
        !valid_len(password, SYSTEM_VAULT_WIFI_PASS_MAX, true))
        return false;
#ifdef ESP_PLATFORM
    wifi_credential_t value = { .version = 1 };
    snprintf(value.ssid, sizeof value.ssid, "%s", ssid);
    snprintf(value.password, sizeof value.password, "%s", password);
    if (!vault_open() ||
        nvs_set_blob(s_handle, K_WIFI, &value, sizeof value) != ESP_OK) {
        memset(&value, 0, sizeof value);
        return false;
    }
    bool ok = nvs_commit(s_handle) == ESP_OK;
    memset(&value, 0, sizeof value);
    return ok;
#else
    snprintf(s_wifi_ssid, sizeof s_wifi_ssid, "%s", ssid);
    snprintf(s_wifi_pass, sizeof s_wifi_pass, "%s", password);
    return true;
#endif
}

bool system_vault_wifi_has(void)
{
#ifdef ESP_PLATFORM
    wifi_credential_t value;
    bool ok = wifi_get(&value);
    memset(&value, 0, sizeof value);
    return ok;
#else
    return s_wifi_ssid[0] != '\0';
#endif
}

bool system_vault_wifi_ssid(char *dst, size_t cap)
{
#ifdef ESP_PLATFORM
    char password[SYSTEM_VAULT_WIFI_PASS_MAX + 1];
    bool ok = system_vault_wifi_read(dst, cap, password, sizeof password);
    memset(password, 0, sizeof password);
    return ok;
#else
    return copy_if_set(s_wifi_ssid, dst, cap);
#endif
}

bool system_vault_wifi_read(char *ssid, size_t ssid_cap,
                            char *password, size_t password_cap)
{
    if (!ssid_cap || !password_cap)
        return false;
#ifdef ESP_PLATFORM
    wifi_credential_t value;
    if (!wifi_get(&value)) {
        memset(ssid, 0, ssid_cap);
        memset(password, 0, password_cap);
        return false;
    }
    snprintf(ssid, ssid_cap, "%s", value.ssid);
    snprintf(password, password_cap, "%s", value.password);
    memset(&value, 0, sizeof value);
    return true;
#else
    if (!system_vault_wifi_has()) {
        ssid[0] = password[0] = '\0';
        return false;
    }
    snprintf(ssid, ssid_cap, "%s", s_wifi_ssid);
    snprintf(password, password_cap, "%s", s_wifi_pass);
    return true;
#endif
}

bool system_vault_wifi_forget(void)
{
#ifdef ESP_PLATFORM
    return erase_key(K_WIFI);
#else
    memset(s_wifi_ssid, 0, sizeof s_wifi_ssid);
    memset(s_wifi_pass, 0, sizeof s_wifi_pass);
    return true;
#endif
}

bool system_vault_tailscale_set(const char *auth_key)
{
    if (!valid_len(auth_key, SYSTEM_VAULT_TS_AUTH_MAX, false))
        return false;
#ifdef ESP_PLATFORM
    if (!vault_open() || nvs_set_str(s_handle, K_TS_AUTH, auth_key) != ESP_OK)
        return false;
    return nvs_commit(s_handle) == ESP_OK;
#else
    snprintf(s_ts_auth, sizeof s_ts_auth, "%s", auth_key);
    return true;
#endif
}

bool system_vault_tailscale_has(void)
{
#ifdef ESP_PLATFORM
    return has_string(K_TS_AUTH);
#else
    return s_ts_auth[0] != '\0';
#endif
}

bool system_vault_tailscale_read(char *dst, size_t cap)
{
#ifdef ESP_PLATFORM
    return get_string(K_TS_AUTH, dst, cap);
#else
    return copy_if_set(s_ts_auth, dst, cap);
#endif
}

bool system_vault_tailscale_forget(void)
{
#ifdef ESP_PLATFORM
    bool auth = erase_key(K_TS_AUTH);
    bool en = erase_key(K_TS_ENABLED);  /* back to default ON */
    return auth && en;
#else
    memset(s_ts_auth, 0, sizeof s_ts_auth);
    s_ts_enabled = true;
    return true;
#endif
}

bool system_vault_tailscale_enabled(void)
{
#ifdef ESP_PLATFORM
    uint8_t v;
    if (!vault_open() || nvs_get_u8(s_handle, K_TS_ENABLED, &v) != ESP_OK)
        return true;  /* never set -> default ON */
    return v != 0;
#else
    return s_ts_enabled;
#endif
}

bool system_vault_tailscale_set_enabled(bool enabled)
{
#ifdef ESP_PLATFORM
    if (!vault_open() ||
        nvs_set_u8(s_handle, K_TS_ENABLED, enabled ? 1 : 0) != ESP_OK)
        return false;
    return nvs_commit(s_handle) == ESP_OK;
#else
    s_ts_enabled = enabled;
    return true;
#endif
}
