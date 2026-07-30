/*
 * term_lp_ring.c — the LP SRAM black box (docs/term-design.md §4.4).
 * Contract, layout, and every decision worth arguing about: term_lp_ring.h.
 *
 * Layout of this file:
 *   - pure logic: CRC32, the strip rule, header/chain validation, format,
 *     append, the iterator, the two JSON builders. No ESP-IDF, no locks, no
 *     allocation. This is the part a host harness exercises against a plain
 *     malloc'd region, and the host_test runner compiles it into all 22
 *     suites.
 *   - the platform seam: the RTC_NOINIT region itself, esp_reset_reason,
 *     the monotonic clock and the PSRAM snapshot allocator, all
 *     ESP_PLATFORM-guarded with inert host equivalents (same shape as
 *     term_lp_probe.c had, and the reason the singleton answers under
 *     run_pc).
 *   - the singleton: boot, the tee, stats.
 *
 * The one rule that shapes the write path: every content byte lands in space
 * the CURRENTLY PUBLISHED header calls free, and the header is
 * double-buffered. A crash mid-append therefore costs the record in flight
 * and nothing else (term_lp_ring.h, P5).
 */
#include "term_lp_ring.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "term_lp";
#define LPLOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LPLOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
#define LPLOGI(fmt, ...) do { } while (0)
#define LPLOGW(fmt, ...) do { } while (0)
#endif

/* C99 has no _Static_assert (host_test builds -std=c99). */
typedef char lp_hdr_size_check[(sizeof(term_lp_hdr_t) == TERM_LP_HDR_BYTES) ? 1 : -1];
typedef char lp_rec_size_check[(sizeof(term_lp_rec_t) == TERM_LP_REC_HDR) ? 1 : -1];
typedef char lp_part_size_check[(sizeof(term_lp_part_t) == 32) ? 1 : -1];
typedef char lp_sys_align_check[(TERM_LP_SYS_BYTES % TERM_LP_REC_ALIGN == 0) ? 1 : -1];
typedef char lp_app_align_check[(TERM_LP_APP_BYTES % TERM_LP_REC_ALIGN == 0) ? 1 : -1];
/* A record must fit in an empty partition with room for a PAD header. */
typedef char lp_recmax_check[
    (TERM_LP_REC_HDR + TERM_LP_REC_MAX + TERM_LP_REC_HDR < TERM_LP_SYS_BYTES) ? 1 : -1];

#define LP_ALIGN_UP(n) (((uint32_t)(n) + (TERM_LP_REC_ALIGN - 1u)) & \
                        ~(uint32_t)(TERM_LP_REC_ALIGN - 1u))

/* ===================================================================== */
/* pure logic: CRC32                                                     */
/* ===================================================================== */

/* IEEE 802.3, reflected — the polynomial esp_rom_crc32_le uses, spelled out
   so host and device compute the same check with no ROM symbol. */
static uint32_t lp_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* ===================================================================== */
/* pure logic: the strip rule (term_lp_ring.h, P4)                       */
/* ===================================================================== */

/* Longest complete UTF-8 sequence ending at or before `n`; cuts an
   incomplete trailing sequence so a pulled log is always valid UTF-8. */
static size_t lp_utf8_backoff(const char *s, size_t n)
{
    size_t k = n;
    int back = 0;

    while (k > 0 && back < 4) {
        unsigned char c = (unsigned char)s[k - 1];
        size_t need;
        if ((c & 0xC0u) == 0x80u) {   /* continuation byte, keep looking */
            k--;
            back++;
            continue;
        }
        if (c < 0x80u)                 need = 1;
        else if ((c & 0xE0u) == 0xC0u) need = 2;
        else if ((c & 0xF0u) == 0xE0u) need = 3;
        else if ((c & 0xF8u) == 0xF0u) need = 4;
        else                           need = 1;   /* stray 0x80..0xBF/0xF8+ */
        return (k - 1 + need <= n) ? n : k - 1;
    }
    return n;
}

size_t term_lp_strip(char *dst, size_t dst_cap, const char *src, size_t len,
                     bool *out_trunc)
{
    size_t i = 0, n = 0;
    bool trunc = false;

    if (out_trunc)
        *out_trunc = false;
    if (!src)
        return 0;

    while (i < len) {
        unsigned char c = (unsigned char)src[i];

        if (c == 0x1Bu) {                    /* ESC: drop the whole sequence */
            unsigned char t;
            i++;
            if (i >= len)
                break;                        /* unterminated: drop the rest */
            t = (unsigned char)src[i++];
            if (t == '[') {                   /* CSI ... final 0x40..0x7E */
                while (i < len) {
                    unsigned char f = (unsigned char)src[i++];
                    if (f >= 0x40u && f <= 0x7Eu)
                        break;
                }
            } else if (t >= 0x20u && t <= 0x2Fu) {
                /* nF sequences: one or more intermediates then a final
                   0x30..0x7E. ESC ( B and friends turn up in ordinary
                   terminal output, and dropping only two of their three
                   bytes would leave a stray letter in the log. */
                while (i < len) {
                    unsigned char f = (unsigned char)src[i++];
                    if (f >= 0x30u && f <= 0x7Eu)
                        break;
                }
            } else if (t == ']' || t == 'P' || t == 'X' || t == '^' ||
                       t == '_') {            /* OSC / DCS / SOS / PM / APC */
                while (i < len) {
                    unsigned char f = (unsigned char)src[i++];
                    if (f == 0x07u)
                        break;                /* BEL terminator */
                    if (f == 0x1Bu) {         /* ST = ESC \ */
                        if (i < len)
                            i++;
                        break;
                    }
                }
            }
            /* anything else was ESC + one byte, already consumed */
            continue;
        }

        if ((c < 0x20u || c == 0x7Fu) && c != '\n' && c != '\t') {
            i++;                              /* other C0 / DEL: dropped */
            continue;
        }

        if (n >= dst_cap) {
            trunc = true;
            break;
        }
        if (dst)
            dst[n] = (char)c;
        n++;
        i++;
    }

    if (trunc && dst)
        n = lp_utf8_backoff(dst, n);
    if (out_trunc)
        *out_trunc = trunc;
    return n;
}

/* ===================================================================== */
/* pure logic: header slots and validation                               */
/* ===================================================================== */

static uint32_t lp_hdr_crc(const term_lp_hdr_t *h)
{
    return lp_crc32(0, (const uint8_t *)h, offsetof(term_lp_hdr_t, crc));
}

static const uint32_t k_part_off[TERM_LP_PART_COUNT] = {
    TERM_LP_SYS_OFF, TERM_LP_APP_OFF
};
static const uint32_t k_part_cap[TERM_LP_PART_COUNT] = {
    TERM_LP_SYS_BYTES, TERM_LP_APP_BYTES
};

/* Read one header slot and check the fixed fields plus its CRC. */
static bool lp_slot_load(const uint8_t *base, unsigned slot,
                         term_lp_hdr_t *out, bool *magic_ok, bool *ver_ok)
{
    memcpy(out, base + (size_t)slot * TERM_LP_HDR_BYTES, sizeof *out);
    if (magic_ok)
        *magic_ok = (out->magic == TERM_LP_MAGIC);
    if (ver_ok)
        *ver_ok = (out->version == TERM_LP_VERSION &&
                   out->hdr_bytes == TERM_LP_HDR_BYTES &&
                   out->region_bytes == TERM_LP_REGION_BYTES);
    if (out->magic != TERM_LP_MAGIC || out->version != TERM_LP_VERSION ||
        out->hdr_bytes != TERM_LP_HDR_BYTES ||
        out->region_bytes != TERM_LP_REGION_BYTES)
        return false;
    return out->crc == lp_hdr_crc(out);
}

static bool lp_geom_ok(const term_lp_hdr_t *h)
{
    int i;
    for (i = 0; i < TERM_LP_PART_COUNT; i++) {
        const term_lp_part_t *p = &h->part[i];
        if (p->off != k_part_off[i] || p->cap != k_part_cap[i])
            return false;
        if (p->head >= p->cap || p->tail >= p->cap || p->used > p->cap)
            return false;
        if ((p->head | p->tail | p->used) % TERM_LP_REC_ALIGN)
            return false;
        if ((p->tail + p->used) % p->cap != p->head)
            return false;
    }
    if (h->writers > TERM_LP_WRITERS)
        return false;
    return true;
}

/* Walk one partition from tail to head. A chain that walks cleanly and lands
   exactly on head, with a record count that matches the header, is the
   structural integrity check that stands in for a per-record CRC (§1's
   device result: retention is bit-perfect or absent, never subtly rotten). */
static bool lp_chain_ok(const uint8_t *base, const term_lp_part_t *p)
{
    uint32_t off = p->tail, left = p->used, recs = 0;

    while (left) {
        term_lp_rec_t rec;
        uint32_t sz, cap_len;
        term_lp_class_t cls;

        if (left < TERM_LP_REC_HDR)
            return false;
        memcpy(&rec, base + p->off + off, sizeof rec);
        cls = TERM_LP_CLASS_OF(rec.cls);
        cap_len = (cls == TERM_LP_CLASS_PAD) ? p->cap - TERM_LP_REC_HDR
                                             : TERM_LP_REC_MAX;
        if (cls != TERM_LP_CLASS_APP && cls != TERM_LP_CLASS_SYS &&
            cls != TERM_LP_CLASS_TERM && cls != TERM_LP_CLASS_PAD)
            return false;
        if (rec.len > cap_len)
            return false;
        sz = TERM_LP_REC_HDR + LP_ALIGN_UP(rec.len);
        if (sz > left || off + sz > p->cap)   /* records never straddle */
            return false;
        if (cls != TERM_LP_CLASS_PAD)
            recs++;
        off = (off + sz) % p->cap;
        left -= sz;
    }
    return off == p->head && recs == p->records;
}

static bool lp_region_zeroed(const uint8_t *base, size_t bytes)
{
    size_t i;
    for (i = 0; i < bytes; i++)
        if (base[i])
            return false;
    return true;
}

static bool lp_wt_ok(const term_lp_ring_t *r)
{
    const uint8_t *wt = r->base + TERM_LP_WT_OFF;
    unsigned n = r->hdr.writers;
    if (!n)
        return true;
    /* Only the first `writers` entries are covered: a torn write into the
       entry beyond them is unreferenced and must not invalidate the rest. */
    return lp_crc32(0, wt, (size_t)n * TERM_LP_WRITER_MAX) == r->hdr.wt_crc;
}

bool term_lp_ring_open(term_lp_ring_t *r, void *region, size_t bytes,
                       bool read_only, term_lp_check_t *out)
{
    term_lp_check_t chk;
    term_lp_hdr_t h[TERM_LP_HDR_SLOTS];
    bool slot_ok[TERM_LP_HDR_SLOTS];
    bool m[TERM_LP_HDR_SLOTS], v[TERM_LP_HDR_SLOTS];
    const uint8_t *base = (const uint8_t *)region;
    int i, best = -1;

    memset(&chk, 0, sizeof chk);
    if (out)
        *out = chk;
    if (!r || !region || bytes < TERM_LP_REGION_BYTES ||
        ((uintptr_t)region % 8u))
        return false;
    memset(r, 0, sizeof *r);

    for (i = 0; i < (int)TERM_LP_HDR_SLOTS; i++) {
        slot_ok[i] = lp_slot_load(base, (unsigned)i, &h[i], &m[i], &v[i]);
        if (slot_ok[i] && (best < 0 || h[i].hseq > h[best].hseq))
            best = i;
    }
    chk.magic_ok = m[0] || m[1];
    chk.version_ok = v[0] || v[1];
    chk.crc_ok = (best >= 0);
    if (!chk.magic_ok)
        chk.zeroed = lp_region_zeroed(base, TERM_LP_REGION_BYTES);
    if (best < 0) {
        if (out)
            *out = chk;
        return false;
    }

    chk.slot = (uint8_t)best;
    chk.boot_seq = h[best].boot_seq;
    chk.reset_reason = h[best].reset_reason;
    chk.geometry_ok = lp_geom_ok(&h[best]);
    if (chk.geometry_ok) {
        chk.chain_ok = true;
        for (i = 0; i < TERM_LP_PART_COUNT; i++) {
            if (!lp_chain_ok(base, &h[best].part[i]))
                chk.chain_ok = false;
            chk.records[i] = h[best].part[i].records;
            chk.used[i] = h[best].part[i].used;
        }
    }
    chk.ok = chk.magic_ok && chk.version_ok && chk.crc_ok &&
             chk.geometry_ok && chk.chain_ok;
    if (!chk.ok) {
        if (out)
            *out = chk;
        return false;
    }

    r->base = (uint8_t *)region;
    r->bytes = bytes;
    r->attached = true;
    r->read_only = read_only;
    r->slot = (uint8_t)best;
    r->hdr = h[best];
    r->wt_ok = lp_wt_ok(r);
    r->wcache_idx = TERM_LP_WRITER_UNKNOWN;
    chk.wt_crc_ok = r->wt_ok;
    if (out)
        *out = chk;
    return true;
}

/* ===================================================================== */
/* pure logic: format                                                    */
/* ===================================================================== */

/* Publish the shadow into the header slot that is NOT currently live (P5). */
static void lp_hdr_store(term_lp_ring_t *r)
{
    uint8_t slot = (uint8_t)(r->slot ^ 1u);
    r->hdr.hseq++;
    r->hdr.crc = lp_hdr_crc(&r->hdr);
    memcpy(r->base + (size_t)slot * TERM_LP_HDR_BYTES, &r->hdr, sizeof r->hdr);
    r->slot = slot;
}

bool term_lp_ring_format(term_lp_ring_t *r, void *region, size_t bytes,
                         uint32_t boot_seq, uint8_t reset_reason,
                         term_lp_prev_t prev, uint32_t prev_boot_seq)
{
    int i;

    if (!r || !region || bytes < TERM_LP_REGION_BYTES ||
        ((uintptr_t)region % 8u))
        return false;

    memset(r, 0, sizeof *r);
    r->base = (uint8_t *)region;
    r->bytes = bytes;
    r->attached = true;
    r->read_only = false;
    r->wt_ok = true;
    r->wcache_idx = TERM_LP_WRITER_UNKNOWN;

    r->hdr.magic = TERM_LP_MAGIC;
    r->hdr.version = (uint16_t)TERM_LP_VERSION;
    r->hdr.hdr_bytes = (uint16_t)TERM_LP_HDR_BYTES;
    r->hdr.region_bytes = TERM_LP_REGION_BYTES;
    r->hdr.hseq = 0;
    r->hdr.boot_seq = boot_seq;
    r->hdr.prev_boot_seq = prev_boot_seq;
    r->hdr.reset_reason = reset_reason;
    r->hdr.prev = (uint8_t)prev;
    r->hdr.writers = 0;
    r->hdr.wt_crc = 0;
    for (i = 0; i < TERM_LP_PART_COUNT; i++) {
        r->hdr.part[i].off = k_part_off[i];
        r->hdr.part[i].cap = k_part_cap[i];
    }

    /* Zero slot 1, publish slot 0 (hseq = 1): a leftover header from the
       previous session may carry a far higher hseq, and "greatest valid hseq
       wins" would then hand a reader the old session's pointers over a region
       we are about to overwrite. Zeroing takes it out of the race outright
       instead of trying to outrank it. */
    memset(r->base + TERM_LP_HDR_BYTES, 0, TERM_LP_HDR_BYTES);
    r->slot = 1;                  /* so the first store lands in slot 0 */
    lp_hdr_store(r);
    return true;
}

/* ===================================================================== */
/* pure logic: append                                                    */
/* ===================================================================== */

/* Staging for one record's stripped payload. File-static rather than
   per-handle: appends are serialised by the caller (term_lp_ring.h,
   SERIALISATION), and 512 B of .bss is not to be paid twice. */
static uint8_t s_stage[TERM_LP_REC_MAX];

/* Writer names are labels a human reads out of a pulled log; a control byte
   or a quote in one would corrupt the JSON dump. App names come from signed
   pushes and are already tame, so this is a belt on a brace.
 *
 * Returns false when the name does NOT fit whole into a 32-byte NUL-padded
 * table entry, i.e. when it is 32 bytes or longer. Truncating instead would
 * make two names that differ only past byte 31 intern to one entry and hand
 * two distinct apps the same writer_id, which term_lp_ring.h forbids outright
 * ("Names are stored whole, never truncated"). Same shape as phase 2's
 * decision #1 on over-long owner names: reject, never truncate. */
static bool lp_sanitise(char *dst, const char *src)
{
    size_t i = 0;
    memset(dst, 0, TERM_LP_WRITER_MAX);
    /* leave room for the NUL: names are <= 31 bytes, like MQJS_APP_NAME_MAX */
    while (src[i]) {
        unsigned char c;
        if (i >= TERM_LP_WRITER_MAX - 1u)
            return false;
        c = (unsigned char)src[i];
        dst[i] = (c < 0x20u || c == 0x7Fu || c == '"' || c == '\\')
                     ? '_' : (char)c;
        i++;
    }
    return true;
}

/* §4.4's writer_id: intern the name, return its table index. */
static uint8_t lp_intern(term_lp_ring_t *r, const char *writer)
{
    char name[TERM_LP_WRITER_MAX];
    uint8_t *wt = r->base + TERM_LP_WT_OFF;
    unsigned i;

    if (!writer || !writer[0]) {
        r->hdr.unnamed++;
        return TERM_LP_WRITER_UNKNOWN;
    }
    if (!lp_sanitise(name, writer)) {
        /* Too long to be stored whole: the table-full path, for the same
           reason (P2). A truncated entry could pass for another app's
           identity, and no writer_id at all is the honest answer. */
        r->hdr.unnamed++;
        return TERM_LP_WRITER_UNKNOWN;
    }
    /* One-entry cache in DRAM: consecutive records almost always share a
       writer, so the common case touches no LP memory at all. */
    if (r->wcache_idx < r->hdr.writers &&
        memcmp(r->wcache, name, TERM_LP_WRITER_MAX) == 0)
        return r->wcache_idx;
    for (i = 0; i < r->hdr.writers; i++) {
        if (memcmp(wt + (size_t)i * TERM_LP_WRITER_MAX, name,
                   TERM_LP_WRITER_MAX) == 0) {
            memcpy(r->wcache, name, TERM_LP_WRITER_MAX);
            r->wcache_idx = (uint8_t)i;
            return (uint8_t)i;
        }
    }
    if (r->hdr.writers >= TERM_LP_WRITERS) {
        /* Full: an unknown id, never somebody else's identity (P2). */
        r->hdr.unnamed++;
        return TERM_LP_WRITER_UNKNOWN;
    }
    i = r->hdr.writers;
    memcpy(wt + (size_t)i * TERM_LP_WRITER_MAX, name, TERM_LP_WRITER_MAX);
    r->hdr.writers = (uint8_t)(i + 1);
    r->hdr.wt_crc = lp_crc32(0, wt, (size_t)(i + 1) * TERM_LP_WRITER_MAX);
    r->wt_ok = true;
    memcpy(r->wcache, name, TERM_LP_WRITER_MAX);
    r->wcache_idx = (uint8_t)i;
    return (uint8_t)i;
}

/* Drop the oldest record. The only LP read on the append path: 8 bytes. */
static bool lp_evict_one(term_lp_ring_t *r, term_lp_part_t *p)
{
    term_lp_rec_t rec;
    uint32_t sz;

    if (!p->used)
        return false;
    memcpy(&rec, r->base + p->off + p->tail, sizeof rec);
    if (rec.len > p->cap - TERM_LP_REC_HDR)
        return false;                        /* corrupt: refuse to guess */
    sz = TERM_LP_REC_HDR + LP_ALIGN_UP(rec.len);
    if (sz > p->used)
        return false;
    if (TERM_LP_CLASS_OF(rec.cls) != TERM_LP_CLASS_PAD) {
        if (p->records)
            p->records--;
        p->evicted++;
        r->hdr.dropped_bytes += rec.len;
    }
    p->tail = (p->tail + sz) % p->cap;
    p->used -= sz;
    return true;
}

static void lp_write_rec(term_lp_ring_t *r, term_lp_part_t *p, uint32_t off,
                         uint8_t cls, uint8_t writer, uint32_t t_ms,
                         const uint8_t *payload, uint32_t len)
{
    term_lp_rec_t rec;
    uint8_t *dst = r->base + p->off + off;
    uint32_t pad = LP_ALIGN_UP(len) - len;

    rec.len = (uint16_t)len;
    rec.writer = writer;
    rec.cls = cls;
    rec.t_ms = t_ms;
    memcpy(dst, &rec, sizeof rec);            /* 8 B, 8-byte aligned */
    if (len && payload)
        memcpy(dst + TERM_LP_REC_HDR, payload, len);
    if (pad)
        memset(dst + TERM_LP_REC_HDR + len, 0, pad);
}

bool term_lp_ring_append(term_lp_ring_t *r, term_lp_class_t cls,
                         const char *writer, const char *text, size_t len,
                         uint32_t t_ms)
{
    term_lp_part_t *p;
    size_t n;
    uint32_t need, pad = 0;
    uint8_t widx, flags = 0;
    bool trunc = false, evicted = false;
    int part;

    if (!r || !r->attached || r->read_only)
        return false;
    /* The caller may pass TERM_LP_F_SCREEN alongside the class; every other
       flag bit is ours (TRUNC) or unassigned, and an unassigned bit must not
       reach the region — a reader would have to guess what it meant. */
    flags = (uint8_t)(TERM_LP_FLAGS_OF(cls) & TERM_LP_F_SCREEN);
    cls = TERM_LP_CLASS_OF(cls);
    if (cls != TERM_LP_CLASS_APP && cls != TERM_LP_CLASS_SYS &&
        cls != TERM_LP_CLASS_TERM) {
        r->hdr.refused++;
        return false;                         /* PAD is not for callers */
    }
    /* §4.4's 2026-07-30 exception: recorded session content shares the APP
       partition, so SYS's 8 KiB of platform last words stays out of its
       reach (term_lp_ring.h, RECORDING MODE). */
    part = (cls == TERM_LP_CLASS_SYS) ? TERM_LP_PART_SYS : TERM_LP_PART_APP;
    p = &r->hdr.part[part];

    n = term_lp_strip((char *)s_stage, TERM_LP_REC_MAX, text, len, &trunc);
    if (!n) {
        r->hdr.refused++;                     /* nothing left worth keeping */
        return false;
    }
    if (trunc) {
        flags |= TERM_LP_F_TRUNC;
        r->hdr.truncated++;
    }

    widx = lp_intern(r, writer);
    need = TERM_LP_REC_HDR + LP_ALIGN_UP((uint32_t)n);
    if (p->head + need > p->cap)
        pad = p->cap - p->head;               /* >= REC_HDR, 8-aligned */

    while (p->cap - p->used < pad + need) {
        if (!lp_evict_one(r, p)) {
            r->hdr.refused++;
            return false;                     /* only reachable on a corrupt
                                                 chain, which open() rejects */
        }
        evicted = true;
    }

    /* P5 step 1: if anything was evicted, publish the new tail BEFORE the
       bytes it released are overwritten. Until this store lands, the live
       header still describes the old range and the region stays walkable. */
    if (evicted)
        lp_hdr_store(r);

    if (pad) {
        lp_write_rec(r, p, p->head, (uint8_t)TERM_LP_CLASS_PAD,
                     TERM_LP_WRITER_UNKNOWN, t_ms, NULL,
                     pad - TERM_LP_REC_HDR);
        p->used += pad;
        p->head = 0;
    }
    lp_write_rec(r, p, p->head, (uint8_t)((unsigned)cls | flags), widx, t_ms,
                 s_stage, (uint32_t)n);
    p->head = (p->head + need) % p->cap;
    p->used += need;
    p->records++;
    p->appended++;

    /* P5 step 2: publish. A torn write here leaves the other slot valid and
       one record behind — the record in flight is lost, the region is not. */
    lp_hdr_store(r);
    return true;
}

/* ===================================================================== */
/* pure logic: reading                                                   */
/* ===================================================================== */

static const char *lp_writer_name(const term_lp_ring_t *r, uint8_t idx)
{
    const char *nm;

    if (!r->wt_ok || idx >= r->hdr.writers)
        return "?";
    nm = (const char *)(r->base + TERM_LP_WT_OFF +
                        (size_t)idx * TERM_LP_WRITER_MAX);
    /* A name is stored NUL-padded; if the last byte is not NUL the entry is
       not a string and must not be handed to %s. */
    if (nm[TERM_LP_WRITER_MAX - 1] != '\0' || !nm[0])
        return "?";
    return nm;
}

void term_lp_iter_begin(term_lp_iter_t *it, const term_lp_ring_t *r, int part)
{
    memset(it, 0, sizeof *it);
    if (!r || !r->attached)
        return;
    it->r = r;
    if (part < 0) {
        it->part = 0;
        it->part_end = TERM_LP_PART_COUNT;
    } else if (part < TERM_LP_PART_COUNT) {
        it->part = part;
        it->part_end = part + 1;
    } else {
        return;                               /* empty iteration */
    }
    it->off = r->hdr.part[it->part].tail;
    it->left = r->hdr.part[it->part].used;
    /* Oldest surviving record's ordinal: appended minus what is still live. */
    it->index = r->hdr.part[it->part].appended - r->hdr.part[it->part].records;
}

bool term_lp_iter_next(term_lp_iter_t *it, term_lp_record_t *out)
{
    if (!it->r || !out)
        return false;

    while (it->part < it->part_end) {
        const term_lp_part_t *p = &it->r->hdr.part[it->part];
        term_lp_rec_t rec;
        uint32_t sz;
        term_lp_class_t cls;

        if (it->left < TERM_LP_REC_HDR) {     /* partition done */
            it->part++;
            if (it->part < it->part_end) {
                it->off = it->r->hdr.part[it->part].tail;
                it->left = it->r->hdr.part[it->part].used;
                it->index = it->r->hdr.part[it->part].appended -
                            it->r->hdr.part[it->part].records;
            }
            continue;
        }
        memcpy(&rec, it->r->base + p->off + it->off, sizeof rec);
        cls = TERM_LP_CLASS_OF(rec.cls);
        sz = TERM_LP_REC_HDR + LP_ALIGN_UP(rec.len);
        if (sz > it->left || it->off + sz > p->cap) {
            it->left = 0;                     /* torn: stop this partition */
            continue;
        }
        if (cls == TERM_LP_CLASS_PAD) {       /* filler, never surfaced */
            it->off = (it->off + sz) % p->cap;
            it->left -= sz;
            continue;
        }
        out->cls = cls;
        out->flags = TERM_LP_FLAGS_OF(rec.cls);
        out->writer = lp_writer_name(it->r, rec.writer);
        out->t_ms = rec.t_ms;
        out->index = it->index++;
        out->text = it->r->base + p->off + it->off + TERM_LP_REC_HDR;
        out->len = rec.len;
        it->off = (it->off + sz) % p->cap;
        it->left -= sz;
        return true;
    }
    return false;
}

/* ===================================================================== */
/* names                                                                 */
/* ===================================================================== */

const char *term_lp_prev_str(term_lp_prev_t p)
{
    switch (p) {
    case TERM_LP_PREV_NONE:     return "none";
    case TERM_LP_PREV_CAPTURED: return "captured";
    case TERM_LP_PREV_EMPTY:    return "empty";
    case TERM_LP_PREV_NOMEM:    return "nomem";
    case TERM_LP_PREV_BAD:      return "bad";
    case TERM_LP_PREV_GARBAGE:  return "garbage";
    default:                    return "?";
    }
}

/* The four-character record tag the JSON dump prints, and the token
   tools/bb_pull.py splits a transcript on. One token per (class, screen)
   pair so a reader needs no second field:

     sys/  platform + system apps      (class A, SYS partition)
     app/  user app print()/term.log   (class A, APP partition)
     ses/  a RECORDED session line that scrolled off (class B, APP)
     scr/  a row of a RECORDED screen capture         (class B, APP)

   Anything unrecognised is "?"/ rather than a guess: an unknown class means
   the image was written by a decoder this build does not know, and saying
   "app" about it would be a lie. */
static const char *lp_class_tag(term_lp_class_t cls, unsigned flags)
{
    switch (cls) {
    case TERM_LP_CLASS_SYS:  return "sys/";
    case TERM_LP_CLASS_APP:  return "app/";
    case TERM_LP_CLASS_TERM: return (flags & TERM_LP_F_SCREEN) ? "scr/" : "ses/";
    default:                 return "?/";
    }
}

const char *term_lp_retention_str(term_lp_retention_t v)
{
    switch (v) {
    case TERM_LP_RET_OK:      return "ok";
    case TERM_LP_RET_COLD:    return "cold";
    case TERM_LP_RET_LOST:    return "lost";
    case TERM_LP_RET_CORRUPT: return "corrupt";
    default:                  return "unknown";
    }
}

/* esp_reset_reason_t by value: the numbers are the stable part of that enum
   (0..15 in IDF 6.0) and this table also compiles off-device. */
const char *term_lp_reason_str(unsigned r)
{
    switch (r) {
    case 0:  return "unknown";
    case 1:  return "poweron";
    case 2:  return "ext";
    case 3:  return "sw";
    case 4:  return "panic";
    case 5:  return "int_wdt";
    case 6:  return "task_wdt";
    case 7:  return "wdt";
    case 8:  return "deepsleep";
    case 9:  return "brownout";
    case 10: return "sdio";
    case 11: return "usb";
    case 12: return "jtag";
    case 13: return "efuse";
    case 14: return "pwr_glitch";
    case 15: return "cpu_lockup";
    default: return "?";
    }
}

/* A reset that should have preserved LP SRAM, per §1's device results. */
static bool lp_reason_retains(unsigned reason)
{
    switch (reason) {
    case 3:  /* sw      */
    case 4:  /* panic   */
    case 5:  /* int_wdt */
    case 6:  /* task_wdt */
    case 7:  /* wdt     */
    case 15: /* cpu_lockup */
        return true;
    default:
        /* poweron, ext, brownout, deepsleep, unknown and the peripheral
           causes: no expectation either way, so their emptiness is never
           reported as a failure. */
        return false;
    }
}

static term_lp_retention_t lp_retention(term_lp_prev_t prev, unsigned reason)
{
    switch (prev) {
    case TERM_LP_PREV_CAPTURED:
    case TERM_LP_PREV_EMPTY:
    case TERM_LP_PREV_NOMEM:
        return TERM_LP_RET_OK;
    case TERM_LP_PREV_BAD:
    case TERM_LP_PREV_GARBAGE:
        return TERM_LP_RET_CORRUPT;
    case TERM_LP_PREV_NONE:
    default:
        return lp_reason_retains(reason) ? TERM_LP_RET_LOST
                                         : TERM_LP_RET_COLD;
    }
}

/* ===================================================================== */
/* platform seam                                                         */
/* ===================================================================== */

#ifdef ESP_PLATFORM

/* The black box itself. The linker places .rtc_noinit in lp_ram_seg and
   ASSERTs the fit, so an over-large region fails the LINK rather than
   silently relocating (PHASE3_MANIFEST.md §1 has the linker-script trail). */
static RTC_NOINIT_ATTR __attribute__((aligned(16)))
    uint8_t s_region[TERM_LP_REGION_BYTES];

static uint32_t lp_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static unsigned lp_reset_reason(void)
{
    return (unsigned)esp_reset_reason();
}

/* §4.1: PSRAM, never the internal SRAM the size diet won back (§2.7). A
   failure is reported as TERM_LP_PREV_NOMEM, never fatal. */
static void *lp_snap_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static void lp_snap_free(void *p)
{
    heap_caps_free(p);
}

#define LP_RETAINED 1

#else /* !ESP_PLATFORM */

static __attribute__((aligned(16))) uint8_t s_region[TERM_LP_REGION_BYTES];

/* Deterministic and monotonic: host suites want reproducible timestamps and
   the ring only ever states "ms since boot". */
static uint32_t s_fake_ms;
static uint32_t lp_now_ms(void)   { return ++s_fake_ms; }
static unsigned lp_reset_reason(void) { return 0; }   /* "unknown" */
static void *lp_snap_alloc(size_t n)  { (void)n; return NULL; }
static void lp_snap_free(void *p)     { (void)p; }

#define LP_RETAINED 0

#endif /* ESP_PLATFORM */

/* ===================================================================== */
/* the singleton                                                         */
/* ===================================================================== */

static term_lp_ring_t s_live;
static term_lp_ring_t s_last;
static bool s_booted;
static bool s_have_last;
static term_lp_prev_t      s_prev;
static term_lp_retention_t s_retention = TERM_LP_RET_UNKNOWN;
static unsigned s_reason;

void term_lp_ring_boot(void)
{
    term_lp_check_t chk;
    term_lp_ring_t old;
    term_lp_prev_t prev;
    uint32_t prev_seq = 0, boot_seq = 1;
    char msg[144];
    bool ok;

    if (s_booted)
        return;
    s_booted = true;
    s_reason = lp_reset_reason();

    ok = term_lp_ring_open(&old, s_region, sizeof s_region, true, &chk);
    if (ok) {
        prev_seq = chk.boot_seq;
        boot_seq = chk.boot_seq + 1u;
        if (!chk.records[TERM_LP_PART_SYS] && !chk.records[TERM_LP_PART_APP]) {
            prev = TERM_LP_PREV_EMPTY;
        } else {
            /* Freeze: one flat copy of the region image, so the same
               iterator serves both sources (term_lp_ring.h, LASTBOOT). */
            void *snap = lp_snap_alloc(TERM_LP_REGION_BYTES);
            if (!snap) {
                prev = TERM_LP_PREV_NOMEM;
            } else {
                memcpy(snap, s_region, TERM_LP_REGION_BYTES);
                if (term_lp_ring_open(&s_last, snap, TERM_LP_REGION_BYTES,
                                      true, NULL)) {
                    s_have_last = true;
                    prev = TERM_LP_PREV_CAPTURED;
                } else {
                    lp_snap_free(snap);       /* unreachable: same bytes */
                    prev = TERM_LP_PREV_BAD;
                }
            }
        }
    } else {
        prev = chk.magic_ok ? TERM_LP_PREV_BAD
             : chk.zeroed   ? TERM_LP_PREV_NONE
                            : TERM_LP_PREV_GARBAGE;
    }
    s_prev = prev;
    s_retention = lp_retention(prev, s_reason);

    if (!term_lp_ring_format(&s_live, s_region, sizeof s_region, boot_seq,
                             (uint8_t)s_reason, prev, prev_seq)) {
        LPLOGW("black box unavailable: region %u B rejected",
               (unsigned)sizeof s_region);
        return;
    }

    /* The boot marker (§4.4). Written on EVERY boot: it is what lets a
       reader tell "wiped" from "empty", because it carries this boot's reset
       cause next to the verdict on what was found. */
    snprintf(msg, sizeof msg,
             "boot %u reset=%s(%u) prev=%s(%u) retention=%s region=%u",
             (unsigned)boot_seq, term_lp_reason_str(s_reason), s_reason,
             term_lp_prev_str(prev), (unsigned)prev_seq,
             term_lp_retention_str(s_retention),
             (unsigned)TERM_LP_REGION_BYTES);
    term_lp_ring_append(&s_live, TERM_LP_CLASS_SYS, TERM_LP_WRITER_SYSTEM,
                        msg, strlen(msg), lp_now_ms());
    LPLOGW("black box: %s%s", msg,
           s_retention == TERM_LP_RET_LOST
               ? "  <-- LP SRAM did NOT survive this reset" : "");
    if (s_have_last)
        LPLOGI("lastboot frozen: sys=%u app=%u records",
               (unsigned)chk.records[TERM_LP_PART_SYS],
               (unsigned)chk.records[TERM_LP_PART_APP]);
}

bool term_lp_ring_ready(void)
{
    return s_live.attached;
}

bool term_lp_log(term_lp_class_t cls, const char *writer,
                 const char *text, size_t len)
{
    /* Self-booting: the recorder must not depend on somebody having called
       boot first, because the lines worth keeping include the ones from
       before the rest of the system came up. On the device app_main's call
       has already run and this is a single load-and-branch. */
    term_lp_ring_boot();
    if (!s_live.attached)
        return false;
    return term_lp_ring_append(&s_live, cls, writer, text, len, lp_now_ms());
}

const term_lp_ring_t *term_lp_source(term_lp_src_t src)
{
    term_lp_ring_boot();
    if (src == TERM_LP_SRC_LASTBOOT)
        return s_have_last ? &s_last : NULL;
    return s_live.attached ? &s_live : NULL;
}

void term_lp_stats(term_lp_stats_t *out)
{
    int i;

    if (!out)
        return;
    term_lp_ring_boot();
    memset(out, 0, sizeof *out);
    out->ready = s_live.attached;
    out->retained = (LP_RETAINED != 0);
    out->region_bytes = TERM_LP_REGION_BYTES;
    out->reset_reason = (uint8_t)s_reason;
    out->prev = s_prev;
    out->retention = s_retention;
    out->lastboot = s_have_last;
    if (s_live.attached) {
        out->boot_seq = s_live.hdr.boot_seq;
        out->prev_boot_seq = s_live.hdr.prev_boot_seq;
        out->writers = s_live.hdr.writers;
        out->dropped_bytes = s_live.hdr.dropped_bytes;
        out->truncated = s_live.hdr.truncated;
        out->refused = s_live.hdr.refused;
        out->unnamed = s_live.hdr.unnamed;
        out->hseq = s_live.hdr.hseq;
        for (i = 0; i < TERM_LP_PART_COUNT; i++) {
            out->cap[i] = s_live.hdr.part[i].cap;
            out->used[i] = s_live.hdr.part[i].used;
            out->records[i] = s_live.hdr.part[i].records;
            out->appended[i] = s_live.hdr.part[i].appended;
            out->evicted[i] = s_live.hdr.part[i].evicted;
        }
    }
    if (s_have_last) {
        for (i = 0; i < TERM_LP_PART_COUNT; i++) {
            out->last_records[i] = s_last.hdr.part[i].records;
            out->last_used[i] = s_last.hdr.part[i].used;
        }
    }
}

/* ===================================================================== */
/* JSON builders                                                         */
/* ===================================================================== */

typedef struct {
    char  *p;
    size_t cap;      /* usable bytes excluding the NUL */
    size_t n;
    bool   ovf;
} lp_buf_t;

static void bput(lp_buf_t *b, const char *s, size_t len)
{
    if (b->ovf)
        return;
    if (b->n + len > b->cap) {
        b->ovf = true;
        return;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
}

static void bputs(lp_buf_t *b, const char *s)
{
    bput(b, s, strlen(s));
}

static void bputu(lp_buf_t *b, uint32_t v)
{
    char t[12];
    int k = snprintf(t, sizeof t, "%u", (unsigned)v);
    if (k > 0)
        bput(b, t, (size_t)k);
}

/* A JSON string body. Everything that survives term_lp_strip() is >= 0x20
   plus '\n'/'\t', so this only has to handle the two structural characters
   and those two controls. */
static void bputq(lp_buf_t *b, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            bput(b, "\\", 1);
            bput(b, &c, 1);
        } else if (c == '\n') {
            bput(b, "\\n", 2);
        } else if (c == '\t') {
            bput(b, "\\t", 2);
        } else if ((unsigned char)c < 0x20u) {
            bput(b, " ", 1);                 /* cannot occur; stay safe */
        } else {
            bput(b, &c, 1);
        }
    }
}

static void bpart(lp_buf_t *b, const char *name, const term_lp_stats_t *st,
                  int i)
{
    bputs(b, "\"");
    bputs(b, name);
    bputs(b, "\":{\"cap\":");
    bputu(b, st->cap[i]);
    bputs(b, ",\"used\":");
    bputu(b, st->used[i]);
    bputs(b, ",\"rec\":");
    bputu(b, st->records[i]);
    bputs(b, ",\"app\":");
    bputu(b, st->appended[i]);
    bputs(b, ",\"evic\":");
    bputu(b, st->evicted[i]);
    bputs(b, "}");
}

size_t term_lp_report(char *out, size_t out_size)
{
    term_lp_stats_t st;
    lp_buf_t b;

    if (!out || out_size < 32u)
        return 0;
    b.p = out;
    b.cap = out_size - 1u;
    b.n = 0;
    b.ovf = false;
    term_lp_stats(&st);

    bputs(&b, "{\"ready\":");
    bputu(&b, st.ready ? 1u : 0u);
    bputs(&b, ",\"retained\":");
    bputu(&b, st.retained ? 1u : 0u);
    bputs(&b, ",\"region\":");
    bputu(&b, st.region_bytes);
    bputs(&b, ",\"rec_max\":");
    bputu(&b, TERM_LP_REC_MAX);
    bputs(&b, ",\"boot_seq\":");
    bputu(&b, st.boot_seq);
    bputs(&b, ",\"reset\":\"");
    bputs(&b, term_lp_reason_str(st.reset_reason));
    bputs(&b, "\",\"reset_id\":");
    bputu(&b, st.reset_reason);
    bputs(&b, ",\"retention\":\"");
    bputs(&b, term_lp_retention_str(st.retention));
    bputs(&b, "\",\"prev\":\"");
    bputs(&b, term_lp_prev_str(st.prev));
    bputs(&b, "\",\"prev_boot_seq\":");
    bputu(&b, st.prev_boot_seq);
    bputs(&b, ",\"lastboot\":");
    bputu(&b, st.lastboot ? 1u : 0u);
    bputs(&b, ",\"writers\":");
    bputu(&b, st.writers);
    bputs(&b, ",\"live\":{");
    bpart(&b, "sys", &st, TERM_LP_PART_SYS);
    bputs(&b, ",");
    bpart(&b, "app", &st, TERM_LP_PART_APP);
    bputs(&b, "},\"last\":{\"sys\":{\"rec\":");
    bputu(&b, st.last_records[TERM_LP_PART_SYS]);
    bputs(&b, ",\"used\":");
    bputu(&b, st.last_used[TERM_LP_PART_SYS]);
    bputs(&b, "},\"app\":{\"rec\":");
    bputu(&b, st.last_records[TERM_LP_PART_APP]);
    bputs(&b, ",\"used\":");
    bputu(&b, st.last_used[TERM_LP_PART_APP]);
    bputs(&b, "}},\"dropped\":");
    bputu(&b, st.dropped_bytes);
    bputs(&b, ",\"trunc\":");
    bputu(&b, st.truncated);
    bputs(&b, ",\"refused\":");
    bputu(&b, st.refused);
    bputs(&b, ",\"unnamed\":");
    bputu(&b, st.unnamed);
    bputs(&b, ",\"hseq\":");
    bputu(&b, st.hseq);
    bputs(&b, "}");

    if (b.ovf) {
        /* Never half a document (the §1 precedent). */
        int k = snprintf(out, out_size, "{\"ready\":%d,\"error\":\"truncated\"}",
                         st.ready ? 1 : 0);
        return (k > 0 && (size_t)k < out_size) ? (size_t)k : 0;
    }
    out[b.n] = '\0';
    return b.n;
}

/* Room kept for `],"records":N,"next":N,"more":1}` plus slack. */
#define LP_DUMP_TAIL 64u

size_t term_lp_dump_json(term_lp_src_t src, uint32_t from,
                         char *out, size_t out_size)
{
    return term_lp_dump_json_ex(src, from, out, out_size, NULL);
}

size_t term_lp_dump_json_ex(term_lp_src_t src, uint32_t from,
                            char *out, size_t out_size,
                            term_lp_dump_info_t *info)
{
    const term_lp_ring_t *r = term_lp_source(src);
    const char *sname = (src == TERM_LP_SRC_LASTBOOT) ? "lastboot" : "live";
    term_lp_iter_t it;
    term_lp_record_t rec;
    lp_buf_t b;
    uint32_t skipped = 0, emitted = 0;
    bool more = false, first = true;

    if (info)
        memset(info, 0, sizeof *info);
    if (!out || out_size < 96u)
        return 0;
    b.p = out;
    b.cap = out_size - 1u - LP_DUMP_TAIL;
    b.n = 0;
    b.ovf = false;

    bputs(&b, "{\"src\":\"");
    bputs(&b, sname);
    bputs(&b, "\",\"ok\":");
    bputu(&b, r ? 1u : 0u);
    bputs(&b, ",\"from\":");
    bputu(&b, from);
    bputs(&b, ",\"lines\":[");

    if (r) {
        term_lp_iter_begin(&it, r, -1);
        while (term_lp_iter_next(&it, &rec)) {
            size_t mark;
            if (skipped < from) {
                skipped++;
                continue;
            }
            mark = b.n;
            if (!first)
                bputs(&b, ",");
            bputs(&b, "\"[");
            bputu(&b, rec.t_ms);
            bputs(&b, "] ");
            /* The class tag. rec.cls is already masked by the iterator, so
               this is a class comparison and not a whole-byte one — the
               phase-3 spelling (`rec.cls == TERM_LP_CLASS_SYS`) would have
               mislabelled a TRUNCated SYS record as "app/" the day a
               platform line went over 512 bytes. */
            bputs(&b, lp_class_tag(rec.cls, rec.flags));
            bputq(&b, rec.writer, strlen(rec.writer));
            bputs(&b, ": ");
            if (rec.flags & TERM_LP_F_TRUNC)
                bputs(&b, "~");
            bputq(&b, (const char *)rec.text, rec.len);
            bputs(&b, "\"");
            if (b.ovf) {
                /* Planned stop, not an error: rewind the partial line and
                   tell the caller to come back with `next`. */
                b.n = mark;
                b.ovf = false;
                more = true;
                break;
            }
            first = false;
            emitted++;
        }
    }

    b.cap = out_size - 1u;                    /* release the reserve */
    bputs(&b, "],\"records\":");
    bputu(&b, emitted);
    bputs(&b, ",\"next\":");
    bputu(&b, from + emitted);
    bputs(&b, ",\"more\":");
    bputu(&b, more ? 1u : 0u);
    bputs(&b, "}");
    if (b.ovf)
        return 0;
    out[b.n] = '\0';
    if (info) {
        info->ok = (r != NULL);
        info->records = emitted;
        info->next = from + emitted;
        info->more = more;
    }
    return b.n;
}

/* ===================================================================== */
/* The panic note                                                        */
/* ===================================================================== */

static void bputhex(lp_buf_t *b, uint32_t v)
{
    static const char hx[] = "0123456789abcdef";
    char t[8];
    int i;
    for (i = 7; i >= 0; i--) {
        t[i] = hx[v & 0xFu];
        v >>= 4;
    }
    bput(b, t, 8);
}

/* One field, clipped. A panic string arrives from IDF or FreeRTOS, so it is
   not hostile — but it can be long, and the numbers after it are the part a
   human cannot reconstruct from the serial log. */
static void bputclip(lp_buf_t *b, const char *s, size_t max)
{
    size_t n = 0;
    if (!s)
        return;
    while (s[n] && n < max)
        n++;
    bput(b, s, n);
}

size_t term_lp_panic_fmt(char *out, size_t cap, const term_lp_panic_t *p)
{
    lp_buf_t b;

    if (!out || !p || cap < 32u)
        return 0;
    b.p = out;
    b.cap = (cap < TERM_LP_PANIC_MAX ? cap : TERM_LP_PANIC_MAX) - 1u;
    b.n = 0;
    b.ovf = false;

    bputs(&b, "panic: ");
    bputclip(&b, p->kind ? p->kind : "unknown", 12);
    if (p->reason && p->reason[0]) {
        bput(&b, " ", 1);
        bputclip(&b, p->reason, 48);
    }
    bputs(&b, " task=");
    bputclip(&b, (p->task && p->task[0]) ? p->task : "?", 20);
    bputs(&b, " core=");
    bputu(&b, (uint32_t)(p->core < 0 ? 0 : p->core));
    bputs(&b, " pc=0x");
    bputhex(&b, p->pc);
    bputs(&b, " cause=");
    bputu(&b, p->cause);
    /* b.ovf here means `cap` was tiny (the fields are bounded above). bput() is
       all-or-nothing, so b.n is exactly how many bytes were written: terminate
       there and return that. Terminating at b.cap instead would hand the caller
       a length covering bytes this function never wrote — its own uninitialised
       buffer — and term_lp_ring_append() would then publish them. */
    out[b.n] = '\0';
    return b.n;
}

/* One-shot: set on the first call and never cleared. After a panic there is
   no "later" to reset it for, and a panic inside the panic handler must not
   walk this code again. */
static bool s_panic_noted;

bool term_lp_panic_note(const term_lp_panic_t *p)
{
    char line[TERM_LP_PANIC_MAX];
    size_t n;

    if (s_panic_noted || !p)
        return false;
    s_panic_noted = true;
    if (!s_live.attached || s_live.read_only)
        return false;
    /* The interrupted writer may be stalled mid-append on another core, so
       the shadow header is not known-good here the way it is everywhere else
       in this file. Refuse rather than publish an inconsistent one — see
       term_lp_ring.h on this function. */
    if (!lp_geom_ok(&s_live.hdr))
        return false;
    n = term_lp_panic_fmt(line, sizeof line, p);
    if (!n)
        return false;
    return term_lp_ring_append(&s_live, TERM_LP_CLASS_SYS, "panic", line, n,
                               lp_now_ms());
}

bool term_lp_panic_ready(void)
{
    return s_live.attached && !s_live.read_only && lp_geom_ok(&s_live.hdr);
}

bool term_lp_panic_append(term_lp_class_t cls, const char *writer,
                          const char *text, size_t len)
{
    /* Revalidated on EVERY call, not once for the loop: the shadow header is
       ours between calls, but the state we are refusing to trust is the one
       the stalled core left behind, and one successful append does not prove
       the next one starts from a consistent header (a first append that hit
       eviction rewrites tail/used itself). Cheap — 128 bytes of DRAM. */
    if (!term_lp_panic_ready())
        return false;
    return term_lp_ring_append(&s_live, cls, writer, text, len, lp_now_ms());
}
