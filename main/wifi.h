#pragma once

/* Bring up the WiFi station via the C6 addon (esp_wifi_remote) and return
 * immediately — the connect is asynchronous, so nothing blocks waiting for an
 * IP. on_got_ip (may be NULL) fires once, on the first IP, to start the only
 * network-dependent service; everything else connects on demand. Reconnects
 * automatically on disconnect for the rest of the session. No-op if no SSID
 * is configured. */
void wifi_start(void (*on_got_ip)(void));

/* Register a callback fired each time SNTP sets the wall clock (first sync and
 * periodic re-syncs). Set before wifi_start(). Used by the Tailscale adapter,
 * whose ts2021 handshake needs real time (TAI64N). No-op if NULL. */
void wifi_set_time_sync_cb(void (*cb)(void));
