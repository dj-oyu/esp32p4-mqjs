# Camera pipeline lifecycle redesign (QR / barcode) — design plan

Status: **design** (2026-06-15). Implementation pending. Supersedes the ad-hoc
`cam_tab5` scan path. Companion to the just-landed `tailscale_adapter`
single-owner lifecycle refactor (commit `aa58c85`), whose pattern this mirrors.

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

1. **On-demand**: nothing camera-related runs at boot or between scans (no
   continuous CSI/ISP DMA, no buffers held).
2. **QR / barcode modes**, each with its own resource policy (§4).
3. **Forget-proof resource release**: acquisition and release are paired in one
   place each, and release is *mechanically guaranteed* on every exit path —
   the C analogue of Go `defer` / Java/Python try-with-resources. (Past bug: a
   one-shot boot probe left `STREAMON` running forever.)
4. **Camera ↔ network mutual exclusion** to kill the contention.
5. Single owner serializes all transitions (no scan-vs-cancel / double-start
   races).

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
- The owner calls `cam_run_scan()` — a function that **returns** (so the
  `cleanup` attribute fires; see §5). The owner then loops. (The current
  `scan_task` ends in `vTaskDelete(NULL)`, which never returns and would defeat
  `__attribute__((cleanup))` — hence the resident owner.)

---

## 4. Mode-specific resource policy

| | **Barcode (EAN-13)** | **QR** |
|---|---|---|
| Network suspend | **microlink only** (Wi-Fi stays up) | **Wi-Fi fully off** |
| Capture region | **no crop** (full 800×600 center, θ-fan line scan) | **crop to reticle window** (small, native-res) |
| Decode buffer | n/a (no quirc; line scan) | quirc image → **internal SRAM** |
| Aiming | none (current behavior, already fast) | reticle overlay (§6) |

Rationale:
- **Barcode** is already fast and aiming a box is annoying → keep it as-is, just
  remove the microlink CPU contention. Wi-Fi stays up so the post-scan
  **NDL book lookup** can run (it may show "問い合わせ中" and finish async — the
  user confirmed waiting is acceptable).
- **QR** benefits from a small native crop (fits internal SRAM, faster decode)
  and there is no need for the network *during* the scan: the QR result
  (provisioning: Wi-Fi/Tailscale keys) is processed **after** the pipeline stops,
  in the callback. So Wi-Fi can be fully off, which (a) frees the most CPU/DMA
  bandwidth and (b) frees enough internal SRAM to host the quirc buffer (§8).

---

## 5. Forget-proof resource model — `__attribute__((cleanup))` defer

Two tiers (the split is forced by an esp_video constraint, §9):

- **Persistent (lazy, acquire-once, never freed)**: `esp_video_init` + V4L2
  `REQBUFS`/mmap + PPA client. esp_video cannot be torn down and re-`REQBUFS`'d
  (§9), so these are brought up on the first scan and kept. They do **not** stream
  by themselves — streaming = `STREAMON`, which is per-scan.
- **Per-scan (defer-managed — released on every exit)**: `STREAMON`↔`STREAMOFF`,
  the busy flag, preview canvas + dismiss-cb registration, the network suspend
  (§4), and (QR) the quirc worker + quirc/crop buffers.

The mechanism:

```c
typedef enum { CAM_MODE_BARCODE, CAM_MODE_QR } cam_mode_t;

/* acquisition ledger: cam_scan_begin records what it actually acquired;
   cam_scan_end releases exactly that (idempotent, safe on partial-begin). */
typedef struct {
    bool active;        /* begin got far enough to run */
    bool streaming;     /* STREAMON issued      -> STREAMOFF */
    bool net_suspended; /* microlink/Wi-Fi off  -> resume    */
    bool qr;            /* quirc + worker + crop -> free      */
    bool canvas;        /* preview canvas/dismiss-cb -> unregister */
} cam_scan_t;

static cam_scan_t cam_scan_begin(cam_mode_t mode, uint32_t timeout_ms);
static void       cam_scan_end(cam_scan_t *s);   /* the single teardown */

static void cam_run_scan(cam_mode_t mode, uint32_t to, cam_cb_t cb, void *arg)
{
    cam_scan_t scan __attribute__((cleanup(cam_scan_end)))
        = cam_scan_begin(mode, to);
    if (!scan.active) { cb(arg, FAIL); return; }   /* cleanup still runs (idempotent) */

    /* ... DQBUF/QBUF loop: decode, check s_cancel + deadline ... */
    /* On ANY exit (found / timeout / cancel / error / early return) the compiler
       runs cam_scan_end(&scan): STREAMOFF → free QR bufs → unregister canvas →
       resume network → busy=false. Impossible to forget. */
    cb(arg, result);
}
```

```c
static void cam_scan_end(cam_scan_t *s)            /* one place, symmetric */
{
    if (s->canvas)        ui_tab5_cam_set_dismiss_cb(NULL);  /* + hide canvas */
    if (s->qr)            { /* worker delete, quirc_destroy, free crop/qr bufs */ }
    if (s->streaming)     ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    if (s->net_suspended) net_resume(s->mode);     /* §4: microlink or Wi-Fi */
    s_busy = false; s->active = false;
}
```

**Key invariant**: resource *safety* (no leak, network always resumed) is
guaranteed by `cam_scan_end` + the `cleanup` attribute + the scan **deadline**
(every scan ends → `cam_run_scan` returns → cleanup fires). It does **not**
depend on cancel being observed. The boot-probe class of bug (acquire-and-forget)
is structurally impossible: there is no code path that acquires without the
paired cleanup.

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

---

## 7. Cancel — flag, not a cross-task kill

- `camera.cancel()` sets one atomic `s_cancel` (no "set_flag" abstraction, no
  public "kill" function). The scan loop's condition is
  `while (!s_cancel && now < deadline && !found)`; it waits on the fd with a
  bounded timeout (~50 ms `select`) so the flag is checked promptly even at low
  fps.
- The **only** teardown ("suicide") function is `cam_scan_end`, run **on the
  owner task** via `cleanup`. Resources are owned by the running scan, so freeing
  them from another task = use-after-free. Cancel therefore *requests* a stop; the
  owner reaches `cam_scan_end` and tears down. ("Request suicide; the owner dies",
  not "kill from outside".)
- Even if a cancel is missed, the **deadline** guarantees teardown → no leak.
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
- **Cropped 400×400 = 160 KB**: with Wi-Fi off freeing the esp_hosted/lwIP/microlink
  runtime internal (~60–100 KB) and camera buffers in PSRAM, a contiguous ~160 KB
  internal block should be available → quirc image fits internal.

Open item **M1**: measure `heap_caps_get_largest_free_block(INTERNAL)` with Wi-Fi
off + camera buffers in PSRAM to confirm the achievable contiguous block.
Open item **M3**: micro-bench quirc `identify` on the same crop in internal vs
PSRAM — only worth the SRAM placement if it is PSRAM-cache-bound (vs compute-bound).
(Note: quirc runs on a worker, so this speeds *decode latency*, not preview fps.)

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

- **Phase 1 — owner + defer + mutual exclusion (the fps fix).**
  - `tailscale_adapter_suspend()/resume()` (post to the TS lifecycle queue;
    auto-resume; do not touch enabled/guard; status "一時停止").
  - `cam_owner_task` + `cam_cmd_queue`; `cam_run_scan()` returns; `cam_scan_t`
    `cleanup` ledger with `net_suspended` (barcode: microlink / QR: Wi-Fi).
  - `scan_start` posts SCAN instead of spawning a task.
  - Verify: scan while connected → network suspends → fps recovers → resumes
    after (no leak), barcode and QR.
- **Phase 2 — QR reticle window + internal-SRAM quirc.**
  - Mode-specific decode crop; reticle overlay (display < crop slack); quirc
    image to internal SRAM under Wi-Fi-off; M1/M2/M3 verification.

---

## 12. Open questions / verification items

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

---

## 13. References

- `tailscale_adapter.c` single-owner lifecycle + `ts_lifecycle_cmd_t` (commit
  `aa58c85`) — the pattern this mirrors; also the tcpip_thread deadlock write-up.
- `components/cam_tab5/cam_tab5.c` — current scan path, PPA two-pass, quirc worker.
- `docs/qr-read-performance.md`, `docs/scanline-opt-plan.md` — prior QR/barcode work.
- Measurement: device capture 2026-06-15 (0.2 fps, quirc id 26 s, task_wdt
  ml_derp_tx×14 / ml_wg_mgr×7).
