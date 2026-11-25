/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the Win32 backend that no other file may see: the window,
 * the module, and what the message loop has to remember between messages.
 */

#ifndef FNL_WIN32_STATE_H
#define FNL_WIN32_STATE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* The wakeup: a posted private message, the one thing that reliably wakes a
 * loop blocked in GetMessage. An event object would mean
 * MsgWaitForMultipleObjects and a handle set for a queue of one. */
#define FNL_WM_WAKEUP (WM_APP + 0)
/* Sent, not posted, by the render thread: make the child window frames are
 * presented into anew (win32_present_window_renew()), and paint the gap's
 * picture into the one they go into before its swapchain goes
 * (win32_present_window_hold()). wParam is the ask's ticket (ask_send()). */
#define FNL_WM_PRESENT_RENEW (WM_APP + 1)
#define FNL_WM_PRESENT_HOLD (WM_APP + 2)
/* Posted by the raw input thread: a captured pointer's reports are waiting
 * (rawin_forwarded()). */
#define FNL_WM_RAW (WM_APP + 3)

struct RawIn;

/* What the host owns, beside the windows: the process module, the window
 * class every window is made from, the presentation child's class, the
 * raw-input thread that serves the process, and the process's raw mouse
 * registration. The connection a window server would own is here the
 * process's message queue, which is the pump's thread's already; there is
 * nothing to open. The host's state is in Host::backend_host,
 * and the window's own in App::backend_state. See DESIGN *A second window*. */
struct BackendHostState {
	HINSTANCE instance;
	/* FNL_WNDCLASS, and whether this host may unregister it: a class a
	 * previous Fenestal in the process left behind exists and is good to
	 * make a window from, but is not this host's to remove. */
	ATOM wndclass;
	bool wndclass_ours;
	/* FNL_PRESENT_CLASS, and whether it is this host's to remove. The atom
	 * is kept for the case where the class is someone else's: registering
	 * a name that exists returns 0, so the atom is looked up instead. */
	ATOM present_class;
	bool present_class_ours;
	struct RawIn *rawin; /* the process's raw-input thread, or NULL */
	/* How many windows have confined and hidden the pointer. ClipCursor and
	 * ShowCursor are the process's -- one pointer, one clip, one counter --
	 * so the first window to capture applies them, the last to release gives
	 * them back, and a window releasing while another still holds it leaves
	 * that one's confinement on the pointer rather than taking it. [main] */
	int cursor_confined;

	/* The process's raw mouse registration, one a process has: the last
	 * window to want it owns it, since RegisterRawInputDevices is
	 * last-writer-wins, and a per-window flag cannot say whose the process
	 * has now. raw_target is the window it is pointed at -- the capturing
	 * one, else the one in front, whose wheel is read from raw input under
	 * wine -- and raw_saved the program's own registration, as raw_point()
	 * last found the program holding it, put back by raw_give() when no
	 * window wants it. [main] */
	bool raw_owned;
	HWND raw_target;
	RAWINPUTDEVICE raw_saved;
	bool raw_saved_valid;
};

struct BackendState {
	HINSTANCE instance;
	HWND hwnd;
	/* The child window frames are presented into once a move to another
	 * GPU has made one, NULL until then, and its class, registered at the
	 * first. Written by the window's thread inside the message the render
	 * thread sends for it, which returns only once it is written; read by
	 * the render thread's bridges after that and by the window's thread. */
	HWND present;
	ATOM present_class;
	/* Whether the child has had a present since it was made, and
	 * App::present_calls when it was made: until the first, present_proc
	 * erases it with the frame the move read back, or black, which
	 * win32_flush() ends. Written by both threads and read by the
	 * window's, so atomic. */
	FnlAtomicBool present_presented;
	uint64_t present_calls_at_renew;
	/* The last thing the render thread asked of the window's thread with a
	 * bound (ask_send()): its ticket and where it is, ASK_*. [App::lock] */
	uint32_t ask_ticket;
	int ask_state;

	/* Set while inside DestroyWindow, so the WM_DESTROY it provokes is not
	 * read as the user closing the window. */
	bool closing;

	/* The DPI the window is on, as Windows reports it: 96 is 100%. Converted
	 * to the library's 120ths by resolve_scale120(). */
	UINT dpi;

	/* The monitor the window is on, so that publishing the output costs one
	 * MonitorFromWindow while it has not changed. */
	HMONITOR monitor;

	/* Whether a WM_MOUSELEAVE has been asked for: Win32 sends one only on
	 * request, per entry. */
	bool tracking_leave;

	/* Pointer capture, whose flag is App::pointer_captured. The cursor is
	 * clipped and hidden, and relative motion comes from raw input:
	 * differences of position stop at the clip edge. The registration
	 * itself is the host's (BackendHostState::raw_owned). */
	/* Under wine the wheel is read from raw input as well, which wine's
	 * server never merges: a WM_MOUSEWHEEL it merges a step into after
	 * handing the message out loses that step (DESIGN *Wine on the bare
	 * machine*). wheel_raw is raw input asked for at open for that;
	 * wheel_raw_seen that a raw wheel step has arrived, after which a
	 * WM_MOUSEWHEEL wheel_raw_here() accepts is that step's echo. */
	bool wheel_raw;
	bool wheel_raw_seen;
	/* The raw input thread (win32_rawin.c), on Windows and not under wine,
	 * or NULL: when each input reached the process, for
	 * fnl_input_latency_ms(), and while it holds the process's raw mouse, a
	 * capture's motion forwarded from it instead of read from a window. */
	struct RawIn *rawin;
	/* Whether this backend has taken one off ShowCursor's process-wide
	 * display count. */
	bool cursor_hidden;

	/* The system's cursors by FnlCursor, loaded on first use. */
	HCURSOR cursors[FNL_CURSOR_WAIT + 1];

	/* The pending grow-back-to-the-floor timer, or 0; see arm_size_floor(). */
	UINT_PTR floor_timer;
	/* Set once a grow-back was refused, until the window is next seen at or
	 * above the floor. */
	bool floor_refused;

	/* Where the window was before going fullscreen, which Windows does not
	 * remember. A rectangle and a flag rather than a WINDOWPLACEMENT, which
	 * describes a window that has a frame; see win32_set_fullscreen(). */
	RECT saved_rect;
	bool saved_maximized;
	DWORD saved_style;

	/* The monitor's rectangle, held while the window is fullscreen; see
	 * WM_WINDOWPOSCHANGING. */
	RECT fs_rect;

	/* One size at a time; see size_await(). The serial of the size last
	 * published, the one the frame's thread last presented an answer to,
	 * and the event it sets when it has. size_acked, size_ack_owed and
	 * size_ack_calls -- App::present_calls at the ack -- are that thread's
	 * and the renderer's waiter's, which releases the size once its frame
	 * is shown (win32_ack_release()), so they are taken under ack_lock. */
	HANDLE size_event;
	uint32_t size_serial;
	FnlAtomicU32 size_shown;
	FnlAtomicBool size_live;
	bool size_awaiting;
	FnlMutex ack_lock;
	uint32_t size_acked;
	bool size_ack_owed;
	uint64_t size_ack_calls;

	/* The first half of a surrogate pair, held between the two WM_CHAR that
	 * carry one character outside the basic plane. Zero when none is. */
	uint32_t char_high;
};

/* The window the renderer presents into: the child a move made, or the
 * window itself. */
static inline HWND win32_present_hwnd(const struct BackendState *p)
{
	return p->present ? p->present : p->hwnd;
}

#endif /* FNL_WIN32_STATE_H */
