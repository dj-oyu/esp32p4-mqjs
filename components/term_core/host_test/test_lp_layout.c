/*
 * test_lp_layout.c — the on-memory format, which the header says IS the
 * contract:
 *
 *   offset   size    contents
 *   0        128     term_lp_hdr_t, slot A
 *   128      128     term_lp_hdr_t, slot B          (P5: double buffer)
 *   256      256     writer table: 8 x 32-byte NUL-padded names
 *   512      8192    SYS partition data
 *   8704     23552   APP partition data
 *   32256            TERM_LP_REGION_BYTES
 *
 *   "A partition is a circular buffer of records. One record is an 8-byte
 *    term_lp_rec_t followed by `len` payload bytes, the whole thing rounded up
 *    to TERM_LP_REC_ALIGN. Records never straddle the end of a partition: a
 *    TERM_LP_CLASS_PAD record fills the remainder and the next record starts
 *    at offset 0. The reader therefore never has to reassemble a split record,
 *    and the writer never issues an unaligned or wrapped burst."
 *
 * So this suite walks the raw region after appends and asserts the structure
 * by arithmetic, with payload sizes chosen to hit the interesting remainders
 * exactly: a 384-byte PAD, and the smallest PAD the format allows (a header
 * with len 0 in the last 8 bytes of the partition).
 */
#include "lp_util.h"

/* Compile-time half of the layout contract: the sizes the header asserts. */
typedef char lp_assert_hdr_128[sizeof(term_lp_hdr_t) == 128 ? 1 : -1];
typedef char lp_assert_rec_8[sizeof(term_lp_rec_t) == 8 ? 1 : -1];
typedef char lp_assert_part_32[sizeof(term_lp_part_t) == 32 ? 1 : -1];
typedef char lp_assert_region[TERM_LP_REGION_BYTES == 32256 ? 1 : -1];

static lp_recs_t bagA;

/* Read one raw record header out of a partition. */
static term_lp_rec_t raw_rec(const lpreg_t *g, int part, uint32_t off)
{
    term_lp_rec_t rec;
    memcpy(&rec, g->base + lp_part_off(part) + off, sizeof rec);
    return rec;
}

static uint32_t rec_size(uint16_t len)
{
    return TERM_LP_REC_HDR + (((uint32_t)len + 7u) & ~7u);
}

/* ===================================================================== */
/* Constants and struct shapes                                           */
/* ===================================================================== */

static void case_layout_constants(void)
{
    t_case("the structures are the sizes the header documents");
    CHK_INT(sizeof(term_lp_hdr_t), TERM_LP_HDR_BYTES);
    CHK_INT(sizeof(term_lp_hdr_t), 128);
    CHK_INT(sizeof(term_lp_rec_t), TERM_LP_REC_HDR);
    CHK_INT(sizeof(term_lp_rec_t), 8);
    CHK_INT(sizeof(term_lp_part_t), 32);
    CHK_INT(offsetof(term_lp_hdr_t, crc), 124);
    CHK_INT(sizeof(term_lp_hdr_t) - offsetof(term_lp_hdr_t, crc), 4);

    t_case("the offsets add up to the documented region map");
    CHK_INT(TERM_LP_WT_OFF, 256);
    CHK_INT(TERM_LP_WT_BYTES, 256);
    CHK_INT(TERM_LP_SYS_OFF, 512);
    CHK_INT(TERM_LP_APP_OFF, 8704);
    CHK_INT(TERM_LP_REGION_BYTES, 32256);
    CHK_INT(TERM_LP_SYS_BYTES + TERM_LP_APP_BYTES, 31744);
    CHK_INT(TERM_LP_HDR_BYTES * TERM_LP_HDR_SLOTS, TERM_LP_WT_OFF);
    CHK_INT(TERM_LP_WT_OFF + TERM_LP_WT_BYTES, TERM_LP_SYS_OFF);
    CHK_INT(TERM_LP_SYS_OFF + TERM_LP_SYS_BYTES, TERM_LP_APP_OFF);
    CHK_INT(TERM_LP_APP_OFF + TERM_LP_APP_BYTES, TERM_LP_REGION_BYTES);
    CHK_INT(TERM_LP_WRITERS * TERM_LP_WRITER_MAX, TERM_LP_WT_BYTES);

    t_case("everything a write can land on is a multiple of REC_ALIGN");
    CHK_INT(TERM_LP_REC_ALIGN, 8);
    CHK_INT(TERM_LP_SYS_BYTES % TERM_LP_REC_ALIGN, 0);
    CHK_INT(TERM_LP_APP_BYTES % TERM_LP_REC_ALIGN, 0);
    CHK_INT(TERM_LP_SYS_OFF % TERM_LP_REC_ALIGN, 0);
    CHK_INT(TERM_LP_APP_OFF % TERM_LP_REC_ALIGN, 0);
    CHK_INT(TERM_LP_REC_HDR % TERM_LP_REC_ALIGN, 0);
    CHK_INT(TERM_LP_REC_MAX % TERM_LP_REC_ALIGN, 0);
    CHK_INT(TERM_LP_REC_MAX, 512);
    CHK_INT(TERM_LP_HDR_BYTES % TERM_LP_REC_ALIGN, 0);

    t_case("the magic is \"TLR1\", most significant byte first");
    CHK_HEX(TERM_LP_MAGIC, 0x544C5231u);
    CHK_INT((TERM_LP_MAGIC >> 24) & 0xFFu, 'T');
    CHK_INT((TERM_LP_MAGIC >> 16) & 0xFFu, 'L');
    CHK_INT((TERM_LP_MAGIC >> 8) & 0xFFu, 'R');
    CHK_INT(TERM_LP_MAGIC & 0xFFu, '1');
    CHK_INT(TERM_LP_WRITER_UNKNOWN, 0xFF);
    CHK_INT(TERM_LP_HDR_SLOTS, 2);
    CHK_INT(TERM_LP_PART_COUNT, 2);

    t_case("the class/flag macros split term_lp_rec_t::cls as documented");
    CHK_INT(TERM_LP_CLASS_OF(TERM_LP_CLASS_APP | TERM_LP_F_TRUNC), TERM_LP_CLASS_APP);
    CHK_INT(TERM_LP_CLASS_OF(TERM_LP_CLASS_SYS | TERM_LP_F_TRUNC), TERM_LP_CLASS_SYS);
    CHK_INT(TERM_LP_CLASS_OF(TERM_LP_CLASS_PAD), TERM_LP_CLASS_PAD);
    CHK_INT(TERM_LP_FLAGS_OF(TERM_LP_CLASS_SYS | TERM_LP_F_TRUNC), TERM_LP_F_TRUNC);
    CHK_INT(TERM_LP_FLAGS_OF(TERM_LP_CLASS_SYS), 0);
    CHK_TRUE(TERM_LP_CLASS_PAD <= 0x0F);
    CHK_INT(TERM_LP_F_TRUNC & 0x0Fu, 0);
    CHK_INT(sizeof(term_lp_ring_t) < 512u, 1);   /* "small enough for a stack" */
}

/* ===================================================================== */
/* Records are 8 + align8(len), laid down back to back                   */
/* ===================================================================== */

static void case_records_are_aligned_and_sequential(void)
{
    lpreg_t g;
    uint32_t off = 0;
    unsigned len;

    t_case("every payload length 1..80 lands 8-aligned, sized 8 + align8(len)");
    REQUIRE(lp_fresh(&g, 1));
    for (len = 1; len <= 80u; len++) {
        char buf[128];
        term_lp_rec_t rec;
        uint32_t head_before = g.r.hdr.part[TERM_LP_PART_SYS].head;
        memset(buf, 'a' + (int)(len % 26u), len);
        CHK_INT(head_before, off);
        CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", buf, len,
                                     len));
        rec = raw_rec(&g, TERM_LP_PART_SYS, off);
        CHK_INT(rec.len, len);
        CHK_INT(TERM_LP_CLASS_OF(rec.cls), TERM_LP_CLASS_SYS);
        CHK_INT(TERM_LP_FLAGS_OF(rec.cls), 0);
        CHK_INT(rec.t_ms, len);
        CHK_INT(off % TERM_LP_REC_ALIGN, 0);
        off += rec_size((uint16_t)len);
        CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].head, off);
        CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].used, off);
    }
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 80);
    lp_check_layout(g.base, &g.r.hdr);

    t_case("the padding between records is not readable as content");
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 80);
    for (len = 1; len <= 80u; len++) CHK_INT(bagA.len[len - 1u], len);

    t_case("nothing below TERM_LP_SYS_OFF moved except the header slots");
    {
        unsigned i, dirty = 0;
        /* The writer table holds exactly one interned name; the rest of it, and
         * every unused entry, is still zero. */
        CHK_INT(g.r.hdr.writers, 1);
        for (i = TERM_LP_WT_OFF + TERM_LP_WRITER_MAX; i < TERM_LP_SYS_OFF; i++)
            if (g.base[i]) dirty++;
        CHK_INT(dirty, 0);
    }
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* The PAD record                                                        */
/* ===================================================================== */

static void case_pad_fills_the_remainder_exactly(void)
{
    lpreg_t g;
    term_lp_rec_t pad, first;
    unsigned i;

    /*
     * 512-byte payloads make 520-byte records, so 15 of them fill 7800 of the
     * SYS partition's 8192 and leave 392 — not enough for a 16th. The format
     * says the remainder becomes one PAD record: header at 7800, len
     * 392 - 8 = 384, and the next record starts at offset 0.
     */
    t_case("15 x 520 bytes leaves 392: the PAD is at 7800 with len 384");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 15u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, TERM_LP_REC_MAX,
                        i));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].head, 7800);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].used, 7800);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 15);

    CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", 15, TERM_LP_REC_MAX, 15));
    pad = raw_rec(&g, TERM_LP_PART_SYS, 7800);
    CHK_INT(TERM_LP_CLASS_OF(pad.cls), TERM_LP_CLASS_PAD);
    CHK_INT(pad.len, 384);
    CHK_INT(TERM_LP_FLAGS_OF(pad.cls), 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].head, 520);
    first = raw_rec(&g, TERM_LP_PART_SYS, 0);
    CHK_INT(TERM_LP_CLASS_OF(first.cls), TERM_LP_CLASS_SYS);
    CHK_INT(first.len, TERM_LP_REC_MAX);
    CHK_INT(first.t_ms, 15);

    t_case("the PAD is counted in `used` and not in `records`");
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records +
            g.r.hdr.part[TERM_LP_PART_SYS].evicted, 16);
    {
        unsigned content = 0, pads = 0;
        char why[256];
        CHK_INT(lp_walk(g.base, &g.r.hdr, TERM_LP_PART_SYS, &content, &pads,
                        why, sizeof why), 0);
        if (why[0]) printf("     %s\n", why);
        CHK_INT(pads, 1);
        CHK_INT(content, g.r.hdr.part[TERM_LP_PART_SYS].records);
    }

    t_case("the iterator never hands a PAD to the caller");
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, g.r.hdr.part[TERM_LP_PART_SYS].records);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);
        CHK_INT(bagA.len[i], TERM_LP_REC_MAX);
    }
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

static void case_smallest_possible_pad_is_a_bare_header(void)
{
    lpreg_t g;
    term_lp_rec_t pad;
    unsigned i;

    /*
     * "every record header, payload and partition size is a multiple of this,
     * so ... the remainder before the end of a partition can always hold a PAD
     * header." The tightest case is a remainder of exactly REC_HDR bytes: one
     * 24-byte record (payload 16) plus 510 16-byte records (payload 8) put the
     * head at 8184, eight bytes short of the partition end.
     */
    t_case("a remainder of exactly 8 bytes becomes a PAD with len 0");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system",
                                 "0123456789abcdef", 16, 0));
    for (i = 0; i < 510u; i++)
        CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system",
                                     "12345678", 8, 1u + i));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].head, TERM_LP_SYS_BYTES - 8u);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 511);

    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", "wrap", 4,
                                 999));
    pad = raw_rec(&g, TERM_LP_PART_SYS, TERM_LP_SYS_BYTES - 8u);
    CHK_INT(TERM_LP_CLASS_OF(pad.cls), TERM_LP_CLASS_PAD);
    CHK_INT(pad.len, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].head, 16);
    lp_check_layout(g.base, &g.r.hdr);

    t_case("...and the wrapped record reads back whole");
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_TRUE(bagA.n > 0);
    CHK_INT(bagA.len[bagA.n - 1u], 4);
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, bagA.n - 1u), "wrap");
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* The payload cap, at the boundary                                      */
/* ===================================================================== */

static void case_payload_cap_at_the_boundary(void)
{
    lpreg_t g;
    char big[TERM_LP_REC_MAX + 8u];

    t_case("exactly TERM_LP_REC_MAX bytes is stored whole, without F_TRUNC");
    REQUIRE(lp_fresh(&g, 1));
    memset(big, 'x', sizeof big);
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "app", big,
                                 TERM_LP_REC_MAX, 1));
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, 1);
    CHK_INT(bagA.len[0], TERM_LP_REC_MAX);
    CHK_INT(bagA.flags[0], 0);
    CHK_INT(g.r.hdr.truncated, 0);

    t_case("one byte more is cut to the cap and flagged");
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "app", big,
                                 TERM_LP_REC_MAX + 1u, 2));
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, 2);
    CHK_INT(bagA.len[1], TERM_LP_REC_MAX);
    CHK_INT(bagA.flags[1], TERM_LP_F_TRUNC);
    CHK_INT(g.r.hdr.truncated, 1);

    t_case("one byte less is stored as-is");
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "app", big,
                                 TERM_LP_REC_MAX - 1u, 3));
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, 3);
    CHK_INT(bagA.len[2], TERM_LP_REC_MAX - 1u);
    CHK_INT(bagA.flags[2], 0);
    CHK_INT(g.r.hdr.truncated, 1);

    t_case("no record in the region ever exceeds the cap (raw walk)");
    {
        unsigned i;
        for (i = 0; i < 400u; i++)
            CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "app", big,
                                         sizeof big, 100u + i));
        CHK_INT(g.r.hdr.truncated, 401);
        lp_check_layout(g.base, &g.r.hdr);
        lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
        for (i = 0; i < bagA.n; i++) {
            CHK_INT(bagA.len[i], TERM_LP_REC_MAX);
            CHK_INT(bagA.flags[i], TERM_LP_F_TRUNC);
        }
    }
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

int main(void)
{
    t_suite("lp_layout");
    case_layout_constants();
    case_records_are_aligned_and_sequential();
    case_pad_fills_the_remainder_exactly();
    case_smallest_possible_pad_is_a_bare_header();
    case_payload_cap_at_the_boundary();
    return t_summary();
}
