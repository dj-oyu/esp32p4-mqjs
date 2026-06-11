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

/* Install a verified app to /littlefs/apps/<name>.js (the "// @app"
 * push directive / the P4c registry). `name` must already be
 * sanitized. Returns true on success. */
_Bool storage_save_app(const char *name, const char *src, size_t len);

/* Read an installed app back (malloc'd, NUL-terminated, *len
 * authoritative) — the registry sync uses this for its idempotence
 * byte-compare. NULL when absent. Caller owns the buffer. */
char *storage_load_app(const char *name, size_t *len);

/* Remove an installed app (P4c tombstone / sys.uninstall). True when
 * it existed. */
_Bool storage_delete_app(const char *name);
