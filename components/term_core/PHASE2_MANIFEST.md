# Phase 2 manifest — term registry, TERM_LOG, `term.*` bindings

Scope: docs/term-design.md §11.2 — the registry (§3.1 ownership, generation
ids, owner gate, persist/re-attach, LRU evict, the asynchronous two-stage
quiesce), TERM_LOG mode, the JS bindings of §8 minus `pipe`/`onReply`, and the
ROM header regen. Out of scope: LP SRAM black box and the MQTT responder
(phase 3), the ssh pipe and the ssh_vt migration (phase 4).

Owner of this file: **opus_A** (implementation). Updated as the phase
proceeds — it is a running contract, not a plan written once.

Status: **implementation complete and device-verified (230f543).** Two
contract gaps that the device probe and the test author's dry run surfaced
afterwards are fixed on top of that commit — the missing `term.POST` /
`term.TRUNC` constants and `read`'s `n <= 0` clamp. Both header sets
regenerated, 22 host suites and 4 PC suites green; the device build of the
fixes is the orchestrator's step. See Decisions #11 (the earlier ROM-header
break), #12 and #13.

## Why this ledger exists

The test author writes against **headers and the design document only**.
Implementation files listed below are off limits to them: a test derived from
the code under test asserts what the code does, not what the design says, and
the two differ precisely where the bugs are. If something needed to write a
test is not visible in a header or in `docs/term-design.md`, that is a defect
in the header — say so and it gets fixed there, rather than being read out of
the `.c`.

| Column | Meaning |
|---|---|
| Read by tests | `yes` = the test author may open it. `NO` = implementation. |
| Status | `done`, `wip`, `planned` |

## Contract surface (test author reads these)

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_core.h` | Phase 1 VT core contract (unchanged this phase) | yes | done (phase 1) |
| `term_registry.h` | Slot table, ids, owner gate, quiesce, persist/LRU, all `term.*` entry points | yes | done |
| `term_port.h` | Platform seam: mutex w/ bounded wait, clock, signal, UI post, allocator | yes | done |
| `PHASE2_MANIFEST.md` | This ledger | yes | done |
| `docs/term-design.md` | Ground truth for every contract above | yes | done (phase 1) |

## Implementation (test author must NOT read)

| File | Role | Read by tests | Status |
|---|---|---|---|
| `term_registry.c` | The registry: slot table, id packing, owner/generation gate, create/re-attach, persist LRU evict, stage-1 close, reaper, ring ingest, UI drain/visit, snapshot/read jobs | NO | done |
| `term_port.c` | `term_port_install` / `_get` / `_valid` / `_installed`; holds the installed table | NO | done |
| `term_port_freertos.c` | Device port: FreeRTOS mutex + `esp_timer` + the ui_tab5 job path + `heap_caps_aligned_alloc(MALLOC_CAP_SPIRAM)`, plus the reaper task. Entire file guarded by `#ifdef ESP_PLATFORM` so the host runner's parent-directory glob compiles it to nothing | NO | done |
| `term_ui_tab5.c` | UI glue: drains on the frame tick, blits dirty rows as `UI_CMD_CELLS` runs through the existing `ui.cells` renderer, paints the caret. Device-only, same guard | NO | done |
| `term_ui_tab5.h` | Device bring-up entry point (`term_ui_tab5_start`). Device glue, not contract | NO | done |
| `CMakeLists.txt` | IDF component registration for `term_core` (new — phase 1 was host-only) | NO | done |

## Files changed outside this component

| File | Change | Status |
|---|---|---|
| `components/mqjs/mqjs_runtime.c` | `js_term_*` bindings (§8 minus pipe/onReply); the PC port; lazy bring-up incl. the system console; print-sink tee; `term_registry_owner_stopped` in the app teardown; `mqjs_term_pump()` in both scheduler loops (no-op on device). `js_term_read` no longer clamps `n` — Decisions #13 | done |
| `components/mqjs/device_stdlib.c` | `js_term[]` prop table + `JS_PROP_CLASS_DEF("term", …)` + all 15 `term_err_t` constants (`POST`/`TRUNC` added after the device probe — Decisions #12) | done |
| `components/mqjs/mquickjs/mquickjs_build.c` | Local patch to the vendored generator: a short-int property value is emitted as `N * 2`, not `N << 1`, so the negative `term_err_t` constants do not become UB / a `-Werror=shift-negative-value` error on the device. One line; see Decisions #11 and `docs/mquickjs-patches.md` patch 2 | done |
| `components/mqjs/gen/device_stdlib.h` | ROM stdlib regen (-m32) | done |
| `components/mqjs/gen/mquickjs_atom.h` | Regenerated; **byte-identical** to the committed file (the new property names land in the stdlib string table, not the atom table), so it is not in the diff | done |
| `components/mqjs/CMakeLists.txt` | `term_core` added to `PRIV_REQUIRES` | done |
| `components/ui_tab5/include/ui_tab5.h` | UI-task work seam: `ui_tab5_post_job` / `ui_tab5_is_ui_task` / `ui_tab5_set_frame_cb` (+ the no-UI inline stubs) | done |
| `components/ui_tab5/ui_tab5.cpp` | Job queue + frame hook, drained from the existing mooncake frame timer after the canvas has consumed its command queue | done |
| `main/` boot path | **Not touched — deliberately.** See "Bring-up" below | n/a |

## Tests

| File | Role | Owner |
|---|---|---|
| `host_test/test_registry_*.c`, `fake_port.h`, `reg_util.h` | Registry suites against `term_registry.h` | test author |
| `pc_test/pc_term_*.js`, `run_pc_tests.sh` | `term.*` binding suites under run_pc | test author |

`host_test/run_tests.sh` compiles every source in the parent directory, so an
implementation file that is not host-safe must be `ESP_PLATFORM`-guarded
rather than excluded by hand. Both device-only files carry a
`typedef int …_tu_t;` outside the guard so the translation unit is never
empty.

## Build and run

### Host suite (registry + phase-1 core), Windows/WSL

```sh
cd components/term_core/host_test
sh ./run_tests.sh            # everything; ASan+UBSan when available
sh ./run_tests.sh registry_quiesce registry_read   # named suites only
```

Last run: **22 suites, ALL SUITES PASSED** (14 phase-1 + 8 registry).

### PC build of the bindings (`run_pc`)

The README recipe with three additions: `-I../term_core`, the five
`term_core` sources, and nothing else. Run it from `components/mqjs`,
with relative paths — `wsl gcc` cannot write to a `D:/…` output path.

```sh
cd components/mqjs
wsl gcc -O2 -I mquickjs -o stdlib_tool device_stdlib.c mquickjs/mquickjs_build.c
mkdir -p gen_pc
wsl ./stdlib_tool -a -m64 > gen_pc/mquickjs_atom.h
wsl ./stdlib_tool    -m64 > gen_pc/device_stdlib.h
rm -f stdlib_tool

wsl gcc -O2 -I. -Igen_pc -Imquickjs -I../skk_core/include -I../ui_tab5/include \
    -I../term_core -o run_pc tools/run_pc.c \
    mqjs_runtime.c system_vault.c tailscale_adapter.c app/mqjs_app_manager.c \
    ../skk_core/skk_kana.c ../skk_core/skk_dict.c ../skk_core/skk_builtin.c \
    ../term_core/term_core.c ../term_core/term_registry.c \
    ../term_core/term_port.c ../term_core/term_port_freertos.c \
    ../term_core/term_ui_tab5.c \
    mquickjs/mquickjs.c mquickjs/cutils.c mquickjs/dtoa.c mquickjs/libm.c -lm

wsl timeout 10 ./run_pc ../term_core/pc_test/pc_term_basic.js
```

`term_port_freertos.c` and `term_ui_tab5.c` compile to nothing off-device;
they are in the list so that a host build proves the guard, not the code.

Last run of the phase-2 PC suites through this binary: `pc_term_basic`,
`pc_term_errors`, `pc_term_name`, `pc_term_resize` all **ALL PASS**;
`pc_term_probe` reports `TERMPROBE: ready`.
`tools/smoke_examples.sh` (11 shipped examples) still passes — no regression
from the print-sink tee or the scheduler-loop pump.

**Known breakage in `pc_test/run_pc_tests.sh` (test author's file, not
touched):** it hands `wsl gcc` an absolute Windows path for `-o`
(`.../pc_test/build/stdlib_tool`) and the WSL linker cannot create it —
`cannot open output file D:/…: No such file or directory`. `host_test/run_tests.sh`
avoids this by `cd`-ing first and using relative paths; the same fix applies.

### ROM header regen

Windows builds use the pre-generated headers in `components/mqjs/gen/`
(the IDF CMake copies them instead of running an ELF host tool). They were
regenerated with the **-m32** flag, which is what the device needs:

```sh
cd components/mqjs
wsl gcc -O2 -I mquickjs -o stdlib_tool device_stdlib.c mquickjs/mquickjs_build.c
wsl ./stdlib_tool -a -m32 > gen/mquickjs_atom.h
wsl ./stdlib_tool    -m32 > gen/device_stdlib.h
rm -f stdlib_tool
```

The tool prints `Too many properties, consider increasing ATOM_ALIGN` — the
same harmless warning the CMakeLists already documents (a capped
global-object hash costs a few lookup cycles).

Two things that are easy to get wrong here:

- **A change to `mquickjs_build.c` invalidates both header sets.** Regenerate
  `gen/` with -m32 *and* `gen_pc/` with -m64 in the same pass, or the PC
  binary and the device disagree about the ROM layout.
- `gen/mquickjs_atom.h` is committed with **CRLF** line endings while
  `gen/device_stdlib.h` is LF. A regen through a shell redirect writes LF,
  which makes the atom header show up as a 76-line diff with identical
  content. It is content-unchanged this phase, so it was restored with
  `git checkout --` rather than committed as a line-ending churn.

## Bring-up: why `main/` was not touched

`term.*` brings the subsystem up **lazily, on first use**, from the binding
layer: install the port, initialise the table, create the system console,
start the reaper, hook the UI frame. Two reasons.

1. A device that never opens a terminal never pays the ~110KB of PSRAM the
   console term costs. §4.1 budgets that memory, but not spending it is
   strictly better than spending it at boot.
2. Boot order stops being a correctness question. `term_ui_tab5_start()` is
   idempotent, so a later decision to call it from `app_main` (after
   `ui_tab5_start`, which is where it belongs if the console should exist
   from the first log line) is a one-line addition that changes nothing else.

Everything a boot path would do is behind one call:
`term_ui_tab5_start(&cfg)` in `term_ui_tab5.h`.

## Decisions the header left open, and how they went

Recorded here so a reviewer can check them against the design rather than
discover them in the code.

1. **`owner` is 32 bytes, not the 16 of the §3.1 struct sketch.** App names
   are `MQJS_APP_NAME_MAX` = 32. Truncating one into a 16-byte field would let
   two distinct apps present the same owner identity and share a namespace —
   the exact failure the owner gate exists to prevent. Names and owners longer
   than their field are rejected, never truncated, and the JS binding measures
   the name in an oversized buffer so no clamp can hide the overflow.
2. **`term_registry_producer_bind/_unbind/_ack` are declared in phase 2**
   although `term.pipe` is phase 4. The quiesce state machine of §3.1 is
   defined in terms of a producer's detach ack, and its ZOMBIE path cannot be
   exercised without one. No JS binding is exposed.
3. **`term_registry_deinit()` exists for the host tests only.** The device
   never tears the registry down. It returns the number of slots it could not
   collect. Its exact semantics: stage 1 on every occupied slot; the acks
   *deinit itself creates* are satisfied on the spot (there is no UI task and
   no producer task left to send them); each bound producer is asked to
   detach; then a reap. What survives is exactly the set of slots that were
   **already** mid-quiesce with an unacked producer when deinit ran, and those
   keep their memory — §3.1 chooses a leak over a use-after-free, and that
   choice does not change because the registry is going away.
4. **One mutex covers the table AND every core, held across the drain parse.**
   The alternative (copy under the lock, parse outside it) buys a shorter
   critical section and costs a case analysis over the ack protocol to show
   the UI task is never inside a core the reaper could free. §5 already bounds
   what the long hold can cost: ingest waits at most `ingest_timeout_ms` and
   then drops and counts, and a drain slice is capped by `drain_budget_bytes`.
5. **Readiness is checked before arguments** in every entry point. With no
   registry there is nothing for an argument to be valid against, and
   `TERM_ERR_INVAL` from a table that does not exist sends the caller after
   the wrong bug.
6. **Control-path lock wait is 1,000 ms**, not unbounded and not the 20 ms
   ingest cap. create/close/show/resize/find/producer_* run on js_task, which
   every worker shares, so an unbounded wait there is the failure the
   asynchronous quiesce exists to avoid; but a create that failed because the
   UI happened to be parsing would be a bug, not a dropped log line.
7. **`scrollback_bytes`/`scrollback_lines` of 0 select the mode default**
   (§4.1: 48KB / 1,024 lines), for both modes. `term_registry.h` says both
   "0 selects the mode's default" and that "both explicitly 0" would mean no
   history; the first clause is the normative one, so there is currently no
   way to ask for a term with no scrollback. `TERM_ERR_MODE` is therefore
   never returned by `term_registry_read` — nothing can create the term that
   would provoke it.
8. **`term_registry_log(id, NULL, …)` is the platform escape.** A NULL owner
   skips the gate; it is how `term_registry_system_log` writes to the console
   and how force-close and the introspection calls work. NULL means "a
   platform caller", never "an app whose owner we could not determine" — the
   JS bindings refuse before they get here if `s_cur_wk` has no name.
9. **The byte ring is 8KB per term** (§4.1) and lives in the same single
   allocation as the core, carved after it. Per-frame drain budget: 4,096
   bytes per term, staged through a 512-byte `.bss` buffer so the frame
   budget costs no meaningful SRAM (§2.7).
10. **`stats.zombies` is cumulative** (how many DYING slots ever hit the
    deadline — "ZOMBIE 数はカウンタで可視化"), while `stats.zombie_bytes` is a
    live gauge that comes back down when a late ack collects one.
11. **The negative `term_err_t` constants broke the ROM generator, and the
    generator was fixed rather than the constants.** The first device build
    failed with `error: left shift of negative value
    [-Werror=shift-negative-value]`, once per negative constant, in
    `gen/device_stdlib.h`. `mquickjs_build.c` emits a short-int property
    value as `%d << 1` (the low bit is the JSValue tag), which is undefined
    behaviour for a negative operand and a hard error under IDF's
    `-Wall -Werror`. Nothing before `term` had exported a negative property
    constant, so the bug was latent, not ours; host gcc does not enable that
    diagnostic by default, which is why the PC path stayed green. The
    alternative — biasing the error codes positive — would have made §8's
    "every failure is a negative `term_err_t`" untrue in JS for the sake of a
    `printf`. Fixed in the vendored generator as a one-line `* 2` (identical
    value for every representable short int, same `slli` after codegen);
    documented as patch 2 in `docs/mquickjs-patches.md`. Both header sets
    were regenerated (`gen/` -m32, `gen_pc/` -m64); the only change in
    `gen/device_stdlib.h` is 47 lines of `N << 1,` → `N * 2,` with the
    integer sequence unchanged, and the four PC suites plus a direct
    read-back of all `term.*` constants confirm the values.
12. **`term.POST` and `term.TRUNC` were missing from the export table**
    (found by the device probe, 2026-07-30). The table stopped at
    `TIMEOUT` (-12) while `term_err_t` runs to -14, and `term_registry.h`
    documents `TERM_ERR_POST` as the ordinary return of `resize`/`show` when
    the UI queue refuses a job — a value an app receives in normal operation
    and had no name for. `TERM_ERR_TRUNC` is not returned by any *binding*
    today (`snapshot`'s truncation collapses into `null`), but exporting one
    half of the tail and not the other is a worse contract than exporting
    both: the rule "every `term_err_t` has a `term.*` name" is checkable, and
    "every one the current bindings happen to produce" is not. Both added;
    both header sets regenerated.
13. **`term.read(id, from, n)` clamped `n` up to 1 instead of rejecting
    `n <= 0`** (found by the device probe, 2026-07-30, confirmed against the
    test author's dry run: `term.read(id, 0, -1)` returned a 12-character
    string). The check was **not** missing in the registry —
    `term_registry_read` has always had `if (!out || !out_size || n <= 0)
    return TERM_ERR_INVAL;`. The binding layer dropped it: `js_term_read`
    ran `if (n < 1) n = 1;` before the call, so a malformed request was
    silently rewritten into a valid one and answered with real scrollback.
    That is the worst shape for this bug — the header's contract held in C
    and was unobservable from JS, which is exactly where a test written
    against the header disagrees with the device.

    The clamp is deleted rather than the header relaxed. `n` defaults to 1
    when the argument is *absent*, which is a different statement from "0
    means 1": an explicit 0 or negative is a caller error and now returns
    `null`. A binding that repairs its arguments hides the caller's bug and
    makes the surface untestable, and there is no plausible reading of
    `read(id, 0, -1)` that "one line from the oldest" satisfies.

## JS surface actually shipped (§8 minus pipe/onReply)

```js
var id = term.create({name: "log0", mode: "log", persist: true, cols: 80, rows: 24});
term.show(id, {x: 0, y: 0, w: 720, h: 600});   // no rect (or null) hides
term.log(id, "one line");                       // line-atomic, lossy
term.feed(id, "\x1b[31mred\x1b[0m");            // VT bytes, parsed on the UI task
term.resize(id, cols, rows);
term.snapshot(id);                              // screen as text, or null
term.read(id, from, n);                         // scrollback chunk, or null
term.close(id);
```

- `create` returns a positive id, everything else 0, and every failure is a
  **negative `term_err_t`**. All 15 enumerators are exported as constants, so
  no app needs a magic number for any value the registry can return:

  ```
  term.OK 0   term.INVAL -1   term.NOT_READY -2  term.BAD_ID -3
  term.STALE -4   term.NOT_OWNER -5   term.DYING -6   term.NO_SLOT -7
  term.NO_MEM -8  term.EXISTS -9  term.BUSY -10   term.MODE -11
  term.TIMEOUT -12    term.POST -13   term.TRUNC -14
  ```

  `snapshot`/`read` return `null` instead, because their success value is a
  string. No `term.*` call throws for a terminal-level error (§8).
- `read`'s `n` is **not clamped** by the binding: `n <= 0` is
  `TERM_ERR_INVAL` per `term_registry.h`, which surfaces as `null`. Omitting
  the argument still means one line. See Decisions #13.
- `name` is required and is the per-owner re-attach key; `mode` defaults to
  `"log"`; `cols`/`rows` default to the grid that fills the canvas, clamped
  to the §4.1 worst case (142 × 53, ≤ 4,260 cells).
- **`feed` is included** even though the task brief's list omitted it: §8
  minus `pipe`/`onReply` contains it, the registry implements it, and leaving
  the SPSC rule untestable from JS would have been the odd choice.
- There is no way to reach another app's terminal, and none to reach the
  system console — §7.2's "other apps: API 自体が無い", implemented as an
  entry point that does not exist rather than a check that could be wrong.

## Deliberately not done in this phase

- **`term.pipe` / `term.onReply`** — phase 4, excluded by the brief. The
  registry has the producer half (`producer_bind/_unbind/_ack`) because the
  quiesce state machine needs it; nothing is exposed to JS.
- **LP SRAM black box and the MQTT responder** — phase 3.
  `term_registry_system_log(writer, …)` already carries the `writer_id` of
  §4.4's record tag and is the seam that phase 3 tees from; today the
  parameter is accepted and not stored.
- **The caret sink of §10.2** (`term_core_set_caret_cb` → `ui_tab5` caret
  state, which `ime_core` reads). `ime_core` does not exist yet (§10.4 puts
  I1/I2 before term phase 4), and wiring a sink to nothing would be a
  guess at its shape. `term_ui_tab5.c` paints the caret as a reversed cell
  in the meantime.
- **Underline / strike rendering.** The cells carry `TERM_CELL_UNDERLINE`
  from the parser, but `UI_CMD_CELLS` has no rule to draw; §6 calls it "1 本
  線描画(小規模)" in the blitter, which is renderer work for the phase that
  ships the VT screen.
- **Mouse reporting, snapshot persistence** — §11.5, unchanged.
- **`main/` boot wiring** — see "Bring-up" above; lazy and idempotent, so
  adding it later is one line.

## Open items carried from §12

- persist slot cap: 4 for now (`term_registry_config_t::persist_max`);
  final number waits on on-device memory measurement.
- `term.show` composition order against `ui.*` canvas drawing: settled as far
  as the rectangle. `term_ui_tab5.c` currently posts `UI_CMD_CELLS` into the
  same canvas the `ui.*` primitives draw into, i.e. the term is **part of**
  the canvas, not a layer above it — an app that draws over the term's
  rectangle wins until the term's next damage. Revisit with the CanvasApp
  dirty handling when the VT screen ships.
- SGR stripping for the black box: not reached this phase (phase 3).
