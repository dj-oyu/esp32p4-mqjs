/* script_store.h - persist the active JS script in NVS so it survives
 * a reboot. */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a NUL-terminated heap buffer (caller frees) or NULL if no
   script is stored. *len receives the length without the NUL. */
char *script_store_load(size_t *len);

esp_err_t script_store_save(const char *src, size_t len);

void script_store_erase(void);

#ifdef __cplusplus
}
#endif
