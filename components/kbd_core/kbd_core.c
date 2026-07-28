/* kbd_core — shared keyboard semantics. See kbd_core.h for the why. */
#include <ctype.h>
#include <string.h>

#include "esp_timer.h"
#include "kbd_core.h"

/* ---- modifier state machine ----------------------------------------- */

void kbd_mods_reset(kbd_mods_t *m)
{
    memset(m, 0, sizeof *m);
}

bool kbd_mod_armed(const kbd_mod_t *m)
{
    return m->held || m->lock || m->oneshot;
}

bool kbd_mod_active(const kbd_mod_t *m)
{
    return m->held || m->lock;
}

void kbd_mod_edge(kbd_mod_t *m, bool pressed, bool oneshot_arm)
{
    int64_t now = esp_timer_get_time();

    if (pressed) {
        m->held = true;
        m->used = false;
        m->press_us = now;
        /* consecutive taps only chain while they stay inside the window */
        m->clicks = (now - m->release_us < KBD_TAP_US) ? m->clicks + 1 : 1;
        return;
    }

    m->held = false;
    m->release_us = now;

    if (m->used || now - m->press_us >= KBD_TAP_US) {
        m->clicks = 0; /* a hold, or a chord: not a tap */
        return;
    }
    if (m->clicks >= 2) {
        m->lock = true; /* double-tap: latch until tapped again */
        m->oneshot = false;
        m->clicks = 0;
    } else if (m->lock || m->oneshot) {
        m->lock = false; /* tap while latched or armed: clear */
        m->oneshot = false;
        m->clicks = 0;
    } else if (oneshot_arm) {
        m->oneshot = true;
    }
}

void kbd_mod_tap(kbd_mod_t *m, bool oneshot_arm)
{
    kbd_mod_edge(m, true, oneshot_arm);
    kbd_mod_edge(m, false, oneshot_arm);
}

void kbd_mods_mark_chord(kbd_mods_t *m)
{
    m->sym.used |= m->sym.held;
    m->shift.used |= m->shift.held;
    m->ctrl.used |= m->ctrl.held;
    m->alt.used |= m->alt.held;
}

void kbd_mods_consume(kbd_mods_t *m)
{
    m->sym.oneshot = false;
    m->shift.oneshot = false;
    m->ctrl.oneshot = false;
    m->alt.oneshot = false;
}

/* ---- translation ---------------------------------------------------- */

/* Token name for the keys whose meaning belongs to the app, NULL for
   the ones that are plain input bytes. One table so the dock and the
   on-screen surfaces can never drift apart. */
static const char *key_token(kbd_key_t k)
{
    switch (k) {
    case KBD_K_ESC:   return "esc";
    case KBD_K_DEL:   return "del";
    case KBD_K_UP:    return "up";
    case KBD_K_DOWN:  return "down";
    case KBD_K_LEFT:  return "left";
    case KBD_K_RIGHT: return "right";
    case KBD_K_HOME:  return "home";
    case KBD_K_END:   return "end";
    case KBD_K_PGUP:  return "pgup";
    case KBD_K_PGDN:  return "pgdn";
    case KBD_K_F1:    return "f1";
    case KBD_K_F2:    return "f2";
    case KBD_K_F3:    return "f3";
    case KBD_K_F4:    return "f4";
    case KBD_K_F5:    return "f5";
    case KBD_K_F6:    return "f6";
    case KBD_K_F7:    return "f7";
    case KBD_K_F8:    return "f8";
    case KBD_K_F9:    return "f9";
    case KBD_K_F10:   return "f10";
    case KBD_K_F11:   return "f11";
    case KBD_K_F12:   return "f12";
    case KBD_K_COPY:  return "copy";
    case KBD_K_PASTE: return "paste";
    default:          return NULL;
    }
}

/* Held-down repeat makes sense for anything that moves or deletes, not
   for one-shot actions (Esc, clipboard) or a line commit. */
static bool key_repeats(kbd_key_t k)
{
    switch (k) {
    case KBD_K_ESC:
    case KBD_K_COPY:
    case KBD_K_PASTE:
    case KBD_K_ENTER:
        return false;
    default:
        return true;
    }
}

size_t kbd_translate(const kbd_mods_t *m, char base, kbd_key_t key,
                     char out[KBD_SEQ_MAX], bool *repeats)
{
    bool rep = key_repeats(key);
    char c;

    const char *tok = key_token(key);
    if (tok) {
        size_t n = strlen(tok);
        if (n + 1 > KBD_SEQ_MAX)
            return 0;
        out[0] = '\0'; /* NUL sentinel: "\0name" (design §7 keytoken) */
        memcpy(out + 1, tok, n);
        if (repeats)
            *repeats = rep;
        return n + 1;
    }

    switch (key) {
    case KBD_K_NONE:  c = base; break;
    case KBD_K_BS:    c = '\b'; break;
    case KBD_K_TAB:   c = '\t'; break;
    case KBD_K_ENTER: c = '\n'; break;
    default:          return 0; /* a token this build forgot to name */
    }
    if (!c)
        return 0;

    if (c >= 'a' && c <= 'z' && kbd_mod_armed(&m->shift))
        c = (char)toupper((unsigned char)c);

    size_t n = 0;
    if (kbd_mod_armed(&m->alt)) {
        out[n++] = '\x1b'; /* Meta as an ESC prefix, like every terminal */
        rep = false;       /* repeating ESC-x is never what you meant */
    }
    if (kbd_mod_armed(&m->ctrl)) {
        /* Ctrl+letter and Ctrl+@[\]^_ fold to a control byte; anything
           else passes through unchanged. Ctrl+Space would be NUL, which
           collides with the "\0name" token marker, so it is dropped. */
        char uc = (char)toupper((unsigned char)c);
        if (uc == ' ')
            return 0;
        if (uc >= '@' && uc <= '_')
            c = (char)(uc & 0x1f);
    }
    out[n++] = c;

    if (c == '\n')
        rep = false;
    if (repeats)
        *repeats = rep;
    return n;
}

/* ---- clipboard preview --------------------------------------------- */

size_t kbd_clip_format(const char *data, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!data)
        return 0;

    /* room for the ellipsis (U+2026, 3 bytes) plus the terminator */
    size_t limit = cap > 4 ? cap - 4 : 0;
    size_t n = 0;
    bool blank = true; /* also trims leading whitespace */
    bool cut = false;

    /* copy whole UTF-8 sequences: a preview must never end mid-sequence
       (the label would render a replacement glyph) */
    for (const unsigned char *p = (const unsigned char *)data; *p;) {
        unsigned char ch = *p;
        size_t len = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
        for (size_t i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                len = 0; /* truncated or invalid: drop the lead byte */
                break;
            }
        }
        if (len == 0 || (len == 1 && ch >= 0x80)) {
            p++; /* stray continuation byte: not displayable */
            continue;
        }
        if (len == 1 && (ch < 0x20 || ch == 0x7f || ch == ' ')) {
            p++;
            if (blank)
                continue; /* collapse runs — newlines and tabs included */
            if (n + 1 > limit) {
                cut = true;
                break;
            }
            out[n++] = ' ';
            blank = true;
            continue;
        }
        if (n + len > limit) {
            cut = true;
            break;
        }
        memcpy(out + n, p, len);
        n += len;
        p += len;
        blank = false;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--; /* no dangling blank before the ellipsis or terminator */
    if (cut) {
        memcpy(out + n, "\xE2\x80\xA6", 3); /* U+2026 */
        n += 3;
    }
    out[n] = '\0';
    return n;
}
