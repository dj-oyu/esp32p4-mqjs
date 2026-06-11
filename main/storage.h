#pragma once

#include <stddef.h>

/* Mount the LittleFS "storage" partition (formats it on first boot).
 * Safe to call once at startup; returns true on success. */
_Bool storage_init(void);

/* Return the persisted task as a malloc'd buffer (NUL-terminated for
 * convenience, but *len is authoritative - bytecode tasks contain NULs),
 * or NULL if none is stored. Caller owns the buffer. */
char *storage_load_task(size_t *len);

/* Persist a verified task (overwrites any previous one). */
void storage_save_task(const char *src, size_t len);

/* Install a verified app to /littlefs/apps/<name>.js (P4b-lite: the
 * "// @app <name>" push directive). `name` must already be sanitized.
 * Returns true on success. */
_Bool storage_save_app(const char *name, const char *src, size_t len);
