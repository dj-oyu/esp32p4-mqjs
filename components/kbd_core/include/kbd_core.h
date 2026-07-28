/*
 * kbd_core — the keyboard semantics every input surface shares.
 *
 * Two surfaces feed the same apps: the Tab5 keyboard dock (kbd_tab5,
 * physical key edges) and the on-screen keyboard / control bar
 * (ui_tab5, touch taps). Before this component each had its own idea of
 * what Ctrl+C or an arrow key should put on the wire, so the same
 * intent reached apps as different bytes. Everything that decides
 * "which bytes does this keypress become" lives here instead:
 *
 *   - the modifier state machine (momentary / one-shot / double-tap
 *     lock), identical on both surfaces,
 *   - the named-key vocabulary: which keys travel as raw control
 *     characters and which as "\0name" tokens whose meaning is app-side
 *     (design §7 keytoken),
 *   - the translation itself (Shift uppercases, Ctrl folds to a control
 *     byte, Alt prefixes ESC).
 *
 * Pure logic: no I/O, no LVGL, no clipboard access — callers post the
 * bytes. A kbd_mods_t is owned by ONE surface (hence one task), so
 * nothing here locks: the dock's held Ctrl and a touch one-shot Ctrl
 * are deliberately separate states.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keys that are not a printable character. The split matters: the raw
   ones are ordinary input bytes, the rest travel as "\0name" tokens so
   an app (terminal vs launcher) decides what they mean. */
typedef enum {
    KBD_K_NONE = 0, /* printable: the char is passed separately */
    /* raw control characters */
    KBD_K_BS,       /* "\b"  */
    KBD_K_TAB,      /* "\t"  */
    KBD_K_ENTER,    /* "\n"  */
    /* "\0name" tokens */
    KBD_K_ESC,
    KBD_K_DEL,
    KBD_K_UP,
    KBD_K_DOWN,
    KBD_K_LEFT,
    KBD_K_RIGHT,
    KBD_K_HOME,
    KBD_K_END,
    KBD_K_PGUP,
    KBD_K_PGDN,
    KBD_K_F1,
    KBD_K_F2,
    KBD_K_F3,
    KBD_K_F4,
    KBD_K_F5,
    KBD_K_F6,
    KBD_K_F7,
    KBD_K_F8,
    KBD_K_F9,
    KBD_K_F10,
    KBD_K_F11,
    KBD_K_F12,
    KBD_K_COPY,
    KBD_K_PASTE,
} kbd_key_t;

/* One modifier. Three ways to be in effect, all supported everywhere:
     held    — momentary, while the key is physically down (dock) ;
     oneshot — armed by a bare tap, applies to the next key only ;
     lock    — latched by a bare double-tap until tapped again.
   `used` remembers that another key was pressed during the hold, which
   disqualifies the release from counting as a tap — without it, fast
   chording (sym+9 sym+9 …) would latch the lock by accident. */
typedef struct {
    bool held;
    bool oneshot;
    bool lock;
    bool used;
    int64_t press_us;
    int64_t release_us;
    int clicks;
} kbd_mod_t;

/* The four modifiers a surface tracks. `shift` is the dock's Aa key. */
typedef struct {
    kbd_mod_t sym;
    kbd_mod_t shift;
    kbd_mod_t ctrl;
    kbd_mod_t alt;
} kbd_mods_t;

/* tap / double-tap window, from the dock firmware's own value */
#define KBD_TAP_US 400000

/* Longest sequence kbd_translate can produce (token names included). */
#define KBD_SEQ_MAX 16

void kbd_mods_reset(kbd_mods_t *m);

/* in effect for the next keystroke? (any of held / lock / oneshot) */
bool kbd_mod_armed(const kbd_mod_t *m);
/* in effect and NOT because of a one-shot — i.e. it stays for the key
   after this one. Layer selection (the dock's sym) wants this. */
bool kbd_mod_active(const kbd_mod_t *m);

/* Physical key edge. `oneshot_arm` allows a bare tap to arm a one-shot:
   true for Shift-like keys, false where a stray tap would be dangerous
   (a one-shot Ctrl plus a later 'c' would read as SIGINT). */
void kbd_mod_edge(kbd_mod_t *m, bool pressed, bool oneshot_arm);

/* Touch tap: a button that cannot be "held" while another key is hit,
   so press and release land together. Same state machine, hence the
   same tap / double-tap-lock behavior as the dock. */
void kbd_mod_tap(kbd_mod_t *m, bool oneshot_arm);

/* A non-modifier key was pressed. Whatever is held right now was used
   as a chord, so its release must not count as a tap, and every pending
   tap chain is broken: a double-tap lock requires two consecutive taps
   of the same key with nothing typed in between. */
void kbd_mods_mark_chord(kbd_mods_t *m);

/* A key was emitted: one-shots are spent (locks and holds survive). */
void kbd_mods_consume(kbd_mods_t *m);

/* Bytes an app receives for this keypress. `key` names a non-printable
 * key, or is KBD_K_NONE and `base` carries the character the surface
 * resolved (already layer-shifted; case is applied here). Returns the
 * length written, 0 when the key produces nothing. `repeats` (optional)
 * says whether typematic repeat is appropriate. Does NOT change `m` —
 * call kbd_mods_consume() once the bytes are posted. */
size_t kbd_translate(const kbd_mods_t *m, char base, kbd_key_t key,
                     char out[KBD_SEQ_MAX], bool *repeats);

/* One-line preview of clipboard text for a button/label: control
 * characters and runs of blanks collapse to single spaces and the tail
 * is ellipsized, never cutting a UTF-8 sequence. Returns strlen(out). */
size_t kbd_clip_format(const char *data, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
