/*
 * reg_util.h — shared scaffolding for the phase-2 term_registry suites.
 *
 * Reuses the phase-1 CHK_* harness (test_util.h) and adds the registry
 * boilerplate: install the fake port, build an options struct, ask a slot
 * what state it is in, and settle the asynchronous half of the quiesce.
 *
 * Every expectation in the registry suites is derived from
 * docs/term-design.md, term_registry.h and term_port.h. Where the design
 * fixes no number (ring size, snapshot line count, how many bytes a drain
 * takes by default) the tests assert an invariant instead of inventing one.
 */
#ifndef TERM_REG_UTIL_H
#define TERM_REG_UTIL_H

#include "test_util.h"
#include "fake_port.h"
#include "term_registry.h"

/* Two distinct trusted identities (§3.1: owner comes from a signed push). */
#define OWNER_A "app_alpha"
#define OWNER_B "app_beta"

/* ===================================================================== */
/* Boot / teardown                                                       */
/* ===================================================================== */

static inline bool reg_boot_cfg(const term_registry_config_t *cfg)
{
    fp_reset();
    if (!term_port_install(fp_port())) return false;
    return term_registry_init(cfg) == TERM_OK;
}

static inline bool reg_boot(void) { return reg_boot_cfg(NULL); }

/* Deinit and check the port's own bookkeeping balanced out. Returns the
 * number of uncollectable slots term_registry_deinit() reported. */
static inline int reg_shutdown(void)
{
    return term_registry_deinit();
}

/* ===================================================================== */
/* Options                                                               */
/* ===================================================================== */

static inline term_create_opts_t reg_opts(const char *name, const char *owner,
                                          term_mode_t mode, bool persist,
                                          int cols, int rows)
{
    term_create_opts_t o;
    memset(&o, 0, sizeof o);
    o.name = name;
    o.owner = owner;
    o.mode = mode;
    o.persist = persist;
    o.cols = cols;
    o.rows = rows;
    return o;
}

/* Create and return the id, or TERM_ID_INVALID. */
static inline term_id_t reg_new(const char *name, const char *owner,
                                term_mode_t mode, bool persist,
                                int cols, int rows)
{
    term_create_opts_t o = reg_opts(name, owner, mode, persist, cols, rows);
    term_id_t id = TERM_ID_INVALID;
    if (term_registry_create(&o, &id, NULL) != TERM_OK) return TERM_ID_INVALID;
    return id;
}

/* ===================================================================== */
/* Introspection shorthands                                              */
/* ===================================================================== */

/* Slot state, or -1 when the id names nothing the registry will talk about
 * (FREE slot, stale generation, out of range). */
static inline int reg_state(term_id_t id)
{
    term_slot_info_t i;
    memset(&i, 0, sizeof i);
    if (term_registry_info(id, &i) != TERM_OK) return -1;
    return (int)i.state;
}

static inline term_slot_info_t reg_info(term_id_t id)
{
    term_slot_info_t i;
    memset(&i, 0, sizeof i);
    (void)term_registry_info(id, &i);
    return i;
}

static inline term_registry_stats_t reg_stats(void)
{
    term_registry_stats_t s;
    memset(&s, 0, sizeof s);
    term_registry_stats(&s);
    return s;
}

static inline int reg_live_count(void)
{
    term_slot_info_t v[TERM_SLOT_COUNT];
    return term_registry_list(v, TERM_SLOT_COUNT);
}

/* ===================================================================== */
/* Turning the crank                                                     */
/* ===================================================================== */

/* The renderer half of a frame. §9/term_registry.h: the ui_visit callback
 * is where the blitter lives, and "the renderer, and only the renderer,
 * clears damage" — so a test that never paints would leave every visible
 * term permanently dirty. */
static inline void reg_paint_cb(term_id_t id, term_core_t *core,
                                const term_view_t *view, void *user)
{
    (void)id; (void)view;
    if (user) (*(int *)user) += 1;
    if (core) term_core_dirty_clear(core);
}

static inline int reg_paint(void)
{
    return term_registry_ui_visit(reg_paint_cb, NULL);
}

/* One "frame": run whatever the UI task owes (queued jobs, the drain, the
 * blit). §3.1 puts the reaper on a task that is neither the UI task nor
 * js_task; ordering between the two is not part of the contract, so
 * settle() runs both a few times. */
static inline void reg_frame(void)
{
    fp_pump();
    term_registry_ui_drain();
    reg_paint();
    fp_pump();
}

static inline void reg_settle(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        reg_frame();
        term_registry_reap();
    }
}

/* ===================================================================== */
/* Reads                                                                 */
/* ===================================================================== */

#define REG_SNAP_CAP 8192

/* Snapshot into a rotating static buffer; returns "" and records nothing
 * when the call fails, so a caller can CHK_STR the text and separately
 * CHK_INT the error code. */
static inline const char *reg_snap_rc(term_id_t id, const char *owner,
                                      term_err_t *out_rc)
{
    static char buf[2][REG_SNAP_CAP];
    static int which = 0;
    term_err_t rc;
    which ^= 1;
    memset(buf[which], 0, REG_SNAP_CAP);
    rc = term_registry_snapshot(id, owner, buf[which], REG_SNAP_CAP, NULL);
    if (out_rc) *out_rc = rc;
    if (rc != TERM_OK) buf[which][0] = 0;
    return buf[which];
}

static inline const char *reg_snap(term_id_t id, const char *owner)
{
    return reg_snap_rc(id, owner, NULL);
}

/* First '\n'-delimited line of a snapshot, in a static buffer. */
static inline const char *reg_snap_line0(term_id_t id, const char *owner)
{
    static char line[1024];
    const char *s = reg_snap(id, owner);
    size_t i = 0;
    while (s[i] && s[i] != '\n' && i + 1 < sizeof line) { line[i] = s[i]; i++; }
    line[i] = 0;
    return line;
}

/* ===================================================================== */
/* Owner-gate sweep                                                      */
/* ===================================================================== */

/*
 * I3 (§3.1): "全 API(read 系だけでなく feed/log/pipe/resize/close の書き込み
 * 系も)が呼び出しごとに generation 一致 + owner 一致 を検証し、不一致は
 * 即エラー。" One helper drives every owner-gated entry point so a suite
 * cannot quietly forget one.
 */
static inline void reg_noop_detach(void *user, term_id_t id) { (void)user; (void)id; }

static inline void reg_expect_all_gated(term_id_t id, const char *owner,
                                        term_err_t expect)
{
    char out[256];
    term_view_t view;
    size_t wrote = 12345;
    term_read_result_t res;

    memset(&view, 0, sizeof view);
    view.w = 10; view.h = 10; view.visible = true;
    memset(&res, 0, sizeof res);
    memset(out, 0x7e, sizeof out);

    CHK_INT(term_registry_feed(id, owner, (const uint8_t *)"x", 1, &wrote), expect);
    CHK_INT(term_registry_log(id, owner, "x", 1), expect);
    CHK_INT(term_registry_resize(id, owner, 20, 5), expect);
    CHK_INT(term_registry_show(id, owner, &view), expect);
    CHK_INT(term_registry_snapshot(id, owner, out, sizeof out, NULL), expect);
    CHK_INT(term_registry_read(id, owner, 0, 4, out, sizeof out, &res), expect);
    CHK_INT(term_registry_producer_bind(id, owner, reg_noop_detach, NULL), expect);
    CHK_INT(term_registry_producer_unbind(id, owner), expect);
    CHK_INT(term_registry_close(id, owner), expect);
}

#endif /* TERM_REG_UTIL_H */
