/* Host unit test for System Vault's purpose-built, write-only data model.
 *
 *   gcc -O2 -I.. -o /tmp/system_vault_test system_vault_test.c \
 *       ../system_vault.c && /tmp/system_vault_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "system_vault.h"

int main(void)
{
    char ssid[33], pass[65], key[256];

    assert(!system_vault_wifi_has());
    assert(!system_vault_tailscale_has());
    assert(!system_vault_wifi_set("", "secret"));
    assert(!system_vault_wifi_set("ssid", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                          "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));

    assert(system_vault_wifi_set("home", "secret"));
    assert(system_vault_wifi_has());
    assert(system_vault_wifi_ssid(ssid, sizeof ssid));
    assert(!strcmp(ssid, "home"));
    assert(system_vault_wifi_read(ssid, sizeof ssid, pass, sizeof pass));
    assert(!strcmp(pass, "secret"));
    memset(pass, 0, sizeof pass);
    assert(system_vault_wifi_forget());
    assert(!system_vault_wifi_has());

    assert(system_vault_tailscale_set("tskey-auth-test"));
    assert(system_vault_tailscale_has());
    assert(system_vault_tailscale_read(key, sizeof key));
    assert(!strcmp(key, "tskey-auth-test"));
    memset(key, 0, sizeof key);

    /* enabled flag: defaults ON, toggles, and forget resets to ON */
    assert(system_vault_tailscale_enabled());           /* default ON */
    assert(system_vault_tailscale_set_enabled(false));
    assert(!system_vault_tailscale_enabled());
    assert(system_vault_tailscale_set_enabled(true));
    assert(system_vault_tailscale_enabled());
    assert(system_vault_tailscale_set_enabled(false));
    assert(system_vault_tailscale_forget());
    assert(!system_vault_tailscale_has());
    assert(system_vault_tailscale_enabled());           /* forget -> default ON */

    puts("PASS: System Vault set/read/forget, enabled flag, and limits");
    return 0;
}
