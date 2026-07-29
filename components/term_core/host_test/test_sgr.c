/*
 * test_sgr.c — SGR 16 / 256 / truecolour, reverse, bold, underline.
 *
 * Design §6 row 4: "SGR 16/256/truecolor … パレットはここに一本化。格納は
 * RGB565 直値なので truecolor もパネル能力の範囲で無損失", §4.1 "色は 256
 * パレット index ではなく RGB565 直値で持つ", §6 row "reverse / bold — fg/bg
 * スワップ / 明色パレットマップ(blitter 変更なし)".
 *
 * No absolute colour constants are invented here: expectations are written
 * against term_pal16[] / term_pal16_at() / TERM_RGB565() / term_color_xterm256()
 * from the header, plus structural invariants (a grey is a grey in RGB565).
 */
#include "test_util.h"

static const term_cell_t *write_one(term_core_t *c, const char *sgr, char ch)
{
    char b[128];
    feed(c, "\x1b[H\x1b[2J\x1b[m");
    sprintf(b, "%s%c", sgr, ch);
    feed(c, b);
    return cell_at(c, 0, 0);
}

static void case_basic_16(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    int i;

    t_case("SGR 30-37 select the normal palette foreground");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 8; i++) {
        char sgr[16];
        const term_cell_t *cell;
        sprintf(sgr, "\x1b[%dm", 30 + i);
        cell = write_one(t.c, sgr, 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, term_pal16_at(i));
        CHK_HEX(cell->bg, TERM_COLOR_BG_DEFAULT);
    }

    t_case("SGR 40-47 select the normal palette background");
    for (i = 0; i < 8; i++) {
        char sgr[16];
        const term_cell_t *cell;
        sprintf(sgr, "\x1b[%dm", 40 + i);
        cell = write_one(t.c, sgr, 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->bg, term_pal16_at(i));
        CHK_HEX(cell->fg, TERM_COLOR_FG_DEFAULT);
    }

    t_case("SGR 90-97 / 100-107 select the bright half of the palette");
    for (i = 0; i < 8; i++) {
        char sgr[16];
        const term_cell_t *cell;
        sprintf(sgr, "\x1b[%dm", 90 + i);
        cell = write_one(t.c, sgr, 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, term_pal16_at(8 + i));
        sprintf(sgr, "\x1b[%dm", 100 + i);
        cell = write_one(t.c, sgr, 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->bg, term_pal16_at(8 + i));
    }

    t_case("SGR 39 / 49 return to the default colours");
    {
        const term_cell_t *cell = write_one(t.c, "\x1b[31;44m\x1b[39;49m", 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, TERM_COLOR_FG_DEFAULT);
        CHK_HEX(cell->bg, TERM_COLOR_BG_DEFAULT);
    }

    t_case("SGR 0 and a bare SGR both reset everything");
    {
        const term_cell_t *cell = write_one(t.c, "\x1b[1;4;7;31;44m\x1b[0m", 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, TERM_COLOR_FG_DEFAULT);
        CHK_HEX(cell->bg, TERM_COLOR_BG_DEFAULT);
        CHK_HEX(cell->flags, 0);
        cell = write_one(t.c, "\x1b[1;4;7;31;44m\x1b[m", 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, TERM_COLOR_FG_DEFAULT);
        CHK_HEX(cell->bg, TERM_COLOR_BG_DEFAULT);
        CHK_HEX(cell->flags, 0);
    }

    t_case("several parameters in one SGR are applied in order");
    {
        const term_cell_t *cell = write_one(t.c, "\x1b[0;1;31;44m", 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, term_pal16_at(1 | 8));
        CHK_HEX(cell->bg, term_pal16_at(4));
        CHK_TRUE((cell->flags & TERM_CELL_BOLD) != 0);
    }

    t_case("attributes apply to cells written after them, not before");
    {
        const term_cell_t *row;
        feed(t.c, "\x1b[H\x1b[2J\x1b[m" "a\x1b[31m" "b");
        row = term_core_row(t.c, 0);
        REQUIRE(row != NULL);
        CHK_HEX(row[0].fg, TERM_COLOR_FG_DEFAULT);
        CHK_HEX(row[1].fg, term_pal16_at(1));
    }
    tc_free(&t);
}

static void case_bold_reverse(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    const term_cell_t *cell;

    t_case("bold folds the foreground to its bright twin (index | 8) at ingest");
    REQUIRE(t.c != NULL);
    cell = write_one(t.c, "\x1b[1;31m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, term_pal16_at(1 | 8));
    CHK_TRUE((cell->flags & TERM_CELL_BOLD) != 0);

    t_case("bold before or after the colour makes no difference "
           "(the fold happens when the cell is written)");
    cell = write_one(t.c, "\x1b[31m\x1b[1m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, term_pal16_at(1 | 8));

    t_case("reverse swaps the final fg/bg (blitter needs no attribute logic)");
    cell = write_one(t.c, "\x1b[31;44;7m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, term_pal16_at(4));
    CHK_HEX(cell->bg, term_pal16_at(1));
    CHK_TRUE((cell->flags & TERM_CELL_REVERSE) != 0);

    t_case("reverse on the default colours swaps the defaults");
    cell = write_one(t.c, "\x1b[7m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, TERM_COLOR_BG_DEFAULT);
    CHK_HEX(cell->bg, TERM_COLOR_FG_DEFAULT);

    t_case("bold + reverse: the bright fold happens before the swap");
    cell = write_one(t.c, "\x1b[1;31;44;7m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, term_pal16_at(4));
    CHK_HEX(cell->bg, term_pal16_at(1 | 8));
    tc_free(&t);
}

static void case_flag_attributes(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    const term_cell_t *cell;

    t_case("SGR 2/3/4/9 land in the cell flags (header TERM_CELL_* comments)");
    REQUIRE(t.c != NULL);
    cell = write_one(t.c, "\x1b[2m", 'x');
    REQUIRE(cell != NULL);
    CHK_TRUE((cell->flags & TERM_CELL_DIM) != 0);
    cell = write_one(t.c, "\x1b[3m", 'x');
    REQUIRE(cell != NULL);
    CHK_TRUE((cell->flags & TERM_CELL_ITALIC) != 0);
    cell = write_one(t.c, "\x1b[4m", 'x');
    REQUIRE(cell != NULL);
    CHK_TRUE((cell->flags & TERM_CELL_UNDERLINE) != 0);
    cell = write_one(t.c, "\x1b[9m", 'x');
    REQUIRE(cell != NULL);
    CHK_TRUE((cell->flags & TERM_CELL_STRIKE) != 0);

    t_case("underline does not disturb the colours (§6: blitter draws the rule)");
    cell = write_one(t.c, "\x1b[4;31m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, term_pal16_at(1));
    CHK_HEX(cell->bg, TERM_COLOR_BG_DEFAULT);
    CHK_TRUE((cell->flags & TERM_CELL_UNDERLINE) != 0);
    tc_free(&t);
}

static void case_xterm256_fn(void)
{
    int i;
    uint16_t prev;

    t_case("term_color_xterm256(): 0-15 are the unified 16-colour palette");
    for (i = 0; i < 16; i++) CHK_HEX(term_color_xterm256(i), term_pal16_at(i));

    t_case("term_color_xterm256(): the 6x6x6 cube corners are black and white");
    CHK_HEX(term_color_xterm256(16), TERM_RGB565(0, 0, 0));
    CHK_HEX(term_color_xterm256(231), TERM_RGB565(255, 255, 255));

    t_case("term_color_xterm256(): the cube diagonal is grey in RGB565");
    for (i = 0; i < 6; i++) {
        uint16_t c = term_color_xterm256(16 + 43 * i);   /* r == g == b level */
        unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
        CHK_INT(r, b);
        CHK_INT(g >> 1, r);
    }

    t_case("term_color_xterm256(): 232-255 is a monotonic grey ramp");
    prev = 0;
    for (i = 232; i <= 255; i++) {
        uint16_t c = term_color_xterm256(i);
        unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
        CHK_INT(r, b);
        CHK_INT(g >> 1, r);
        CHK_TRUE(c >= prev);
        prev = c;
    }
    CHK_TRUE(term_color_xterm256(255) > term_color_xterm256(232));

    t_case("term_color_xterm256(): out of range answers the default foreground");
    CHK_HEX(term_color_xterm256(-1), TERM_COLOR_FG_DEFAULT);
    CHK_HEX(term_color_xterm256(256), TERM_COLOR_FG_DEFAULT);
    CHK_HEX(term_color_xterm256(1000000), TERM_COLOR_FG_DEFAULT);
}

static void case_sgr_256_and_truecolor(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    const term_cell_t *cell;
    char sgr[64];
    int idx[6];
    int i;

    t_case("SGR 38;5;n / 48;5;n agree with term_color_xterm256()");
    REQUIRE(t.c != NULL);
    idx[0] = 0; idx[1] = 9; idx[2] = 16; idx[3] = 123; idx[4] = 231; idx[5] = 244;
    for (i = 0; i < 6; i++) {
        sprintf(sgr, "\x1b[38;5;%dm", idx[i]);
        cell = write_one(t.c, sgr, 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->fg, term_color_xterm256(idx[i]));
        sprintf(sgr, "\x1b[48;5;%dm", idx[i]);
        cell = write_one(t.c, sgr, 'x');
        REQUIRE(cell != NULL);
        CHK_HEX(cell->bg, term_color_xterm256(idx[i]));
    }

    t_case("SGR 38;2;r;g;b is stored as RGB565 directly (§4.1, no quantisation "
           "back down to the 256 palette)");
    cell = write_one(t.c, "\x1b[38;2;255;0;0m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, TERM_RGB565(255, 0, 0));
    cell = write_one(t.c, "\x1b[38;2;18;52;86m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, TERM_RGB565(18, 52, 86));
    cell = write_one(t.c, "\x1b[48;2;0;255;0m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->bg, TERM_RGB565(0, 255, 0));
    cell = write_one(t.c, "\x1b[38;2;255;255;255;48;2;0;0;0m", 'x');
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, TERM_RGB565(255, 255, 255));
    CHK_HEX(cell->bg, TERM_RGB565(0, 0, 0));

    t_case("truecolour distinguishes shades the 16-colour palette cannot");
    {
        uint16_t a, b;
        cell = write_one(t.c, "\x1b[38;2;100;100;100m", 'x');
        REQUIRE(cell != NULL);
        a = cell->fg;
        cell = write_one(t.c, "\x1b[38;2;110;110;110m", 'x');
        REQUIRE(cell != NULL);
        b = cell->fg;
        CHK_TRUE(a != b);
    }

    t_case("a malformed 38 sub-sequence does not corrupt the stream");
    feed(t.c, "\x1b[H\x1b[2J\x1b[m\x1b[38m" "z");
    CHK_STR(row_text(t.c, 0), "z");
    feed(t.c, "\x1b[H\x1b[2J\x1b[m\x1b[38;5m" "z");
    CHK_STR(row_text(t.c, 0), "z");
    feed(t.c, "\x1b[H\x1b[2J\x1b[m\x1b[38;2;1;2m" "z");
    CHK_STR(row_text(t.c, 0), "z");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("sgr");
    case_basic_16();
    case_bold_reverse();
    case_flag_attributes();
    case_xterm256_fn();
    case_sgr_256_and_truecolor();
    return t_summary();
}
