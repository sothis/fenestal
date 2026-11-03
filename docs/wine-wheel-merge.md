# Draft: wine's server loses a wheel step it merges into a held message

A draft for wine's Bugzilla (product Wine, component server, or else
winewayland.drv; version 11.20), not yet sent. Seen in the stress round
under wine 11.20's Wayland driver on weston 15.0.1 and cage 0.3.1 on the
thinkpad (Core i3 M 370), read in the 11.20 source, reproduced alone by
`tools/probe/wheelcount.c` with `tools/probe/wheelpairs.py`, and fixed by
the patch below in a wineserver built from the 11.20 tarball. Master was
unchanged on 2026-10-21, and the tracker had no report; read again on
2026-10-31, `server/queue.c` in 11.21 (2026-10-30) and master is 11.20's,
the patch applies with no offset, and Bugzilla and the merge requests
still have nothing. The library
steps round it under wine by reading the wheel from raw input (DESIGN
*Wine on the bare machine*). To
attach: the two probe files, and the patch below saved as a file.

## Summary

server: WM_MOUSEWHEEL delta lost when a wheel event is merged into a
message already returned to the application

## Description

When a second wheel event is queued while the application is still
processing the first WM_MOUSEWHEEL, between the server returning the
message and the client sending accept_hardware_message, the second
event's delta is lost. The server merges the second event into the
message the client already holds, then frees that message when the client
accepts it.

With winewayland.drv this happens in normal use, because input is sent
from a thread of its own (wayland_read_events_thread ->
NtUserSendHardwareInput), so it can arrive while the window's thread is
in that gap. In a busy application, a Vulkan program drawing on llvmpipe,
about one pair of wheel steps in forty arrived as a single notch, on
weston 15.0.1 and on cage 0.3.1. WAYLAND_DEBUG=1 showed that the
compositor sent both steps, as two wl_pointer.axis_discrete(0, -1)
events in separate frames. A thread-local WH_MOUSE hook that takes a
millisecond makes the loss happen every time, which is what the
reproducer below uses.

## Steps to reproduce

wheelcount.c (attached) is a maximized window that prints every
WM_MOUSEWHEEL and the running total. Its first argument is how long, in
ms, an optional thread-local WH_MOUSE hook sleeps on WM_MOUSEWHEEL.
Build:

    x86_64-w64-mingw32-gcc -O2 -mconsole -o wheelcount.exe wheelcount.c

wheelpairs.py (attached, run as root) creates a uinput mouse and sends
pairs of REL_WHEEL -1 events, 1 ms apart within a pair and 250 ms
between pairs.

1. Start a Wayland compositor (weston was used here). Run with no
   DISPLAY, so that wine uses winewayland.drv:

       wine wheelcount.exe 1

2. With the pointer over the window, as root:

       python3 wheelpairs.py 100

3. Read the last line wheelcount prints.

Expected: total -24000 in 200 messages, 200 steps of -120.
Actual: total -12000 in 100 messages: one step of every pair is lost.

## Results

Wine 11.20, weston 15.0.1, Arch Linux, kernel 7.2.11, Intel Core i3
M 370. The wineserver was built from the wine-11.20 tarball (signature
checked), as released and with the attached patch. Each server was
started with -p before the application, and the process list was checked
during each run.

    server      WH_MOUSE hook   pairs   messages   total     steps lost
    11.20       none            200     400        -48000    0
    patched     none            200     400        -48000    0
    11.20       1 ms            100     100        -12000    100
    patched     1 ms            100     200        -24000    0
    11.20       5 ms            100     100        -12000    100
    patched     5 ms            100     200        -24000    0

The system wineserver of the Arch wine 11.20 package also loses one step
per pair with the hook. The idle test program, with no hook, did not lose
a step in 400.

## Analysis

server/queue.c and dlls/win32u/message.c at wine-11.20; the code is
unchanged on master.

1. get_hardware_message() returns the WM_MOUSEWHEEL to the client, sets
   msg->unique_id, and for a mouse message clears PM_REMOVE, so the
   message stays in input->msg_list until the client sends
   accept_hardware_message.
2. On the client side, process_mouse_message() makes several server
   round trips before accepting the message: NtUserGetGUIThreadInfo,
   window_from_point, and the WH_MOUSE hook.
3. If a second wheel event is queued during that time,
   queue_hardware_message() calls merge_message() ->
   merge_mousewheel() -> find_mouse_message(). find_mouse_message()
   returns the last queued mouse message without checking whether it has
   already been returned to a client, so the second delta is added to the
   wparam of the message being processed.
4. The client then sends accept_hardware_message(hw_id), and
   release_hardware_message() frees that message, including the second
   step's delta, which the client never saw.

winex11.drv processes X events on the window's own thread between
messages, which is probably why it is not affected in practice. Any
other source of input on another thread would be.

From reading the same code, merge_mousemove() looks to have the same
problem: a WM_MOUSEMOVE merged into a message that is already being
processed changes a message the client has read and is about to free, so
the newer pointer position would be dropped. I have not observed this.

## Fix (attached, tested as above)

Do not merge into a message that has already been returned to a client:

    --- a/server/queue.c
    +++ b/server/queue.c
    @@ -821,6 +821,7 @@
         }
         if (!ptr) return NULL;
         if (prev->result) return NULL;
    +    if (prev->unique_id) return NULL;  /* already returned to the client */
         if (prev->win && msg->win && prev->win != msg->win) return NULL;
         if (prev->msg != msg->msg) return NULL;
         if (prev->type != msg->type) return NULL;

unique_id is 0 on a newly queued message and is set only when the message
is returned to a client, so messages the client has not yet seen are
still merged. A message returned with PM_NOREMOVE also keeps its
unique_id, so in that case a new message is queued instead of the one the
application has already looked at being changed.
