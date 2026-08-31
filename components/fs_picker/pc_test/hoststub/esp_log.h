#pragma once
#include <stdio.h>
int esp_log_shim(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define ESP_LOGI(tag, fmt, ...) esp_log_shim("%s " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_shim("%s " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) esp_log_shim("%s " fmt, tag, ##__VA_ARGS__)
