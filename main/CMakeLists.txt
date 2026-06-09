idf_component_register(
    SRCS "app_main.c" "net_mqtt.c" "script_store.c"
    PRIV_REQUIRES mqjs mqtt nvs_flash esp_wifi esp_netif esp_event)
