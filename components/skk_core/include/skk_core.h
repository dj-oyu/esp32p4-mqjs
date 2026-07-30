/*
 * skk_core — the SKK conversion engine every app can type Japanese with.
 *
 * SKK needs no morphological analysis: the user marks the conversion
 * span and the okurigana with shift, so the engine's whole job is
 * "romaji -> kana" plus "reading -> candidate list". That is small
 * enough to run natively and be tested on a host, which is why it lives
 * here rather than in JavaScript:
 *
 *   - the romaji->kana table with sokuon / hatsuon / youon handling,
 *   - the SKK state machine (kana / katakana / MIDASHI (~) / OKURI /
 *     SELECT (v)), including where the okurigana starts,
 *   - the dictionary: packed-key index, binary search, zero-copy
 *     candidates.
 *
 * There is no personal dictionary. One existed (an MRU that reordered
 * candidates by recency) and was removed on 2026-07-30 — see
 * docs/skk-ime-design.md §S7 for why, and for the shape a user
 * dictionary would take if one comes back. It would not be this one.
 *
 * WHO CALLS THIS. Not an app — ime_core does, on the one task that owns
 * the device's IME session, and mqjs hands the result to whichever sink
 * has focus (see docs/keyboard-ime-unification.md). Nothing above this
 * file has to know the status bitmask, the accessor lifetimes, or the
 * order keys must be offered in.
 *
 * It was not always so. Until 2026-07-30 the `skk.*` JS bindings let an
 * app drive this engine directly, and three of them did — each with its
 * own copy of when to arm, what the 「あ」 button means and when the
 * candidate bar folds. The bindings are gone; that history is kept here
 * only because the shape of this API still reflects it. In particular
 * skk_key() returns an integer and allocates nothing, which mattered
 * when a JS binding sat on the per-keystroke path and matters less now,
 * but is still the right shape for a hot path.
 *
 * The one thing that genuinely stays above: WHERE a committed string
 * goes. ssh_vt writes it to an ssh session, a widget field inserts it
 * into a textarea. Committed text now travels back out through the key
 * path as an ordinary key event (it no longer has to fit `char text[8]`
 * — that limit is why an app used to have to pull the string itself).
 *
 * Pure logic: no I/O, no LVGL, no allocation, no clock, no ESP-IDF
 * headers. Dictionary bytes arrive as a skk_blob_t (embedded rodata, an
 * mmap'd partition, or malloc+fread on a host) and skk_core never learns
 * which. Every buffer is caller-owned and fixed length: the caller
 * places a skk_t wherever it likes (PSRAM is fine, internal SRAM is not
 * required — largest contiguous there is 34-43 KB and lwIP/SDIO want
 * it). A skk_t is owned by ONE task, so nothing here locks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKK_CORE_API_VERSION 1

/* ------------------------------------------------------------------ */
/* Data access — the one abstraction that makes plum/bamboo/pine the
 * same code. Search only ever sees a base pointer and a length:
 *   plum   : flash rodata embedded with EMBED_FILES (_binary_..._start)
 *   bamboo : esp_partition_mmap() of a `jisyo` partition
 *   host   : malloc + fread of the prep tool's output
 * All three are zero-allocation and zero-load-time; this chip reads
 * flash and PSRAM through the MMU, so nothing is ever copied. */
typedef struct {
    const uint8_t *base;
    size_t         len;
} skk_blob_t;

/* ------------------------------------------------------------------ */
/* Status bitmask — the return of skk_key().
 *
 * skk_key() returns an integer and nothing else, so the JS binding
 * allocates nothing on the per-keystroke path: no object, no strings,
 * and therefore none of the moving-GC nesting hazard that building a
 * result object would invite. The app pulls only what changed, and the
 * candidate array is materialised exactly once per entry into v mode.
 *
 * 0 (SKK_ST_PASSTHROUGH) means the IME did not take the key and the app
 * must run its normal handling. Any non-zero return always has
 * SKK_ST_CONSUMED set, so `if (st !== 0)` is the correct JS test. */
#define SKK_ST_PASSTHROUGH  0u
#define SKK_ST_CONSUMED     (1u << 0) /* key was eaten; bits below say what moved */
#define SKK_ST_PREEDIT      (1u << 1) /* preedit changed  -> skk_preedit() */
#define SKK_ST_CANDS        (1u << 2) /* candidate set changed -> skk_cand_*() */
#define SKK_ST_SEL          (1u << 3) /* only the selection moved -> skk_sel() */
#define SKK_ST_COMMIT       (1u << 4) /* a commit string is ready -> skk_commit() */
#define SKK_ST_MODE         (1u << 5) /* mode indicator changed -> skk_mode() */

/* SKK_ST_CANDS implies the selection is 0; a handler that redraws the
   whole candidate bar on CANDS does not also need to look at SEL. */

/* ------------------------------------------------------------------ */
/* Modes. ASCII is reachable from KANA with 'l' and is NOT the same as
 * "IME off" (skk_enable): an app may want the IME armed while the user
 * types a shell command.
 *
 * TRAP: real SKK leaves ASCII mode with C-j, but this firmware's
 * keyboard already spends 0x0A on Enter (kbd_core maps KBD_K_ENTER to
 * "\n"), so C-j is indistinguishable from Return and skk_key() will
 * NOT honour it. Getting back to KANA is the app's job via
 * skk_set_mode() — wire it to the same surface as the IME toggle. */
typedef enum {
    SKK_MODE_ASCII   = 0, /* pass ASCII straight through */
    SKK_MODE_KANA    = 1, /* hiragana */
    SKK_MODE_KATA    = 2, /* katakana */
    SKK_MODE_MIDASHI = 3, /* ~ : a reading is being typed */
    SKK_MODE_OKURI   = 4, /* ~ : ... and the okurigana has started */
    SKK_MODE_SELECT  = 5, /* v : choosing among candidates */
} skk_mode_t;

/* ------------------------------------------------------------------ */
/* Errors. Negative, so `if (rc < 0)`. */
typedef enum {
    SKK_OK            =  0,
    SKK_ERR_ARG       = -1, /* NULL / nonsense argument */
    SKK_ERR_MAGIC     = -2, /* not a skk image */
    SKK_ERR_VERSION   = -3, /* image built by a different format version */
    SKK_ERR_ALIGN     = -4, /* image base not 8-byte aligned (see below) */
    SKK_ERR_TRUNCATED = -5, /* a section runs past the end of the blob */
    SKK_ERR_CRC       = -6, /* payload CRC mismatch (SKK_OPEN_VERIFY) */
    SKK_ERR_ALPHABET  = -7, /* image built with a different collation table */
    SKK_ERR_NOSPACE   = -8, /* caller's output array was too small */
    SKK_ERR_FORMAT    = -9, /* malformed dictionary text */
    SKK_ERR_NODICT    = -10,/* skk_t has no dictionary attached */
} skk_err_t;

/* ------------------------------------------------------------------ */
/* Packed key — a reading folded into one uint64.
 *
 * Readings contain only hiragana and ASCII (the okurigana stem), never
 * kanji. Measured 2026-07-29 over the shipped dictionaries: S 95, M
 * 127, ML 129, L 182 distinct characters — and each set is a strict
 * subset of L's, so their union is 182. THE TABLE IS THAT UNION, in
 * every stage. Plum ships M but is built with the pine alphabet, so
 * moving to ML or L never changes skk_alphabet_crc32() and never
 * invalidates an image. 182 <= 254 leaves room for the two reserved
 * codes; eight characters fit in a uint64.
 *
 * The code assigned to a character is its RANK IN UTF-8 BYTE ORDER,
 * which is what makes a single uint64 compare agree with a memcmp of
 * the UTF-8 readings. Two codes are reserved:
 *
 *   0x00  terminator / padding. No character maps to it, so a reading
 *         that is a prefix of another packs strictly lower — exactly
 *         what strcmp does.
 *   0xFF  escape, for any character absent from the table. It sorts
 *         above every real code.
 *
 * Packing is little-endian-agnostic: the FIRST character occupies the
 * MOST significant byte, so the natural uint64 `<` is the string order.
 *
 *     pack("かんじ") = (c0<<56)|(c1<<48)|(c2<<40)|0...0
 *
 * WHY THIS EXISTS. Not for speed — a 13-17 probe binary search costs
 * microseconds against a budget of tens of milliseconds. It exists so
 * that the definition of "sorted" lives in exactly one table. SKK-JISYO
 * ships sorted in EUC-JP byte order, and transcoding to UTF-8 silently
 * reorders it (U+30FC "-" and U+FF1A move; 2 violations in M, 285 in
 * L). A binary search over a mis-sorted array does not crash, it
 * answers wrong for some words on some probe paths. With a packed key,
 * the prep tool that sorts and the engine that searches call the same
 * skk_entry_cmp(), so they cannot disagree. skk_alphabet_crc32() is
 * stamped into the image and checked at open, so an image built by an
 * older table is rejected loudly instead of answering wrong quietly.
 *
 * Collisions (readings agreeing in their first 8 characters) are 0.25%
 * in M and 3.19% in L, and only ever occur at the end of a search, so
 * the dictionary text itself is touched once or twice per lookup. */
#define SKK_PACK_CHARS 8    /* characters folded into the uint64 */
#define SKK_CODE_END   0x00 /* terminator / padding; no character owns it */
#define SKK_CODE_ESC   0xFF /* character not in the table */

/* Code for one Unicode scalar. SKK_CODE_ESC when it is not in the
   table. Never returns SKK_CODE_END. */
uint8_t skk_char_code(uint32_t cp);

/* Fold a UTF-8 reading into its packed key. `out_escaped` (optional)
   reports that at least one character fell to SKK_CODE_ESC — the prep
   tool uses it for QA, search does not care. Invalid UTF-8 bytes are
   each treated as one escaped character rather than rejected, so a
   corrupt line can never desynchronise the comparator. */
uint64_t skk_pack(const char *reading, size_t len, bool *out_escaped);

/* THE definition of dictionary order. Primary: the packed keys as
   unsigned integers. Tie-break: memcmp of the full UTF-8 readings, then
   the shorter one first. Returns <0, 0, >0.
   Both the prep tool's sort and the engine's binary search MUST use
   this and nothing else. It is a total order even for escaped
   characters, where it deliberately differs from raw UTF-8 order —
   consistency between writer and reader is what matters, not agreement
   with strcmp. */
int skk_entry_cmp(uint64_t ka, const char *a, size_t alen,
                  uint64_t kb, const char *b, size_t blen);

/* CRC-32 of the collation table. Changes whenever a character is added,
   removed or reordered; stamped into the image header. */
uint32_t skk_alphabet_crc32(void);

/* CRC-32/ISO-HDLC (the zlib polynomial), so the prep tool and the
   runtime share one implementation. Seed with 0. */
uint32_t skk_crc32(uint32_t seed, const void *data, size_t len);

/* UTF-8 stepping, so "delete one character" is not reinvented per call
   site. Both clamp to [0, len] and never run off a malformed sequence:
   a stray continuation byte advances/retreats by one. */
size_t skk_utf8_next(const char *s, size_t len, size_t i);
size_t skk_utf8_prev(const char *s, size_t len, size_t i);
/* Decode at `i`; *adv gets the byte count consumed (>=1). Returns the
   scalar, or 0xFFFD for a malformed sequence. */
uint32_t skk_utf8_decode(const char *s, size_t len, size_t i, size_t *adv);

/* ------------------------------------------------------------------ */
/* The dictionary image, as produced by tools/skk_prep.py.
 *
 * One blob holds the re-sorted entry text and both indices, so there is
 * a single base pointer and every offset in the file — index entries
 * and candidates alike — is relative to it.
 *
 * Layout (all little-endian, all offsets from the image base):
 *
 *   0x00  skk_image_hdr_t          (64 B, see below)
 *   ...   uint64 keys[nasi_count]  8-byte aligned, ascending
 *   ...   uint64 fan[fan_total]    8-byte aligned, the sampled tree below
 *   ...   uint32 offs[nasi_count]  4-byte aligned, line starts
 *   ...   uint64 keys[ari_count]
 *   ...   uint64 fan[fan_total]
 *   ...   uint32 offs[ari_count]
 *   ...   entry text               "<reading> /c1/c2/.../\n" lines
 *
 * ALIGNMENT. The uint64 arrays are read directly, so the image base
 * must be 8-byte aligned; skk_dict_open() returns SKK_ERR_ALIGN
 * otherwise rather than trapping on an unaligned load.
 *
 * DO NOT ship the image with IDF's `EMBED_FILES`. It emits no alignment
 * directive at all — the section comes out sh_addralign = 1, and on this
 * project's own build three of the four existing _binary_*_start symbols
 * are not 8-byte aligned (one is at an odd address). The address is the
 * running sum of every preceding rodata contribution, so it changes with
 * unrelated commits: the firmware would work today and lose its
 * dictionary after the next one, with no build error either way.
 * skk_dict_image.S does it correctly with .balign 64 + .incbin; call
 * skk_builtin_image() to get the blob.
 *
 * BOTH BLOCKS ASCEND. SKK-JISYO stores okuri-ari descending, but this
 * index is ours to define, so the prep tool re-sorts that block
 * ascending too. One comparator, one search direction, one less way to
 * be subtly wrong. (The entry text may stay in either order; only
 * offs[] defines what the index means.) */

#define SKK_IMAGE_MAGIC   0x314b4b53u /* "SKK1" */
#define SKK_IMAGE_VERSION 2u          /* 2 added the sampled tree (below) */
#define SKK_IMAGE_ALIGN   8u

typedef struct {
    uint32_t magic;         /* SKK_IMAGE_MAGIC */
    uint16_t version;       /* SKK_IMAGE_VERSION */
    uint16_t flags;         /* reserved, 0 */
    uint32_t image_len;     /* total bytes; must equal blob.len */
    uint32_t payload_crc32; /* CRC32 of image[64 .. image_len) */
    uint32_t alphabet_crc32;/* must equal skk_alphabet_crc32() */
    uint32_t text_off;
    uint32_t text_len;
    uint32_t nasi_count;
    uint32_t nasi_keys_off; /* uint64[nasi_count] */
    uint32_t nasi_offs_off; /* uint32[nasi_count] */
    uint32_t ari_count;
    uint32_t ari_keys_off;
    uint32_t ari_offs_off;
    /* uint64[skk_fan_total(count)], 0 when the block has no tree. Only
       the START is stored: every level's size is a pure function of
       `count` (skk_fan_levels), so the writer and the reader cannot
       disagree about the shape of the thing. */
    uint32_t nasi_fan_off;
    uint32_t ari_fan_off;
    uint32_t reserved[1];
} skk_image_hdr_t;          /* exactly 64 bytes; the CRC covers what follows */

/* Enforced, because the header size is part of the on-disk format: a
   silently grown struct would shift every section and the CRC range. */
typedef char skk_image_hdr_size_check[(sizeof(skk_image_hdr_t) == 64) ? 1 : -1];

/* Which half of the dictionary. okuri-ari readings carry a trailing
   ASCII consonant ("うごk"); okuri-nasi do not. They are separate
   sorted spaces, which halves the search and removes any question of
   how "うご" orders against "うごk". */
typedef enum {
    SKK_BLK_NASI = 0, /* okuri-nasi */
    SKK_BLK_ARI  = 1, /* okuri-ari  */
    SKK_BLK_COUNT
} skk_blk_t;

/* ------------------------------------------------------------------ */
/* The sampled search tree — one cache line per level, not one per
 * bisection step (design §6.5).
 *
 * A plain binary search over keys[] lands on a different 64-byte line at
 * every step. Measured over every heading of every shipped dictionary,
 * that is 12.8 lines for plum's okuri-nasi block and 15.5-17.4 for
 * bamboo's and pine's, on arrays of 67 KB / 390 KB / 1.41 MB. Once the
 * dictionary is mmap'd flash rather than something L2 can hold, those
 * lines ARE the search: the comparisons are free and the misses are not.
 *
 * So the image carries sampled levels above the keys. Level 1 is every
 * 8th key, level 2 every 8th of those, up until a level holds fewer than
 * eight:
 *
 *     L[0] = keys,   L[k+1][j] = L[k][8j + 7]
 *
 * Each separator IS one of the keys below it, so a lower_bound at level
 * k+1 pins the lower_bound at level k to eight consecutive, 8-ALIGNED
 * elements — one 64-byte line, whichever way the comparison goes. The
 * search is therefore a scan of at most 8 uint64 per level and touches
 * log8(n) lines instead of log2(n):
 *
 *   block              n     bisecting   tree   the tree costs
 *   M   okuri-nasi   6,934    12.8 ln    5 ln    7,904 B  (+9.5%)
 *   ML  okuri-nasi  44,401    15.5 ln    6 ln   50,720 B  (+9.5%)
 *   L   okuri-nasi 159,795    17.4 ln    6 ln  182,600 B  (+9.5%)
 *
 * (measured by tools/test_skk_dict.c over every heading of every shipped
 * dictionary, both blocks, plus a guaranteed-absent variant of each so
 * the bounds that fall BETWEEN entries are covered too; the percentage
 * is against that block's keys[] + offs[]). The top levels are small
 * enough to stay in L2 between lookups — bamboo's levels 3-5 are 97 keys
 * = 776 B — so the lines actually fetched from flash are fewer still.
 *
 * THIS IS ALSO §4.5's FIRST-CHARACTER BUCKET INDEX, generalised. A
 * bucket table is a one-level sampled index that assumes the first
 * character splits the space evenly; this is the same idea at every
 * level and assumes nothing. Adding both would be redundant.
 *
 * WHY NOT THE EYTZINGER / B-TREE PERMUTATION §6.5 first proposed: it
 * reorders keys[], and two things downstream walk to the NEXT entry in
 * sorted order after a hit — the packed-key collision group in
 * find_entry(), and TAB completion in skk_complete(). Sampling leaves
 * the sorted array exactly as it was and costs n/7 extra keys (+10% of
 * the index, +8% of a bamboo image), so nothing downstream changes and
 * a block without a tree still searches correctly.
 *
 * THE TREE IS DERIVED DATA, and a tree that disagrees with its keys does
 * not crash — it answers wrong, the same failure mode as a mis-sorted
 * dictionary (§6.1). Two things keep that from shipping: every level
 * size is computed from `count` alone (so writer and reader cannot
 * disagree about the shape), and the prep tool re-searches every single
 * heading THROUGH THE TREE before it writes an image out. */
#define SKK_FANOUT          8u  /* uint64 per 64-byte line */
#define SKK_FAN_LEVELS_MAX 12u  /* 8^11 > 2^32; index 0 is keys itself */

/* Sizes of the sampled levels above `count` sorted keys. Sets
   lvl_n[0] = count and lvl_n[1..T], and returns T — 0 when count is
   below SKK_FANOUT and the block is simply scanned. `lvl_n` must hold
   SKK_FAN_LEVELS_MAX entries. A pure function of `count`: the image
   stores where the tree starts and nothing else. */
uint32_t skk_fan_levels(uint32_t count, uint32_t *lvl_n);

/* Total uint64 in the tree, i.e. the sum of lvl_n[1..T]. */
uint32_t skk_fan_total(uint32_t count);

/* Fill `fan` (skk_fan_total(count) elements) from a sorted key array.
   For the prep tool and the host tests; the firmware only reads. */
void skk_fan_build(const uint64_t *keys, uint32_t count, uint64_t *fan);

typedef struct {
    const uint64_t *keys;
    const uint32_t *offs;
    uint32_t        count;
    /* lvl[0] is keys itself, lvl[1..levels] the sampled levels as laid
       out in the image. levels == 0 means there is no tree — either the
       block is shorter than SKK_FANOUT or the image was built without
       one — and the search falls back to bisection. */
    uint8_t         levels;
    const uint64_t *lvl[SKK_FAN_LEVELS_MAX];
    uint32_t        lvl_n[SKK_FAN_LEVELS_MAX];
} skk_index_t;

/* Opened dictionary. Caller-owned, no allocation, safe to share
   read-only between several skk_t. */
typedef struct {
    skk_blob_t  image;
    const uint8_t *text;     /* image.base + hdr.text_off */
    size_t         text_len;
    skk_index_t    blk[SKK_BLK_COUNT];
} skk_dict_t;

/* skk_dict_open() flags */
#define SKK_OPEN_VERIFY (1u << 0) /* also check payload_crc32 (reads the
                                     whole image: ~295 KB for plum). Cheap
                                     insurance once the dictionary lives in
                                     a flashed partition rather than in the
                                     firmware image itself. */

/* Attach to an image. Validates magic, version, alignment, section
   bounds and the alphabet stamp; with SKK_OPEN_VERIFY also the CRC.
   `d` may live anywhere; it holds no resources, so there is no close. */
int skk_dict_open(skk_dict_t *d, skk_blob_t image, uint32_t flags);

/* The image linked into the firmware, or {NULL, 0} when it was built
 * without one. Read in place — do NOT free it, and do not pass
 * SKK_OPEN_VERIFY: the bootloader already checks the whole app image
 * with SHA-256 on every boot, so a CRC pass here re-does that job with a
 * weaker checksum three orders of magnitude slower (§6.10). */
skk_blob_t skk_builtin_image(void);


/* ------------------------------------------------------------------ */
/* Candidates — zero copy.
 *
 * A candidate is an offset and a length into memory skk_core already
 * has mapped; nothing is copied until the JS binding calls
 * JS_NewStringLen, once, on entering v mode.
 *
 * `src` says which base `off` counts from. It costs nothing (the struct
 * is 8 bytes either way once aligned) and keeps katakana conversion —
 * which is not in the image — from forcing a format change.
 * Resolve it with skk_cand() rather than by hand. */
typedef enum {
    SKK_SRC_DICT    = 0, /* off is from skk_dict_t.image.base */
    SKK_SRC_SCRATCH = 1, /* off is into skk_t.scratch (e.g. the reading
                            rendered as katakana) */
    /* 2 was SKK_SRC_USER, the personal dictionary's own storage
       (removed 2026-07-30). Left unassigned rather than reused: a
       skk_cand_t never leaves the process, so nothing forces the
       number to be recycled, and a `2` appearing anywhere is a bug
       worth being able to recognise. */
} skk_src_t;

typedef struct {
    uint32_t off;
    uint16_t len;
    uint16_t src; /* skk_src_t */
} skk_cand_t;

/* Candidate parsing, so the implementer and the prep tool agree on what
 * an entry line yields (v1 scope — §5 "not in the first version"):
 *
 *   line   := reading SP '/' cand ('/' cand)* '/' '\n'
 *   cand   := text [';' annotation]
 *
 *   - the annotation after ';' is stripped; it is not displayed in v1,
 *   - a candidate starting with '(' is a (concat "...") form and is
 *     SKIPPED in v1 (it needs unescaping into a scratch buffer),
 *   - a "[okuri/c1/c2/]" group inside an okuri-ari entry is SKIPPED
 *     whole in v1 (okurigana-specific candidate ordering is S7 work),
 *   - an empty candidate is skipped.
 * Skipping is silent; skk_stats_t.dropped counts it. */

/* ------------------------------------------------------------------ */
/* Fixed capacities. Everything the engine touches per keystroke is
 * inside skk_t, so nothing allocates and nothing can fragment. Sizes
 * are byte counts of UTF-8, not characters. */
#define SKK_ROMA_MAX     8   /* pending romaji ("tch" + a spare) */
#define SKK_READING_MAX  96  /* the ~ reading. Measured 2026-07-29: the longest
                                reading in any shipped dictionary is 75 B (L);
                                M and ML top out at 42 B. */
#define SKK_OKURI_MAX    16  /* kana produced by the okurigana */
#define SKK_CAND_MAX     64  /* candidates kept; the rest are dropped */
#define SKK_PREEDIT_MAX  192 /* rendered "~かんじ*り" incl. markers */
#define SKK_COMMIT_MAX   256 /* one candidate + okurigana */
#define SKK_SCRATCH_MAX  96  /* katakana rendering of the reading, etc. */

/* Counters, no clock: skk_core must not depend on esp_timer and must
 * build on a host, so wall time is measured by the caller (the mqjs
 * binding brackets the engine call with esp_timer_get_time and exposes
 * both through ui.imeStats()). What only the engine can know is
 * counted here. */
typedef struct {
    uint32_t keys;       /* skk_key() calls */
    uint32_t consumed;   /* ... of which returned non-zero */
    uint32_t lookups;    /* dictionary searches */
    uint32_t probes;     /* index probes: one per 64-byte line the search
                            touches — one tree level, or one bisection
                            step in a block with no tree */
    uint32_t fullcmp;    /* packed-key hits that needed a byte compare */
    uint32_t cands;      /* candidates produced */
    uint32_t dropped;    /* candidates skipped or past SKK_CAND_MAX */
    uint32_t commits;
} skk_stats_t;

/* ------------------------------------------------------------------ */
/* Machinery skk_dict.c exports that the state machine, the prep tool
 * and the tests all need. These lived as guarded prototypes inside
 * skk_dict.c during bring-up; they belong here, because a struct that
 * every translation unit has to hand-copy is a silent-divergence bug
 * waiting to happen. Defining the guards below turns those in-file
 * blocks into no-ops. */
#define SKK_DICT_EXTRAS_DECLARED 1

/* skk_lookup() plus the counters, so the state machine fills
 * skk_stats_t.probes/fullcmp/cands/dropped without a second search.
 * skk_lookup() is this with a NULL `st`. */
int skk_lookup_stats(const skk_dict_t *d, skk_blk_t blk,
                     const char *reading, size_t len,
                     skk_cand_t *out, size_t cap, size_t *out_n,
                     skk_stats_t *st);
/* Find the ";; okuri-ari/nasi entries." markers. skk_index_build() takes
 * ONE block, so this is the first thing the prep tool must do (§6.1). */
int skk_text_split(skk_blob_t text, skk_blob_t *out_ari, skk_blob_t *out_nasi);
/* Monotonicity check on the arrays as built, before an image exists.
 * skk_index_verify() is the same check on an opened image. */
int skk_index_check(skk_blob_t text, const uint64_t *keys, const uint32_t *offs,
                    size_t n, uint32_t *out_at);

/* The `nth` reading (0-based) that starts with `prefix` and is longer
 * than it — TAB completion (§6.8).
 *
 * Free because the index is already sorted: one binary search puts us at
 * the head of the run and the matches are contiguous after it. Measured
 * over SKK-JISYO.L, a 2-character prefix has a median of 5 matches and
 * 78 bytes of forward scan (1.2 cache lines); callers stop at the handful
 * they display, so the cost is O(log N + nth), not O(matches).
 *
 * Returns 1 and points *out at the reading (inside the dictionary image,
 * NOT NUL-terminated, valid as long as the image is) when there is an
 * nth match, 0 when there is not, and a negative skk_err_t on a bad
 * argument. `probes` may be NULL. */
int skk_complete(const skk_dict_t *d, skk_blk_t blk,
                 const char *prefix, size_t plen, size_t nth,
                 const char **out, size_t *out_len, uint32_t *probes);

/* ------------------------------------------------------------------ */
/* How the preedit is composed.
 *
 * The renderer wants to paint the parts differently — the reading is
 * provisional input, a chosen candidate is not, and the okurigana is a
 * third thing again — and it CANNOT work the boundaries out from the
 * string. In SELECT the candidate and the okurigana are concatenated
 * with no separator at all (skk_kana.c build_preedit), so "ﾃｷｽﾄ + り"
 * and a candidate that happens to end in り are the same bytes. Only
 * this engine knows the lengths.
 *
 * So build_preedit records the spans AS IT BUILDS rather than anything
 * downstream re-deriving the rule. There is exactly one description of
 * how a preedit is put together, and it is the code that puts it
 * together.
 *
 * The spans PARTITION the preedit: concatenating them in order gives
 * skk_preedit() back, byte for byte. A renderer that wants to drop the
 * ▽/▼ marker just skips that span instead of slicing the string. */
typedef enum {
    SKK_SPAN_MARK = 0,  /* ▽ (U+25BD) or ▼ (U+25BC) */
    SKK_SPAN_READING,   /* the reading being typed, in ▽ */
    SKK_SPAN_CAND,      /* the selected candidate, in ▼ */
    SKK_SPAN_SEP,       /* the '*' between reading and okurigana */
    SKK_SPAN_OKURI,     /* the okurigana */
    SKK_SPAN_ROMA,      /* romaji that has not become kana yet */
} skk_span_kind_t;

/* mark + reading/cand + sep + okuri + roma */
#define SKK_SPAN_MAX 5

typedef struct {
    uint16_t off;  /* byte offset into skk_preedit() */
    uint16_t len;
    uint16_t kind; /* skk_span_kind_t */
} skk_span_t;

/* ------------------------------------------------------------------ */
/* The IME instance.
 *
 * Laid out in the header on purpose: the caller decides where it lives
 * (a static, or heap_caps_malloc into PSRAM) and skk_core never calls
 * malloc. ~1.2 KB. Fields below the marker are private — read them in
 * tests if it helps, but the accessors are the contract. */
typedef struct {
    /* ---- private ---- */
    const skk_dict_t *dict;
    uint8_t  enabled;
    uint8_t  mode;        /* skk_mode_t */
    uint8_t  base_mode;   /* KANA or KATA: where ~/v return to */
    uint8_t  pad0;

    char     roma[SKK_ROMA_MAX];   /* un-converted romaji tail */
    uint8_t  roma_len;

    char     reading[SKK_READING_MAX]; /* the ~ span, kana + okuri stem */
    uint16_t reading_len;
    uint8_t  okuri_stem;           /* ASCII consonant that opened OKURI, 0 = none */
    char     okuri[SKK_OKURI_MAX]; /* kana of the okurigana */
    uint8_t  okuri_len;

    skk_cand_t cand[SKK_CAND_MAX];
    uint16_t cand_n;
    int16_t  sel;                  /* -1 when there is no selection */

    /* TAB completion. No list is kept: each TAB re-runs the prefix search
       from comp_len and takes the comp_i-th match, so cycling costs a
       binary search (13-17 probes, µs) instead of an array. comp_len = 0
       means "not completing"; any non-TAB key clears it. */
    uint16_t comp_len;             /* bytes of the reading TAB started from */
    uint16_t comp_i;               /* which match is being shown */

    char     preedit[SKK_PREEDIT_MAX];
    uint16_t preedit_len;
    /* How that preedit is composed, recorded BY build_preedit as it
       builds (see skk_preedit_spans). 30 bytes, written once per key
       and read once per repaint. */
    skk_span_t span[SKK_SPAN_MAX];
    uint8_t    span_n;
    char     commit[SKK_COMMIT_MAX];
    uint16_t commit_len;
    char     scratch[SKK_SCRATCH_MAX];
    uint16_t scratch_len;

    skk_stats_t stats;
} skk_t;

/* ------------------------------------------------------------------ */
/* Lifecycle. No allocation, so there is no failure path and no close;
   dropping the struct is enough. */

/* Zero the state. Mode becomes SKK_MODE_KANA, IME disabled, no dict. */
void skk_init(skk_t *s);

/* Point at a dictionary (or NULL to detach). `d` must outlive `s`; it
   is shared read-only, so several skk_t may use one. Conversion without
   a dictionary returns SKK_ERR_NODICT rather than misbehaving. */
void skk_attach(skk_t *s, const skk_dict_t *d);

/* Abandon any preedit/candidates and return to base_mode. Whatever was
   being typed is discarded, NOT committed — call it when an app loses
   focus or the session it was typing into goes away. */
void skk_reset(skk_t *s);

/* IME on/off. Off means skk_key() consumes nothing at all. The app
   normally short-circuits before even calling (see §4.2), so this is
   for the case where the same skk_t is reachable from several paths. */
void skk_enable(skk_t *s, bool on);
bool skk_enabled(const skk_t *s);

/* Force a mode. Only ASCII / KANA / KATA are accepted; anything else
   returns SKK_ERR_ARG, because MIDASHI/OKURI/SELECT need state that
   only the key path can build. This is how an app leaves ASCII mode,
   since C-j is unreachable on this keyboard (see skk_mode_t). Any
   pending preedit is discarded, as skk_reset(). */
int skk_set_mode(skk_t *s, skk_mode_t m);

/* ------------------------------------------------------------------ */
/* The hot path.
 *
 * `key`/`len` is exactly what mqjs delivers to ui.onKey: at most 8
 * bytes, one of
 *   - a printable byte, or UTF-8 for a character the surface produced,
 *   - "\b" (0x08), "\t" (0x09), "\n" (0x0A),
 *   - a control byte from Ctrl folding — 0x07 (C-g) cancels,
 *   - ESC-prefixed bytes from Alt, which the engine never consumes,
 *   - a "\0name" token: "left" "right" "up" "down" "esc" "del" are
 *     understood, everything else falls through.
 * A token is recognised by key[0] == '\0', so `len` is load-bearing —
 * do not pass strlen().
 *
 * Returns a SKK_ST_* bitmask; 0 means the app must handle the key
 * itself. Returning 0 costs one call and no allocation.
 *
 * The bindings this implements (v1 — abbrev '/', numeric '#',
 * annotations and dictionary servers are out of scope):
 *
 *   KANA/KATA   'l'          -> ASCII mode
 *               'q'          -> toggle KANA <-> KATA
 *               A-Z          -> start ~ with the lowercased letter
 *               other        -> romaji accumulates, kana commits out
 *   MIDASHI     SPACE        -> convert, enter v
 *               TAB          -> complete the reading; TAB again cycles
 *               'q'          -> commit the reading as katakana
 *               A-Z          -> start the okurigana (OKURI)
 *               BS           -> delete one character, empty ~ ends it
 *               C-g          -> cancel, discard the reading
 *               ENTER        -> commit the reading as typed
 *   OKURI       (kana completes) -> convert automatically, enter v
 *   SELECT      SPACE        -> next candidate
 *               'x'          -> previous; before the first, back to ~
 *               ENTER        -> commit the selection
 *               C-g          -> cancel back to ~
 *               anything else-> commit the selection, then re-feed the
 *                               key (so one call can set both COMMIT
 *                               and PREEDIT)
 *
 * The IME toggle itself is NOT here: the "\0ime" token / control-bar
 * button belongs to the app, which calls skk_enable(). Note the dock
 * cannot send Ctrl+Space — kbd_core drops it because NUL collides with
 * the token prefix — so §9.3's dock binding needs another key. */
uint32_t skk_key(skk_t *s, const char *key, size_t len);

/* ------------------------------------------------------------------ */
/* Accessors. All return pointers into `s` (or into the dictionary
 * image) and copy nothing.
 *
 * LIFETIME: every one of these is valid only until the next skk_key(),
 * skk_reset(), skk_set_mode() or skk_enable() on the same skk_t. The JS
 * binding materialises what it needs immediately after skk_key()
 * returns, which is exactly what the status bits tell it to do. */

skk_mode_t skk_mode(const skk_t *s);

/* The parts of that preedit, in order, partitioning it exactly (see
   skk_span_t). Returns how many and points *out at them; the array is
   valid for as long as skk_preedit()'s bytes are. Zero spans means
   there is nothing being composed. */
int skk_preedit_spans(const skk_t *s, const skk_span_t **out);

/* Display preedit, e.g. "▽かんじ" / "▽おく*り" / "▼漢字る". NUL
   terminated as a convenience; *len (optional) gets the byte length. */
const char *skk_preedit(const skk_t *s, size_t *len);

/* The string to insert. Only meaningful when the last skk_key()
   returned SKK_ST_COMMIT. Cleared on the next key. */
const char *skk_commit(const skk_t *s, size_t *len);

int skk_cand_count(const skk_t *s);
int skk_sel(const skk_t *s);            /* -1 when not in SELECT */
/* i-th candidate as UTF-8. NOT NUL terminated — use *len. Returns NULL
   when i is out of range. */
const char *skk_cand(const skk_t *s, int i, size_t *len);

void skk_stats(const skk_t *s, skk_stats_t *out);
void skk_stats_reset(skk_t *s);

/* ------------------------------------------------------------------ */
/* Dictionary search, exposed directly because the host test for S2
 * drives it without a state machine, and because skk_complete() above
 * shares its index.
 *
 * `reading` is UTF-8 as it appears in the dictionary: kana only for
 * SKK_BLK_NASI, kana plus the trailing ASCII stem ("うごk") for
 * SKK_BLK_ARI. Fills up to `cap` candidates and reports how many were
 * written. SKK_ERR_NOSPACE is not returned for a long entry — the tail
 * is dropped and counted — so a caller with a small array still gets
 * the first candidates. Returns SKK_OK even when nothing matched;
 * *out_n == 0 says so. */
int skk_lookup(const skk_dict_t *d, skk_blk_t blk,
               const char *reading, size_t len,
               skk_cand_t *out, size_t cap, size_t *out_n);

/* Resolve a candidate produced by skk_lookup() (SKK_SRC_DICT only). */
const char *skk_cand_text(const skk_dict_t *d, const skk_cand_t *c, size_t *len);

/* ------------------------------------------------------------------ */
/* Index construction — for the prep tool and for host tests, not for
 * the firmware. Kept in skk_core so the code that decides the order is
 * literally the code that searches it (see skk_entry_cmp).
 *
 * `text` is one block's entry lines (comments and ";; ..." headers
 * removed by the caller). Writes one key/offset pair per line, in the
 * order the lines appear — the caller then sorts the pairs with
 * skk_entry_cmp() before emitting them. Offsets are relative to
 * `text.base`; the writer rebases them onto the image.
 *
 * *out_count is always the number of entries found, even when it
 * exceeds `cap` (so a caller can size an array in one pass and fill it
 * in a second). Returns SKK_ERR_NOSPACE in that case, SKK_OK otherwise.
 * Allocates nothing: the arrays are the caller's. */
int skk_index_build(skk_blob_t text, uint64_t *keys, uint32_t *offs,
                    size_t cap, size_t *out_count);

/* Verify that an opened block really is sorted by skk_entry_cmp(), and
 * that its sampled tree samples the keys it claims to — the two ways an
 * index can be wrong without crashing (§6.1 and the tree note above).
 * O(n) and worth running once in the host test for every dictionary.
 * Returns SKK_OK, or SKK_ERR_FORMAT with *out_at set to the index of
 * the first entry that is not >= its predecessor, or to
 * count + <tree index> for a separator that is not the key it samples. */
int skk_index_verify(const skk_dict_t *d, skk_blk_t blk, uint32_t *out_at);

#ifdef __cplusplus
}
#endif
