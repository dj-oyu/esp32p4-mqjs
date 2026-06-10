#pragma once

#include <stddef.h>

/* Mount the LittleFS "storage" partition (formats it on first boot).
 * Safe to call once at startup; returns true on success. */
_Bool storage_init(void);

/* Return the persisted task as a malloc'd NUL-terminated string, or NULL
 * if none is stored / storage is unavailable. Caller owns the buffer. */
char *storage_load_task(void);

/* Persist a verified task (overwrites any previous one). */
void storage_save_task(const char *src, size_t len);
