/* Host integration test for immutable system-app authorization.
 *
 * Build with the same sources as run_pc. The ordinary launcher-shaped script
 * cannot write System Vault even after renaming itself; the embedded system
 * source can.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqjs_runtime.h"
#include "system_vault.h"

static const char SYSTEM_SRC[] =
    "system.wifiSet('trusted-ssid', 'trusted-pass');\n"
    "system.tailscaleSet('tskey-auth-trusted');\n";

static const char ORDINARY_SRC[] =
    "sys.setAppName('device_settings');\n"
    "try { system.wifiSet('evil-ssid', 'evil-pass'); } catch (e) {}\n"
    "sys.open('trusted_settings');\n";

int main(void)
{
    mqjs_register_system_app_source("trusted_settings", SYSTEM_SRC,
                                    strlen(SYSTEM_SRC));
    void *mem = malloc(MQJS_APP_MEM_SIZE);
    assert(mem);
    assert(mqjs_run_script(ORDINARY_SRC, strlen(ORDINARY_SRC), "ordinary",
                           mem, MQJS_APP_MEM_SIZE) == 0);

    char ssid[33], pass[65], auth[256];
    assert(system_vault_wifi_read(ssid, sizeof ssid, pass, sizeof pass));
    assert(!strcmp(ssid, "trusted-ssid"));
    assert(!strcmp(pass, "trusted-pass"));
    assert(system_vault_tailscale_read(auth, sizeof auth));
    assert(!strcmp(auth, "tskey-auth-trusted"));
    memset(pass, 0, sizeof pass);
    memset(auth, 0, sizeof auth);
    puts("PASS: only immutable embedded system source wrote System Vault");
    return 0;
}
