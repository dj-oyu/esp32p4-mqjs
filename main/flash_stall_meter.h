#pragma once

/* フラッシュ停止の実測 (docs/native-editor-design.md §6.4)。
   CONFIG_MQJS_FLASH_STALL_METER が無効なビルドでは no-op なので、
   常に呼んでよい。 */
void flash_stall_meter_start(void);
void flash_stall_meter_report(const char *why);
void flash_stall_meter_reset(void);
