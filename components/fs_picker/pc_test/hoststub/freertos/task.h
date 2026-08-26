#pragma once
#include "freertos/FreeRTOS.h"

/* 本物 (IDF 6.0.1 components/freertos/FreeRTOS-Kernel/include/freertos/task.h)
   と同じ形。PRIVILEGED_FUNCTION は空に落ちるので省く。 */
typedef struct tskTaskControlBlock *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void         vTaskDelay(const TickType_t xTicksToDelay);
TickType_t   xTaskGetTickCount(void);
