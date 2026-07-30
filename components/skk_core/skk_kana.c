/*
 * skk_kana.c — romaji->kana and the SKK state machine. See skk_core.h
 * for the contract; this file is everything except the dictionary
 * (collation table, packed key, CRC32, image open, binary search —
 * those live in skk_dict.c and are only *called* from here).
 *
 * Pure logic: <string.h> and skk_core.h, nothing else. No allocation,
 * no clock, no ESP-IDF. Every byte the engine writes lands in a fixed
 * field of the caller's skk_t.
 *
 * Everything is UTF-8 end to end. There is no wchar_t and no UTF-16
 * anywhere: ui.cells, ssh_vt and mqjs are all UTF-8, so a transcode in
 * the middle would be pure loss. "One character back" is
 * skk_utf8_prev(), the same walk cells_utf8_next() does in ui_tab5.
 */
#include <string.h>

#include "skk_core.h"

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */

/* Bytes in the sequence a lead byte opens, or 0 when `b` cannot lead
   one (a continuation byte, or an overlong / out-of-range lead). */
static int u8_seq_len(unsigned char b)
{
    if (b < 0x80) return 1;
    if (b < 0xC2) return 0; /* 0x80-0xBF continuation, 0xC0/0xC1 overlong */
    if (b < 0xE0) return 2;
    if (b < 0xF0) return 3;
    if (b < 0xF5) return 4;
    return 0;
}

size_t skk_utf8_next(const char *s, size_t len, size_t i)
{
    if (!s || i >= len) return len;

    int n = u8_seq_len((unsigned char)s[i]);
    if (n <= 1) return i + 1; /* ASCII, or a stray byte: advance by one */

    /* Stop at the first byte that is not a continuation, so a truncated
       sequence eats only itself and never the character after it. */
    size_t j = i + 1;
    while (j < len && j < i + (size_t)n && ((unsigned char)s[j] & 0xC0) == 0x80)
        j++;
    return j;
}

size_t skk_utf8_prev(const char *s, size_t len, size_t i)
{
    if (!s || i == 0) return 0;
    if (i > len) i = len;

    size_t j = i;
    while (j > 0 && i - j < 4) {
        j--;
        if (((unsigned char)s[j] & 0xC0) != 0x80) break;
    }
    /* Only accept the walk when the lead byte agrees with the distance;
       otherwise the tail is malformed and we retreat by exactly one. */
    int n = u8_seq_len((unsigned char)s[j]);
    return (n >= 1 && (size_t)n == i - j) ? j : i - 1;
}

uint32_t skk_utf8_decode(const char *s, size_t len, size_t i, size_t *adv)
{
    static const uint32_t min_cp[5] = { 0, 0, 0x80, 0x800, 0x10000 };

    if (adv) *adv = 1;
    if (!s || i >= len) return 0xFFFD;

    unsigned char b = (unsigned char)s[i];
    int n = u8_seq_len(b);
    if (n == 1) return b;
    if (n == 0 || (size_t)n > len - i) return 0xFFFD;

    uint32_t cp = (uint32_t)(b & (0xFF >> (n + 1)));
    for (int k = 1; k < n; k++) {
        unsigned char c = (unsigned char)s[i + (size_t)k];
        if ((c & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (uint32_t)(c & 0x3F);
    }
    if (cp < min_cp[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0xFFFD; /* overlong, surrogate or out of range */

    if (adv) *adv = (size_t)n;
    return cp;
}

/* Encode `cp` into `out` (4 bytes are always enough). Returns the
   length. Never called with a scalar this file did not just decode. */
static size_t u8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Hiragana -> katakana, in place of a second table: the two blocks are
   0x60 apart across U+3041-U+3096 (small a .. ke, including U+3094 vu),
   and everything else — the prolonged sound mark, punctuation, ASCII —
   is already shared. One table cannot drift out of step with itself.
   Returns the byte length written, 0 when it would not fit. */
static size_t to_katakana(const char *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;

    for (size_t i = 0; i < len;) {
        size_t adv = 1;
        uint32_t cp = skk_utf8_decode(in, len, i, &adv);

        if (cp == 0xFFFD) { /* malformed: copy the byte through verbatim */
            if (o + 1 > cap) return 0;
            out[o++] = in[i];
            i += adv;
            continue;
        }
        if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;

        size_t need = (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
        if (need > cap - o) return 0;
        o += u8_encode(cp, out + o);
        i += adv;
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Romaji -> kana table                                                */

/* One flat, prefix-searchable table. `r` is the romaji, `k` the
 * hiragana; katakana is derived (see to_katakana), so there is exactly
 * one place where "ki + small ya" is spelled out.
 *
 * The table is scanned linearly on every keystroke. That is ~350
 * memcmp of at most 4 bytes against a budget of 55 ms per key (the
 * dock's typematic tick, kbd_tab5.c), i.e. three orders of magnitude of
 * slack — an index would buy nothing and cost a second thing to keep
 * consistent.
 *
 * Sokuon and hatsuon are NOT in the table because they are not
 * romaji->kana rules, they are what happens when a rule fails to
 * match: a doubled consonant emits small tsu (roma_feed's dead-end
 * path), and bare "n" resolves to hatsuon when the next key cannot
 * extend it. Handling them as fallbacks is what makes "kkk", "tch" and
 * "n" + punctuation all fall out of one loop.
 *
 * Deliberately absent: "z"-prefixed symbol shortcuts (z- z. z,), the
 * abbrev "/" and the numeric "#" entries — v1 scope (design doc §5).
 * Also absent are wi/we as the archaic kana; they map to the modern
 * "u + small i/e" as every current IME does. */
typedef struct {
    char r[5];  /* romaji, NUL-terminated; longest is "ltsu" */
    char k[12]; /* hiragana, NUL-terminated; longest is "nnya" = 9 B */
} roma_ent_t;

static const roma_ent_t ROMA[] = {
    /* vowels and the small vowels */
    {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
    {"xa","ぁ"},{"xi","ぃ"},{"xu","ぅ"},{"xe","ぇ"},{"xo","ぉ"},
    {"la","ぁ"},{"li","ぃ"},{"lu","ぅ"},{"le","ぇ"},{"lo","ぉ"},

    /* k / g */
    {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
    {"kya","きゃ"},{"kyi","きぃ"},{"kyu","きゅ"},{"kye","きぇ"},{"kyo","きょ"},
    {"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
    {"gya","ぎゃ"},{"gyi","ぎぃ"},{"gyu","ぎゅ"},{"gye","ぎぇ"},{"gyo","ぎょ"},

    /* s / z */
    {"sa","さ"},{"si","し"},{"su","す"},{"se","せ"},{"so","そ"},
    {"sha","しゃ"},{"shi","し"},{"shu","しゅ"},{"she","しぇ"},{"sho","しょ"},
    {"sya","しゃ"},{"syi","しぃ"},{"syu","しゅ"},{"sye","しぇ"},{"syo","しょ"},
    {"za","ざ"},{"zi","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
    {"zya","じゃ"},{"zyi","じぃ"},{"zyu","じゅ"},{"zye","じぇ"},{"zyo","じょ"},
    {"ja","じゃ"},{"ji","じ"},{"ju","じゅ"},{"je","じぇ"},{"jo","じょ"},
    {"jya","じゃ"},{"jyi","じぃ"},{"jyu","じゅ"},{"jye","じぇ"},{"jyo","じょ"},

    /* t / d */
    {"ta","た"},{"ti","ち"},{"tu","つ"},{"te","て"},{"to","と"},
    {"tsu","つ"},{"tsa","つぁ"},{"tsi","つぃ"},{"tse","つぇ"},{"tso","つぉ"},
    {"cha","ちゃ"},{"chi","ち"},{"chu","ちゅ"},{"che","ちぇ"},{"cho","ちょ"},
    {"tya","ちゃ"},{"tyi","ちぃ"},{"tyu","ちゅ"},{"tye","ちぇ"},{"tyo","ちょ"},
    {"cya","ちゃ"},{"cyi","ちぃ"},{"cyu","ちゅ"},{"cye","ちぇ"},{"cyo","ちょ"},
    {"tha","てゃ"},{"thi","てぃ"},{"thu","てゅ"},{"the","てぇ"},{"tho","てょ"},
    {"twa","とぁ"},{"twi","とぃ"},{"twu","とぅ"},{"twe","とぇ"},{"two","とぉ"},
    {"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
    {"dya","ぢゃ"},{"dyi","ぢぃ"},{"dyu","ぢゅ"},{"dye","ぢぇ"},{"dyo","ぢょ"},
    {"dha","でゃ"},{"dhi","でぃ"},{"dhu","でゅ"},{"dhe","でぇ"},{"dho","でょ"},
    {"dwa","どぁ"},{"dwi","どぃ"},{"dwu","どぅ"},{"dwe","どぇ"},{"dwo","どぉ"},

    /* n — "n" itself is here so a dead end can resolve it to hatsuon */
    {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
    {"nya","にゃ"},{"nyi","にぃ"},{"nyu","にゅ"},{"nye","にぇ"},{"nyo","にょ"},
    {"n","ん"},{"nn","ん"},{"n'","ん"},{"xn","ん"},
    /* "nn" has to stay extendable, or "konnichiha" comes out こんいちは:
       everyone types the hatsuon before a na-row kana as a third n.
       These entries carry the hatsuon themselves, so "nn" alone still
       resolves to plain ん through roma_feed's dead-end path
       ("gennki" -> げんき) and "nnn" flushes it and starts over. */
    {"nna","んな"},{"nni","んに"},{"nnu","んぬ"},{"nne","んね"},{"nno","んの"},
    {"nnya","んにゃ"},{"nnyi","んにぃ"},{"nnyu","んにゅ"},
    {"nnye","んにぇ"},{"nnyo","んにょ"},

    /* h / b / p / f */
    {"ha","は"},{"hi","ひ"},{"hu","ふ"},{"he","へ"},{"ho","ほ"},
    {"hya","ひゃ"},{"hyi","ひぃ"},{"hyu","ひゅ"},{"hye","ひぇ"},{"hyo","ひょ"},
    {"fa","ふぁ"},{"fi","ふぃ"},{"fu","ふ"},{"fe","ふぇ"},{"fo","ふぉ"},
    {"fya","ふゃ"},{"fyu","ふゅ"},{"fyo","ふょ"},
    {"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
    {"bya","びゃ"},{"byi","びぃ"},{"byu","びゅ"},{"bye","びぇ"},{"byo","びょ"},
    {"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
    {"pya","ぴゃ"},{"pyi","ぴぃ"},{"pyu","ぴゅ"},{"pye","ぴぇ"},{"pyo","ぴょ"},

    /* m / y / r */
    {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
    {"mya","みゃ"},{"myi","みぃ"},{"myu","みゅ"},{"mye","みぇ"},{"myo","みょ"},
    {"ya","や"},{"yi","い"},{"yu","ゆ"},{"ye","いぇ"},{"yo","よ"},
    {"xya","ゃ"},{"xyu","ゅ"},{"xyo","ょ"},
    {"lya","ゃ"},{"lyu","ゅ"},{"lyo","ょ"},
    {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
    {"rya","りゃ"},{"ryi","りぃ"},{"ryu","りゅ"},{"rye","りぇ"},{"ryo","りょ"},

    /* w / v */
    {"wa","わ"},{"wi","うぃ"},{"wu","う"},{"we","うぇ"},{"wo","を"},
    {"wha","うぁ"},{"whi","うぃ"},{"whu","う"},{"whe","うぇ"},{"who","うぉ"},
    {"xwa","ゎ"},{"lwa","ゎ"},
    {"va","ゔぁ"},{"vi","ゔぃ"},{"vu","ゔ"},{"ve","ゔぇ"},{"vo","ゔぉ"},
    {"vya","ゔゃ"},{"vyu","ゔゅ"},{"vyo","ゔょ"},

    /* explicit small tsu */
    {"xtu","っ"},{"ltu","っ"},{"xtsu","っ"},{"ltsu","っ"},

    /* punctuation that has a kana form. "-" matters more than it looks:
       readings like "ぺーじ" are exactly the ones §6.1's EUC-JP/UTF-8
       reordering trips over, so they must be typable. */
    {"-","ー"},{".","。"},{",","、"},{"[","「"},{"]","」"},
};

#define ROMA_N ((int)(sizeof ROMA / sizeof ROMA[0]))

#define KANA_SOKUON "っ"

/* Exact match for `r[0..n)`, and whether any entry strictly extends it.
   One pass, because both answers are needed on every keystroke. */
static const roma_ent_t *roma_find(const char *r, size_t n, bool *out_ext)
{
    const roma_ent_t *hit = NULL;
    bool ext = false;

    if (n == 0 || n > 4) { /* no entry is longer than 4 bytes */
        if (out_ext) *out_ext = false;
        return NULL;
    }
    for (int i = 0; i < ROMA_N; i++) {
        if (memcmp(ROMA[i].r, r, n) != 0) continue;
        if (ROMA[i].r[n] == '\0') hit = &ROMA[i];
        else ext = true;
    }
    if (out_ext) *out_ext = ext;
    return hit;
}

static bool is_vowel(char c)
{
    return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

/* Does `pend` + `c` mean a sokuon? A doubled consonant does ("kk",
   "tt", "pp"), and so does the one asymmetric pair Hepburn produces,
   "t" + "c" as in "matcha". "nn" never does — it is hatsuon and lives
   in the table. Only a single pending consonant qualifies, so a
   mistyped "kyk" is discarded rather than turned into a small tsu. */
static bool is_sokuon(const char *pend, size_t plen, char c)
{
    if (plen != 1) return false;
    if (c < 'a' || c > 'z') return false;
    if (is_vowel(c) || c == 'n') return false;
    return c == pend[0] || (pend[0] == 't' && c == 'c');
}

/* ------------------------------------------------------------------ */
/* Fixed-buffer appends                                                */

/* Each returns false and changes NOTHING when the bytes will not fit.
   A reading that would overflow is refused outright: silently
   truncating it would build a different dictionary key and answer the
   wrong word, which is exactly the class of quiet-wrong-answer bug the
   packed key exists to prevent. */

static bool add_reading(skk_t *s, const char *p, size_t n)
{
    if (n > (size_t)(SKK_READING_MAX - s->reading_len)) return false;
    memcpy(s->reading + s->reading_len, p, n);
    s->reading_len = (uint16_t)(s->reading_len + n);
    return true;
}

static bool add_okuri(skk_t *s, const char *p, size_t n)
{
    if (n > (size_t)(SKK_OKURI_MAX - s->okuri_len)) return false;
    memcpy(s->okuri + s->okuri_len, p, n);
    s->okuri_len = (uint8_t)(s->okuri_len + n);
    return true;
}

/* One byte is held back so skk_commit() can hand out a NUL-terminated
   string without writing to a const skk_t. */
/* Copy what fits, cut on a UTF-8 boundary. False only when nothing at
   all could be written.
 *
 * Refusing outright looked safer and was worse: span_commit() saw no
 * growth, so SKK_ST_COMMIT never got set while the span closed anyway —
 * Enter on an over-long candidate silently inserted NOTHING and the key
 * looked dead, with no way for the user to tell why. SKK_COMMIT_MAX is
 * 256 B (~85 kanji) and no dictionary entry comes near it, so this is
 * the robustness path; there, partial text beats a key that does
 * nothing. */
static bool add_commit(skk_t *s, const char *p, size_t n)
{
    size_t room;

    if (s->commit_len + 1u >= (unsigned)SKK_COMMIT_MAX) return false;
    room = (size_t)(SKK_COMMIT_MAX - 1) - s->commit_len;
    if (n > room) {
        n = room;
        /* back off any trailing continuation bytes so we never emit half
           a character */
        while (n && ((unsigned char)p[n] & 0xC0u) == 0x80u) n--;
        if (n == 0) return false;
    }
    memcpy(s->commit + s->commit_len, p, n);
    s->commit_len = (uint16_t)(s->commit_len + n);
    s->commit[s->commit_len] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Emission — where produced kana goes                                 */

/* MIDASHI -> the reading, OKURI -> the okurigana, anything else -> the
 * commit string.
 *
 * v1 simplification, deliberate: only direct kana input in KATA base
 * mode is katakana-ised. The reading and the okurigana stay hiragana
 * whatever the base mode, because the dictionary is keyed on hiragana;
 * katakana output from a ~ span is what 'q' is for. */
static void emit(skk_t *s, const char *p, size_t n, uint32_t *st)
{
    if (n == 0) return;

    if (s->mode == SKK_MODE_MIDASHI) {
        if (add_reading(s, p, n)) *st |= SKK_ST_PREEDIT;
        return;
    }
    if (s->mode == SKK_MODE_OKURI) {
        if (add_okuri(s, p, n)) *st |= SKK_ST_PREEDIT;
        return;
    }
    if (s->base_mode == SKK_MODE_KATA) {
        char kb[32]; /* one table entry is at most 2 kana */
        size_t kn = to_katakana(p, n, kb, sizeof kb);
        if (kn && add_commit(s, kb, kn)) *st |= SKK_ST_COMMIT;
        return;
    }
    if (add_commit(s, p, n)) *st |= SKK_ST_COMMIT;
}

/* ------------------------------------------------------------------ */
/* The romaji converter                                                */

/* Feed one lowercase-ASCII byte. The loop runs at most three times:
   a dead end resolves the pending romaji (as kana, as a sokuon, or by
   discarding it) and retries `c` against an empty buffer, which cannot
   dead-end twice. */
static void roma_feed(skk_t *s, char c, uint32_t *st)
{
    for (int pass = 0; pass < 3; pass++) {
        char cand[SKK_ROMA_MAX + 1];
        size_t n = s->roma_len;

        if (n >= SKK_ROMA_MAX) n = 0; /* unreachable with this table */
        memcpy(cand, s->roma, n);
        cand[n++] = c;

        bool ext = false;
        const roma_ent_t *hit = roma_find(cand, n, &ext);

        if (hit && !ext) { /* complete and unextendable: it is kana now */
            s->roma_len = 0;
            emit(s, hit->k, strlen(hit->k), st);
            *st |= SKK_ST_PREEDIT;
            return;
        }
        if (hit || ext) { /* "n", "ky", "ch": hold and wait for more */
            memcpy(s->roma, cand, n);
            s->roma_len = (uint8_t)n;
            *st |= SKK_ST_PREEDIT;
            return;
        }

        /* Dead end. Resolve whatever was pending, then retry `c`. */
        if (s->roma_len) {
            const roma_ent_t *pend = roma_find(s->roma, s->roma_len, NULL);
            char last = s->roma[s->roma_len - 1];
            size_t plen = s->roma_len;
            s->roma_len = 0;

            if (pend) { /* "n" + "k" -> hatsuon, then start over on 'k' */
                emit(s, pend->k, strlen(pend->k), st);
                continue;
            }
            if (is_sokuon(&last, plen, c)) { /* "k" + "k", "t" + "c" */
                emit(s, KANA_SOKUON, sizeof KANA_SOKUON - 1, st);
                continue;
            }
            continue; /* mistyped prefix: drop it silently */
        }

        /* Nothing pending and `c` maps to nothing: it stays itself.
           Digits and symbols reach the buffer this way. */
        emit(s, &c, 1, st);
        *st |= SKK_ST_PREEDIT;
        return;
    }
}

/* Resolve a pending romaji tail before a mode change. An exact match
   becomes kana ("n" -> hatsuon); an incomplete one is discarded. */
static void roma_flush(skk_t *s, uint32_t *st)
{
    if (!s->roma_len) return;

    const roma_ent_t *pend = roma_find(s->roma, s->roma_len, NULL);
    s->roma_len = 0;
    *st |= SKK_ST_PREEDIT;
    if (pend) emit(s, pend->k, strlen(pend->k), st);
}

/* ------------------------------------------------------------------ */
/* Preedit rendering                                                   */

#define MARK_MIDASHI "\xe2\x96\xbd" /* U+25BD white down-pointing triangle */
#define MARK_SELECT  "\xe2\x96\xbc" /* U+25BC black down-pointing triangle */

static void pre_add(skk_t *s, const char *p, size_t n)
{
    /* Leave room for the NUL. The preedit is display only, so clipping
       a pathological line here loses nothing the engine needs. */
    if (n > (size_t)(SKK_PREEDIT_MAX - 1 - s->preedit_len)) return;
    memcpy(s->preedit + s->preedit_len, p, n);
    s->preedit_len = (uint16_t)(s->preedit_len + n);
}

/* pre_add, plus a note of what was just appended. Records what ACTUALLY
   landed rather than what was asked for: pre_add drops an append whole
   when it would not fit (a dictionary candidate is bounded by
   SKK_COMMIT_MAX, not by SKK_PREEDIT_MAX, so this really happens in v),
   and a span claiming bytes that never arrived would send the renderer
   past the end of the string. A dropped or empty append records
   nothing, so a renderer never has to skip zero-length spans. */
static void pre_span(skk_t *s, skk_span_kind_t kind, const char *p, size_t n)
{
    uint16_t off = s->preedit_len;
    uint16_t got;

    pre_add(s, p, n);
    got = (uint16_t)(s->preedit_len - off);
    if (got == 0 || s->span_n >= SKK_SPAN_MAX)
        return;
    s->span[s->span_n].off  = off;
    s->span[s->span_n].len  = got;
    s->span[s->span_n].kind = (uint16_t)kind;
    s->span_n++;
}

static void build_preedit(skk_t *s)
{
    s->preedit_len = 0;
    s->span_n = 0;

    switch (s->mode) {
    case SKK_MODE_MIDASHI:
        pre_span(s, SKK_SPAN_MARK, MARK_MIDASHI, sizeof MARK_MIDASHI - 1);
        pre_span(s, SKK_SPAN_READING, s->reading, s->reading_len);
        break;

    case SKK_MODE_OKURI:
        pre_span(s, SKK_SPAN_MARK, MARK_MIDASHI, sizeof MARK_MIDASHI - 1);
        pre_span(s, SKK_SPAN_READING, s->reading, s->reading_len);
        pre_span(s, SKK_SPAN_SEP, "*", 1);
        pre_span(s, SKK_SPAN_OKURI, s->okuri, s->okuri_len);
        break;

    case SKK_MODE_SELECT: {
        size_t n = 0;
        const char *c = skk_cand(s, s->sel, &n);
        pre_span(s, SKK_SPAN_MARK, MARK_SELECT, sizeof MARK_SELECT - 1);
        /* the fallback is the reading, so it is READING and not CAND —
           the renderer must not paint it as a chosen candidate */
        if (c) pre_span(s, SKK_SPAN_CAND, c, n);
        else   pre_span(s, SKK_SPAN_READING, s->reading, s->reading_len);
        pre_span(s, SKK_SPAN_OKURI, s->okuri, s->okuri_len);
        break;
    }

    default: /* ASCII / KANA / KATA: only the untranslated romaji shows */
        break;
    }
    /* The romaji tail always trails the span it is being typed into;
       SELECT has no tail because conversion needs an empty buffer. */
    pre_span(s, SKK_SPAN_ROMA, s->roma, s->roma_len);
    s->preedit[s->preedit_len] = '\0';
}

int skk_preedit_spans(const skk_t *s, const skk_span_t **out)
{
    if (out)
        *out = s->span;
    return s->span_n;
}

/* ------------------------------------------------------------------ */
/* State transitions                                                   */

static void clear_span(skk_t *s)
{
    s->reading_len = 0;
    s->okuri_len   = 0;
    s->okuri_stem  = 0;
    s->roma_len    = 0;
    s->cand_n      = 0;
    s->sel         = -1;
    s->scratch_len = 0;
}

/* Leave the ~/v span without producing anything. */
static void span_cancel(skk_t *s, uint32_t *st)
{
    clear_span(s);
    s->mode = s->base_mode;
    *st |= SKK_ST_PREEDIT | SKK_ST_MODE;
}

/* Leave the span after something has been added to the commit. */
static void span_done(skk_t *s, uint32_t *st)
{
    clear_span(s);
    s->mode = s->base_mode;
    *st |= SKK_ST_PREEDIT | SKK_ST_MODE;
}

/* Close the span and announce the commit — but only when bytes were
   actually produced. Closing an empty ~ with Enter must NOT set
   SKK_ST_COMMIT: the app would be told to insert "" (ssh_vt would send
   an empty bracketed paste, an editor would run an empty insert), and
   an empty commit is not a commit for skk_stats_t either. */
static void span_commit(skk_t *s, uint32_t *st, size_t before)
{
    if (s->commit_len > before) {
        *st |= SKK_ST_COMMIT;
        s->stats.commits++;
    }
    span_done(s, st);
}

/* Commit the reading exactly as typed (Enter in ~). */
static void commit_reading(skk_t *s, uint32_t *st)
{
    size_t before = s->commit_len;

    if (add_commit(s, s->reading, s->reading_len))
        (void)add_commit(s, s->okuri, s->okuri_len);
    span_commit(s, st, before);
}

/* Commit the reading as katakana ('q' in ~). */
static void commit_katakana(skk_t *s, uint32_t *st)
{
    size_t before = s->commit_len;
    size_t n = to_katakana(s->reading, s->reading_len,
                           s->scratch, SKK_SCRATCH_MAX);

    if (n && add_commit(s, s->scratch, n))
        (void)add_commit(s, s->okuri, s->okuri_len);
    span_commit(s, st, before);
}

/* The reading as the dictionary indexes it: the kana span, plus the
   ASCII stem for an okuri-ari entry ("うご" + 'k'). `buf` must hold
   SKK_READING_MAX + 1. */
static size_t span_key(const skk_t *s, char *buf, skk_blk_t *blk)
{
    size_t klen = s->reading_len;

    memcpy(buf, s->reading, klen);
    *blk = SKK_BLK_NASI;
    if (s->okuri_stem) {
        buf[klen++] = (char)s->okuri_stem;
        *blk = SKK_BLK_ARI;
    }
    return klen;
}

/* Commit the selected candidate plus the okurigana (Enter in v, or any
   key that is not a selection command). */
static void commit_selection(skk_t *s, uint32_t *st)
{
    size_t before = s->commit_len;
    size_t n = 0;
    const char *c = skk_cand(s, s->sel, &n);

    if (c && add_commit(s, c, n))
        (void)add_commit(s, s->okuri, s->okuri_len);
    span_commit(s, st, before);
}

/* Look the span up and enter v. On a miss (or with no dictionary) the
 * span is left exactly as it was, so the user can keep typing, cancel
 * with C-g, or take the kana with Enter. Dictionary registration is out
 * of v1 scope.
 *
 * SKK_ST_CANDS is set only when a non-empty set arrives, so the header's
 * "CANDS implies sel == 0" holds; the app clears its candidate bar when
 * skk_mode() stops being SKK_MODE_SELECT (signalled by SKK_ST_MODE). */
static void convert(skk_t *s, uint32_t *st)
{
    char key[SKK_READING_MAX + 1];
    skk_blk_t blk;
    size_t klen;
    size_t n = 0;

    if (!s->dict || s->reading_len == 0) return;
    klen = span_key(s, key, &blk);

    /* The _stats variant, not skk_lookup(): it fills probes / fullcmp /
       cands / dropped as it searches, so ui.imeStats() can answer "what did
       that conversion cost" without running the search a second time.
       Calling the plain one here left those three counters at zero
       forever, which is the instrument design §8.1 says to read before
       reconsidering predictive conversion. It also does lookups++ and
       cands += itself, so this function must not count them again. */
    if (skk_lookup_stats(s->dict, blk, key, klen, s->cand, SKK_CAND_MAX,
                         &n, &s->stats) < 0)
        n = 0;
    if (n == 0) return;

    s->cand_n = (uint16_t)n;
    s->sel    = 0;
    s->mode   = SKK_MODE_SELECT;
    *st |= SKK_ST_CANDS | SKK_ST_PREEDIT | SKK_ST_MODE;
}

/* An okurigana converts by itself the moment its first kana is
   complete — which is when the romaji buffer is empty again, so
   "MotTe" gets to build the sokuon before firing. */
static void okuri_maybe_convert(skk_t *s, uint32_t *st)
{
    if (s->mode == SKK_MODE_OKURI && s->okuri_len && !s->roma_len)
        convert(s, st);
}

/* TAB: replace the reading with the next dictionary entry that starts
 * with it. Repeated TAB cycles forward and wraps; any other key ends the
 * cycle and keeps whatever is showing (skk_key() clears comp_len).
 *
 * The prefix stays the ORIGINAL reading (comp_len bytes), and every
 * completion begins with it, so s->reading[0..comp_len) is still the
 * prefix after a substitution — no separate copy is needed.
 *
 * Cheap enough to be worth having (§6.8): one binary search plus a
 * forward walk of a median 5 entries / 78 bytes in L. Only in MIDASHI —
 * once the okurigana has started, the reading is no longer a prefix of
 * anything in the okuri-nasi block. */
static void span_complete(skk_t *s, uint32_t *st)
{
    const char *r = NULL;
    size_t rlen = 0;

    *st |= SKK_ST_CONSUMED;
    if (!s->dict || s->mode != SKK_MODE_MIDASHI) return;

    roma_flush(s, st); /* "かん" plus a pending "j" is not a prefix */

    if (!s->comp_len) {
        if (!s->reading_len) return;
        s->comp_len = s->reading_len;
        s->comp_i   = 0;
    } else {
        s->comp_i++;
    }

    if (skk_complete(s->dict, SKK_BLK_NASI, s->reading, s->comp_len,
                     s->comp_i, &r, &rlen, &s->stats.probes) != 1) {
        if (s->comp_i == 0) {
            s->comp_len = 0;  /* nothing completes it: not in a cycle after all */
            return;
        }
        s->comp_i = 0;        /* ran off the end: wrap to the first */
        if (skk_complete(s->dict, SKK_BLK_NASI, s->reading, s->comp_len,
                         0, &r, &rlen, &s->stats.probes) != 1) {
            return;
        }
    }
    if (rlen == 0 || rlen > SKK_READING_MAX) return;

    memcpy(s->reading, r, rlen);   /* r points into the image, never into s */
    s->reading_len = (uint16_t)rlen;
    *st |= SKK_ST_PREEDIT;
}

/* Delete one character from the ~ span. */
static void span_backspace(skk_t *s, uint32_t *st)
{
    *st |= SKK_ST_PREEDIT;

    if (s->roma_len) {
        s->roma_len--;
        return;
    }
    if (s->mode == SKK_MODE_OKURI) {
        if (s->okuri_len) {
            s->okuri_len = (uint8_t)skk_utf8_prev(s->okuri, s->okuri_len,
                                                  s->okuri_len);
            if (s->okuri_len) return;
        }
        s->okuri_stem = 0; /* the okurigana is gone: back to plain ~ */
        s->mode = SKK_MODE_MIDASHI;
        *st |= SKK_ST_MODE;
        return;
    }
    if (s->reading_len) {
        s->reading_len = (uint16_t)skk_utf8_prev(s->reading, s->reading_len,
                                                 s->reading_len);
        if (s->reading_len) return;
    }
    span_cancel(s, st); /* the ~ emptied out */
}

/* An uppercase letter inside ~ starts the okurigana. The stem letter
 * that goes into the dictionary key is the FIRST consonant of the
 * okurigana, which is the pending romaji when there is one — "MotTe"
 * keys on "もt" and produces って, not もっ + て.
 *
 * But only when that pending romaji is an INCOMPLETE fragment. A
 * pending "n"/"nn" already spells a kana on its own, and that ん
 * belongs to the READING, not to the okurigana: "KanJiru" must key on
 * "かんj", not on "かn". The distinction is exactly roma_find() — it
 * returns an entry for "n" and NULL for "t".
 *
 * Getting this wrong is not a lookup failure, it is a silent wrong
 * answer: "かn" exists in SKK-JISYO.L with 8 candidates, so the search
 * succeeds on the wrong entry and commits 兼んじる for 感じる. Every
 * ん-final stem is affected, which is one of the most common patterns
 * in Japanese input. Flush BEFORE switching the mode, so emit() still
 * routes the kana into the reading. */
static void okuri_start(skk_t *s, char lower, uint32_t *st)
{
    if (s->roma_len && roma_find(s->roma, s->roma_len, NULL))
        roma_flush(s, st); /* still MIDASHI: the kana joins the reading */

    s->okuri_stem = (uint8_t)(s->roma_len ? s->roma[0] : lower);
    s->okuri_len  = 0;
    s->mode       = SKK_MODE_OKURI;
    *st |= SKK_ST_PREEDIT | SKK_ST_MODE;

    roma_feed(s, lower, st);
    okuri_maybe_convert(s, st);
}

/* ------------------------------------------------------------------ */
/* Key dispatch                                                        */

#define K_BEL   0x07 /* C-g: cancel */
#define K_BS    0x08
#define K_TAB   0x09
#define K_LF    0x0A /* Enter (kbd_core maps KBD_K_ENTER to "\n") */
#define K_ESC   0x1B
#define K_SPACE 0x20
#define K_DEL   0x7F

/* "\0name" tokens skk_key understands. Anything else falls through.
 *
 * TOK_SWALLOW is for keys that move the APP's cursor. They must not
 * reach the app while a preedit is open, for the same reason the arrows
 * are swallowed below — the preedit is drawn where the cursor is, and
 * letting the app jump to another line strands it. They were reaching
 * the app because token_of() lumped them in with TOK_OTHER, which
 * returns before the mode switch ever runs.
 *
 * Deliberately NOT swallowed wholesale: "rotate" (the app must relayout),
 * "ctrl"/"alt" (modifier latches the app owns), "ime", "copy"/"paste",
 * the F-keys. Consuming every unknown token would break all of those,
 * so the list here is explicit. */
typedef enum {
    TOK_NONE = 0, TOK_PREV, TOK_NEXT, TOK_ESC, TOK_DEL, TOK_SWALLOW, TOK_OTHER
} tok_t;

static tok_t token_of(const char *p, size_t n)
{
    if (n == 4 && memcmp(p, "left", 4) == 0) return TOK_PREV;
    if (n == 2 && memcmp(p, "up",   2) == 0) return TOK_PREV;
    if (n == 5 && memcmp(p, "right",5) == 0) return TOK_NEXT;
    if (n == 4 && memcmp(p, "down", 4) == 0) return TOK_NEXT;
    if (n == 3 && memcmp(p, "esc",  3) == 0) return TOK_ESC;
    if (n == 3 && memcmp(p, "del",  3) == 0) return TOK_DEL;
    if (n == 4 && memcmp(p, "home", 4) == 0) return TOK_SWALLOW;
    if (n == 3 && memcmp(p, "end",  3) == 0) return TOK_SWALLOW;
    if (n == 4 && memcmp(p, "pgup", 4) == 0) return TOK_SWALLOW;
    if (n == 4 && memcmp(p, "pgdn", 4) == 0) return TOK_SWALLOW;
    return TOK_OTHER;
}

static void sel_move(skk_t *s, int delta, uint32_t *st)
{
    int n = s->sel + delta;

    if (n < 0) { /* stepped back off the front: return to ~ */
        s->cand_n = 0;
        s->sel    = -1;
        /* Back to the span exactly as it was. An okurigana is part of
           the span, so returning to plain MIDASHI would drop it from
           the preedit while it is still in the buffer and still in the
           dictionary key — the user would see ~おく and get 送り. */
        s->mode   = s->okuri_stem ? SKK_MODE_OKURI : SKK_MODE_MIDASHI;
        *st |= SKK_ST_PREEDIT | SKK_ST_MODE;
        return;
    }
    if (n >= (int)s->cand_n) {
        /* Past the last candidate there is nowhere to go (dictionary
           registration is out of v1 scope). Clamp — but the key is
           still ours: passing it on would insert a space into the
           middle of a conversion. */
        *st |= SKK_ST_CONSUMED;
        return;
    }
    s->sel = (int16_t)n;
    *st |= SKK_ST_SEL | SKK_ST_PREEDIT;
}

/* One pass of the state machine. Returns true when the caller must feed
   the same key again — the v-mode "any other key commits, then acts"
   rule, which is how one skk_key() sets both COMMIT and PREEDIT. */
static bool dispatch(skk_t *s, const char *key, size_t len, uint32_t *st)
{
    unsigned char k0 = (unsigned char)key[0];
    char c = 0;        /* the ASCII byte, 0 when the key is not ASCII */
    size_t seq = 1;    /* bytes of the leading character */
    tok_t tok = TOK_NONE;

    if (k0 == '\0') {
        if (len < 2) return false;
        tok = token_of(key + 1, len - 1);
        if (tok == TOK_OTHER) return false; /* not ours */
    } else if (k0 == K_ESC && len > 1) {
        return false; /* ESC-prefixed: Alt-something, never consumed */
    } else if (k0 == K_ESC) {
        tok = TOK_ESC;
    } else if (k0 < 0x80) {
        c = (char)k0;
    } else if (skk_utf8_decode(key, len, 0, &seq) == 0xFFFD && seq == 1) {
        /* Not a whole, well-formed character (a genuine U+FFFD decodes
           with seq == 3 and is let through). Never let a partial
           sequence into a buffer: a lone continuation byte in the
           reading would make the preedit un-renderable and the
           dictionary key garbage. Swallow it while a preedit is open,
           pass it on otherwise. */
        if (s->mode == SKK_MODE_ASCII || s->mode == SKK_MODE_KANA ||
            s->mode == SKK_MODE_KATA)
            return false;
        *st |= SKK_ST_CONSUMED;
        return false;
    }

    switch ((skk_mode_t)s->mode) {

    /* ---- ASCII: the IME is armed but transparent ------------------ */
    case SKK_MODE_ASCII:
        return false;

    /* ---- kana / katakana ------------------------------------------ */
    case SKK_MODE_KANA:
    case SKK_MODE_KATA:
        if (tok != TOK_NONE) {
            /* Arrows and Delete belong to the app; only swallow them
               while a romaji tail is on screen, so it cannot go stale. */
            if (!s->roma_len) return false;
            s->roma_len = 0;
            *st |= SKK_ST_PREEDIT;
            return false;
        }
        if (c == K_BEL || c == K_BS) {
            if (!s->roma_len) return false;
            if (c == K_BS) s->roma_len--;
            else           s->roma_len = 0;
            *st |= SKK_ST_PREEDIT;
            return false;
        }
        if (c == K_LF || c == K_SPACE) {
            /* A pending tail must not survive the app's own handling of
               Enter/Space, and there is no way to both consume and pass
               through: resolve it and eat this one key. */
            if (!s->roma_len) return false;
            roma_flush(s, st);
            return false;
        }
        /* 'l' and 'q' FLUSH the pending romaji, they do not drop it.
           roma_flush() turns a complete entry into kana ("n" -> ん) and
           discards a genuine fragment; dropping unconditionally lost the
           ん in "nl" and "nq", while "n\n" and "nk" kept it — the same
           keystroke meaning two different things depending on what came
           next. ddskk commits the ん and then changes mode. Flush BEFORE
           the switch so the kana lands in the mode it was typed in. */
        if (c == 'l') { /* leave to ASCII (skk_set_mode() comes back) */
            roma_flush(s, st);
            s->mode = SKK_MODE_ASCII;
            *st |= SKK_ST_PREEDIT | SKK_ST_MODE;
            return false;
        }
        if (c == 'q') { /* toggle kana <-> katakana */
            roma_flush(s, st);
            s->base_mode = (s->base_mode == SKK_MODE_KATA)
                         ? SKK_MODE_KANA : SKK_MODE_KATA;
            s->mode = s->base_mode;
            *st |= SKK_ST_PREEDIT | SKK_ST_MODE;
            return false;
        }
        if (c >= 'A' && c <= 'Z') { /* shift opens the ~ span */
            s->roma_len = 0;        /* an unfinished tail is not a reading */
            clear_span(s);
            s->mode = SKK_MODE_MIDASHI;
            *st |= SKK_ST_PREEDIT | SKK_ST_MODE;
            /* 'Q' opens an EMPTY span, as in ddskk — it is the "convert
               what I am about to type" key, not the letter q. Feeding it
               through romaji would leave ~q sitting in the reading. */
            if (c != 'Q') {
                roma_feed(s, (char)(c - 'A' + 'a'), st);
            }
            return false;
        }
        if (c >= 0x21 && c <= 0x7E) {
            roma_feed(s, c, st);
            return false;
        }
        if (c == 0 && k0 >= 0x80 && s->roma_len) {
            /* Kana straight off a soft keyboard while a romaji tail is
               pending. Passing it through as-is left the tail sitting in
               the preedit while the character went around the IME, and
               the tail then reappeared attached to whatever was typed
               next. Resolve the tail and take the character too — both
               halves have to land in the same place or the order is
               wrong.

               With no tail pending, a bare kana still passes through
               untouched: that is the documented contract for this mode
               (t_passthrough covers it), and there is nothing to fix. */
            roma_flush(s, st);
            emit(s, key, seq, st);
            *st |= SKK_ST_PREEDIT;
            return false;
        }
        return false; /* controls and bare kana: the app's business */

    /* ---- ~ : a reading is being typed ----------------------------- */
    case SKK_MODE_MIDASHI:
    case SKK_MODE_OKURI:
        if (tok != TOK_NONE) {
            if (tok == TOK_ESC) span_cancel(s, st);
            /* Arrows and Delete are swallowed so they cannot move the
               app's cursor out from under an open preedit. */
            *st |= SKK_ST_CONSUMED;
            return false;
        }
        if (c == K_BEL) { span_cancel(s, st); return false; }
        if (c == K_BS || c == K_DEL) { span_backspace(s, st); return false; }
        if (c == K_LF) { roma_flush(s, st); commit_reading(s, st); return false; }
        if (c == K_TAB) { span_complete(s, st); return false; }
        if (c == K_SPACE) {
            roma_flush(s, st);
            if (!s->reading_len) span_cancel(s, st);
            else                 convert(s, st);
            *st |= SKK_ST_CONSUMED;
            return false;
        }
        if (c == 'q' && s->mode == SKK_MODE_MIDASHI) {
            roma_flush(s, st);
            commit_katakana(s, st);
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            char lower = (char)(c - 'A' + 'a');
            if (s->mode == SKK_MODE_MIDASHI) {
                if (!s->reading_len) {
                    /* The okurigana cannot start before there is a
                       reading to hang it on, and a pending romaji tail
                       is not a reading: "KA" is ~か and "KyO" is ~きょ,
                       exactly as the lowercase run would be. Starting
                       the okurigana here would key the lookup on an
                       empty reading and strand the kana after the *. */
                    roma_feed(s, lower, st);
                    return false;
                }
                okuri_start(s, lower, st);
            } else {
                roma_feed(s, lower, st); /* already in the okurigana */
                okuri_maybe_convert(s, st);
            }
            return false;
        }
        if (c >= 0x21 && c <= 0x7E) {
            roma_feed(s, c, st);
            okuri_maybe_convert(s, st);
            return false;
        }
        if (c == 0 && k0 >= 0x80) { /* kana straight off a soft keyboard */
            roma_flush(s, st);
            emit(s, key, seq, st);
            okuri_maybe_convert(s, st);
            *st |= SKK_ST_PREEDIT;
            return false;
        }
        *st |= SKK_ST_CONSUMED; /* other controls: swallow, do nothing */
        return false;

    /* ---- v : choosing a candidate --------------------------------- */
    case SKK_MODE_SELECT:
        if (tok == TOK_PREV) { sel_move(s, -1, st); return false; }
        if (tok == TOK_NEXT) { sel_move(s, +1, st); return false; }
        if (tok == TOK_ESC)  { sel_move(s, -1 - s->sel, st); return false; }
        if (tok == TOK_DEL || tok == TOK_SWALLOW) {
            *st |= SKK_ST_CONSUMED;
            return false;
        }
        if (c == K_SPACE) { sel_move(s, +1, st); return false; }
        if (c == 'x')     { sel_move(s, -1, st); return false; }
        if (c == K_BEL || c == K_BS) { /* cancel back to ~ */
            sel_move(s, -1 - s->sel, st);
            return false;
        }
        if (c == K_LF) { commit_selection(s, st); return false; }
        if (c == 0 && k0 >= 0x80) {
            /* Not an ASCII key, so there is nothing to re-feed: take the
               candidate and append the character behind it. */
            commit_selection(s, st);
            if (add_commit(s, key, seq)) *st |= SKK_ST_COMMIT;
            return false;
        }
        commit_selection(s, st);
        return true; /* ... and let the key act in the base mode */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */

void skk_init(skk_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->mode      = SKK_MODE_KANA;
    s->base_mode = SKK_MODE_KANA;
    s->sel       = -1;
}

void skk_attach(skk_t *s, const skk_dict_t *d)
{
    if (!s) return;
    s->dict = d;
}

void skk_reset(skk_t *s)
{
    if (!s) return;
    clear_span(s);
    s->commit_len = 0;
    s->commit[0]  = '\0';
    if (s->mode != SKK_MODE_ASCII) s->mode = s->base_mode;
    build_preedit(s);
}

void skk_enable(skk_t *s, bool on)
{
    if (!s) return;
    s->enabled = on ? 1 : 0;
    skk_reset(s);
}

bool skk_enabled(const skk_t *s)
{
    return s && s->enabled;
}

int skk_set_mode(skk_t *s, skk_mode_t m)
{
    if (!s) return SKK_ERR_ARG;
    if (m != SKK_MODE_ASCII && m != SKK_MODE_KANA && m != SKK_MODE_KATA)
        return SKK_ERR_ARG; /* ~/v need state only the key path builds */

    clear_span(s);
    s->commit_len = 0;
    s->commit[0]  = '\0';
    s->mode = (uint8_t)m;
    if (m != SKK_MODE_ASCII) s->base_mode = (uint8_t)m;
    build_preedit(s);
    return SKK_OK;
}

uint32_t skk_key(skk_t *s, const char *key, size_t len)
{
    uint32_t st = 0;

    if (!s) return SKK_ST_PASSTHROUGH;
    s->stats.keys++;
    s->commit_len = 0;    /* skk_commit() is valid only until the next key */
    s->commit[0]  = '\0';

    if (!s->enabled || !key || len == 0) return SKK_ST_PASSTHROUGH;

    /* Any key other than TAB ends a completion cycle, keeping whatever
       is showing. Cleared here rather than in each branch so no future
       key handler can forget and leave a stale prefix that a later TAB
       would resume from. */
    if (!(len == 1 && key[0] == K_TAB)) {
        s->comp_len = 0;
        s->comp_i   = 0;
    }

    if (dispatch(s, key, len, &st))
        (void)dispatch(s, key, len, &st); /* v mode committed; act again */

    if (st) {
        st |= SKK_ST_CONSUMED;
        s->stats.consumed++;
        build_preedit(s);
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Accessors — pointers into `s` (or the dictionary image), no copies  */

skk_mode_t skk_mode(const skk_t *s)
{
    return s ? (skk_mode_t)s->mode : SKK_MODE_ASCII;
}

const char *skk_preedit(const skk_t *s, size_t *len)
{
    if (!s) {
        if (len) *len = 0;
        return "";
    }
    if (len) *len = s->preedit_len;
    return s->preedit;
}

const char *skk_commit(const skk_t *s, size_t *len)
{
    if (!s) {
        if (len) *len = 0;
        return "";
    }
    if (len) *len = s->commit_len;
    return s->commit; /* add_commit() keeps it NUL-terminated */
}

int skk_cand_count(const skk_t *s)
{
    return s ? (int)s->cand_n : 0;
}

int skk_sel(const skk_t *s)
{
    return s ? s->sel : -1;
}

const char *skk_cand(const skk_t *s, int i, size_t *len)
{
    if (len) *len = 0;
    if (!s || i < 0 || i >= (int)s->cand_n) return NULL;

    const skk_cand_t *c = &s->cand[i];

    if (c->src == SKK_SRC_SCRATCH) {
        if ((size_t)c->off + c->len > s->scratch_len) return NULL;
        if (len) *len = c->len;
        return s->scratch + c->off;
    }
    if (!s->dict) return NULL;
    return skk_cand_text(s->dict, c, len);
}

void skk_stats(const skk_t *s, skk_stats_t *out)
{
    if (!out) return;
    if (!s) memset(out, 0, sizeof *out);
    else    *out = s->stats;
}

void skk_stats_reset(skk_t *s)
{
    if (s) memset(&s->stats, 0, sizeof s->stats);
}
