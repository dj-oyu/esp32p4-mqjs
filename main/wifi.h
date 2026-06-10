#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bring up WiFi station via the C6 addon (esp_wifi_remote) and block until
 * an IP is obtained or timeout_ms elapses (0 = wait forever).
 * Returns true once connected, false on timeout or if no SSID is configured.
 * Reconnects automatically on disconnect for the rest of the session. */
bool wifi_start_and_wait(uint32_t timeout_ms);
