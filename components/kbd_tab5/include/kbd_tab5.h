/*
 * M5Stack Tab5 keyboard dock (A164) — hot-pluggable I2C key event source.
 *
 * Feeds decoded key sequences into mqjs_post_key(), i.e. apps see dock
 * keys through the exact same ui.onKey vocabulary as the on-screen
 * keyboard and control bar: printable UTF-8, "\n", "\b", "\t",
 * "\x00name" tokens (esc/del/arrows/home/end/pgup/pgdn/ins/f1..f12),
 * Ctrl+letter as control bytes, Alt as ESC prefix.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the keyboard task (returns immediately). Probes the dock in the
 * background and keeps probing: dock attach/detach is handled at any
 * time. Safe to call once at boot, before or after the UI is up. */
void kbd_tab5_start(void);

/* Dock currently attached and answering? */
bool kbd_tab5_present(void);

/* Presence transitions (attach/detach), called from the keyboard task
 * (never from an ISR). Used by the platform to flip the screen into
 * landscape while the Tab5 sits in the dock. */
typedef void (*kbd_tab5_presence_cb_t)(bool present);
void kbd_tab5_set_presence_cb(kbd_tab5_presence_cb_t cb);

#ifdef __cplusplus
}
#endif
