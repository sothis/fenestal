/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The backend seam: everything the core asks of a window server. One of two
 * dispatch tables; see DESIGN *Two dispatch tables*.
 *
 * The core never asks for a native handle, for where the window is on screen,
 * or to move the pointer: Wayland cannot answer any of the three, and a seam
 * entry one backend cannot fill is one to renegotiate.
 *
 * Every entry takes the App; the backend's own state hangs off it as
 * App::backend_state.
 *
 * ---- when an entry can be called -------------------------------------------
 *
 * Every entry except close() and wakeup() runs only between a successful
 * open() and the start of close(), so a backend does not test
 * App::backend_state or its window there. close() is also the failed-open
 * path out of fnl_init(), and wakeup() may arrive from a signal handler or
 * from another thread at any moment, close() included; those two test.
 *
 * Which thread: the pump's, except the entries the frame makes -- frame_wait,
 * frame_request, frame_cancel, window_on_screen, set_geometry, ack_configure,
 * apply_scale, present_pixels, present_window_renew, present_window_hold and
 * flush are the render thread's, which has been joined before close() runs,
 * and buffer_granularity is asked by whichever thread is building a
 * swapchain. update_scale is called twice from fnl_init() as well, ahead of
 * the first pump turn, so a backend that queues requests flushes for itself
 * there.
 */

#ifndef FNL_BACKEND_H
#define FNL_BACKEND_H

/* Included from app.h, which has App and FnlResult by this point. */

/* Which backend a table is, for what has one half per backend beside the
 * table: a renderer's bridges. From 1, so that a table which never said is
 * caught where the backend is chosen rather than taken for Wayland. */
typedef enum {
	BACKEND_WAYLAND = 1, BACKEND_X11, BACKEND_KMS, BACKEND_WIN32,
	BACKEND_COCOA
} BackendId;

typedef struct FnlBackendVtbl {
	/* As it goes on screen: "Wayland", "X11", "Win32", "Cocoa", "KMS". */
	const char *name;
	BackendId id;
	/* As FNL_BACKEND spells it, at configure and in the environment:
	 * "wayland", "x11", "kms", "win32", "cocoa". */
	const char *key;

	/* Why this session has none of this backend, as it goes in the log --
	 * "no WAYLAND_DISPLAY" -- or NULL where it may. Read from the
	 * environment alone, before open(), and only where the backend is
	 * chosen rather than named: a session with no reason to have one is
	 * not made to try. backend_always_present() where the system has one
	 * backend and nothing to choose. */
	const char *(*absent)(void);

	/* What stands between this backend and a window server of its own, as
	 * it goes on screen, or "" for nothing: a string literal. Asked and not
	 * a field beside `name`, because it is only known once the process is
	 * running. Callable at any time and from any thread, so it reads
	 * nothing of App::backend_state. */
	const char *(*host)(const App *a);

	/* Whether the renderers here are translations onto one driver
	 * underneath -- wine's Direct3D 12 is vkd3d-proton on the Vulkan its
	 * Vulkan is -- so that one listing its GPUs beside another that draws
	 * is a second user of that driver while the first presents, which
	 * NVIDIA's on Wayland does not survive. There every renderer the list
	 * spans is listed before the one drawing is built
	 * (renderer_choose_and_build()). Asked on the main thread after
	 * open(). */
	bool (*shared_driver)(const App *a);

	/* The GPUs here that can be moved to another kernel driver, into `out`,
	 * at most `max`, and how many; and the move, `g` onto `driver`, true
	 * once it is bound and its device nodes are there. An NVIDIA GPU
	 * between nouveau and NVIDIA's own module, through a root helper
	 * (src/backend/kms/kms_driver.c): KMS's alone, the one backend whose
	 * program nothing else shares the GPU with; the others name the
	 * no-ops. Nothing is listed where the card the backend scans out on
	 * is an NVIDIA GPU's: it holds that card for the window's life.
	 * Each may wait on a program, so neither is the pump's: they are asked
	 * by fnl_init() after open(), by the render thread at a move with
	 * nothing of the renderer built, and by the move's own thread, and
	 * read of App::backend_state only what open() set and nothing writes
	 * again. DESIGN *Another kernel driver, with the window open*. */
	int (*gpu_drivers)(const App *a, GpuDriver *out, int max);
	bool (*gpu_driver_set)(const App *a, const GpuDriver *g,
						   const char *driver);
	/* Whether this process holds `g` open as its driver counts a hold --
	 * a descriptor on the GPU's own nodes, and on NVIDIA's module any
	 * /dev/nvidia* -- with the first such path in `what`: asked of the
	 * renderer drawn across a move, which must hold none of it (DESIGN
	 * *Drawn across the move*). With `every_nvidia`, any /dev/nvidia*
	 * whatever `g` is bound to: asked before the move, with no renderer
	 * built, when nothing of NVIDIA's driver may be left in the process.
	 * From any thread. */
	bool (*gpu_driver_held)(const App *a, const GpuDriver *g,
							bool every_nvidia, char *what, size_t size);

	/* The transparent margin a client-decorated window carries outside its
	 * geometry, in logical points, or 0 where the window server draws its own
	 * resize edges. In force only while there is a titlebar; deco_margin()
	 * is the reader. */
	int grab_ring;

	/* Whether frame_wait() blocks until the server asks for a frame,
	 * timestamped into App::frame_cb_at, or returns at once and leaves the
	 * present as the only throttle. Picks the late-latch signal in
	 * src/core/frame.c.
	 *
	 * Declared, never divined from `App::frame_cb_at > 0.0`: only an
	 * inviting backend writes that field, so the test reads as a permanent
	 * zero everywhere else and nothing fails. */
	bool invites;

	/* Whether the frame loop holds each frame, ahead of its latch, until the
	 * window server says the frame before it is on the screen
	 * (app_frame_answered()): where the present is the only throttle and
	 * the graphics API keeps frames waiting ahead of the screen. X11's,
	 * from the Present extension's completions: Mesa's Vulkan WSI kept
	 * three there, 54 ms from a frame's latch to the screen. Not Win32's
	 * until it is measured. Declared, for invites' reason. */
	bool display_waits;

	/* A configure's ack must not reach the window server before the frame
	 * that answers it is on the screen: X11's _NET_WM_SYNC_REQUEST, whose
	 * counter the manager waits on and the flush after the present sets.
	 * A graphics API whose present only queues the frame for a thread of
	 * the driver's own waits for it to be shown first, where it can tell
	 * (VK_KHR_present_wait), and only on the frame that acks. */
	bool ack_when_shown;

	/* Whether the window is the output itself, as on KMS: it can be neither
	 * minimized nor maximized and restored, so the titlebar offers neither
	 * (deco_button_rects()), and its size is the output's display mode,
	 * chosen with set_mode and set_output rather than asked for with
	 * request_size. Declared, for invites' reason. */
	bool is_output;

	/* Whether set_output can move the window only while it is fullscreen:
	 * Wayland, whose clients place nothing and can only name the output a
	 * fullscreen window fills. The titlebar's output box is drawn dim and
	 * inert there, since the bar is never shown in fullscreen. */
	bool outputs_fullscreen_only;

	/* ---- lifetime ------------------------------------------------------ */

	/* Connect, bind the globals, create the window, and block until the
	 * server has configured it. Everything this allocates is released by
	 * close(), which must be safe on a partly-opened backend because this is
	 * also the failure path out of fnl_init().
	 *
	 * FNL_ERR_NO_DISPLAY says there is no window server here to speak to,
	 * and only that: where the backend was chosen, fnl_init() closes this
	 * one and opens the next in the order. So it is returned before the
	 * window exists, and never for a failure the next backend would meet
	 * as well -- memory, a descriptor -- which ends fnl_init() instead. */
	FNL_MUST_USE FnlResult (*open)(App *a);
	void (*close)(App *a);

	/* ---- the event loop ------------------------------------------------ */

	/* One turn: block until something arrives, then dispatch it. */
	void (*pump)(App *a);
	/* Break a blocked pump() from another thread. */
	void (*wakeup)(App *a);
	/* Non-zero once the connection has failed, with the reason recorded. The
	 * loop ends on it. */
	FNL_MUST_USE FnlResult (*check_error)(App *a);

	/* ---- what the render thread sends ----------------------------------
	 * Immediately around the present, which is what carries them to the
	 * server. */

	/* The window geometry and opaque region for the buffer about to be
	 * committed. Sent with that buffer rather than from the configure
	 * handler, which would describe a size the surface does not have yet. */
	void (*set_geometry)(App *a, int w, int h, int margin);
	/* Answer the configure this buffer is the answer to. */
	void (*ack_configure)(App *a, uint32_t serial);
	/* Ask to be told when the next frame is wanted, and take that request
	 * back when the present it was riding on never committed -- or, where
	 * the server had answered that present already, say so in
	 * App::present_answered_anyway and keep it; renderers call it through
	 * app_frame_take_back(). */
	void (*frame_request)(App *a);
	void (*frame_cancel)(App *a);
	/* Block until the server invites another frame, or until the wait times
	 * out. False on timeout, which the loop reads as "the window is not on
	 * screen" -- but see TRANSITION_GRACE in src/core/frame.c. */
	bool (*frame_wait)(App *a, bool break_on_urgent);

	/* Is there anywhere for a frame to go? False means minimized, hidden or
	 * otherwise not shown. Cheap and non-blocking: the frame asks every turn.
	 * A backend that can only answer by waiting for an invitation says true
	 * and answers through frame_wait(). A window that is mapped but fully
	 * covered is not this entry's to report; see
	 * FnlRendererVtbl::occluded. */
	bool (*window_on_screen)(const App *a);

	/* A frame's pixels, for a backend that shows them itself: the copy
	 * path, where no device can present to the output and the frame is
	 * drawn on a headless surface (RendererState::present_copy). Called once
	 * the device has finished the frame, right after its present; `px` is
	 * good until this returns. 32-bit pixels, rows `pitch` bytes apart, blue
	 * first when `bgra` and red first when not, alpha ignored. Under vsync
	 * this waits until they are on screen, which makes it the throttle the
	 * swapchain's present is everywhere else. Everywhere else it is
	 * backend_noop_present_pixels, and never called. */
	void (*present_pixels)(App *a, const void *px, int w, int h,
						   size_t pitch, bool bgra);

	/* A native window of its own for the renderer to present into, made
	 * anew at a move to another GPU (fnl_request_gpu()): called by the
	 * renderer's recovery between the old surface's teardown and the new
	 * one's, on the render thread. A driver can claim a window for good --
	 * on Win32 a pixel format, which a window takes once: aspire's Intel
	 * Vulkan driver sets one, and NVIDIA's swapchain on that window is then
	 * VK_ERROR_NATIVE_WINDOW_IN_USE_KHR for the window's life; or a window
	 * server can keep a surface's last frame for good, as GNOME Shell does
	 * once lavapipe draws where a hardware GPU did. Win32's is a child
	 * window filling the client area and Wayland's a subsurface covering
	 * the surface, each made at the first move and made again at each one
	 * after; backend_noop where neither has been seen. */
	void (*present_window_renew)(App *a);
	/* The window the frames go into, about to lose its swapchain -- a
	 * move, a loss, a swap of renderer -- told while the swapchain is
	 * still up, on the render thread, by every renderer's recovery and by
	 * the swap. Where the window server shows what was last painted under
	 * a window whose swapchain has gone, the backend paints the gap's
	 * picture there first: the frame read back (App::snap), else black --
	 * the same picture as the frame on screen, or the fallback the veil
	 * makes of the gap, never one from an earlier move. Win32's; backend_noop
	 * where the window server keeps the last frame or fills the gap itself
	 * (DESIGN *The veil*). */
	void (*present_window_hold)(App *a);

	/* Send whatever has been queued onto the wire now, without waiting for
	 * the next commit: the ack and the frame request must ride out with the
	 * present, and a state change made by a frame whose acquire timed out
	 * must not wait for the next one. A no-op where requests are not
	 * queued. While App::ack_held, the ack it would release is kept. */
	void (*flush)(App *a);
	/* [any thread] The frame that acks the last configure is on the screen:
	 * release the ack now, which flush() kept while App::ack_held. From the
	 * renderer's thread that waited for the frame, so the render thread
	 * goes on drawing meanwhile. Where ack_when_shown; NULL elsewhere. */
	void (*ack_release)(App *a);

	/* ---- window state --------------------------------------------------
	 * Asked for, not set: a server may answer with something else, or with
	 * nothing, and what actually happened comes back as a configure. */

	/* Ask for a *window* of this size, in logical points: the titlebar is
	 * already in the height, and the sizes are positive. The backend
	 * publishes the size it asked for where no answer will follow, and
	 * corrects itself when the answer differs.
	 *
	 * Reached only through app_request_size(), which refuses while the
	 * window is fullscreen or maximized -- fnl_request_size_pt() promises that,
	 * and a window server cannot be relied on to decline by itself. */
	void (*request_size)(App *a, int w_pt, int h_pt);
	/* Have the window server announce the window's size anew, the window
	 * ending at the size it has: the render thread asks when every
	 * swapchain made at that size is out of date at once (frame.c,
	 * swap_stale_note()). backend_noop where that has not been seen. */
	void (*nudge_size)(App *a);
	void (*set_fullscreen)(App *a, bool on);
	void (*set_maximized)(App *a, bool on);
	void (*minimize)(App *a);

	/* ---- the output's modes, where is_output, and the outputs -----------
	 * Modes are KMS's alone: elsewhere the list answers 0 and set_mode does
	 * nothing (backend_noop_*). The outputs are every backend's: KMS lists
	 * its card's, and a window server's backend publishes its monitors
	 * (app_publish_outputs()) and answers from that copy
	 * (backend_published_outputs()). The lists are asked from any thread --
	 * the titlebar's are drawn on the render thread and hit-tested on the
	 * pump's, and the public getters are [any] -- so they read nothing that
	 * changes after open() but atomics, or App::lock's copy.
	 *
	 * modes: the modes of the output the window is on, at most `max`, in the
	 * order a list shows them; `*current` is the index of the one in force,
	 * or -1. outputs: the outputs the window can be on, their names into
	 * `names`, at most `max`; `*current` likewise. */
	int (*modes)(const App *a, FnlMode *out, int max, int *current);
	int (*outputs)(const App *a, char (*names)[OUTPUT_NAME_MAX], int max,
				   int *current);
	/* Set the mode of this size, the refresh nearest, on the output the
	 * window is on; or move the window to the output of this name -- on
	 * KMS at its preferred mode, under a window server at the window's
	 * size and state, where app_place_on_output() puts it. Neither
	 * answers: what happened comes back as a publish, like a configure. */
	void (*set_mode)(App *a, const FnlMode *mode);
	void (*set_output)(App *a, const char *name);

	/* Hand the pointer over and let the window server move or resize the
	 * window from the press currently held; see fnl_begin_move(). The edge
	 * is this library's own spelling, mapped by each backend, and EDGE_NONE
	 * is rejected rather than guessed at. Also what the library's own
	 * titlebar and resize edges call, from deco_drag() and deco_press(), so
	 * a backend keeps what it needs of the held press -- a serial, a root
	 * position -- as each button event arrives. */
	void (*begin_move)(App *a);
	void (*begin_resize)(App *a, Edge edge);

	/* Re-apply the size floor after it moved. Called when a caller replaces
	 * the titlebar, and once after the renderer's first GPU list, which
	 * puts the GPU box in the bar: the two things that change
	 * deco_min_window() after open(). */
	void (*apply_size_bounds)(App *a);

	/* ---- scale --------------------------------------------------------- */

	/* Re-resolve the surface scale from the outputs, the compositor's
	 * preference, the override and whatever ceiling the device reported.
	 * Idempotent; called whenever any of those may have moved. */
	void (*update_scale)(App *a);

	/* Tell the server how many logical points the buffer covers, and return
	 * whether anything had to be said. The frame's size is already latched,
	 * so it compares against what was last applied. */
	bool (*apply_scale)(App *a);

	/* The multiple a presentable buffer's width and height must each be a
	 * whole number of, at this scale, or 1 when the backend has no such
	 * rule. Wayland without wp_viewporter has one: the buffer must be an
	 * exact multiple of the integer scale or the attach is rejected. */
	uint32_t (*buffer_granularity)(const App *a, int s120);

	/* ---- pointer ------------------------------------------------------- */

	/* Take and release the pointer. Captured, it stops moving and reports
	 * relative motion; where the reticle goes is the application's. A click
	 * into the presentable area calls capture_pointer from deco_press().
	 * The backend sets App::pointer_captured and says app_reticle_start()
	 * and app_reticle_publish(). */
	void (*capture_pointer)(App *a);
	void (*release_pointer)(App *a);

	/* App::cursor changed -- show it, if the pointer is somewhere it
	 * applies. A notification and not a setter: the backend's pointer path
	 * picks between it, a resize edge and nothing on every motion, and this
	 * asks for that decision once more. See deco_cursor_is_callers(). */
	void (*cursor_changed)(App *a);

	/* ---- text ---------------------------------------------------------- */

	/* Text entry was switched, or the caret moved. The mode itself is read
	 * per key from App::text_input; this is for a window server that has to
	 * be asked to compose. A no-op where there is nobody to ask. */
	void (*text_input_changed)(App *a);

	/* App::clip_out is what this window now offers; take the selection. */
	void (*clipboard_set)(App *a);
	/* Fetch the clipboard's text and hand it to app_clipboard_deliver(),
	 * now or when it arrives -- but always, even when there is none. */
	void (*clipboard_request)(App *a);

	/* ---- the title ------------------------------------------------------ */

	/* fnl_set_title(), from app_drain_requests(): tell the window server
	 * `utf8`, a copy the drain took under App::lock. The library's own bar
	 * draws the frame's copy and needs nothing from here. */
	void (*set_title)(App *a, const char *utf8);
} FnlBackendVtbl;

/* The backends this build has, in the order fnl_init() tries them: DESIGN
 * *One build per operating system*. Windows and macOS have one each, by the
 * test CMake picks their sources with -- if(WIN32) there and _WIN32 here,
 * if(APPLE) and __APPLE__ likewise. A Linux build has those of Wayland, X11
 * and KMS that FNL_BACKEND gave it, each a -DFNL_HAVE_<NAME> beside its
 * sources, so a backend listed and not built is an undefined reference at
 * the link, and a build that defines none fails to compile the core's empty
 * list.
 *
 * FNL_BACKENDS(X) is X(key) for each, in order: the core's list and every
 * renderer's bridges are made from it, and cannot disagree on the set or on the
 * order. */
#if defined(_WIN32)
#  define FNL_BACKENDS(X) X(win32)
#elif defined(__APPLE__)
#  define FNL_BACKENDS(X) X(cocoa)
#else
#  if defined(FNL_HAVE_WAYLAND)
#    define FNL_WITH_WAYLAND(X) X(wayland)
#  else
#    define FNL_WITH_WAYLAND(X)
#  endif
#  if defined(FNL_HAVE_X11)
#    define FNL_WITH_X11(X) X(x11)
#  else
#    define FNL_WITH_X11(X)
#  endif
#  if defined(FNL_HAVE_KMS)
#    define FNL_WITH_KMS(X) X(kms)
#  else
#    define FNL_WITH_KMS(X)
#  endif
/* Wayland ahead of X11, which on a Wayland desktop is XWayland, a second
 * server between the window and the compositor; KMS last, and only where
 * neither window server is in the environment (kms.c, kms_absent()). */
#  define FNL_BACKENDS(X) FNL_WITH_WAYLAND(X) FNL_WITH_X11(X) FNL_WITH_KMS(X)
#endif

extern const FnlBackendVtbl fnl_backend_wayland;
extern const FnlBackendVtbl fnl_backend_x11;
extern const FnlBackendVtbl fnl_backend_kms;
extern const FnlBackendVtbl fnl_backend_win32;
extern const FnlBackendVtbl fnl_backend_cocoa;

/* Open a backend: the one FNL_BACKEND names in the environment, or else
 * the first in the order that is not absent and does not refuse with
 * FNL_ERR_NO_DISPLAY. Sets App::backend. In app.c. */
FNL_MUST_USE FnlResult backend_choose_and_open(App *a);

#define BACKEND(a) ((a)->backend)

/* ---- entries a backend may name rather than write --------------------------
 * In src/core/app.c. For a window server that has nothing to do there;
 * named in the backend's own table rather than left out, which designated
 * initialisers compile as a NULL entry.
 *
 * backend_noop serves every `void (*)(App *)` entry: frame_request,
 * frame_cancel, flush, text_input_changed, present_window_renew,
 * present_window_hold. backend_frame_wait_shown is
 * frame_wait where nothing invites a frame: it answers from the backend's own
 * window_on_screen, and waits out FRAME_WAIT_S before saying no. */
void     backend_noop(App *a);
const char *backend_noop_host(const App *a);
bool     backend_noop_shared_driver(const App *a);
int      backend_noop_gpu_drivers(const App *a, GpuDriver *out, int max);
bool     backend_noop_gpu_driver_set(const App *a, const GpuDriver *g,
									 const char *driver);
bool     backend_noop_gpu_driver_held(const App *a, const GpuDriver *g,
									  bool every_nvidia, char *what,
									  size_t size);
void     backend_noop_set_geometry(App *a, int w, int h, int margin);
void     backend_noop_ack_configure(App *a, uint32_t serial);
void     backend_noop_present_pixels(App *a, const void *px, int w, int h,
									 size_t pitch, bool bgra);
bool     backend_noop_apply_scale(App *a);
uint32_t backend_noop_buffer_granularity(const App *a, int s120);
int      backend_noop_modes(const App *a, FnlMode *out, int max,
							int *current);
int      backend_noop_outputs(const App *a, char (*names)[OUTPUT_NAME_MAX],
							  int max, int *current);
void     backend_noop_set_mode(App *a, const FnlMode *mode);
void     backend_noop_set_output(App *a, const char *name);
/* FnlBackendVtbl::outputs for a window server's backend: the list it last
 * gave app_publish_outputs(), read under App::lock. */
int      backend_published_outputs(const App *a,
								   char (*names)[OUTPUT_NAME_MAX], int max,
								   int *current);
void     backend_noop_set_title(App *a, const char *utf8);
FNL_MUST_USE FnlResult backend_noop_check_error(App *a);
bool     backend_frame_wait_shown(App *a, bool break_on_urgent);
/* absent() where there is nothing to choose: NULL. */
const char *backend_always_present(void);

#endif /* FNL_BACKEND_H */
