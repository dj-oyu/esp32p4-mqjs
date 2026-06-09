/*
 * net_mqtt.h - WiFi (via the Stamp-AddOn C6 / esp_wifi_remote) and the
 * MQTT script-distribution channel.
 *
 * Topics (PREFIX = CONFIG_MQJS_TOPIC_PREFIX, ID = efuse MAC hex):
 *   PREFIX/<ID>/script   <- JS source (retain recommended); empty
 *                           retained payload clears the stored script
 *   PREFIX/<ID>/cmd      <- "restart" | "stop" | "clear"
 *   PREFIX/<ID>/status   -> retained JSON {"state":...,"rc":...}
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up netif + WiFi STA and connect to the broker once an IP is
   obtained. No-op (with a warning) if CONFIG_MQJS_WIFI_SSID is empty. */
void net_mqtt_start(void);

/* Publish a retained status message; silently dropped while offline. */
void net_mqtt_publish_status(const char *state, int rc);

#ifdef __cplusplus
}
#endif
