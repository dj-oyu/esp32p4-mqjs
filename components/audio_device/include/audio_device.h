/*
 * audio_device: token-based arbitration over the single Tab5 speaker
 * (P1 of docs/audio-device-design.md). Producers (tone, WAV, future Opus
 * / JS) ask for a stream with audio_request(); the manager task grants a
 * capability token via callback when the device is free, queueing the rest
 * by priority. Only the live token may write PCM; finish/abort revoke it
 * and dispatch the next waiter. The physical PCM path stays in audio_tab5;
 * this layer owns ownership, the request queue, and revocation.
 *
 * Threading contract:
 *   - audio_request / audio_request_cancel / audio_stream_finish /
 *     audio_stream_abort are non-blocking: they post to the manager task.
 *   - on_granted / on_cancelled fire on the manager task, never on the
 *     caller's stack and never with a device lock held.
 *   - audio_stream_write runs on the producer's own task (hot path); it
 *     validates the token and forwards to audio_tab5 without going through
 *     the manager task.
 *
 * All entry points are no-op stubs unless CONFIG_MQJS_TAB5_AUDIO=y.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "audio_device_core.h" /* audio_format_t, policy, token, cb types */

#if CONFIG_MQJS_TAB5_AUDIO

/* Idempotent. Brings up the manager task + command queue. Called lazily by
 * audio_request(), but may be invoked explicitly at boot. */
esp_err_t audio_device_init(void);

/* Ask for a stream. Returns a request id (>0) the caller can cancel, or 0
 * if the request could not even be submitted (out of memory / queue down).
 * on_granted fires with the token when the device becomes available;
 * on_cancelled fires if the request is withdrawn, rejected (queue full), or
 * (P2) preempted. Either callback may be NULL. arg is passed back verbatim. */
audio_request_id_t audio_request(const audio_format_t *format,
                                 const audio_request_policy_t *policy,
                                 audio_granted_cb_t on_granted,
                                 audio_cancelled_cb_t on_cancelled, void *arg);

/* Withdraw a still-queued request. Best-effort/async: true means the cancel
 * was posted; the actual removal arrives as an on_cancelled callback. A
 * request already granted cannot be cancelled this way — finish/abort it. */
bool audio_request_cancel(audio_request_id_t request);

/* Queue interleaved s16 PCM for the granted stream. Blocks up to timeout_ms
 * for ring space. On success returns ESP_OK and (if accepted != NULL) the
 * number of frames taken (backpressure < frames is normal). A stale/unknown
 * token is rejected with ESP_ERR_INVALID_STATE and consumes no PCM. */
esp_err_t audio_stream_write(audio_token_t token, const int16_t *pcm,
                             size_t frames, uint32_t timeout_ms,
                             size_t *accepted);

/* Normal completion: drain the remaining PCM, then revoke the token and
 * dispatch the next waiter. ESP_ERR_INVALID_STATE for a stale token. */
esp_err_t audio_stream_finish(audio_token_t token);

/* Owner's immediate stop: discard pending PCM, revoke, dispatch next.
 * ESP_ERR_INVALID_STATE for a stale token. */
esp_err_t audio_stream_abort(audio_token_t token);

/* True while the token is the live, writable stream. */
bool audio_token_valid(audio_token_t token);

/* Spawn a one-shot task that drives two overlapping requests through the
 * token API (grant + queue + dispatch) and beeps once per grant. Wired into
 * app_main behind CONFIG_MQJS_TAB5_AUDIO_DEVICE_SELFTEST. */
void audio_device_selftest_async(void);

#else /* stubs — same pattern as audio_tab5.h / ui_tab5.h */

static inline void audio_device_selftest_async(void) {}

static inline esp_err_t audio_device_init(void) { return ESP_OK; }
static inline audio_request_id_t audio_request(
    const audio_format_t *format, const audio_request_policy_t *policy,
    audio_granted_cb_t on_granted, audio_cancelled_cb_t on_cancelled, void *arg)
{
    (void)format;
    (void)policy;
    (void)on_granted;
    (void)on_cancelled;
    (void)arg;
    return 0;
}
static inline bool audio_request_cancel(audio_request_id_t request)
{
    (void)request;
    return false;
}
static inline esp_err_t audio_stream_write(audio_token_t token,
                                           const int16_t *pcm, size_t frames,
                                           uint32_t timeout_ms, size_t *accepted)
{
    (void)token;
    (void)pcm;
    (void)frames;
    (void)timeout_ms;
    if (accepted)
        *accepted = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_stream_finish(audio_token_t token)
{
    (void)token;
    return ESP_OK;
}
static inline esp_err_t audio_stream_abort(audio_token_t token)
{
    (void)token;
    return ESP_OK;
}
static inline bool audio_token_valid(audio_token_t token)
{
    (void)token;
    return false;
}

#endif /* CONFIG_MQJS_TAB5_AUDIO */
