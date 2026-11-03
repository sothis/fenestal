# Draft: weston applies a held content update with xdg_surface state sent after it

A draft for weston's issue tracker (libweston-desktop, xdg-shell), not yet
sent. Seen with weston 15.0.1 (Arch 15.0.1-3), Mesa 26.2.4 and Linux
7.2.10 on a ThinkPad Edge (Core i3 M 370, Ironlake graphics, lavapipe),
and read in the 15.0.1 source as Arch's debuginfod serves it. The client is
this library under ThreadSanitizer, which is what makes it slow enough to
fall into the window; the workaround it now carries is commit `7ea43dd`
(DESIGN *Wayland*).

## Summary

`xdg_surface.set_window_geometry` and `xdg_surface.ack_configure` are
double-buffered state that the next `wl_surface.commit` applies. weston 15
can hold a commit back -- here a content update waiting on a
`wp_fifo_v1` barrier -- and apply it at a later output latch. When it does,
libweston-desktop applies the *current* pending xdg_surface state with it,
including requests the client sent after that commit, and runs its
maximized check against whatever subset of them has arrived. A client that
restores a maximized window and sends the restored geometry ahead of the
ack is disconnected with

    xdg_surface geometry (1066 x 548) does not match the configured
    maximized state (1366 x 768)

although its next commit -- the one those requests belong to -- would
have carried both.

## What the client sent

From `WAYLAND_DEBUG=client`, the client's requests and the server's
events in order, Mesa's Vulkan WSI presenting with `wp_fifo_v1` and
`wp_commit_timer_v1`, refresh 60 Hz:

    23.946 <- xdg_toplevel.configure(0, 0, [activated])   restore from maximized
    23.946 <- xdg_surface.configure(1398)
    23.961 -> wl_surface.attach / damage_buffer           previous frame's present
              wp_commit_timer_v1.set_timestamp, wp_fifo_v1.set_barrier
              wp_fifo_v1.wait_barrier
    23.961 -> wl_surface.commit()                         (1) the buffer
    23.961 -> wp_fifo_v1.wait_barrier
    23.961 -> wl_surface.commit()                         (2) empty, held for the barrier
    23.988 -> wp_fifo_v1.destroy, wp_commit_timer_v1.destroy, get_fifo, get_timer
              (the swapchain rebuilt at the restored size)
    23.993 -> xdg_surface.set_window_geometry(6, 6, 1066, 548)
    23.993 -> xdg_surface.ack_configure(1398)
    23.993 -> wl_surface.frame
    23.998 <- wl_display.error(xdg_wm_base, 4, "xdg_surface geometry (1066 x 548)
              does not match the configured maximized state (1366 x 768)")

No commit follows the geometry before the error. The client also flushed
between `set_window_geometry` and `ack_configure`, so the two can reach
the server in separate reads.

## What weston does with it

In `libweston/desktop/xdg-shell.c` (15.0.1):

- `weston_desktop_xdg_surface_protocol_set_window_geometry` stores the
  geometry in `surface->next_geometry` and sets `has_next_geometry`;
  nothing ties it to a commit.
- `weston_desktop_xdg_surface_protocol_ack_configure` calls
  `weston_desktop_xdg_toplevel_ack_configure`, which writes the acked
  configure's state and size into `toplevel->next` the moment the request
  is dispatched.
- `weston_desktop_xdg_surface_committed` -- the surface's `committed` hook,
  run when a content update is *applied* -- moves `next_geometry` into the
  surface and calls `weston_desktop_xdg_toplevel_committed`, which posts
  `XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE` when `toplevel->next.state` is
  maximized and the geometry differs from `toplevel->next.size`.

Held content updates are applied at an output latch
(`weston_compositor_apply_transactions` in `weston_output_latch`,
`libweston/compositor.c`). So commit (2), sent at 23.961, was applied at
the latch near 23.995 -- after `set_window_geometry` had been dispatched,
and before `ack_configure` -- with the restored geometry against the
maximized state still acked.

## What is asked

That the xdg_surface state double-buffered on a commit be latched with
that commit's content update, as `wl_surface` state is: a held update
applied later carries the geometry and the acked configure that were
pending when it was committed, and nothing sent after it. As it is, which
geometry and which state an update is checked against depends on where
the server's reads happen to split the client's requests.

## Workaround in a client

Order the two requests so that either alone leaves a valid pair: the ack
first when the new state is floating, the geometry first when entering
maximized or fullscreen from floating, the ack first from maximized into
fullscreen, and flush only after both. Eight runs of the round that failed
four times in seven, none failed.

## Reproducer

`tools/probe/heldgeom.c`, a wl_shm client with no GPU and nothing of this
library in it (build lines in its header). It maximizes, and answers the
restore's configure with

    wl_surface.attach / damage_buffer, wp_fifo_v1.set_barrier, commit   (1)
    wp_fifo_v1.wait_barrier, commit                                    (2) held
    xdg_surface.set_window_geometry(0, 0, 640, 480), flush
    -- a pause, HELDGEOM_GAP_MS, 100 by default --
    xdg_surface.ack_configure(restore), attach, commit                 (4)

and says whether it was disconnected; `heldgeom nowait` sends commit (2)
without `wait_barrier`, the requests otherwise the same. On weston 15.0.1,
DRM backend, 60 Hz, the output idle before the restore, ten runs each:

| machine | gap | errors |
|---|---|---|
| ThinkPad Edge (Ironlake) | 60, 70, 100 ms | 10 each |
| | 0, 5, 20, 30, 40, 50 ms | none |
| | `nowait`, 100 ms | none |
| Schenker (Kaby Lake) | 100 ms | 10 |
| | 20 ms | 3 |
| | `nowait`, 100 ms | none |

The error, each time:

    xdg_surface geometry (640 x 480) does not match the configured
    maximized state (1366 x 768)

Below the gap at which the held commit is applied, the ack is in first and
both go in with commit (2), which passes the check but is still state sent
after that commit; the client in the summary, rendering every frame, hit
the window at about 32 ms. A run without the error ends on the frame
callback of commit (4), so after weston has applied commit (2) and
checked it.

## weston's main branch and the tracker

Read again 2026-10-31, main at `b5cd1011` (2026-10-30): unchanged, the
last commit to `xdg-shell.c` still `f41430a4` (2026-05-13), and
`xdg-shell.c` in weston 16.0.2 (2026-10-26, the latest release) the same
bytes as main's. No issue or merge request names it; !2059 is still an
open draft. Seen and measured on 15.0.1 alone, so a `heldgeom` run on
16.0.2 is worth having before it is sent, the version a reporter is asked
about.

Read 2026-10-17, main at `ffb9751` (2026-10-14):

- `libweston/desktop/xdg-shell.c` differs from 15.0.1 here only in a
  `client_resource` NULL check around the same `wl_resource_post_error`:
  `set_window_geometry` still stores into `next_geometry` unlatched,
  `ack_configure` still writes `toplevel->next` when dispatched, and the
  maximized and fullscreen checks are unchanged.
- Held updates are applied the same way: `weston_output_latch` clears the
  output's FIFO barriers and calls `weston_compositor_apply_transactions`
  (`libweston/compositor.c`), which runs the surface's `committed` hook
  (`libweston/surface-state.c`).
- Open draft !2059 (Derek Foreman, "tree-wide: Rename committed
  handlers") renames those hooks to `updated`, because since fifo and
  commit-timing "a content update is committed, and later on applied";
  in `xdg-shell.c` it changes names only. It is the nearest thing to this
  on the tracker and the place to point at.
- No issue or merge request on `wayland/weston` names it: searched
  through the GitLab API for set_window_geometry, invalid_surface_state,
  "does not match the configured maximized", fifo, geometry, commit
  timing, transaction, xdg_surface and maximized, and every open merge
  request touching `libweston/desktop/`. Unauthenticated, so confidential
  issues were not seen.

## Owed before this is sent

- Sending it, from the account that files the reports.
