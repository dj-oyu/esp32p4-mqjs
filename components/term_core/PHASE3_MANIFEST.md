# Phase 3 manifest — LP SRAM black box, MQTT responder

Scope: docs/term-design.md §11.3 — the LP SRAM black-box ring of §4.4 (two
static partitions, `{writer_id, class}` record headers, SGR-stripped tee from
ingest), the `lastboot` read-back, and the MQTT responder behind the Ed25519
signature gate of §7.3. Out of scope: `term.pipe` / `term.onReply` and the
ssh_vt migration (phase 4), mouse reporting and snapshot persistence (§11.5).

Owner of this file: **opus_A** (implementation). Updated as the phase
proceeds — a running contract, not a plan written once. Same reading rule as
phase 2: the test author works from headers and `docs/term-design.md`, never
from an implementation `.c`.

Status: **§1 (the retention probe) implemented, off-device verification done,
device run pending.** Nothing else in the phase has started, and by §11.3's own
rule nothing else may: the ring is not implemented until the probe has run
("未検証のまま実装しない").

| Column | Meaning |
|---|---|
| Read by tests | `yes` = the test author may open it. `NO` = implementation. |
| Status | `done`, `wip`, `planned` |

---

## 1. LP SRAM retention probe (§11.3 prerequisite)

### What it tests, and why it exists at all

§4.4 puts the flight recorder in LP SRAM on one assumption: an
`RTC_NOINIT_ATTR` region survives a panic, a watchdog reset and a software
reset with **its full contents** intact. §11.3 refuses to build on that
assumption unmeasured, because ESP32-family parts have historically wiped the
whole RTC domain on some reset causes, and a black box that silently loses the
last words is worse than no black box.

So the probe answers one question per reset cause: *did 31 KiB of
`RTC_NOINIT_ATTR` come back, and if not, how much of it?* Partial survival is
a separate verdict from full survival — a few corrupted words means the ring
needs per-record CRCs, a full wipe means that cause has no `lastboot` at all.

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_lp_probe.h` | Probe contract: region size, cause list, the four entry points, the JSON report format | yes | done |
| `term_lp_probe.c` | Pure logic (CRC32 / PRNG pattern / verdict) unguarded; RTC region, NVS state store, crash triggers and the trigger task `ESP_PLATFORM`-guarded | NO | done |
| `CMakeLists.txt` | `term_lp_probe.c` added to `SRCS`, `nvs_flash` to `PRIV_REQUIRES` | NO | done |
| `components/mqjs/mqjs_runtime.c` | `js_sys_lpProbe` binding | NO | done |
| `components/mqjs/device_stdlib.c` | `sys.lpProbe` in the `js_sys[]` prop table | NO | done |
| `components/mqjs/gen/device_stdlib.h` | ROM stdlib regen (-m32). `gen/mquickjs_atom.h` regenerates byte-identical (the name lands in the stdlib string table, not the atom table) and was restored with `git checkout --` | NO | done |
| `main/app_main.c`, `main/CMakeLists.txt` | `term_lp_probe_boot()` as the first statement of `app_main`; `term_core` added to `PRIV_REQUIRES` | NO | done |
| `tools/probe_lp.js` | Orchestrator-side dev-slot probe: arm on the first push, publish results on the second | orchestrator | done |

### The payload

31 KiB exactly — `TERM_LP_PROBE_REGION_BYTES`, a 32-byte header
(`magic`, `seq`, `len`, `crc`, `hdr_crc`, pad) followed by 31,712 bytes of
xorshift32 pattern. The size is deliberate: §4.4 budgets "LP SRAM 32KB:
{header} + ~31KB ring", and the only placement worth proving is the one the
ring will actually ask for.

Three details that make a "survived" verdict mean something:

- The expected pattern is regenerated from the `seq` **NVS** remembers, not
  from the `seq` the region claims. A region whose header was wiped is still
  fully checkable, and a stale region left over from an earlier stage cannot
  masquerade as a survivor (each stage reseeds from `esp_random()`).
- `magic` / `len` / `hdr_crc` / `crc` are four independent bits in the
  verdict, so a corrupt header is distinguishable from a corrupt body.
- When the body CRC fails, the verdict carries `match` (bytes equal to the
  expected stream), `bad` (first mismatching offset) and `nz` (non-zero byte
  count). `nz == 0` says "wiped to zeros"; a high `nz` with a low `match` says
  "somebody else is using this memory".

The pattern is written before the header, so a reset in the middle of a fill
cannot leave a valid header over a half-written body.

### Where the region lands (linker evidence)

`RTC_NOINIT_ATTR` is `_SECTION_ATTR_IMPL(".rtc_noinit", …)`
(`esp_common/include/esp_attr.h`), and IDF's `ld.rtc.sections` places
`.rtc_noinit (NOLOAD) … > rtc_data_seg`, which `esp32p4/memory.ld.in` aliases
to `lp_ram_seg`. There is no path by which this lands anywhere but LP RAM: the
same file `ASSERT`s that the RTC_FAST length fits `LENGTH(rtc_data_seg)`, so an
over-large region fails the **link**, it does not silently relocate.

From the current `build_tab5/esp32p4-mqjs.map` (before this change), LP RAM is
almost entirely unused:

```
lp_ram_seg      0x50108018   0x00007fe8        (32,744 B usable, ends 0x50110000)
lp_reserved_seg 0x50108000   0x00000018

.rtc.text       0x50108018   0x68     RTC wake stub
.rtc.data       0x50108080    0x0
.rtc.bss        0x50108080    0x0
.rtc_noinit     0x50108080    0xc     <- 3x uint32 from mqjs/tailscale_adapter.c
.rtc.force_slow 0x5010808c   0x24     esp_hw_support/sleep_modes.c
                             ====
                             0x98 = 152 B used, 32,592 B free
```

So the plumbing already exists and 31,744 B fits with **848 B to spare**
(`_rtc_force_slow_end` moves from 0x501080b0 to ≈0x5010FCB0). **The map of the
device build must be re-checked** — expected: `.rtc_noinit` at 0x50108080 with
size 0x7C0C (31,756 = 0xc + 31,744), still inside `lp_ram_seg`. If it is
anywhere else, that is a finding to report, not to accept.

Two consequences of claiming the region, both accepted:

- `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y` puts the LP RAM left over
  after the `.rtc.*` sections into the heap, so this takes ~31 KB of
  `MALLOC_CAP_RTCRAM` away. Nothing in this firmware allocates from it.
- The 848 B remainder may be too small for `multi_heap_register`, in which case
  `heap_caps_init` logs the region and leaves `heap->heap == NULL`; every
  allocation path skips such an entry (`heap_caps_base.c:146`), so it is not
  fatal. If the device build objects anyway, drop the region to 30 KiB — one
  `#define` in `term_lp_probe.h`.

The region is linked **unconditionally**, in a build where the probe is inert.
That is on purpose: whether the linker can place 31 KiB there at all is part
of what phase 3 needs to know, and it is exactly the allocation the ring will
make.

### The sequence

Four causes, in this order (the index is persisted, so appending is safe and
reordering invalidates older results):

| # | cause | trigger | expected `esp_reset_reason()` |
|---|---|---|---|
| 0 | `panic` | `volatile` read of address 0 → LoadProhibited | `ESP_RST_PANIC` (4) |
| 1 | `task_wdt` | `esp_task_wdt_add(NULL)` + a spin that never yields (starves idle too) | see the note below |
| 2 | `int_wdt` | `portDISABLE_INTERRUPTS()` + spin | `ESP_RST_INT_WDT` (5) |
| 3 | `restart` | `esp_restart()` | `ESP_RST_SW` (3) |

**Brownout is not in the table.** It is not software-triggerable, so it stays
untested and the report says so (`"untested":["brownout"]`). §11.3's own
fallback is what covers it: "消える cause があれば『その cause では lastboot
無し』と契約に明記して縮退" — the black box will document brownout as a cause
with no `lastboot`, on the safe assumption that a power loss takes LP RAM with
it (which is also what §4.4 already says: "LP は電源断で消える").

**The task WDT probably will not reset this firmware at all.**
`sdkconfig.tab5` has `CONFIG_ESP_TASK_WDT_PANIC` unset (`ESP_TASK_WDT_EN=y`,
`TIMEOUT_S=5`), so a triggered task watchdog prints a backtrace and continues.
That is itself worth recording: in this configuration `ESP_RST_TASK_WDT` is
unreachable and "does the black box survive a task WDT" has no content — what
actually resets a hung system here is the *interrupt* watchdog. So both
watchdog spins are **bounded** (20 s for the task WDT, four timeouts' worth;
5 s for the int WDT, whose timeout is 300 ms): if the cause does not reset the
chip, the probe records `"noreset":1` and moves to the next cause instead of
hanging the device. A cause that fails to fire is a result, not a stall.

### Sequencer design: two traps, and what avoids them

**Trap 1 — the region under test cannot be the sequencer's memory.** The cause
being measured may wipe it. So all state lives in NVS (namespace `termlp`),
and every advance is `nvs_commit`ed *before* the crash it enables:

| key | type | meaning |
|---|---|---|
| `step` | u8 | index of the cause being exercised. **Present ⇒ a sequence is in progress**; absent ⇒ idle or done |
| `pend` | u8 | 1 ⇒ a cause has been fired and the next boot owes a verdict |
| `seq` | u32 | the PRNG seed written into the region for `step` — the authority the survival check compares against |
| `res` | blob | `lp_verdict_t[]`, one per judged cause; the count is the blob length / element size |

`pend` is set (and committed) **immediately before** firing, not when the stage
is set up. The 15 s delay window therefore cannot produce a false verdict: a
reset that happens while the probe is waiting simply re-stages the same cause.

Boot flow (`term_lp_probe_boot()`, first statement of `app_main`):

1. no `step` key → do nothing (re-log any completed results and return). This
   is the inert-by-default path, and it is what a normal firmware does.
2. `pend` set → read `esp_reset_reason()`, judge the region against `seq`,
   append the verdict, clear `pend`, advance `step`. After the last cause:
   erase `step`/`pend`, commit, leave the results in NVS, stop.
3. re-fill the region with a fresh pattern for the current `step`, commit the
   state, start the trigger task.
4. trigger task: log a countdown for **15 s**, re-read `step` (a `clear` during
   the window stands the sequence down instead of crashing), commit `pend=1`,
   fire. If the cause returns, record `noreset` and stage the next one.

The 15 s delay is a **safety invariant, not a tuning knob**: it is the window
in which a bad build can be reflashed, and the window in which
`sys.lpProbe("clear")` can abort. Do not shorten it.

**Trap 2 — the dev-slot script re-runs on every boot,** including the four
boots the probe itself causes. So arming is one-shot *in C*, not in JS:
`term_lp_probe_arm()` returns false and changes nothing when a sequence is in
progress or when results are already present. `tools/probe_lp.js` is
convergent on top of that — it publishes results if it finds them, reports
progress if a sequence is mid-flight, and otherwise arms once.

Reporting is over **MQTT** (`<base>/proberep`), the project norm. Every verdict
is *also* `ESP_LOGW`n at boot as a serial backup, and an idle boot with results
re-logs them, so a serial capture alone is enough to read the outcome if MQTT
is unavailable.

### JS surface

```js
sys.lpProbe()          // -> JSON string: state + verdicts, changes nothing
sys.lpProbe("arm")     // one-shot arm; no-op if running or if results exist
sys.lpProbe("clear")   // erase state + results; also aborts a pending fire
```

All three return the report, so a caller never needs a second call to see what
happened. The report shape is documented in `term_lp_probe.h`; `obs_id` (the
raw `esp_reset_reason()`) is the authoritative field and `obs` is its name.

### How to run it (orchestrator)

1. Push `tools/probe_lp.js` to the dev slot. It publishes
   `{stage:"armed", region, addr, delay_s, causes}` and stops. **The device now
   crash-cycles on its own**: 4 resets, ~15-20 s apart, one to two minutes
   total. It is normal for the device to reboot repeatedly during this.
2. Re-push `tools/probe_lp.js` (or wait for the dev-slot re-run and read the
   `{stage:"running"}` reports on the way). Once four verdicts exist it
   publishes `{stage:"results", rep:{…}}`.
3. Cross-check `addr` in the report against `.rtc_noinit` in the device
   `.map` — the report's address is read from the linked symbol, so agreement
   proves the probe measured the memory the map says it did.
4. Restore `tools/dev_idle.js`. `sys.lpProbe("clear")` before any re-run.

If a boot loop turns out to be unrecoverable (it should not — the 15 s window
is there for this), reflash: the state lives in NVS, so an erase of the NVS
partition also disarms it.

### Results

Filled from the device run 2026-07-30 (Tab5, region 31,744 B at 0x5010808c,
sequence armed via tools/probe_lp.js over MQTT, collected the same way;
raw JSON in the session transcript):

| # | cause | observed reset | magic | len | hdr | crc | match / 31712 | first bad | nonzero | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | panic | panic (id 4) | 1 | 1 | 1 | 1 | 31712 | — | 31586 | **full survival** |
| 1 | task_wdt | *no reset occurred* (noreset=1) | — | — | — | — | — | — | — | **cause cannot fire**: CONFIG_ESP_TASK_WDT_PANIC unset, TWDT prints and continues (predicted in §1) |
| 2 | int_wdt | int_wdt (id 5) | 1 | 1 | 1 | 1 | 31712 | — | 31592 | **full survival** |
| 3 | restart | sw (id 3) | 1 | 1 | 1 | 1 | 31712 | — | 31585 | **full survival** |
| — | brownout | *not software-triggerable* | — | — | — | — | — | — | — | untested → degraded contract (§11.3) |

Map check: `.rtc_noinit` at 0x50108080 size 0x7c0c, inside `lp_ram_seg` (y/n): **y**
(probe payload at 0x5010808c = section base + the pre-existing 12 B of mqjs guards,
matching the linker-script prediction exactly).

**Decision (per "What the answers decide")**: every cause that actually resets
this firmware preserves the region bit-perfectly → §4.4's ring is implementable
as written; the region-level CRC suffices, no per-record CRC for correctness.
Boot marker still distinguishes "wiped" (brownout/power loss) from "empty".
**Open product question for the user**: with TASK_WDT_PANIC off, a task-WDT
hang never reboots — so it also never produces a `lastboot`. Enabling it would
align with the design's "WDT が落とす直前に何が出ていたか" motivation, but it
changes device behaviour on hangs (reset instead of limp-along) and is not
this component's call.

What the answers decide:

- **All four survive intact** → §4.4's ring is implementable as written; no
  per-record CRC is needed for correctness, only the region-level one.
- **Some cause corrupts a few words** → the ring needs per-record integrity so
  a partial region still yields the records that are whole.
- **Some cause wipes the region** → that cause is documented as having no
  `lastboot`, per §11.3's degraded contract, and the boot marker written on
  every boot becomes the way a reader tells "wiped" from "empty".

### Off-device verification (done)

- **Pure logic**, throwaway host harness (ASan+UBSan, `-std=c99 -Wall
  -Wextra`, not a repo file): `sizeof(payload) == 31744`; an intact region
  scores `match = 31712, bad = -1, flags = magic|len|hdr|crc`; two flipped
  bytes at offset 20000 give `match = 31710, bad = 20000` with the header bits
  still set and `crc` clear; a zeroed region gives `magic = 0`, `nz = 0` and
  121 incidental matches (≈31712/256, the rate at which the expected stream
  emits a zero byte — the number a "wipe" verdict should look like); a region
  filled from the *wrong* seq has all four flag bits set yet
  `match = 103`, `seq_seen != seq_exp` — a stale region cannot pass as a
  survivor; corrupting only `magic` clears `magic`+`hdr` and leaves `crc` set
  and `match` full. `term_lp_probe_report` round-trips through `JSON.parse`,
  and a too-small buffer yields `{"state":…,"error":"truncated"}` rather than
  half a document.
- **Host suites**: `host_test/run_tests.sh` → **22 suites, ALL SUITES PASSED**
  (the runner globs the parent directory, so `term_lp_probe.c` is compiled into
  every suite; it builds clean off-device with no ESP-IDF headers reachable).
- **PC build**: `run_pc` links with `../term_core/term_lp_probe.c` added to the
  phase-2 recipe; `pc_term_basic` → **ALL PASS**, which is the check that the
  ROM regen and the binding table are sane. `sys.lpProbe()` under `run_pc`
  returns `{"state":"idle","supported":0,"region":31744,…,"results":[]}`,
  `sys.lpProbe("arm")` is inert off-device, and a non-string argument still
  returns a string.
- **Not run** (orchestrator's steps, deliberately): the device build, the
  flash, and any push to the device.

### Decisions the header left open, and how they went

1. **The boot hook is in `app_main`, not lazy like `term.*`.** Phase 2 kept
   `main/` untouched because the registry can come up on first use. This
   cannot: the sequence has to continue across four crashes whether or not the
   JS runtime, Wi-Fi or MQTT came up on any given boot, and a probe that
   depends on the thing it is crashing is not a probe. It is the first
   statement of `app_main` so the region is judged before anything else in boot
   could touch LP RAM and so a verdict reaches the log even if a later boot
   stage fails.
2. **`term_lp_probe.c` is host-neutral, not `ESP_PLATFORM`-guarded whole.**
   `term_port_freertos.c` and `term_ui_tab5.c` compile to nothing off-device
   because they are pure device glue. Here the interesting part — CRC, pattern,
   verdict arithmetic — is portable, and it is the part a host harness can
   actually check. So only the RTC region, the NVS store, the triggers and the
   task are guarded; off-device the store reports "unavailable" and every entry
   point is inert, which keeps `sys.lpProbe()` answerable in `run_pc` and keeps
   the pure logic linked (and therefore compiled and warning-checked) in all 22
   host suites. The `typedef int …_tu_t;` of the phase-2 rule is still there.
3. **CRC32 is hand-rolled, not `esp_rom_crc32_le`.** Two lines of table-less
   loop, and in exchange the host and the device compute the same check with no
   ROM symbol and no shim. The polynomial is the same (0xEDB88320, reflected).
4. **`sys.lpProbe` returns a JSON *string*, not an object.** The report is
   built in C so the shape lives next to the data, the probe script forwards
   the parsed object wholesale (a field added in C needs no JS change), and it
   goes onto MQTT as-is. It also keeps the binding free of the nested
   `JS_NewString` inside `JS_SetPropertyStr` hazard entirely — there is one
   allocation, at the end, with nothing live to move.
5. **`clear` exists although the brief only asked for read and arm.** Results
   are sticky by design (arming is refused while they exist, so the four
   verdicts cannot be lost to an accidental re-arm), which means there has to
   be a way to release them. It doubles as the abort button inside the 15 s
   window, which is worth more than the one line it costs.
6. **A `step` value outside the cause table clears the state** rather than
   guessing. That can only come from a stale NVS blob written by a different
   build; continuing from it would fire an unintended trigger.

---

## 2. LP SRAM black box ring (§4.4)

**Blocked on §1's device run.** Not started, deliberately — §11.3 forbids
implementing it on an unverified assumption. The seam it will tee from already
exists: `term_registry_system_log(writer, …)` carries the `writer_id` of
§4.4's record tag (accepted and not stored today).

Open questions that §1's results will settle, plus the ones §12 already lists:

- per-record CRC or region-level only (depends on whether any cause corrupts
  rather than wipes)
- where SGR stripping happens (§12: "ingest 時に属性ラン化した後なら素のテキス
  トは手元にあるはず — 実装時に確認")
- the 8 KB system / 23 KB app static split of §4.4 against the 31 KiB the probe
  proves is placeable

## 3. MQTT responder + signature gate (§7.3)

Not started. Depends on §2 for `lastboot`; the accepted-counter high-water mark
goes in **NVS, not LP SRAM** (§7.3 is explicit: LP is lost on power cut, so a
replay window would reopen after one power cycle).
