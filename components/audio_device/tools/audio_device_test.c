/*
 * Host unit test for the audio_device core state machine. No FreeRTOS, no
 * hardware — drives audio_dev_core_* directly and interprets the emitted
 * action lists against a mock backend, exactly as the device manager task
 * would. Build + run in WSL:
 *   cd components/audio_device
 *   gcc -O2 -Iinclude -fsanitize=address,undefined \
 *       -o /tmp/audev_test tools/audio_device_test.c audio_device_core.c
 *   /tmp/audev_test
 */
#include "audio_device_core.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL (line %d): %s\n", __LINE__, msg);                     \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* ---- mock backend + producer handles -------------------------------- */

typedef struct {
    int running;
    uint32_t rate;
    uint8_t ch;
    int starts, stops, flushes;
    int mixer_running;
    int mixer_starts, mixer_stops, mixer_removes;
    int mix_sources; /* live MIXING streams the wrapper would be summing */
} mock_backend_t;

typedef struct {
    audio_request_id_t id;
    audio_token_t token;
    int granted;
    int cancelled;
} producer_t;

static mock_backend_t g_be;

static void on_granted(audio_request_id_t req, audio_token_t token, void *arg)
{
    producer_t *p = arg;
    p->granted++;
    p->token = token;
    CHECK(req == p->id, "granted req id matches");
}

static void on_cancelled(audio_request_id_t req, void *arg)
{
    producer_t *p = arg;
    p->cancelled++;
    CHECK(req == p->id, "cancelled req id matches");
}

/* Execute an action list the way the manager task does: backend effects
 * first, then producer callbacks, in order. Also verifies the invariant
 * that a START always precedes the GRANTED that hands out its token. */
static void run_actions(const audio_action_list_t *acts)
{
    for (int i = 0; i < acts->n; i++) {
        const audio_action_t *a = &acts->a[i];
        switch (a->kind) {
        case AUDIO_ACT_BACKEND_START:
            g_be.running = 1;
            g_be.rate = a->u.fmt.sample_rate;
            g_be.ch = a->u.fmt.channels;
            g_be.starts++;
            break;
        case AUDIO_ACT_BACKEND_STOP:
            g_be.running = 0;
            g_be.stops++;
            break;
        case AUDIO_ACT_BACKEND_FLUSH:
            g_be.running = 0;
            g_be.flushes++;
            break;
        case AUDIO_ACT_MIXER_START:
            CHECK(g_be.running, "MIXER_START after BACKEND_START");
            g_be.mixer_running = 1;
            g_be.mixer_starts++;
            break;
        case AUDIO_ACT_MIXER_REMOVE:
            CHECK(g_be.mixer_running, "MIXER_REMOVE while mixer running");
            g_be.mixer_removes++;
            g_be.mix_sources--;
            break;
        case AUDIO_ACT_MIXER_STOP:
            g_be.mixer_running = 0;
            g_be.mixer_stops++;
            break;
        case AUDIO_ACT_FIRE_GRANTED:
            /* A stream is writable only once its backend is live. For an
               EXCLUSIVE grant a START precedes it here; for a MIXING join
               the backend is already running from the session's open. */
            CHECK(g_be.running, "backend running when GRANTED fires");
            if (a->u.grant.mixing) {
                CHECK(g_be.mixer_running, "mixer running for a MIXING grant");
                g_be.mix_sources++;
            }
            a->u.grant.cb(a->u.grant.req, a->u.grant.token, a->u.grant.arg);
            break;
        case AUDIO_ACT_FIRE_CANCELLED:
            a->u.cancel.cb(a->u.cancel.req, a->u.cancel.arg);
            break;
        }
    }
}

static audio_action_list_t ACTS;
static audio_action_list_t *acts(void)
{
    memset(&ACTS, 0, sizeof ACTS);
    return &ACTS;
}

static const audio_request_policy_t POL_NORMAL = {
    .conflict = AUDIO_CONFLICT_QUEUE, .cancelable = true, .priority = 10
};
static const audio_format_t FMT_48S = { .sample_rate = 48000, .channels = 2 };
static const audio_format_t FMT_16M = { .sample_rate = 16000, .channels = 1 };

static void submit(audio_dev_core_t *c, producer_t *p, audio_request_id_t id,
                   const audio_format_t *fmt, const audio_request_policy_t *pol)
{
    memset(p, 0, sizeof *p);
    p->id = id;
    audio_action_list_t *a = acts();
    audio_dev_core_submit(c, id, fmt, pol, on_granted, on_cancelled, p, a);
    run_actions(a);
}

/* ---- tests ----------------------------------------------------------- */

static void test_grant_on_idle(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    producer_t p;
    submit(&c, &p, 1, &FMT_48S, &POL_NORMAL);

    CHECK(p.granted == 1, "granted once");
    CHECK(p.cancelled == 0, "not cancelled");
    CHECK(g_be.running && g_be.rate == 48000 && g_be.ch == 2, "backend at fmt");
    CHECK(g_be.starts == 1, "one start");
    CHECK(audio_dev_core_token_active(&c, p.token), "token active");
}

static void test_finish_to_idle(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    producer_t p;
    submit(&c, &p, 1, &FMT_48S, &POL_NORMAL);

    audio_action_list_t *a = acts();
    CHECK(audio_dev_core_finish(&c, p.token, a), "finish ok");
    run_actions(a);

    CHECK(!audio_dev_core_token_active(&c, p.token), "token revoked");
    CHECK(g_be.stops == 1 && g_be.flushes == 0, "drained, not flushed");
    CHECK(!g_be.running, "backend stopped at idle");

    /* finishing a stale token again is a no-op */
    a = acts();
    CHECK(!audio_dev_core_finish(&c, p.token, a), "stale finish rejected");
    CHECK(a->n == 0, "stale finish emits nothing");
}

static void test_abort_flushes(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    producer_t p;
    submit(&c, &p, 1, &FMT_48S, &POL_NORMAL);

    audio_action_list_t *a = acts();
    CHECK(audio_dev_core_abort(&c, p.token, a), "abort ok");
    run_actions(a);

    CHECK(!audio_dev_core_token_active(&c, p.token), "token revoked on abort");
    CHECK(g_be.flushes == 1 && g_be.stops == 0, "flushed, not drained");
}

static void test_queue_priority_and_fifo(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    /* P1 active; queue P2(prio1) P3(prio5) P4(prio1). */
    producer_t p1, p2, p3, p4;
    audio_request_policy_t lo = { AUDIO_CONFLICT_QUEUE, true, 1 };
    audio_request_policy_t hi = { AUDIO_CONFLICT_QUEUE, true, 5 };

    submit(&c, &p1, 1, &FMT_48S, &POL_NORMAL);
    submit(&c, &p2, 2, &FMT_16M, &lo);
    submit(&c, &p3, 3, &FMT_48S, &hi);
    submit(&c, &p4, 4, &FMT_16M, &lo);

    CHECK(p1.granted == 1, "p1 active");
    CHECK(p2.granted == 0 && p3.granted == 0 && p4.granted == 0, "rest queued");

    /* finish p1 -> highest priority (p3) runs next */
    audio_action_list_t *a = acts();
    audio_dev_core_finish(&c, p1.token, a);
    run_actions(a);
    CHECK(p3.granted == 1, "p3 (prio5) dispatched first");
    CHECK(g_be.rate == 48000, "backend reconfigured to p3 fmt");

    /* finish p3 -> p2 before p4 (same prio, lower seq) */
    a = acts();
    audio_dev_core_finish(&c, p3.token, a);
    run_actions(a);
    CHECK(p2.granted == 1 && p4.granted == 0, "p2 before p4 (FIFO)");
    CHECK(g_be.rate == 16000 && g_be.ch == 1, "backend reconfigured to p2 fmt");

    /* finish p2 -> p4, then idle */
    a = acts();
    audio_dev_core_finish(&c, p2.token, a);
    run_actions(a);
    CHECK(p4.granted == 1, "p4 last");

    a = acts();
    audio_dev_core_finish(&c, p4.token, a);
    run_actions(a);
    CHECK(c.session == AUDIO_SESSION_IDLE, "idle after last finish");
    CHECK(!g_be.running, "backend stopped");
}

static void test_cancel_queued(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    producer_t p1, p2;
    submit(&c, &p1, 1, &FMT_48S, &POL_NORMAL);
    submit(&c, &p2, 2, &FMT_48S, &POL_NORMAL);
    CHECK(p2.granted == 0, "p2 queued");

    audio_action_list_t *a = acts();
    CHECK(audio_dev_core_cancel_request(&c, 2, a), "cancel queued p2");
    run_actions(a);
    CHECK(p2.cancelled == 1, "p2 got cancelled cb");

    /* cancelling again -> unknown */
    a = acts();
    CHECK(!audio_dev_core_cancel_request(&c, 2, a), "double cancel unknown");

    /* finishing p1 now goes idle (p2 is gone) */
    a = acts();
    audio_dev_core_finish(&c, p1.token, a);
    run_actions(a);
    CHECK(p2.granted == 0, "cancelled request never grants");
    CHECK(c.session == AUDIO_SESSION_IDLE, "idle, queue empty");
}

static void test_queue_full_rejects(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    producer_t active;
    submit(&c, &active, 1, &FMT_48S, &POL_NORMAL);

    /* fill the queue exactly */
    producer_t q[AUDIO_DEV_MAX_QUEUED];
    for (int i = 0; i < AUDIO_DEV_MAX_QUEUED; i++) {
        submit(&c, &q[i], (audio_request_id_t)(100 + i), &FMT_48S, &POL_NORMAL);
        CHECK(q[i].granted == 0 && q[i].cancelled == 0, "queued, pending");
    }

    /* one more must be rejected via cancelled cb */
    producer_t overflow;
    submit(&c, &overflow, 999, &FMT_48S, &POL_NORMAL);
    CHECK(overflow.granted == 0, "overflow not granted");
    CHECK(overflow.cancelled == 1, "overflow rejected via cancelled");
}

static void test_generation_recycle(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    producer_t p1;
    submit(&c, &p1, 1, &FMT_48S, &POL_NORMAL);
    audio_token_t old = p1.token;

    audio_action_list_t *a = acts();
    audio_dev_core_finish(&c, p1.token, a);
    run_actions(a);

    producer_t p2;
    submit(&c, &p2, 2, &FMT_48S, &POL_NORMAL);

    CHECK(p2.token.slot == old.slot, "reused the same slot");
    CHECK(p2.token.generation != old.generation, "fresh generation");
    CHECK(!audio_dev_core_token_active(&c, old), "old token dead");
    CHECK(audio_dev_core_token_active(&c, p2.token), "new token live");
}

static void test_bad_tokens(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    audio_token_t zero = { .slot = 0, .generation = 0 };
    audio_token_t oob = { .slot = AUDIO_DEV_MAX_STREAMS, .generation = 1 };
    CHECK(!audio_dev_core_token_active(&c, zero), "gen-0 token never active");
    CHECK(!audio_dev_core_token_active(&c, oob), "out-of-range slot rejected");

    audio_action_list_t *a = acts();
    CHECK(!audio_dev_core_finish(&c, zero, a), "finish gen-0 rejected");
    CHECK(!audio_dev_core_abort(&c, oob, a), "abort oob rejected");
}

static const audio_request_policy_t POL_OVERDUB = {
    .conflict = AUDIO_CONFLICT_OVERDUB, .cancelable = true, .priority = 10
};

static void test_overdub_chord(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    /* Three OVERDUB requests at the same format: the first opens a MIXING
       session, the next two join it -> all three live at once. */
    producer_t p1, p2, p3;
    submit(&c, &p1, 1, &FMT_48S, &POL_OVERDUB);
    CHECK(g_be.mixer_running && g_be.mixer_starts == 1, "first opens mixer");
    CHECK(c.session == AUDIO_SESSION_MIXING, "session is MIXING");
    submit(&c, &p2, 2, &FMT_48S, &POL_OVERDUB);
    submit(&c, &p3, 3, &FMT_48S, &POL_OVERDUB);

    CHECK(p1.granted && p2.granted && p3.granted, "all three granted at once");
    CHECK(g_be.starts == 1, "backend started exactly once for the session");
    CHECK(g_be.mix_sources == 3, "three sources being mixed");
    CHECK(audio_dev_core_token_is_mixing(&c, p1.token), "p1 routes to mixer");
    CHECK(audio_dev_core_token_is_mixing(&c, p3.token), "p3 routes to mixer");
    CHECK(p1.token.slot != p2.token.slot && p2.token.slot != p3.token.slot,
          "distinct slots (concurrent, not reused)");

    /* Finishing two of three keeps the session up (drop a source each). */
    audio_action_list_t *a = acts();
    audio_dev_core_finish(&c, p1.token, a);
    run_actions(a);
    CHECK(c.session == AUDIO_SESSION_MIXING, "still mixing after one finish");
    CHECK(g_be.mixer_running && g_be.mix_sources == 2, "two sources left");
    CHECK(g_be.stops == 0, "backend not stopped mid-session");

    a = acts();
    audio_dev_core_finish(&c, p2.token, a);
    run_actions(a);
    CHECK(g_be.mix_sources == 1, "one source left");

    /* Last finish tears the whole session down. */
    a = acts();
    audio_dev_core_finish(&c, p3.token, a);
    run_actions(a);
    CHECK(!g_be.mixer_running && g_be.mixer_stops == 1, "mixer stopped");
    CHECK(g_be.stops == 1 && !g_be.running, "backend stopped");
    CHECK(c.session == AUDIO_SESSION_IDLE, "idle after last finish");
}

static void test_overdub_fallbacks(void)
{
    audio_dev_core_t c;
    audio_dev_core_init(&c);
    memset(&g_be, 0, sizeof g_be);

    /* OVERDUB onto an EXCLUSIVE session must queue, not join. */
    producer_t ex, od;
    submit(&c, &ex, 1, &FMT_48S, &POL_NORMAL); /* EXCLUSIVE */
    submit(&c, &od, 2, &FMT_48S, &POL_OVERDUB);
    CHECK(od.granted == 0, "OVERDUB queues behind EXCLUSIVE");
    CHECK(g_be.mixer_starts == 0, "no mixer while EXCLUSIVE");

    audio_action_list_t *a = acts();
    audio_dev_core_finish(&c, ex.token, a);
    run_actions(a);
    /* Dispatched from idle, the queued OVERDUB now opens its own session. */
    CHECK(od.granted == 1 && c.session == AUDIO_SESSION_MIXING,
          "queued OVERDUB opens a session when dispatched");
    CHECK(g_be.mixer_running, "mixer up for the dispatched OVERDUB");

    /* A different-format OVERDUB cannot join; it queues. */
    producer_t bad;
    submit(&c, &bad, 3, &FMT_16M, &POL_OVERDUB);
    CHECK(bad.granted == 0, "format-mismatch OVERDUB does not join");
    CHECK(g_be.mix_sources == 1, "still a single source");
}

int main(void)
{
    test_grant_on_idle();
    test_finish_to_idle();
    test_abort_flushes();
    test_queue_priority_and_fifo();
    test_cancel_queued();
    test_queue_full_rejects();
    test_generation_recycle();
    test_bad_tokens();
    test_overdub_chord();
    test_overdub_fallbacks();

    printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
