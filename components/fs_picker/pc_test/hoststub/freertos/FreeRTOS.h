#pragma once
#include <stdint.h>

typedef struct { int owner; int count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0, 0 }
void taskENTER_CRITICAL(portMUX_TYPE *m);
void taskEXIT_CRITICAL(portMUX_TYPE *m);

/* 本物では portmacro.h / projdefs.h から来る。値はデバイスの
   sdkconfig.tab5 (CONFIG_FREERTOS_HZ=100) に合わせてある —— この tick で
   pdMS_TO_TICKS(2) が 0 に潰れることが fs_pick_cancel の刻み幅の
   クランプの理由なので、ここを 1000 にするとその分岐が消えてしまう。 */
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100
#define pdMS_TO_TICKS(xTimeInMs)                                              \
    ((TickType_t)(((TickType_t)(xTimeInMs) * (TickType_t)configTICK_RATE_HZ) / \
                  (TickType_t)1000U))
