/*
 * term_pipe — the producer behind term.pipe (docs/term-design.md §5, §8).
 *
 * term_registry.h defines what a producer IS (a single C-side writer that
 * owns a term's byte ring, answers its replies and acks its detach). This
 * header is the one implementation of that role which JS can ask for: an
 * ssh channel. `term.pipe(id, sshHandle)` lands here.
 *
 *   ssh rx task ──► term ring ──► [UI drain] parse ──► grid
 *   grid replies ──► ssh tx
 *
 * Neither direction touches the JS heap; the app orchestrates connect and
 * tab switching and nothing else (§5, §9.1).
 *
 * ---------------------------------------------------------------------
 * TWO IMPLEMENTATIONS, ONE CONTRACT
 * ---------------------------------------------------------------------
 * On the device `handle` is an sshc session id and the glue installs an
 * sshc_sink_t on it (sshc.h). Off the device there is no ssh, so the
 * handle is ignored and the bind installs a STUB producer: it never
 * produces a byte, but it is a real producer as far as the registry is
 * concerned — it makes the term piped, so feed/log answer TERM_ERR_BUSY,
 * and it acks its detach one term_pipe_pump() later rather than instantly,
 * which is what makes the re-pipe rule (§5) observable from a host test or
 * from run_pc. That deferral is deliberate: an ack that arrived inside the
 * detach call would hide the very window the protocol exists for.
 *
 * The stub is the reason this file is not #ifdef'd away off-device.
 */
#pragma once

#include "term_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * term.pipe(id, handle). Binds the term's single producer to `handle`
 * (an ssh session id on the device; ignored by the host stub).
 *
 * Returns what term_registry_pipe() returns, so all of its rules apply
 * unchanged — owner gate, TERM_ERR_MODE for a non-VT term, and
 * TERM_ERR_BUSY when a producer was already bound (in which case that
 * producer has just been asked to detach and a retry after its ack
 * succeeds; §5, and term_registry.h's term_registry_pipe).
 *
 * Additional failures of this layer:
 *   TERM_ERR_NO_SLOT  no free pipe context (one per term slot)
 *   TERM_ERR_BAD_ID   the handle names no live session
 *
 * A pipe outlives neither side: when the ssh session ends, the producer
 * acks and the term becomes unpiped (feed/log work again). The app learns
 * about it the way it already does, through ssh.onClose.
 */
term_err_t term_pipe_bind(term_id_t id, const char *owner, int handle);

/*
 * term.unpipe(id). Requests the producer's detach and returns
 * immediately — §3.1 stage 1 semantics, no join. The term stops being
 * piped when the ack lands, which for the ssh producer is one recv
 * timeout away.
 */
term_err_t term_pipe_unbind(term_id_t id, const char *owner);

/*
 * Give the host stub its "own task" moment: this is where a requested
 * detach turns into term_registry_producer_ack(). Called from the mqjs
 * scheduler loop next to term_registry_ui_drain(). NO-OP ON THE DEVICE,
 * where the ssh session task does the same work from the right thread.
 */
void term_pipe_pump(void);

/* How many pipe contexts are in use. Introspection for tests and probes;
 * never a gate on anything. */
int term_pipe_count(void);

#ifdef __cplusplus
}
#endif
