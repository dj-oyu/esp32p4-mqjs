#pragma once

/* auto-suspend の受け入れ試験を起動する (docs/filer-storage-design.md の
   議論、実体は flash_suspend_test.c 冒頭)。CONFIG_MQJS_FLASH_SUSPEND_TEST が
   無効なビルドでは何もしない no-op なので、常に呼んでよい。 */
void flash_suspend_test_start(void);
