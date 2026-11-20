---
date: 2026-11-20T20:08:51+0100
author: Janos Sonntag
commit: 9132f46
branch: main
repository: fenestal
topic: "ambienceDJ frame cost and latency Performance"
tags: [performance, latency, ambiencedj, overlay, frame-policy, text-measure]
status: complete
last_updated: 2026-11-20T20:08:51+0100
last_updated_by: Janos Sonntag
type: refactoring
---

# Handoff: ambienceDJ's frame cost and latency, reviewed and fixed; the NAS push is unfinished

## Task(s)

**Completed.** A read-only review of `/home/json/ws/ambienceDJ` as a fenestal
application, then implementation of its findings. Two questions drove it: what
fenestal should improve (API-wise) for a UI-only application, and where the
program could get lower latency and a cheaper frame.

- Review and findings — **done** (delivered in chat, not written to a file).
- Implementation in ambienceDJ — **done**, committed `da77310`, built, selftest
  green.
- Implementation in fenestal — **done**, committed `a8b84ad` and `29d46fb`.
- Push — **partly done, the NAS leg failed** (see *Other Notes*).
- Verification by ear and by eye — **not done**, nobody has run the program.

The previous session's uncommitted work (a `ruff format` pass over
`examples/embed.c`, `examples/events.c` and `tools/mkdist.py`, plus its sweep
handoff) was also committed, as `2c322dc` and `9132f46`, and went out with the
rest.

## Critical References

- `/home/json/ws/ambienceDJ/src/radio_run.c` — the mix, the engine lock, and
  `radio_view()`, which is where the window meets the audio thread.
- `/home/json/ws/ambienceDJ/src/ui.c` — every frame: the overlay, the text
  measurement, the redraw interval.
- `/home/json/ws/fenestal/DESIGN.md` — *Recording* (what a rectangle is,
  what an overlay costs), *At rest* (when a frame is drawn at all), *Late
  latching* (why vsync stays on).

## Recent changes

**ambienceDJ, `da77310`** — all in `src/ui.c` and `src/radio_run.c`:

- `src/ui.c:205-227` — `text_w()`, a memo in front of `fnl_text_width()`,
  keyed on a **copy** of the string (a caller's buffer is reused, so a
  changed string must miss). Routed through it: `chip_w()` (`:789`),
  `chip_box()` (`:833`), `knob_cols()` (`:1112`), `heading()` (`:365`), the
  wordmark in `draw_titlebar()` (`:456`) and `ui_install_bar()` (`:477`).
- `src/ui.c:98` — `COL_CLIP` was `{1.00, 0.52, 0.36, 0.45}`; a rectangle's
  alpha is written to the surface, not blended, so the meter's clip zone drew
  as a solid block. Now an opaque dim coral `{0.22, 0.11, 0.10, 1.0}`.
- `src/ui.c:132-141`, `:1914` — the redraw is 100 ms while a phrase or a
  master is playing and 500 ms when nothing moves, chosen from a `_Atomic int
  ui_live` the frame writes at the end of `ui_overlay()`.
- `src/ui.c:1648` — the output card gained `%d fps   frame %.1f ms   in %.1f
  ms`, from `fnl_fps`/`fnl_frame_cpu_ms`/`fnl_input_latency_ms`;
  `stats_h()` (`:1596`) grew by one line to match.
- `src/radio_run.c:1627-1655` — `radio_render()` renders a block in
  256-frame pieces and takes `radio.eng_mu` again for each, so
  `radio_view()` never waits behind a whole 1024-frame decode.
- `src/radio_run.c:2323` — `radio_solo_ti()` reads `radio.mu` and one atomic
  instead of `eng_mu` then `mu`; every `solo_ti` access is now an
  `atomic_load`/`atomic_store`.

**fenestal** — no API change, no ABI change:

- `src/core/api_overlay.c:41-97` — `to_surface()`'s colour memo was 4
  entries round-robin, sized for the library's own fifteen colours. A
  caller's palette is thirty, so it missed on nearly every primitive at
  three `powf()` a miss. Now 64 entries keyed by an FNV hash over the colour,
  8 slots probed, the tag the whole colour.
- `include/fenestal/fenestal.h:1013` and `:1082` — says what a rectangle's
  alpha does, where a caller reads it.

## Learnings

- **ambienceDJ's frame can still block on disk.** `radio_view()`
  (`src/radio_run.c:3060`) takes `radio.eng_mu` and reads the engine under it;
  the feeder holds that lock across `engine_render()` — decode, resample,
  seek. The piece-splitting bounds the common case to one 256-frame piece
  (about 5 ms of audio), but the tail remains: `solo_swap()`
  (`src/radio_run.c:2427`) calls `engine_seek()` under the lock, and an
  `avformat_seek_file()` on a slow library is tens of milliseconds.
- **`fnl_text_width()` on a proportional face is not cheap.** It walks every
  baked scale — `FONT_SCALE_COUNT` is **6** in
  `build/linux/release/generated/font_data.h` — and every character, each a
  UTF-8 decode plus a binary search over the glyph ranges
  (`src/core/text_measure.c:86`). A 20-character UI string is about 120 glyph
  lookups. Monospaced sizes are one multiply per character, so the masters
  list's label loop was never the cost it looked like.
- **A rectangle is an opaque write.** `shaders/rect.frag` hands the colour
  over untouched with blending disabled; the rectangles are drawn before the
  strokes and the glyphs whatever the call order. A caller's alpha is not a
  wash. This bit ambienceDJ once and is now documented at both call sites.
- **A queued input event is a reason to record.** `src/core/frame.c:2146`
  bumps `record_gen`, so `fnl_invalidate_in()` is only the *idle* refresh;
  keys, presses and motion draw at once without an explicit invalidate.
- **`fnl_frame_cpu_ms()` and its siblings are valid only inside
  `on_overlay`**, and are published at most once a `COST_WINDOW`.
- **The language server's demo errors in this tree are pre-existing**: the
  demos include generated shader headers that only exist in a configured
  build tree (`build/<target>/<variant>/generated/`). Editing
  `fenestal.h` makes them surface. `./build.sh linux` configures those trees
  and they clear.
- **The NAS is not reachable from this shell.** `/mnt/storage/scm` and
  `/mnt/storage/ws` both read as `Permission denied`, while `stat` shows
  `scm` as `json:json 0700` and the session runs as `json` (uid 1000); the
  mount is NFS `sec=sys`. `touch /mnt/storage/ws/.nas-probe` fails too. This
  is a mount problem, not a repository problem.

## Artifacts

Read in this order:

1. `/home/json/ws/ambienceDJ` at `da77310` — `src/ui.c`, `src/radio_run.c`.
2. `/home/json/ws/fenestal` at `9132f46` — `src/core/api_overlay.c`,
   `include/fenestal/fenestal.h`.
3. `.rpiv/artifacts/handoffs/2026-11-20_17-03_schenker-win64-macos-sweep.md`
   — the previous session's, for the d3d12 and sweep work.

The review itself was never written to a file. Its findings are the
*Action Items* below.

## Action Items & Next Steps

**First — finish the push.** fenestal reached GitHub (`a630f90..9132f46`),
ambienceDJ reached nothing. Once the NAS mount reads:

```sh
cd /home/json/ws/fenestal  && git push /mnt/storage/scm/fenestal.git main
cd /home/json/ws/ambienceDJ && git push
```

**Second — verify by ear and by eye**, none of which has been done:

1. Listen for the mix. A seek, a solo, a crossfade and a shorts overlay must
   land where they did. The piece-splitting can move a cue at most one piece
   (about 5 ms) earlier.
2. Look at the meter with the mix paused: the clip zone should be a dim band,
   not a bright block and not a hole.
3. Watch the new card line while dragging a knob, then while paused. That
   second number is the baseline for everything else here.

**Third — the deferred review items**, each with the reason it was left:

- `buffer_ms` (default 180, `src/sinks.c:1625`) is the one number that trades
  a slow library read against latency. Pause and volume are already
  quantum-accurate (both applied in the RT callback); what the ring delays is
  *content* — a seek, a solo, a skip. Lower it, or flush the ring on those
  actions, once the new `ring N ms   fill F` line on the card says what the
  disk actually costs.
- The full `radio_view()` snapshot — the window never touching `eng_mu`. It
  reads a `const struct Track *` whose lifetime is tied to that lock;
  getting the ownership wrong is a use-after-free.
- `engine_seek()` on the feeder: prepare a seeked engine on the worker the
  way `next_solo` already is, and have `solo_swap()` install it.
- `radio_gone_sweep()` per frame (`src/ui.c` → `src/radio_run.c:2694`):
  it returns on `nheld == 0`, so the cost is one branch. Moving it to the
  worker would put the window's `held_add` against the worker's `held_drop`
  with no lock between them.
- The fenestal API additions this review motivated, none of them started:
  a cached measure (the library-side version of ambienceDJ's `text_w()`),
  `fnl_overlay_palette()` for a caller's colours, `fnl_overlay_counts()` so
  an app can see it is near the 1024-rect / 4096-glyph ceilings, and a UI
  clock that is honest about rest. Each needs `tools/abi/`, a `CHANGELOG.md`
  line, a caller in `tools/headercheck.c`, and `mkapidoc`.

## Other Notes

- Only fenestal's `linux` release tree was rebuilt. `clang`, `asan`, `tsan`,
  `analyzer` and `gc` are a commit behind.
- fenestal's `.git/hooks/pre-push` refuses any ref but `main` to GitHub;
  `history` goes to the NAS alone.
- The `ruff format` pass in `2c322dc` is exactly that:
  `ruff format --diff` is empty on all three files, and `ruff.toml` sets no
  quote style, so ruff's defaults applied.
- The ambienceDJ output card's new line is the instrument for the rest of
  this work: it is the only honest measure of what the window costs, and it
  did not exist before this session.