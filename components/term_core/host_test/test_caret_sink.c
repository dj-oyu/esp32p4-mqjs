/*
 * test_caret_sink.c — the caret push of §10.2 (phase 4).
 *
 * Contracts under test (term_registry.h, "The caret sink (§10.2)"):
 *   "ONE sink for the whole registry, installed by the platform at bring-up,
 *    not per term and not by an app … term_registry_deinit() clears it".
 *   "WHEN IT FIRES. Only on change … only for a term whose view is visible,
 *    and AT MOST ONCE PER TERM PER term_registry_ui_drain() PASS — i.e. once
 *    per frame, not once per byte and not once per drain chunk."
 *   "The drain accumulates the latest position per slot and flushes after it
 *    has released the table lock, so the sink runs UNLOCKED and may call back
 *    into the registry".
 *   "Coordinates are canvas pixels: the term's view rect plus col/row times
 *    the configured cell size. `h` is the cell height … `visible` is false
 *    when the cursor is hidden (DECTCEM off)".
 *   term_registry_config_t cell_w/cell_h: "0 selects TERM_CELL_W_DEFAULT /
 *    TERM_CELL_H_DEFAULT."
 *   PHASE4_MANIFEST Decision 7: "The caret is only pushed for a VISIBLE term,
 *    and term_registry_show re-arms it."
 *   §10.2: "push は表示中の term に限り、drain 1 パスあたり 1 回(バイト毎
 *    でもチャンク毎でもない)。カーソル非表示は「古い座標を残す」のではなく
 *    visible=false として通知".
 */
#include "pipe_util.h"

static p4_caret_t p4_car;
static p4_prod_t  p4_pa;

static void p4_feed_as(term_id_t id, const char *owner, const char *s)
{
    CHK_INT(term_registry_feed(id, owner, (const uint8_t *)s, strlen(s), NULL),
            TERM_OK);
}

static void p4_feed(term_id_t id, const char *s)
{
    p4_feed_as(id, OWNER_A, s);
}

/* ===================================================================== */
/* Geometry                                                              */
/* ===================================================================== */

static void case_geometry_uses_the_configured_cell(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;
    term_caret_ev_t ev;

    t_case("a visible term's caret is view.xy + col/row * the config's cell");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 30, 70, true), TERM_OK);
    reg_frame();
    REQUIRE(p4_car.n > 0);

    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[4;9H");           /* row 3, col 8 (0-based) */
    reg_frame();
    CHK_INT(p4_car.n, 1);
    ev = p4_caret_last(&p4_car);
    CHK_INT(ev.x, 30 + 8 * P4_CELL_W);
    CHK_INT(ev.y, 70 + 3 * P4_CELL_H);
    CHK_INT(ev.h, P4_CELL_H);
    CHK_TRUE(ev.visible);

    t_case("the event is attributed to the right term and owner");
    CHK_INT(ev.id, id);
    CHK_STR(ev.owner, OWNER_A);

    t_case("moving the view moves the caret with it");
    p4_caret_reset(&p4_car);
    CHK_INT(p4_show(id, OWNER_A, 100, 5, true), TERM_OK);
    reg_frame();
    CHK_TRUE(p4_car.n >= 1);
    ev = p4_caret_last(&p4_car);
    CHK_INT(ev.x, 100 + 8 * P4_CELL_W);
    CHK_INT(ev.y, 5 + 3 * P4_CELL_H);

    t_case("printed text advances the caret by one cell per column");
    p4_caret_reset(&p4_car);
    p4_feed(id, "ab");
    reg_frame();
    CHK_INT(p4_car.n, 1);
    ev = p4_caret_last(&p4_car);
    CHK_INT(ev.x, 100 + 10 * P4_CELL_W);
    CHK_INT(ev.y, 5 + 3 * P4_CELL_H);

    CHK_INT(reg_shutdown(), 0);
}

static void case_zero_config_selects_the_default_cell(void)
{
    term_id_t id;
    term_caret_ev_t ev;

    t_case("cell_w/cell_h left at 0 select TERM_CELL_W/H_DEFAULT (header)");
    CHK_TRUE(reg_boot());                     /* NULL config: all defaults */
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    reg_frame();

    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[3;5H");                  /* row 2, col 4 */
    reg_frame();
    CHK_INT(p4_car.n, 1);
    ev = p4_caret_last(&p4_car);
    CHK_INT(ev.x, 4 * TERM_CELL_W_DEFAULT);
    CHK_INT(ev.y, 2 * TERM_CELL_H_DEFAULT);
    CHK_INT(ev.h, TERM_CELL_H_DEFAULT);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Only on change, at most once per drain pass                            */
/* ===================================================================== */

static void case_only_on_change(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;

    t_case("an unchanged caret is silent, however many frames go by");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    reg_frame();
    p4_caret_reset(&p4_car);
    reg_frame();
    reg_frame();
    reg_frame();
    CHK_INT(p4_car.n, 0);

    t_case("bytes that do not move the cursor are silent too");
    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[?2004h\x1b]0;title\x07");   /* modes and an OSC */
    reg_frame();
    CHK_INT(p4_car.n, 0);

    t_case("a move that lands where the cursor already was is silent");
    p4_feed(id, "\x1b[1;1H");
    reg_frame();
    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[1;1H\x1b[1;1H");
    reg_frame();
    CHK_INT(p4_car.n, 0);

    t_case("one move, one event");
    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[2;3H");
    reg_frame();
    CHK_INT(p4_car.n, 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_coalesced_to_one_per_frame(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;
    term_caret_ev_t ev;
    char burst[600];
    int i, o = 0;

    t_case("hundreds of cursor moves in one drain produce ONE event (§10.2)");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    reg_frame();
    p4_caret_reset(&p4_car);

    /* 60 CUP sequences, ending at row 5 col 7 (1-based) => (6,4) 0-based. */
    for (i = 0; i < 59; i++)
        o += sprintf(burst + o, "\x1b[%d;%dH", (i % 9) + 1, (i % 39) + 1);
    o += sprintf(burst + o, "\x1b[5;7H");
    CHK_TRUE((size_t)o < sizeof burst);
    p4_feed(id, burst);
    reg_frame();

    CHK_INT(p4_car.n, 1);
    ev = p4_caret_last(&p4_car);
    CHK_INT(ev.x, 6 * P4_CELL_W);
    CHK_INT(ev.y, 4 * P4_CELL_H);

    t_case("...and it is the LAST position, even across several drain chunks");
    {
        term_registry_config_t small = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
        term_id_t v;
        int guard;

        CHK_INT(reg_shutdown(), 0);
        small.drain_budget_bytes = 16;   /* forces several chunks/frames   */
        CHK_TRUE(reg_boot_cfg(&small));
        p4_caret_install(&p4_car);
        v = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
        REQUIRE(v != TERM_ID_INVALID);
        CHK_INT(p4_show(v, OWNER_A, 0, 0, true), TERM_OK);
        reg_frame();
        p4_caret_reset(&p4_car);

        /* One frame can only parse 16 bytes, so this payload spans many
         * frames — the header allows one event per frame, and each frame's
         * event must be the position after that frame's chunk. Never more
         * than one per frame is the property; the count is therefore
         * bounded by the number of frames, not by the number of moves. */
        o = 0;
        for (i = 0; i < 20; i++)
            o += sprintf(burst + o, "\x1b[%d;%dH", (i % 9) + 1, (i % 39) + 1);
        p4_feed(v, burst);
        for (guard = 0; guard < 64; guard++) reg_frame();
        CHK_TRUE(p4_car.n >= 1);
        CHK_TRUE(p4_car.n <= 64);
        CHK_TRUE(p4_car.n < 20);         /* coalescing really happened     */
        ev = p4_caret_last(&p4_car);
        CHK_INT(ev.x, ((19 % 39)) * P4_CELL_W);
        CHK_INT(ev.y, ((19 % 9)) * P4_CELL_H);
        CHK_INT(reg_shutdown(), 0);
    }
}

/* ===================================================================== */
/* Visibility gating                                                     */
/* ===================================================================== */

static void case_only_visible_terms(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t shown, never, hidden;
    int i;

    t_case("a term that was never shown has no caret to offer");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    never = reg_new("n", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(never != TERM_ID_INVALID);
    p4_feed_as(never, OWNER_A, "\x1b[3;3Hmoving");
    reg_frame();
    CHK_INT(p4_car.n, 0);

    t_case("show(visible=false) is not a caret either");
    hidden = reg_new("h", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(hidden != TERM_ID_INVALID);
    CHK_INT(p4_show(hidden, OWNER_A, 10, 10, false), TERM_OK);
    p4_feed_as(hidden, OWNER_A, "\x1b[4;4Hmoving");
    reg_frame();
    CHK_INT(p4_car.n, 0);

    t_case("...but the hidden terms were still parsed (they just do not push)");
    CHK_TRUE(strstr(reg_snap(never, OWNER_A), "moving") != NULL);
    CHK_TRUE(strstr(reg_snap(hidden, OWNER_A), "moving") != NULL);

    t_case("a visible term pushes, and only for itself");
    shown = reg_new("s", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(shown != TERM_ID_INVALID);
    CHK_INT(p4_show(shown, OWNER_A, 0, 0, true), TERM_OK);
    reg_frame();
    p4_caret_reset(&p4_car);
    p4_feed_as(shown, OWNER_A, "\x1b[2;2H");
    p4_feed_as(hidden, OWNER_A, "\x1b[7;7H");
    p4_feed_as(never, OWNER_A, "\x1b[8;8H");
    reg_frame();
    CHK_INT(p4_car.n, 1);
    CHK_INT(p4_caret_last(&p4_car).id, shown);

    t_case("hiding it again goes silent (tab switching is show/hide, §8)");
    CHK_INT(p4_show(shown, OWNER_A, 0, 0, false), TERM_OK);
    reg_frame();
    p4_caret_reset(&p4_car);
    for (i = 0; i < 4; i++) {
        char seq[32];
        sprintf(seq, "\x1b[%d;%dH", i + 1, i + 2);
        p4_feed_as(shown, OWNER_A, seq);
        reg_frame();
    }
    CHK_INT(p4_car.n, 0);

    t_case("show() re-arms it: making it visible owes an anchor (Decision 7)");
    CHK_INT(p4_show(shown, OWNER_A, 4, 8, true), TERM_OK);
    reg_frame();
    CHK_TRUE(p4_car.n >= 1);
    {
        term_caret_ev_t ev = p4_caret_last(&p4_car);
        CHK_INT(ev.id, shown);
        CHK_TRUE(ev.visible);
        CHK_INT(ev.x, 4 + 4 * P4_CELL_W);     /* last CUP was row 4 col 5 */
        CHK_INT(ev.y, 8 + 3 * P4_CELL_H);
    }

    t_case("hide(NULL view) also stops the pushes (header: `view` NULL hides)");
    CHK_INT(term_registry_show(shown, OWNER_A, NULL), TERM_OK);
    reg_frame();
    p4_caret_reset(&p4_car);
    p4_feed_as(shown, OWNER_A, "\x1b[9;9H");
    reg_frame();
    CHK_INT(p4_car.n, 0);

    CHK_INT(reg_shutdown(), 0);
}

static void case_a_hidden_cursor_is_reported_as_such(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;
    term_caret_ev_t ev;

    t_case("DECTCEM off is notified as visible=false, not as a stale rect");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    p4_feed(id, "\x1b[3;5H");
    reg_frame();
    CHK_TRUE(p4_car.n >= 1);
    CHK_TRUE(p4_caret_last(&p4_car).visible);

    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[?25l");
    reg_frame();
    CHK_INT(p4_car.n, 1);
    ev = p4_caret_last(&p4_car);
    CHK_TRUE(!ev.visible);
    CHK_INT(ev.id, id);

    t_case("moves while the cursor is hidden are still reported, still invisible");
    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[6;6H");
    reg_frame();
    CHK_TRUE(p4_car.n <= 1);
    if (p4_car.n == 1) CHK_TRUE(!p4_caret_last(&p4_car).visible);

    t_case("DECTCEM back on is a change, and the anchor is current again");
    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[?25h");
    reg_frame();
    CHK_INT(p4_car.n, 1);
    ev = p4_caret_last(&p4_car);
    CHK_TRUE(ev.visible);
    CHK_INT(ev.x, 5 * P4_CELL_W);
    CHK_INT(ev.y, 5 * P4_CELL_H);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Where the sink runs                                                    */
/* ===================================================================== */

static void case_the_sink_runs_unlocked(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;
    int i;

    t_case("the flush is OUTSIDE the table lock, so the sink may call back in");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    p4_car.call_back_in = true;          /* term_registry_info() from inside */
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    reg_frame();
    p4_caret_reset(&p4_car);
    p4_car.call_back_in = true;
    p4_feed(id, "\x1b[5;5H");
    reg_frame();

    CHK_INT(p4_car.n, 1);
    CHK_INT(p4_car.lock_depth_seen, 0);
    CHK_TRUE(p4_car.callback_tries >= 1);
    CHK_INT(p4_car.callback_rc, TERM_OK);
    CHK_INT(fp.recursive_attempts, 0);
    CHK_INT(fp.depth, 0);
    CHK_INT(fp.lock_ok, fp.unlocks);

    t_case("pushing carets allocates nothing (I4: the flush list is on stack)");
    p4_car.call_back_in = false;
    fp.alloc_frozen = true;
    for (i = 0; i < 32; i++) {
        char seq[32];
        sprintf(seq, "\x1b[%d;%dH", (i % 9) + 1, (i % 39) + 1);
        p4_feed(id, seq);
        reg_frame();
    }
    fp.alloc_frozen = false;
    CHK_INT(fp.alloc_violations, 0);

    CHK_INT(reg_shutdown(), 0);
}

static void case_the_sink_belongs_to_the_table(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;

    t_case("deinit clears the caret sink (header / Decision 18)");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    reg_frame();
    CHK_TRUE(p4_car.n >= 1);
    CHK_INT(reg_shutdown(), 0);

    /* A second registry, and NO sink installed. If deinit had kept the
     * pointer, this case's events would land in the previous case's storage —
     * which on the device is another subsystem's memory. */
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_reset(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    p4_feed(id, "\x1b[4;4Hx");
    reg_frame();
    reg_frame();
    CHK_INT(p4_car.n, 0);

    t_case("set_caret_sink(NULL) removes it, and a re-install works");
    p4_caret_install(&p4_car);
    p4_feed(id, "\x1b[5;5H");
    reg_frame();
    CHK_TRUE(p4_car.n >= 1);

    term_registry_set_caret_sink(NULL, NULL);
    p4_caret_reset(&p4_car);
    p4_feed(id, "\x1b[6;6H");
    reg_frame();
    CHK_INT(p4_car.n, 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The caret of a piped term                                             */
/* ===================================================================== */

static void case_a_piped_term_pushes_too(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t id;
    term_err_t rc = TERM_ERR_INVAL;
    term_registry_stats_t before, after;

    t_case("a term fed by a producer pushes its caret the same way");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 10);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 2, 3, true), TERM_OK);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
    reg_frame();

    p4_caret_reset(&p4_car);
    before = reg_stats();
    {
        static const char seq[] = "\x1b[7;2Hprompt$ ";
        CHK_INT((int)p4_write(id, &p4_pa, seq, sizeof seq - 1, &rc),
                (int)(sizeof seq - 1));
        CHK_INT(rc, TERM_OK);
    }
    reg_frame();
    after = reg_stats();

    CHK_INT(p4_car.n, 1);
    CHK_INT(p4_caret_last(&p4_car).x, 2 + (1 + 8) * P4_CELL_W);
    CHK_INT(p4_caret_last(&p4_car).y, 3 + 6 * P4_CELL_H);

    t_case("caret_pushes counts exactly the events the sink saw");
    CHK_INT((int)(after.caret_pushes - before.caret_pushes), p4_car.n);

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

static void case_several_terms_each_get_one(void)
{
    term_registry_config_t c = p4_cfg_cells(P4_CELL_W, P4_CELL_H);
    term_id_t a, b;
    int i, seen_a = 0, seen_b = 0;

    t_case("two visible terms moving in one frame produce one event each");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_caret_install(&p4_car);
    a = reg_new("a", OWNER_A, TERM_VT, false, 40, 10);
    b = reg_new("b", OWNER_B, TERM_VT, false, 40, 10);
    REQUIRE(a != TERM_ID_INVALID && b != TERM_ID_INVALID);
    CHK_INT(p4_show(a, OWNER_A, 0, 0, true), TERM_OK);
    CHK_INT(p4_show(b, OWNER_B, 200, 0, true), TERM_OK);
    reg_frame();

    p4_caret_reset(&p4_car);
    p4_feed_as(a, OWNER_A, "\x1b[2;2H\x1b[3;3H");
    p4_feed_as(b, OWNER_B, "\x1b[4;4H\x1b[5;5H");
    reg_frame();

    CHK_INT(p4_car.n, 2);
    for (i = 0; i < p4_car.n && i < P4_CARET_MAX; i++) {
        if (p4_car.ev[i].id == a) {
            seen_a++;
            CHK_STR(p4_car.ev[i].owner, OWNER_A);
            CHK_INT(p4_car.ev[i].x, 2 * P4_CELL_W);
            CHK_INT(p4_car.ev[i].y, 2 * P4_CELL_H);
        } else if (p4_car.ev[i].id == b) {
            seen_b++;
            CHK_STR(p4_car.ev[i].owner, OWNER_B);
            CHK_INT(p4_car.ev[i].x, 200 + 4 * P4_CELL_W);
            CHK_INT(p4_car.ev[i].y, 4 * P4_CELL_H);
        }
    }
    CHK_INT(seen_a, 1);
    CHK_INT(seen_b, 1);

    t_case("a LOG term's caret is pushed on the same terms as a VT one");
    {
        term_id_t lg = reg_new("l", OWNER_A, TERM_LOG, false, 40, 10);
        REQUIRE(lg != TERM_ID_INVALID);
        CHK_INT(p4_show(lg, OWNER_A, 0, 300, true), TERM_OK);
        reg_frame();
        p4_caret_reset(&p4_car);
        CHK_INT(term_registry_log(lg, OWNER_A, "a line", 6), TERM_OK);
        reg_frame();
        /* The design does not say a LOG term must have a caret, only that a
         * visible term's cursor movement is pushed. Either answer is
         * consistent; what must not happen is an event for the WRONG term. */
        for (i = 0; i < p4_car.n && i < P4_CARET_MAX; i++)
            CHK_INT(p4_car.ev[i].id, lg);
    }

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("caret_sink");
    case_geometry_uses_the_configured_cell();
    case_zero_config_selects_the_default_cell();
    case_only_on_change();
    case_coalesced_to_one_per_frame();
    case_only_visible_terms();
    case_a_hidden_cursor_is_reported_as_such();
    case_the_sink_runs_unlocked();
    case_the_sink_belongs_to_the_table();
    case_a_piped_term_pushes_too();
    case_several_terms_each_get_one();
    fp_reclaim_all();
    return t_summary();
}
