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

Status: **§1 (the retention probe) done and device-verified 2026-07-30; §2 (the
ring, the boot-time `lastboot` capture and the JS introspection) implemented and
verified off-device, device run pending. §3 (the MQTT responder) not started.**

The probe of §1 has been **removed from the firmware** now that it has answered
its question — it claimed the same LP region the ring needs, and the device must
never have two owners of that memory. Its results below stand as the record, and
the ring's boot marker is its permanent successor (§2, "Probe reconciliation").

| Column | Meaning |
|---|---|
| Read by tests | `yes` = the test author may open it. `NO` = implementation. |
| Status | `done`, `wip`, `planned` |

---

## 1. LP SRAM retention probe (§11.3 prerequisite) — DONE, THEN REMOVED

**The files described in this section no longer exist.** `term_lp_probe.{c,h}`,
`tools/probe_lp.js`, the `sys.lpProbe` binding and the `app_main` hook were
removed when the ring took the region (§2). Everything below is kept as the
record of what was measured and how, because the ring's whole licence to exist
is the "Results" table; to re-run a dedicated crash sequencer, restore the files
from commit **77b1c99** — but read §2's "Probe reconciliation" first, because the
ring measures the same thing continuously and on real data.

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

## 2. LP SRAM black box ring (§4.4) — implemented, off-device verified

Status: **implementation complete, host + run_pc verification done, device build
/ flash / device run are the orchestrator's steps (deliberately not run here).**

### Contract surface (test author reads these)

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_lp_ring.h` | The whole black box: region layout, record/header/partition structs, the strip rule, format/open/append, the iterator, the singleton, stats and the two JSON shapes. Six numbered properties (P1-P6) at the top are the invariants to test against | yes | done |
| `term_registry.h` | `term_wclass_t` + `term_registry_platform_log` (new), and the tee note on `term_registry_log` | yes | done |
| `PHASE3_MANIFEST.md` | This ledger | yes | done |
| `docs/term-design.md` §4.4/§5/§7/§12 | Ground truth | yes | — |

### Implementation (test author must NOT read)

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_lp_ring.c` | Pure ring logic (CRC32, strip, validate, format, append, iterator, JSON) unguarded; the `RTC_NOINIT` region, `esp_reset_reason`, the clock and the PSRAM snapshot allocator `ESP_PLATFORM`-guarded, with inert host equivalents | NO | done |
| `term_registry.c` | `log_locked` split out; the tee in `term_registry_log` (TERM_LOG only) and in `term_registry_platform_log`; `term_lp_ring_boot()` from `term_registry_init` | NO | done |
| `components/mqjs/mqjs_runtime.c` | `js_sys_blackbox`; the print sink now passes a `term_wclass_t` and no longer needs a status-bar sink to tee | NO | done |
| `components/mqjs/device_stdlib.c` | `sys.blackbox` replaces `sys.lpProbe` in the `js_sys[]` table | NO | done |
| `components/mqjs/gen/device_stdlib.h` | ROM stdlib regen (-m32). `gen/mquickjs_atom.h` regenerates content-identical and was restored with `git checkout --` (CRLF trap) | NO | done |
| `main/app_main.c` | `term_lp_ring_boot()` as the first statement, replacing the probe's hook | NO | done |
| `CMakeLists.txt` | `term_lp_ring.c` in, `term_lp_probe.c` out, `nvs_flash` dropped from `PRIV_REQUIRES` (the probe was its only user) | NO | done |

### The region, and where the numbers come from

```
offset   size    contents
0        128     term_lp_hdr_t slot A      double-buffered (P5)
128      128     term_lp_hdr_t slot B
256      256     writer table: 8 x 32-byte names
512      8192    SYS partition   §4.4 "システム区画 8KB"
8704     23552   APP partition   §4.4 "アプリ区画 23KB"
------------------
32256            TERM_LP_REGION_BYTES
```

LP RAM is 32,744 B usable and IDF's own `.rtc.*` sections plus the 12 B of
pre-existing `.rtc_noinit` take ~152 B, so this leaves **~336 B of slack**
(§1 measured the probe's 31,744 B fitting with 848 B to spare). The linker
ASSERTs the fit, so an over-large region fails the build rather than
relocating; if the device link ever objects, `TERM_LP_APP_BYTES` is the one
number to reduce. **To re-check on the device build:** `.rtc_noinit` at
0x50108080 with size 0x7E0C (32,268 = 0xc + 32,256), still inside
`lp_ram_seg`.

Sizes worth knowing: record header 8 B, records 8-byte aligned and sized,
payload cap 512 B, so ~68 B for a typical 60-byte log line (12% framing) and
~120 live records in the SYS partition, ~340 in the APP partition at that
size. `.bss` cost: one 512-byte staging buffer plus two 184-byte handles.

### Decisions

1. **SGR stripping happens in `term_lp_ring_append()`, not in the parser
   (§12's open question, resolved).** §12 guessed the plain text would be at
   hand "after attribute run-splitting at ingest". It is not: ingest copies
   raw bytes into the per-term byte ring and the parser runs later, on the UI
   frame task, producing attributed *cells* — there is no run-of-text stage
   anywhere. Three reasons the tee therefore sits at the line-oriented ingest
   instead: (a) re-serialising cells back to text would undo the parser's work
   and lose line grouping; (b) the parser runs a frame or more after the
   writer spoke, so a device that dies before the next frame would lose
   exactly the last words the black box exists for; (c) the parser sees class
   B (SSH) bytes, so class separation would become a filter instead of a
   structure. The strip is *inside* append rather than at the call site, so no
   caller can write raw bytes even by mistake.

   **It strips every escape sequence, not only SGR.** That is a security
   property: the black box is pulled off the device and printed on the
   operator's terminal, so a retained escape would let any app that can call
   `print()` inject terminal control into the reader's session. Also dropped:
   the C0 controls except `\n`/`\t`, and DEL. Everything >= 0x20 passes
   through byte for byte, so UTF-8 survives; a truncation at 512 B backs off
   to a codepoint boundary so a pulled log is always valid UTF-8.

2. **`lastboot` is frozen by copying the region image to PSRAM at boot, not
   by sealing in place.** LP SRAM is the irreplaceable resource — 32 KiB,
   no second source — and sealing would permanently halve the live ring to
   protect the abundant kind of memory. §4.1's budget is 1.3 MB of PSRAM for
   eight terminals; 31.5 KB more is 2.4% of that and 0.1% of the chip's PSRAM.
   The copy is allocated **only when there is something to freeze**, so a cold
   power-on pays nothing, and it is a flat byte copy, which means the frozen
   image has the same layout as the live one and **one iterator serves both
   sources** — the reason the MQTT responder of §3 needs no second decoder.
   Allocation failure is `TERM_LP_PREV_NOMEM`: no lastboot, everything else
   still works. PSRAM only (`MALLOC_CAP_SPIRAM`), never the internal SRAM the
   size diet won back.

3. **Probe reconciliation: the probe module is deleted, not repointed.** The
   brief allowed either. Deleting wins because (a) a crash sequencer that can
   still reboot the device four times while measuring a `.bss` buffer is a
   hazard with no remaining benefit, and (b) the ring is a strictly better
   instrument for the same question: the boot marker records this boot's
   `esp_reset_reason()` next to the verdict on the previous image, so
   `retention: ok | cold | lost | corrupt` is computed on **real log data,
   every boot, forever** — including the two causes the probe could not reach
   (brownout, and a task WDT if `CONFIG_ESP_TASK_WDT_PANIC` is ever enabled).
   `retention: "lost"` means a reset that should have preserved LP SRAM found
   the region wiped, and it is also `ESP_LOGW`n at boot with an explicit
   marker. §1's results stay in this file; the code is one `git show 77b1c99`
   away.

   **Orchestrator note:** the dev slot may still hold `tools/probe_lp.js` from
   the §1 run. `sys.lpProbe` no longer exists, so that script would now throw
   on the dev slot after a flash — re-push `tools/dev_idle.js` (or the
   blackbox probe) as §1's step 4 already said.

4. **Region-level integrity only, as §1's results license — but two-level.**
   `hdr.crc` covers the 124-byte header (recomputed per append over a DRAM
   shadow, not over 31 KB of LP SRAM), and the content is validated
   *structurally*: both record chains must walk from `tail` to `head`
   consuming exactly `used` bytes, with every `len` in range, every class
   valid, no record straddling a partition end, and a record count that
   matches the header. Payload bytes are not checksummed — §1 measured
   retention as bit-perfect or absent, never subtly rotten, so a per-record
   CRC would only buy what the chain walk already catches. The writer table
   has its own CRC over just the interned entries, and a failure there loses
   **names only** (records still read, writer shows `?`).

5. **The header is double-buffered and content is only ever written into
   published free space (P5).** Without this, a crash during the append that
   evicts the oldest record would overwrite the record the live header still
   calls `tail`, and the chain walk would then reject the *whole* region —
   losing the entire black box at the one moment it matters. So: evict, then
   publish the advanced tail to the other header slot, then write the record,
   then publish head. A reader takes the valid slot with the greatest `hseq`.
   Cost: 128-256 B of header writes per record on a slow bus (§4.4 budgets
   "a few KB/s"); benefit: a torn append costs the record in flight and
   nothing else, which the host harness verifies directly by corrupting the
   final header slot and the in-flight payload of a real append.

6. **No lock in the ring; the registry's table mutex is the serialisation.**
   The tee already runs under it, the lock order is registry -> ring, and the
   ring never calls back — so there is no second order to get wrong. Adding a
   mutex here would recreate the nested-lock question this project has already
   paid for. `term_lp_ring_boot()` runs before any other task can reach the
   ring, and the one path that can reach it with no registry (a `print()`
   before bring-up) is js_task-only.

7. **`term_registry_system_log`'s signature is unchanged; the class arrives
   through a new sibling.** The phase-2 host suites call it with three
   arguments and `host_test/` is off limits, so the class channel is
   `term_registry_platform_log(writer, wc, text, len)` and `system_log` is
   now `platform_log(writer, TERM_WCLASS_APP, …)`. The caller decides the
   class because only the runtime knows whether a writer is a trusted system
   app (`MqjsWorker::trusted_system`); the registry does not guess it from the
   name. Platform code with no app on the stack also counts as system class.

8. **The tee is more reliable than the console, on purpose.**
   `term_registry_platform_log` records to the black box **before** it looks
   for a console term and **even when the registry is not initialised** — the
   lines from a boot that failed before bring-up are the ones worth having. It
   also cost one change in `mqjs_runtime.c`: `out_write` no longer skips line
   assembly when no status-bar sink is installed, because the flight recorder
   must not depend on whether the host wired up a UI sink (and run_pc, the one
   build without that sink, is where the tee is testable). The return value of
   `platform_log` still describes the console write only, so the existing
   suites' expectations (`NOT_READY` with no console, `OK` with one) hold.

9. **The ring is self-starting.** Every singleton entry point calls
   `term_lp_ring_boot()` first, so a build that forgets the `app_main` hook
   still records — it only gets a later timestamp on the boot marker. Nothing
   can overwrite the retained image before boot runs, because this module is
   the region's only writer. This is also what makes `term_registry_init`'s
   call (and therefore the 22 host suites and run_pc) exercise a real ring.

10. **Class B is unreachable, not filtered (P3).** The only entry into the
    ring is the line-oriented log path. `term.feed`, the byte ring, the
    parser and the (phase-4) ssh pipe never call append. `term.log` on a
    **TERM_VT** term does not tee either, per §3.2's table row — and a term's
    mode is fixed at create (a re-attach with a different mode is
    `TERM_ERR_MODE`), so that gate cannot drift. Preedit never enters term
    state at all (§10.2).

11. **A record is one ingest call, newlines included; `\r` is dropped.** The
    tee sees the text before `term_registry_log` adds CRLF, so a record is
    normally exactly one line. An app that hands `term.log` an embedded `\n`
    gets it back inside one record (rendered as `\n` in the JSON dump). The
    alternative — splitting into several records — would multiply the 8-byte
    framing and attribute the halves as separate lines.

12. **The writer table interns 8 names of 32 bytes, and overflows to
    `WRITER_UNKNOWN` rather than truncating.** §4.4 wants a structural
    `{writer_id, class}` tag with no prefix parsing; a 32-byte name in every
    record would cost more than half the payload of a typical line, and
    truncating names would let two distinct apps present the same identity
    (the same argument that made `TERM_OWNER_MAX` 32). Interning also survives
    the eviction of the record that introduced the name. A ninth writer's
    records read `?` — never somebody else's identity — and `unnamed` counts
    it. Names are sanitised (controls, `"` and `\` become `_`), which is what
    lets `TERM_OWNER_SYSTEM` ("\1system") appear as `_system` and keeps the
    JSON dump well-formed.

13. **A name too long to store whole takes the table-full path, not a
    truncation** (fix; the contract suite found it). Decision 12 said the
    table "overflows to `WRITER_UNKNOWN` rather than truncating" and
    `term_lp_ring.h` says it twice — "Names are stored whole, never truncated:
    two distinct apps must not be able to present the same writer_id" and "a
    record gets TERM_LP_WRITER_UNKNOWN rather than somebody else's identity".
    The first implementation honoured that for the *ninth* writer and broke it
    for a *long* one: the sanitiser silently cut at 31 bytes, so two 40-byte
    names differing only after byte 33 interned to one entry and shared a
    writer_id (`writers=1`, `unnamed=0` — the collision was not even counted).
    Found by the phase-3 contract suite (`test_lp_writers`,
    "a name that cannot be stored whole never becomes another identity"), which
    asserted the header's claim without the reachability qualifier the
    implementation had quietly assumed.

    Fixed by making the sanitiser *fail* instead of truncating: a name that
    does not fit whole into a 32-byte NUL-padded entry gets
    `TERM_LP_WRITER_UNKNOWN` and `unnamed++`, exactly as a ninth writer does.
    Not reachable from an app today — `TERM_LP_WRITER_MAX == MQJS_APP_NAME_MAX`
    bounds every real writer at 31 bytes — so this is defence in depth, but the
    header states the property with no qualifier and a header that overstates
    is worse than one that is silent. It is also phase 2's decision #1
    restated: **reject, never truncate**, because a truncated identifier is
    indistinguishable from somebody else's.

14. **`t_ms` is milliseconds since boot, not wall time.** After a reset the
    only honest statement about a record is how long after boot it was
    written; it wraps at 49 days of uptime. Records are emitted SYS-partition
    first, then APP, so the dump is per-partition chronological, not globally
    sorted — merging the two into one timeline is a reader-side choice the
    responder can make.

### JS surface

```js
sys.blackbox()               // stats JSON: any app (telemetry, like sys.heap)
sys.blackbox("live")         // this session's tail   \ dev slot or embedded
sys.blackbox("lastboot")     // the frozen previous  / system app ONLY
sys.blackbox(what, from)     // skip `from` records; reply carries "next"
```

- Both content reads throw a `TypeError` for any other app rather than
  returning an empty answer, because an empty answer looks like an empty
  black box. The gate is the dev slot **or** `trusted_system`: a dev-slot push
  arrives by the same Ed25519 signature as the MQTT responder's requests, i.e.
  the device owner's key, which is exactly §7.2's gate for this data.
- Stats carry no content and **no writer names** — counters only.
- Both return a JSON *string* (the §1 decision #4 precedent): the shape lives
  in C next to the data, a probe script forwards the parsed object wholesale,
  and there is exactly one allocation with nothing live to move.
- The shapes are documented in `term_lp_ring.h`. `retention` is the field to
  read for "did LP SRAM survive this reset".

### What the MQTT responder (§3) will consume

Nothing new: `term_lp_report()`, `term_lp_dump_json(src, from, …)` or, for a
custom wire format, `term_lp_iter_begin/next` over
`term_lp_source(TERM_LP_SRC_LASTBOOT)`. The frozen source is immutable, so the
responder needs no lock and its chunk cursor is exact; against LIVE the cursor
is best-effort by construction, which is why forensics pull `lastboot`. The
responder adds the signature gate and the NVS replay counter (§3) and no state
here.

### Off-device verification (done)

- **Throwaway host harness** (ASan+UBSan, `-std=c99 -Wall -Wextra`, not a repo
  file), 10 groups, **ALL RING CHECKS PASSED**:
  - *layout*: `sizeof(term_lp_hdr_t) == 128`, `term_lp_rec_t == 8`,
    `term_lp_part_t == 32`, region 32,256, SYS@512, APP@8704, handle 184 B.
  - *strip*: SGR, OSC (BEL- and ST-terminated), DCS, nF (`ESC ( B` vanishes
    whole — an early version left a stray `B`, which this test caught), an
    unterminated CSI eating the rest, `\r`/BEL/DEL dropped, `\n`/`\t` kept,
    UTF-8 byte-exact, truncation backing off to a codepoint boundary,
    measure-only mode.
  - *format/open*: a virgin region reports `zeroed`, one stray byte makes it
    `garbage` instead, a formatted region validates on all six bits, an
    unaligned or short region is refused rather than coerced, a read-only
    handle refuses appends.
  - *append/partitions*: SYS and APP routed by class, escapes stripped on the
    way in, an all-escape line refused and counted, `TERM_LP_CLASS_PAD`
    refused from a caller, iteration oldest-first over one or both partitions,
    a fresh `open` of the same bytes seeing the same records.
  - *writers*: 8 interned then 3 unnamed with `?` (never a wrong identity),
    NULL writer, control/quote sanitisation, the same writer twice interning
    once.
  - *truncation*: a 1,024-byte line stored as 512 with `TERM_LP_F_TRUNC`.
  - *wrap/evict*: 300 x 96-byte records through an 8,192-byte partition
    (~3.8 laps, PAD exercised every lap) — 78 survivors, 222 evicted, chain
    still valid, **the survivors are the newest records in order** (checked by
    decoding the sequence number in each payload), and the APP partition
    untouched at 0 bytes, which is P1's static split demonstrated.
  - *corruption*: both header slots wrecked -> no open; the **stale** slot
    wrecked -> the live one still wins and all 20 records read; a smashed
    record header mid-chain -> `chain_ok` false while `crc_ok`/`geometry_ok`
    stay true, and the iterator stops instead of running away; a corrupt
    writer table -> `ok` true, `wt_crc_ok` false, records intact with `?`.

    **Correction to an earlier line here** (it read "one flipped bit in the
    live header -> `crc_ok` false with `magic_ok` true"): that is only the
    outcome when **both** slots are damaged. Flip a bit in the *newer* slot
    alone and `open()` still SUCCEEDS with `crc_ok` **true**, because the
    double buffer hands the reader the older slot — which describes the same
    region one record earlier. That is not a degraded result, it is P5 working
    exactly as designed: the cost of a torn header write is the record in
    flight and nothing else. `crc_ok` therefore means "*a* slot validated",
    not "the newest slot validated"; the only way to make it false is to leave
    no valid slot at all. The suites cover both shapes (`test_lp_torn`).

    Where the other two integrity bits divide, for the same reason: a `used`
    that contradicts head/tail arithmetic ((tail+used)%cap != head) is a
    **geometry** failure, caught from the header alone before any record byte
    is read; `chain_ok` covers only what the record walk can see (classes,
    lengths, no straddling, landing on `head`, the record count). `chain_ok` is
    not computed at all unless `geometry_ok` holds. Now stated in
    `term_lp_ring.h` on `term_lp_check_t` itself.
  - *reformat + freeze*: 40 records, copy out, re-format in place with
    `boot_seq+1`; the frozen copy keeps all 40 while the new session logs, and
    a reader of the re-formatted region is **not** fooled by the stale slot's
    much higher `hseq`.
  - *torn append (P5)*: after the partition is full, one more append, then
    (a) the final header store corrupted -> the region still validates and
    reads one record fewer, (b) the in-flight record's bytes smashed as well
    -> still validates, still reads the same set, (c) the untouched region
    shows the completed record. This is the property the whole double-buffer
    exists for.
  - *singleton*: idempotent boot, `retained` false off-device, boot marker
    present, `retention: cold` on a zeroed region, JSON round-trips through
    `JSON.parse`, `"` and `\` escaped, a small buffer stops with `more: 1` and
    still closes its braces, a hopeless buffer yields
    `{"ready":1,"error":"truncated"}` rather than half a document.
- **Host suites**: `host_test/run_tests.sh` -> at the time of writing **22
  suites, ALL SUITES PASSED** (the runner globs the parent directory, so
  `term_lp_ring.c` is compiled into every suite and `term_registry_init` boots
  a real ring in all of them).

  **Superseded by the phase-3 contract suites.** The test author's nine
  `test_lp_*` suites bring the runner to **31 suites / ~28.4k checks**; after
  decision 13's fix, **30 pass and `test_lp_writers` has one red check left**,
  at `test_lp_writers.c:222`. That check is not a defect report — it is
  unsatisfiable alongside the block directly above it in the same case, which
  requires **both** over-long names to resolve to `"?"` (the header's rule, and
  what the implementation now does) while line 222 requires the two resolved
  names to **differ**. Two C strings cannot both equal `"?"` and differ, so no
  implementation can turn that case green. Flagged to the orchestrator; the
  suite is the test author's file and was deliberately not edited here.
- **run_pc**: builds clean with `../term_core/term_lp_ring.c` added to the
  phase-2 recipe; `pc_term_basic`, `pc_term_errors`, `pc_term_name`,
  `pc_term_resize` all **ALL PASS**, `pc_term_probe` reports ready, and
  `tools/smoke_examples.sh` (11 shipped examples) passes — no regression from
  the `out_write` change.
- **`sys.blackbox` through run_pc**: stats report `ready=1 retained=0
  region=32256 boot_seq=1 reset=unknown retention=cold`, `sys.blackbox("live")`
  returns the boot marker plus the app's own `print()` lines tagged
  `app/<appname>`, a `term.log` line arrives SGR-stripped, **a `term.log` on a
  TERM_VT term does not appear at all** (P3 observed from JS), `from` advances
  the cursor, `sys.blackbox("nope")` throws.
- **Not run** (orchestrator's steps, deliberately): the device build, the
  flash, and any push to the device.

### Open on the device

- **Is the LP region write-buffered from the HP core?** Everything here
  assumes a store to LP SRAM is visible to the next boot immediately, which
  §1's probe cannot distinguish (it crashed 15 s after writing). If P4's LP
  RAM were behind a write-back cache, the *last* record before a panic could
  be lost. The observable: whether the record immediately preceding a crash is
  present in `lastboot`. Expected present (LP RAM is outside the cached
  address ranges).
- **`.rtc_noinit` placement and size** in the device `.map` (expected 0x7E0C
  at 0x50108080), and that the ~336 B remainder does not upset
  `heap_caps_init` — §1 established that a region too small for
  `multi_heap_register` is logged and skipped, not fatal.
- The first real `retention` verdict after a panic: expected `ok` with
  `prev: captured`, which is §1's result restated on live data.

## 3. MQTT responder + signature gate (§7.3)

Not started. Depends on §2 for `lastboot` — and only on its **read** API:
`term_lp_report` / `term_lp_dump_json` / the iterator over
`term_lp_source(TERM_LP_SRC_LASTBOOT)`, all of which exist and are verified.
The accepted-counter high-water mark goes in **NVS, not LP SRAM** (§7.3 is
explicit: LP is lost on a power cut, so a replay window would reopen after one
power cycle). Note that `nvs_flash` was dropped from this component's
`PRIV_REQUIRES` when the probe went; the responder will need it back (or,
better, will live where the MQTT client already does).
