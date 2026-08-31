/* Host stub: the log goes to stdout so a failing run shows the boot line
   (spec §A.3 wants the internal-SRAM largest-free-block number in it). */
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, ...) do { printf("E %s: ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGW(tag, ...) do { printf("W %s: ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGI(tag, ...) do { printf("I %s: ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
