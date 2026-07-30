# Phase 4 manifest — the pipe, terminal replies, the caret sink, underline

Scope: the **native plumbing** of docs/term-design.md §11.4. Five things:

1. `term.pipe(id, sshHandle)` — an ssh channel's rx wired straight into a
   term's byte ring in C (§5), with the SPSC rule and the re-pipe
   choreography enforced by the registry, and ring-full handled by *not
   consuming* so the SSH window provides the backpressure.
2. `term.onReply(id, cb)` (§6/§8) — DSR/DA answers to JS for a term the app
   feeds itself; a piped term answers into its own channel in C instead.
3. The **caret sink** of §10.2 — the drain pushes the cursor rectangle into
   the same platform caret state `ui.caret` writes, event-driven and
   coalesced per frame. `ime_core` reads that state; term never learns ime
   exists.
4. **Underline / strike** in the `UI_CMD_CELLS` blitter — §6's "1 本線描画".
5. The JS surface (`pipe`/`unpipe`/`onReply`) and the ROM header regen.

Since then the **app half** of §11.4 landed too — `examples/ssh_vt2.js`, a
second app that coexists with the shipped `ssh_vt.js`. It has its own
section at the bottom of this file ("Phase 4b"), including the two C
defects writing it exposed. The shipped `ssh_vt.js` is **untouched** and
still runs its JS terminal exactly as before.

Owner of this file: **opus_A** (implementation). A running contract, not a
plan written once.

Status: **implementation complete; host- and PC-verified; NOT device
verified.** 38 host suites green (43 as of phase 4b, the test author's), 5
PC suites green, 11 shipped examples smoke-clean, plus a throwaway fakes
harness (52 checks, ASan/UBSan clean, two deliberate sabotages caught — see
"Verification"). Everything that needs a real ssh server or real pixels is
listed under "Device-only".

Files touched by phase 4b: `examples/ssh_vt2.js` (new),
`examples/README.md`, `tools/smoke_examples.sh`, plus the Defect-A fix in
`term_core.{c,h}` and `term_registry.c`. `host_test/` and `pc_test/` were
not touched.

## Why this ledger exists

Same rule as phases 2 and 3: the test author writes against **headers and
`docs/term-design.md` only**. A test derived from the code under test
asserts what the code does, not what the design says, and the two differ
precisely where the bugs are. If something needed to write a test is not
visible in a header, that is a defect in the header — say so and it gets
fixed there.

| Column | Meaning |
|---|---|
| Read by tests | `yes` = the test author may open it. `NO` = implementation. |

## Contract surface (test author reads these)

| File | Role this phase | Read by tests |
|---|---|---|
| `term_registry.h` | **Extended.** Producers gain a reply route (`term_producer_t`, `term_registry_producer_bind_ex`), the pipe lifecycle (`term_registry_pipe`), the ring's producer side (`term_registry_producer_space` / `_write`), the JS reply sink (`term_registry_set_reply`), the caret sink (`term_registry_set_caret_sink`, `term_caret_ev_t`), cell metrics in the config, and 8 new stats counters | yes |
| `term_pipe.h` | **New.** `term.pipe`'s producer: an ssh channel on the device, a stub producer with a deferred ack off it. `term_pipe_bind` / `_unbind` / `_pump` / `_count` | yes |
| `term_core.h` | `term_reply_fn` / `term_caret_fn` / `TERM_CELL_UNDERLINE` were already there from phase 1 — this phase is what finally connects them. **Phase 4b adds `term_core_repaint_all()`** (Defect A) | yes |
| `sshc.h` | **Extended.** `sshc_sink_t` + `mqjs_ssh_set_sink` / `mqjs_ssh_drop_sink`. Device-only in effect (stubs when `CONFIG_MQJS_SSH=n`) but the contract is worth reading: it is where the backpressure protocol is spelled out | yes |
| `ui_tab5.h` | **Extended.** `UI_CELL_ATTR_UNDERLINE` / `_STRIKE` and `UI_CMD_CELLS`'s `w` field. Device-only | yes |
| `PHASE4_MANIFEST.md` | This ledger | yes |
| `docs/term-design.md` | Ground truth (§5, §6, §8, §10.2) | yes |

## Implementation (test author must NOT read)

| File | Change |
|---|---|
| `term_registry.c` | The producer's ring side, the reply routing trampoline, the caret accumulate-and-flush, the pipe/re-pipe lifecycle, the SPSC gate on `log` |
| `term_pipe.c` | **New.** The ssh glue (device) and the stub producer (host). Both halves compile everywhere; the device half is `ESP_PLATFORM`-guarded internally, not file-wide, because the host half is real code |
| `term_ui_tab5.c` | Runs now break on the attribute pair as well as on fg/bg, and carry it in `cmd.w` |
| `ui_tab5.cpp` | `cells_rules()`: one `fill_rect` per rule per run |
| `sshc.c` | The rx sink: a spinlock-guarded sink slot, the space-then-read protocol, `gone()` as the detach ack, release on session end |
| `mqjs_runtime.c` | `js_term_pipe` / `_unpipe` / `_onReply`, the caret sink implementation, the reply trampoline + `EV_TERM_REPLY`, `TermCb` per-term callbacks and their teardown, `term_pipe_pump()` in the PC pump, cell metrics into the registry config |
| `device_stdlib.c` | Three more `JS_CFUNC_DEF`s in `js_term[]` |
| `gen/device_stdlib.h`, `gen_pc/device_stdlib.h` | ROM regen (-m32 / -m64). `gen/mquickjs_atom.h` regenerated and **byte-identical**, so it is not in the diff (the new names land in the stdlib string table) |
| `term_core/CMakeLists.txt` | `term_pipe.c` + `PRIV_REQUIRES sshc` |

`host_test/run_tests.sh` and `pc_test/run_pc_tests.sh` both **glob** the
component directory, so `term_pipe.c` joined both builds with no script
change. Neither test directory was touched.

## The JS surface added

```js
term.pipe(id, sshHandle);   // 0, or a negative term_err_t
term.unpipe(id);            // 0, or a negative term_err_t
term.onReply(id, fn);       // fn(bytesString); fn null/omitted removes it
```

- `pipe` needs a **VT** term (`term.MODE` on a log term), the caller must own
  it, and while it is piped `feed` and `log` both answer `term.BUSY`.
- `pipe` on an **already piped** term answers `term.BUSY` **and asks the old
  producer to detach**; a retry succeeds once the ack lands. See Decision 2.
- `unpipe` is stage-1 only: it returns immediately and the term stops being
  piped when the ack arrives.
- `onReply` fires only for a term with **no** producer. A piped term's
  replies go back over the channel in C — that is the point (§6).
- Every failure is still a negative `term_err_t` and nothing throws (§8). No
  new constants: `MODE`, `BUSY`, `NO_SLOT`, `BAD_ID` already cover it.

## Decisions

Recorded so a reviewer can check them against the design rather than
discover them in the code.

1. **The pipe producer never drops a byte; JS/print ingest still does.**
   §5 gives them different rules and the implementation keeps them apart.
   `term_registry_feed` takes what fits, counts the rest and returns
   `TERM_ERR_TIMEOUT` — a dropped log line is better than a blocked app.
   `term_registry_producer_write` may not do that: truncating a run
   mid-escape corrupts the screen with no way back. So the pipe uses a
   three-step protocol — ask for space, read at most that much, write it —
   and the reason step 3 can never come up short is **SPSC**: only the
   producer adds to the ring and only the UI drain removes from it, so
   observed space is a lower bound that can only grow. When space is 0 the
   producer stops reading its socket and the TCP/SSH window does the rest.
   A lock timeout on either call consumes nothing and is retried.

2. **`term.pipe` on a piped term returns `TERM_ERR_BUSY` after requesting
   the old producer's detach. The "ack join" of §5 is the caller's retry,
   not a wait.** §5 says a re-pipe completes the old detach "ack join, the
   same procedure as §3.1's quiesce" — and §3.1's procedure is emphatically
   *stage 1 asks, a platform-side pass collects, js_task joins nothing*,
   because js_task is shared by every worker and a terminal's lifecycle must
   not be able to stall all JS on the device. A bounded sleep-and-poll on
   js_task would have needed a new port primitive (there is deliberately no
   delay hook, term_port.h) to buy a slightly nicer JS call. The retry is
   one recv timeout away for the ssh producer, and `term_registry.h` already
   promised this shape ("re-piping requires unbinding first and waiting for
   its ack").

3. **A producer is authenticated by its `user` cookie, not by an owner.**
   `producer_space`/`_write` take no owner: a producer is platform C code,
   not an app. The cookie check is load-bearing for exactly one window — a
   producer that has been unbound but has not acked yet still holds a valid
   id, and once it *has* acked and somebody else has taken the pipe, its
   late writes must not land in the new producer's stream. Mismatch is
   `TERM_ERR_BUSY`.

4. **`term.log` is now `TERM_ERR_BUSY` on a piped term.** This was a real
   SPSC hole, not a tightening for tidiness: `term_registry_log` writes into
   the same ring head, and line-atomicity only protects several *log*
   writers from each other. §5's "the producer is either the pipe or feed"
   has to include the convenience path, or the console's own writers could
   interleave into an ssh stream's ring.

5. **`pipe` requires `TERM_VT`.** A LOG term is multi-writer by
   construction (the print sink, `term.log`), and the alternative to
   refusing the pipe would be silently disabling the console's writers —
   the wrong half to give up.

6. **The caret is accumulated in the slot and flushed after the lock is
   dropped, once per term per drain pass.** §10.2 asks for event-driven and
   change-only; `term_core` already fires only on change, so coalescing
   costs three ints and a flag. Flushing outside the lock matters twice: the
   sink writes into another subsystem's state, and it means the sink is
   allowed to call back into the registry. The flush list is a
   `TERM_SLOT_COUNT`-sized stack array — bounded, so I4 ("nothing is
   allocated after create") holds on this path too.

7. **The caret is only pushed for a VISIBLE term, and `term_registry_show`
   re-arms it.** A hidden term's anchor is not just useless, it is wrong:
   tab switching is show/hide, and without the re-arm the IME float would
   keep pointing at the tab that just went away. Making a term visible, or
   moving its rect, therefore owes the platform an anchor even though the
   cursor did not move.

8. **A hidden cursor (DECTCEM off) is reported with `visible = false`, and
   the mqjs sink ignores it rather than writing a stale rectangle.** The
   float then falls back the way it does for an app that never called
   `ui.caret`. Writing the last known position would have been a lie the
   IME cannot detect.

9. **The caret sink is registry-wide, installed by the platform — not per
   term and not per app.** An app does not opt into having a cursor. The
   event carries the owner name so mqjs can find the right worker's
   `caret_x/y/h`; that is a `strcmp` over at most four workers, once per
   term per frame.

10. **§10.2's "the ui_tab5 caret state" is, in this tree,
    `MqjsWorker::caret_x/y/h` in `mqjs_runtime.c`.** The design says
    ui_tab5; the merged IME work put the state on the worker instead,
    because the anchor is per app and the IME's owner task reads the
    *foreground* worker's copy (`ime_float_update`). The rule that mattered
    — "term pushes into the same sink `ui.caret` writes, and ime_core reads
    that sink" — is implemented exactly; only the address differs from the
    doc's prose. The rejected alternative (`ime` polling a
    `term_cursor_pos()`) has no entry point anywhere.

11. **Terminal replies reach JS as an INLINE event payload.** The reply sink
    fires on the UI task with the registry's lock held, which forbids both a
    JS call and a malloc. `EV_TERM_REPLY` therefore carries the bytes in the
    event (`TERM_REPLY_MAX` = 16; the longest reply a VT100 CPR can produce
    is 14) and `dispatch_term_reply` calls the app from js_task. The event
    union was already 86 bytes wide because of `ssh_closed.reason`, so
    `MqjsEvent` did not grow. Posted with a **zero** timeout: a refused
    reply is one the remote asks for again, a blocked frame is a frozen
    device.

12. **The reply's owner is addressed by (worker slot, generation), not by a
    pointer.** An app that stopped and restarted between the DSR and its
    answer must not have its successor's callback invoked — the same
    stale-event rule §3.2 applies to every worker-addressed event.

13. **`sshc` gained a generic byte sink, not a term dependency.** Three
    function pointers (`space`/`write`/`gone`) and sshc still knows nothing
    about terminals; `term_pipe.c` is the only file that names both sides.
    `gone()` is mandatory and is **only ever called by the session task**,
    which is what makes it a usable detach ack: `mqjs_ssh_drop_sink()` sets
    a flag and returns, and the ack comes from the thread that was actually
    touching the ring. The session task also calls it when the session ends
    for any other reason — a session that died without acking would turn a
    closed terminal into a permanent zombie (§3.1).

14. **The sink slot is guarded by a spinlock, not a mutex.** Every hold is a
    three-pointer struct copy with no call inside it, and the session task
    must not be able to block js_task there. `sink_closed` bars an install
    that arrives after the session task has passed the last point where it
    could ever call `gone()` — the one window where a sink could otherwise
    be adopted by nobody.

15. **Underline and strike are one `fill_rect` per rule per RUN, drawn after
    the glyphs.** §6 sizes this as "1 本線描画(小規模)" and that is what it
    is: a fully underlined 142-column row costs two rect writes, not 142.
    Runs already break on fg/bg, so breaking them on the attribute pair as
    well is the only structural change; a run with no attributes costs one
    compare. The rule goes on top of a descender rather than under it,
    which is what a terminal looks like. Underline sits one pixel below
    `blit_glyph`'s own baseline, clamped inside the cell so consecutive
    underlined rows never touch.

16. **`UI_CMD_CELLS` carries the attributes in `w`**, which the header
    documented as unused for that op. No new field, so `ui_cmd_t` and the
    UI queue's memory are unchanged, and every existing `ui.cells` caller
    keeps working: `ui_post_bg` passes 0 there, which means "no rules".
    `ui.cells` is deliberately **not** given a JS-visible attribute
    argument this phase — nothing asked for one, and the native term is the
    only producer of underlined cells.

17. **The core's reply/caret callbacks are wired at `term_core_init`, not
    attached later.** A term that could emit a reply before somebody
    remembered to attach the trampoline would lose exactly the first ones,
    and the first thing a shell does on connect is the DA/DSR handshake.

18. **`term_registry_deinit()` clears the caret sink.** It belongs to the
    table's lifetime; a suite that builds one registry per case would
    otherwise keep pointing at the previous case's storage.

## Verification

### Host suite — the shipped one, unchanged

```sh
cd components/term_core/host_test && sh ./run_tests.sh
```

**38 suites, ALL SUITES PASSED** (same 38 as phase 3; nothing regressed and
nothing was added here — the phase-4 suites are the test author's job).

### PC suites — the shipped ones, unchanged

```sh
cd components/term_core/pc_test && sh ./run_pc_tests.sh
```

**ALL PC SUITES PASSED** (`pc_term_basic`, `errors`, `name`, `resize`, and
`pc_term_probe` reporting ready). The runner globs `../term_core/*.c`, so it
picked `term_pipe.c` up by itself; the absolute-path breakage phase 2
recorded did not reproduce.

`tools/smoke_examples.sh` — **11/11 shipped examples clean**, so neither the
new event type nor the extra pump disturbed anything that ships.

### Throwaway harness (implementer's, driven by fakes)

Its own fake port (counting mutex that reports recursion, settable clock,
inline UI task, malloc), a fake producer, a fake JS reply sink and a fake
caret sink. Built at `-O1 -Wall -Wextra` with **ASan + UBSan**.

**52 checks, 0 failures, max lock depth 1, no leaks.** What it covers:

- SPSC: `feed` and `log` both BUSY while piped; owner gate on `pipe`;
  `TERM_ERR_MODE` on a LOG term.
- Re-pipe: BUSY, the old producer's detach requested on *each* attempt,
  success only after the ack; `unbind` + ack restores `feed`.
- Backpressure: an empty ring offers 8,192; space shrinks by exactly what
  was written; a full ring offers 0 and **takes nothing** (`wrote == 0`, so
  no half-written escape); one drain slice frees its budget; a foreign
  cookie can neither write nor ask; a DYING term answers `TERM_ERR_DYING`.
- Replies: DSR 5n → `\x1b[0n` and CPR → `\x1b[1;4R` to the JS sink; a reply
  with no listener counted in `replies_dropped`; a piped term's DA reaching
  the producer's channel and **not** its JS sink even when one is
  registered; the three routing counters.
- Caret: nothing while hidden; `show(visible)` pushes the anchor;
  `x == view.x + col*cell_w`, `y == view.y + row*cell_h`, `h == cell_h`;
  owner/id attribution; an unchanged caret is silent; **2,000 bytes across
  four drain chunks produce exactly one event**; DECTCEM off reported as
  `visible = false`; hiding the term goes silent again.
- `term_pipe` stub: bind, `term_pipe_count`, a refused bind frees its
  context, the pump's ack, re-bind, unbind.

**Sabotage-verified** (an agent-written test is unproven until the defect it
names is introduced and it fails):

| Sabotage | Result |
|---|---|
| Remove the `piped` gate from `term_registry_log` | `FAIL log on a piped term is BUSY` |
| Deliver the caret per notification instead of per drain pass | 6 failures, incl. `coalesced to one per frame (5)` and `a hidden term has no caret to offer (1)` |

Both restored; the tree contains neither, and the full suites were re-run
afterwards.

### JS surface through run_pc

A throwaway script (not added to `pc_test/`, which is the test author's
directory) driving the real bindings: `onReply` install/fire/remove for a
fed term, DSR and CPR contents, `pipe` MODE/BUSY/owner behaviour,
`feed`/`log` BUSY while piped, re-pipe after the ack, `unpipe`, a bad id
answering negatively rather than throwing, and the screen still rendering
afterwards. **21/21 ok, `TERM PC SELFTEST: ALL PASS`.**

One thing that script learned the hard way, worth writing down for whoever
writes the shipped version: **poll, do not assume a tick.** run_pc's loop
runs timers *before* it dispatches queued events, so a reply produced by
`mqjs_term_pump()` is delivered one loop iteration after a naive
`setTimeout` would look for it. That is the loop's ordering, not the
terminal's behaviour; asserting on a single tick tests the wrong thing.

## Host-testable vs device-only

**Host-testable now, through `term_port` and fakes** — this is where the
test author's phase-4 suites belong:

- the SPSC rule (feed/log BUSY, cookie authentication),
- the whole backpressure protocol (`producer_space`/`_write`: space
  arithmetic, zero-space refusal, nothing-consumed-on-timeout, DYING),
- the pipe lifecycle: bind, re-pipe BUSY + detach request, ack, unbind, and
  the interaction with close/stage-2 (a piped term that never acks becoming
  a ZOMBIE is already exercised by phase 2's suites and still is),
- reply routing in all three cases (producer / JS sink / nobody) and the
  reply *contents* for DSR 5n, DSR 6n and DA,
- the caret sink: geometry, change-only, per-frame coalescing, visibility
  gating, `show`'s re-arm, attribution,
- `term_pipe_bind`/`_unbind`/`_pump`/`_count` against the stub producer,
- the JS bindings through `run_pc`.

**Device-only, and therefore still unproven:**

- the real `sshc` glue: that `mqjs_ssh_set_sink` actually diverts a live
  channel's bytes, that a full ring really does stall `wolfSSH_stream_read`
  and that the peer throttles instead of the session dying, and that
  `gone()` arrives promptly enough that a re-pipe retry feels immediate;
- replies actually reaching a remote pty (`vim` sending DA/DSR on startup
  and getting a usable answer);
- **the pixels**: underline and strike position/thickness at 9×24 HackGen,
  including the PPA path and the CPU fallback, and whether the underline
  should sit one or two pixels below the baseline. §6's numbers are a
  reading of `blit_glyph`'s arithmetic, not something anybody has looked at;
- the caret sink feeding a real IME float in a real terminal — that the
  preedit lands under the terminal cursor and not one row off;
- the cell metrics coming from `ui_tab5_cell_size()` rather than the 9×24
  fallback (the PC path uses the fallback);
- whether `TERM_CELL_ITALIC`/`DIM` deserve anything. They are carried in the
  cells and drawn as nothing, which is the honest state: the grid font has
  no italic face and a "dim" that is not a colour change would need a
  second palette. Not started, not promised.

## Not done in this phase

- **`ui.cells` attribute argument for JS apps** — Decision 16.
- **Mouse reporting, snapshot persistence** — §11.5, unchanged.
- **`main/` boot wiring** — still lazy and idempotent (phase 2's "Bring-up").
- **Anything about `term.show`'s composition order against `ui.*`** — §12's
  open item, unchanged: the term still blits into the same canvas.

---

# Phase 4b — the app: `examples/ssh_vt2.js`

§11.4's other half: the staged migration of `ssh_vt.js` onto the native
path. **`examples/ssh_vt.js` is untouched and still ships** — it is the
fallback until the native path is device-verified, which is exactly what
§11.4 asks for. The new app is a separate `@app` (`ssh_vt2`), so both can
be pushed and the user can switch back by launching the old one.

Status: **parses and runs clean under run_pc; 25-check in-file selftest
green and sabotage-verified; NOT device verified.** Everything that needs a
real ssh server, real touches or real pixels is in "Device checklist".

## Size

| | lines | bytes (LF) |
|---|---|---|
| `examples/ssh_vt.js` (JS terminal, still shipping) | 1,486 | 59,805 |
| `examples/ssh_vt2.js` (native term) | 1,043 | 40,443 |

−30% of the lines for the same features **plus** everything the C core adds
that the JS one never had (alt screen, truecolor, UTF-8 split tolerance,
scrollback, DSR/DA). The dispatch asked for "well under 30KB"; the file
lands at 40KB and I did not cut further, because what is left is not code:
comment density is identical to `ssh_vt.js` (39 vs 40 bytes/line) and the
real ceiling is `MQJS_SCRIPT_MAX` = 128KB, which the 60KB app already lives
under. Getting to 30KB would have meant deleting the trap notes, which is
the one kind of line in this tree that has repeatedly paid for itself.

## What moved, what stayed (§11.4)

**Deleted from the app (now C):** the whole VT state machine
(GROUND/ESC/CSI/OSC/DCS), the grid and row model, `dirty`/`dirtySeq` and
the flush budget, `ui.scroll` bookkeeping, `applySGR` + the 16-colour
palette + `nearestPalette`/`xtermColor` 256-colour quantisation, the CONT
detach logic, per-byte `charCodeAt` ingest, `ui.cells` run assembly, the
cursor rectangle, `resizeTerm`'s content carry-over, `gridLines`/the
REPORT demo — ~590 lines of `makeTerm()` and its helpers, replaced by
`term.create` + `term.show` + `term.pipe` + `term.resize`.

**Kept in JS, and why:**

| Kept | Why (§11.4) |
|---|---|
| Tab bar, hit testing, `switchTo` | "タブ … の chrome は JS のまま". Tab switching is `term.show` re-pointing (§8) |
| Connect UX: host list, form, `vault`, host-key trust, dangerous-paste confirm | Not terminal work at all; unchanged from `ssh_vt.js` except for what the sections below list |
| Long-press selection → `clipboard` | §11.4: "選択は grid 読み出し API で足りる" — the grid is read with `term.snapshot` |
| `ui.onKey` → `ssh.write`, TOKSEQ, one-shot Ctrl/Alt | §10.1: term is output only, the key path never touches it |
| `relayout` → `term.resize` + `ssh.resize` | §8: notifying the pty is the caller's business |
| `ui.ime(1)` and nothing else about IME | §10.2: the caret anchor is pushed by C now. **There is no `ui.caret` call in this app** — the six lines of `reportCaret()` that `ssh_vt.js` still runs are gone |

`ssh.onData` is deliberately **not** registered: while piped, sshc hands
the bytes to the sink and never posts a JS event (sshc.h), so a handler
would be dead code that implies the bytes still come through JS.

## App-side decisions

19. **Three fixed term names, `t0`/`t1`/`t2`, not one per host.** The name
    is the per-owner re-attach key (§3.1), so it has to be stable across an
    app restart, and a host-derived name would grow without bound against a
    3-slot app and an 8-slot registry. The tab *labels* live in
    `store["svt2_tabs"]` (`[{n, label}]`), rewritten on every tab change.
    All three terms are `persist: true`.

20. **A restored tab is proven by its content, not by a flag.** `term.create`
    re-attaches silently — `term_registry_create`'s `out_reattached` is not
    plumbed into the JS binding (see "Gaps") — so `restoreTabs()` creates
    the recorded names and treats a **blank `term.snapshot`** as "this was a
    fresh create, there is nothing to restore" and closes it again. A record
    naming an unknown term is ignored. That keeps a post-reboot start from
    opening three empty tabs.

21. **A disconnected tab stays as a readable tab.** `ssh.onClose` sets
    `live = false` and keeps the term, so the last screen survives the
    session (the point of a persist term) and selection/copy still work on
    it. Keys answer `sys.notify` instead of writing to a dead session. A
    tab whose snapshot is blank, or that died with a `hostkey*` reason, is
    closed instead — an auth failure should not leave a blank tab behind.
    `ssh_vt.js` folded the tab in every case; this is the one deliberate
    behaviour change.

22. **`pickTab` prefers a free name, then recycles a disconnected tab with
    RIS.** Reuse feeds `\x1bc` rather than closing and re-creating: a close
    is stage-1 asynchronous (§3.1), so a create with the same name
    immediately afterwards races the reaper for the slot. Feeding a reset
    keeps the id and the memory.

23. **`term.pipe` BUSY is retried on a timer, never waited on.** Decision 2
    made the ack join the caller's retry; `pipeWithRetry(entry, 8)` is that
    caller — 150ms apart, and it stops if the session went away in between.
    A terminal error notifies and does not throw (§8).

24. **A full repaint is requested by hide→show, and that is now a real
    trigger** (see Defect A). `repaintActive()` exists because JS has to
    repaint after `ui.clear`, after a foreground restore, and after a
    selection highlight is dragged smaller — none of which the core's damage
    set can see.

25. **The app hides its terms in `sys.onBackground`.** `term_registry_ui_visit`
    blits every *visible* term with no idea which app is in front, so a
    background app's terminal would paint over the foreground app's canvas.
    An app's screen is its own business at that transition anyway (§3.3
    destroys the outgoing app's widgets), so this belongs in the app — but
    it is a rule every native-term app must follow, not an optional
    courtesy. The *stopped* case is already handled in C
    (`term_registry_owner_stopped` clears `view.visible`).

26. **Selection reads the screen through one cached snapshot.**
    `term.snapshot` skips CONT cells (term_registry.h), so a column
    coordinate is not a string index: `expandRow()` re-inserts the width-2
    filler using `ui.cellWidth`, which gives both the `ui.cells` form (one
    codepoint per column, filler included) and the copy form (filler
    dropped). The rows are grabbed once when the selection changes and
    refreshed every 4th tick, so a drag does not post a snapshot job to the
    UI task per finger sample.

27. **The tick is 60ms and does almost nothing.** No flush, no caret, no
    dirty scan — the C frame hook owns the pixels. What is left is the
    layout-drift check `ssh_vt.js` had to add because `"\x00rotate"` only
    reaches the foreground app, plus re-drawing the selection highlight that
    the term's own repaint paints over (self-healing, the same dance the old
    app documented).

## Two C defects the app exposed

Both are plumbing gaps in phase 4a, found by writing the app against the
headers. Neither can be worked around in JS, and one of them is fixed in
this commit because the app is unshippable without it.

**A. `term.show` did not mark a repaint — FIXED here.** The damage set
describes cell *changes*, so a term that is hidden and shown again (i.e.
every tab switch, §8's whole model of tab switching) painted nothing until
its remote happened to write. With `vim` on screen that is "never". The
same hole swallowed every app-side `ui.clear`. There is no app-side
expression of "repaint": `term.resize` with unchanged geometry returns
early by design (`term_core_resize`), and no other call touches damage.

Fix, minimal and at the right layer:

- `term_core.h` / `term_core.c`: `void term_core_repaint_all(term_core_t *)`
  — raise the existing `full_repaint` flag, touch nothing else. It is the
  same flag resize/reset/alt-screen already set, so the renderer needed no
  change.
- `term_registry.c`, `term_registry_show()`: on the transition that already
  re-arms the caret (becoming visible, or the rect moved — now also
  w/h changing), call it. One condition, two consequences, same reason:
  the cursor did not move and no cell changed, but which pixels are on the
  glass did.

43 host suites and all PC suites stay green with it in — and they were green
*before* it, which is itself a hole: nothing in the suites asserted either
behaviour. A throwaway probe of my own (scratchpad, `reg_util.h` + the fake
port, ASan/UBSan, 17 checks green) pins it down, and the shipped version
belongs in `host_test/` where the test author can own it:

- a quiet frame owes no full repaint, and re-showing the **same** rect while
  already visible is not a transition either (both controls);
- hidden terms are not visited at all;
- becoming visible again **does** owe a full repaint, exactly once;
- so does a moved rect;
- the content is untouched afterwards (`term_core_repaint_all` writes damage
  state and nothing else).

**Sabotage-verified**: rebuilt against a `term_registry.c` with the
`term_core_repaint_all(sl->core)` line removed → `FAIL seen_full expected 1
actual 0` on both the tab-switch and the moved-view checks, and only those.

**B. The bracketed-paste mode is invisible to JS — NOT fixed.**
`ssh_vt.js` wraps a paste in `\x1b[200~`/`\x1b[201~` only when the remote
declared DECSET 2004, which it knew because its own parser tracked it. The
native core tracks it too (`TERM_MODE_BRACKETED`, `term_core_modes`) but
the registry and the JS surface expose nothing, so `ssh_vt2.js` **never
brackets a paste**. Wrapping unconditionally is not an option (a shell that
never asked for 2004 gets the literal escape typed into it), so the app
falls back to the confirm dialog for control characters, which was the
actual safety mechanism. What is missing is one accessor —
`term.modes(id)` or a `bracketed` bit in a small `term.info(id)` — and it
is a plumbing job, not an app one.

**C. `out_reattached` is dropped by the binding — worked around, worth
plumbing.** `term_registry_create` reports whether a create re-attached;
`js_term_create` passes NULL. Decision 20 recovers the same information
from the snapshot, which is fine but indirect. If a second app ever needs
it, return it (e.g. a negative-free `term.create` that answers an object,
or `term.info(id).reattached`).

## Verification (off-device)

Build (the runner globs `../term_core/*.c`, so nothing new to add):

```sh
cd components/term_core/host_test && sh ./run_tests.sh     # 43 suites, ALL PASSED
cd components/term_core/pc_test   && sh ./run_pc_tests.sh  # ALL PC SUITES PASSED
RUN_PC=... bash tools/smoke_examples.sh                    # 12/12 OK
```

`tools/smoke_examples.sh`'s default list gained `ssh_vt2`, so the app is in
the standing smoke set next to `ssh_vt`.

**In-file selftest.** `var SELFTEST = false;` at the top of the app, flipped
by `sed` the way `ssh_vt.js`'s flags are (one-line comments only, so the
substitution cannot break a block comment). It drives the real `term.*`
bindings and prints `SSHVT2 SELFTEST: ALL PASS` — **25/25 ok**:

- `term.create` / `feed` / `snapshot` round trip, polled rather than
  assumed: run_pc runs timers *before* dispatching queued events, so
  "one tick after feed" is the wrong clock (phase 4a recorded the same
  lesson).
- the wide-character column model: `日本語x` occupies columns 0..6, the
  filler columns are marked, copy text drops them, draw text keeps them,
  and a two-row selection extracts `bc\n日本`.
- SGR is a C concern now: the snapshot of an `\x1b[1;32m` row is plain
  text.
- `\x1bc` (RIS) blanks the screen — Decision 22's recycle path.
- the pipe contract the app depends on: `pipe` OK, `feed` BUSY while
  piped, a re-pipe BUSY, and a re-pipe that succeeds after the stub
  producer's deferred ack, then `unpipe`.
- `tabLayout` (runs, active colours, modifier block pinned to the right
  edge, ordered hit boxes, `[+]`-only when there are no sessions) and
  `gridFor` (portrait 80×28, landscape 132×25, and a 4000×4000 screen
  clamped inside the core's 142/53/4,260 maxima).

**Sabotage-verified** (a test is unproven until the defect it names fails
it):

| Sabotage | Result |
|---|---|
| width-2 cell stops claiming its filler column | 5 failures incl. `wide cell occupies 2 cols`, `selection across rows` |
| modifier block drawn at column 0 | `FAIL mods at the right edge` |
| `rowText` stops skipping fillers in copy mode | `FAIL copy text drops fillers`, `FAIL selection across rows` |

**Also exercised off-device:** a plain run (no flags) reaches the host page
with no exception; a run with a seeded `store` (`svt2_tabs` naming `t0`
plus a bogus name, `ssh_hosts` with one entry) exercises `restoreTabs`'s
blank-branch — `t0` created, found blank, closed, the record rewritten to
`[]`, the unknown name ignored, and the host list rendered from the shared
`store` key.

**Could NOT be exercised off-device**, and therefore all of it is in the
device checklist:

- anything behind `ui.onKey` / `ui.onTouch` — both are stubs that never
  fire on PC, which covers the whole key path, the one-shot modifiers, the
  long-press/drag/release selection gestures and the tab taps;
- `ssh.connect` is a stub that never connects, so the connect/auth/trust
  flows and every `ssh.write`/`resize` are unproven; the form callbacks
  behind `ui.screen` never run either;
- the **re-attach** branch of `restoreTabs` (a non-blank persisted term):
  run_pc has no way to stage a persist term for another owner and no app
  restart;
- pixels: tab bar, selection highlight colours, the term's own cursor cell,
  underline position, and whether hide→show actually repaints on glass.

## Device checklist (the orchestrator runs this)

Push both apps; `ssh_vt` stays installed as the fallback. Note that
`vault` is namespaced by app name, so **the password and host key must be
entered again for `ssh_vt2`** (the `store` host list is shared and will
already be there).

1. **Connect** to the linux tailnet host (`100.124.214.100`, memory
   `tailnet-hosts`): new host → password → host-key trust page → shell
   prompt appears. Type `whoami`, `date`.
2. **Full-screen TUI**: `vim` (or `nvim`) and `less` on a long file.
   Watch for: alt screen entry/exit not polluting the shell scrollback,
   the DA/DSR handshake not hanging the startup, the status line's
   Japanese, and the cursor cell tracking the caret. `top` for the
   redraw storm.
3. **Rotation** with a session live: portrait ↔ landscape, and dock
   insert/remove (the keyboard reservation changes 480→80). The grid must
   re-fit, the pty must learn the new size (`stty size` agrees), the
   content must survive, and nothing may be left painted below the grid.
4. **Tab switching**: three sessions, tap between tabs. **This is the
   Defect-A regression test** — each switch must repaint the whole
   incoming screen immediately, with `vim` sitting idle in one of them.
5. **Selection / clipboard**: long-press → drag across a wide-character
   run → release; the notify counts characters, the highlight covers whole
   wide glyphs, the copied text has no filler spaces, and the highlight
   disappears when the finger lifts (the hide→show repaint). Paste into
   another app via `clip_mirror`/`clipboard.get`.
6. **Paste in**: copy multi-line text elsewhere, paste with the control-bar
   button → the dangerous-paste confirm page appears; send it. Note that
   nothing is bracketed (Defect B) — check whether a `vim` insert-mode
   paste is mangled enough to make Defect B a blocker.
7. **Japanese input**: `ui.ime(1)` opt-in only. The preedit float must
   anchor under the **terminal's** cursor (§10.2's caret sink — this is
   the first time anything but `ui.caret` feeds it), including after a tab
   switch, and the committed string must arrive as one `ssh.write`.
8. **Background/foreground**: switch to the launcher and back
   (Decision 25). While `ssh_vt2` is in the background its terminal must
   paint **nothing** over the other app; on return the screen must come
   back complete, with the session still alive and the output that arrived
   meanwhile on screen.
9. **Restart / persist**: `sys.stop("ssh_vt2")`-and-relaunch (or launch it
   again from the launcher). The old screen must come back as a `×`-marked
   tab; connecting from the host page must recycle that tab (RIS, then the
   new session in the same term). After a **reboot** the record must
   restore nothing and the app must open on the host page.
10. **Teardown**: disconnect (`exit`) with 1 and with 3 tabs open, then
    "切断済みタブを閉じる". `sys.heap()` before/after a full cycle should
    return to its baseline (3 VT terms ≈ 500KB PSRAM while open).
