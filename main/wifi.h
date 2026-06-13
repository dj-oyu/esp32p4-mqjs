#pragma once

/* Bring up the WiFi station via the C6 addon (esp_wifi_remote) and return
 * immediately — the connect is asynchronous, so nothing blocks waiting for an
 * IP. on_got_ip (may be NULL) fires once, on the first IP, to start the only
 * network-dependent service; everything else connects on demand. Reconnects
 * automatically on disconnect for the rest of the session. No-op if no SSID
 * is configured. */
void wifi_start(void (*on_got_ip)(void));
