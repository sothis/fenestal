/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the X11 backend that no other file may see: its state, and
 * what its files ask of one another. The backend's own files include it, and
 * the X11-to-Vulkan bridge, which reads the connection and the window.
 *
 * xcb and not Xlib, and core xcb only: DESIGN *X11: xcb, not Xlib* has the
 * argument. What that costs is written down in x11_ext.c, where it is paid.
 * libxcb itself is opened at run time, and reached through xcb_dyn.h.
 */

#ifndef FNL_X11_STATE_H
#define FNL_X11_STATE_H

#include "backend/x11/xcb_dyn.h"

#include "backend/clip_linux.h"
#include "backend/keymap.h"

/* An event's type, and the bit above it that says another client sent it
 * rather than the server. */
#define X11_EVENT_SENT      0x80
#define X11_EVENT_TYPE_MASK 0x7f

/* The atoms this backend interns: one list, each row the enum's suffix and
 * the name the server knows it by, expanded into the enum here and into the
 * names table in x11.c. Two lists bound by a count would let a swapped pair
 * compile and intern the wrong atom under a name. The names are not derivable
 * from the suffixes, so both are written.
 *
 * Every request is sent before any reply is collected, so the list costs one
 * round trip rather than one apiece.
 *
 * Only the ones the protocol does not predefine are here. WM_NAME, WM_CLASS,
 * WM_NORMAL_HINTS, WM_HINTS, WM_CLIENT_MACHINE, RESOURCE_MANAGER, STRING,
 * CARDINAL and ATOM all have fixed values in <xcb/xproto.h> and are used from
 * there. */
#define X11_ATOMS(X)                                                    \
	X(WM_PROTOCOLS,               "WM_PROTOCOLS")                       \
	X(WM_DELETE_WINDOW,           "WM_DELETE_WINDOW")                   \
	X(WM_CHANGE_STATE,            "WM_CHANGE_STATE")                    \
	X(NET_WM_PING,                "_NET_WM_PING")                       \
	X(NET_WM_SYNC_REQUEST,        "_NET_WM_SYNC_REQUEST")               \
	X(NET_WM_SYNC_REQUEST_COUNTER, "_NET_WM_SYNC_REQUEST_COUNTER")      \
	X(NET_WM_PID,                 "_NET_WM_PID")                        \
	X(NET_WM_NAME,                "_NET_WM_NAME")                       \
	X(UTF8_STRING,                "UTF8_STRING")                        \
	X(NET_WM_STATE,               "_NET_WM_STATE")                      \
	X(NET_WM_STATE_FULLSCREEN,    "_NET_WM_STATE_FULLSCREEN")           \
	X(NET_WM_STATE_MAXIMIZED_VERT, "_NET_WM_STATE_MAXIMIZED_VERT")      \
	X(NET_WM_STATE_MAXIMIZED_HORZ, "_NET_WM_STATE_MAXIMIZED_HORZ")      \
	X(NET_WM_STATE_HIDDEN,        "_NET_WM_STATE_HIDDEN")               \
	X(NET_WM_MOVERESIZE,          "_NET_WM_MOVERESIZE")                 \
	X(NET_MOVERESIZE_WINDOW,      "_NET_MOVERESIZE_WINDOW")             \
	X(NET_WM_WINDOW_TYPE,         "_NET_WM_WINDOW_TYPE")                \
	X(NET_WM_WINDOW_TYPE_NORMAL,  "_NET_WM_WINDOW_TYPE_NORMAL")         \
	X(MOTIF_WM_HINTS,             "_MOTIF_WM_HINTS")                    \
	X(NET_WORKAREA,               "_NET_WORKAREA")                      \
	X(NET_CURRENT_DESKTOP,        "_NET_CURRENT_DESKTOP")               \
	X(NET_SUPPORTING_WM_CHECK,    "_NET_SUPPORTING_WM_CHECK")           \
	X(CLIPBOARD,                  "CLIPBOARD")                          \
	X(TARGETS,                    "TARGETS")                            \
	X(INCR,                       "INCR")                               \
	X(TEXT_PLAIN_UTF8,            "text/plain;charset=utf-8")           \
	X(FNL_PASTE,                  "FNL_PASTE")                          \
	X(FNL_PASTE2,                 "FNL_PASTE2")                         \
	X(FNL_STAMP,                  "FNL_STAMP")

enum {
#define X11_ATOM_ENUM(suffix, name) FNL_ATOM_##suffix,
	X11_ATOMS(X11_ATOM_ENUM)
#undef X11_ATOM_ENUM
	FNL_ATOM_COUNT
};

/* Every cursor the window ever shows: the arrow, the eight resize edges and
 * the eight more fnl_set_cursor() offers. */
typedef enum {
	X11_CURSOR_DEFAULT,
	X11_CURSOR_N, X11_CURSOR_S, X11_CURSOR_W, X11_CURSOR_E,
	X11_CURSOR_NW, X11_CURSOR_NE, X11_CURSOR_SW, X11_CURSOR_SE,
	X11_CURSOR_TEXT, X11_CURSOR_POINTER, X11_CURSOR_CROSSHAIR,
	X11_CURSOR_MOVE, X11_CURSOR_EW, X11_CURSOR_NS,
	X11_CURSOR_NOT_ALLOWED, X11_CURSOR_WAIT,
	X11_CURSOR_COUNT,
	/* Not a cursor: asks for none at all, which is what a captured pointer
	 * wants while the caller draws its own. */
	X11_CURSOR_NONE = -1,
	/* Not a cursor either: what the window wears is not known, so whatever
	 * is asked for next is sent. "None" must not compare equal to it. */
	X11_CURSOR_UNKNOWN = -2
} X11Cursor;

/* One RandR monitor. The list is this backend's: the core is told the room
 * the window's leaves and its name, through app_publish_output(), and the
 * names, through app_publish_outputs(). */
typedef struct {
	int  x, y, w, h;        /* on the root, in pixels                        */
	char name[64];          /* the connector, e.g. HDMI-1                    */
	unsigned int refresh_mhz; /* its CRTC's mode; 0 until read, or unknown  */
	bool primary;           /* RandR's primary, where one is set             */
} X11Monitor;

/* Frames presented and not yet answered by the Present extension, at most:
 * a server that answers none -- a driver that presents some other way --
 * lets the oldest go. */
#define X11_PRESENT_Q 16

struct BackendState {
	/* Two connections to the one server, and which is which is load-bearing.
	 * `conn` is the window's: the main thread creates the window on it and is
	 * the only thread that ever reads it. `gfx` is handed to
	 * VK_KHR_xcb_surface and belongs to the presentation engine, which blocks
	 * in it once a frame from the render thread. Shared, that blocking read
	 * would move the pump's events out of the socket into xcb's decoded
	 * queue, and the pump's epoll would sleep on input it had already been
	 * handed; the split is what lets that wait be indefinite.
	 *
	 * The rule is about readers, and a write may cross with one duty: libxcb
	 * polls for input while it waits to send, moving what has arrived into
	 * the decoded queue, so whoever writes on `conn` off the pump -- the
	 * sync counter, from x11_flush() on the render thread, x11_ack_release()
	 * on the renderer's waiter, x11_frame_cancel() -- wakes the pump
	 * afterwards (loop_wake()), and the pump's own flush comes ahead of its
	 * queued pass, never between that and the wait. Resource ids are
	 * the server's rather than a connection's, so `gfx` presents into a
	 * `window` it did not create. DESIGN *Frame pacing and the off-screen
	 * skip* has the measurement. */
	xcb_connection_t   *conn;
	xcb_connection_t   *gfx;
	const xcb_screen_t *screen;
	int                 screen_num;
	xcb_window_t        root;
	xcb_window_t        window;

	xcb_atom_t          atom[FNL_ATOM_COUNT];
	/* _NET_WM_CM_S<screen>, the selection a compositing manager owns; its
	 * name has the screen in it, so it is interned beside the list rather
	 * than in it. */
	xcb_atom_t          atom_cm;

	/* The cursor font and what has been cut from it so far: each is a
	 * server-side object that costs a request, so none is made unasked. */
	xcb_font_t          cursor_font;
	xcb_cursor_t        cursors[X11_CURSOR_COUNT];
	xcb_cursor_t        blank_cursor;
	/* What is on the window now, so that a motion event that does not change
	 * the answer costs no request. */
	X11Cursor           cursor_applied;

	/* ---- extensions ----------------------------------------------------
	 * Either being absent is a degradation rather than a failure, so both
	 * are flags and every use is guarded. See x11_ext.c. */
	bool                have_randr;
	uint8_t             randr_first_event;
	bool                detectable_repeat;
	/* XKB's own events are selected and this is where they are numbered
	 * from; false and unused where the extension would not start. */
	bool                have_xkb_events;
	uint8_t             xkb_first_event;
	/* Which FnlKey each keycode is, read off XKB's key names (<AC01>, <UP>)
	 * where the server gives them, so a keymap that is not evdev's still
	 * names keys by position. Without them, evdev plus 8; see
	 * x11_keys.c. Stale like keymap_stale, and read by the next key. */
	uint8_t             key_of[256];
	bool                key_names;
	bool                key_names_stale;

	/* An EWMH window manager is running (_NET_SUPPORTING_WM_CHECK is set).
	 * Without one this backend moves and sizes the window itself. */
	bool                ewmh;
	/* A compositing manager owned _NET_WM_CM_S<screen> at open: what is
	 * presented reaches the screen through it, at its refresh, and does not
	 * tear. Read once -- a compositor that comes or goes afterwards is not
	 * followed -- and read by the GL bridge (src/renderer/gl/egl_x11.c). */
	bool                composited;

	/* _NET_WM_SYNC_REQUEST (x11_ext.c). The counter, or 0 without the SYNC
	 * extension. The value the manager last asked for: its low word, and
	 * that one has come and not yet been set, on the main thread; its high
	 * word in an atomic, since the render thread reads it in
	 * ack_configure(), and the low word last set, which the render thread
	 * writes. And the render thread's own: the value to set and that it is
	 * owed, set by ack_configure() and spent by the flush after a present --
	 * `presents` is App::presents at the ack, so the flush before the
	 * present, which app_surface_sync() can make, does not spend it; nor
	 * does a present that failed, after which the loop rebuilds and presents
	 * again, where a counter set would show the manager the old frame. */
	uint32_t            sync_counter;
	uint32_t            sync_lo;
	bool                sync_asked;
	/* Whether the ConfigureNotify after the request has come, and until
	 * when it is waited for before the value goes out with the size the
	 * window has (x11_publish_size()). Main thread. */
	bool                sync_configured;
	double              sync_deadline;
	FnlAtomicU32        sync_hi;
	FnlAtomicU32        sync_set_lo;
	/* The three below are the renderer's waiter's too, which releases the
	 * value once its frame is shown (x11_ack_release()), so they are taken
	 * under sync_lock. */
	FnlMutex            sync_lock;
	uint64_t            sync_release;
	uint64_t            sync_presents;
	bool                sync_due;
	/* The value owed, kept until Present's completion of the frame that
	 * answers it (x11_flush()), where the renderer does not wait for that
	 * frame itself: sync_await_index is the frame's, and the value goes
	 * anyway at sync_await_until. Under sync_lock; sync_await_said is the
	 * main thread's, whether the one line about a wait run out is said. */
	bool                sync_await;
	uint64_t            sync_await_index;
	double              sync_await_until;
	bool                sync_await_said;

	/* The Present extension's completions, which say when each frame the
	 * graphics API presented reached the screen (fnl_frame_shown(),
	 * x11_ext.c): present_opcode is its major opcode, 0 without it. The
	 * frames presented and not yet answered, oldest first, by index --
	 * pushed by the render thread's frame_request() and taken back by its
	 * frame_cancel(), answered on the main thread -- under present_lock,
	 * with when each was pushed, just ahead of its present; present_pushed
	 * is the render thread's, whether this frame pushed. */
	uint8_t             present_opcode;
	FnlMutex            present_lock;
	uint64_t            present_q[X11_PRESENT_Q];
	double              present_at[X11_PRESENT_Q];
	/* Each queued frame's number among the presents of its chain
	 * (App::present_chain), counted as the presenter counts its serial:
	 * Mesa's X11 WSI numbers each swapchain's PresentPixmap requests from
	 * 1. A completion is the oldest queued frame of its serial's number,
	 * and every frame queued before that one was never sent -- Mesa drops
	 * the presents still in its queue with the swapchain it retires --
	 * since the server answers a window's presents in order. present_ord
	 * is under present_lock; present_ord_next and present_chain_seen are
	 * the render thread's. */
	uint32_t            present_ord[X11_PRESENT_Q];
	uint32_t            present_ord_next;
	uint32_t            present_chain_seen;
	/* [main] Whether a completion's serial has matched a queued frame's
	 * number: then one that matches none answers a present taken back --
	 * NVIDIA's driver shows a frame whose present it said was out of date
	 * -- and is no frame of the queue's, where before any match it is the
	 * oldest, the presenter counting otherwise. */
	bool                present_by_serial;
	int                 present_head, present_n;
	bool                present_pushed;
	/* present_index is the render thread's, the index this frame pushed;
	 * present_answered is the last index answered plus one, under
	 * present_lock; and present_live says this window's presents are
	 * answered at all: a presenter that is not Present's -- Mesa's
	 * software one by PutImage -- answers none, and a wait for one that
	 * runs out clears it until a completion comes again. */
	uint64_t            present_index;
	uint64_t            present_answered;
	FnlAtomicBool       present_live;
	/* FNL_X11_PRESENT_LOG=1, read at open: a line for each frame pushed,
	 * answered or taken back, each value kept for its frame and each wait
	 * for one run out -- a diagnostic for which completion answers which
	 * frame, the queue being matched in order (x11_present_event()). */
	bool                present_log;
	/* [main] The last completion's stamp and counter (ust in µs, msc in
	 * refreshes), and the refresh period the two give between completions,
	 * 0 until two have: what a stamp from before its present is moved on
	 * by (x11_present_event()). */
	uint64_t            present_ust, present_msc;
	double              present_period;

	/* ---- the window, in pixels on the root -----------------------------
	 * The position is tracked rather than asked for. A reparenting manager
	 * puts the client inside a frame, so a non-synthetic ConfigureNotify is
	 * relative to that frame, and TranslateCoordinates would be a round trip
	 * per step of a drag. The frame is watched instead, and the two added. */
	int                 win_x, win_y;
	int                 win_w, win_h;
	/* The same, as last seen neither fullscreen nor maximized: where the
	 * manager puts the window back, which an output move under EWMH must
	 * place (x11_set_output()) -- win_* is the monitor's then. A rectangle
	 * caught mid-transition is the monitor's, which is no worse. */
	int                 norm_x, norm_y, norm_w, norm_h;
	bool                norm_valid;

	/* One size at a time, with no manager (size_give()): the size this
	 * backend last gave the window, in pixels, and when; and the geometry
	 * asked for since, held until a frame at that size has been presented
	 * or SIZE_HOLD_S has gone by -- the request's mask, and its values.
	 * Main thread. shown_w and shown_h are the extent of the frame last
	 * presented, from the render thread's flush, which wakes the pump
	 * after one where held_wake asks it to; flush_presents is that
	 * thread's own. */
	int                 given_w, given_h;
	double              given_at;
	bool                held;
	uint16_t            held_mask;
	int                 held_x, held_y, held_w, held_h;
	FnlAtomicU32        shown_w, shown_h;
	FnlAtomicBool       held_wake;
	uint64_t            flush_presents;
	xcb_window_t        parent;        /* the frame, or the root             */
	int                 parent_x, parent_y;
	int                 child_x, child_y;   /* us, inside the frame          */

	bool                mapped;
	/* Whether a frame has anywhere to go. The main thread writes it from
	 * Map/UnmapNotify and _NET_WM_STATE_HIDDEN; the render thread reads it
	 * every turn through window_on_screen. An atomic rather than App::lock
	 * because it is one bit that nothing else has to agree with. */
	FnlAtomicBool       on_screen;

	/* What the event handlers asked for, spent once at the end of the pump.
	 * Events arrive in bursts, and answering each would cost a swapchain
	 * rebuild or a blocking round trip for an answer about to be replaced. */
	bool                publish_pending;    /* the size or a toplevel state  */
	bool                outputs_stale;      /* the monitor list              */
	bool                work_area_stale;    /* _NET_WORKAREA or the desktop  */
	bool                refresh_stale;      /* the monitors' refresh rates   */

	/* ---- pointer capture ------------------------------------------------
	 * A grab that confines the pointer to the window, plus a warp back to the
	 * middle once it is running out of room. The delta is the difference
	 * from last_*, never from the middle: see on_motion(). */
	bool                captured;
	/* The capture stands and the grab does not: lifted with the focus, and
	 * taken again when it comes back (capture_resume()). The pointer is the
	 * desktop's meanwhile -- no warp, no delta. */
	bool                grab_suspended;
	int                 warp_x, warp_y;      /* the middle, in window pixels */
	int                 last_x, last_y;      /* the previous motion's landing */
	/* A warp has been sent and its own motion event has not come back: stops
	 * the events still in flight from asking for the same warp again. */
	bool                warp_pending;

	/* The pointer's last position in root coordinates, the space
	 * _NET_WM_MOVERESIZE is told a drag started in. Recorded on every motion
	 * and press: a begin_move arrives with no event in hand to read it from. */
	int                 ptr_root_x, ptr_root_y;
	/* A move or resize this backend is running itself, there being no manager
	 * to hand it to: the edge index plus one, so that zero is "not dragging"
	 * and there is no second flag to disagree with. */
	int                 manual_drag;
	/* Where in the window the press landed, which is what keeps a move from
	 * jumping to put the pointer at the corner. */
	int                 drag_off_x, drag_off_y;

	/* What the window's background tile was last made for, and the focus
	 * it is coloured by; see background_update(). bg_h is 0 until there is
	 * one. */
	int                 bg_bar, bg_h;
	bool                bg_active;
	bool                focus_in;

	/* Which keycodes are down, one bit each, and the keycode and timestamp
	 * of the last release: what catches an auto-repeat on a server that
	 * refused XKB's detectable form. See x11_on_key(). */
	uint8_t             key_down[32];
	xcb_keycode_t       repeat_kc;
	xcb_timestamp_t     repeat_time;

	/* ---- text entry ----------------------------------------------------
	 * The server's keysym table, and which bits of a key event's state
	 * select within it. Shift, Lock and Control have fixed bits; the three
	 * here are assigned per server and found by x11_keymap_take(). Zero is a
	 * layout without one, and a mask that matches nothing. */
	Keymap              keymap;
	/* The server said the layout changed and it has not been read since.
	 * Read by the next key that types, not by the event. */
	bool                keymap_stale;
	uint16_t            mod_num;
	uint16_t            mod_level3;
	uint16_t            mod_switch;
	/* The accent a dead key is holding for the next letter. */
	KeyCompose          compose;

	/* ---- clipboard -----------------------------------------------------
	 * A paste in flight. `clip_waiting` from the request until the answer
	 * has been handed to the core; `clip_incr` while that answer is
	 * arriving in pieces, which collect in `clip`. */
	bool                clip_waiting;
	bool                clip_incr;
	/* The transfer has outgrown what is kept, or was not text: it is read
	 * to its end for the owner's sake and then reported as no text. */
	bool                clip_over;
	/* ConvertSelection has gone, stamped with `clip_time`, a server time
	 * read off FNL_STAMP. The owner's answer carries that time back
	 * (ICCCM), which tells it from a late answer to a request given up. */
	bool                clip_asked;
	xcb_timestamp_t     clip_time;
	/* The answer is asked for on FNL_PASTE2 instead of FNL_PASTE: flipped
	 * each time a request is given up, so that what its owner still sends
	 * lands where the next request does not read. */
	bool                clip_alt;
	/* FNL_STAMP writes, counted as they go and as they come back: they
	 * come back in the order written, so the count names the stamp. Only
	 * the latest of each purpose acts -- the paste's, while one waits
	 * unasked, and the ownership's, while `clip_owning` -- and a stamp
	 * superseded acts as neither: a paste given up or asked again before
	 * its own came back would otherwise ask at a time from before this
	 * window took the selection. Pump. */
	uint32_t            clip_stamps_sent;
	uint32_t            clip_stamps_back;
	uint32_t            clip_stamp_paste;
	uint32_t            clip_stamp_own;
	bool                clip_owning;
	/* No FNL_STAMP to write, and the log has said so (clip_stamp()). */
	bool                clip_unstamped_said;
	/* A copy was made while a paste was on its way: the next request asks
	 * again, of this window, rather than share the older answer
	 * (x11_clipboard_request(), as Wayland's wl_clipboard_request()). Pump. */
	bool                clip_copied;
	/* What has arrived, and when the wait for the next word is given up. */
	ClipIn              clip;

	/* ---- outputs --------------------------------------------------------
	 * The RandR monitor list, and the work area the shell leaves free. */
	X11Monitor          monitors[MAX_OUTPUTS];
	int                 monitor_count;
	int                 work_x, work_y, work_w, work_h;
	/* Which monitor was last published, so a drag that stays on one screen
	 * costs a comparison and nothing else. */
	int                 published_monitor;

	/* Where a fullscreen or maximized window came from, for the case where
	 * there is no window manager to remember it. Pixels on the root. */
	int                 saved_x, saved_y, saved_w, saved_h;
	bool                saved_valid;

	/* Xft.dpi out of the root's RESOURCE_MANAGER, in 120ths, or 0 when the
	 * desktop does not publish one. Re-read when that property changes. */
	int                 xft_scale120;
	/* The scale WM_NORMAL_HINTS was last written at, or 0 before the first.
	 * The manager holds the floor in pixels, so a different scale in force
	 * is what says to write it again. */
	int                 hints_scale120;

	/* How many X errors have been reported. Capped, because a mistake on the
	 * pointer path would otherwise write one line per motion event. */
	int                 errors_logged;

	/* The event loop's two descriptors: an eventfd any thread can wake the
	 * pump with, and the epoll set watching it beside the connection. */
	int                 wake_fd;
	int                 epoll_fd;
};

/* ---- what the backend's files ask of one another ---------------------------
 *
 * All of it on the pump's thread, as everything that reads `conn` is.
 *
 * An "ask" sends and an X11*Ask is what was sent; the matching "take" waits
 * for it. Split so that a caller can ask for several things before it waits
 * for the first. Every ask is taken, on every path, or its reply is never
 * freed. */

/* Sequence numbers, 0 for a request that was not sent. */
typedef struct {
	unsigned int xkb_use, xkb_flags, xkb_names;
	unsigned int randr_version, monitors;
} X11ExtAsk;

typedef struct {
	xcb_get_property_cookie_t desktop, area;
} X11WorkAreaAsk;

typedef struct {
	bool                              sent;
	xcb_get_keyboard_mapping_cookie_t map;
	xcb_get_modifier_mapping_cookie_t mod;
} X11KeymapAsk;

/* x11.c */
uint32_t x11_prop_words(const xcb_get_property_reply_t *r);

/* x11_ext.c: RandR and XKB, the monitor list and the work area. */
void x11_ext_prefetch(struct BackendState *p);
void x11_sync_open(struct BackendState *p);
void x11_sync_set(struct BackendState *p, uint64_t value);
void x11_sync_close(struct BackendState *p);
void x11_present_open(struct BackendState *p);
bool x11_present_event(App *a, const xcb_generic_event_t *ev);
/* x11.c: a sync value kept for a frame Present has now answered. */
void x11_sync_answered(struct BackendState *p, uint64_t index);
X11ExtAsk x11_ext_ask(struct BackendState *p);
void x11_ext_take(struct BackendState *p, const X11ExtAsk *ask);
unsigned int x11_key_names_ask(struct BackendState *p);
void x11_key_names_take(struct BackendState *p, unsigned int seq);
bool x11_ext_event(struct BackendState *p, const xcb_generic_event_t *ev);
unsigned int x11_monitors_ask(struct BackendState *p);
void x11_monitors_take(struct BackendState *p, unsigned int seq);
void x11_refresh_read(struct BackendState *p);
X11WorkAreaAsk x11_work_area_ask(struct BackendState *p);
void x11_work_area_take(struct BackendState *p, X11WorkAreaAsk ask);
int  x11_monitor_index_for(const struct BackendState *p,
						   int x, int y, int w, int h);
void x11_monitor_room(const struct BackendState *p, int idx, bool work,
					  int *x, int *y, int *w, int *h);

/* x11_keys.c: which key an event names, and what it types. */
X11KeymapAsk x11_keymap_ask(struct BackendState *p);
void x11_keymap_take(struct BackendState *p, X11KeymapAsk ask);
void x11_keys_seed(App *a);
void x11_keys_from_names(struct BackendState *p, const uint8_t *names,
						 int first, int count);
void x11_on_key(App *a, const xcb_key_press_event_t *e, bool pressed);
void x11_text_input_changed(App *a);

/* x11_clipboard.c */
void x11_clipboard_set(App *a);
void x11_clipboard_request(App *a);
void x11_selection_request(App *a, const xcb_selection_request_event_t *e);
void x11_selection_notify(App *a, const xcb_selection_notify_event_t *e);
void x11_clip_property(App *a, const xcb_property_notify_event_t *e);
void x11_clip_expire(App *a);

#endif /* FNL_X11_STATE_H */
