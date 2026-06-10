#pragma once

/* Enable the C6 power rail on M5Stack Tab5 (no-op unless
 * CONFIG_MQJS_TAB5_POWER is set). Call before wifi_start_and_wait(). */
#if CONFIG_MQJS_TAB5_POWER
void board_tab5_power_init(void);
#else
static inline void board_tab5_power_init(void) {}
#endif
