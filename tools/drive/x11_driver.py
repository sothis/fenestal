# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""What an X server can be *asked* about a window, and nothing that drives one.

The same code serves the Win32 target: a wine window *is* an X11 window on
XWayland, so environment.py's Win32 only names a wine-hosted process.

There is no input here and there must not be. XWayland runs with
-enable-ei-portal, so a process's first XTEST fake_input raises a
RemoteDesktop consent dialog, and until it is answered no X client is
delivered any input at all. All three backends are driven through the portal
session wayland_driver.py holds a token for; see environment.py's _Portal.

What this cannot say is what the *compositor* made of a frame: `import`
captures what the X server holds, which is the client's own output. See
tools/drive/README.md *Driving a window* for the other
half.
"""

import subprocess
import time

from Xlib import X, display, protocol

import testbed

d = display.Display()
root = d.screen().root


# ---- finding the window ---------------------------------------------------

def find_window(title_substr="Fenestal", timeout=20.0, pid=None, index=0):
    """One toplevel, by title, and pinned to one process by _NET_WM_PID.

    Pass the pid: any other Fenestal window on the display carries a
    matching title, and a round that drives one of those reports a full set
    of plausible failures with nothing behind them.

    `index` picks between several when the process has more than one window
    that matches -- two windows on one host, a `--second-window` beside the
    first. An empty `title_substr` matches every window of the process, so
    index=0, 1, ... walk them and find_windows() counts them; the order is
    the tree's, stable for a round's length but not across a restack. The
    default, the first, is what a single-window round sees."""
    end = time.time() + timeout
    while time.time() < end:
        wins = _collect(root, title_substr, pid)
        if len(wins) > index:
            return wins[index]
        time.sleep(0.2)             # poll: a tree walk is not free
    raise RuntimeError("no window titled %r for pid %s appeared%s"
                       % (title_substr, pid,
                          "" if index == 0 else " (index %d)" % index))


def find_windows(title_substr="Fenestal", pid=None, timeout=5.0):
    """Every toplevel of the process whose title holds `title_substr`, or, for
    an empty `title_substr`, every window the process has. Waits up to
    `timeout` for at least one, and returns them in the tree's order (see
    find_window()). Empty when none appears."""
    end = time.time() + timeout
    while True:
        wins = _collect(root, title_substr, pid)
        if wins or time.time() >= end:
            return wins
        time.sleep(0.2)


def _net_wm_pid(win):
    try:
        p = win.get_full_property(d.intern_atom("_NET_WM_PID"),
                                  X.AnyPropertyType)
        return p.value[0] if p else None
    except Exception:
        return None


def _collect(win, needle, pid=None):
    """Every window at or below `win` matching, in tree order. A match is not
    searched into: a window's own title is the one that names it, not a
    child's."""
    try:
        name = win.get_wm_name()
    except Exception:
        name = None
    if name and needle in str(name):
        # A window that went between the name and this, as a dying
        # client's do, is not the one.
        try:
            g = win.get_geometry()
        except Exception:
            g = None
        if g and g.width > 100 and g.height > 100:
            if pid is None or _net_wm_pid(win) == pid:
                return [win]
    try:
        kids = win.query_tree().children
    except Exception:
        return []
    out = []
    for k in kids:
        out.extend(_collect(k, needle, pid))
    return out


def _walk(win, needle, pid=None):
    """The first match, for the callers that want one and no more."""
    first = _collect(win, needle, pid)
    return first[0] if first else None


def kill_stale():
    """testbed.kill_stale(), and then the wait for the *windows* to go.

    X reaps a dead client's windows a moment after it exits, and in that gap
    find_window() returns one that every later call fails on with
    BadDrawable. Nothing killed means nothing to wait for."""
    if not testbed.kill_stale():
        return
    for _ in range(20):
        if _walk(root, "Fenestal") is None:
            return
        time.sleep(0.3)             # poll, as above


def geom(win):
    """Absolute position and size on the root."""
    g = win.get_geometry()
    t = win.translate_coords(root, 0, 0)
    return (-t.x, -t.y, g.width, g.height)


def scene_row(win, top=40):
    """One row across the middle of the window's content, every pixel, as
    bytes; None where it cannot be read. Two of them some frames apart differ
    while the scene moves -- its sweep crosses every row -- and are the same
    where the window shows a still frame. For any window, not just the one a
    round drives: two windows of one process can be read apart."""
    try:
        g = win.get_geometry()
    except Exception:
        return None
    w, h = g.width, g.height
    if w < 8 or h < top + 8:
        return None
    try:
        img = win.get_image(0, top + (h - top) // 2, w, 1, X.ZPixmap,
                            0xffffffff)
    except Exception:
        return None
    data = img.data
    return data.encode("latin-1") if isinstance(data, str) else data


def focus(win):
    """Ask the window manager to activate it, and do nothing behind its back.

    _NET_ACTIVE_WINDOW and no SetInputFocus beside it. Under a Wayland
    compositor input goes to the window *it* holds focused, so a focus taken
    without it makes the X server say "yours" about a window no key will
    reach. Asked this way, the X server's answer only turns true because the
    manager made it so."""
    atom = d.intern_atom("_NET_ACTIVE_WINDOW")
    ev = protocol.event.ClientMessage(
        window=win, client_type=atom,
        data=(32, [2, X.CurrentTime, 0, 0, 0]))   # source 2 == a pager
    root.send_event(ev, event_mask=X.SubstructureRedirectMask |
                                   X.SubstructureNotifyMask)
    d.sync()


def is_focused(win):
    """Whether keys go to this window, by both accounts that can be asked:
    the X server's input focus and the manager's _NET_ACTIVE_WINDOW. They
    are written by different parties at different moments, and it is the
    second that says where the compositor sends a key."""
    return focused_id() == win.id and active_id() == win.id


def focus_settled(win, timeout=4.0):
    """Activate, and wait until the manager agrees.

    One _NET_ACTIVE_WINDOW is a request, and mutter does not always grant
    the first -- least of all right after another window has been driven. So
    it is asked again while the answer is no, and the answer ends the wait."""
    end = time.time() + timeout
    asked = 0.0
    while time.time() < end:
        if is_focused(win):
            return True
        if time.time() - asked > 0.5:
            focus(win)
            asked = time.time()
        time.sleep(0.03)            # poll
    return is_focused(win)


def focused_id():
    try:
        return d.get_input_focus().focus.id
    except Exception:
        return None


def active_id():
    try:
        p = root.get_full_property(d.intern_atom("_NET_ACTIVE_WINDOW"),
                                   X.AnyPropertyType)
        return p.value[0] if p else None
    except Exception:
        return None


def is_hidden(win):
    """Whether the manager holds the window minimized (_NET_WM_STATE_HIDDEN).
    The X map state does not say: under a compositing manager an iconified
    window stays mapped."""
    try:
        p = win.get_full_property(d.intern_atom("_NET_WM_STATE"),
                                  X.AnyPropertyType)
        return bool(p) and d.intern_atom("_NET_WM_STATE_HIDDEN") in p.value
    except Exception:
        return False


def work_area():
    """_NET_WORKAREA for the current desktop, in pixels, or None.

    What the X11 backend reports through fnl_output_size_pt(), and so what a
    maximized window should come out as."""
    try:
        cur = root.get_full_property(d.intern_atom("_NET_CURRENT_DESKTOP"),
                                     X.AnyPropertyType)
        i = cur.value[0] if cur else 0
        wa = root.get_full_property(d.intern_atom("_NET_WORKAREA"),
                                    X.AnyPropertyType)
        if not wa or len(wa.value) < (i + 1) * 4:
            return None
        v = wa.value[i * 4:i * 4 + 4]
        return (v[0], v[1], v[2], v[3])
    except Exception:
        return None


# ---- capture --------------------------------------------------------------

def capture(win, path):
    wid = hex(win.id)
    subprocess.run(["import", "-window", wid, path], check=True,
                   stderr=subprocess.DEVNULL, timeout=30)
    return path


def pointer_pos():
    p = root.query_pointer()
    return (p.root_x, p.root_y)
