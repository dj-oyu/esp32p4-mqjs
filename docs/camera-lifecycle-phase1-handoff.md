# Camera lifecycle Phase 1 — progress / resume handoff (2026-06-16)

Snapshot for resuming after a break. Companion to
[`docs/camera-lifecycle-plan.md`](camera-lifecycle-plan.md) (the design).
Branch: **`codex/system-settings-ui`**. Worktree:
`D:\vscode\misc\esp32p4-mqjs-system-settings`. **Uncommitted, unpushed.**

---

## TL;DR state

- **Phase 1 implemented and builds clean** (`build_system_settings`, exit 0,
  bin ~4.26 MB, 32% free). No warnings on the changed files.
- **Flashed to Tab5 (COM8)** successfully (Hash verified ×3, watchdog-reset
  reboot). Clean boot, no panic/boot-loop, Wi-Fi up, **tailnet connected
  100.83.60.44**.
- **Mutual exclusion confirmed live in a boot capture**: a real scan started
  (`cam_tab5: video ready`) and ~170 ms later `microlink: Stopping...` with all
  ml tasks (derp/coord/net_io/wg_mgr/zero-copy) exiting = the suspend hook
  firing. Stop completion ~3 s (zero-copy drain), within the 8 s bound.
- **OPEN BUG (not root-caused):** after the flash, opening the *camera demo app*
  left the **UI responsive but with no screen transition / navigation stuck**.
  Suspected stuck camera modal scrim or a scan not completing — see §5. NOT yet
  reproduced under capture; a repro protocol is ready (§6).
- Full fps-recovery + network-resume cycle **not yet captured** (the 26 s window
  ended mid-scan; the camera was idle on the later boots so no auto-repro).

---

## 1. What changed (the 6 files)

```
 components/cam_tab5/cam_tab5.c         | +208/-…  owner task + ledger + bounded loop + net hooks
 components/cam_tab5/include/cam_tab5.h | +12      cam_tab5_net_hook_t + cam_tab5_set_net_hooks
 components/mqjs/tailscale_adapter.c    | +62      SUSPEND/RESUME cmds, s_suspended, suspend()/resume()
 components/mqjs/tailscale_adapter.h    | +13      tailscale_adapter_suspend()/resume() decls
 main/app_main.c                        | +5       register hooks after tailscale_adapter_init()
 docs/camera-lifecycle-plan.md          | +43      status -> "Phase 1 implemented"
 tools/cap_boot.py                      | (new)    serial boot/runtime capture helper for COM8
```

### cam_tab5.c (the core refactor)
- `cam_scan_req_t` (was anon `s_req` struct) posted to a resident
  **`cam_owner_task` + `s_cam_queue`** (depth 1). `scan_start` posts instead of
  spawning a self-deleting task; busy-reject at post time (returns 0, never
  queued). `cam_run_scan()` **returns** (no `vTaskDelete`) and the owner loops.
- `cam_tab5_net_hook_t s_net_suspend/s_net_resume` + `cam_tab5_set_net_hooks()`.
- `cam_net_policy_t` + `cam_ledger_t {net, dismiss_cb, canvas}` — teardown on
  EVERY exit path in §12 completion order: qr drain → canvas hide → dismiss-cb
  clear → **network resume** → busy release → result callback.
- Network suspend is called **before** `pipeline_once()` (own the bus first).
- **Bounded DQBUF**: `VIDIOC_S_DQBUF_TIMEOUT` (`CAM_DQBUF_TIMEOUT_MS` = 500 ms,
  from `esp_video_ioctl.h`) + `CAM_DQBUF_MAX_FAILS` = 12 (~6 s of zero frames →
  "no frames (camera stalled)"). Replaces the old unbounded DQBUF that hung 113 s
  past the deadline.
- Kept (Phase 2 / §9): esp_video acquire-once, **keeps streaming between scans**;
  QR still decodes full-res 800×600 in PSRAM (no reticle window, no internal-SRAM
  quirc).

### tailscale_adapter.c / .h
- `TS_LIFECYCLE_CMD_SUSPEND` / `_RESUME`; `static bool s_suspended`;
  `static SemaphoreHandle_t s_suspend_done` (created in init).
- SUSPEND handler: `s_suspended=true; stop_session();` then a stop-completion
  `xSemaphoreGive(s_suspend_done)`; status set to "カメラ起動のため一時停止" only if
  there was a live session, else `refresh_idle_status()`.
- RESUME handler: `s_suspended=false;` then `begin_connect()` if still
  configured+enabled+net_up+!skip, else `refresh_idle_status()`.
- `s_suspended` guards every auto-start path: START handler, `begin_connect`,
  `watchdog_tick` (early return), `on_time_synced`.
- Public `tailscale_adapter_suspend()` posts SUSPEND and **blocks ≤8 s** on
  `s_suspend_done` (the cross-owner stop-completion). `resume()` posts RESUME
  (fire-and-forget). Stubs added in the CONFIG-off branch.
- One-way dependency: camera owner waits on the adapter, never the reverse.

### app_main.c
```c
tailscale_adapter_init();
wifi_set_time_sync_cb(tailscale_adapter_on_time_synced);
cam_tab5_set_net_hooks(tailscale_adapter_suspend, tailscale_adapter_resume);  // <-- added
```

---

## 2. Build (incremental, no reconfigure)

`build_system_settings` is already configured (esp32p4, SDIO/C6, CAMERA + TAILSCALE
on — verified). Plain `.c/.h` edits rebuild incrementally; do NOT
`reconfigure`/`fullclean` (kconfig-drop trap). One-shot PowerShell:

```powershell
$env:IDF_PATH='C:\Espressif\.espressif\v6.0.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.0.1\venv'
$env:IDF_PYTHON_CHECK_CONSTRAINTS='0'
$env:ESP_IDF_VERSION='6.0'
$venv='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
& $venv "$env:IDF_PATH\tools\idf_tools.py" export --format key-value 2>$null | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item ("Env:"+$matches[1]) ($matches[2] -replace '%PATH%',$env:PATH) } }
& $venv "$env:IDF_PATH\tools\idf.py" -C 'D:\vscode\misc\esp32p4-mqjs-system-settings' -B 'D:\vscode\misc\esp32p4-mqjs-system-settings\build_system_settings' build
```
(Shell env does NOT persist between PowerShell tool calls — set it in the same
call as the build.)

---

## 3. Flash (Tab5 = COM8; misroutes DTR/RTS → use watchdog-reset)

```powershell
$venv='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$b='D:\vscode\misc\esp32p4-mqjs-system-settings\build_system_settings'
& $venv -m esptool --chip esp32p4 -p COM8 -b 460800 --before default-reset --after watchdog-reset `
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x2000 "$b\bootloader\bootloader.bin" 0x8000 "$b\partition_table\partition-table.bin" 0x10000 "$b\esp32p4-mqjs.bin"
```
- The trailing `OSError(22, '存在しないデバイス')` + exit 1 is the **benign**
  USB-JTAG re-enumeration — "Hash of data verified" prints before it = success.
- Before flashing, verify the config used: `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=1`
  + slave `esp32c6` (grep `build_system_settings/config/sdkconfig.h`). Confirmed
  good on this build.

---

## 4. Serial capture (`tools/cap_boot.py`)

```powershell
& 'C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe' `
  'D:\vscode\misc\esp32p4-mqjs-system-settings\tools\cap_boot.py' <seconds>
```
- App logs come out the **secondary console on COM8** (primary is UART0 on the
  Tab5 header pins, not wired).
- **Opening COM8 fires `rst:0x17 (CHIP_USB_UART_RESET)` → fresh boot.** So every
  capture reboots the device and wipes the current UI/scan state. This also means
  a stuck-camera state is CLEARED by a capture (camera is free on a fresh boot
  unless an app re-triggers a scan).
- Boot → usable launcher takes ~12 s; tailnet connect ~16 s.

Observed heap (this firmware):
- net-up (pre-connect): `INTERNAL free=179987 largest=81920`
- microlink **connected**: `INTERNAL free≈109000 largest=31744` — connect costs
  ~70 KB internal and fragments the largest block 81920→31744. (Relevant to
  Phase 2 M1: the QR quirc buffer needs a contiguous internal block; this is why
  suspending microlink frees/defrags internal SRAM.)

---

## 5. THE OPEN BUG — "camera demo app: UI alive but no screen transition"

**Symptom (user, post-flash):** opened the camera demo app → the UI still
responds (status bar etc.) but tapping doesn't navigate anywhere.

**Strong suspect:** the camera modal **scrim**. `ui_tab5_cam_canvas()` adds a
full-screen `LV_OBJ_FLAG_CLICKABLE` scrim on `lv_layer_top()` that intercepts
**all** touches to widgets behind it (web-modal). If a scan is shown but its
teardown never runs `ui_tab5_cam_canvas_hide()`, the scrim stays up forever →
"UI responds (scrim redraws) but nothing navigates." Tapping the scrim should
fire the dismiss cb (`cam_tab5_cancel`).

**Analysis — where `cam_run_scan` could hang before teardown (all look bounded):**
- `s_net_suspend()` → `tailscale_adapter_suspend()` waits ≤ 8 s on
  `s_suspend_done`, then proceeds. Bounded.
- scan loop: `while (!s_cancel && now < deadline && !found)`, deadline = +45 s,
  DQBUF now bounded 500 ms. Bounded.
- `if (qr_outstanding) xSemaphoreTake(s_qr_result, portMAX_DELAY)` — UNBOUNDED in
  code, but the qr_worker gives `s_qr_result` for every `s_qr_go` it takes, even
  when `s_quirc` is NULL; quirc identify is finite. With microlink suspended it's
  fast. → bounded in practice (worst case ~the identify time).
  **→ This is the one remaining unbounded wait; suspect #1 to scrutinize if the
  repro shows the hang AFTER "scanning" but with no "MEAS scan-end".**

So a *permanent* stuck implies `cam_run_scan` is blocked on something I haven't
identified, OR it's app-level: the demo app **re-scans in a loop** (e.g., a QR
reader that restarts on each 45 s timeout), keeping the scrim up ~continuously
and `s_cam_active`/`s_busy` true so other scans get rejected. The first boot
capture's scan came from `device_settings` (the Tailscale QR provisioning
reader) being the persisted foreground — that reader may loop.

**Hypotheses, ranked:**
1. Scan never reaches teardown → scrim stuck (find the blocked call via repro log).
2. App re-scan loop holds the camera continuously (app-level, maybe not a
   regression). Confirm whether it worked before the flash.
3. Stuck busy (`s_cam_active`/`s_busy` true) → new scan returns 0 → no viewfinder
   at all (same root as #1 if a prior scan never fired its cb).

**Decisive data needed (from the repro capture):** does `cam_tab5: video ready`
appear (scan requested)? then `microlink: Stopping...` (suspend fired)? then the
`scanning`/`scanning QR` status line (loop entered)? then `MEAS scan-end`
(completed)? then a tailnet reconnect (resume)? **Where the markers stop = the
bug location.**

**Do NOT fix blind** — get the repro log first.

---

## 6. Repro protocol (ready to run on resume)

Camera is idle on a fresh boot, so the bug must be driven by hand. Capture
reboots the device, so the user acts *during* the capture window.

1. Start a ~120 s capture (`tools/cap_boot.py 120`) → device reboots.
2. User waits ~12 s for the launcher.
3. User opens the **same camera demo app** that got stuck. (Get its exact
   launcher name; read its JS — likely under the seed apps / store catalog or
   `device_settings.js` if it's the QR reader.)
4. User observes the preview: black / noise / smooth / nothing — and notes it.
5. User tries to scan a code, then tries to navigate back (tap outside / back).
6. User reports rough timestamps of each action.

Then read the capture and match against the §5 marker chain.

**Questions for the user:**
- Exact app name in the launcher.
- Did the same app work **before** this flash? (regression vs pre-existing)

---

## 7. Next steps on resume (in order)

1. Run the §6 repro capture; locate where the marker chain stops.
2. If stuck before teardown → fix that call (e.g., bound the qr drain with a
   timeout AND a safe "worker still owns buffers" handling; or ensure the scan
   loop/suspend can't wedge). If app-level re-scan loop → decide whether to
   change the app or make the platform robust to it.
3. As a safety net regardless of root cause, consider: a teardown guarantee for
   the scrim (always hide on any owner exit), and/or a hard cap so the camera
   can never hold the modal indefinitely.
4. Once a real scan completes while connected: capture the **MEAS scan-end** line
   (fps) + the network resume, to finally verify the fps-recovery claim and
   no-leak across repeated found/timeout/cancel cycles.
5. Then commit (currently uncommitted) and decide on push.

---

## 8. Git state

- Branch `codex/system-settings-ui`, **6 files modified + `tools/cap_boot.py`
  untracked**, nothing committed.
- The device is running this exact (uncommitted) firmware.
- Memory updated: see `tailscale-microlink-phase3.md` (Phase 1 implemented +
  flashed + suspend-on-scan confirmed; open bug noted).
