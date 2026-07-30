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
ring, the boot-time `lastboot` capture and the JS introspection) done and
device-verified 2026-07-30 (7ad253e); §2a (the panic reason in the SYS
partition) and §3 (the MQTT responder + signature gate, and
`tools/bb_pull.py`) implemented and verified off-device — device build, flash
and device run are the orchestrator's steps and were deliberately not run
here.**

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
**Product question RESOLVED by the user 2026-07-30**: TASK_WDT_PANIC stays
**off**. The user's rule: on failure, prefer preserving state for log
retrieval over automatic reset. A hung-but-alive device keeps its full RAM
and its live ring, both readable over MQTT (the §3 responder runs off the
task-MQTT context, not js_task, so it answers even when JS is wedged).
Corollary kept as-is: real panics continue to auto-reboot (IDF default) —
that IS the log-preserving path, because LP SRAM survives reset but not the
power cycle a user gives a halted device. Follow-up added to §2's scope:
the panic handler writes the panic reason into the SYS partition so
`lastboot` carries the cause, not just the boot marker's reset id.

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

### Open on the device — ANSWERED (device run 2026-07-30, tools/probe_blackbox.js)

All three closed by a two-boot run (plant marks → external esptool
watchdog reset → auto re-run publishes lastboot); raw JSON in the session
transcript:

- **LP writes are not buffered.** The record written moments before the
  reset (`sys: stop 'probe_bb'`) is present in `lastboot`, along with both
  planted marks (`app/probe_bb: BBMARK-print-77 …`, `BBMARK-termlog-77 RED
  plain` — SGR stripped as specified). Nothing between the last append and
  the reset was lost.
- **`.rtc_noinit` landed at 0x50108080 size 0x7E10** (4 B over the 0x7E0C
  prediction — alignment padding between the pre-existing 12 B guards and
  the ring struct), inside `lp_ram_seg`; boot clean, heap_caps un-upset.
- **boot 1 after flashing over the §1 probe firmware: `retention=corrupt,
  prev=garbage`** (the probe's PRNG image correctly judged not-a-ring and
  reformatted — the garbage-vs-empty distinction on real data). **boot 2:
  `retention=ok, prev=captured, lastboot=1`** with the full previous boot
  readable. Bonus: the reset used was `wdt (id 7)` — the RTC WDT that
  esptool's watchdog-reset fires, a cause §1 never measured — and the
  region survived it bit-perfectly too. Writer tags and the SYS/APP split
  visible end to end (`last: sys rec=2, app rec=4`); `dropped=0 trunc=0
  refused=0 unnamed=0`.

## 2a. Addendum: the panic reason (§4.4's "panic 理由") — implemented

§4.4 lists panic reasons as SYS-partition content and §2 shipped without them:
`lastboot` said what the device was saying, not what killed it. This closes
that, in one line per panic.

User rule that frames it: **on failure, preserve state for log retrieval
rather than auto-reset** — so `CONFIG_ESP_TASK_WDT_PANIC` stays off (a task-WDT
hang limps along and never produces a `lastboot`, §1's open question answered)
and the panic auto-reboot stays (LP survives a reset, not a power-off).

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_lp_ring.h` | `term_lp_panic_t`, `TERM_LP_PANIC_MAX`, `term_lp_panic_fmt`, `term_lp_panic_note` — the contract, including what makes the append panic-safe | yes | done |
| `term_lp_ring.c` | `bputhex`/`bputclip`, the formatter, the one-shot note | NO | done |
| `main/panic_note.c` | `__wrap_esp_panic_handler`: panic_info_t -> term_lp_panic_t, then `__real_` | NO | done |
| `main/CMakeLists.txt` | `panic_note.c` in `SRCS`, `-Wl,--wrap=esp_panic_handler` | NO | done |

### The line

```
panic: fault Load access fault task=js_task core=0 pc=0x4800f2a4 cause=5
panic: abort assert failed: foo bar task=IDLE core=1 pc=0x40001234 cause=2
```

Writer `panic`, class SYS, ≤ 159 bytes. `kind` is IDF's
`panic_exception_t` reduced to a word (fault/abort/iwdt/twdt/debug), `reason`
is the architecture's string (for an abort, `g_panic_abort_details` — the same
override `esp_panic_handler` makes a few lines later), `pc` is `info->addr`
(mepc), `cause` is `panic_get_cause(info->frame)` (mcause). No backtrace, no
registers, no stack: coredump exists separately and this is a flight-recorder
note. Fields are clipped individually so a long task name can never push the
numbers off the end — the numbers are the part a human cannot reconstruct.

### Decisions

1. **A linker wrap, because IDF 6 offers no hook.** `panic.c` calls nothing
   weak and nothing registrable on the panic path (the only `__attribute__
   ((weak))` symbols there are the reset-reason hint pair), and
   `esp_register_shutdown_handler` is **not** called on a panic — it runs on
   `esp_restart`, i.e. exactly the case that does not need this. So
   `-Wl,--wrap=esp_panic_handler`, which is sound here because the call site
   (`port/panic_handler.c`) is a different translation unit from the
   definition, and which is the mechanism `components/ui_tab5` already uses
   twice (`esp_hosted_init`, `lvgl_port_ppa_create`). `panic_info_t` comes
   from `esp_private/panic_internal.h`; `esp_system` is a common requirement,
   so no `REQUIRES` change was needed.

2. **The note is written BEFORE `__real_esp_panic_handler`.** After it there is
   no "after" — the real handler prints, feeds the WDTs, may write a coredump
   and then reboots. Going first means the line lands even if a later stage of
   the handler dies. The cost is symmetric and accepted: if *our* line were to
   hang, the operator loses the Guru Meditation print, but the RTC WDT is still
   armed at this point and reboots the chip, so the black box still holds
   whatever landed. This adds no new failure class: `panic.c` itself is
   flash-resident by default, so the panic path already assumes flash is
   reachable (the entry code re-enables the cache before calling it).

3. **Panic-safety is a geometry re-check, not a new lock or a raw entry
   point.** §2's decision 6 says the ring has no lock and term_registry's table
   mutex is the serialisation — and in panic context that guarantee is simply
   gone: interrupts are off, the other core was stalled by `esp_cpu_stall`
   wherever it happened to be, possibly inside `term_lp_ring_append`. The
   dangerous windows are the ones where the DRAM shadow header is *between*
   two updates (inside `lp_evict_one`, or after a PAD's `used +=` but before
   `head = 0`): appending from such a shadow would publish a header describing
   a region state that never existed, and `open()` would then reject the
   **whole** black box on `geometry_ok` — trading a few hundred lines for one
   line about the crash. So `term_lp_panic_note()` runs `lp_geom_ok()` on the
   shadow first and **refuses** if it does not hold. Losing the note is the
   cheaper failure and it is bounded to a crash landing inside the handful of
   instructions an append window spans. Outside those windows P5 already
   bounds the damage to the record in flight.

   What this deliberately does **not** do: allocate, take a lock, call
   FreeRTOS beyond `xTaskGetCurrentTaskHandleForCore`/`pcTaskGetName` (IDF's
   own panic-context idiom, from `panic_arch.c`), or log. The clock is
   `esp_timer_get_time()`, which is IRAM-resident for ISR use.

4. **One-shot, set before anything else.** A panic inside the panic handler
   must not walk this code again — at that point nothing about the ring can be
   trusted — and there is no "later" to reset the flag for.

5. **The formatter is separate from the append** (`term_lp_panic_fmt`) so the
   whole of the interesting logic is pure and host-testable; the wrap is 30
   lines of translation in `main/`, where every other piece of system
   integration in this phase lives. The `ESP_PLATFORM` guard is therefore not
   needed — the device-only code is in a device-only component.

6. **`term_lp_panic_fmt` returns the length it actually WROTE, not `cap - 1`.**
   `test_lp_panic`'s "…and the length it returns is the length it actually
   wrote" case (`host_test/test_lp_panic.c`, formatting into a buffer pre-filled
   with `0xAA`) caught the original: on a `cap` too small for the whole line the
   function clamped `b.n = b.cap`, so the bytes between the end of the text and
   `cap - 1` were never written — and the header's "returns the length written"
   invited the caller to pass that length to `term_lp_ring_append()`, i.e. to
   publish its own uninitialised buffer into the black box. 34 `cap` values in
   [32, 73] were affected. The fix is one deleted clamp: `bput()` is
   all-or-nothing, so `b.n` already IS the write offset — terminate there,
   return that, and an over-tight buffer simply stops at the last field that
   fitted whole. Decision 5 is what made this findable at all: the bug lived in
   the pure half. **No device impact today** — the only caller,
   `term_lp_panic_note()`, passes a full `TERM_LP_PANIC_MAX` buffer that the
   fields are bounded to fit — so this was a contract violation waiting for the
   second caller, which is the kind of thing a suite is for. The header now also
   states the truncation rule and that `cap < 32` is the "hopeless" threshold.

### Off-device verification

In the phase-3 harness below, section [8]: the exact line for a fault and for
an all-NULL info, clipping (a 299-byte kind/reason/task still yields a line
under `TERM_LP_PANIC_MAX` **with `pc=` and `cause=` intact**), a hopeless
buffer writing nothing rather than half a line, NULL guards, the append
landing in the SYS partition and reading back as `sys/panic: panic: abort
assert failed …` through `term_lp_dump_json`, and the one-shot refusal.

**Not host-reachable, and honestly so:** the `lp_geom_ok` refusal path. `s_live`
is static, so a host test cannot corrupt the shadow between two appends. The
guard reuses the exact predicate `term_lp_ring_open()` applies (covered by
`test_lp_format`'s geometry cases), so what is untested here is the two-line
call, not the check.

**Device-side to confirm when the orchestrator runs it:** trigger a panic
(`sys.panic()` from the dev slot — §3 decision 12), reboot, pull `lastboot`, and expect
one `sys/panic:` record of kind `fault` whose `pc=` matches the address in the
serial Guru Meditation block. That cross-check is the whole point: the same
fact from two independent paths. `task=` will read `esp_timer`, because that is
where `sys.panic()`'s armed fault fires; a real crash inside a JS callback is
what names the JS worker.

---

## 3. MQTT responder + signature gate (§7.2) — implemented, off-device verified

Status: **implementation complete, host verification done (266-check throwaway
harness + 31 host suites + 5 run_pc suites), device build / flash / device run
are the orchestrator's steps (deliberately not run here).**

### Contract surface (test author reads these)

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_bb_pull.h` | The whole responder: the five properties (Q1-Q5), the request and reply wire formats, the error enum, `term_bb_req_parse` / `term_bb_req_gate` / `term_bb_serve`, and the four-callback environment | yes | done |
| `term_lp_ring.h` | `term_lp_dump_json_ex` + `term_lp_dump_info_t` (new, see "The one header change"), and `term_lp_panic_*` (§2a) | yes | done |
| `PHASE3_MANIFEST.md` | This ledger | yes | done |
| `docs/term-design.md` §7 | Ground truth (annotated with what implementation decided) | yes | — |

### Implementation (test author must NOT read)

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_bb_pull.c` | Parse, gate, chunk loop, JSON framing. Pure C99: no printf, no allocation, no ESP-IDF, nothing to guard | NO | done |
| `term_lp_ring.c` | `term_lp_dump_json_ex` (the old entry point is now a one-line wrapper) | NO | done |
| `main/task_source.c` | The device half: the `<base>/bb` subscription and route, `crypto_sign_open` with `MQJS_TASK_PUBKEY`, the NVS high-water mark, the QoS 0 reply publish, the `/status` refusal line | NO | done |
| `CMakeLists.txt` | `term_bb_pull.c` in `SRCS` (no new `PRIV_REQUIRES` — see decision 1) | NO | done |
| `components/mqjs/mqjs_runtime.c` | `js_sys_panic` — the drill button of decision 12 (dev-slot gated, 500 ms armed fault) | NO | done |
| `components/mqjs/device_stdlib.c` | `sys.panic` in the `js_sys[]` prop table | NO | done |
| `components/mqjs/gen/device_stdlib.h` | ROM stdlib regen (-m32); `gen/mquickjs_atom.h` regenerated content-identical, restored with `git checkout --` (CRLF trap) | NO | done |
| `tools/bb_pull.py` | The reader: signs, publishes, reassembles, prints | orchestrator | done |

### Wire format

**Request** — topic `<CONFIG_MQJS_TASK_TOPIC>/bb`, payload
`Ed25519 signature(64) || message`, message ASCII ≤ 512 B:

```
bbpull1
ctr=1753900000123
top=esp32p4-mqjs/task/u7q3x9f2
what=lastboot
from=0
```

| key | req | meaning |
|---|---|---|
| `ctr` | yes | replay counter, 1 .. 2^63-1, must be **strictly greater** than the device's stored high-water mark |
| `top` | yes | the device's own task topic, compared byte for byte |
| `what` | yes | `stats` (term_lp_report, one chunk) / `lastboot` / `live` |
| `from` | no | record cursor, default 0 — resumes a pull |

Line 1 must be exactly `bbpull1`. A CR per line is tolerated and blank lines
are ignored; **an unknown key, a duplicate key, a missing required key, a
non-decimal number, an embedded NUL or an overlong message is refused**, never
partially honoured (Q4).

**Reply** — topic `<CONFIG_MQJS_TASK_TOPIC>/bb/reply`, QoS 0, one JSON object
per chunk, `body` being exactly what `sys.blackbox()` returns:

```json
{"bb":1,"ctr":1753900000123,"what":"lastboot","seq":0,"from":0,
 "body":{"src":"lastboot","ok":1,"from":0,"lines":["[142] sys/system: boot 7 …"],
         "records":12,"next":12,"more":1},
 "last":0}
```

`seq` counts chunks from 0 (a gap = a lost QoS 0 chunk, visible to the reader);
`last:1` ends the reply; `body.next` is the cursor to resume from. Two flags
appear only when they apply and both force `last:1`: `"stall":1` (the buffer
cannot hold even one rendered line — advance on `records > 0`, not on `more`)
and `"cut":1` (`TERM_BB_CHUNKS_MAX` = 64 chunks reached with records left).

**Refusal** — one line on `<CONFIG_MQJS_TASK_TOPIC>/status`, the convention
`task_source.c` already uses: `bb: rejected (replay)`. The names come from
`term_bb_err_str`: `bad-length`, `bad-signature`, `bad-magic`, `bad-request`,
`unknown-field`, `duplicate-field`, `missing-field`, `out-of-range`,
`wrong-topic`, `replay`, `counter-store`, `misconfigured`, `publish-failed` —
those thirteen and nothing else (`ok` for success, `bad-code` for a value
outside the enum). An accepted pull answers `bb: sent 13 chunks, 172 records` there
too, and raises a status-bar event — an accepted pull means class-A content
just left the device, which belongs on the screen and not only in a log.

### Replay defence, and the threat it answers

§7.2 pins the mechanism, so nothing was invented: "署名リクエストは単調カウンタ
を含めて署名し、デバイスは最後に受理した値より大きいもののみ通す … 受理済み
カウンタの高水準マークは NVS に保存する — LP SRAM には置かない … 受理は低頻度
なので NVS commit を受理ごとに同期実行してよい". Implemented exactly:

- the counter is **inside** the signed message, so it cannot be edited in
  flight;
- the high-water mark lives in NVS (`termbb`/`ctr`, `nvs_set_u64` +
  `nvs_commit` **synchronously**, per accept), never in the LP region this
  component otherwise owns — LP is lost on a power cut (§4.4), so a mark kept
  there would let one power cycle reopen the whole replay window;
- **strictly greater**, so a captured request cannot be replayed even once;
- the mark is stored **before the first reply byte is published**. A reset
  mid-reply therefore burns the counter instead of leaving the request
  replayable. The operator's remedy costs nothing because the counter is
  wall-clock milliseconds: `tools/bb_pull.py` uses `time_ns()//1e6`, which is
  monotonic across tool runs and across machines with sane clocks and needs no
  state on either side. (Consequence, documented in the tool: a second machine
  with a slow clock is refused until its clock passes; `--ctr` overrides.)

The threat this closes is concrete, and it is worth being explicit about what
the gate does and does not protect:

- **The reply is plaintext on the broker.** Confidentiality of class-A content
  in flight is the broker's and the network's job (LAN-only broker today,
  tailnet later) — the same single-trust-domain assumption §4.4 states and the
  same one the unsigned catalog rows in `task_source.c` already rest on. What
  the signature gate controls is **who can cause** the device to emit the log.
- Therefore a passive subscriber sees the request as well. Without the
  counter, that observer could re-publish the captured request next week and
  pull a **future** boot's class-A log — exactly the exfiltration §7.2's gate
  is supposed to make impossible for a non-key-holder. With it, a captured
  request is inert.
- **`top=` closes the cross-device variant** (Q3): a captured request
  re-published on another device's task topic would meet an independent, and
  probably lower, high-water mark there. Every device in this project already
  has its own task topic with a random suffix, which is what makes the check
  meaningful. **Known limit:** two devices deliberately sharing one task topic
  share the weakness, because the counter is per-device state. That is a
  deployment choice, not something the protocol can fix without a device
  identity, and there is no device identity in this design yet.
- A device that has never been pulled has mark 0, so any `ctr >= 1` is fresh.
  `ctr = 0` is refused outright, so an "unset counter" cannot be spelled.
- Failing closed is the rule: if the mark cannot be **read**, or cannot be
  **stored**, the request is refused and nothing is published.

Not attempted, deliberately: a challenge/nonce handshake (an unsigned status
query returning a device nonce, then a signed request over it). It would be
strictly stronger against a device with no clock and no NVS, and it costs a
round trip, a second topic and per-request device state — while §7.2 already
chose the counter and the counter needs neither. If the design ever grows
multiple readers with multiple keys (§4.4 anticipates this), the counter
becomes per-key and that is when this gets revisited.

### The one header change

`term_lp_dump_json_ex(src, from, out, out_size, info)` was added to
`term_lp_ring.h`, with `term_lp_dump_json` becoming a one-line wrapper (the
rendering has one implementation, not two, and no existing suite changes).

§2 promised the responder would need "nothing new", and that was wrong on one
point: `records`, `next` and `more` are inside the JSON document, so a C
chunker could only learn whether to ask again by re-parsing its own output.
The alternatives were worse — drive `term_lp_iter_*` and re-implement the line
rendering and the JSON quoting (two serialisers to keep in step, and the wire
format would drift from the one `sys.blackbox` emits), or scan the produced
JSON for `"more":`. So the numbers come out of the side door and the format
stays exactly the one the host suites already cover. `info` may be NULL, and
on a 0 return it is zeroed so a caller that only reads `info->more`
terminates.

### Decisions

1. **The responder is pure; the device glue lives in `main/task_source.c`.**
   The brief allowed an `ESP_PLATFORM`-guarded half here, the
   `term_port_freertos.c` pattern. Callbacks beat a guard in this one case:
   with `verify` / `hwm_load` / `hwm_store` / `publish` injected, the **whole**
   responder — verify order, replay gate, counter durability ordering, chunk
   loop, framing — is host-drivable, there is nothing in `term_bb_pull.c` that
   compiles to nothing off-device, and `term_core` gains no dependency on
   `mqtt`, `nvs_flash` or `tweetnacl` (so §2's note about re-adding
   `nvs_flash` is moot: it stays out). The glue lands where the MQTT
   connection is owned, which is also the project's single-owner rule.

2. **No second broker connection, and no second task.** `task_source.c`
   subscribes `<base>/bb` on its existing client and routes it in
   `MQTT_EVENT_DATA` next to the app-push path, so the work runs on the MQTT
   event task — which already does Ed25519 verification, LittleFS writes and
   QoS 0 publishes there, and which is not `tcpip_thread`. The work is
   bounded: one verify, one NVS commit, ≤ 64 small publishes, for a
   human-driven once-in-a-crash operation. A queue and an owned task would be
   the right answer for an unbounded or periodic job; this is neither, and an
   extra hop would add a second owner of the same socket.

3. **The `/bb` subscription is an exact topic, not a wildcard.** Replies go to
   `<base>/bb/reply` underneath it; a `<base>/bb/#` subscription would make
   the device receive its own answers.

4. **QoS 0 for the reply, with `seq` making loss visible.** esp-mqtt's publish
   with QoS > 0 waits for an ack that only the MQTT event task can process —
   the deadlock esp-mqtt's own documentation warns about, and the reason
   `publish_status()` has always been QoS 0. So chunks are lossy in principle;
   `seq` makes a gap detectable and `from` makes it recoverable, which on a LAN
   broker is the right trade. A black box one re-pull away from complete beats
   a responder that hangs the connection.

5. **Chunk size 1600 B (`TERM_BB_SCRATCH_WANT`), from the configured buffers,
   not from taste.** `task_source.c` sets `buffer.out_size = TX_BUF_SIZE =
   2048` (deliberately small: without it, `out` inherits `buffer.size` and
   costs a second ~66 KB block — the rx side was raised to
   `MQJS_SCRIPT_MAX + 2048` for large pushes, aa4d7e6, and tx was capped for
   exactly this reason). 1600 B of payload plus the fixed header and a ~30-byte
   topic sits inside 2048 with slack. Raising it means raising `TX_BUF_SIZE`
   first; the header says so at the constant. ~31 KB of `lastboot` therefore
   arrives in ~20-25 chunks.

6. **One scratch buffer serves the envelope and every chunk.** `crypto_sign_open`
   needs a message buffer as wide as the payload, and the parsed request is
   copied into `term_bb_req_t` before the buffer is reused — so the responder
   allocates nothing, and the one `malloc` in the whole path is the glue's
   1600-byte scratch (off the MQTT event task's 8 KB stack, freed before
   return).

7. **Numbers are formatted by hand, no `snprintf`.** The ROM's nano formatting
   is a moving target for 64-bit conversions on this chip, and `term_lp_ring.c`
   already builds its JSON with a bounded writer — two serialisers that behave
   identically at a buffer edge are worth 30 lines.

8. **`stats` is answered through the gate too**, although `sys.blackbox()`
   gives the same counters to any app unprivileged. It carries no content, and
   a reader that wants to know whether a `lastboot` exists before pulling 31 KB
   should not have to guess. It costs one counter value, which is free.

9. **Refusals are named, not generic.** One error code per reason, published
   as a word on `/status`. An operator debugging a failed pull should not have
   to guess whether the signature or the counter was the problem, and neither
   name leaks anything a key holder does not already know. `term_bb_result_t`
   reports what the responder reached, so `res->hwm` is **0** on any refusal
   raised before `hwm_load` runs (`E_ENV`, `E_SIZE`, `E_SIG`, every parse
   error) rather than the stored mark; the header says so and `test_bb_refuse`
   pins the pair.

10. **A failed parse leaves `term_bb_req_t` zeroed** — the header promised it
    and the first implementation did it only for the "missing field" path. The
    harness caught it: an unknown key returned an error with `ctr` and `topic`
    still filled in, which is exactly the shape of thing a caller acts on by
    mistake. Fixed with a wrapper that clears on every non-OK return.

11. **`live` pulls are best-effort and that is by construction.** The ring is
    being appended to while the responder reads it and `term_registry` exposes
    no lock, so a live pull may render a torn tail line. It cannot fault
    (`term_lp_iter_next` bounds every walk by the header's own `used`/`cap` and
    stops on an implausible record), and it is why forensics pull `lastboot`,
    whose snapshot is immutable and whose cursor is therefore exact.

12. **`sys.panic()` is a permanent drill button, gated like the content
    reads.** Step 4 above used to say "trigger a panic from the dev slot" with
    nothing to trigger it: §1's probe owned the crash triggers and went away
    with the region handover, so the black box's most important record — the
    one written by the thing that killed the device — had no way to be
    exercised except by waiting for a real bug. `sys.panic()` fixes that with
    the *same* predicate as `sys.blackbox("live")`
    (`s_cur_wk->idx == MQJS_WORKER_DEV || s_cur_wk->trusted_system`, §7.2's
    owner-key gate), so it is not an app API and anything else gets a
    `TypeError`. Three sub-choices: **a real fault** (a volatile NULL read →
    LoadProhibited), not `abort()`, because although both reach
    `__wrap_esp_panic_handler` only a fault carries an mepc/mcause and the
    cross-check step 4 asks for is exactly `pc=` against the Guru Meditation
    block — and it is the trigger §1 measured as fully retention-safe;
    **armed 500 ms out on an `esp_timer`** with the binding returning 0
    immediately, so the caller's "about to panic" publish leaves the device
    before the chip dies (a second call inside the window is a no-op, not a
    second fault); and **inert off-device**, returning `-1` under
    `ESP_PLATFORM`'s `#else` so `run_pc` cannot take the test runner down —
    the gate is still real there, only the fault is absent.
    Files: `components/mqjs/mqjs_runtime.c` (`js_sys_panic`),
    `components/mqjs/device_stdlib.c` (the `js_sys[]` row, where the delay is
    documented), `components/mqjs/gen/device_stdlib.h` (ROM regen, -m32;
    `gen/mquickjs_atom.h` regenerated content-identical and was restored with
    `git checkout --`). Verified through `run_pc`: `sys.panic()` returns `-1`
    twice with the process alive afterwards, and `pc_test/run_pc_tests.sh`
    still reports **ALL PC SUITES PASSED**.

### The reader: `tools/bb_pull.py`

```
python3 tools/bb_pull.py HOST BASE_TOPIC [lastboot|live|stats] \
        [--port N] [--from N] [--ctr N] [--timeout S] [--json] [--raw] [--tamper]

python3 tools/bb_pull.py 192.168.1.2 esp32p4-mqjs/task/u7q3x9f2
python3 tools/bb_pull.py 192.168.1.2 esp32p4-mqjs/task/u7q3x9f2 stats
```

Signs with `tools/task_signing_key.pem` (the same key and the same envelope as
`mqjs_push.py`), subscribes to `<base>/bb/reply` **and** `<base>/status` before
publishing, reassembles `body.lines` in `seq` order until `last`, prints the
log to stdout and everything else to stderr. It warns on a `seq` gap, exits 1
on a `bb: rejected (...)` line, and follows a `"cut":1` reply automatically
with a fresh counter and `from=body.next` (up to 8 rounds). `--raw` publishes
the request unsigned and `--tamper` flips a byte after signing — both exist so
the orchestrator can watch the device refuse them.

### Off-device verification (done)

- **Throwaway harness** (ASan+UBSan, `-std=c99 -Wall -Wextra`, not a repo
  file), **266 checks, 0 failures**, with **real TweetNaCl verification over
  real Ed25519 signatures** produced by a python generator, and the real LP
  ring underneath:
  - *29 signed cases, in order, through `term_bb_serve`*: three accepted
    (`stats`, `live`, `lastboot`-with-no-snapshot); `replay` for a counter
    equal to the mark and for an older one; `bad-signature` for a flipped
    signature bit, for a message edited after signing, for an unsigned
    payload and for a truncated envelope; `bad-length` for a payload shorter
    than a signature, for a signature with no message and for a message over
    `TERM_BB_REQ_MAX`; `bad-magic`, `unknown-field`, `duplicate-field`,
    `missing-field` (both required keys), `wrong-topic`, `out-of-range`
    (`ctr=0`, 2^63, 24 digits), `bad-request` (non-decimal counter, unknown
    `what`, empty value, empty key, embedded NUL); and three tolerated shapes
    (CRLF, blank lines, leading zeros). **Every refusal also asserts that
    nothing was published and that the high-water mark did not move**, and
    every acceptance asserts the mark equals the request's counter.
  - *chunk loop*: 13 chunks for 172 records at 1600 B, `seq` dense from 0,
    exactly one chunk claiming `last`, every chunk NUL-terminated, closed with
    `}`, within the scratch, carrying the right preamble, and the sum of the
    per-chunk `records` equal both to `res.records` and to the ring's own live
    record count — i.e. **the reassembly is complete, not just plausible**.
  - *the two honesty flags*: at the smallest legal scratch (704 B) a 512-byte
    record under a 31-byte writer name cannot be rendered, and the reply ends
    `"stall":1,"last":1` (40 chunks, 171 of 172 records); with a denser ring
    (700 and 1200 records) the 64-chunk bound is hit instead and the reply ends
    `"cut":1,"last":1`. In every configuration the loop terminates with
    `last:1`, never exceeds `TERM_BB_CHUNKS_MAX`, and never stops with records
    pending without saying which of the two happened.
  - *counter durability ordering*: a failing `hwm_store` yields
    `counter-store` with **zero chunks published** and exactly one store
    attempt; a failing `hwm_load` likewise; and a publish that fails on chunk 3
    yields `publish-failed` with the mark **still burned** — the documented
    direction (a request is never replayable twice).
  - *environment refusals*: NULL env, an all-zero env, a scratch one byte
    below `TERM_BB_SCRATCH_MIN`, a NULL payload, a zero length.
  - *pure parse/gate, no crypto*: the minimal message, no trailing LF, `from`
    at `UINT32_MAX` and one past it, magic-only, a duplicated optional key,
    `len` (not a NUL) deciding where the message ends, a failed parse leaving
    `out` zeroed, and the gate's ordering — topic mismatch outranks a stale
    counter, a prefix or an extension of the topic is not a match, equal
    counters are a replay.
  - *stats reply*: one chunk, `what` echoed, the report body present, closed
    with `"last":1}`, zero records counted.
  - *the panic note* (§2a section [8]).
- **The tool and the device agree, end to end**: a second harness feeds
  `tools/bb_pull.py`'s own `request()`/`sign()` output into the C responder
  verified against **`main/task_pubkey.h`'s real public key** — accepted first
  time, `replay` the second, `bad-signature` for one flipped byte and for a
  5-byte truncation. A python-side check confirms `bb_pull.py` signs with the
  key the firmware embeds (one trust root), that the request bytes are exactly
  the documented format, and that `collect()` reassembles chunks, ignores a
  reply for another counter, ignores unrelated `/status` traffic, warns on a
  `seq` gap and exits 1 on a rejection.
- **Host suites**: `host_test/run_tests.sh` -> **31 suites, ALL SUITES
  PASSED**, no new warnings (the runner globs the parent directory, so
  `term_bb_pull.c` is compiled with `-Wall -Wextra` and ASan/UBSan into every
  suite). Note that §2's line about `test_lp_writers` having one unsatisfiable
  check is now stale: all 31 pass.
- **run_pc**: builds with `term_bb_pull.c` in the glob; `pc_term_basic`,
  `pc_term_errors`, `pc_term_name`, `pc_term_probe`, `pc_term_resize` all
  **ALL PASS**, and `sys.blackbox()` / `("live")` / `("live", next)` still
  answer correctly through the refactored `term_lp_dump_json` — the JS surface
  is unchanged.
- **Not run** (orchestrator's steps, deliberately): the device build, the
  flash, any push to the device.

### Device verification plan (for the orchestrator)

1. Flash, then `python3 tools/bb_pull.py 192.168.1.2 <task topic> stats` —
   expect one chunk and `retention`/`lastboot` in the body.
2. `--raw` and `--tamper` — expect `bb: rejected (bad-signature)` on
   `<base>/status` and no reply chunk.
3. Re-run a normal pull twice with `--ctr 1` — the second must be
   `bb: rejected (replay)`. Then a fresh (clock-derived) counter must work
   again, and it must **still** be refused after a power cycle, which is the
   NVS mark doing its job.
4. Trigger a panic from the dev slot with **`sys.panic()`** (decision 12 —
   publish first, the fault is armed 500 ms out), let it reboot, then
   `bb_pull.py ... lastboot` — expect the previous boot's lines **and** the
   `sys/panic:` note (§2a), and cross-check its `pc=` against the serial Guru
   Meditation block. The drill's note reads `kind=fault … task=esp_timer`
   (the fault fires on the timer task, not `js_task`) — §2a's "task= names the
   JS worker" describes a real crash inside a JS callback, not this.
5. Confirm the reply arrived in ~20-25 chunks with no `seq` gap, and that
   `bb: sent N chunks` appeared on `/status` and in the status bar.

### Device verification — RESULTS (2026-07-30, all steps run, all passed)

Executed over MQTT with tools/bb_pull.py + a 210 s serial capture; raw
outputs in the session transcript.

1. `stats --ctr 1` accepted: 1 chunk, `retention=ok, prev=captured,
   lastboot=1`, `/status`: `bb: sent 1 chunk, 0 records`.
2. `--ctr 1` again → `bb: rejected (replay)`. `--raw` → `bb: rejected
   (bad-length)` — the 63 B unsigned payload cannot even contain the 64 B
   signature, so the length gate correctly fires *before* the signature
   verdict the plan predicted; `--tamper` → `bb: rejected (bad-signature)`.
   Every refusal: zero reply chunks.
3. `--ctr 2` accepted; after an esptool reboot `--ctr 2` → `bb: rejected
   (replay)` — the NVS high-water mark doing its job across a reset. A
   fresh clock-derived counter then pulled `lastboot` clean (3 records,
   `bb: sent 1 chunk, 3 records`; the short chunk count is the short boot,
   the 13-chunk/172-record loop is covered by the §3 harness).
4. `sys.panic()` drill (convergent dev-slot probe tools/probe_panic_drill.js:
   arms on a non-panic boot, collects on the panic boot): `lastboot` after
   the drill contains `sys/panic: panic: fault Load access fault
   task=esp_timer core=0 pc=0x4002ebbc cause=5`, and the serial capture's
   Guru Meditation block reads `Core 0 panic'ed (Load access fault)`,
   `MEPC: 0x4002ebbc` — **cause, core and pc all match the note exactly.**
   Partition-ordered dump confirmed (SYS lines, then APP).
5. Dense `seq`, exactly one `last:1` per pull, tool rc=0 on accepts and
   rc=1 on every refusal. dev_idle restored after the drill.
