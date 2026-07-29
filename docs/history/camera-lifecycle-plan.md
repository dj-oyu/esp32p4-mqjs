# Camera pipeline lifecycle redesign (QR / barcode) — design plan

Status: **closed** — this is the record, not a live plan. Phase 1 (single-owner
camera task + microlink mutual exclusion) is device-verified and on `main`
(`7b76ffd`); the single-owner design it describes is the shipping architecture.
Of Phase 2, the **QR reticle window + native-res decode crop landed**
(`fd98fac`), while **internal-SRAM quirc was proven infeasible on this board**
and is not coming back — see the note in §6. Supersedes the ad-hoc `cam_tab5`
scan path. Companion to the `tailscale_adapter` single-owner lifecycle refactor
(commit `aa58c85`), whose pattern this mirrors.

## Phase 1 — what landed

- **Single-owner camera task.** `cam_tab5.c` now has a resident `cam_owner_task`
  + `cam_cmd_queue` (`cam_scan_req_t`, depth 1). `scan_start` posts a request
  instead of spawning a self-deleting task; a SCAN while busy is rejected at post
  time (public API returns 0), never queued. `cam_run_scan()` returns; the owner
  loops. Cancel stays the atomic `s_cancel` flag (§7), not a command.
- **Camera ↔ network mutual exclusion.** `cam_tab5_set_net_hooks(suspend,
  resume)` keeps cam_tab5 network-agnostic; `main/app_main.c` registers
  `tailscale_adapter_suspend/resume`. `tailscale_adapter` gained SUSPEND/RESUME
  lifecycle commands + an `s_suspended` flag that inhibits every auto-start path
  (START handler, `begin_connect`, watchdog, on-net-up, on-time-synced). suspend()
  posts SUSPEND and **blocks (≤8s) on a stop-completion semaphore** — the
  cross-owner handshake (§3). The persisted enabled flag / crash guard are
  untouched; one-way dependency (camera waits on the adapter, never the reverse).
  Phase 1 suspends **microlink for both modes** (the measured heavy contender);
  the QR-only full Wi-Fi-off (§4/§8) is deferred to Phase 2.
- **Ownership ledger.** `cam_ledger_t {net, dismiss_cb, canvas}` records exactly
  what each scan acquired; teardown releases the same set on **every** exit path
  (found / timeout / cancel / init-failure) in the §12 completion order: decode
  drain → UI dismiss → network resume → busy release → result callback.
- **Bounded ownership (§2.6).** `VIDIOC_S_DQBUF_TIMEOUT` (500 ms) makes DQBUF
  return so the loop re-checks the deadline/cancel — the old unbounded DQBUF hung
  113 s past the 45 s deadline. A consecutive-empty-wait cap (≈6 s) bails on a
  dead stream before the full deadline.
- **Kept as-is (Phase 2 / §9 open items):** esp_video stays acquire-once
  (no re-REQBUFS) and **keeps streaming between scans** (per-scan STREAMOFF is a
  measured item); QR still decodes the full-res 800×600 crop in PSRAM (no reticle
  window, no internal-SRAM quirc yet).

Verify list for the device: scan while a tailnet session is connected →
microlink suspends → fps recovers (was 0.2) → resumes after with no leak, for
both barcode and QR; repeated found/timeout/cancel cycles return to idle; no
heap/largest-block downward trend across cycles (MEAS line).

---

## 1. Motivation — the camera is unusable while Tailscale is connected

Device measurement (Tab5, QR scan, Tailscale/microlink connected, zero-copy WG):

```
MEAS scan-end: timeout (no code)
  [pv 547 ms/f, gray 17340 ms/run, id(quirc) 26271 ms/run, candidates 0, runs 2]
  29 frames  0.2 fps  in 112815 ms
  INT free=76459 largest=31744 | DMA free=36895 largest=19456 | PSRAM free=15.1MB
first DQBUF frame: 13.2 s after "scanning QR"
task_wdt: ml_derp_tx ×14 (CPU0), ml_wg_mgr ×7 (CPU1)
```

- **0.2 fps**; quirc `identify` took **26 s** vs the normal ~127 ms (~200× slower).
- **13.2 s** to the first frame.
- Symptom the user sees: "noise + long wait + unbelievable low fps". The "noise"
  is the viewfinder stuck on the first few unconverged (AWB) frames; it is a
  *symptom* of frame starvation, not (only) a cold-start issue.

### Root cause — priority/core starvation (measured, not theorized)

| Task | Core | Prio |
|------|------|------|
| `ml_net_io`  | 0 | 7 |
| `ml_derp_tx` | 0 | 5 |
| **`scan_task` (camera)** | **0** | **4** ← lowest |
| `ml_coord`   | 1 | 5 |
| `ml_wg_mgr`  | 1 | 7 |

`scan_task` is the lowest-priority task on core 0. When microlink has DERP
traffic, `ml_net_io`(7) and `ml_derp_tx`(5) preempt it indefinitely → the camera
never gets CPU → `VIDIOC_DQBUF` starves (one frame in 13 s, then the loop hangs
in DQBUF and never reaches its deadline check). Both microlink tasks also trip
the task watchdog. The MIPI-CSI→ISP→PSRAM DMA (~57 MB/s) and microlink's
zero-copy WG decrypt (now in `tcpip_thread`) additionally contend for PSRAM/AXI
bandwidth.

Conclusion: **camera and microlink/Wi-Fi must be mutually exclusive**, and the
camera lifecycle must be a clean, leak-proof, on-demand owner — not the current
spawn-a-task-per-scan path that also leaves the pipeline streaming forever (the
boot-probe regression we already removed).

---

## 2. Design goals

1. **On-demand activity**: nothing camera-related runs at boot, and no continuous
   CSI/ISP DMA runs between scans. Any persistent buffers or driver state must be
   measured and justified by the esp_video lifecycle constraint.
2. **QR / barcode modes**, each with its own resource policy (§4).
3. **Auditable resource release**: acquisition and release are paired and every
   exit path is observable in measurement. The exact mechanism is intentionally
   left open until the lifecycle experiments below establish what the drivers
   and worker tasks actually guarantee. (Past bug: a one-shot boot probe left
   `STREAMON` running forever.)
4. **Camera ↔ network mutual exclusion** to kill the contention.
5. Single owner serializes all transitions (no scan-vs-cancel / double-start
   races).
6. **Bounded ownership** (requirement, not a tuning knob): the owner must never
   block indefinitely in a driver or worker call. Frame wait, stream stop,
   network stop/resume, and worker completion all use bounded waits (e.g.
   `select` timeout / non-blocking `DQBUF`). This is settled by §1: the current
   blocking `DQBUF` hung **113 s past the 45 s deadline** because control never
   returned to the owner to re-check it. The §12 "blocked operations" items
   verify each call actually meets its bound.

---

## 3. Architecture — single-owner lifecycle task

Mirror the `tailscale_adapter` lifecycle model:

```
cam_owner_task (resident)  ← cam_cmd_queue
    CAM_CMD_SCAN { mode (QR|BARCODE), timeout_ms, cb, arg }
    (cancel is an atomic flag, not a queued command — see §7)
```

- Public `camera.scan/scanQr` → `cam_cmd_post(SCAN{...})` (non-blocking).
- The owner runs **one scan at a time**; a second SCAN while busy → immediate
  `cb(busy)` (not queued).
- The owner runs one bounded scan operation, completes its teardown, publishes
  the result, and then loops. The exact teardown mechanism remains open (§5);
  unlike the current self-deleting `scan_task`, the resident owner preserves a
  context in which completion and repeated-cycle behavior can be observed.
- **Cross-owner handshake**: the camera owner and the `tailscale_adapter` owner
  coordinate across tasks. The camera owner suspends the network by command, but
  must wait for an observable **stop-completion** from the network owner before
  relying on the exclusion — posting STOP is not "stopped" (§12 suspend
  completion). Resume is the symmetric step during teardown. Avoid a circular
  wait between the two owners (define a one-way dependency).

---

## 4. Mode-specific resource policy candidates

| | **Barcode (EAN-13)** | **QR** |
|---|---|---|
| Network suspend | **microlink only** (Wi-Fi stays up) | **Wi-Fi fully off target** |
| Capture region | **no crop** (full 800×600 center, θ-fan line scan) | **crop to reticle window** (small, native-res) |
| Decode buffer | n/a (no quirc; line scan) | quirc image → **internal SRAM target** |
| Aiming | none (current behavior, already fast) | reticle overlay (§6) |

Rationale:
- **Barcode** is already fast and aiming a box is annoying → keep it as-is, just
  remove the microlink CPU contention. Wi-Fi stays up so the post-scan
  **NDL book lookup** can run (it may show "問い合わせ中" and finish async — the
  user confirmed waiting is acceptable).
- **QR** may benefit from a small native crop (target: internal SRAM, faster decode)
  and there is no need for the network *during* the scan: the QR result
  (provisioning: Wi-Fi/Tailscale keys) is processed **after** the pipeline stops,
  in the callback. Full Wi-Fi-off is therefore the first candidate to measure:
  it should free the most CPU/DMA bandwidth and may free enough internal SRAM to
  host the quirc buffer (§8).

---

## 5. Resource ownership model

Two tiers (the split is forced by an esp_video constraint, §9):

- **Persistent (lazy, acquire-once, never freed)**: `esp_video_init` + V4L2
  `REQBUFS`/mmap + PPA client. esp_video cannot be torn down and re-`REQBUFS`'d
  (§9), so these are brought up on the first scan and kept. They do **not** stream
  by themselves — streaming = `STREAMON`, which is per-scan.
- **Per-scan (must be released before completion is published)**:
  `STREAMON`↔`STREAMOFF`, the busy state, preview canvas + dismiss callback,
  network suspension (§4), and (QR) the decoder worker + decode buffers.

The intended ownership rule is simple: the camera owner acquires per-scan
resources, is the only context allowed to release them, and does not publish the
scan result until teardown has reached a known state. Acquisition must record
what actually succeeded so partial-start failures can be unwound and measured.

The exact teardown mechanism is deliberately not selected here. A scope cleanup
helper, explicit state machine, or another mechanism is acceptable only after it
demonstrates the same behavior for success, timeout, cancel, initialization
failure, decode-worker failure, and network-resume failure.

The deadline bounds normal scans, but it is not by itself a resource-safety
guarantee: a blocked driver call or worker that does not finish can prevent the
owner from reaching teardown. Those cases are lifecycle verification items, not
assumed solved.

---

## 6. QR reticle window — crop, don't downscale

The earlier "decode at 400×300" failed because **downscaling** drops the QR's
pixel density below quirc's detection threshold. The fix is to **crop a small
window at the sensor's native resolution**: the QR, aligned into the window, keeps
full pixel density while the buffer stays small.

- **Decode crop** (actual): sized to fit internal SRAM (§8), e.g. 400×400 = 160 KB,
  taken at native res from the 1600×1200 sensor center.
- **Display reticle** (visible): drawn **smaller** than the decode crop, e.g.
  300×300. The ~50 px margin between reticle and crop is built-in **slack** that
  absorbs the user's aiming error and QR-size variance, so a slightly-misaligned
  or slightly-oversized QR still lands inside the decode crop (finder patterns not
  clipped). The slack costs nothing — it is inside the already-allocated buffer.
- Preview shows the wider view (context for aiming) with the reticle overlay;
  only the reticle region is cropped for decode.
- **QR only.** Barcode keeps the full-frame θ-fan (it needs horizontal extent and
  is already fast).

Open item **M2**: find the smallest reticle that reliably captures an aligned QR
(empirical sweep with real codes), and the decode-crop size around it.

> **Settled (`fd98fac`, device-verified):** the shipping crop is **640×640**,
> not the 400×400 sketched above. Crop size turned out to be driven by decode
> quality (px per module), not by the SRAM budget: the provisioning QR is
> v11/12, so 400×400 gives 4.6 px/module and fails ECC while 640×640 gives
> 8 px/module and decodes. 640×640 = 410 KB, which never had a chance of
> fitting internal SRAM — see the note in §8.

---

## 7. Cancel — flag, not a cross-task kill

- `camera.cancel()` sets one atomic `s_cancel` (no "set_flag" abstraction, no
  public "kill" function). The scan loop's condition is
  `while (!s_cancel && now < deadline && !found)`; it waits on the fd with a
  bounded timeout (~50 ms `select`) so the flag is checked promptly even at low
  fps.
- Teardown runs on the owner task. Resources are owned by the running scan, so
  freeing them from another task risks use-after-free. Cancel therefore requests
  a stop; the owner performs teardown after outstanding operations reach a known
  state.
- A deadline bounds the normal scan loop, but cancel latency and teardown still
  depend on blocking driver/worker operations returning. Measure these paths.
- (Optional, only if low-fps cancel latency matters: `STREAMOFF` from cancel
  unblocks a stuck `DQBUF` — V4L2's documented unblock — but needs the careful
  "invalidate → wait for the select cycle → free" ordering, cf. microlink
  `microlink_rebind`. Prefer the simple bounded-wait+flag first.)

---

## 8. quirc buffer in internal SRAM (QR, Wi-Fi off)

Internal heap budget (measured):

```
768 KB  HP L2MEM (physical)
−128 KB  L2 cache (CONFIG_CACHE_L2_CACHE_128KB — already the MINIMUM option)
−~64 KB  ROM/reserved
=576 KB  linker DIRAM region (idf.py size)
−283 KB  static .bss/.data/.text-in-RAM
≈293 KB  heap ceiling (immovable — Wi-Fi off frees runtime allocs, not the static floor)
```

- quirc image = `calloc(w,h)` = 1 byte/pixel (pixels aliases image,
  `QUIRC_PIXEL_ALIAS_IMAGE`, uint8 regions), so total ≈ H×W + small flood-fill vars.
- **Full-res 800×600 = 469 KB > 293 KB ceiling → cannot fit internal, ever**
  (cache already minimal; reducing static 283 KB enough is unrealistic).
- **Cropped 400×400 = 160 KB**: Wi-Fi off may free enough esp_hosted/lwIP/
  microlink runtime internal memory for the image, but total free bytes do not
  imply a contiguous block or that the allocator will place the image there.

Open item **M1**: measure `heap_caps_get_largest_free_block(INTERNAL)` with Wi-Fi
off + camera buffers in PSRAM to confirm the achievable contiguous block.
Open item **M3**: micro-bench quirc `identify` on the same crop in internal vs
PSRAM — only worth the SRAM placement if it is PSRAM-cache-bound (vs compute-bound).
(Note: quirc runs on a worker, so this speeds *decode latency*, not preview fps.)

> **Settled — this whole section is dead. Do not re-attempt.** M1 was measured
> on device: with esp-hosted up, P4 internal SRAM is held by lwIP/SDIO and the
> largest contiguous INTERNAL block is only ~34–43 KB. Turning Wi-Fi off frees
> almost nothing here, because the radio lives on the C6, not the P4. And the
> crop that actually decodes is 640×640 = 410 KB (§6), an order of magnitude
> past the ceiling. **quirc stays in PSRAM.**

---

## 9. esp_video constraint

esp_video cannot be torn down and re-`REQBUFS`'d — the 2nd scan's `STREAMON`
fails (`cam_tab5.c` persistent-pipeline note; M5's UserDemo never closes the
camera either). Therefore:

- `esp_video_init` + `REQBUFS`/mmap happen **once** (lazy, first scan) and are
  kept (persistent tier, §5). They don't stream by themselves.
- Per-scan start/stop uses `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF` only.

Open item: verify `STREAMOFF`→(no REQBUFS)→`STREAMON` cycling works on this
esp_video. If it does → streaming is fully per-scan (zero DMA between scans). If
not → fallback: keep streaming after the first scan (still no boot streaming);
the mutual exclusion (§4) still removes the contention.

---

## 10. Warm-up / noise

The observed "noise" is mostly the viewfinder stuck on the first unconverged
(AWB/AGC) frames *because of starvation*. With the exclusion (§4) the camera gets
full CPU → frames flow → AWB converges in a fraction of a second → noise clears.
If residual cold-start noise remains, add a brief warm-up to `cam_scan_begin`:
`STREAMON`, then discard the first N frames / ~M ms (show "準備中") before
decode/preview. (Verify need after Phase 1.)

---

## 11. Phasing

- **Phase 1 — owner + auditable teardown + mutual exclusion (the fps fix).**
  - Add temporary network suspension without changing the user's enabled/guard
    state; determine the required stop-completion and resume semantics by
    measurement.
  - `cam_owner_task` + `cam_cmd_queue`; `cam_run_scan()` returns; an ownership
    ledger that records the per-scan resources actually acquired, incl. network
    state (barcode: microlink / QR: Wi-Fi). (Ledger as a concept; the teardown
    mechanism stays open per §5.)
  - `scan_start` posts SCAN instead of spawning a task.
  - Verify: scan while connected → network suspends → fps recovers → resumes
    after (no leak), barcode and QR.
- **Phase 2 — QR reticle window + internal-SRAM quirc.**
  - Mode-specific decode crop; reticle overlay (display < crop slack); quirc
    image to internal SRAM under Wi-Fi-off; M1/M2/M3 verification.

---

## 12. Lifecycle and memory investigation

This section records problems to resolve while implementing and measuring. It
does not prescribe APIs or teardown code before the device behavior is known.

### Lifecycle management

- **Suspend completion**: posting a network-stop request is not proof that its
  tasks, sockets, DMA, or callbacks have stopped. Determine the observable point
  at which camera start is safe.
- **Starts during suspension**: account for reconnects, delayed lifecycle
  commands, time-sync callbacks, and user configuration changes arriving while
  a scan owns the exclusion window.
- **Completion ordering**: define and measure the order between decoder drain,
  camera stream stop, UI dismissal, network resume, busy-state release, and
  result callback. In particular, result handling must not race resources still
  owned by the previous scan.
- **Worker shutdown**: a QR timeout or cancel may occur while decode is running.
  Verify that the worker has stopped touching shared buffers before they are
  released or reused.
- **Blocked operations**: verify bounded behavior for frame wait, stream stop,
  network stop/resume, and worker completion. The scan deadline only works if
  control returns to the owner.
- **Partial acquisition**: inject failure after each acquisition step and verify
  that the system returns to a usable idle state.
- **Repeated cycles**: run scan/cancel/timeout/found loops and confirm no stale
  frames, duplicate callbacks, stuck busy state, lost network resume, or gradual
  latency growth.
- **Wi-Fi full-off semantics**: determine how disconnect callbacks, automatic
  reconnect, DHCP, SNTP, microlink, and other network users behave across a QR
  scan. Full-off remains a hypothesis until this is repeatable.
- **esp_video state boundary**: establish which combination of persistent setup,
  queued buffers, and stream state survives repeated scans. Do not assume either
  full teardown or keep-streaming fallback is acceptable before measurement.

### Memory management

- **Peak, not steady-state, budget**: record internal/DMA/PSRAM free bytes and
  largest blocks before, during, and after every acquisition and teardown step.
- **Fragmentation**: repeated Wi-Fi/microlink and QR worker cycles may return the
  same total bytes while shrinking the largest usable block. Track both values
  over long cycle tests.
- **Placement guarantee**: verify where each large allocation actually lands.
  Requesting or expecting internal SRAM is not sufficient without an observed
  placement guarantee.
- **Overlapping lifetimes**: identify the true peak when old network resources
  are still draining while camera/QR resources begin, and again during resume
  before camera resources are fully released.
- **Persistent cost**: quantify the memory retained by lazy camera initialization,
  mapped V4L2 buffers, PPA state, task stacks, queues, and synchronization
  objects between scans.
- **Stack headroom**: measure owner and decoder worker high-water marks on normal,
  failure, and cancellation paths.
- **Allocation failure behavior**: every large allocation failure must leave the
  next scan and network resume usable; validate with deliberate low-memory runs.
- **quirc policy**: choose crop size, allocation placement, and reuse-vs-release
  policy only after M1/M3 and fragmentation measurements.

Exit criteria for a lifecycle choice: repeated found/timeout/cancel/failure
cycles return to the same observable idle state, network service recovers, no
resource metric trends downward, and the next scan behaves like the first.

---

## 13. Open questions / verification items

- **M1**: contiguous internal free with Wi-Fi off + camera PSRAM buffers (does
  the 400×400 quirc image fit?).
- **M2**: smallest reliable QR reticle / decode-crop size (real-code sweep).
- **M3**: quirc `identify` internal vs PSRAM (is it PSRAM-cache-bound?).
- esp_video `STREAMOFF`/`STREAMON` cycling (per-scan streaming vs keep-after-first).
- Wi-Fi full-off resume cost for QR (reconnect + DHCP + microlink ~seconds) —
  acceptable since the QR result is processed post-resume with a "問い合わせ中"
  style wait, but confirm UX.
- Barcode microlink-suspend: confirm WiFi-idle SDIO DMA doesn't itself starve the
  camera (expected fine; microlink was the heavy contender).
- Suspend/resume completion points and callbacks that can race the exclusion
  window.
- Result callback ordering relative to camera teardown and network resume.
- QR worker drain/cancel behavior and bounded teardown latency.
- Long-run heap fragmentation and stack high-water marks across repeated cycles.

---

## 14. References

- `tailscale_adapter.c` single-owner lifecycle + `ts_lifecycle_cmd_t` (commit
  `aa58c85`) — the pattern this mirrors; also the tcpip_thread deadlock write-up.
- `components/cam_tab5/cam_tab5.c` — current scan path, PPA two-pass, quirc worker.
- `docs/history/qr-read-performance.md`, `docs/history/scanline-opt-plan.md` — prior QR/barcode work.
- Measurement: device capture 2026-06-15 (0.2 fps, quirc id 26 s, task_wdt
  ml_derp_tx×14 / ml_wg_mgr×7).
