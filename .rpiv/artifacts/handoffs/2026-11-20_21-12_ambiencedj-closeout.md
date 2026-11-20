---
date: 2026-11-20T21:12:00+0100
author: Janos Sonntag
commit: 6f60c16
branch: main
repository: fenestal
topic: "ambienceDJ review items closed; two API additions declined with reasons"
tags: [handoff, performance, latency, ambienceDJ, overlay, text-measure, abi]
status: complete
last_updated: 2026-11-20T21:12:00+0100
last_updated_by: Janos Sonntag
type: refactoring
---

# Handoff: the ambienceDJ review's open items, closed

## Task(s)

**Completed.** The session before last left a list of open items; this one
closed every item that was not the user's own (ear and eye). In order:

- The four fenestal API additions the review motivated — **two built, two
  declined** (detail below).
- `radio_gone_sweep()` — **closed as a decision**, one comment at the call
  site.
- The full `radio_view()` snapshot — **closed with a smaller change that
  meets the same goal** (detail below).
- The `win64` and `macos` checker trees — **built**, in the full sweep.
- Both trees — **committed and pushed** (fenestal to NAS and GitHub,
  ambienceDJ to the NAS).

## Critical References

- `/home/json/ws/fenestal` at `6f60c16` — `src/core/text_measure.c`,
  `src/core/api_overlay.c`, `include/fenestal/fenestal.h`, `tools/abi/`.
- `/home/json/ws/ambienceDJ` at `a34a935` — `src/radio_run.c`,
  `src/ui.c`.
- `.rpiv/artifacts/handoffs/2026-11-20_20-38-39_ambiencedj-handoff-resumed.md`
  — the handoff this one closes; its *Action Items* are the list.

## Recent changes

**fenestal, `6f60c16`** — one commit, two additions:

- `text_measure.c`: `text_width()` now memoises. 32 entries, keyed on the
  string's bytes (FNV) and the face, not the pointer — a caller's buffer is
  reused, so a changed string must miss. A string of 32 bytes or more is
  never held and always measured. `fnl_text_width()` is `[any]`; the memo is
  `_Thread_local`. Safe because the cells are loaded once, before any thread
  (`text_load_cells()`); a reload would want the table cleared with it.
- `fnl_overlay_counts()` (`FnlOverlayCounts`): rects, strokes, glyphs, each
  with its ceiling, for the frame being recorded. `[overlay]`; zeroed
  everywhere else. `abidump` carries the struct, `headercheck.c` calls it, the
  ABI baselines moved in the same commit (`fenestal-0.abi`,
  `fenestal-0-gl.abi`, and the three `.syms`).

**ambienceDJ** — two commits:

- `3a13832`: `radio_view()` takes the engine lock with `pthread_mutex_trylock`
  or goes without, showing the previous frame's engine half from a
  thread-local copy. The render thread never waits on the decoder.
- `a34a935`: one comment at `ui.c`'s `radio_gone_sweep()` call site saying
  why it stays on the frame (returns on `nheld == 0`; the window's `held_add`
  would race the worker's `held_drop`).

## Learnings

- **Two of the four fenestal API additions are declined, with reasons.** The
  next session should not re-open them without new information:
  - `fnl_overlay_palette()` — **not needed**. The colour memo added in
    `a8b84ad` is keyed on the colour's own bytes (FNV, 8 slots probed), so a
    caller's thirty-colour palette already hits; registering a palette would
    only move the same conversion somewhere it could go stale.
  - A "rest-honest UI clock" — **not a library call**. What is live in a UI
    is the caller's own state (a phrase playing, a solo on), which the library
    cannot see; the honest clock is the caller's liveness plus
    `fnl_invalidate_in()`. A library-side guess would be wrong for any app
    whose motion is not the library's.
- **The full `radio_view()` snapshot was declined as specified, and the goal
  met another way.** The snapshot would have to publish the mix position,
  which is the *window's* heard-frame count mapped through the voice's
  origin/bus0/tempo — only the window knows that count, so the feeder would
  either duplicate the engine's bus-frame mapping or publish a position one
  frame stale anyway. The trylock removes the wait with neither: the render
  thread never blocks, and a frame that could not have the lock shows the
  last one that did. One frame of a card redrawn ten times a second.
- **`engine_close()` does not free the `Track`** (`voice_free` on the three
  voices, then the engine): a track's lifetime is the library's arrays and
  `park`, not the engine's. That was checked before deciding the snapshot's
  risk; it is why the trylock was enough.
- **A jump now reopens the master rather than seeking the live one.** The
  live engine belongs to the feeder, so a drag costs one open per event *on
  the worker* and never a wait on the render path. One seek is still made on
  the feeder: `solo_render()`'s loop rewind, which cannot be handed off
  because that block needs its samples in the same call (one seek per
  play-through, not per frame).
- **The full sweep is green after the fenestal change**: linux (release,
  clang, asan, tsan, analyzer, gc), linux-arm64 (release, asan, tsan, gc),
  win64 (release, analyzer, gc), macos and macos-arm64 (release, gc) — all
  built 21:05–21:11, no warnings, the ABI check passing in each native tree.

## Artifacts

Read in this order:

1. `.rpiv/artifacts/handoffs/2026-11-20_20-38-39_ambiencedj-handoff-resumed.md`
   — the list this closes.
2. `git show 6f60c16` and `git show a34a935` — the code itself.

## Action Items & Next Steps

**Ear and eye, still nobody's** (unchanged from the previous handoff):

1. The mix: a seek, a solo, a crossfade, a shorts overlay landing where they
   did.
2. The meter's clip zone as a dim band, not a bright block and not a hole.
3. The card line as the baseline, and — new — a jump landing where it was
   dropped, with the card showing one frame of the old position at most.

**The deferred items that remain** (each with the reason it is left):

- `buffer_ms` (default 180, `src/sinks.c:1625`) — still wants the card's
  `ring N ms   fill F` reading, which nobody has taken.
- The four fenestal additions above: two done, two declined with reasons.

## Other Notes

- Both trees are clean and pushed: fenestal `6f60c16` on the NAS and GitHub
  (`sothis/fenestal`), ambienceDJ `a34a935` on the NAS.
- The `win64` and `macos` checker trees are now built and current; nothing
  outstanding there.
- The next handoff should be written in this directory when the ear-and-eye
  pass has been done, since that is the only open item left.