/*
 * test_rec_clear.c — R1's non-persistence, one case per transition.
 *
 * term_registry.h R1 enumerates where the flag is cleared, and the enumeration
 * IS the guarantee: "OFF BY DEFAULT, PER TERM, AND NEVER REMEMBERED ANYWHERE
 * ... This is the user's decision of 2026-07-30 and it is a GUARANTEE a test
 * may assert, not a default a caller may change":
 *
 *   - create             "a slot is reused; whatever the last tenant left is
 *                         not a guarantee" (PHASE5_MANIFEST decision 5)
 *   - RE-ATTACHING       "clears it, even though the term's scrollback
 *                         survived. Picking a tmux session back up is a new
 *                         session for this purpose"
 *   - DETACHING          "(the owner app stopping) clears it"
 *   - BINDING A PRODUCER "clears it, which is what makes 'a tab reused for a
 *                         new ssh login after a disconnect' start off"
 *   - a detach ack       "clears it (the session ended)"
 *   - a reboot           "the flag lives in .bss, not in the LP region"
 *   plus stage 1 (close) which R1's rec_cleared list names explicitly:
 *   "re-attach, detach, pipe bind, detach ack, close".
 *
 * PHASE5_MANIFEST decision 5 adds two paths that reach stage 1 by another
 * door — close_forced and the persist LRU evict — and says "Each of the last
 * five also takes the final screen capture on its way out, so 'the flag is
 * cleared' and 'the last screen is kept' are the same event". Both halves are
 * asserted here.
 *
 * THE COUNTER DISTINCTION, which is the whole point of having two: rec_off is
 * "turned off explicitly (not by lifecycle)" and rec_cleared is "turned off BY
 * a lifecycle transition, i.e. the R1 guarantee doing its work". Every case
 * below asserts that the lifecycle moved rec_cleared and did NOT move rec_off.
 */
#include "rec_util.h"

/* Arm a term and check it took. Returns 1 when the case can continue. */
static int arm(term_id_t id, const char *owner)
{
    term_err_t rc = term_registry_record(id, owner, true);
    int on;
    CHK_INT(rc, TERM_OK);
    on = p5_recording(id, owner);
    CHK_INT(on, 1);
    return rc == TERM_OK && on == 1;
}

/* A term with something on the glass, so the final capture has a row to
 * write and "the last screen is kept" is observable rather than vacuous. */
static term_id_t armed_term(const char *name, const char *owner, bool persist,
                            const char *screen_text)
{
    term_id_t id = reg_new(name, owner, TERM_VT, persist, 24, 4);
    if (id == TERM_ID_INVALID) return id;
    p5_feed(id, owner, screen_text);
    p5_feed(id, owner, "\r\n");
    if (!arm(id, owner)) return TERM_ID_INVALID;
    return id;
}

/* ===================================================================== */
/* 1. create                                                             */
/* ===================================================================== */

static void case_create_starts_off(void)
{
    term_id_t first, second;
    p5_mark_t a, b;

    t_case("a REUSED slot's new tenant starts off (R1: create)");
    CHK_TRUE(reg_boot());
    first = armed_term("c1", OWNER_A, false, "FIRSTTENANT");
    REQUIRE(first != TERM_ID_INVALID);
    CHK_INT(term_registry_close(first, OWNER_A), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(first), -1);                 /* the slot is FREE again */

    p5_mark(&a);
    second = reg_new("c2", OWNER_A, TERM_VT, false, 24, 4);
    REQUIRE(second != TERM_ID_INVALID);
    CHK_INT(term_id_slot(second), term_id_slot(first));   /* the same slot   */
    CHK_INT(p5_recording(second, OWNER_A), 0);
    CHK_INT(p5_info_recording(second), 0);
    p5_show(second, OWNER_A);
    CHK_INT(p5_core_recording(), 0);

    t_case("...and creating it recorded nothing and moved no counter");
    p5_feed_lines(second, OWNER_A, "REUSEDSLOT", 0, 12);
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("REUSEDSLOT"), 0);
    P5_CHK_RECD(&a, &b, "create into a slot whose last tenant recorded",
                P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 2. re-attach — and the scrollback that must survive it                */
/* ===================================================================== */

static void case_reattach_comes_back_off(void)
{
    term_id_t id, again = TERM_ID_INVALID;
    term_create_opts_t o;
    bool re = false;
    p5_mark_t a, b;
    char out[512];
    term_read_result_t res;

    t_case("a persist term is armed, then its owner stops (R1: detach)");
    CHK_TRUE(reg_boot());
    id = reg_new("tab0", OWNER_A, TERM_VT, true, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p5_feed_lines(id, OWNER_A, "HISTORY", 0, 10);   /* archived, survives   */
    p5_feed(id, OWNER_A, "ONGLASS\r\n");
    REQUIRE(arm(id, OWNER_A));

    p5_mark(&a);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(id), TERM_SLOT_DETACHED);
    CHK_INT(p5_info_recording(id), 0);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "detach clears and captures",
                { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });

    /*
     * The re-attach clear can only be OBSERVED if the flag is on when the
     * re-attach happens, and the detach above has already cleared it. The
     * header does not say whether a DETACHED term may be armed; it can be
     * (observed), so this is the path that makes R1's re-attach bullet
     * testable at all. If a future change refuses it, this case must be
     * re-derived rather than deleted: the bullet would then be structurally
     * unreachable and should be documented as such.
     */
    t_case("armed again while DETACHED, so the re-attach clear is observable");
    REQUIRE(arm(id, OWNER_A));

    t_case("re-attach returns the same id, off, with its scrollback intact");
    p5_mark(&a);
    o = reg_opts("tab0", OWNER_A, TERM_VT, true, 24, 4);
    CHK_INT(term_registry_create(&o, &again, &re), TERM_OK);
    CHK_INT(again, id);                      /* generation included (§3.1)  */
    CHK_TRUE(re);
    CHK_INT(p5_recording(again, OWNER_A), 0);
    CHK_INT(p5_info_recording(again), 0);
    p5_show(again, OWNER_A);
    CHK_INT(p5_core_recording(), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "re-attach clears (and does not count as an explicit off)",
                { 0, 0, 1, P5_ANY, P5_ANY, P5_ANY, 0, 0, 0 });

    /* The other half: the case is only meaningful if the history really did
     * survive. A re-attach that lost the scrollback would pass the "off"
     * assertion for entirely the wrong reason. */
    t_case("...the scrollback really did survive (else the case proves nothing)");
    memset(out, 0, sizeof out);
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(again, OWNER_A, 0, 4, out, sizeof out, &res), TERM_OK);
    CHK_TRUE(res.lines >= 3);
    CHK_TRUE(strstr(out, "HISTORY000") != NULL);
    CHK_TRUE(strstr(reg_snap(again, OWNER_A), "ONGLASS") != NULL);

    t_case("...and the re-attached session records nothing until armed again");
    p5_mark(&a);
    p5_feed_lines(again, OWNER_A, "AFTERREATTACH", 0, 12);
    p5_quiet((int64_t)TERM_REC_SNAP_MIN_MS + 100, 2);
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("AFTERREATTACH"), 0);
    P5_CHK_RECD(&a, &b, "traffic after a re-attach", P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 3. detach — and what it kept                                          */
/* ===================================================================== */

static p5_bag_t detach_bag;

static void case_detach_keeps_the_last_screen(void)
{
    term_id_t id;
    p5_mark_t a, b;
    p5_lines_t glass;
    unsigned scr_before;

    t_case("detach clears the flag AND keeps the screen (manifest decision 5)");
    CHK_TRUE(reg_boot());
    id = armed_term("d0", OWNER_A, true, "DETACHGLASS");
    REQUIRE(id != TERM_ID_INVALID);
    p5_feed(id, OWNER_A, "DETACHSECOND\r\n");
    p5_screen_lines(&glass, id, OWNER_A);
    CHK_TRUE(glass.n >= 2);

    p5_collect(&detach_bag);
    scr_before = p5_bag_count(&detach_bag, p5_is_scr);
    p5_mark(&a);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    p5_mark(&b);
    CHK_INT(p5_info_recording(id), 0);
    P5_CHK_RECD(&a, &b, "detach", { 0, 0, 1, P5_ANY, 1, (long)glass.n, 0, 0, 0 });

    t_case("...the kept screen is the one that was on the glass");
    p5_collect(&detach_bag);
    {
        unsigned k, i = 0, found = 0;
        for (k = 0; k < detach_bag.n; k++) {
            const p5_rec_t *d = &detach_bag.r[k];
            if (!p5_is_scr(d)) continue;
            if (i++ < scr_before) continue;          /* records from before  */
            if (p5_is_marker(d)) { found = 1; continue; }
            CHK_TRUE(found);                          /* marker comes first  */
            CHK_TRUE(p5_bag_with(&detach_bag, d->text) > 0);
        }
        CHK_TRUE(found);
    }
    CHK_TRUE(p5_region_hits("DETACHSECOND") >= 1);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 4. producer bind — all three doors                                    */
/* ===================================================================== */

static void case_producer_bind_clears(void)
{
    term_id_t id;
    static p4_prod_t prod;
    term_producer_t pt;
    p5_mark_t a, b;

    t_case("term_registry_pipe on an armed term clears it (R1: bind)");
    CHK_TRUE(reg_boot());
    id = armed_term("b0", OWNER_A, false, "BINDGLASS");
    REQUIRE(id != TERM_ID_INVALID);
    p4_prod_reset(&prod, "b");
    pt = p4_producer(&prod, false);
    p5_mark(&a);
    CHK_INT(term_registry_pipe(id, OWNER_A, &pt), TERM_OK);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_show(id, OWNER_A);
    CHK_INT(p5_core_recording(), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "pipe bind", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });

    t_case("...the new session records nothing until a human arms it again");
    p5_mark(&a);
    {
        size_t wrote = 0;
        CHK_INT(term_registry_producer_write(id, &prod,
                    (const uint8_t *)"NEWSESSION1\r\nNEWSESSION2\r\nNEWSESSION3\r\n",
                    39, &wrote), TERM_OK);
    }
    p5_feed_frames(id, OWNER_A, "", 6);
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT((int)p5_region_hits("NEWSESSION"), 0);
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);

    t_case("producer_bind (the plain one) clears it too");
    CHK_TRUE(reg_boot());
    id = armed_term("b1", OWNER_A, false, "BINDGLASS2");
    REQUIRE(id != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, p4_detach, &prod), TERM_OK);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "producer_bind", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);

    t_case("producer_bind_ex clears it too — no door is left unlocked");
    CHK_TRUE(reg_boot());
    id = armed_term("b2", OWNER_A, false, "BINDGLASS3");
    REQUIRE(id != TERM_ID_INVALID);
    pt = p4_producer(&prod, true);
    p5_mark(&a);
    CHK_INT(term_registry_producer_bind_ex(id, OWNER_A, &pt), TERM_OK);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "producer_bind_ex", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 5. the detach ack — "the session ended"                               */
/* ===================================================================== */

static void case_detach_ack_clears(void)
{
    term_id_t id;
    static p4_prod_t prod;
    term_producer_t pt;
    p5_mark_t a, b;

    t_case("a producer's detach ack clears the flag (R1: session end)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("a0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p4_prod_reset(&prod, "a");
    pt = p4_producer(&prod, false);
    CHK_INT(term_registry_pipe(id, OWNER_A, &pt), TERM_OK);
    {
        size_t wrote = 0;
        CHK_INT(term_registry_producer_write(id, &prod,
                    (const uint8_t *)"ACKGLASS\r\n", 10, &wrote), TERM_OK);
    }
    p5_feed_frames(id, OWNER_A, "", 4);
    REQUIRE(arm(id, OWNER_A));      /* armed AFTER the bind, as a human would */

    t_case("unbind alone is not one of the six — it only requests the detach");
    p5_mark(&a);
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(prod.detaches, 1);
    p5_mark(&b);
    /* The header lists the ACK, not the unbind: the session has not ended
     * until the producer says it has stopped touching the term. */
    CHK_INT(p5_recording(id, OWNER_A), 1);
    P5_CHK_RECD(&a, &b, "unbind", P5_RECD_ZERO);

    t_case("...the ack is, and it keeps the last screen of the session");
    p5_mark(&a);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "detach ack", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });
    CHK_TRUE(p5_region_hits("ACKGLASS") >= 1);

    /*
     * The header's stale-ack clause is about generations: "Safe to call on an
     * id whose slot has already moved on: a stale ack is dropped, counted, and
     * is never mistaken for the ack of a later term in the same slot (that is
     * what the generation in the id is for, I2)." Recording makes that clause
     * load-bearing in a new way — a stale ack that WAS mistaken would reach
     * into a different session's flag.
     */
    t_case("...and a stale ack cannot clear a later term in the same slot (I2)");
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    {
        term_id_t next = p5_new_vt("a1", OWNER_A, 24, 4);
        REQUIRE(next != TERM_ID_INVALID);
        CHK_INT(term_id_slot(next), term_id_slot(id));
        p5_feed(next, OWNER_A, "SECONDSESSION\r\n");
        REQUIRE(arm(next, OWNER_A));

        p5_mark(&a);
        /* The header does not fix the return value of a dropped ack ("safe to
         * call"), only its effect: dropped and counted. */
        (void)term_registry_producer_ack(id);                 /* the OLD id */
        p5_mark(&b);
        CHK_INT(p5_recording(next, OWNER_A), 1);
        P5_CHK_RECD(&a, &b, "an ack for a term that is gone", P5_RECD_ZERO);
        CHK_TRUE(reg_stats().stale_acks > 0);
        CHK_INT(term_registry_close(next, OWNER_A), TERM_OK);
    }
    reg_settle();
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 6. close, and the two other doors into stage 1                        */
/* ===================================================================== */

static void case_close_clears(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("close clears the flag and keeps the last screen (R1: close)");
    CHK_TRUE(reg_boot());
    id = armed_term("x0", OWNER_A, false, "CLOSEGLASS");
    REQUIRE(id != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    /* The owner-gated query is TERM_ERR_DYING from here on, so the flag has to
     * be read through the platform's introspection — which is why
     * term_slot_info_t carries it. */
    CHK_INT(term_registry_recording(id, OWNER_A), TERM_ERR_DYING);
    CHK_INT(p5_info_recording(id), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "close", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });
    CHK_TRUE(p5_region_hits("CLOSEGLASS") >= 1);

    t_case("...and a second close is a no-op that clears nothing again");
    p5_mark(&a);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "close of an already-DYING term", P5_RECD_ZERO);
    reg_settle();
    CHK_INT(reg_shutdown(), 0);

    t_case("close_forced reaches stage 1 by another door, and clears too");
    CHK_TRUE(reg_boot());
    id = armed_term("x1", OWNER_A, false, "FORCEDGLASS");
    REQUIRE(id != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_close_forced(id), TERM_OK);
    CHK_INT(p5_info_recording(id), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "close_forced", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });
    CHK_TRUE(p5_region_hits("FORCEDGLASS") >= 1);
    reg_settle();
    CHK_INT(reg_shutdown(), 0);

    t_case("owner_stopped on a NON-persist term is stage 1, and clears too");
    CHK_TRUE(reg_boot());
    id = armed_term("x2", OWNER_A, false, "TEARDOWNGLASS");
    REQUIRE(id != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    CHK_INT(p5_info_recording(id), 0);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "teardown of a non-persist term",
                { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });
    reg_settle();
    CHK_INT(reg_shutdown(), 0);
}

static void case_lru_evict_clears(void)
{
    term_registry_config_t cfg;
    term_id_t victim, newcomer;
    p5_mark_t a, b;

    t_case("the persist LRU evict clears too (manifest decision 5)");
    memset(&cfg, 0, sizeof cfg);
    cfg.persist_max = 1;
    CHK_TRUE(reg_boot_cfg(&cfg));
    victim = reg_new("e1", OWNER_A, TERM_VT, true, 24, 4);
    REQUIRE(victim != TERM_ID_INVALID);
    p5_feed(victim, OWNER_A, "EVICTGLASS\r\n");
    REQUIRE(arm(victim, OWNER_A));
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(victim), TERM_SLOT_DETACHED);
    /* Detach already cleared it; arm the detached term so that what the evict
     * does is visible. */
    REQUIRE(arm(victim, OWNER_A));
    fp_advance(50);

    p5_mark(&a);
    newcomer = reg_new("e2", OWNER_B, TERM_VT, true, 24, 4);
    CHK_TRUE(newcomer != TERM_ID_INVALID);
    p5_mark(&b);
    CHK_TRUE(reg_stats().evicted >= 1);
    CHK_INT(reg_state(victim), TERM_SLOT_DYING);
    CHK_INT(p5_info_recording(victim), 0);
    CHK_INT(p5_recording(newcomer, OWNER_B), 0);
    P5_CHK_RECD(&a, &b, "LRU evict", { 0, 0, 1, P5_ANY, 1, P5_ANY, 0, 0, 0 });

    reg_settle();
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The reboot bullet, as far as a host can reach it                      */
/* ===================================================================== */

static void case_nothing_survives_the_table(void)
{
    term_id_t id, again;
    p5_mark_t a, b;

    t_case("R1's reboot bullet: the flag lives in the table, nowhere else");
    /*
     * A host cannot reboot, but it can do the thing a reboot does to this
     * state: destroy the whole table and build a new one. If anything wrote
     * the flag to a store, a term recreated with the same owner+name would
     * come back armed. It must not.
     */
    CHK_TRUE(reg_boot());
    id = armed_term("keep", OWNER_A, true, "REBOOTGLASS");
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p5_recording(id, OWNER_A), 1);
    CHK_INT(reg_shutdown(), 0);

    /* term_registry_stats_t belongs to the table, so it starts again from zero
     * with the table: the mark has to be taken after the new init. The RING's
     * counters are process-wide and do span the two registries, which is what
     * makes the "nothing was recorded" half of this case meaningful. */
    CHK_TRUE(reg_boot());
    p5_mark(&a);
    again = reg_new("keep", OWNER_A, TERM_VT, true, 24, 4);
    REQUIRE(again != TERM_ID_INVALID);
    CHK_INT(p5_recording(again, OWNER_A), 0);
    CHK_INT(p5_info_recording(again), 0);
    p5_show(again, OWNER_A);
    CHK_INT(p5_core_recording(), 0);

    t_case("...and the new term records nothing on its own");
    p5_feed_lines(again, OWNER_A, "POSTREBOOT", 0, 12);
    p5_quiet((int64_t)TERM_REC_SNAP_MIN_MS + 100, 2);
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT((int)p5_region_hits("POSTREBOOT"), 0);
    CHK_INT(P5_DS(rec_on), 0);
    CHK_INT(P5_DS(rec_lines), 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* And the negative: a lifecycle transition on a term nobody armed        */
/* ===================================================================== */

static void case_clear_of_an_unarmed_term_counts_nothing(void)
{
    term_id_t id;
    p5_mark_t a, b;
    static p4_prod_t prod;
    term_producer_t pt;

    t_case("rec_cleared counts the guarantee WORKING, not every transition");
    CHK_TRUE(reg_boot());
    id = reg_new("u", OWNER_A, TERM_VT, true, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p5_feed(id, OWNER_A, "UNARMEDGLASS\r\n");
    p4_prod_reset(&prod, "u");
    pt = p4_producer(&prod, false);

    p5_mark(&a);
    CHK_INT(term_registry_pipe(id, OWNER_A, &pt), TERM_OK);
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(term_registry_close_forced(id), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "five transitions on a term nobody armed", P5_RECD_ZERO);
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("UNARMEDGLASS"), 0);

    reg_settle();
    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("rec_clear");
    case_create_starts_off();
    case_reattach_comes_back_off();
    case_detach_keeps_the_last_screen();
    case_producer_bind_clears();
    case_detach_ack_clears();
    case_close_clears();
    case_lru_evict_clears();
    case_nothing_survives_the_table();
    case_clear_of_an_unarmed_term_counts_nothing();
    p5_check_live_layout();
    fp_reclaim_all();
    return t_summary();
}
