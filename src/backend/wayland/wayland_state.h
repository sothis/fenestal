/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Wayland backend's handles and the state only this window server has,
 * behind App::backend_state so that app.h includes no Wayland header. For
 * wayland.c and the Wayland-to-Vulkan bridge alone. What every backend has
 * stays in App.
 */

#ifndef FNL_WAYLAND_STATE_H
#define FNL_WAYLAND_STATE_H

/* First, ahead of every protocol header: it is what points their inline
 * requests at the library opened at run time. */
#include "backend/wayland/wl_dyn.h"

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

#include "backend/clip_linux.h"
#include "backend/keymap.h"
#include "backend/loop.h"

#define WL_MAX_OUTPUTS 16

/* Frames whose presentation feedback is still to come: a compositor
 * answers one when it next shows something, a frame or two after the
 * commit, so a handful are ever pending. A frame finding none free goes
 * unstamped. */
#define WL_FEEDBACK_SLOTS 16

/* One frame's wp_presentation_feedback (wayland.c, feedback_request() and
 * feedback_take()): its proxy, the frame's FnlFrame::index, whether its
 * commit was taken back (frame_cancel), which leaves the feedback on the
 * next commit's frame, and whether the compositor said it was shown --
 * which, before a take-back, is a present refused and shown all the same
 * (surface_frame_cancel()). Under App::lock: made on the render thread,
 * answered on the main. */
typedef struct {
	struct wp_presentation_feedback *fb;
	App *a;
	uint64_t index;
	bool live, cancelled, shown;
} WlFeedback;

/* Not a wp_cursor_shape shape (those start at 1): no cursor at all, which is
 * what a captured pointer wants while the caller draws its own reticle. */
#define WL_CURSOR_HIDDEN 0xffffffffu

typedef struct {
	struct wl_output *handle;
	uint32_t global; /* its name in the registry */
	int scale;
	int mode_w, mode_h;       /* current mode, in physical pixels */
	unsigned int refresh_mhz; /* and its rate; 0 where not said */
	char name[64];            /* connector, e.g. HDMI-1 */
	char model[64];           /* make and model from wl_output.geometry */
} WlOutput;

/* What the host owns, beside the windows: the one wl_display, its registry and
 * the globals bound from it, the one seat and its devices, the clipboard and
 * text-input managers, the layout, the outputs and the event loop. One display
 * a program -- a second would be a second client, with the clipboard and the
 * input method each made two -- and one seat, so these are the host's and a
 * window borrows them. The window's own -- its surface, its xdg objects, its
 * viewports, its frames -- is in BackendState.
 *
 * The seat's events name one window at a time and are routed to it here: the
 * keyboard and the pointer by the surface in their enter/leave (DESIGN *A
 * second window*), and the clipboard and the input method, which are one
 * program's, to the window with the focus. */
struct BackendHostState {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	/* Between the keyboard's enter and its leave: the one FNL_EVENT_FOCUS
	 * owed when the keyboard goes with the capability, and no leave comes. */
	bool kb_focused;
	/* The window the seat's events go to: the one whose surface the keyboard
	 * entered, the one the pointer is inside, and the one the input method
	 * writes for (the focused one). NULL while focus or pointer is nowhere of
	 * ours. Written and read on the pump's thread alone. */
	App *kb_window;
	App *ptr_window;
	struct wl_pointer *pointer;
	/* A cursor may only be set with the serial of an enter still in force:
	 * `pointer_serial`, while `pointer_inside`. */
	bool pointer_inside;
	uint32_t pointer_serial;
	/* The shape last set under this enter; 0 is none. */
	uint32_t current_shape;
	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wp_cursor_shape_device_v1 *cursor_device;
	struct zxdg_decoration_manager_v1 *decoration_manager;
	struct zwp_pointer_constraints_v1 *pointer_constraints;
	struct zwp_relative_pointer_manager_v1 *relative_manager;
	struct wp_viewporter *viewporter;
	/* Presentation time: bound, and its clock CLOCK_MONOTONIC, the frame
	 * clock's, which is the one a stamp is taken from as it comes. */
	struct wp_presentation *presentation;
	bool presentation_mono;
	struct wp_fractional_scale_manager_v1 *fractional_manager;

	/* The windows the backend has open, newest first, the first window's
	 * included: the registry's, and one wl_window_open() is making before
	 * fnl_window_create() has linked it into Host::windows. An event that
	 * names a surface is answered from here. */
	App *open_list;

	/* The card's outputs and their modes, as the registry saw them. Which of
	 * them a window is on is the window's (BackendState::on_output): a
	 * surface per window straddles outputs of its own. */
	WlOutput outputs[WL_MAX_OUTPUTS];
	int output_count;

	/* The wakeup and the set, which watches it beside the display: Linux's
	 * eventfd and epoll, or FreeBSD's pipe and poll (backend/loop.h). */
	LoopWake wake;
	Loop     loop;
	/* The uevent socket (loop_uevent_open()), in the set beside the
	 * display: a GPU that came or went. -1 where the system has no such
	 * group, which is a hotplug nobody hears. */
	int uevent_fd;
	/* The display's descriptor is watched for room as well as for input:
	 * only while a flush has found the socket full (display_watch_out()). */
	bool display_out;

	/* The compositor's layout, and its modifiers as the union of depressed,
	 * latched and locked, which is what selects a level. One keyboard, so the
	 * layout is the host's even though what it types goes to the window. */
	Keymap keymap;
	uint32_t kb_mods;
	uint32_t kb_group;

	/* Key repeat is the client's on Wayland: a deadline the pump folds into
	 * its wait, armed while text entry is on and a repeating key is down.
	 * One keyboard, one repeat, which fires into the focused window. */
	double   repeat_due;    /* on now_seconds()'s clock */
	double   repeat_period; /* seconds between repeats */
	int32_t repeat_rate;    /* per second; 0 is off */
	int32_t repeat_delay;   /* milliseconds before the first */
	uint32_t repeat_key;    /* evdev code */
	bool repeat_armed;

	/* zwp_text_input_v3 is the seat's: one object, which entered one surface
	 * (ti_focus), enabled while the window with the focus wants text (the
	 * window it was enabled for is ti_enabled_for). The strings are one
	 * transaction, acted on at `done` for that window. Fixed buffers: a heap
	 * pointer stored through a listener's `void *` reads as a leak to
	 * -fanalyzer. */
	struct zwp_text_input_manager_v3 *ti_manager;
	struct zwp_text_input_v3 *ti;
	bool ti_focus;
	bool ti_enabled;
	App *ti_enabled_for;
	bool ti_has_commit;
	char ti_commit[1024];
	char ti_preedit[256];

	/* The clipboard's manager and the one data device made from it: the
	 * selection is the seat's, so the offer is the program's. `clip_owner` is
	 * the window whose clip_out the selection holds, and `clip_waiter` the one
	 * whose answer changes hands at clip_finish(); BackendState::clip_waiting
	 * is each window that asked and has not been answered. `clip_marker` is a
	 * mime type nobody else offers, which marks our own selection come
	 * back.
	 *
	 * `clip_source` is what this program is offering. An offer is described
	 * one mime type at a time and only then named as the selection, so the one
	 * being described is kept apart from the one in force; `*_kind` indexes
	 * CLIP_MIME. clip_fd is a paste's pipe, in the set until end of file
	 * and -1 otherwise; `clip` is what has arrived of it (clip_linux.h).
	 * `clip_sync` is outstanding from a set until the compositor has answered
	 * it, while `offer_sel` is still the selection before. */
	struct wl_data_device_manager *ddm;
	struct wl_data_device *dd;
	char clip_marker[48];
	App *clip_owner;
	App *clip_waiter;
	struct wl_data_source *clip_source;
	struct wl_callback *clip_sync;
	struct wl_data_offer *offer_new;
	struct wl_data_offer *offer_sel;
	int offer_new_kind;
	int offer_sel_kind;
	bool offer_new_ours;
	bool offer_sel_ours;
	int clip_fd;
	ClipIn clip;
	/* The newest input serial: set_selection wants one from a real event. */
	uint32_t input_serial;
};

/* [main], the pump's thread, unless tagged otherwise. */
struct BackendState {
	/* The host's display, seat and event loop, on which this window's
	 * surface, xdg objects and frames are made. */
	struct BackendHostState *host;
	/* The next entry in the host's open list (BackendHostState::open_list). */
	App *open_next;

	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *fractional;
	struct wl_surface *surface;
	/* The outputs this surface has entered and not left, a set: it straddles
	 * outputs, and each window is on its own. Held by handle, not by the
	 * host's slot, which a departed output's removal renumbers. */
	struct wl_output *on_output[WL_MAX_OUTPUTS];
	int on_output_count;
	/* The one this window follows, for sharpness: the highest-scale output it
	 * is on. */
	struct wl_output *current_output;
	/* ---- where the frames go (wl_present_window_renew()) ---------------
	 * [render] `present` is what the renderer's bridge presents into:
	 * `surface` until the first move to another GPU, a subsurface over it
	 * from then on, made anew at each move, with its own viewport. The
	 * window's surface then shows `backing`, one transparent pixel its
	 * viewport stretches over it. `parent_dirty` is window state -- an
	 * ack, the geometry, the viewport -- waiting for the window's own
	 * commit, which follows the present (`presenting`, wl_flush()). */
	struct wl_surface *present;
	struct wl_subsurface *present_sub;
	struct wp_viewport *present_viewport;
	struct wl_buffer *backing;
	bool parent_dirty;
	bool presenting;
	bool renew_refused;
	/* The subsurface the frames went into before the last renew, kept
	 * showing its last frame under the new one until the new one has shown
	 * a frame of its own, and dropped then (present_settle()); and
	 * `backing` owed to the window's surface on the same terms at the
	 * first renew, so its own last frame stays under the first subsurface
	 * meanwhile. [App::lock] `present_old_marked` says the outstanding
	 * frame callback is the new surface's, and `present_old_shown` that
	 * it was answered, which is the frame before it on screen. */
	struct wl_surface *present_old;
	struct wl_subsurface *present_old_sub;
	struct wp_viewport *present_old_viewport;
	bool backing_owed;
	bool present_old_marked;
	bool present_old_shown;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zxdg_toplevel_decoration_v1 *decoration;

	/* Non-NULL while captured. Locked, not confined: a locked pointer reports
	 * relative motion, so the reticle can be placed. */
	struct zwp_locked_pointer_v1 *locked;
	struct zwp_relative_pointer_v1 *relative;
	/* The accelerated delta stood in for a missing unaccelerated one, and
	 * the log has said so: once, since it is every motion after. */
	bool relative_accel_said;

	/* The last button *press*, which xdg_toplevel.move and .resize want; never
	 * `pointer_serial`, an enter's, which a compositor rejects. `left_serial`
	 * is preferred while `left_held`: a second button pressed during a
	 * titlebar drag must not replace it. */
	uint32_t button_serial;
	uint32_t left_serial;
	bool left_held;
	/* A wheel's whole notches on each axis (vertical, horizontal), from
	 * wl_pointer.axis_discrete, which arrives just before the axis event it
	 * describes; zero for a smooth source, whose value is scaled instead. */
	int32_t axis_discrete[2];

	/* Set once server-side decorations have been complained about. */
	bool deco_ssd_warned;

	/* ---- the configure in the making -----------------------------------
	 * xdg_toplevel.configure's size and states, pending until
	 * xdg_surface.configure applies them. A size of 0 is "pick your own". */
	int32_t cfg_w, cfg_h;
	bool cfg_fullscreen, cfg_maximized, cfg_activated;
	/* A size the application asked for. The compositor answers with a
	 * configure echoing the size it still has, so the request stands against
	 * exactly that one, identified by req_from_*. Not "the next configure":
	 * mutter sends none in reply. req_w of 0 is none pending. */
	int req_w, req_h;
	int32_t req_from_w, req_from_h;
	/* xdg_toplevel.configure_bounds, in points. 0 is not said: a pre-v4
	 * xdg_wm_base, or an explicit "unknown". */
	int bounds_w, bounds_h;

	/* ---- scale ---------------------------------------------------------
	 * wp_fractional_scale_v1's figure, in 120ths; 0 until it has said. */
	int preferred_scale120;
	/* [render] The viewport destination last set, in surface points. */
	int applied_vp_w, applied_vp_h;
	/* [render] The source rectangle last set on the window's viewport and
	 * on the frames' own, in buffer pixels from the corner: zero for none,
	 * the whole buffer (crop_apply()). */
	int src_w, src_h, present_src_w, present_src_h;

	/* Presentation feedback: asked on a commit and answered when the frame
	 * was shown, so per surface and so per window. */
	WlFeedback feedback[WL_FEEDBACK_SLOTS];
	int feedback_cur; /* -1: none */

	/* [App::lock] `frame_cb` is non-NULL while an invitation is outstanding,
	 * so at most one is; `frame_ready` is what the render thread waits on. */
	struct wl_callback *frame_cb;
	bool frame_ready;
	/* [App::lock] A callback asked of a subsurface since destroyed, which
	 * may never be answered. Destroyed by whichever thread takes it out of
	 * this slot under the lock: frame_done() if it is answered,
	 * present_drop() if a later orphan displaces it, close() if neither.
	 * frame_done() destroys only a callback it took from a slot, so a
	 * displaced one whose answer is being dispatched is not destroyed
	 * twice; libwayland keeps the proxy alive for an event in dispatch. */
	struct wl_callback *frame_cb_orphan;

	/* The held accent, where no input method holds it instead. */
	KeyCompose compose;

	/* Pump thread: this window has asked for the clipboard and not been
	 * answered. Every window with it set is answered by the one paste's
	 * finish (clip_finish()), or at once where our own selection answers. */
	bool clip_waiting;
};
/* wl_surface.set_buffer_scale arrived in wl_compositor v3. Without it -- and
 * without a viewport -- the only scale we can express is 1.0. */
static inline bool wl_can_set_buffer_scale(const struct BackendState *p)
{
	return p->surface && wl_surface_get_version(p->surface) >=
							 WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
}

#endif /* FNL_WAYLAND_STATE_H */
