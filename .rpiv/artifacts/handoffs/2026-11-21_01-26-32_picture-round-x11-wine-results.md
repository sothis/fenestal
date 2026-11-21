---
date: 2026-11-21T01:26:32+0100
author: Janos Sonntag
commit: 1deaf19
branch: main
repository: fenestal
topic: "The picture round: X11 and wine results, and the KMS half still owed"
tags: [implementation, overlay, image, stress, x11, vulkan, d3d12, kms]
status: complete
last_updated: 2026-11-21T01:26:32+0100
last_updated_by: Janos Sonntag
type: feature_development
---

# Handoff: the picture round (1deaf19) — measured on X11 and wine, KMS still owed

## Task(s)

Two things were owed from the previous session (recorded in `TODO.md` under *An overlay image*):

1. **A round where `read-pixels` exists** for the three new picture checks — `stress.py`'s `picture()` (`tools/drive/stress.py:1629`), which reads the middle row through `I` three times (drawn → destroyed → drawn again). Run on the linux release tree against `x11-vulkan x11-gl win64-vulkan win64-d3d12`. **Status: done, with two findings (below).**
2. **`kms-vulkan-bare`** for the LED bit test at open and `GETCRTC` in the motion the round drives — the other half of commit `7b4d5b5` ("The review's defects"). **Status: not done — no KMS on this desktop; needs a bare machine.**

Nothing of the *library* changed this session. The two files that were dirty (`testbed/overlay.c`, `tools/drive/stress.py`) were **pure formatter churn** (`ruff format` / `clang-format`, zero semantic change — verified with `ruff format --diff` and a byte-level look at the hunks); they were restored, so the working tree is clean and nothing was committed. The picture code itself — `picture_draw()` in `testbed/overlay.c:436`, `picture()` in `tools/drive/stress.py:1629` — was already committed in `1deaf19`.

## Critical References

- `TODO.md` § *An overlay image* — the owed list this session was clearing, and what verifies the pictures.
- `testbed/overlay.c:411-447` — `PICTURE_PX`, `picture_pixels()` (white/black checkerboard), `picture_draw()` (60% x, vertically centred, `I` toggles `g_picture_on`).
- `tools/drive/stress.py:1629` — `picture()`: three checks, `share > 0.03` drawn, `share < 0.01` after `I`.
- `DESIGN.md` § *Recording* — a picture leaves the image push constant in force; the regression shows in the panels flushed after `on_overlay` (the metrics panel, drop-down, veil).

## Recent changes

None to the repository this session. The dirty working tree (`testbed/overlay.c` 96/88 lines, `tools/drive/stress.py` 1559/810 lines) was `clang-format` and `ruff format` churn with no semantic diff, restored with `git checkout --` before the handoff.

## Learnings

### The picture path draws correctly on X11 (Vulkan and OpenGL)

Measured share of the middle row that is white, at a 2880x1730 window:

| environment | drawn | after `I` | after `I` again |
|---|---|---|---|
| `x11-vulkan` | 7.0% (176/2560 px) | 0.0% | 7.0% |
| `x11-gl` | 7.0% (176/2560 px) | 0.0% | 7.0% |

Both X11 rounds now pass the three picture checks. `x11-gl` is **111 checks, 0 failed**. `x11-vulkan` is 111 checks with the one pre-existing `log is clean` failure below.

The measured 6.9–7.0% is comfortably above the 3% threshold and the destroyed state is exactly 0.0%, well under 1%. The 0.3 s `quiet(PAUSE)` after `I` was long enough on this desk — no `I`-toggle race seen.

### `x11-vulkan`'s `log is clean` failure is a real, pre-existing library defect (not the picture)

Every `x11-vulkan` release round fails `log is clean` with:

```
X11: a frame that answers a size was not answered by Present within 34 ms; its sync value went without it
```

This is `sync_await_expire()` in `src/backend/x11/x11.c:2140`, fired from `x11_flush()`'s kept sync value on a resize frame (`SYNC_SHOWN_WAIT = 0.034`, `src/backend/x11/x11.c:38`). It is **Vulkan-on-X11 only**: `x11-gl` (same backend, same present path, no Vulkan `sync_due` wait) never prints it. It appears mid-drag-resize (`EV resize …` immediately after it in the log).

It is **not new**: the ledger shows every `x11-vulkan` release round since `1deaf19` fails on it (8/8), but the kept logs `build/drive/x11-vulkan-release.failed-2026110{2,5}*.log` and `x11-vulkan-gc.failed-20261106-114820.log` carry the same line going back to 2026-11-02, before the picture work. It reproduces with the picture check stubbed out. **This is the one thing keeping `x11-vulkan` from a clean release round on this desk, and it predates the picture.**

### The wine failure is not the library — the X pixmap readback is stale

`win64-vulkan` and `win64-d3d12` both fail the two *drawn* checks (`0.0%` throughout; `I takes it away` passes trivially at 0.0%):

- `win64-vulkan` 94 checks, 2 failed; `win64-d3d12` 98 checks, 2 failed — both only the picture's two.
- The picture **is** drawn: a full-window `get_image` reads 1640 pure-white pixels in the checkerboard (rows 656–735), and a screen capture shows a gradient ramp (`247,239,231,…`) in the window's bottom-right — wine's X11 pixmap readback is a **stale gradient**, not the rendered frame.
- `I`, `F1`, `F2` change nothing in the X pixmap (`scene_row()` is byte-identical before and after each), while `has_focus()` is true and `EV key I press/release` is echoed — so the application receives the key and acts, but the X pixmap the harness samples never updates.
- `win64-*` on X11 inherit `_XEnvironment.scene_row()` (reads the X window's pixmap directly via `x11_driver`); the `Wayland`/`Win32VM`/`Win32Native` classes override `scene_row()` to cut the row out of a *screen* picture instead. Under wine-on-X11 the X pixmap is stale; the black-frame check (`content_black_pct` = 0.0) and every other check pass because they don't depend on fresh white pixels.
- Wine version: `wine-10.0 (Ubuntu 10.0~repack-12ubuntu1)`, `WINEPREFIX` unset (the account's own). This is the same wine that already carries the `WINE_HELD` declarations in `tools/drive/environment.py:1697`.

So the picture checks as written cannot pass on wine-on-X11 until either the harness reads the wine window's pixels a different way (a screen capture through the portal, as the Wayland/VM classes do) or wine's pixmap readback is fixed. That is a **harness** decision, not a library one.

### The two image fixes are now measured as not-crashing

The `vk_image_free` (Vulkan) and `d3d_image_free` (Direct3D 12) drains, and the `image_frames_reset()` `ts_unread` fix from `7b4d5b5`/`a6a51a0`, had their pictures destroyed and re-created under a live frame (the `I`-twice path) on Vulkan and D3D12, natively on X11 and under wine — no crash, no leak reported, log otherwise clean. The reasoned-not-measured risk from the previous handoff ("a picture destroyed under a live frame has never been drained") is now retired for these two environments.

## Artifacts

- `tools/drive/stress.py:1629` — `picture()` (committed in `1deaf19`).
- `testbed/overlay.c:411-447` — `picture_draw()` and its checkerboard (committed in `1deaf19`).
- `src/backend/x11/x11.c:2140` — `sync_await_expire()`, the source of the `log is clean` failure.
- `src/backend/x11/x11.c:38` — `SYNC_SHOWN_WAIT 0.034`.
- `tools/drive/environment.py:1491,1513` (`_XEnvironment.scene_row`/`white_share`) and `:2608,2621` (the screen-picture override) — why wine reads stale pixels.
- `build/drive/x11-vulkan-release.failed-2026112100*.log`, `x11-gl-release.log`, `win64-{vulkan,d3d12}-release.failed-20261121-*.log` — the rounds this handoff describes.

## Action Items & Next Steps

1. **`x11-vulkan` `log is clean`** — decide whether to fix `sync_await_expire()` (Vulkan present-sync expiry on a resize frame) or to accept it. It is the only red on an otherwise clean X11 Vulkan round, and it predates the picture work. Whatever is decided, `x11-vulkan` release rounds will stay red until then.
2. **The wine picture check** — decide whether to make `win64-*` on X11 read pixels through a screen capture (the `Wayland`/`Win32VM` pattern) so the picture check can run there, or to skip the picture check on wine with a declared reason. The library draws the picture correctly under wine; only the harness's readback is stale.
3. **`kms-vulkan-bare`** — run on a bare machine (`bare_run.py --kms`, or a `kms-vulkan-bare@<machine>` round) for `7b4d5b5`'s LED bit test at open and `GETCRTC`-per-motion halves. No KMS on this desktop.
4. Then update `TODO.md` § *An overlay image* with the X11 results and the two findings, and remove what is now measured.

## Other Notes

- The tree is clean at `1deaf19`; nothing to commit from this session. The formatter churn that was in the tree was restored, not committed — do not re-run `clang-format -i` or `ruff format` over the tree (CLAUDE.md: "not as something to run over the tree"; the churn is ~3,900 lines of deliberate alignment).
- `./build.sh linux` was run and passed (layering, key/event tables, API doc all clean) — the release tree is current at `1deaf19`.
- The harness drives input through the RemoteDesktop portal (`environment.py`); do not use XTEST on this desktop (CLAUDE.md). The wine rounds share one wine server; `wine_clear()` waits for the previous one to exit.
- The Pi (`pi4`, `linux-arm64`) and the laptops' bare KMS rounds are the natural homes for the owed `kms-vulkan-bare` run; `bare-hosts` names them.