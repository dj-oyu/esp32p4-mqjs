/*
 * term_bb_pull — the signed black-box pull (docs/term-design.md §7.2/§7.3,
 * phase 3 §3 of §11).
 *
 * §4.4 ends with "再起動後、magic+CRC 検証済みの前ブート分を署名付き pull で
 * 読める (`lastboot`)". This file is that pull: a request arrives over MQTT,
 * is verified against the SAME Ed25519 key that signs app pushes
 * (main/task_pubkey.h — there is one trust root on this device and this file
 * does not add a second), and the device answers with black-box content on a
 * reply topic, chunked.
 *
 * WHAT IS IN THIS FILE AND WHAT IS NOT. Everything here is pure C99 over
 * caller-supplied callbacks and a caller-supplied buffer: no MQTT, no NVS, no
 * FreeRTOS, no TweetNaCl, no allocation. That is deliberate and it is the
 * whole testing strategy — term_bb_serve() is the complete responder,
 * including the replay gate and the chunk loop, and a host test drives it with
 * four function pointers. The device glue (subscribe on task_source's existing
 * client, crypto_sign_open with the embedded pubkey, the NVS high-water mark,
 * esp_mqtt_client_publish) lives in main/task_source.c, which is where the
 * MQTT connection is owned. No second broker connection exists.
 *
 * =====================================================================
 * THE FIVE PROPERTIES
 * =====================================================================
 *
 * Q1. NOTHING OUTSIDE THE SIGNATURE IS TRUSTED. The envelope is
 *     signature(64) || message, byte for byte the format task_source.c
 *     already verifies for app pushes, so tools/mqjs_push.py's signing code
 *     is reused unchanged in tools/bb_pull.py. The topic a request arrived on
 *     is NOT authenticated by MQTT, so the message carries `top=` and the
 *     device compares it against its own task topic (Q3).
 *
 * Q2. REPLAY IS REFUSED BY A MONOTONIC COUNTER WITH THE HIGH-WATER MARK IN
 *     NVS. §7.2 pins this mechanism: "署名リクエストは単調カウンタを含めて
 *     署名し、デバイスは最後に受理した値より大きいもののみ通す ... 受理済み
 *     カウンタの高水準マークは NVS に保存する — LP SRAM には置かない". The
 *     threat is concrete and is why the gate exists at all: the reply is
 *     plaintext on the broker, so a passive subscriber sees both the request
 *     and the log. Without a counter that observer could re-publish the
 *     captured request tomorrow and pull a FUTURE boot's class-A log — the
 *     one thing §7.2's gate is supposed to make impossible for a non-key
 *     holder. The mark must survive a power cut, hence NVS and not the LP
 *     region this component otherwise lives in (LP is lost on power-off,
 *     §4.4, so one power cycle would reopen the whole window).
 *
 *     The mark is stored BEFORE the first reply byte is published
 *     (term_bb_serve() calls hwm_store() and gives up if it fails). A crash
 *     mid-reply therefore burns the counter rather than leaving it reusable,
 *     and the operator's remedy is the next counter value — which costs
 *     nothing, because the counter is wall-clock milliseconds.
 *
 * Q3. A REQUEST IS BOUND TO ONE DEVICE'S TOPIC. `top=` must equal the
 *     device's own task topic exactly. Without it, a captured request could
 *     be re-published on a second device's topic, where the counter's
 *     high-water mark is independent and probably lower — replay across
 *     devices instead of across time. This project already gives every
 *     device its own task topic (CONFIG_MQJS_TASK_TOPIC ends in a random
 *     suffix), which is what makes the check meaningful; two devices
 *     deliberately sharing one topic share the weakness, and the manifest
 *     says so.
 *
 * Q4. AN UNKNOWN FIELD IS A REFUSAL, NOT AN IGNORED LINE. A signed request
 *     containing `redact=1` came from someone who believed the device would
 *     redact. Silently answering with everything would be the worst possible
 *     reading of the sender's intent, so the parser rejects any key it does
 *     not implement (and any duplicate key, and any trailing junk). The same
 *     rule as term_registry's "reject, never truncate".
 *
 * Q5. THE RESPONDER ADDS NO STATE TO THE RING AND NEVER WRITES TO IT. It is
 *     a consumer of term_lp_ring.h's read API (term_lp_report,
 *     term_lp_dump_json_ex, and through them term_lp_iter_*). Reads do not
 *     mutate, so a pull cannot change what a later pull sees (§7.2: "読み出し
 *     は無変異 — probe が測定対象を変えない").
 *
 * =====================================================================
 * WIRE FORMAT — REQUEST
 * =====================================================================
 *
 * Topic:    <CONFIG_MQJS_TASK_TOPIC>/bb        (QoS 1 subscribe, not retained)
 * Payload:  Ed25519 signature (64 bytes) || message (ASCII, <= 512 bytes)
 *
 * The message is a magic line followed by `key=value` lines, LF-separated, a
 * trailing LF optional and a trailing CR per line tolerated:
 *
 *     bbpull1
 *     ctr=1753900000123
 *     top=esp32p4-mqjs/task/u7q3x9f2
 *     what=lastboot
 *     from=0
 *
 *   ctr   REQUIRED, decimal, 1 .. 2^63-1. Must be strictly greater than the
 *         device's stored high-water mark (Q2). tools/bb_pull.py uses
 *         wall-clock milliseconds, which is monotonic across tool runs and
 *         across machines with sane clocks, and needs no state on either
 *         side. A device pulled from two machines answers whichever request
 *         has the larger clock; the other must retry with a bigger number
 *         (the tool's --ctr does that by hand).
 *   top   REQUIRED, the device's task topic, compared byte for byte (Q3).
 *   what  REQUIRED, one of:
 *           stats     — term_lp_report(), one chunk, no content
 *           lastboot  — the frozen previous session (the forensic answer)
 *           live      — this session's tail
 *   from  OPTIONAL, default 0. Skips that many records, i.e. resumes a pull
 *         (the same cursor `next` in every reply chunk).
 *
 * Anything else — a missing required key, an unknown key, a duplicate key, a
 * non-decimal number, an overlong message, ctr=0 — is refused (Q4), and the
 * reason is published as one short line on <task topic>/status, the reply
 * convention task_source.c already uses ("bb: rejected (replay)").
 *
 * The two length refusals, by code, because a size is not a syntax error:
 *
 *   - a message longer than TERM_BB_REQ_MAX (and an envelope longer than
 *     TERM_BB_ENVELOPE_MAX, or one no longer than the signature) is
 *     TERM_BB_E_SIZE, "bad-length". Exactly TERM_BB_REQ_MAX bytes is legal.
 *   - a `top=` value too long to store whole — TERM_BB_TOPIC_MAX - 1 bytes is
 *     the most that fits — is TERM_BB_E_RANGE, "out-of-range", the same code as
 *     a number that does not fit. It is a value that exceeds its field, not
 *     malformed syntax, and the point is that it is never clipped to a prefix
 *     and then compared.
 *
 * =====================================================================
 * WIRE FORMAT — REPLY
 * =====================================================================
 *
 * Topic: <CONFIG_MQJS_TASK_TOPIC>/bb/reply, one MQTT message per chunk,
 * QoS 0, not retained. Each chunk is a self-contained JSON object:
 *
 *   {"bb":1,"ctr":1753900000123,"what":"lastboot","seq":0,"from":0,
 *    "body":{"src":"lastboot","ok":1,"from":0,"lines":[...],
 *            "records":12,"next":12,"more":1},
 *    "last":0}
 *
 *   ctr    echoes the request's counter: the correlation id (it is unique by
 *          construction, so no separate request id is invented).
 *   seq    0-based chunk index. A gap means a lost QoS 0 chunk — the reader
 *          can see it and re-pull from the last good `next`.
 *   from   the record cursor this chunk started at; `body.next` is the cursor
 *          for the following one.
 *   last   1 on the final chunk. The reader reassembles by concatenating
 *          `body.lines` in `seq` order until it sees it.
 *   body   for what=stats, term_lp_report()'s object; otherwise
 *          term_lp_dump_json()'s object, unchanged — the wire format is the
 *          one the JS surface already emits and the host suites already
 *          cover.
 *
 * Two honesty flags appear only when they apply, and both also force
 * "last":1 because the responder stops:
 *
 *   "stall":1  the buffer could not hold even one rendered line, so the pull
 *              cannot make progress (term_lp_ring.h: advance on records > 0,
 *              not on `more` alone).
 *   "cut":1    TERM_BB_CHUNKS_MAX chunks were published and records remained.
 *              A bound is required: the responder runs on the MQTT event task
 *              and must not be turnable into an unbounded publish loop.
 *
 * WHY JSON AND NOT RAW TEXT. The black box is text and it is escape-stripped
 * (term_lp_ring.h P4), so raw text would print fine — but chunk framing needs
 * structure anyway, and reusing term_lp_dump_json() means the responder adds
 * no second serialiser, no second quoting rule, and nothing new to test.
 *
 * WHY QoS 0 FOR THE REPLY. esp-mqtt's publish with QoS > 0 waits for an ack
 * that is processed by the very task this code runs on (the MQTT event task),
 * which is the deadlock esp-mqtt's own documentation warns about, and
 * task_source.c's existing publish_status() is QoS 0 for the same reason. The
 * chunks are therefore lossy in principle; `seq` makes loss VISIBLE and
 * `from` makes it recoverable, which for a LAN broker is the right trade. A
 * black box that is one re-pull away from complete beats a responder that
 * hangs the connection.
 *
 * =====================================================================
 * WHERE THIS RUNS, AND WHY THAT IS SAFE
 * =====================================================================
 *
 * On the MQTT event task, from task_source.c's MQTT_EVENT_DATA, exactly like
 * the app-push path next to it: that task already performs Ed25519
 * verification, LittleFS writes and QoS 0 publishes, and it is not
 * tcpip_thread. The work is bounded — one verify, one NVS commit, at most
 * TERM_BB_CHUNKS_MAX small publishes — and pulls are a human-driven,
 * once-in-a-crash operation, so no separate task and no queue is added. The
 * project's rule that a second owner of a resource is a bug points the same
 * way: task_source.c owns this connection.
 *
 * The one race worth stating: against TERM_LP_SRC_LIVE the ring is being
 * appended to by other tasks while this reads it, and term_registry does not
 * expose its table mutex, so a live pull may render a torn tail line. It
 * cannot fault (term_lp_iter_next bounds every walk by the header's own
 * `used`/`cap` and stops on an implausible record), and it is why forensics
 * pull `lastboot`, whose snapshot is immutable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Wire constants                                                        */
/* ===================================================================== */

/* First line of a request message. The version digit is part of it: a future
 * format is a different magic, never a "compatible" extension (Q4). */
#define TERM_BB_MAGIC        "bbpull1"

#define TERM_BB_SIG_LEN      64u    /* Ed25519, as task_source.c            */
#define TERM_BB_REQ_MAX      512u   /* message bytes after the signature    */
#define TERM_BB_ENVELOPE_MAX (TERM_BB_SIG_LEN + TERM_BB_REQ_MAX)

/* Room for CONFIG_MQJS_TASK_TOPIC plus its NUL. The shipped topic is 27
 * bytes; a topic that does not fit here can never match, so a device
 * configured with a longer one refuses every request loudly (TERM_BB_E_RANGE
 * from the parser, before the topic is ever compared) rather than comparing a
 * prefix. */
#define TERM_BB_TOPIC_MAX    128u

/* Smallest scratch buffer term_bb_serve() accepts. It must hold the whole
 * envelope (the signature is opened into it before it is reused for chunks)
 * and one reply chunk with at least one rendered line. A device should pass
 * more — see TERM_BB_SCRATCH_WANT — because chunk count scales inversely.
 *
 * This floor also has to leave room for a WHOLE `stats` body, because
 * term_lp_report() degrades to {"ready":1,"error":"truncated"} rather than
 * emitting half a document. Today's report is ~427 bytes and this floor leaves
 * ~610 for it; a test pins the relationship (a `stats` pull at exactly
 * TERM_BB_SCRATCH_MIN must succeed AND must not come back "truncated"), so a
 * report that grows past the margin fails a check here instead of on a
 * device. */
#define TERM_BB_SCRATCH_MIN  (TERM_BB_ENVELOPE_MAX + 128u)   /* 704 */

/* What the device actually passes: the largest reply payload that fits
 * task_source.c's 2048-byte esp-mqtt OUT buffer with room for the fixed
 * header, the topic and slack. Do not raise this without raising
 * TX_BUF_SIZE — esp-mqtt would fragment or fail the publish. */
#define TERM_BB_SCRATCH_WANT 1600u

/* Chunk bound (the "cut":1 flag). 31 KB of stripped text at ~1.5 KB of JSON
 * per chunk is ~25; 64 leaves room for a pathological ring of tiny records
 * without letting a single request publish forever. */
#define TERM_BB_CHUNKS_MAX   64u

/* ===================================================================== */
/* Request                                                               */
/* ===================================================================== */

typedef enum {
    TERM_BB_WHAT_STATS    = 0,  /* term_lp_report(), one chunk             */
    TERM_BB_WHAT_LASTBOOT = 1,
    TERM_BB_WHAT_LIVE     = 2,
} term_bb_what_t;

typedef struct {
    uint64_t       ctr;
    term_bb_what_t what;
    uint32_t       from;
    char           topic[TERM_BB_TOPIC_MAX];  /* NUL-terminated copy of top= */
} term_bb_req_t;

/*
 * Every way a request can be refused. One code per reason on purpose: the
 * device publishes the name on <task topic>/status and an operator debugging
 * a failed pull should not have to guess whether the signature or the counter
 * was the problem (neither leaks anything a key holder does not know).
 */
typedef enum {
    TERM_BB_OK = 0,
    TERM_BB_E_SIZE,     /* envelope shorter than a signature, or too long   */
    TERM_BB_E_SIG,      /* Ed25519 verification failed                      */
    TERM_BB_E_MAGIC,    /* first line is not TERM_BB_MAGIC                  */
    TERM_BB_E_SYNTAX,   /* not key=value, empty key/value, non-decimal, NUL */
    TERM_BB_E_UNKNOWN,  /* a key this version does not implement (Q4)       */
    TERM_BB_E_DUP,      /* the same key twice                               */
    TERM_BB_E_MISSING,  /* ctr, top or what absent                          */
    TERM_BB_E_RANGE,    /* ctr == 0, or a number that does not fit          */
    TERM_BB_E_TOPIC,    /* top= is not this device's topic (Q3)             */
    TERM_BB_E_REPLAY,   /* ctr <= the stored high-water mark (Q2)           */
    TERM_BB_E_HWM,      /* the mark could not be read or persisted          */
    TERM_BB_E_ENV,      /* misconfigured env: no callback, scratch too small */
    TERM_BB_E_PUBLISH,  /* a reply chunk could not be published             */
} term_bb_err_t;

/* Short, stable, lower-case names ("replay", "bad-signature", ...). Safe for
 * any code including out-of-range ones ("bad-code"). */
const char *term_bb_err_str(term_bb_err_t e);

/*
 * Parse a request MESSAGE — the plaintext, i.e. what the signature covers,
 * with the 64-byte signature already stripped and verified. `msg` need not be
 * NUL-terminated; `len` is authoritative and an embedded NUL is TERM_BB_E_SYNTAX.
 *
 * `out` is fully overwritten on success and left zeroed on failure, so a
 * caller cannot accidentally act on half a request. Optional fields carry
 * their defaults (from = 0).
 */
term_bb_err_t term_bb_req_parse(const char *msg, size_t len,
                                term_bb_req_t *out);

/*
 * The two checks that need device context, split out so a test can drive them
 * independently of parsing: `req->topic` must equal `my_topic` (Q3) and
 * `req->ctr` must be strictly greater than `hwm` (Q2). Order is topic first —
 * a request meant for another device must not consume this one's counter
 * space, and it is not a replay attempt.
 */
term_bb_err_t term_bb_req_gate(const term_bb_req_t *req, const char *my_topic,
                               uint64_t hwm);

/* ===================================================================== */
/* The environment (the device seam — four function pointers)             */
/* ===================================================================== */

/*
 * Open an Ed25519 envelope. EXACTLY crypto_sign_open()'s contract, which is
 * what the device passes: `sm`/`smlen` is signature||message, `m` receives
 * the message and must have room for `smlen` bytes, `*mlen` receives its
 * length, and 0 means verified. The public key is the callback's business —
 * that is how this file stays free of TweetNaCl and how the device keeps its
 * ONE trust root (main/task_pubkey.h) in one place.
 */
typedef int (*term_bb_verify_fn)(void *ctx, unsigned char *m,
                                 unsigned long long *mlen,
                                 const unsigned char *sm,
                                 unsigned long long smlen);

/* Read the replay high-water mark. false = could not read (fail closed:
 * term_bb_serve returns TERM_BB_E_HWM and answers nothing). An absent store
 * is not a failure — report true with *out = 0. */
typedef bool (*term_bb_hwm_load_fn)(void *ctx, uint64_t *out);

/* Persist the mark DURABLY before returning (on the device: nvs_set + a
 * synchronous nvs_commit, as §7.2 requires — "NVS の遅延 commit に期待値を
 * 置かない"). false = not stored, and the request is then refused. */
typedef bool (*term_bb_hwm_store_fn)(void *ctx, uint64_t v);

/* Publish one reply chunk. `json` is NUL-terminated and `len` excludes the
 * NUL. false aborts the pull with TERM_BB_E_PUBLISH. */
typedef bool (*term_bb_publish_fn)(void *ctx, const char *json, size_t len);

typedef struct {
    term_bb_verify_fn    verify;
    term_bb_hwm_load_fn  hwm_load;
    term_bb_hwm_store_fn hwm_store;
    term_bb_publish_fn   publish;
    void                *ctx;      /* passed to all four                    */
    const char          *topic;    /* this device's task topic (Q3)         */
    char                *scratch;  /* >= TERM_BB_SCRATCH_MIN, caller-owned  */
    size_t               scratch_size;
} term_bb_env_t;

/* What term_bb_serve() did, for the caller's log line and for tests. Always
 * written (zeroed first), whatever the return code. */
typedef struct {
    bool          parsed;    /* req below is meaningful                     */
    bool          accepted;  /* the gate passed and the mark was stored     */
    term_bb_req_t req;
    uint32_t      chunks;    /* reply chunks published                      */
    uint32_t      records;   /* content records rendered in total           */
    uint64_t      hwm;       /* the mark in force after this call — 0 until
                                the mark has been loaded, so a refusal that
                                happens before hwm_load (E_ENV, E_SIZE, E_SIG
                                and every parse error) leaves it 0 rather than
                                reporting the stored mark                    */
} term_bb_result_t;

/*
 * THE RESPONDER. Verify, parse, gate, burn the counter, publish the reply.
 *
 * `payload`/`len` is the raw MQTT payload: signature(64) || message. Returns
 * TERM_BB_OK only when the request was accepted AND every chunk published.
 * `res` may be NULL.
 *
 * Order of operations, and it is contract because each step protects the
 * next:
 *
 *   1. env sanity (callbacks, scratch size)          -> E_ENV
 *   2. length bounds                                 -> E_SIZE
 *   3. signature verified into `scratch`             -> E_SIG
 *   4. parse                                         -> E_MAGIC/E_SYNTAX/...
 *   5. topic bind, then the counter                  -> E_TOPIC / E_REPLAY
 *   6. hwm_store(ctr), durably                       -> E_HWM
 *   7. chunk loop: publish until `last`              -> E_PUBLISH
 *
 * Nothing is published before step 6 succeeds, and the message is parsed only
 * after the signature verified — so an unauthenticated payload reaches
 * nothing but a length check and TweetNaCl.
 *
 * Not reentrant with itself against one `env` (it writes `scratch`); on the
 * device the single MQTT event task provides that serialisation. Performs no
 * allocation.
 */
term_bb_err_t term_bb_serve(const term_bb_env_t *env, const void *payload,
                            size_t len, term_bb_result_t *res);

#ifdef __cplusplus
}
#endif
