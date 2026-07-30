# Phase 5 manifest — recording mode: a session's display content in the black box

`docs/term-design.md` §4.4 "記録モード" (the 2026-07-30 exception), §7.1,
§11.5. Branch `feat/term-record`, off `feat/term-native` at a0cc62b.

**Status: implemented, off-device verified, NOT device-verified, NOT
committed.** Nothing in the shipped behaviour changes until a human turns
recording on for a live session, which is the whole design.

## Why this is a new phase and not a phase-4 addendum

It **reverses a documented design decision**. §4.4 said, in as many words,
that SSH session content (class B) is *never* put in the LP black box — 量・
プライバシー両面 — and §7.1's whole classification rested on that sentence.
The device owner authorised the exception on 2026-07-30. A ledger that
recorded this as "phase 4, also we added a flag" would hide the only thing a
future reader needs to know: which sentence changed, who changed it, and what
they accepted in exchange.

## What the user decided (2026-07-30), verbatim in substance

1. **Recording exists at all**, putting the terminal's *display content* into
   the LP black box — not a file.
2. **Continuous** capture, not only at events.
3. **Off by default, per-session opt-in.**
4. **The opt-in must not be remembered anywhere.** Every session starts OFF:
   no NVS/`store`, not in ssh_vt2's saved tab record, and a re-attached
   persist term comes back off even though its scrollback survived.
   *Rationale the user gave:* the failure this forecloses is a forgotten
   opt-in — a flag set once, persisted, and still quietly recording months
   later into a region that a pull returns in plaintext. A per-session-only
   flag makes "am I being recorded?" answerable from the current screen and
   nothing else.
5. **A recorded session must be readable from the PC.** This is the *purpose*.
   §7.2 gives no cross-app read API, which is exactly why an nvim Lua error on
   the glass could not be diagnosed earlier that day. `bb_pull.py --session`
   is the deliverable, not a convenience.
6. **Content that is displayed and never scrolls off must be captured too** —
   manual "capture now", plus a settle-debounced snapshot — because the
   motivating case (an error in nvim's message area on the alt screen, read
   and then dismissed) scrolls nothing at all.

Consequence the user accepted, now a user-level rule: a recorded session is
pullable **in plaintext** by the signing-key holder and survives resets until
power-off; with screen capture that covers a secret merely *displayed*. "Do
not record a session that will display secrets."

## Contract surface (test author reads these)

| File | Role this phase | Read by tests |
|---|---|---|
| `term_registry.h` | **The contract.** New block "RECORDING" at the top: R1 (off by default, per term, never remembered — with the six lifecycle clears enumerated), R2 (the tee is at the line level), R3 (three screen-capture triggers), R4 (what the operator accepted), R5 (off costs nothing because it is a NULL callback, not a tested boolean). API: `term_registry_record`, `_recording`, `_record_screen`, `_panic_capture`. Constants `TERM_REC_SETTLE_MS` / `_SNAP_MIN_MS` / `_SCREEN_MAX` / `_PANIC_MAX`. `term_slot_info_t::recording`, 9 new `rec_*` stats | yes |
| `term_lp_ring.h` | **Extended.** `TERM_LP_CLASS_TERM` (=2), `TERM_LP_F_SCREEN`, `term_lp_panic_append`, `term_lp_panic_ready`. P3 rewritten (class B is unreachable *by default*, and reachable only through one per-session human switch). New "RECORDING MODE" block, incl. the compatibility direction | yes |
| `term_core.h` | **Extended.** `term_record_fn` + `term_core_set_record_cb` / `term_core_recording`, and `term_core_screen_hash`. The long comment on `term_record_fn` is where the choice of tee point is argued | yes |
| `docs/term-design.md` | §4.4 rewritten (the exception, marked as the user's decision and dated), §3.2's table, §7.1's classification, §8's API list, §11.5 | yes |
| `PHASE5_MANIFEST.md` | This ledger | yes |

### The JS surface added

```js
term.record(id);            // 1 recording, 0 not, or a negative term_err_t
term.record(id, on);        // 0 or a negative term_err_t
term.recordScreen(id);      // 0, or term.INVAL when not recording
```

- **`term.record(id)` is THE introspection point.** There is deliberately
  only one, and a UI must read it on every repaint rather than caching a
  boolean: the platform clears recording at six lifecycle transitions, and a
  cached copy is how a badge ends up claiming a session is recorded when it is
  not. Aggregate counters live in `term_registry_stats_t` (platform-only, as
  the rest of that struct is).
- **There is no `term.create` option, and adding one would break decision 4**,
  not merely its spirit: a field on the config object is a field an app puts
  in its saved-tab record and replays after a reboot.
- **`term.record` on a LOG term is `term.MODE`.** A log term's content is
  already in the black box as class A (`term.log` and the print sink tee
  unconditionally), so arming one would write every archived row a *second*
  time as `TERM_LP_CLASS_TERM` and halve the retained history to say nothing
  new. Recording is the class-B exception and nothing else, so it is refused
  where it has no work to do rather than allowed and documented as pointless.
- Errors are still return values; nothing throws (§8). No new constants.

## Implementation (test author must NOT read)

| File | Change |
|---|---|
| `term_core.c` | The tee call at the end of `sb_archive_row` (hands over the bytes just written to the arena — no second serialisation, no buffer); `term_core_screen_hash`; the two setters |
| `term_registry.c` | The recording section (writer id, capture, arm/disarm, the settle debounce), the public four, the six lifecycle clears, `moved`+`now` in the drain |
| `term_lp_ring.c` | `TERM_LP_CLASS_TERM` accepted by append and by the chain walk; `TERM_LP_F_SCREEN` passed through and every other flag bit dropped; `lp_class_tag()`; the two panic entry points |
| `mqjs_runtime.c` | `js_term_record`, `js_term_recordScreen` |
| `device_stdlib.c` | Two `JS_CFUNC_DEF`s |
| `gen/device_stdlib.h` | ROM regen (`stdlib_tool -m32`). `gen/mquickjs_atom.h` regenerated and **byte-identical** again, so it is not in the diff |
| `main/panic_note.c` | `term_registry_panic_capture()` after the note |
| `tools/bb_pull.py` | Class tokens documented; `--session` transcript view; `--writer` filter |
| `examples/ssh_vt2.js` | Tab menu (tap the active tab), the recording indicator, `tabLayout`'s 7th argument, 16 new selftest checks |

No test directory was touched. `host_test/run_tests.sh` and
`pc_test/run_pc_tests.sh` both glob the component directory and no `.c` file
was added, so neither needed a change.

## Decisions

### 1. The tee is at the line archive, not the raw ssh byte stream

The seam is `term_core`'s `sb_archive_row`, via a callback. Three reasons, in
order of how load-bearing they are:

- **A raw-byte tee would not deliver 表示内容 at all.** The ring strips every
  escape sequence, and that is a security property, not tidiness: a pulled
  record is printed on the operator's terminal, so a retained escape is
  control injection (phase 3's rule). Raw ssh output is escape-dense; with all
  the cursor addressing removed, a full-screen TUI redraw becomes unreadable
  text soup.
- **A row archived to scrollback IS the display content**, already resolved
  from escapes into ordered text.
- **Cost during the case the user worried about is zero.** In-place repainting
  archives no rows, and the alt screen never reaches scrollback at all
  (`scroll_up`'s `archive` predicate requires `active == MAIN`). Measured: 200
  full-screen `top`-shaped repaints produced **0** records.

Corollary, and the hole decision 2 exists to fill: content that is displayed
and never scrolls off passes no row to the callback.

### 2. Screens are captured too, on three triggers

| Trigger | When | Why this one |
|---|---|---|
| arming, and `term.recordScreen` | a human pressed a button | The reason they pressed it is on the glass *now*; waiting for the settle timer loses it to the next redraw |
| settle-debounced | quiet for `TERM_REC_SETTLE_MS` (1,200) **and** `term_core_screen_hash` differs from the last capture | Records the screen states a human actually reads. A storm never settles, so it stays free |
| session end / close / detach / panic | lifecycle | A hang during `top` scrolled nothing, so this is the only record of what it showed |

- **The change test is a hash, not a serialisation.** Serialising to compare
  would cost the same as capturing. `term_core_screen_hash` is FNV-1a over
  cp/fg/bg/flags plus geometry, and deliberately **excludes the cursor**: a
  caret that moves without changing a cell has not changed what a human reads.
- **The hash runs at most once per settle interval per recording term**, and
  never for a term nobody records or a screen nobody stopped writing to. The
  condition order in `record_settle_locked` is the design: `!record` →
  `moved` → `rec_settled` → not-quiet → rate-limited → *only then* hash. An
  unchanged settled screen sets `rec_settled` and cannot become interesting
  again without bytes arriving.
- **The debounce cannot starve the ring**: one capture may land per
  `TERM_REC_SNAP_MIN_MS` (5,000), each bounded by `TERM_REC_SCREEN_MAX`
  (8,192 B ≈ a third of the APP partition), and `rec_snaps_deferred` counts
  the settle episodes the limit turned away — once per episode, not per frame.

### 3. A new class, in the APP partition

`TERM_LP_CLASS_TERM = 2` (2..14 were free; 3..14 remain), with
`TERM_LP_F_SCREEN` distinguishing capture rows from scrolled lines.

- **Why not write session content as APP.** The tag is what makes
  `--session` possible at all, and what a future redaction or filter policy
  would name. Reusing APP would have made both permanently impossible.
- **Why the APP partition and not a third one.** SYS's 8 KiB is the
  anti-eviction-DoS reserve for the platform's last words and the panic note
  (P1). Recording is a *chattier* writer than the chatty app P1 was written
  about; it must not be able to flush them. Splitting the region again would
  cost live capacity in the one memory that has no second source.
- **Compatibility, stated because it is a real cost.** `TERM_LP_VERSION` is
  **not** bumped: this build reads older images exactly as before. The reverse
  fails — a build without class 2 rejects the whole retained image as BAD, so
  a **downgrade** loses one boot's log tail. Bumping the version instead would
  have lost the same tail on the *upgrade* as well, so not bumping is strictly
  better.

### 4. Recording on/off IS the callback pointer

`record_set_locked` installs or removes `term_core`'s `record_cb`. With
recording off there is no route from a term to the ring — the same kind of
statement P3 made before this feature existed, rather than a filter that could
be wrong. Sabotage-verified below.

### 5. Six lifecycle clears, and why each one is there

| Transition | Clear | Because |
|---|---|---|
| create (new slot) | explicit field writes | A slot is reused; "whatever the last tenant left" is not a guarantee |
| **re-attach** | `record_clear_locked("reattach")` | The path a restarted app takes to pick a tab back up — precisely where a persisted flag would come back to life. The scrollback survives; the flag does not |
| owner stopped → DETACHED | `"detach"` | The app showing the session is gone |
| **producer bind** | `"bind"` | The one event *every* new session passes through: a fresh connect, a reconnect into a tab cleared with RIS, a re-pipe onto another channel. This is what makes the guarantee independent of app code being careful |
| producer detach ack | `"session-end"` | The ssh session ended |
| stage 1 (close / teardown / LRU evict) | `"close"` | The term is going away |

Each of the last five also takes the final screen capture on its way out, so
"the flag is cleared" and "the last screen is kept" are the same event.

`term_core_reset` (RIS) deliberately does **not** clear the callback, for the
same reason it does not clear `reply_cb`: those are the owner's wiring, and a
*remote* that can emit `ESC c` must not be able to silently desynchronise the
indicator from what is being recorded. The session-lifecycle rule lives one
layer up, where the session is.

### 6. The writer id carries the tab: `"<owner>:<name>"`

A transcript of three simultaneously recorded tabs is useless if every line
says `ssh_vt2`, and the writer field is the only per-record provenance the
format has (P2). Both halves are as trustworthy as the owner alone (`owner`
from the signed push, `name` inside that owner's namespace, I3).

**The cost, honestly:** the writer table interns **8** names, shared with
every app's `print()`. Three recorded tabs are three of them. Overflow, or a
name over 31 bytes, degrades to the bare owner and then to
`WRITER_UNKNOWN` — **attribution only, never content**.

### 7. The panic capture is the riskiest code here, and is ordered last

`term_registry_panic_capture()` reads a grid in **PSRAM with no lock, in panic
context**. The table itself is `.bss` and safe to walk; the core it points at
is not ours alone at that moment, and a cache-disabled panic could fault the
read. The design contains the damage instead of claiming it cannot happen:

- the panic **note** is appended first, and P5 publishes a header per record,
  so everything appended before a fault reads back;
- therefore the worst case is a panic becoming a **double panic** — the UART
  dump cut short, the reset reason changed — **without losing black box
  content**;
- and the debounced capture has usually already stored the last screen a human
  could read, so this path only adds the case where the screen was still
  changing when the device died;
- it is one-shot, budgeted at `TERM_REC_PANIC_MAX` (4 KiB), visible terms
  first, with its own static buffers (sharing `s_rec_row` would let a crash
  mid-capture publish half of one row and half of another as one record).

**This is a device-checklist item, not a proof.** See D6 below.

### 8. `record_screen` runs inline under the lock, not posted to the UI task

`snapshot`/`read` post a job because they want a frame boundary *for the
caller* and copy into the caller's buffer. Neither applies to an append into
the ring, and the table lock is what protects a core from the parser
(term_registry.c's opening comment: one mutex covers the table **and** every
term's core). So a capture under the lock is exactly as consistent as the
drain's own parse.

### 9a. `record` is refused on a LOG term

`TERM_ERR_MODE`. See the JS-surface note above: it would be pure duplication,
and the duplication would come out of the same 23.5 KiB partition the
non-duplicated history lives in.

### 9. `recordScreen` is refused when not recording

`TERM_ERR_INVAL`, not a silent capture. It is a button that exists only while
the indicator is lit — not a second, unannounced door into the ring.

### 10. The UI: tap the active tab

Tapping a *non-active* tab still switches, so no existing gesture was taken.
Tapping the tab you are already on opens the tab menu. Three redundant
indicators, because each survives a different layout accident:

- `●` prefixed to the tab's label (survives a narrow bar),
- the tab's background turns **red, active or not** (survives the label being
  clipped at 22 chars),
- ` ●REC ` at the right edge when the *active* tab records (survives the tab
  not fitting in the bar at all).

`drawTabs` asks C for the state every repaint (Decision: no cached boolean).
Transitions are `print()`ed as well as toasted — the Defect C lesson: a toast
is gone in five seconds, and a transcript without "recording started here" in
it cannot tell a later reader where the recording began.

### 11. The manual capture spends none of the debounce's state

*(Defect the test author's `test_rec_screen`
`case_manual_does_not_reset_the_rate_limit` caught — their D1. The header had
said this since the first commit; the code did the opposite.)*

`capture_screen_locked()` moved `rec_snap_ms` on **every** landing, and
`record_settle_locked()` measures `TERM_REC_SNAP_MIN_MS` from that field. So
pressing "record this screen now" closed the rate limit for the next 5 s: the
following settle capture was turned away and counted `rec_snaps_deferred`. The
button cost the operator the automatic snapshot of whatever the remote painted
next — precisely the content the button-presser was about to want, and the exact
opposite of what R3(a) is for.

The fix is a split, not a special case: `capture_screen_locked()` takes a
`charge_limit` flag, the three automatic triggers (arm, settle, stop/lifecycle)
pass `true`, and `record_screen()` passes `false`. The **hash** still moves for a
manual capture, because it answers a different question — "is this screen
already in the box" — and the answer is yes however it got there. The two
episode flags (`rec_settled`, `rec_deferred`) are still cleared by the manual
path, so a screen that changes *after* the press is still picked up.

The suite's A/B is the right shape for this and is why it was caught: identical
timing with and without the press in the middle, asserting the same number of
settle captures. A single-arm test would have passed.

### 12. The panic ordering was right; the assertion that checked it could not fail

*(The test author's D2, and the one report that was **not** a defect in the
registry. Reported as "walks slots in index order"; it does not.)*

`term_registry_panic_capture()` has walked the table twice since the first
commit — pass 0 taking `view.visible` terms, pass 1 the rest — and an
instrumented run of the suite's own fixture confirms it: the visible term in the
**higher** slot is appended first (`pass=0 slot=1 w=app_alpha:pV`), the hidden
one in slot 0 second.

What failed was `rec_util.h`'s `p5_writer_of()`, which formatted into **one
shared static buffer** and returned a pointer to it. The suite holds two of its
results at once:

```c
w_shown  = p5_writer_of(OWNER_A, "pV");
w_hidden = p5_writer_of(OWNER_A, "pH");   /* rewrote what w_shown points at */
```

so both pointers read `"app_alpha:pH"`, `first_visible` and `first_hidden`
landed on the same record, and `first_visible < first_hidden` was unsatisfiable
— the assertion could not pass whatever the ordering was, and could not fail if
the ordering broke. **This is the only change made inside `host_test/`**: the
helper now interns one stable buffer per distinct `owner:name`. The assertion is
strengthened, not weakened — before the fix it tested nothing.

The registry keeps the two-pass loop; its comment now says *why* the ordering
rule exists (the budget is shared, so index order spends it on the tab nobody
was looking at) rather than only that it is obeyed.

### 13. `TERM_REC_PANIC_MAX` is a cap on what is written, not on what precedes the last record

*(The test author's D3. Their diagnosis named the markers; the markers were in
fact charged — `spent += n` — and the overrun was the row loop.)*

Both loops tested `spent < TERM_REC_PANIC_MAX` **before** appending and added
the cost **after**, so the last record of the walk always straddled the cap:
4,126 payload bytes observed against a 4,096 budget. Every append now checks for
fit first (`spent + need > MAX` → stop on a row boundary; a marker that does not
fit means the term contributes nothing at all, because rows without their own
marker read as a continuation of the previous term's frame). Measured after the
fix: **4,087 bytes, 105 records, both markers present.** "4,096 plus however
long the last row happened to be" is not a bound worth writing down, least of
all for the one path that spends a shared LP region under a fault.

### 14. Four behaviours the phase-5 suites pinned, now written into the header

The suites documented these in their own comments and asked for a ruling. All
four are **kept as-is** — no behaviour changed — and are now stated in
`term_registry.h` so a later change to them is a change to a documented
contract:

1. **`record(id, owner, true)` on a DETACHED term returns `TERM_OK`.** Kept.
   It behaves like any arming (tee on, immediate capture of the retained
   screen) and **re-attach clears it** (R1), so it cannot carry into the session
   the app picks up. What it cannot do is answer R4: no app is showing the term,
   so there is no indicator — which is why nothing in the platform arms a
   detached term on its own. Refusing it would mean an error whose meaning is
   "this id is yours but not right now", a worse contract than a flag the next
   transition throws away. The suites' re-attach case needs to be able to arm
   it.
2. **A duplicate `producer_ack()` on a live term with no producer bound is
   processed as a real session end** — recording cleared, final capture taken,
   *not* counted in `stale_acks` (which means "the slot moved on", and it has
   not). Fail-safe direction: the cost is one extra screen in the box, while
   ignoring an ack that is real would be recording that outlived its session,
   the one thing R1 exists to prevent.
3. **An interior blank row is written as one space.** The ring refuses an empty
   payload and dropping the row would shift every row below it in a pulled
   transcript, so this is the only rendering under which R3's "one record per
   row" and the ring's "no empty record" both hold. Trailing blank rows are
   still dropped. A transcript reader may rely on both halves.
4. **Counter semantics.** `rec_screens` counts markers (one per landing),
   `rec_screen_rows` counts rows, so a landed capture appends
   `rec_screen_rows + 1` records and a blank-screen capture is a landing with
   **0** rows. The panic path moves `rec_panic_rows` only and leaves
   `rec_screens` at 0: its marker is not a "screen the running system
   captured", and that counter is the one thing a post-mortem reader cannot
   re-derive from the records.

## Verification (off-device)

### Host suite — the shipped one, unchanged

```sh
cd components/term_core/host_test && sh ./run_tests.sh
```

**45 suites, ALL SUITES PASSED** — the same 45 as phase 4. No suite was added;
the phase-5 suites are the test author's job.

**After the test author's six phase-5 suites landed: 51 suites, ALL SUITES
PASSED** (Decisions 11-13 are the three fixes that took it there; 49/51 on
arrival). Re-run afterwards, both green: the 5 PC suites
(`run_pc_tests.sh` → ALL PC SUITES PASSED) and `tools/smoke_examples.sh`
(12/12 examples clean).

### PC suites — the shipped ones, unchanged

```sh
cd components/mqjs && sh ../term_core/pc_test/run_pc_tests.sh
```

**ALL PC SUITES PASSED** (5: `pc_term_basic`, `errors`, `name`, `resize`,
`pc_term_probe`). `tools/smoke_examples.sh` — **12/12 shipped examples clean**,
including `ssh_vt2`.

`examples/ssh_vt2.js` with `SELFTEST=true` through `run_pc`: **41/41,
`SSHVT2 SELFTEST: ALL PASS`** — the pre-existing 25 plus 16 new ones covering
the indicator (`tabLayout`'s red/dot/badge and an *inactive* recording tab
staying red) and `term.record`'s contract (off by default, query form,
`recordScreen` refused while off, `term.MODE` on a LOG term, a bad id
answering negatively rather than throwing).

### Throwaway harness (implementer's, deleted)

Built at `-O1 -Wall -Wextra` with **ASan + UBSan** against the real component
sources and `host_test/`'s fakes. **51 checks, 0 failures.** Output:

```
  [storm] 200 repaints of 394 bytes -> 0 new ses records, 0 new scr records
  [stats] rec_on=4 rec_off=1 rec_cleared=3 lines=12 screens=10 rows=66 deferred=1 trunc=0
  [ring] sys=1 app=0 ses=12 scr=76 (ses 93B, scr 881B)
SUITE-OK [record] 51 checks, 0 failures
```

What it proves, claim by claim:

- **Recording off is provably inert.** 32 lines fed and scrolled off with
  recording off: zero `ses` records, zero `scr` records, zero `ses` bytes —
  and a walk of the whole region finds **0** occurrences of the fed text.
  Advancing the clock 10 s produces no capture either (the settle timer cannot
  fire for a term that is not armed).
- **Recording on gets archived lines in, with the new class.** 12 archived
  rows appear as `TERM_LP_CLASS_TERM` (not APP), the last one's text matches,
  and the writer id is `app_alpha:t0` — the tab, not just the app.
- **A `top`-style redraw storm archives nothing.** 200 repaints of a captured
  in-place redraw (394 B each: `ESC[H`, then `ESC[r;1H ESC[K` per row, no
  newline) → **0** new `ses` records and **0** captures, because a storm never
  settles. The alt screen half of the same claim: 50 lines written on the alt
  screen → **0** archived rows.
- **The debounce behaves as documented.** Not captured before
  `TERM_REC_SETTLE_MS`; deferred (and counted) while inside
  `TERM_REC_SNAP_MIN_MS` of the previous capture; captured once the limit
  opens, with the error text in it; and **20 further settle windows over an
  unchanged screen produce nothing**.
- **The end-of-session snapshot lands.** Stopping recording writes exactly one
  `--- screen stop ...` marker plus the visible row, after which 20 more fed
  lines produce zero records (the tee really is gone).
- **R1's non-persistence, at three transitions.** Binding a producer clears
  it; a detach ack clears it; and a **persist term that is detached and
  re-attached comes back OFF while its scrollback survives** (the same read
  that proves recording is off also proves `history-line` is still in the
  scrollback — the case is only meaningful if the history really did survive).

### Sabotage-verified

An agent-written test is unproven until the defect it names is introduced and
it fails.

| Sabotage | Result |
|---|---|
| Install the record callback at `create` (i.e. make "off" a filter that is absent rather than a missing route) | 3 failures: `c1.ses`, `c1.ses_bytes`, and the region-wide text search `hits` |
| Drop `record_clear_locked` from both producer-bind paths | 1 failure: recording still on after a bind (R1's "a new producer is a new session") |
| Remove the `TERM_REC_SNAP_MIN_MS` rate limit | 2 failures: `rec_snaps_deferred >= 1`, and a capture landing before the limit opened |

All three reverted; the tree contains none of them, and the full host and PC
suites were re-run afterwards.

### Reading it back on the PC

`tools/bb_pull.py`'s `split_record` / `transcript` exercised against a
hand-built record list covering `sys/`, `app/`, `ses/`, `scr/`, a screen
marker, a second tab and a malformed line:

```
$ ls -la
total 4

--- screen manual 80x28 rows=3 ---
E5108: Error executing lua
stack traceback:
other tab line
```

and with `--writer ssh_vt2:t0` the last line drops out. The device-side half
of the format (`lp_class_tag`) is what produces those tokens, and the harness
above is what confirms the classes and flags the tokens are derived from.

## Host-testable vs device-only

**Host-testable now, through `term_port` and fakes** — where the test author's
phase-5 suites belong:

- everything the throwaway harness covered, and it should be re-derived from
  the header rather than ported: R1's six clears (including LRU evict and
  `close_forced`, which the harness did not reach), R2's zero-cost claim, the
  debounce's five short-circuits, `TERM_REC_SCREEN_MAX` truncation (needs a
  geometry the harness did not build — 53×142 of multi-byte codepoints), the
  writer-table-full fallback to the bare owner, and `TERM_LP_CLASS_TERM`
  surviving a `term_lp_ring_open` chain walk;
- `term_core_screen_hash`'s stated properties: stable with no mutation,
  changes on a cell change, **unchanged by a cursor move**, changes on resize;
- `term_lp_panic_append`'s refusal on a deliberately corrupted shadow header;
- `lp_class_tag`'s four tokens, including the flags bug it fixed (a TRUNCated
  SYS record used to print as `app/`).

**Device-only:**

- D1 the tab bar actually being unmissable on the glass (three indicators, one
  screen, one photograph);
- D2 the settle interval *feeling* right — 1,200 ms is a guess about human
  reading, and the only test is using it;
- D3 the eviction rate of a real recorded session against 23.5 KiB (how much
  history a 20-minute ssh session actually leaves);
- D4 the LP write cost of a 53-row capture on the slow bus, from the UI task,
  measured as a frame-time bump;
- D5 the end-to-end goal: record an nvim Lua error on the device, pull it with
  `bb_pull.py live --session`, and read it on the PC;
- D6 **the panic capture** — provoke a panic with recording on (`sys.panic()`
  is the drill button) and confirm the black box still reads back, the note is
  present, the `--- screen panic ...` marker is present, and the reset reason
  is still `panic` rather than a double fault.

## Device checklist

1. Flash, `sys.blackbox()` → ring present, and **no `ses/`/`scr/` records**
   before anybody touches the toggle.
2. `ssh_vt2`, connect a session, tap the active tab → the menu shows
   "記録: 切 (既定)".
3. Turn recording on → **D1**: photograph the tab bar; the `●`, the red tab and
   the ` ●REC ` badge are all present. Switch to another tab: the recording
   tab stays red, the badge disappears (it follows the active tab).
4. `ls -la` a directory, `cat` a long file → `bb_pull.py live --session`
   shows the transcript, with the `--- screen start ...` marker at the front.
5. Run `top` for a minute → **D3/D4**: `sys.blackbox()`'s `appended` for the
   APP partition barely moves during the storm, and captures appear only when
   you quit `top` (or press "今の画面を記録する"). Watch for a frame-time
   hitch at the moment a capture lands.
6. `nvim`, provoke a Lua error, read it, `:q` → **D5**: the error is in
   `--session` output even though nothing scrolled. This is the acceptance
   test for the whole feature.
7. Disconnect the session (or kill it server-side) → a `--- screen
   session-end ...` capture lands and `term.record(id)` is 0.
8. Reconnect into the same tab → recording is **off** (R1, the bind clear).
9. Stop `ssh_vt2` and restart it → tabs restore, and every restored tab is
   **off** (R1, the re-attach clear). Confirm the scrollback did survive, so
   the test is not passing for the wrong reason.
10. Turn recording on, `sys.panic()` → **D6**: after the reboot,
    `bb_pull.py lastboot` contains the `panic:` note *and* the
    `--- screen panic ...` capture, and `esp_reset_reason` is `panic`.
11. Power-cycle → `lastboot` is absent (LP lost its charge), which is also the
    statement "recording cannot outlive a power cut".

## Open questions the implementation surfaced (for the user, not decided here)

1. **The writer table is 8 entries and recording eats them.** `TERM_LP_WRITERS`
   is 8, shared with every app's `print()`. On a device running the launcher,
   an app, `system` and `panic`, recording three tabs at once exhausts it and
   the extra tabs fall back to `WRITER_UNKNOWN` — the transcript is still
   complete but `--writer` can no longer separate those tabs. Raising it to 12
   costs 128 B of LP data area, which has to come out of `TERM_LP_APP_BYTES`
   (i.e. out of retained history). Not changed unilaterally, because the trade
   is 128 B of history against per-tab attribution in the multi-tab case, and
   that is a preference.
2. **The panic screen capture could be dropped.** It is the only genuinely
   risky code in the feature (Decision 7), and the settle-debounced capture
   already keeps the last screen a human could *read* — the panic path only
   adds the case where the screen was still changing at the moment of death.
   If D6 shows any sign of double-faulting, deleting
   `term_registry_panic_capture()` and its one call site costs the feature
   very little. Recommendation: keep it, verify D6 early, delete on the first
   sign of trouble rather than trying to make it safe.
3. **`TERM_REC_SETTLE_MS` (1,200) is a guess about human reading speed**, and
   the only instrument for it is a person using the device (D2). It is one
   constant in `term_registry.h`.
4. **The on-device reader has no session view.** `sys.blackbox("live")` returns
   the tokens, so a JS-side filter is a few lines, but nothing on the device
   presents a transcript. The user asked for the PC, so that is what was built;
   say so if the device-side view is also wanted.

## Not done in this phase

- **No redaction or filter policy.** The class tag exists so that one can be
  added; none is implemented, and inventing a heuristic for "this line looks
  like a secret" would be worse than the user-level rule.
- **No per-record encryption.** Unchanged from §4.4: single trust domain, one
  Ed25519 key. Recording raises the *stakes* of that, which is why the opt-in
  is per session; it does not change the model.
- **No `--session` output for the on-device `sys.blackbox("live")` reader.**
  The JSON dump carries the tokens, so a JS-side filter is trivial, but the
  consumer the user asked for is the PC.
- **No mouse reporting / NVS snapshot persistence** — still §11.6.
