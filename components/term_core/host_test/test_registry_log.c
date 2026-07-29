/*
 * test_registry_log.c — TERM_LOG ingest: bounded, lossy, counted, atomic.
 *
 * Contracts under test:
 *   §3.2 TERM_LOG "溢れ時: 行単位アトミック drop + カウンタ", "一次ストア =
 *     スクロールバック(論理行)", "alt screen: なし"
 *   §5   "producer 側は bounded wait のみ: JS/print は 20ms タイムアウトで
 *     drop+カウンタ(ロギングが誰かを待たせない)"
 *        "drain は 1 フレームあたりのバイト予算制 … 予算を使い切ったら
 *     残りは次フレームへ持ち越し"
 *   term_registry.h feed: "as much as fits is taken, the remainder is
 *     dropped, the drop counter advances and the call returns
 *     TERM_ERR_TIMEOUT"; log: "ATOMIC PER LINE — a line is taken whole or
 *     dropped whole"; "On a VT term it is equivalent to feeding the text
 *     plus CRLF."
 *   term_port.h: TERM_WAIT_FOREVER is "Legal for platform-internal callers
 *     … ingest paths must NOT use it".
 */
#include "reg_util.h"

static term_registry_config_t cfg_budget(int budget)
{
    term_registry_config_t c;
    memset(&c, 0, sizeof c);
    c.drain_budget_bytes = budget;
    return c;
}

/* ===================================================================== */
/* The happy path                                                        */
/* ===================================================================== */

static void case_log_round_trip(void)
{
    term_id_t id;
    char out[1024];
    term_read_result_t res;
    int i;

    t_case("term.log appends whole lines; scrollback holds the logical ones");
    CHK_TRUE(reg_boot());
    id = reg_new("con", OWNER_A, TERM_LOG, false, 20, 3);
    CHK_TRUE(id != TERM_ID_INVALID);
    for (i = 0; i < 12; i++) {
        char line[32];
        int n = sprintf(line, "L%d", i);
        CHK_INT(term_registry_log(id, OWNER_A, line, (size_t)n), TERM_OK);
    }
    reg_frame();

    /* How many rows a 3-row screen ends up showing after 12 appended lines
     * is arithmetic the design does not fix; that the newest line is on
     * screen and the oldest has left it, it does (§3.2). */
    t_case("the newest lines are on screen, the oldest have left it");
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "L11") != NULL);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "L0\n") == NULL);

    t_case("the older ones are in scrollback and read back as logical lines");
    memset(out, 0, sizeof out);
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 4, out, sizeof out, &res), TERM_OK);
    CHK_INT(res.lines, 4);
    CHK_STR(out, "L0\nL1\nL2\nL3");
    CHK_TRUE(!res.truncated);
    CHK_INT((int)res.bytes, (int)strlen(out));

    t_case("res.next chunks the rest without repeating or skipping (§7.2)");
    memset(out, 0, sizeof out);
    CHK_INT(term_registry_read(id, OWNER_A, res.next, 4, out, sizeof out, &res),
            TERM_OK);
    CHK_INT(res.lines, 4);
    CHK_STR(out, "L4\nL5\nL6\nL7");

    t_case("log's length argument is honoured, not strlen");
    CHK_INT(term_registry_log(id, OWNER_A, "abcdef", 3), TERM_OK);
    reg_frame();
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "abc") != NULL);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "abcdef") == NULL);

    t_case("bad log arguments are errors, not crashes (I5)");
    CHK_INT(term_registry_log(id, OWNER_A, NULL, 4), TERM_ERR_INVAL);
    CHK_INT(term_registry_feed(id, OWNER_A, NULL, 4, NULL), TERM_ERR_INVAL);

    CHK_INT(reg_shutdown(), 0);
}

static void case_log_mode_has_no_alt_screen(void)
{
    term_id_t vt, lg;
    static const char seq[] = "main\x1b[?1049h" "alt";

    t_case("TERM_LOG accepts-and-ignores DECSET 1049 (§3.2: no alt screen)");
    CHK_TRUE(reg_boot());
    lg = reg_new("l", OWNER_A, TERM_LOG, false, 20, 4);
    vt = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(lg != TERM_ID_INVALID && vt != TERM_ID_INVALID);

    CHK_INT(term_registry_feed(lg, OWNER_A, (const uint8_t *)seq, strlen(seq), NULL),
            TERM_OK);
    CHK_INT(term_registry_feed(vt, OWNER_A, (const uint8_t *)seq, strlen(seq), NULL),
            TERM_OK);
    reg_frame();

    /* On a VT term the alt screen hides "main"; on a LOG term there is no
     * alt screen to hide it with. */
    CHK_TRUE(strstr(reg_snap(vt, OWNER_A), "main") == NULL);
    CHK_TRUE(strstr(reg_snap(vt, OWNER_A), "alt") != NULL);
    CHK_TRUE(strstr(reg_snap(lg, OWNER_A), "main") != NULL);

    t_case("a LOG term therefore costs less memory than a VT one (§4.1)");
    CHK_TRUE(reg_info(lg).mem_bytes < reg_info(vt).mem_bytes);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Bounded wait: the lock is contended                                   */
/* ===================================================================== */

static void case_ingest_is_bounded_and_lossy(void)
{
    term_id_t id;
    term_registry_stats_t before, after;
    size_t wrote = 999;
    int forever_before;

    t_case("ingest never asks for TERM_WAIT_FOREVER (§5, term_port.h)");
    CHK_TRUE(reg_boot());
    id = reg_new("con", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    forever_before = fp.forever_locks;
    CHK_INT(term_registry_log(id, OWNER_A, "hello", 5), TERM_OK);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"hello", 5, NULL), TERM_OK);
    CHK_INT(fp.forever_locks, forever_before);
    CHK_TRUE(fp.bounded_locks > 0);
    CHK_TRUE(fp.last_timeout_ms <= TERM_INGEST_TIMEOUT_MS_DEFAULT);

    t_case("a lock it cannot get within the cap becomes a counted drop (§5)");
    before = reg_stats();
    fp.refuse_bounded = true;
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"lost", 4, &wrote),
            TERM_ERR_TIMEOUT);
    CHK_INT((int)wrote, 0);
    CHK_TRUE(term_registry_log(id, OWNER_A, "lost", 4) != TERM_OK);
    fp.refuse_bounded = false;
    after = reg_stats();
    CHK_TRUE(after.drops_lock - before.drops_lock >= 2);

    t_case("...and logging did not block: no unbounded wait was attempted");
    CHK_INT(fp.forever_locks, forever_before);

    t_case("nothing of the dropped text reached the term");
    reg_frame();
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "lost") == NULL);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "hello") != NULL);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Bounded ring: overflow                                                */
/* ===================================================================== */

static void case_ring_overflow_is_counted_not_overrun(void)
{
    term_id_t id;
    uint8_t chunk[512];
    size_t offered = 0, accepted = 0;
    size_t wrote;
    term_err_t rc = TERM_OK;
    term_slot_info_t inf;
    int i;

    t_case("a full byte ring drops the remainder and counts it (§4.2)");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    memset(chunk, 'x', sizeof chunk);

    /* No drain in between, so the ring can only fill. The design fixes the
     * ring at 8KB (§4.1) but the test does not depend on that: it pushes
     * until the registry says stop, or gives up well past any plausible
     * size. */
    for (i = 0; i < 256; i++) {
        wrote = 0;
        rc = term_registry_feed(id, OWNER_A, chunk, sizeof chunk, &wrote);
        offered += sizeof chunk;
        accepted += wrote;
        CHK_TRUE(wrote <= sizeof chunk);
        if (rc != TERM_OK) break;
    }
    CHK_INT(rc, TERM_ERR_TIMEOUT);
    CHK_TRUE(accepted < offered);

    t_case("the slot's own counters account for every offered byte");
    inf = reg_info(id);
    CHK_INT((int)inf.bytes_in, (int)accepted);
    CHK_INT((int)(inf.bytes_in + inf.bytes_dropped), (int)offered);
    CHK_TRUE(reg_stats().drops_full > 0);

    t_case("draining makes room again — the producer is not wedged");
    reg_frame();
    reg_frame();
    reg_frame();
    wrote = 0;
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"after", 5, &wrote),
            TERM_OK);
    CHK_INT((int)wrote, 5);

    CHK_INT(reg_shutdown(), 0);
}

static void case_log_lines_are_atomic(void)
{
    term_id_t id;
    char body[41];
    char line[256];
    int i, accepted = 0, rejected = 0;

    t_case("under pressure a line is taken whole or dropped whole (§3.2)");
    CHK_TRUE(reg_boot());
    id = reg_new("con", OWNER_A, TERM_LOG, false, 60, 3);
    CHK_TRUE(id != TERM_ID_INVALID);

    /* Short enough that a line never soft-wraps at 60 columns, so a torn
     * scrollback entry can only come from a torn ingest. */
    memset(body, 'y', sizeof body);
    body[sizeof body - 1] = 0;

    /* Push far more than any ring can hold, never draining, so some of
     * these lines must be refused. */
    for (i = 0; i < 2000; i++) {
        int n = sprintf(line, "<%03d%s>", i, body);
        if (term_registry_log(id, OWNER_A, line, (size_t)n) == TERM_OK) accepted++;
        else rejected++;
    }
    CHK_TRUE(accepted > 0);
    CHK_TRUE(rejected > 0);

    t_case("no half line ever appears: every archived line is well formed");
    for (i = 0; i < 32; i++) reg_frame();
    {
        char txt[4096];
        uint32_t from = 0;
        term_read_result_t res;
        int guard = 0;
        int checked = 0;
        do {
            memset(txt, 0, sizeof txt);
            memset(&res, 0, sizeof res);
            if (term_registry_read(id, OWNER_A, from, 8, txt, sizeof txt, &res) != TERM_OK)
                break;
            if (res.lines <= 0) break;
            {
                char *p = txt;
                while (p && *p) {
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    if (p[0]) {
                        /* A line accepted by an atomic ingest starts with
                         * '<' and ends with '>'; a torn one does not. */
                        checked++;
                        if (p[0] != '<' || p[strlen(p) - 1] != '>') {
                            t_head(__FILE__, __LINE__);
                            printf("     torn scrollback line: \"%.40s\"...\n", p);
                        }
                        t_checks++;
                    }
                    p = nl ? nl + 1 : NULL;
                }
            }
            from = res.next;
        } while (++guard < 256);
        CHK_TRUE(checked > 0);
    }

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Drain budget (§5)                                                     */
/* ===================================================================== */

static void case_drain_budget_carries_over(void)
{
    term_registry_config_t c = cfg_budget(16);
    term_id_t id;
    const char *payload = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJ"; /* 46 */
    size_t len;
    size_t seen_after_one;
    int frames;

    t_case("one frame parses at most the configured budget (§5)");
    CHK_TRUE(reg_boot_cfg(&c));
    id = reg_new("v", OWNER_A, TERM_VT, false, 60, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    len = strlen(payload);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)payload, len, NULL),
            TERM_OK);

    reg_frame();
    seen_after_one = strlen(reg_snap_line0(id, OWNER_A));
    CHK_TRUE(seen_after_one > 0);
    CHK_TRUE(seen_after_one <= 16);

    t_case("the leftovers carry to the following frames, in order, intact");
    for (frames = 0; frames < 8 && strlen(reg_snap_line0(id, OWNER_A)) < len; frames++)
        reg_frame();
    CHK_STR(reg_snap_line0(id, OWNER_A), payload);
    CHK_TRUE(frames >= 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_drain_reports_visible_change(void)
{
    term_id_t shown, hidden;
    term_view_t view;

    t_case("ui_drain says whether a blit pass is worth running (header)");
    CHK_TRUE(reg_boot());
    shown = reg_new("s", OWNER_A, TERM_VT, false, 20, 4);
    hidden = reg_new("h", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(shown != TERM_ID_INVALID && hidden != TERM_ID_INVALID);

    memset(&view, 0, sizeof view);
    view.x = 0; view.y = 0; view.w = 100; view.h = 50; view.visible = true;
    CHK_INT(term_registry_show(shown, OWNER_A, &view), TERM_OK);
    reg_frame();                      /* drain + blit, damage now clear */
    CHK_INT(reg_paint(), 1);          /* exactly one term is painted    */

    CHK_INT(term_registry_feed(hidden, OWNER_A, (const uint8_t *)"quiet", 5, NULL),
            TERM_OK);
    fp_pump();
    CHK_TRUE(!term_registry_ui_drain());
    reg_paint();

    t_case("...but a hidden term is still parsed — it just is not painted");
    CHK_TRUE(strstr(reg_snap(hidden, OWNER_A), "quiet") != NULL);

    t_case("a visible term that changed asks for the blit");
    CHK_INT(term_registry_feed(shown, OWNER_A, (const uint8_t *)"loud", 4, NULL),
            TERM_OK);
    CHK_TRUE(term_registry_ui_drain());

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("registry_log");
    case_log_round_trip();
    case_log_mode_has_no_alt_screen();
    case_ingest_is_bounded_and_lossy();
    case_ring_overflow_is_counted_not_overrun();
    case_log_lines_are_atomic();
    case_drain_budget_carries_over();
    case_drain_reports_visible_change();
    fp_reclaim_all();
    return t_summary();
}
