/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Shared state and the interface each translation unit exposes to the others.
 * Include this first in every .c file.
 *
 * Nothing backend- or renderer-specific reaches this file: what it includes
 * is libc and the two shims. Each backend and each renderer keeps its own
 * state behind an opaque pointer (App::backend_state,
 * App::renderer_state).
 */

#ifndef APP_H
#define APP_H

#include "core/compiler.h"    /* also pins _POSIX_C_SOURCE; must precede libc */
#include "core/thread.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* The backend's own state, defined by whichever one was compiled in and
 * opaque to everything else -- src/backend/<p>/<p>_state.h. The pointer is
 * App::backend_state. */
struct BackendState;
/* The same for the graphics API: src/renderer/<g>/<g>_state.h,
 * App::renderer_state. */
struct RendererState;

/* ---- errors ----------------------------------------------------------------
 * Two tiers, and neither ends the process. The unrecoverable -- no window, no
 * GPU, no memory -- records what broke and returns a negative FnlResult, and
 * the caller unwinds. Anything degradable returns bool or NULL and puts one
 * line through the log callback.
 *
 * FNL_SKIPPED is the one non-zero success: an acquire that timed out, or a
 * frame for a window that is not on screen. It must not be logged as an
 * error -- an occluded window produces one every tenth of a second. */
#include "fenestal/fenestal.h"

/* Defined further down; the two dispatch tables name it first. */
typedef struct FnlApp App;

/* Record a failure and return the code, so a caller can write
 * `return fnl_fail(FNL_ERR_SYSTEM, "...")` and have both happen at once. */
FNL_MUST_USE FnlResult fnl_fail(FnlResult code, const char *fmt, ...)
	FNL_PRINTF(2, 3);
/* The same, recorded and not logged: a refusal said once already, asked
 * again -- the GPU listing and the renderer each open the Vulkan loader --
 * whose reason the caller still wants for its own line. */
FNL_MUST_USE FnlResult fnl_fail_quiet(FnlResult code, const char *fmt, ...)
	FNL_PRINTF(2, 3);
/* One line, for the degradable tier. Goes to the log callback, or to stderr
 * when none is set. Nothing in the tree writes to stderr directly. */
void fnl_log(FnlLogLevel lvl, const char *fmt, ...) FNL_PRINTF(2, 3);
void fnl_set_log_fn(FnlLogFn fn, void *ud);

/* An invariant the code relies on and no caller input can break. Off under
 * NDEBUG, so in every optimised tree; in a Debug tree a failure is one line
 * through the log sink and then abort() -- the one exception to "nothing ends
 * the process", taken because a broken invariant is a bug in this library
 * and the sanitizers print their stack on abort. Not for input validation:
 * what a caller or a peer can get wrong is answered by the two error tiers,
 * in every build. Not callable from a signal handler. */
FNL_NORETURN void fnl_assert_fail(const char *expr, const char *file,
								  int line, const char *func);
#ifdef NDEBUG
#  define FNL_ASSERT(cond) ((void)0)
#else
#  define FNL_ASSERT(cond)                                                    \
	((cond) ? (void)0 : fnl_assert_fail(#cond, __FILE__, __LINE__, __func__))
#endif

/* Which of the library's threads this is: the one that called fnl_init(),
 * which pumps the window server, or the render thread. Anything else -- the
 * reaper, a caller's own -- is OTHER. Thread-local, so asking is never a
 * race; set once at the top of each thread. FNL_ASSERT_THREAD is for the
 * library's own paths: a caller on the wrong thread is answered by the
 * public entry point, in every build, and never reaches one of these. */
typedef enum { APP_THREAD_OTHER, APP_THREAD_MAIN, APP_THREAD_RENDER } AppThread;
void      app_thread_set(AppThread t);                         /* app.c */
AppThread app_thread(void);
#define FNL_ASSERT_THREAD(t) FNL_ASSERT(app_thread() == (t))
/* Whether this thread may read App::renderer_state: the render thread, or
 * the main thread while there is none -- before fnl_run() starts it, which
 * is the first on_gpu_created's, and after it is joined, the last
 * on_gpu_lost's. Asked by the interop getters a caller may call from
 * anywhere; the library's own paths assert the thread instead. */
bool      app_renderer_state_ours(const App *a);               /* app.c */

/* A recovery or fallback path made to run by name, from FNL_FAULT in the
 * environment (core/fault.c has the grammar and the list of sites). Debug
 * trees only: under NDEBUG a site is the constant false and compiles away.
 * A new site is a name in fault.c's SITES and a row in DESIGN *Forcing the
 * paths that never run*. */
/* FNL_SELFTEST(name) is the same for a deliberate bug a backend or a
 * renderer commits itself, where only it can (fault.c's list): whether
 * FNL_SELFTEST names it. */
#ifdef NDEBUG
#  define FNL_FAULT(name) false
#  define FNL_FAULT_INIT() ((void)0)
#  define FNL_SELFTEST(name) false
#else
void fault_init(void);                                     /* fault.c */
bool fault_hit(const char *name);
bool fault_selftest(const char *name);
#  define FNL_FAULT(name) fault_hit(name)
#  define FNL_FAULT_INIT() fault_init()
#  define FNL_SELFTEST(name) fault_selftest(name)
#endif

/* Propagate a failure out of the current function. Early return rather than a
 * goto: almost every failure is answered by the caller tearing the whole
 * device down (see destroy_device_objects), so there is nothing half-built to
 * unwind past. A function that does own something uses `goto fail`. */
#define FNL_TRY(expr)                                                         \
	do {                                                                      \
		FnlResult fnl_try_r_ = (expr);                                        \
		if (fnl_try_r_ < 0)                                                   \
			return fnl_try_r_;                                                \
	} while (0)

/* The presentable area the window starts with, in pixels -- everything under
 * the titlebar, not the window, which comes out taller by that titlebar. */
#define DEFAULT_AREA_W 1280
#define DEFAULT_AREA_H 720

#define MAX_FRAMES_IN_FLIGHT 2
#define MAX_OUTPUTS 16

/* How long a wait for the server's invitation to draw may last: an occluded
 * or minimized surface stops being invited, and the render thread must not
 * park behind an invitation that is never coming. */
#define FRAME_WAIT_S       0.1            /* seconds */

/* ---- text metrics ---------------------------------------------------------
 * A closed set of sizes: an atlas is baked for each at every surface scale,
 * and tools/mkfont emits their metrics into font_metrics.h. TEXT_BODY and
 * TEXT_LARGE are the monospaced face; TEXT_UI is the proportional one. See
 * DESIGN *Text*.
 *
 * The type is the public one and the short names are its values, so nothing
 * converts between the two. */
typedef FnlTextSize TextSize;
#define TEXT_BODY       FNL_TEXT_BODY
#define TEXT_LARGE      FNL_TEXT_LARGE
#define TEXT_UI         FNL_TEXT_UI
#define TEXT_SIZE_COUNT FNL_TEXT_SIZE_COUNT

#include "font_metrics.h"

_Static_assert(FONT_SIZE_COUNT == TEXT_SIZE_COUNT,
			   "mkfont baked a different number of text sizes than app.h has");

/* Pen step per glyph, logical points, for a monospaced size; 0 for TEXT_UI,
 * which has an advance per glyph. text_width() measures either. */
static inline int text_advance(TextSize sz) { return FONT_ADV_PT[sz]; }
static inline bool text_is_mono(TextSize sz) { return FONT_SIZE_MONO[sz] != 0; }
/* Cap height, logical points: what text is centred in a box by. */
static inline int text_height(TextSize sz)  { return FONT_CAP_PT[sz]; }

/* ---- client-side decorations ----------------------------------------------
 * In logical points, DECO_DBLCLICK_MS apart; scale_pt() converts to physical
 * pixels. */
#define DECO_TITLEBAR_H 34
#define DECO_TITLE_X    12      /* where the title text starts */
/* How deep a resize edge is: a transparent margin *outside* the window
 * geometry where the backend asks for a grab ring
 * (FnlBackendVtbl::grab_ring), and the same depth hit-tested as an inner
 * margin where it does not. */
#define DECO_GRAB        6
#define DECO_BTN_W      28
#define DECO_BTN_H      20
#define DECO_BTN_GAP     6
#define DECO_BTN_MARGIN 10
#define DECO_DBLCLICK_MS 400
/* How far the pointer must travel with the button held before a press on the
 * titlebar becomes a window move rather than a click. */
#define DECO_DRAG_SLOP   4

/* The wider gap that sets fullscreen apart from minimize/maximize/close. */
#define DECO_BTN_GROUP_GAP 12

/* The whole button strip including the margin outside it: the arithmetic
 * deco_button_rects() walks, as one number. */
#define DECO_BTN_STRIP_W  (DECO_BTN_MARGIN + 4 * DECO_BTN_W + 2 * DECO_BTN_GAP \
						   + DECO_BTN_GROUP_GAP)

/* The smallest presentable area worth *offering*, in logical points: what
 * fnl_min_content_size_pt() reports. Advisory; the size the window refuses to
 * go below is deco_min_window()'s. */
#define CONTENT_MIN_W     640
#define CONTENT_MIN_H     480

/* ---- the hard floor -------------------------------------------------------
 * The smallest window whose button strip still shows, in logical points of
 * window geometry -- the grab ring outside it does not count. The width is
 * one point above where deco_button_rects() hides the strip (`w -
 * DECO_BTN_STRIP_W <= DECO_BTN_MARGIN`); the height is the bar. The floor
 * enforced is deco_min_window()'s, which adds the controls' run to this
 * width; see DESIGN *The size floor*. */
#define DECO_MIN_W        (DECO_BTN_STRIP_W + DECO_BTN_MARGIN + 1)
#define DECO_MIN_H        DECO_TITLEBAR_H

_Static_assert(DECO_MIN_W > DECO_BTN_STRIP_W + DECO_BTN_MARGIN,
			   "DECO_MIN_W is below the width where the button strip vanishes");
_Static_assert(DECO_MIN_H == DECO_TITLEBAR_H,
			   "DECO_MIN_H is not the titlebar's height -- the floor exists to "
			   "keep the bar drawable and grabbable, and nothing else");
_Static_assert(CONTENT_MIN_W >= DECO_MIN_W &&
			   CONTENT_MIN_H + DECO_TITLEBAR_H >= DECO_MIN_H,
			   "CONTENT_MIN_* is below the hard floor, so "
			   "fnl_min_content_size_pt would offer sizes the window will "
			   "not take");

/* ZONE_BUTTON_* stay contiguous and in this order: deco_button_rects() and the
 * hit test both index them as ZONE_BUTTON_MIN + i. */
typedef enum {
	ZONE_CONTENT, ZONE_TITLEBAR, ZONE_BUTTON_MIN, ZONE_BUTTON_MAX,
	ZONE_BUTTON_FULL, ZONE_BUTTON_CLOSE, ZONE_RESIZE,
	/* The caller's: the run it reserved with fnl_titlebar_reserve(), or a
	 * rectangle it claimed with fnl_titlebar_claim() or fnl_claim(). It
	 * exists so that a press there is not taken for a window drag, nor in
	 * the presentable area for a capture. */
	ZONE_CLIENT_ITEM,
	/* One of the library's own titlebar controls, or a row of an open
	 * drop-down's list. */
	ZONE_CONTROL,
} Zone;

/* The eight resize edges plus "none"; each backend maps them to its server's
 * vocabulary at exactly one place. The type is the public FnlEdge and the
 * short names are its values; EDGE_COUNT is what a backend sizes its table
 * by. */
typedef FnlEdge Edge;
#define EDGE_NONE  FNL_EDGE_NONE
#define EDGE_T     FNL_EDGE_TOP
#define EDGE_B     FNL_EDGE_BOTTOM
#define EDGE_L     FNL_EDGE_LEFT
#define EDGE_R     FNL_EDGE_RIGHT
#define EDGE_TL    FNL_EDGE_TOP_LEFT
#define EDGE_TR    FNL_EDGE_TOP_RIGHT
#define EDGE_BL    FNL_EDGE_BOTTOM_LEFT
#define EDGE_BR    FNL_EDGE_BOTTOM_RIGHT
#define EDGE_COUNT ((Edge)(FNL_EDGE_BOTTOM_RIGHT + 1))

/* ---- the library's titlebar controls ---------------------------------------
 * Three checkboxes and the output, GPU and size drop-downs, in
 * src/core/controls.c. The indices are here because App carries their
 * state. */
enum {
	CTL_CHECK_FSAA = 0, CTL_CHECK_MLAA, CTL_CHECK_SCALE, CTL_CHECK_COUNT
};

/* ---- the caller's run of the titlebar -------------------------------------
 * How tall a control in the titlebar is -- the buttons' height, since a
 * caller's controls line up with them -- and how much clear space the title
 * keeps to its right before the run is refused (see deco_reserved_rect). */
#define DECO_ITEM_H     DECO_BTN_H
#define DECO_TITLE_GAP   8

/* A size in pixels. */
typedef struct { int w, h; } Extent;

/* How many events the queue holds before it starts reporting losses. Sized
 * against the drain rather than the input rate: the contract is one drain a
 * frame, and motion coalesces. Here because App carries the ring inline. */
#define EVENT_QUEUE_MAX 256

/* The inputs one queued event carries, for fnl_input_latency_ms(); see
 * App::ev_born. */
typedef struct { double sum, oldest; uint32_t n, stamped; } EvBorn;

/* An origin and a size: physical pixels in the overlay's interface, window
 * points in the titlebar claims. Prefixed because macOS's MacTypes.h, which
 * every AppKit and Metal header brings in, declares a Rect of its own. */
typedef struct { int x, y, w, h; } FnlRect;

/* The status line's list of outputs. One constant because the published copy
 * and the latched one are memcpy'd into each other, each sized by its own
 * sizeof -- two numbers that drifted apart would read past the shorter. */
#define OUTPUT_NAME_MAX 160

/* The most modes and outputs the public getters and the titlebar's lists
 * read at once; FnlBackendVtbl::modes and ::outputs give at most these. */
#define OUTPUT_MODES_MAX 64
#define OUTPUTS_MAX      8

/* What the [any]-thread knobs have asked the pump to do; see App::req. */
typedef struct {
	bool fullscreen,  fullscreen_val;
	bool maximized,   maximized_val;
	bool minimize;
	bool release_ptr;
	bool size;        int size_w, size_h;
	bool mode;        FnlMode mode_val;
	bool output;      char output_name[OUTPUT_NAME_MAX];
	bool move;
	bool resize;      Edge resize_edge;
	bool bar;         int bar_h, bar_min_w;
	bool cursor;      FnlCursor cursor_val;
	bool text_mode;
	bool clip_get;
	bool nudge_size;  /* the render thread's; app_request_size_nudge() */
	bool title;       /* App::title, which the drain copies under the lock */
} PendingReq;

/* How many drawn frames fnl_frame_samples() keeps unread: seventeen
 * seconds at 60 Hz, a second at 1000 frames a second -- a caller drains
 * once a frame or once a second, and either keeps up. fenestal.h states
 * the number. */
#define FRAME_RING 1024
_Static_assert(FRAME_RING == 1024,
			   "fenestal.h states this number at fnl_frame_samples()");

/* How long a drawn frame the GPU timed waits in that ring for its time
 * (FnlFrameSample::gpu_ms), and holds back the frames after it: as many
 * frames drawn after it, or as long since it was latched, whichever comes
 * first. The renderers read a time when the frame's slot or image is
 * waited for again, or by the poll once its fence has passed, which is
 * two or three frames later; a frame still unread past either is handed
 * over without. GPU_LATE_MAX is how many times read in one turn the
 * render thread keeps for the next publish. fenestal.h states the two
 * holds. */
#define GPU_HOLD_FRAMES 8
#define GPU_HOLD_S      0.25
#define GPU_LATE_MAX    16
/* How many frames' word on whether their GPU time counts is kept
 * (App::gpu_uncounted): well past OVERLAY_MAX_IMAGES, the most frames a
 * renderer has in flight, each read by the time its slot or image is
 * drawn into again. */
#define GPU_UNCOUNTED   32
/* FnlFrameSample::gpu_ms while the frame waits for it; -1 is "none read". */
#define GPU_PENDING     (-2.0f)

/* How many rectangles a caller may claim in one frame, fnl_claim() and
 * fnl_titlebar_claim() together. A fixed array because claims are appended
 * from on_overlay, where an allocation is a stall; a claim past it is
 * dropped with one line through the log sink. 64 because a panel of
 * options claims a row each, where a titlebar claimed a few groups. */
#define CLAIMS_MAX 64
_Static_assert(CLAIMS_MAX == 64,
			   "fenestal.h states this number at fnl_claim() and "
			   "fnl_titlebar_claim()");

/* A scene size of zero: the scene is drawn straight into the window and no
 * offscreen image exists. See DESIGN *Render scale*. */
#define SCENE_NATIVE 0

/* The window's title, with its terminator: fenestal.h states 127 bytes at
 * fnl_set_title(). */
#define TITLE_MAX 128

/* Copy a title into `dst`, "Fenestal" for NULL or empty, cut at a character's
 * start rather than inside one where it is longer than TITLE_MAX - 1. */
void app_title_copy(char dst[TITLE_MAX], const char *utf8);       /* app.c */

/* The device's name, one constant for the same reason: it is copied into a
 * per-thread snapshot. At least what any graphics API reports; each
 * renderer asserts its own figure against it. */
#define GPU_NAME_MAX 256
/* A part of a device's name a caller asks for one by (gpu_name, FNL_GPU),
 * with its terminator. */
#define GPU_WANT_MAX 64

/* One GPU the renderer can draw this window on, as fnl_gpu_count()'s list
 * and the titlebar's GPU box show it. The renderer is the one that drew up
 * the list, and a field so that a list may one day span several. At most
 * GPU_LIST_MAX: a fixed array, since the titlebar reads it from the hit
 * test, where an allocation is a stall. */
#define GPU_LIST_MAX 8
/* The renderers one list may span: every one a build has, which is two on
 * each system (DESIGN *One build per operating system*). */
#define GPU_SPAN_MAX 4
/* A kernel driver's name as tools/gpu-driver.sh spells it, "nouveau" or
 * "nvidia", with its terminator; and a PCI address, "0000:01:00.0". */
#define GPU_DRIVER_NAME_MAX 16
#define GPU_PCI_MAX 16
/* What an entry's frames reach the window through where that is another
 * graphics API's swapchain, "D3D11", with its terminator. */
#define GPU_VIA_MAX 8
typedef struct {
	char        name[GPU_NAME_MAX];
	FnlGpuKind  kind;
	FnlRenderer renderer;
	/* What the frames reach the window through where that is not the
	 * renderer's own swapchain: "D3D11" for Vulkan's through a DXGI one
	 * (src/renderer/vulkan/present_dxgi.c), "" otherwise. A renderer lists
	 * such an entry beside the plain one for the same GPU, so the two can
	 * be chosen apart (fnl_gpu_via_at()); they are ranked as one. */
	char        via[GPU_VIA_MAX];
	/* Of a GPU listed both ways, whether this is the one its renderer
	 * takes by itself: what the ranking measures and moves to, since how
	 * the frames reach the window is the renderer's to choose and not a
	 * frame's cost to say (src/core/rank.c). False for a GPU listed once. */
	bool        way_default;
	/* 0 for a GPU. Otherwise the entry is a move of a GPU to another
	 * kernel driver (FNL_GPU_DRIVER), drawn on after it with `renderer`,
	 * and this is its place in App::gpu_drivers plus one:
	 * src/core/drivers.c. */
	int         driver_move;
	/* While moves are offered, the kernel driver an entry of the GPU that
	 * can move draws on -- the one bound, or a move's -- which the
	 * titlebar's GPU box names; "" for every other entry. Set by
	 * gpu_merge() alone, as driver_move is. */
	char        driver[GPU_DRIVER_NAME_MAX];
	/* Where in the parts the entry came from: App::gpu_span's slot and
	 * its place in that renderer's own list, -1 for a move. The list is
	 * not the parts end to end once a move has swapped the GPU's rows
	 * (app_gpu_driver_entries()), so a row is mapped back by these and
	 * never by counting. Set by gpu_merge(). */
	int         slot, local;
	/* Its measured frame rate where a ranking has one (src/core/rank.c),
	 * 0 where none; set by gpu_merge(). */
	float       fps;
} GpuEntry;

/* A ranked entry as src/core/rank.c keys it, "<renderer key>|<GPU>" with
 * the kernel driver after the GPU where the list names one ("gl|NVIDIA
 * GeForce GTX 1050 Ti on nvidia"), with its terminator; the most figures
 * a ranking keeps, every entry of a list on each of eight displays; and
 * one figure, for an entry on a display, 0 where it would not draw. */
#define RANK_ID_MAX 112
#define RANK_MAX (GPU_LIST_MAX * 8)
/* A display as figures are kept for it, the output and its mode: "HDMI-A-1
 * 3840x2160@30", with its terminator. */
#define RANK_DISPLAY_MAX (OUTPUT_NAME_MAX + 32)
typedef struct {
	char   display[RANK_DISPLAY_MAX];
	char   id[RANK_ID_MAX];
	double fps;
} RankRow;
/* A GPU that can be moved to another kernel driver with the window open,
 * as FnlBackendVtbl::gpu_drivers lists it: its address, the driver bound
 * ("none" for none), the one it can be moved to, and whether that one's
 * module is loaded. At most GPU_DRIVERS_MAX, which is one more than any
 * machine measured has. */
#define GPU_DRIVERS_MAX 2
typedef struct {
	char pci[GPU_PCI_MAX];
	char bound[GPU_DRIVER_NAME_MAX];
	char other[GPU_DRIVER_NAME_MAX];
	bool other_loaded;
} GpuDriver;

/* src/core/drivers.c: DESIGN *Another kernel driver, with the window
 * open*. FNL_GPU_DRIVER read and acted on, from fnl_init() once the window
 * is open and before a renderer is built; the list's entries for the
 * moves, appended to the `n` GPUs of `list` by gpu_merge(), and how many,
 * the GPU's other entries given the driver they draw on; the move an
 * entry asks for, onto `renderer`, owed to the next recovery
 * (gpu_switch_owed()); the move owed taken into `g`, from renderer_swap()
 * with nothing of the renderer built, false where there is none; and that
 * move made there, true where the GPU is on the new driver -- or started
 * on a thread of its own, true where it has (DESIGN *Drawn across the
 * move*), whose end the render thread looks for each turn and then owes
 * the swap onto the moved GPU (app_gpu_driver_poll()). The thread joined
 * at the render thread's end, the move waited for to its own.
 */
void app_gpu_drivers_start(App *a);                            /* drivers.c */
void app_gpu_drivers_pin(App *a, const char *why);             /* app.c */
/* The list made again from the parts as they are, for what the moves
 * offered depend on and the parts do not. */
void app_gpu_list_merge(App *a);                               /* app.c */
/* The whole list said, every entry with its number, the moves included:
 * once the other renderers are listed, and when the ranking orders it
 * again for another display -- what a harness clicks by. */
void app_gpu_list_say(App *a);                                 /* app.c */
int  app_gpu_driver_entries(App *a, GpuEntry *list, int n,
							int max);                          /* drivers.c */
void app_gpu_driver_owe(App *a, int move, const char *name,
						FnlRenderer renderer);                 /* drivers.c */
bool app_gpu_driver_take(App *a, GpuDriver *g);                /* drivers.c */
bool app_gpu_driver_make(App *a, const GpuDriver *g);          /* drivers.c */
bool app_gpu_driver_start_move(App *a, const GpuDriver *g);    /* drivers.c */
void app_gpu_driver_poll(App *a);                              /* drivers.c */
void app_gpu_driver_join(App *a);                              /* drivers.c */
/* After the build that follows a move: whether it draws on the GPU moved,
 * and the move undone -- the GPU back on the driver it left -- where it
 * does not or the build failed. Both nothing where no move was made. */
bool app_gpu_driver_landed(App *a);                            /* drivers.c */
void app_gpu_driver_undo(App *a);                              /* drivers.c */
/* A landing that found no GPU of the moved one's name, the GPU on the
 * driver asked for: every later move there with that renderer would miss
 * the same way, so that one is offered no more -- the driver's other
 * renderers still are, each a userspace of its own. */
void app_gpu_driver_unseen(App *a);                            /* drivers.c */
/* The GPU that can move bound to `driver` before any renderer is built,
 * where a move is offered and it is on the other: what a ranked entry on
 * that driver opens on (app_rank_start()). */
void app_gpu_driver_prefer(App *a, const char *driver);        /* drivers.c */
/* Whether a move is offered, and then the GPU's drivers into `g`: what a
 * ranked key naming a driver needs before this run can make it. */
bool app_gpu_driver_offered(App *a, GpuDriver *g);             /* drivers.c */
/* The kernel driver a GPU's part of a key ends in -- " on nouveau",
 * " on nvidia", as app_gpu_entry_id() writes them -- or NULL. */
const char *app_gpu_driver_of(const char *name);               /* drivers.c */
/* A GPU came or went -- a dock, an external GPU -- as a backend heard it
 * (on any thread): the list made again from every renderer at the render
 * loop's next quiet turn, by a rebuild on the GPU in use that lists the
 * others first (app_gpu_hotplug_poll(), from the render loop). And
 * whether a device path is of the GPU a move to another kernel driver
 * takes away and brings back, whose card comes and goes with each move,
 * which is no hotplug: the move lists again itself. Untested: no GPU
 * here can be plugged (TODO). */
void app_gpu_hotplug(App *a);                                  /* drivers.c */
void app_gpu_hotplug_poll(App *a);                             /* drivers.c */
bool app_gpu_driver_owns(const App *a, const char *devpath);   /* drivers.c */

/* src/core/rank.c: DESIGN *Ranked by measured frame rate*. FNL_GPU_RANK
 * read and the ranking loaded, from fnl_init() before a renderer is built,
 * where an index asked for is moved to the ranked list; an entry's key
 * into `out`, under App::lock; `list`'s `n` entries ordered by the
 * ranking, measured first, and given their figures, from gpu_merge() under
 * App::lock; and on the render thread each turn, the calibration and the
 * move to the ranked entry asked for, once the list is whole. */
void app_rank_start(App *a);                                   /* rank.c */
void app_gpu_entry_id(const App *a, const GpuEntry *e, char *out,
					  size_t size);                            /* rank.c */
void app_rank_sort(const App *a, GpuEntry *list, int n);       /* rank.c */
void app_rank_tick(App *a);                                    /* rank.c */
/* Whether a calibration holds the window's input -- every press, motion,
 * scroll and character dropped, a release let through so nothing stays
 * held -- on any thread; and the calibration's panel over everything the
 * frame drew, from draw_overlay() on the render thread. */
bool app_rank_holds_input(const App *a);                       /* rank.c */
/* Whether the GPU box offers a calibration, and one asked for there: every
 * entry on this display measured again now, and the figures of every other
 * display connected dropped, to be measured when the window is there -- a
 * monitor swapped on a cable at the mode it had keeps its key. Any
 * thread. */
bool app_rank_recalibrates(const App *a);                      /* rank.c */
void app_rank_recalibrate(App *a);                             /* rank.c */
FNL_MUST_USE FnlResult app_rank_draw(App *a, int w, int h);    /* rank.c */

/* renderer.h's, which comes later, named for the prototypes below. */
typedef struct FnlRendererVtbl FnlRendererVtbl;

/* What a renderer's pick found: `n` GPUs, the one in use `cur`, in that
 * renderer's own count, which App::gpu_list places after the GPUs of the
 * renderers before it. Under App::lock. */
void app_gpu_list_publish(App *a, const GpuEntry *list, int n,
						  int cur);                                /* app.c */
/* One line of a pick's inventory, `GPU n: <name> (<kind>)`, GPU `i` of
 * the renderer's own list: held, and said by app_gpu_list_publish() with
 * the number the merged list gives it, naming the renderer beside the kind
 * where the list spans more than one. */
void app_gpu_inventory(const App *a, int i, const char *name,
					   const char *kind);                          /* app.c */
/* A drawn frame's time on the GPU, from its first command to its last, as
 * a renderer read it back from its timestamps: from any thread, folded into
 * the mean on the render thread, by app_frame_cost() after each frame and
 * by app_cost_poll() every turn, which asks the renderer for the times of
 * frames since finished. app_cost_publish() puts the two means where
 * fnl_frame_gpu_ms() and the draw state read them, at its own pace; see
 * src/core/app.c. And the reset that starts the GPU's mean again on a
 * device that was rebuilt. */
#define COST_UNKNOWN 0xffffffffu
/* How often the figures are published while frames are drawn, and how
 * often a sampling window at rest draws one (fnl_set_cost_sampling()):
 * the frame-rate counter's window, so the three figures move together.
 * Seconds. */
#define COST_WINDOW 0.2

/* fnl_set_transition_ms()'s default and ceiling: how long the first frames
 * after a move dissolve from the last frame before it, and, where no frame
 * could be read back, how long the veil takes to come down to black ahead
 * of the move and to lift after it. Milliseconds. Short enough not to read
 * as a delay, long enough to read as a fade; past the ceiling a figure is a
 * move that never comes. */
#define VEIL_MS     250
#define VEIL_MS_MAX 5000

/* What fnl_input_latency_ms() takes a window server's or a device's stamp
 * on an input to be (app_input_age()). An age past INPUT_AGE_MAX is a clock
 * the library does not share, and the input counts from when the pump read
 * it; one below zero by no more than INPUT_AGE_SLACK is the stamp's own
 * rounding, and reads as zero. A stamp is the input's that the backend
 * hands in next, within INPUT_STAMP_FRESH -- one handler's work, so that an
 * input with no stamp of its own never takes another's. Seconds. */
#define INPUT_AGE_MAX     1.0
#define INPUT_AGE_SLACK   0.002
#define INPUT_STAMP_FRESH 0.002
/* The GPU's time on a frame, by the renderer that timed it: the frame
 * being drawn says so with app_gpu_timed(), which returns its index
 * (FnlFrame::index), and the renderer hands that back with the time once
 * it reads it, frames later. Render thread. */
uint64_t app_gpu_timed(App *a);                                     /* app.c */
void app_gpu_time(App *a, double seconds, uint64_t index);         /* app.c */
void app_frame_cost(App *a);                                       /* app.c */
void app_cost_poll(App *a);                                        /* app.c */
void app_cost_publish(App *a, double now, bool window);            /* app.c */
void app_frame_cost_reset(App *a);                                 /* app.c */

/* Where a frame's CPU time is read, by every renderer, on the render
 * thread: the CPU half of the cost (fnl_frame_cpu_ms()) is the render
 * thread's CPU time from the end of its waits to the return of the call
 * that hands the frame over, so the present's own work is in it and a
 * wait that blocks, inside the recording or the present, is not (DESIGN
 * *What a frame costs, compared*). FRAME_MARK_REC again where the frame
 * waited after its first starts the span there. The same marks feed
 * FNL_COST_PARTS. app_cpu_cost() is the figure the cost publishes and the
 * ranking samples: that mean, or the wall clock over late_cost's span
 * where the thread's CPU time is not known (App::wall_cost). */
typedef enum FrameMark {
	FRAME_MARK_REC,       /* past the frame's waits: where late_cost starts */
	FRAME_MARK_PRESENT,   /* the call that hands the frame over, entered */
	FRAME_MARK_PRESENTED, /* and returned from */
} FrameMark;
void app_frame_mark(App *a, FrameMark m);                          /* app.c */
double app_cpu_cost(const App *a);                                 /* app.c */

/* FNL_COST_PARTS=1 beside --frame-times: what a frame's two cost figures
 * are made of, a line or three after each frame line -- the render
 * thread's CPU time over the spans the line gives in wall time, the GPU's
 * span in the parts a renderer stamps, and on Linux each thread's CPU per
 * frame. Kept from docs/comparable-frame-costs.md's Phase 0 to answer why
 * an entry ranks where it does. App::parts is NULL when off, and every
 * entry then returns at once. Render thread, but for parts_open() and
 * parts_close(). parts_mark() is app_frame_mark()'s, with the thread's CPU
 * time it read (negative where not known, or not read). */
#define PARTS_GPU_MAX 8
void parts_open(App *a);                                           /* parts.c */
void parts_close(App *a);                                          /* parts.c */
void parts_mark(App *a, FrameMark m, double cpu);                  /* parts.c */
/* One GPU sample cut into `n` parts, the first the whole span; `names` is
 * static and the renderer's, and a new one starts the sums again. */
void parts_gpu(App *a, const char *const *names, const double *seconds,
			   int n);                                             /* parts.c */
void parts_log(App *a);                                            /* parts.c */
/* One period sample, its four parts (held, device, present, cost), and
 * whether the estimator took it: what the frame line's `period` is made
 * of. */
void parts_period(App *a, const double s[4], bool taken);          /* parts.c */
void parts_reset(App *a);                                          /* parts.c */

/* The other renderers' GPUs, listed once and placed in App::gpu_list: the
 * listing, and the thread the render thread starts it on after the first
 * frame. */
void app_gpu_list_others(App *a);                                  /* app.c */
void app_gpu_list_start(App *a);                                   /* app.c */
/* "discrete", "software", "hardware" for an unknown kind: the word the
 * inventory and the titlebar's list give a kind. And a renderer's name,
 * "OpenGL", and key, "gl", from the build's tables. */
const char *app_gpu_kind_word(FnlGpuKind k);                       /* app.c */
const char *app_renderer_name(FnlRenderer r);                      /* app.c */
const char *app_renderer_key(FnlRenderer r);                       /* app.c */
/* Where App::gpu_list's entry `i` comes from: the renderer (gpu_span's)
 * and its own 0-based index there; false past the end. Takes App::lock. */
bool app_gpu_entry_of(App *a, int i, const FnlRendererVtbl **r,
					  int *local);                                 /* app.c */
/* `name` cut short enough for a title, as fnl_gpu_short_name() cuts the
 * name of the one in use: into `out`, or `name` itself where nothing would
 * be left. */
const char *app_gpu_short(char *out, size_t size,
						  const char *name);                       /* app.c */
/* Whether `part` is somewhere in `name`, ASCII letter case aside: how every
 * renderer's pick reads FnlInitDesc::gpu_name and FNL_GPU, and how a move
 * to another kernel driver finds the GPU moved by the name it keeps. The
 * drivers spell the names, and nobody types "Intel(R)": a name matches
 * with the vendors' marks in it or left out. */
bool app_name_has(const char *name, const char *part);             /* app.c */

/* Where the veil is (App::veil_phase, veil_step() in frame.c): drawn down
 * over the last frames before a move the library makes, black through the
 * rebuild, and lifted over the first frames after it. DESIGN *The veil*. */
typedef enum {
	VEIL_NONE,    /* nothing drawn */
	VEIL_SNAP,    /* the next frame drawn reads itself back */
	VEIL_OUT,     /* coming down to black ahead of a move */
	VEIL_HELD,    /* black; the move is made once a frame of it is shown */
	VEIL_ARMED,   /* black on the rebuilt device until its first present */
	VEIL_IN,      /* lifting after it */
} VeilPhase;

/* Everything the command-buffer recording depends on: the render thread
 * re-records only when this changes (see src/core/frame.c).
 *
 * Three kinds of thing belong here: what the library draws (the titlebar's
 * own inputs), what it knows and draws (the outputs, the reticle, the metrics
 * panel's figures), and what the frame is drawn from (the scene's size and
 * antialiasing, its time, the veil). Everything a caller *can* observe is
 * covered by `user_gen`, which fnl_invalidate() bumps.
 *
 * Prefixed because <windows.h> declares DrawState. */
typedef struct {
	bool     is_fullscreen;
	bool     is_active;       /* the window has the keyboard focus */
	bool     is_maximized;    /* picks the restore icon over the maximize one */
	bool     pointer_captured;  /* locked; the caller draws its own cursor */
	/* Whether the caller paints the bar this frame. Latched beside bar_h:
	 * fnl_titlebar_custom() changes both, and the pair has to cross to the
	 * render thread together. */
	bool     custom_bar;
	/* The reticle, in *pixels of the presentable area* -- App::cur_px's unit,
	 * not logical points. Here only so that a moved reticle re-records. */
	int      cursor_x, cursor_y;
	/* The outputs the surface is on, the one being followed first. */
	char     output[OUTPUT_NAME_MAX];
	/* The title the bar shows, latched so the render thread draws its own
	 * copy and fnl_set_title() re-records the bar. */
	char     title[TITLE_MAX];
	Zone     hovered_button;  /* ZONE_BUTTON_*, or ZONE_CONTENT for none */
	/* The titlebar band this frame is laid out against, in logical points, or
	 * 0 in fullscreen. Latched with the size. */
	int      bar_h;
	int      out_w_pt, out_h_pt;   /* the output, in logical points */
	/* What fnl_fps() reports. It must stay here: a caller can compare the
	 * reading against the one it drew only inside on_overlay, which runs only
	 * when the frame is already being re-recorded. Moves with the window's
	 * frames, and at rest only while fnl_set_cost_sampling() is on, in the
	 * same turn as the two below, so the three cost one re-record. */
	int      fps;
	/* What fnl_frame_gpu_ms() and fnl_frame_cpu_ms() report, in
	 * microseconds, for fps's reason. Published at most once a COST_WINDOW
	 * while frames are drawn, and once more when the window comes to rest
	 * (app_cost_publish()), so they cost no more re-records than fps. */
	uint32_t cost_gpu_us, cost_cpu_us;
	/* What fnl_input_latency_ms() reports, in microseconds, for fps's
	 * reason, and published with the two above (events_latency_publish()). */
	uint32_t input_lat_us;
	/* The metrics panel shown (fnl_set_metrics_panel()), and the process's
	 * figures it shows, as App::met_* has them: CPU in tenths of a percent
	 * of one core, the two memories in MiB, and the GPU's time its frames
	 * took in tenths of a percent, each METRICS_UNKNOWN where the system
	 * will not say. Moved at most once a METRICS_WINDOW, and only while the
	 * panel is shown, so they cost no more re-records than fps. */
	bool     metrics;
	uint32_t met_cpu_pm, met_ram_mib, met_vram_mib, met_gpu_pm;
	/* The scene's own resolution in pixels, or SCENE_NATIVE. Here because
	 * the letterbox rectangle and the fetch that fills it are recorded, not
	 * pushed. */
	int      scene_w, scene_h;
	/* The antialiasing mode in force (App::want_aa): the one asked for, as
	 * far as the device drawing can take it -- unlike the pair above, not
	 * read back from what was built. */
	FnlAntialias aa;
	/* Seconds into the scene's animation, or 0 while motion is off. Being in
	 * here is what re-records a moving scene. */
	float    scene_time;
	/* The veil over a move to another GPU, renderer or kernel driver: 0
	 * for none, 1 for black, in steps of 1/255 so that each step of a fade
	 * is one re-record and a veil that stands is none (veil_step() in
	 * frame.c, drawn by draw_veil() in draw.c). */
	float    veil;
	/* Bumped by fnl_invalidate(): everything on_record and on_overlay draw
	 * from that the library cannot see. */
	uint32_t user_gen;
} FnlDrawState;

/* Compared field by field rather than with memcmp: the struct has padding
 * bytes that are never written. A field left out of fnl_draw_state_eq() is an
 * overlay that stops refreshing when that field alone changes, so the size is
 * pinned and adding a field fails the build here. Update the comparison,
 * then the number. */
_Static_assert(sizeof(FnlDrawState) == 380, "FnlDrawState changed -- keep "
			   "fnl_draw_state_eq() in step, then update this size");

static inline bool fnl_draw_state_eq(const FnlDrawState *x,
									 const FnlDrawState *y)
{
	return x->is_fullscreen  == y->is_fullscreen  &&
		   x->is_active      == y->is_active      &&
		   x->is_maximized   == y->is_maximized   &&
		   x->pointer_captured == y->pointer_captured &&
		   x->custom_bar     == y->custom_bar     &&
		   x->cursor_x       == y->cursor_x       &&
		   x->cursor_y       == y->cursor_y       &&
		   !strcmp(x->output, y->output)          &&
		   !strcmp(x->title, y->title)            &&
		   x->hovered_button == y->hovered_button &&
		   x->bar_h          == y->bar_h          &&
		   x->out_w_pt       == y->out_w_pt       &&
		   x->out_h_pt       == y->out_h_pt       &&
		   x->fps            == y->fps            &&
		   x->cost_gpu_us    == y->cost_gpu_us    &&
		   x->cost_cpu_us    == y->cost_cpu_us    &&
		   x->input_lat_us   == y->input_lat_us   &&
		   x->metrics        == y->metrics        &&
		   x->met_cpu_pm     == y->met_cpu_pm     &&
		   x->met_ram_mib    == y->met_ram_mib    &&
		   x->met_vram_mib   == y->met_vram_mib   &&
		   x->met_gpu_pm     == y->met_gpu_pm     &&
		   x->scene_w        == y->scene_w        &&
		   x->scene_h        == y->scene_h        &&
		   x->aa             == y->aa             &&
		   x->scene_time     == y->scene_time     &&
		   x->veil           == y->veil           &&
		   x->user_gen       == y->user_gen;
}

/* The metrics panel (fnl_set_metrics_panel()). The process's figures -- CPU,
 * resident memory, GPU memory, the GPU's share -- sampled on the render thread
 * while the panel is shown, at most once a METRICS_WINDOW, into App::met_*,
 * which the draw state latches: after the present where the window draws, and
 * before the draw state only at rest, where no hold is run (the render loop in
 * frame.c); and the panel itself, from draw_overlay() after the caller's
 * drawing, from the draw state's figures. */
#define METRICS_WINDOW 0.5          /* seconds between process samples */
#define METRICS_UNKNOWN 0xffffffffu  /* a figure the system will not say */
void app_metrics_sample(App *a, double now);                 /* metrics.c */
FNL_MUST_USE FnlResult app_metrics_draw(App *a, const FnlDrawState *ds,
										int w, int h);       /* metrics.c */

/* The antialiasing mode the caller asked for, worked out again for the
 * device just built: from each on_gpu_created's site, ahead of the
 * callback, so the caller reads the mode the new device can draw. Any
 * thread. */
void app_aa_settle(App *a);                                /* api_state.c */

/* fnl_key_down()'s set: one bit per FnlKey, in words because the atomics shim
 * has no wider type. Sized past the enum, so appending a key is not also an
 * edit here until the assert says so. */
#define KEY_HELD_WORDS 4
_Static_assert(FNL_KEY_KP_ENTER < 32 * KEY_HELD_WORDS,
			   "FnlKey has outgrown the held set -- raise KEY_HELD_WORDS");

/* ---- the overlay's three primitives ---------------------------------------
 * Solid rectangles, antialiased strokes and glyphs. Each is one instance in a
 * per-image vertex buffer, and each *kind* is one draw for the whole frame --
 * legal because every filled rectangle is a *background*; see DESIGN
 * *Recording*.
 *
 * Each struct must match the `layout(location=...) in` block of its vertex
 * shader; the asserts pin every offset padding could move. */

/* One solid rectangle. Written, not blended. */
typedef struct {
	float rect[4];      /* x, y, w, h -- physical pixels                     */
	float color[4];     /* straight RGBA, already converted for the surface  */
} RectInstance;

_Static_assert(sizeof(RectInstance) == 32, "RectInstance must match rect.vert");
_Static_assert(offsetof(RectInstance, color) == 16,
			   "RectInstance layout drifted");

/* One stroke: a capsule from p0 to p1, or two meeting at p1 and ending at p2.
 * The fragment shader takes the nearer of the two (see shaders/shape.frag). */
typedef struct {
	float p0[2], p1[2];   /* spine, physical pixels                          */
	float p2[2];          /* second arm's end; == p1 for a plain line        */
	float half_w;         /* half the stroke width, pixels                   */
	float feather;        /* coverage ramp half-width, pixels                */
	float color[4];
} ShapeInstance;

_Static_assert(sizeof(ShapeInstance) == 48,
			   "ShapeInstance must match shape.vert");
_Static_assert(offsetof(ShapeInstance, p2) == 16,
			   "ShapeInstance layout drifted");
_Static_assert(offsetof(ShapeInstance, color) == 32,
			   "ShapeInstance layout drifted");

typedef struct {
	float rect[4];      /* x, y, w, h -- physical pixels                     */
	float uv[4];        /* u0, v0, u1, v1 -- normalised atlas coordinates    */
	float color[4];     /* straight RGBA, already converted for the surface  */
} GlyphInstance;

_Static_assert(sizeof(GlyphInstance) == 48,
			   "GlyphInstance must match text.vert");
_Static_assert(offsetof(GlyphInstance, uv) == 16,
			   "GlyphInstance layout drifted");
_Static_assert(offsetof(GlyphInstance, color) == 32,
			   "GlyphInstance layout drifted");

/* Swapchain images the instance buffers, and every per-image array of the
 * swapchain, are sized for. Each image records its own command buffer and so
 * needs its own instance data. The buffers are device-scoped, not
 * swapchain-scoped: a rebuild keeps the old command buffers alive until they
 * are provably idle.
 *
 * create_swapchain() refuses an image count above this rather than letting the
 * index wrap, which would put two frames in flight on one buffer. */
#define OVERLAY_MAX_IMAGES 8
_Static_assert(GPU_UNCOUNTED > 2 * OVERLAY_MAX_IMAGES,
			   "a GPU time is read within the frames in flight, and its "
			   "frame's word in App::gpu_uncounted must outlast them");

/* Ceilings per frame. An append past one draws nothing and reports nothing,
 * so they are sized for a caller: the library's own bar spends about 800
 * glyphs, thirty rectangles and twenty strokes. Rectangles get the room they
 * do because a caller spends one per panel, row and cell. The eight images
 * come to 1.9 MB of host-visible memory, in two allocations. */
#define TEXT_MAX_GLYPHS     4096
#define OVERLAY_MAX_RECTS   1024
#define OVERLAY_MAX_SHAPES   512

/* The two non-text streams share one allocation per image: rectangles from
 * offset 0, strokes from OVERLAY_SHAPE_OFF. src/core/overlay.c appends and
 * the renderer draws, so both read the offset here. */
#define OVERLAY_SHAPE_OFF   ((size_t)OVERLAY_MAX_RECTS * sizeof(RectInstance))
#define OVERLAY_ARENA_BYTES (OVERLAY_SHAPE_OFF + \
							 (size_t)OVERLAY_MAX_SHAPES * sizeof(ShapeInstance))
#define TEXT_ARENA_BYTES    ((size_t)TEXT_MAX_GLYPHS * sizeof(GlyphInstance))
/* The strokes are cast in place out of the mapped arena, so their offset has
 * to keep a ShapeInstance aligned; the mapping itself is (Vulkan's
 * minMemoryMapAlignment is at least 64). */
_Static_assert(OVERLAY_SHAPE_OFF % FNL_ALIGNOF(ShapeInstance) == 0,
			   "the stroke run must start ShapeInstance-aligned");

/* How far from the origin a logical-point figure a caller chose may be. Not a
 * limit on what may be drawn, but the bound that keeps scale_pt()'s
 * multiplication from wrapping: 2^19 points times the largest scale fits an
 * int. Every figure a caller hands over in points takes it through
 * clamp_pt(), and src/core/overlay.c bounds how far the text pen may walk,
 * which no clamp on a coordinate reaches. */
#define OVERLAY_COORD_MAX (1 << 19)

/* ---- the scale, bounded ----------------------------------------------------
 * The range a surface scale may take, in 120ths, wherever one arrives from
 * outside: FnlInitDesc::scale_override120, Xft.dpi on X11, the compositor's
 * preference on Wayland, the window's DPI on Win32. Every writer takes this
 * bound.
 *
 * The ceiling is --scale's own 8.0 (DESIGN *Scale*). The floor is 60 rather
 * than 120 because the library itself may arrive there:
 * clamp_device_scale120() cuts a scale to fit the device's largest buffer. */
#define SCALE120_MIN 60
#define SCALE120_MAX 960

/* ---- the titlebar's palette ------------------------------------------------
 * Authored in sRGB and converted once into App::pal when the surface format
 * is known: an _SRGB swapchain encodes on write, so the values are linearised
 * first, and UNORM takes them as-is. Only what the library paints; a
 * caller's colours are converted per call by api_overlay.c. */
typedef enum {
	COL_BAR_ON,         /* titlebar, focused / unfocused                    */
	COL_BAR_OFF,
	COL_LINE_ON,        /* window outline and button hover                  */
	COL_LINE_OFF,
	COL_ICON_ON,        /* button icons, focused / unfocused                */
	COL_ICON_OFF,
	COL_CLOSE,          /* hover behind the close button                    */
	COL_CTL_BOX,        /* a control's own background                       */
	COL_CTL_DIM,        /* a control that cannot act, or an unfocused bar   */
	COL_CTL_LIVE,       /* a tick, and the row already picked               */
	COL_CTL_HILITE,     /* the list row under the pointer                   */
	COL_MET_HEAD,       /* the metrics panel's headings                     */
	COL_MET_SLOW,       /* the slower half of a frame's cost                */
	COL_MET_RATE,       /* the rate the slower half allows                  */
	COL_COUNT
} OverlayColor;

/* One baked glyph's place and offset, all integers, so that the quad lands on
 * the whole pixels FreeType computed the coverage for.
 *
 * Two units: the atlas is three texels wide per pixel, so `x` indexes texels,
 * `w` counts pixels, and the cell occupies 3*w texels starting at x.
 * Everything else is pixels or rows. */
typedef struct {
	int16_t x, y;            /* cell origin: x in TEXELS, y in rows          */
	int16_t w, h;            /* cell size:   w in PIXELS, h in rows          */
	int16_t left, top;       /* from the pen on the baseline, y down, pixels */
	int16_t adv;             /* pen step to the next glyph, PIXELS           */
} GlyphCell;

/* The layout is a contract between three programs: tools/mkfont writes the
 * blob as seven int16 in this order, tools/verifyfont reads it back through
 * its own copy of this declaration, and text_load_cells() decodes the bytes
 * into an array of these. The order is pinned as well as the size. */
_Static_assert(sizeof(GlyphCell) == 14, "the blob is 7 int16 per cell");
_Static_assert(offsetof(GlyphCell, x)    == 0  &&
			   offsetof(GlyphCell, y)    == 2  &&
			   offsetof(GlyphCell, w)    == 4  &&
			   offsetof(GlyphCell, h)    == 6  &&
			   offsetof(GlyphCell, left) == 8  &&
			   offsetof(GlyphCell, top)  == 10 &&
			   offsetof(GlyphCell, adv)  == 12,
			   "GlyphCell's field order is the baked blob's -- move one and "
			   "tools/mkfont's emitter and tools/verifyfont's copy move too");

/* The two seams. After FnlDrawState, which the renderer table names; before
 * App, which holds a pointer to each. */
#include "core/backend.h"
#include "core/renderer.h"

/* Tagged `FnlApp` and typedef'd `App`: `FnlApp` is what the installed header
 * declares and never defines, `App` the spelling used inside. */
struct FnlApp {

	/* ---- set once -----------------------------------------------------
	 * Written by fnl_init() or fnl_run() before the render thread exists,
	 * and only read after, by either thread. */

	const FnlBackendVtbl  *backend;
	/* The exception to "set once": a move to another renderer's GPU
	 * writes it, on the render thread (renderer_swap()), which alone reads
	 * it while it runs. Any other thread reads renderer_pub
	 * (renderer_any()). */
	const FnlRendererVtbl *renderer;

	/* Allocated by backend->open(). Anything only one backend needs
	 * belongs there rather than here. */
	struct BackendState  *backend_state;
	/* Allocated by renderer->create_instance() and freed by destroy_instance().
	 * Everything only the graphics API needs -- its device, swapchain,
	 * pipelines and targets -- is there, and nothing outside src/renderer/<g>/
	 * reads it. */
	struct RendererState *renderer_state;

	/* --scale=, in 120ths, or 0 for the backend's own preference. */
	int                   scale_override120;
	/* --gpu=, one-based into the list of devices that can present, or 0 for
	 * the automatic ranking. */
	int                   gpu_index;
	/* Or by name: a part of the device's, empty for none. Wins over the
	 * index. */
	char                  gpu_want[GPU_WANT_MAX];

	/* The caller's callbacks, copied by fnl_run(). */
	FnlCallbacks          cb;
	/* When the window opened, so FnlFrame::time and FnlEvent::time start near
	 * zero. Set in fnl_init() immediately before the backend's open(),
	 * which can already queue events. */
	double                started_at;

	/* The window's title, from FnlInitDesc at fnl_init and fnl_set_title()
	 * after it, written under App::lock; the frame latches its own copy
	 * (FnlDrawState::title) and the pump's drain another. title_w is what
	 * the *latched* copy measures to in logical points: taken at fnl_init
	 * and by the render thread when the latched title changes, an atomic
	 * because the hit test reads it too (see deco_title_fits). */
	char                  title[TITLE_MAX];
	FnlAtomicU32          title_w;
	/* The presentable area asked for at fnl_init, in logical points, or 0 for
	 * the library's own default. */
	int                   init_w_pt, init_h_pt;

	/* FnlInitDesc::echo_events, or FNL_ECHO_EVENTS in the environment. */
	bool                  echo_events;
	/* Whether a handler on the process's terminating signals could still
	 * reach this app (FnlInitDesc::install_signal_handlers). The handler
	 * reaches app_wakeup(), so while this is set the backends leave their
	 * wakeup descriptors open rather than risk a write to a recycled number.
	 * app_teardown() clears it once it has waited every handler out, which
	 * is every teardown but one whose handler was stopped mid-way. */
	bool                  installed_signals;

	/* ---- pump thread ---------------------------------------------------
	 * The thread that called fnl_init() and runs fnl_run(): it owns the
	 * window server. Nothing here is read by the render thread except where
	 * a field says which lock carries it across. */
	_Alignas(64)
	bool                  pointer_captured;
	/* Where the reticle is, in pixels of the presentable area. Kept here
	 * rather than derived from the pointer, which does not move while the
	 * pointer is captured. */
	double                cur_px, cur_py;

	/* The shape fnl_set_cursor() last asked for: the call records into
	 * App::req and the drain stores it here, so the backends' pointer paths
	 * read it without a lock. */
	FnlCursor             cursor;
	double                ptr_x, ptr_y;
	uint32_t              last_titlebar_click_ms;

	/* A press on the titlebar that has not yet travelled DECO_DRAG_SLOP, so
	 * it is still either a click or the start of a move. */
	bool                  titlebar_armed;
	double                titlebar_press_x, titlebar_press_y;

	bool                  configured;

	/* Current toplevel states, as of the configure that applied them. The
	 * render thread draws from FnlDrawState::is_fullscreen instead, which is
	 * latched with the size it belongs to. */
	bool                  is_fullscreen;
	bool                  is_maximized;
	int                   win_w, win_h;
	int                   cur_w, cur_h;

	/* What a window has to fit inside to be shown whole, in logical points --
	 * what a caller's size list has to be filtered against. See
	 * refresh_size_bounds. */
	int                   out_w_pt, out_h_pt;

	/* The titlebar, whoever draws it: its height in logical points, and who
	 * paints it -- DECO_TITLEBAR_H and false for the library's own bar,
	 * until fnl_titlebar_custom() replaces both. Written only here, by
	 * deco_apply_custom() under App::lock; the render thread latches the
	 * pair into FnlDrawState under the same lock.
	 *
	 * deco_min_w is the width floor a custom bar asked for, or 0. */
	int                   deco_h;
	int                   deco_min_w;
	bool                  deco_custom;

	/* What this window is offering the desktop's clipboard. The pump's own
	 * from the moment it takes it out of clip_pending: serving it to another
	 * program may happen at any time after. */
	char                 *clip_out;
	size_t                clip_out_len;

	/* The input the backend hands in next, as app_input_age() or
	 * app_input_ages() stamped it: when each was made, on FnlEvent::time's
	 * clock, and when the stamp was taken, on now_seconds()'s; 0 for
	 * none. */
	EvBorn                in_born;
	double                in_stamped_at;

	/* Set once create_instance() has succeeded, so teardown -- which is
	 * also fnl_init()'s failure path -- knows whether there is a renderer
	 * to unwind. */
	bool                  renderer_ready;
	/* Whether the render thread has been started, and whether the three
	 * mutexes and the condition variable have been made, so that teardown --
	 * which is also fnl_init()'s failure path -- knows whether there is
	 * anything to join or destroy. */
	bool                  render_thread_started;
	bool                  sync_ready;
	FnlThread             render_thread;

	/* ---- render thread -------------------------------------------------
	 * The frame policy (src/core/frame.c), the overlay's streams and the
	 * renderer. The main thread writes these only before fnl_run()
	 * starts the render thread and after it has joined it. */
	_Alignas(64)
	uint32_t              frame;
	bool                  resized;
	/* When a toplevel state change was last answered; see TRANSITION_GRACE. */
	double                transition_at;

	uint64_t              presents;        /* successful presents, all time */
	/* Presents made, whatever they returned, raised by the renderer
	 * once the call is back: what a backend's ack that must follow the
	 * present compares against, since app_surface_sync() can flush before
	 * it and a present that failed must release the ack too. Render
	 * thread. */
	uint64_t              present_calls;
	/* The frame that acks a configure has been presented and is waited for
	 * until shown on a thread of the renderer's, which calls
	 * FnlBackendVtbl::ack_release when it is; the backend's flush keeps
	 * the ack meanwhile. Set by the render thread, cleared by that one. */
	FnlAtomicBool         ack_held;
	double                latch_time;      /* when this frame took its state */
	/* How long the last frame's *presentation engine* made the CPU wait, in
	 * seconds. Negative when the last turn did not reach a present, or when
	 * the renderer cannot measure it, which leaves late latching inert there.
	 * It is the late-latch signal where the window server does not invite
	 * frames; see src/core/frame.c.
	 *
	 * Vulkan adds the acquire's block to the present call's. A wait on the
	 * device's own fences must *not* be counted: that is the GPU being
	 * behind, and a delay that spent it would push the next submit later
	 * still. */
	double                present_block;
	/* The frame loop's hold on the queue ahead of the screen
	 * (FnlBackendVtbl::display_waits, frame.c): how long this turn's wait
	 * took, which the period estimate counts; how many frames may wait,
	 * and the misses that put it back up; when the last wait came back and
	 * after which present; and when the deeper queue is let go again.
	 * Render thread. */
	double                display_block;
	/* The index and one of the last frame the window server answered for,
	 * shown or skipped (app_frame_answered(), the main thread's), 0 for
	 * none; and whether the hold stopped waiting for an answer that did not
	 * come, and what had come then. */
	FnlAtomicU32          frame_answered;
	bool                  display_unanswered;
	uint32_t              display_unanswered_at;
	int                   display_ahead;
	int                   display_misses;
	double                display_last;
	uint64_t              display_last_presents;
	double                display_retry_at, display_retry_gap;
	/* Whether this device's present can never be measured: a software
	 * device's waits for its own rasteriser, not the display, so the
	 * renderer leaves present_block negative for good. Set by the renderer
	 * each frame where it resets present_block, from what it already
	 * knows of the device; read by the spacing, for which "no period yet"
	 * means "space every turn" here and "let the swapchain throttle"
	 * everywhere else (pace_period()). Render thread. */
	bool                  present_untimed;
	/* And how long it waited on the device's fences in the same turn. No
	 * spare time, so never part of the figure above; but part of the
	 * *period*, because a driver may hold a FIFO swapchain to the display
	 * there and block in neither the acquire nor the present. Read only on
	 * a turn whose present_block is a measurement. */
	double                device_block;

	/* Late latching: waiting out most of the display period before taking the
	 * frame's state. All policy; see src/core/frame.c and DESIGN *Late
	 * latching*. */
	bool                  late_latch;
	double                late_fixed;
	double                late_delay;
	double                late_period;
	double                late_ceiling;
	double                late_prev_disp;
	int                   late_cooldown;
	int                   late_short;
	int                   late_recent;
	double                late_cost;
	/* The render thread's CPU time over a frame, the running mean in
	 * seconds (app_frame_mark()), 0 until known; the reading at the
	 * frame's FRAME_MARK_REC, and whether it was taken; and the clock, whose
	 * rate is state on Windows. Render thread. */
	double                cpu_cost;
	double                cpu_mark;
	bool                  cpu_marked;
	FnlCpuClock           cpu_clock;
	/* Where the thread's CPU time is not known: the wall clock over the
	 * same span, FRAME_MARK_REC to FRAME_MARK_PRESENT, as late_cost has it
	 * but over the frames that count (App::cost_counts) alone -- late_cost
	 * is the pacing's and takes every frame. The running mean in seconds,
	 * 0 until known, and the clock at the frame's FRAME_MARK_REC, negative
	 * where none was taken. Render thread. */
	double                wall_cost;
	double                wall_mark;
	/* The GPU's running mean over drawn frames, in seconds, and the sample
	 * it last took in (gpu_sample_seq's), on the render thread; 0 until the
	 * device in use has given one. */
	double                gpu_cost;
	uint32_t              gpu_cost_seq;
	/* When the two figures were last published (cost_*_us), whether frames
	 * drawn since have moved them, and whether the frames being drawn now
	 * count: not while the window draws only to show the figures, or each
	 * such frame would owe the next (app_cost_publish()) and be in the
	 * means it shows (app_gpu_time(), app_frame_mark()). Render thread. */
	double                cost_published_at;
	/* When fnl_input_latency_ms()'s figure was last published
	 * (events_latency_publish()). Render thread. */
	double                lat_published_at;
	bool                  cost_owed;
	bool                  cost_counts;
	/* When a frame was last drawn, which a sampling window at rest draws
	 * the next one a COST_WINDOW after. Render thread. */
	double                cost_sampled_at;
	int                   late_stable;
	/* What the hold this turn cost, or negative when the turn took none and
	 * its timings say nothing about spare time: a rebuild, a tearing frame,
	 * an uninvited one. A measurement only together with present_block. */
	double                late_held;
	/* What a turn costs with nothing held -- hold, block and cost summed
	 * over turns whose hold was zero -- how many have been measured, and
	 * the same sum averaged over the held ones. The measured controller
	 * reads the second against the first; see late_latch_slack(). Where
	 * frames are invited, late_turn is the invitations' gaps averaged, read
	 * against late_period; see late_latch_feedback(). Seconds. */
	double                late_base;
	int                   late_base_n;
	double                late_turn;
	/* Frames left to ignore after a swapchain rebuild, whose empty queue
	 * reads as a period with no slack in it. See LATE_SLACK_SETTLE. */
	int                   late_settle;

	/* The display period as this loop measures it, when the last rebuild turn
	 * was let through, and when the last vsynced turn was. Zero until enough
	 * throttled frames have gone by to have measured one. pace_at is the
	 * anchor while late_settle says the presentation engine is not
	 * throttling, rebuild_at the rest of the time. Read only where
	 * the backend does not invite; see disp_period_sample() and hold_wait()
	 * in src/core/frame.c. */
	double                disp_period;
	/* The period a new window is spaced by until it has settled: the run of
	 * early samples that agree, how many there are, and whether disp_period
	 * is still that run's and not yet a settled window's. See
	 * disp_period_early(). */
	double                disp_try;
	int                   disp_try_n;
	bool                  disp_early;
	/* An estimate under PACE_UNTHROTTLED of the refresh, put right: the
	 * run of samples it drops that agree (disp_long, its mean, and how
	 * many), and the probe that lets samples come while the spacing holds
	 * every turn -- turns of a burst left, turns to the next, and the gap.
	 * Render thread's. See disp_period_sample() and period_probe(). */
	double                disp_long;
	int                   disp_long_n;
	int                   probe_left, probe_wait, probe_gap;
	/* App::resumes as the render thread last acted on it; see
	 * frame_resumed(). */
	uint32_t              resumes_seen;
	/* App::surface_remakes, likewise; see render_loop(). */
	uint32_t              surface_remakes_seen;
	/* A move to another GPU under way (gpu_switch_owed()): the rebuild it
	 * raised is not a loss and is not said as one. gpu_switch_back is the
	 * move back to the GPU before, after the new one would not build, and
	 * gpu_back_* the choice that GPU was made by. */
	bool                  gpu_switching;
	bool                  gpu_switch_back;
	int                   gpu_back_index;
	char                  gpu_back_want[GPU_WANT_MAX];
	/* A move to another renderer's GPU owed (gpu_switch_owed()), which the
	 * next recovery makes as a swap (renderer_swap()); and the renderer a
	 * swap and its way back both failed to build, tried again each turn
	 * until it builds (renderer_owed_retry()). Render thread's. */
	const FnlRendererVtbl *renderer_next;
	const FnlRendererVtbl *renderer_owed;
	/* The other renderers' GPUs are being or have been listed, and the
	 * thread that lists them (app_gpu_list_start()), joined with the
	 * render thread. Listed again once a move to another kernel driver
	 * has landed, never while one runs: what the GPU is called, and which
	 * renderers can draw on it, is its driver's (renderer_swap()). */
	bool                  gpu_others_listed;
	bool                  lister_started;
	/* The lister's last write, done listing: lister_started stays until
	 * the thread is joined. */
	FnlAtomicBool         lister_done;
	FnlThread             lister;
	/* The GPUs that can be moved to another kernel driver (FNL_GPU_DRIVER,
	 * src/core/drivers.c): listed by fnl_init() and again after each move,
	 * under App::lock, which gpu_merge() reads them under on any thread.
	 * gpu_drivers_on is whether a move is offered at all -- FNL_GPU_DRIVER
	 * set, and one GPU here that can move -- written once before the
	 * render thread starts, and what the OpenGL renderer closes its
	 * library between spells for; gpu_driver_next the move owed to
	 * renderer_swap(), an index into gpu_drivers or -1, the render
	 * thread's, and gpu_driver_to the renderer it lands on, the render
	 * thread's too. gpu_drivers_pinned, under App::lock, once something of
	 * NVIDIA's driver was found in the process where nothing should be
	 * (gpu_driver_leftover(), app.c): no move is offered after it. And
	 * under App::lock, the moves a landing found no GPU for, by driver
	 * and renderer (app_gpu_driver_unseen()), each offered no more; and
	 * the name the GPU that can move goes by on either driver, which
	 * gpu_merge() writes for the GPU box's rows of it; and the driver it
	 * was on when the list first offered a move, whose rows stay where
	 * the renderers list the GPU for the window's life. */
	GpuDriver             gpu_drivers[GPU_DRIVERS_MAX];
	int                   gpu_drivers_n;
	bool                  gpu_drivers_on;
	bool                  gpu_drivers_pinned;
	int                   gpu_driver_next;
	const FnlRendererVtbl *gpu_driver_to;
	/* The place in the list of the GPU in use as the last FNL_EVENT_GPU
	 * saw it: a ranking that orders the list again for another display
	 * moves it, which is a list to read again (events_post_state()). */
	int                   ev_last_gpu_cur;
	/* The GPU hotplugs heard (app_gpu_hotplug()), counted by any thread,
	 * and the rebuild they owe the render thread, which lists every
	 * renderer before it builds (renderer_swap()). A count and not a flag:
	 * the render thread spends the hotplugs up to the one its wait began
	 * at (gpu_hotplug_seen_gen, gpu_hotplug_done), so one heard during
	 * the wait starts it again and one heard after it is owed a rebuild
	 * of its own -- a flag cleared after the wait would spend it unwaited
	 * for. */
	FnlAtomicU32          gpu_hotplug_gen;
	uint32_t              gpu_hotplug_seen_gen, gpu_hotplug_done;
	bool                  gpu_hotplug_relist;
	double                gpu_hotplug_seen;   /* render thread's, 0: none */
	/* The renderer fnl_init() tries first where nothing named one: the
	 * ranked first entry's (app_rank_start()), FNL_RENDERER_DEFAULT for
	 * the build's order. Main thread's, before the render thread. */
	FnlRenderer           renderer_first;
	/* The renderer FNL_RENDERER or FnlInitDesc::renderer named, which the
	 * window draws with or fails: the ranking's move then counts that
	 * renderer's entries alone (rank.c). FNL_RENDERER_DEFAULT where none
	 * was named. Main thread's, before the render thread. */
	FnlRenderer           renderer_named;
	struct {
		char        driver[GPU_DRIVER_NAME_MAX];
		FnlRenderer renderer;
	}                     gpu_driver_unseen[GPU_DRIVERS_MAX * GPU_SPAN_MAX];
	int                   gpu_driver_unseen_n;
	char                  gpu_driver_gpu[GPU_NAME_MAX];
	char                  gpu_driver_home[GPU_DRIVER_NAME_MAX];
	/* The ranking (src/core/rank.c): rank, the figures kept, read by
	 * gpu_merge() under App::lock and written under it; rank_path the file
	 * they are kept in, "" where FNL_GPU_RANK is off or there is nowhere
	 * to keep them;
	 * rank_target the ranked entry to move to once the list is whole,
	 * from 1, 0 for none -- the first where nothing was named; rank_land
	 * an entry's key to move back to instead, "" for none, where the
	 * caller named a GPU (rank.c's aim()); rank_asked_name and
	 * rank_asked_index what the caller asked for at fnl_init(), kept
	 * because a move spends App::gpu_want and App::gpu_index. The render
	 * thread's from the first frame on, with the calibration's state. */
	RankRow               rank[RANK_MAX];
	int                   rank_n;
	char                  rank_path[768];
	int                   rank_target;
	char                  rank_land[RANK_ID_MAX];
	bool                  rank_asked_name;
	int                   rank_asked_index;
	/* FNL_GPU_RANK as rank.c's RANK_*; whether what is missing is to be
	 * asked again (rank.c's check_missing()), and whether =calibrate has
	 * had its one measure of everything; and the display and the list
	 * that were last asked about, the list by its keys' hash. */
	int                   rank_mode;
	bool                  rank_dirty;
	bool                  rank_forced;
	char                  rank_display[RANK_DISPLAY_MAX];
	unsigned long         rank_list_hash;
	/* What the window was last seen on and at what size, and since when:
	 * a display is measured, and the window moved to its first entry, only
	 * once it has stayed on one output, unmoved and not straddling another,
	 * for RANK_QUIET (rank.c's quiet()). */
	char                  rank_seen[RANK_DISPLAY_MAX];
	int                   rank_seen_w, rank_seen_h;
	double                rank_seen_at;
	struct {
		int    state;               /* rank.c's CAL_* */
		int    i, n;                /* the entry measured, of n */
		char   ids[GPU_LIST_MAX * 2][RANK_ID_MAX];
		double fps[GPU_LIST_MAX * 2];
		double at;                  /* when the state was entered */
		double started, entry_at;   /* the calibration's, the entry's */
		/* Which group each entry is of -- 0 on the driver the GPU is on,
		 * 1 a move's or after one -- and what each group's entries took,
		 * which the time left is reckoned from. */
		unsigned char group[GPU_LIST_MAX * 2];
		double dur_sum[2];
		int    dur_k[2];
		double gpu_sum, cpu_sum;
		int    gpu_k, cpu_k;
		/* The figure watched in CAL_BIN spans while the entry settles and
		 * is sampled (rank.c's bin_tick()): this span's sums and when it
		 * began; the last span's figure and its GPU and CPU seconds; and
		 * the running mean's figure at the last span. */
		double bin_gpu, bin_cpu;
		int    bin_gk, bin_ck;
		double bin_at, bin_last, span_gpu, span_cpu, mean_last;
		char   back[RANK_ID_MAX];   /* the entry in use before it */
		char   display[RANK_DISPLAY_MAX];  /* where it measures */
	}                     cal;
	/* A calibration under way, which holds the window's input
	 * (app_rank_holds_input()): the render thread writes it, the input
	 * thread reads it. */
	FnlAtomicBool         calibrating;
	/* A calibration asked for from the GPU box (app_rank_recalibrate()),
	 * the input thread's, which the render thread spends. */
	FnlAtomicBool         rank_recal_owed;
	/* The buttons whose press was posted, by FnlButton bit: a release
	 * during a calibration goes for those alone. The input thread's. */
	uint32_t              buttons_down;
	/* A pick's inventory lines, held until its list is merged, which is
	 * what numbers them (app_gpu_inventory()). The thread picking. */
	char                  inv_name[GPU_LIST_MAX][GPU_NAME_MAX];
	char                  inv_kind[GPU_LIST_MAX][32];
	int                   inv_n;
	/* The move made and not yet seen to land (app_gpu_driver_landed()):
	 * the GPU, the driver it left as `bound` and the one it is on as
	 * `other`. Render thread's. */
	GpuDriver             gpu_driver_made;
	bool                  gpu_driver_moved;
	/* A move drawn across (DESIGN *Drawn across the move*): the renderer
	 * built on drivers that open nothing of the GPU moved while the move
	 * runs on a thread of its own (renderer_cross()). gpu_cross from that
	 * build until the move has ended, which every Vulkan instance made
	 * meanwhile reads (make_instance()); gpu_cross_off once a cross would
	 * not build or held the GPU anyway, for the process's life;
	 * gpu_cross_want the moved GPU's name, drawn on after it;
	 * gpu_cross_landing the swap onto it owed, which makes no move of its
	 * own (app_gpu_driver_poll()); and gpu_cross_from the renderer the
	 * window was on before the move, whose choice gpu_back_* is, and so
	 * the way back where the landing fails -- not the one drawn across
	 * on. The thread, `mover`, makes `mover_move` and writes
	 * gpu_driver_made, gpu_driver_moved and mover_moved, which the render
	 * thread reads once it has joined it; mover_done is its last write.
	 * Render thread's otherwise. */
	bool                  gpu_cross;
	bool                  gpu_cross_off;
	bool                  gpu_cross_landing;
	char                  gpu_cross_want[GPU_WANT_MAX];
	const FnlRendererVtbl *gpu_cross_from;
	/* What the last build's three steps took, in milliseconds -- the
	 * instance, the surface, the device objects -- said with a swap, where
	 * a move's pause is whichever of them the driver is slow at
	 * (renderer_build()). The thread building. */
	double                build_ms[3];
	bool                  mover_started;
	FnlThread             mover;
	GpuDriver             mover_move;
	bool                  mover_moved;
	FnlAtomicBool         mover_done;
	double                rebuild_at;
	double                pace_at;
	/* Whether this turn went through pace_space(), which is whether a
	 * rebuild it makes dates from pace_at. */
	bool                  pace_ran;

	/* A period estimate and a missed-cycle count for --frame-times, which
	 * reports them as `miss n/m`. Kept independently of late_period, which
	 * exists only where and while the probe runs. See inst_sample(). */
	double                inst_period;
	double                inst_prev;
	int                   inst_miss;
	int                   inst_frames;

	/* Bumped whenever the frame must be recorded again, and the draw state
	 * the last recording was made from. */
	uint32_t              record_gen;
	FnlDrawState          last_draw;
	/* The loop at rest (rest_owed() in src/core/frame.c): whether the last
	 * turn drew nothing, record_gen as the last submitted frame left it, and
	 * how many frames are still drawn after the last change before a turn
	 * may draw none. */
	bool                  resting;
	uint32_t              shown_gen;
	int                   rest_settle;

	/* The last-reported copy of everything events_post_state() diffs
	 * against. src/core/events.c owns it. */
	bool                  ev_state_seeded;
	int                   ev_last_w_pt, ev_last_h_pt;
	int                   ev_last_w_px, ev_last_h_px;
	int                   ev_last_scale120;
	int                   ev_last_scene_w, ev_last_scene_h;
	FnlAntialias          ev_last_aa;
	bool                  ev_last_fullscreen, ev_last_maximized;
	bool                  ev_last_captured;
	char                  ev_last_output[OUTPUT_NAME_MAX];
	int                   ev_last_out_w_pt, ev_last_out_h_pt;
	/* The GPU in use as last reported, by name and renderer: not by
	 * index, which the list can change around it, and not by how often it
	 * changed, which a move and its way back count twice. */
	char                  ev_last_gpu[GPU_NAME_MAX];
	FnlRenderer           ev_last_gpu_renderer;
	/* And the list's length, which grows once the other renderers' GPUs
	 * are listed (app_gpu_list_others()). */
	int                   ev_last_gpu_n;

	/* When the scene's clock started, so the first moving frame begins at
	 * zero however long the window has been up. */
	double                scene_motion_t0;
	/* The veil (veil_step() in frame.c): its phase, when that phase
	 * began, and App::presents when the frame at full veil was latched or
	 * the lift armed, so the phase after each waits for a present of it.
	 * Render thread's. */
	VeilPhase             veil_phase;
	double                veil_t0;
	uint64_t              veil_presents;
	/* The veil VEIL_SNAP draws: where a lift stood when a move was asked
	 * for during it, held for the frame read back; 0 from no veil. */
	float                 veil_snap;
	/* The frame read back ahead of a move (app_snapshot_set()): rows of
	 * snap_w * 4 bytes, the top row first, BGRA whatever the renderer's
	 * order. The device built after the move draws it over its first
	 * frames (the cover, FnlRendererVtbl::overlay_cover), and the KMS and
	 * Win32 backends show it through the gap. snap_owed asks the renderer for
	 * one on the next frame it draws, snap_fresh says that frame gave it,
	 * snap_gen counts every one set, so a renderer knows the one it
	 * uploaded; veil_black is a transition with none, down to black and
	 * up from it. The block lives for the one move it was read for, let go
	 * when the next is asked for and when the lift ends (snapshot_drop()
	 * in frame.c), so no later gap shows it. Render thread's, the block
	 * and its size under App::lock as well for the Win32 window thread,
	 * which paints from them. */
	unsigned char        *snap_px;
	int                   snap_w, snap_h;
	bool                  snap_valid;
	bool                  snap_owed;
	bool                  snap_fresh;
	uint32_t              snap_gen;
	bool                  veil_black;
	/* A backend showed the snapshot whole through a gap since the last
	 * rebuild -- the KMS take-back, the Win32 erase or hold -- which a loss
	 * during the lift answers by lifting again from it (device_recover()).
	 * [App::lock] */
	bool                  snap_shown;
	double                fps_last_t;
	/* When the last --frame-times line was written, or its sums started
	 * again (FRAME_TIMES_WINDOW in frame.c). Render thread. */
	double                fc_last_t;
	/* `presents` at the start of the window being measured: the rate counts
	 * presents, not loop turns. */
	uint64_t              fps_presents;

	/* --frame-times: the phases that partition the loop between two
	 * consecutive turns. See DESIGN *Where the time actually goes*.
	 *
	 *   pace     the loop's own throttle
	 *   wait     blocked before recording, which the line splits in two:
	 *            fence    on the device's fences (App::device_block): the
	 *                     GPU behind
	 *            acquire  the rest: the swapchain's image or its frame-
	 *                     latency object, a presentation engine behind
	 *   record   building the command buffer; zero on a cached recording
	 *   submit   handing it to the queue
	 *   present  the present call itself
	 *
	 * The loop writes `pace` and the fence share, the renderer the rest.
	 * Summed over the reporting window, not smoothed. */
	bool                  frame_times;
	/* Whether this turn times its phases: --frame-times, or a caller's
	 * fnl_set_frame_stats(). The render thread's, set at the top of each
	 * turn; every site that reads the clock for either reads this. */
	bool                  timing;
	bool                  counting;     /* and stats_on, for this turn */
	/* fnl_set_frame_stats(): whether to count, and whether ever counted
	 * (the getter's false before). `st` is the render thread's running
	 * totals, `pub_st` their copy under App::lock, once a frame. */
	FnlAtomicBool         stats_on, stats_ever;
	FnlFrameStats         st, pub_st;
	/* The last FRAME_RING drawn frames for fnl_frame_samples(), under
	 * App::lock: `ring_head` is where the next goes, `ring_n` how many are
	 * waiting, `ring_lost` how many were overwritten unread since the last
	 * drain. Pushed with the totals' publish, once a drawn frame. */
	FnlFrameSample        ring[FRAME_RING];
	uint32_t              ring_head, ring_n;
	uint64_t              ring_lost;
	/* The loop has ended, so no frame waits for its GPU time any more
	 * (GPU_HOLD_FRAMES); cleared as the next starts. Under App::lock. */
	bool                  ring_done;
	/* The GPU times read this turn for the frames in the ring, folded in
	 * at the publish: the render thread's (app_gpu_time()). `gpu_timed`
	 * is whether the frame being drawn was timed (app_gpu_timed()). */
	struct { uint64_t index; float ms; } gpu_late[GPU_LATE_MAX];
	uint32_t              gpu_late_n;
	bool                  gpu_timed;
	/* Which timed frames stay out of the GPU's mean (App::cost_counts
	 * false as they were drawn), in slot index % GPU_UNCOUNTED: the index
	 * and the word, compared whole, so a slot a later frame took or none
	 * wrote says "counts", as does an index no frame was timed at. Said
	 * when the frame is timed, since its time is read frames later, when
	 * cost_counts may say otherwise. Render thread. */
	struct { uint64_t index; bool uncounted; } gpu_uncounted[GPU_UNCOUNTED];
	/* The same for fnl_frame_shown(), from any thread the backend hears
	 * its display on (app_frame_shown()). */
	FnlFrameShown         shown[FRAME_RING];
	uint32_t              shown_head, shown_n;
	uint64_t              shown_lost;
	/* The presents a swapchain numbered, newest at numbered_head - 1, and
	 * the number last stamped (app_present_numbered()). Render thread. */
	struct { uint32_t number; uint64_t index; double at; } numbered[16];
	uint32_t              numbered_head, numbered_n;
	uint32_t              number_stamped;
	double                fc_wait, fc_record;               /* this frame */
	double                fc_submit, fc_present;
	double                fc_pace_sum, fc_wait_sum;          /* the window */
	double                fc_fence_sum;
	double                fc_record_sum, fc_submit_sum, fc_present_sum;
	double                fc_draw_sum, fc_rebuild_sum;
	int                   fc_frames;
	/* Of those frames, how many found the device already finished when they
	 * began to wait on it -- what tells a busy GPU from a slow presentation
	 * engine, which `wait` alone cannot. */
	int                   fc_idle;
	/* FNL_COST_PARTS's sums (src/core/parts.c), NULL when off. Set before
	 * the render thread starts and freed after it is joined. */
	struct PartsState    *parts;

	/* What the frame being drawn latched: the window's size in points and
	 * its scale in 120ths. */
	int                   draw_w, draw_h;
	int                   draw_scale120;
	/* The scene resolution this frame's objects were last *asked* for, in
	 * pixels, or SCENE_NATIVE. The request and not the answer: a device that
	 * refuses the target leaves scene_extent zero while this stays, which
	 * stops the render loop asking again on every frame. What exists is
	 * always scene_extent. */
	int                   draw_scene_w, draw_scene_h;
	/* The grab ring in effect for the frame being drawn, in points (see
	 * deco_margin). The swapchain is this much bigger than the window on
	 * every side, and window coordinates are offset by it. */
	int                   draw_margin;
	int                   applied_scale120;
	/* The window geometry and opaque region last committed, sent with the
	 * buffer they describe. */
	int                   applied_geo_w, applied_geo_h, applied_geo_m;
	/* The state of the configure last acked: what the compositor holds the
	 * geometry against until the next ack (app_surface_sync). */
	bool                  acked_fullscreen, acked_maximized;

	/* The claims this frame's on_overlay has made so far: filled by
	 * fnl_claim() and fnl_titlebar_claim(), swapped into pub_claims under
	 * App::lock when the callback returns, and cleared at the top of each
	 * frame's overlay. */
	FnlRect               claims[CLAIMS_MAX];
	int                   claim_count;
	bool                  claim_overflow_warned;

	/* Whether on_gpu_created has been delivered and not yet answered by
	 * on_gpu_lost. It keeps the two strictly alternating when a recovery
	 * fails and the loss flag stays set. Render thread after fnl_run()
	 * starts it, main thread before and after -- every site is either side
	 * of the join. */
	bool                  cb_gpu_created;
	/* Whether fnl_run() is on the main thread's stack, which alone writes
	 * and reads it. The outermost on_gpu_created and on_gpu_lost run
	 * there, so it is this and not the thread that refuses fnl_shutdown()
	 * and fnl_run() from either. */
	bool                  in_run;

	/* ---- what every renderer has ------------------------------------ */

	/* Set when a call reports that what this frame was built on is gone:
	 * raised inside a frame and acted on at the top of the next one, where
	 * nothing is half-recorded (see recover_from_loss). The two differ only
	 * in whether the surface is replaced as well. */
	bool                  device_lost;
	bool                  surface_lost;
	/* Raised with surface_lost when the surface is replaced because the
	 * backend asked (FnlRendererVtbl::remake_surface) and not because it
	 * went: the same rebuild, said as what it is and not as an error. */
	bool                  surface_remake;

	/* The swapchain's size, and the scene's -- zero when the scene is drawn
	 * straight into the window. Both in pixels. `extent` is what a frame
	 * draws, from the image's top-left corner. */
	Extent                extent;
	Extent                scene_extent;
	/* Where the window shows the top-left `extent` of a larger image, the
	 * image's size, and zero where the image is `extent`: kept across the
	 * sizes of a drag and made again when the window outgrows it (DESIGN
	 * *A swapchain larger than the window*). Written by the renderer.
	 * The backend reads it where it crops, and the frame loop's settle
	 * reads it either way; both on the render thread. `buffer_crops`,
	 * set by the backend at open and not changed, is Wayland's viewport.
	 * A renderer also writes `buffer` where Win32's DXGI shows a buffer
	 * from its corner, where an X11 Vulkan swapchain presents one to one
	 * from its corner, and where Wayland OpenGL keeps a larger native
	 * window. */
	Extent                buffer;
	bool                  buffer_crops;
	/* When the size, scale or margin last changed (swapchain_adopt()), and
	 * whether the rebuild that makes a larger image the window's own size
	 * has been asked since: once a size, so an image that cannot be made
	 * so is not asked for every frame. Render thread. */
	double                size_at;
	bool                  buffer_settled;

	/* True when the surface encodes linear -> sRGB as fragments are written,
	 * which is what the palette conversion and the text shader turn on. */
	bool                  srgb_surface;

	/* The overlay palette, converted once for that blend space. */
	float                 pal[COL_COUNT][4];

	/* ---- the overlay's instance streams ------------------------------
	 * Written by src/core/overlay.c, which names no graphics API. The
	 * renderer supplies the pointers, each of them memory the CPU may write
	 * while the device reads what was submitted last.
	 *
	 * `*_map` is null whenever there is nothing to append into, and every
	 * appender checks it: drawing before a device exists, or after one is
	 * lost, is a no-op.
	 *
	 * `*_count` is what has been appended this frame, `*_drawn` what has
	 * already been issued. A flush draws the difference and moves drawn up to
	 * count. */

	/* Whether a recording is open, i.e. whether ovl_img and text_img name an
	 * arena it is safe to write. Set by overlay_begin() and cleared by
	 * overlay_end(). Every appender is gated on it: outside a recording the
	 * two still name the image the *previous* recording used, whose command
	 * buffer may be executing. */
	bool                  overlay_open;

	/* Whether fnl_overlay_clip() has narrowed the scissor and not put it
	 * back. A clip is command state and would reach every later draw of the
	 * frame, so draw_overlay() takes it back when on_overlay returns. */
	bool                  overlay_clipped;

	int                   font_si;       /* baked scale index, or -1 */
	int                   font_atlas_w, font_atlas_h;

	GlyphInstance        *text_map[OVERLAY_MAX_IMAGES];
	uint32_t              text_count;     /* glyphs appended this recording */
	uint32_t              text_drawn;     /* of those, issued by text_end() */
	uint32_t              text_img;       /* which buffer that is going into */

	unsigned char        *ovl_map[OVERLAY_MAX_IMAGES];
	uint32_t              rect_count,  rect_drawn;
	uint32_t              shape_count, shape_drawn;
	uint32_t              ovl_img;

	/* ---- shared: under App::lock -------------------------------------- */
	_Alignas(64)
	FnlMutex              lock;
	FnlCond               cond;
	int                   want_w, want_h;
	int                   want_scale120;
	bool                  quit;
	Zone                  hovered_button;

	/* The newest configure the compositor has sent, and whether it is still
	 * unanswered. The ack is deferred to the frame that actually commits a
	 * buffer of that size (see surface_ack_configure), so the compositor's
	 * resize handshake stays in step with what is on screen. */
	uint32_t              cfg_serial;
	bool                  cfg_pending;
	/* Latched by the render thread together with want_w/want_h, so a frame's
	 * decoration state always matches the size it is drawn at. */
	bool                  want_fullscreen;
	bool                  want_active;     /* has the keyboard focus */
	bool                  want_maximized;
	/* Names of the outputs the surface currently covers, the one whose scale
	 * is being followed first. Published for the status line. */
	char                  want_output[OUTPUT_NAME_MAX];
	int                   want_out_w_pt, want_out_h_pt;
	/* The followed output's own mode in pixels, where the backend knows it
	 * apart from the room above -- Wayland, whose room is the compositor's
	 * bounds for the window, which may be another output's -- and 0 by 0
	 * where not. Published with the name (app_publish_output_mode()), so
	 * the two are never read a pair apart; what rank.c names a display
	 * by. */
	int                   want_out_mode_w, want_out_mode_h;
	/* A window server's monitors and the one the window is on, as its
	 * backend last published them (app_publish_outputs()); what
	 * backend_published_outputs() answers. KMS keeps its own. */
	char                  out_names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int                   out_count, out_current;
	bool                  want_captured;
	int                   want_cursor_x, want_cursor_y;
	/* The scene size fnl_set_scene_size_px() asked for. The render thread
	 * turns the pair into draw_scene_* once it knows the area it has to fit
	 * (see scene_pin_applies). Under the lock rather than two atomics: a
	 * width latched with a stale height is an image nobody asked for. */
	int                   want_scene_w, want_scene_h;
	/* Bumped by fnl_invalidate(), latched into FnlDrawState::user_gen. Under
	 * the lock because the shim has no atomic add, and a lost bump is a
	 * stale frame. */
	uint32_t              want_user_gen;
	/* The antialiasing mode the caller asked for (fnl_set_antialias()),
	 * and the one in force: aa_asked as far as the device drawing now can
	 * take it, worked out again at every device build (app_aa_settle()),
	 * so a move to a GPU without FSAA drops it and a move back puts it
	 * back. want_aa is what fnl_antialias() reports and the frame
	 * latches. */
	FnlAntialias          aa_asked;
	FnlAntialias          want_aa;
	/* A change that has to be answered by the next frame rather than by the
	 * next invitation to draw: a configure that changed the toplevel's states
	 * or a present-mode toggle. Cleared by the render thread as it takes the
	 * change on; such a frame is never paced -- see frame.c. */
	bool                  want_urgent;

	/* When the last invitation to draw arrived, and 0 until the first one
	 * ever does: a compositor that sends none cannot be told apart from one
	 * that has stopped, so until then the render thread keeps drawing (see
	 * frame.c). Written only by a backend that invites. */
	double                frame_cb_at;

	/* ---- the window as an [any]-thread caller sees it ----------------------
	 * The [any] getters may not read the fields their answers derive from:
	 * App::extent, draw_scale120 and draw_margin are the render thread's and
	 * is_fullscreen is the pump's. So the render thread computes the lot once
	 * a frame and publishes it here under App::lock. What comes back
	 * describes the frame that was last *built*, and is consistent with
	 * itself.
	 *
	 * Points, except the two named _px. */
	int                   pub_win_w, pub_win_h;
	int                   pub_cx, pub_cy, pub_cw, pub_ch;
	int                   pub_area_w_px, pub_area_h_px;
	int                   pub_scene_w, pub_scene_h;
	int                   pub_scale120;
	/* The output the window is on, in logical points. Published because the
	 * titlebar controls filter their size list against it from *both*
	 * threads, and the two have to filter to the same list. */
	int                   pub_out_w_pt, pub_out_h_pt;
	bool                  pub_csd;
	/* The titlebar band the published figures were measured against, in
	 * points -- 0 in fullscreen. */
	int                   pub_bar_h;

	/* What the caller claimed, in a bar of its own or in the presentable
	 * area, published for the input thread's hit test (claim_at() in
	 * decor.c). Swapped in under the lock each frame from the list
	 * on_overlay declared, so a control is hit-testable exactly where the
	 * frame that declared it drew it. */
	FnlRect               pub_claims[CLAIMS_MAX];
	int                   pub_claim_count;

	/* ---- what the pump has been asked to do ------------------------------
	 * The knobs that reach into the window server do not act where they are
	 * called: they record here and app_drain_requests() spends them on the
	 * pump's thread, which is what makes them [any]. Each flag is *latest
	 * wins*, not a queue. */
	PendingReq            req;
	/* The last shape fnl_set_cursor() was given, under the same lock. Not
	 * App::cursor, which is the pump's copy and one drain behind. */
	FnlCursor             cursor_asked;
	/* What fnl_clipboard_set() was given, on its way to the pump, which
	 * takes it as clip_out when it drains the request. */
	char                 *clip_pending;

	/* The device's name, written by pick_device() -- on the main thread at
	 * startup and on the *render* thread when loss recovery re-picks a
	 * device. fnl_gpu_name() is [any], so it hands back a copy taken under
	 * App::lock, never a pointer into this buffer: a longer name written
	 * over a shorter one is unterminated for an interval. */
	char                  gpu_name[GPU_NAME_MAX];
	/* What the frames go through, as GpuEntry::via says it for the entry in
	 * use; written by the renderer once it has chosen, under App::lock for
	 * fnl_renderer_via(), which copies it out as fnl_gpu_name() does. */
	char                  gpu_via[GPU_VIA_MAX];
	/* The GPUs the renderer found when it last picked one, how many, and the
	 * one in use, -1 before there is any (app_gpu_list_publish()). All
	 * written by the renderer's pick, on the thread that builds the device.
	 * gpu_req is fnl_request_gpu()'s wish, the index plus one and 0 for
	 * none, and gpu_req_entry the entry it named then, spent by the render
	 * loop (gpu_switch_owed()): the list may grow before it is, as the
	 * other renderers' GPUs are listed, and the entry is found again. */
	GpuEntry              gpu_list[GPU_LIST_MAX];
	int                   gpu_list_n;
	int                   gpu_list_cur;
	int                   gpu_req;
	GpuEntry              gpu_req_entry;
	/* The list as the renderers it spans gave it: gpu_span[0] the one the
	 * window started with, then each other the caller has a scene for, in
	 * the order fnl_init() tries them; gpu_part[k] the GPUs gpu_span[k]
	 * lists, its own pick's for the one drawing and list_gpus()'s for the
	 * rest. gpu_list is their run end to end, then the moves to another
	 * kernel driver, each of which may trade places with its renderer's
	 * row of the GPU (GpuEntry::slot, app_gpu_driver_entries()). The
	 * others' parts are empty until they are listed, once, after the first
	 * frame
	 * (app_gpu_list_others()). gpu_part_cur is the GPU in use in the part
	 * of the renderer drawing; gpu_spans whether the caller has a scene
	 * for more than one, known before the first build, which the
	 * inventory lines say. */
	const FnlRendererVtbl *gpu_span[GPU_SPAN_MAX];
	int                   gpu_span_n;
	GpuEntry              gpu_part[GPU_SPAN_MAX][GPU_LIST_MAX];
	int                   gpu_part_n[GPU_SPAN_MAX];
	int                   gpu_part_cur;
	bool                  gpu_spans;
	/* The OpenGL renderer's profile at its last device build, 45 or 41,
	 * for fnl_gl_version() on any thread; 0 before one. */
	FnlAtomicU32          gl_version;
	/* Where the renderers share a driver (FnlBackendVtbl::shared_driver),
	 * or OpenGL draws, what each of gpu_early_r listed before the one
	 * drawing was built, which app_gpu_list_others() publishes in place
	 * of listing it then. The main thread's in fnl_init(), and the render
	 * thread's again at a move to another kernel driver, before the
	 * renderer is built on the GPU moved (gpu_relist_landing()); read by
	 * the lister, which is started after each and joined before the
	 * second. */
	const FnlRendererVtbl *gpu_early_r[GPU_SPAN_MAX];
	GpuEntry              gpu_early[GPU_SPAN_MAX][GPU_LIST_MAX];
	int                   gpu_early_c[GPU_SPAN_MAX];
	int                   gpu_early_n;

	/* ---- shared: atomics -----------------------------------------------
	 * Guarded by nothing but compiler.h's shim. Grouped by who stores: the
	 * pump, a caller on any thread, the render thread.
	 *
	 * No cache line of their own, and none between the groups: each is read
	 * by the thread that does not store it, so a boundary would move the
	 * sharing rather than remove it, and the stores are per key, per
	 * rebuild or once a COST_WINDOW -- titlebar_reserved apart, which the
	 * pump's hit test reads anyway. gpu_name above, written once per
	 * device, keeps them clear of the busy fields under App::lock. */

	/* Every held key, a bit per FnlKey. Stored by the pump only, which is
	 * what lets a set bit be a load and a store rather than an atomic OR. */
	FnlAtomicU32          keys_held[KEY_HELD_WORDS];

	/* The titlebar controls, owned by src/core/controls.c. The pump writes
	 * them from a press and the render thread reads them to draw;
	 * fnl_invalidate() carries the "something moved" edge.
	 *
	 * ctl_pin_* is the size the drop-down last picked, in pixels of the
	 * presentable area. It survives the checkbox being unticked, so
	 * re-ticking it spends the same size. ctl_check is the scaling box's
	 * tick; the antialiasing boxes' entries are unused, since those two
	 * show the mode in force (ctl_ticked()). */
	FnlAtomicBool         ctl_check[CTL_CHECK_COUNT];
	FnlAtomicU32          ctl_pin_w, ctl_pin_h;
	/* Which drop-down is open: none, the size box, the output box or the
	 * GPU box (CTL_SHUT, CTL_OPEN_SIZE, CTL_OPEN_OUTPUT, CTL_OPEN_GPU in
	 * controls.c). */
	FnlAtomicU32          ctl_combo_open;
	/* The row under the pointer, plus one, so that zero is "none". */
	FnlAtomicU32          ctl_combo_hover1;

	/* The refresh rate of the monitor the window is on, in millihertz, as
	 * its window server reports the mode; zero where it reports none. The
	 * pump writes it and the render thread's spacing reads it: a measured
	 * period shorter than one refresh is a loop nothing throttles (see
	 * pace_period() in frame.c). */
	FnlAtomicU32          out_refresh_mhz;

	/* Set by the pump when the window is asked to close, and by
	 * fnl_quit(). */
	FnlAtomicBool         should_close;

	/* How wide a run of the titlebar the caller has asked for, in logical
	 * points; 0 for none. Stored by fnl_titlebar_reserve() and read by the
	 * pump's hit test; the rectangle is recomputed on both sides from this
	 * and the window width. */
	FnlAtomicU32          titlebar_reserved;
	/* On unless FNL_VSYNC says "0", set once in fnl_init() before any
	 * thread reads it. */
	FnlAtomicBool         vsync;
	/* fnl_set_cost_sampling()'s: a window at rest draws a frame every
	 * COST_WINDOW, to keep the cost figures measured. */
	FnlAtomicBool         cost_sampling;
	/* fnl_set_metrics_panel()'s and fnl_set_metrics_key()'s; the key is an
	 * FnlKey, FNL_KEY_F5 from fnl_init(), and read by app_key() on the
	 * pump. */
	FnlAtomicBool         metrics_shown;
	FnlAtomicU32          metrics_key;
	/* The process's figures the panel shows, as the draw state takes them
	 * (METRICS_UNKNOWN where none); when the last sample was taken, 0
	 * before the first; and what the two shares are measured from: the
	 * process's CPU seconds and a monotonic clock at the last sample,
	 * met_cpu_s negative where there is none, and the GPU's seconds on
	 * every frame read so far (app_gpu_time(), counted or not) and the
	 * clock at the last sample, met_gpu_at 0 where there is none.
	 * Render thread's. */
	uint32_t              met_cpu_pm, met_ram_mib, met_vram_mib, met_gpu_pm;
	double                met_at, met_cpu_s, met_wall_s;
	double                met_gpu_s, met_gpu_base, met_gpu_at;
	/* fnl_set_transition_ms()'s: how long the veil takes to come down
	 * ahead of a move and to lift after it, in milliseconds; 0 is none
	 * that way. VEIL_MS until a caller says, and one figure for both. */
	FnlAtomicU32          veil_out_ms, veil_in_ms;
	/* fnl_set_text_input(); read by the pump as each key arrives. */
	FnlAtomicBool         text_input;
	/* fnl_set_text_rect_pt(), as four words and a flag saying they were ever
	 * written. Torn across a write it is a rectangle one caret-move stale in
	 * one coordinate, for one commit, as a hint to somebody else's popup. */
	FnlAtomicU32          text_rect[4];
	FnlAtomicBool         text_rect_set;
	/* Whether the scene's animation clock runs. Off by default, so that a
	 * still window re-records nothing (see FnlDrawState::scene_time). */
	FnlAtomicBool         scene_motion;

	/* The largest buffer this device will allocate for the surface, or zero
	 * until there is a device to ask. It bounds the *scale* rather than the
	 * buffer (see clamp_device_scale120). The render thread republishes it on
	 * every rebuild and the pump reads it; a reader one build stale resolves
	 * the scale the way it did a moment ago. */
	FnlAtomicU32          max_extent_w, max_extent_h;

	/* What FSAA multisamples at, as a renderer-specific sample count: the best
	 * the device will give for both colour and depth, or the one-sample
	 * value when it will give neither. Written on the main thread before the
	 * render thread exists, and by the render thread when loss recovery
	 * re-picks a device; read by the pump. */
	FnlAtomicU32          msaa_samples;
	/* App::gpu_list_n again, for the titlebar's run: its width is the
	 * floor's, and deco_min_window() is read with App::lock already held
	 * (ctl_env()). Written with the list (app_gpu_list_publish()). */
	FnlAtomicU32          gpu_count;
	/* App::renderer, for any thread but the render thread: stored where
	 * that is written, before the render thread starts and by it at a
	 * move to another renderer (renderer_any()). */
	FnlAtomicPtr          renderer_pub;

	/* The mode the swapchain that exists was built with, in the renderer's
	 * own terms: the renderer sets it on the render thread at every
	 * rebuild while fnl_present_mode() reads it from [any], through
	 * FnlRendererVtbl::present_mode_name. On Vulkan it is a
	 * VkPresentModeKHR (vk_present_mode()); on OpenGL, Direct3D 12 and
	 * Metal the swap interval vsync asks for, 1 waiting for the vertical
	 * blank and 0 not -- on OpenGL the interval asked, which the one given
	 * may differ from (gl_state.h's `interval`). Core never reads it as
	 * either: it goes back to the renderer through present_mode_name and
	 * present_mode_matches, and one renderer's value means nothing to
	 * another's. */
	FnlAtomicU32          present_mode;
	/* Presents per second, as fnl_fps() reports it from [any]; the render
	 * thread recomputes it once a COST_WINDOW. */
	FnlAtomicU32          fps_display;
	/* What a drawn frame costs, in microseconds, as fnl_frame_gpu_ms() and
	 * fnl_frame_cpu_ms() report it from [any] and the draw state latches
	 * it; COST_UNKNOWN before there is a figure. Written when published
	 * (app_cost_publish()), not at every frame. The GPU's raw sample is a
	 * renderer's, counted, so the render thread folds each into the mean
	 * once (app_frame_cost(), app_cost_poll()). */
	FnlAtomicU32          cost_gpu_us;
	FnlAtomicU32          cost_cpu_us;
	FnlAtomicU32          gpu_sample_us;
	FnlAtomicU32          gpu_sample_seq;
	/* What fnl_input_latency_ms() reports, in microseconds, and the draw
	 * state latches; COST_UNKNOWN before an input has reached the caller.
	 * Written when published, as the two above are. */
	FnlAtomicU32          input_lat_us;
	/* How many times the machine has come back from a sleep, counted on the
	 * pump thread (app_resumed()) and read by the render thread against
	 * resumes_seen. */
	FnlAtomicU32          resumes;
	/* How many times the backend has asked for its surface to be made
	 * again (app_surface_remake()), against surface_remakes_seen. */
	FnlAtomicU32          surface_remakes;
	/* [render] Set by a backend's frame_cancel() that found the present
	 * it is told to take back answered already by the server -- shown,
	 * though the graphics API said it was refused (NVIDIA's out-of-date
	 * present on X11) -- and spent by app_frame_take_back(). */
	bool                  present_answered_anyway;
	/* [render] Bumped each time the graphics API starts numbering its
	 * presents afresh -- a Vulkan swapchain made (swapchain_make()) -- for
	 * a backend that ties a server's answer to its frame by that number
	 * (X11's Present, x11_frame_request()). */
	uint32_t              present_chain;

	/* ---- the event queue -----------------------------------------------
	 * A ring and its own mutex. src/core/events.c owns both; nothing else
	 * may touch these.
	 *
	 * ev_lock is deliberately not App::lock, which is held across the
	 * frame's whole latch: an input burst must not wait behind the render
	 * thread's turn.
	 *
	 * The lock and the indices start a cache line of their own, ahead of the
	 * ring: the pump takes the lock per event, and the atomics before it are
	 * stored by the render thread. */
	_Alignas(64)
	FnlMutex              ev_lock;
	int                   ev_head, ev_tail;
	unsigned              ev_lost;
	FnlEvent              ev_ring[EVENT_QUEUE_MAX];
	/* Each slot's inputs as fnl_input_latency_ms() counts them: how many
	 * the event carries -- more than one where motion coalesced, none for
	 * a notification or a repeat -- how many of those by a stamp of the
	 * server's or the device's, the sum of when each was made and the
	 * oldest, on FnlEvent::time's clock. And what handing them over has
	 * added up since the figure was last published: the latencies' sum,
	 * the longest, how many and how many stamped. */
	EvBorn                ev_born[EVENT_QUEUE_MAX];
	double                lat_sum, lat_worst;
	uint32_t              lat_n, lat_stamped;

	/* ---- shared: under text_lock ---------------------------------------
	 * The strings too long for an event, and the lock that is theirs alone:
	 * a paste of a megabyte should sit behind neither App::lock nor ev_lock.
	 * Taken at typing rate at most, so it is not given a line of its own.
	 *
	 *   compose   the composition, as last reported
	 *   clip_in   the last answer a clipboard request got */
	FnlMutex              text_lock;
	char                  compose[256];
	char                 *clip_in;
	size_t                clip_in_len;
};

/* ---- inline helpers --------------------------------------------------------
 * The pure ones first; those that read an App say whose fields they read. */

/* A logical-point figure, held inside OVERLAY_COORD_MAX. Clamped rather than
 * refused: everything past the framebuffer is clipped away anyway. */
static inline int clamp_pt(int v)
{
	if (v >  OVERLAY_COORD_MAX) return  OVERLAY_COORD_MAX;
	if (v < -OVERLAY_COORD_MAX) return -OVERLAY_COORD_MAX;
	return v;
}

/* A surface scale, held inside SCALE120_MIN..SCALE120_MAX. */
static inline int clamp_scale120(int s120)
{
	if (s120 < SCALE120_MIN) return SCALE120_MIN;
	if (s120 > SCALE120_MAX) return SCALE120_MAX;
	return s120;
}

/* The window a presentable area of this many pixels needs, in logical points.
 * A window that has never been configured is sized through these. The height
 * takes the titlebar in force: a caller that replaced the bar before opening
 * chose its own, and opening at the library's would resize the window on the
 * first frame. */
static inline int res_window_w_pt(int px, int s120)
{
	return (px * 120 + s120 - 1) / s120;
}

static inline int res_window_h_pt(int px, int s120, int bar_h)
{
	return (px * 120 + s120 - 1) / s120 + bar_h;
}

/* The presentable area: everything the window has below the titlebar, which is
 * what a scene renders into, in window points. There is no border on the
 * other three sides. Takes the bar's height rather than a "has a bar" flag --
 * 0 in fullscreen, the caller's own under fnl_titlebar_custom() -- and every
 * caller gets it from deco_bar_h(). */
static inline void content_rect(int win_w, int win_h, int bar_h,
								int *x, int *y, int *w, int *h)
{
	*x = 0;
	*y = bar_h > 0 ? bar_h : 0;
	*w = win_w;
	*h = win_h - *y;
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
}

/* Whether this window is wearing a client titlebar. Takes the flag rather
 * than an App because the render thread asks about the state it latched with
 * the frame's size, not about what the pump has since been told. */
static inline bool deco_client(bool fullscreen)
{
	return !fullscreen;
}

/* How tall the titlebar band is for this frame, in logical points, or 0 in
 * fullscreen. What every geometry site wants: DECO_TITLEBAR_H is the answer
 * only for the library's own bar.
 *
 * Like the two below it, this reads the deco_* trio: the pump's to read
 * freely, and any other thread's only under App::lock. */
static inline int deco_bar_h(const App *a, bool fullscreen)
{
	return deco_client(fullscreen) ? a->deco_h : 0;
}

/* Is the pointer's shape here the caller's to choose? Over what the caller
 * draws: the presentable area, a run or a claim of its own in the bar, and
 * the whole of a bar it replaced. One definition for every backend. */
static inline bool deco_cursor_is_callers(const App *a, Zone z)
{
	return z == ZONE_CONTENT || z == ZONE_CLIENT_ITEM ||
		   (z == ZONE_TITLEBAR && a->deco_custom);
}

/* The hard floor for a particular window, in logical points of window
 * geometry: the one size that is enforced. Fullscreen has none -- one point,
 * because a zero-sized window is a swapchain nobody can build. The library's
 * bar keeps its buttons and its controls; a custom bar's floor is its own
 * height and the width its caller asked for. Out of line because the
 * controls' width is controls.c's. */
void deco_min_window(const App *a, bool fullscreen, int *w, int *h);

/* The transparent grab ring in effect, in points: only while there is a
 * titlebar, and only on a backend with no resize border of its own (see
 * FnlBackendVtbl::grab_ring). */
static inline int deco_margin(const App *a, bool fullscreen)
{
	return deco_client(fullscreen) ? BACKEND(a)->grab_ring : 0;
}

/* Surface scale as 120ths (Wayland's fractional-scale unit): 120 = 100%.
 * Render thread: it is the scale of the frame being drawn. */
static inline int scale120(const App *a)
{
	return a->draw_scale120 > 0 ? a->draw_scale120 : 120;
}

/* Logical point -> physical pixel. */
static inline int scale_pt(int v, int s120)
{
	return (v * s120 + 60) / 120;
}

/* The presentable area in physical pixels, for a window of this size at this
 * scale with this grab ring. Rounded through the same scale_pt() the drawing
 * uses, so the number the swapchain is built for and the number the overlay
 * lays out against cannot disagree by a pixel. */
static inline void content_px_for(int w_pt, int h_pt, int s120, int m,
								  int bar_h, int *pw, int *ph)
{
	int cx, cy, cw, ch;
	content_rect(w_pt, h_pt, bar_h, &cx, &cy, &cw, &ch);
	*pw = scale_pt(cx + cw + m, s120) - scale_pt(cx + m, s120);
	*ph = scale_pt(cy + ch + m, s120) - scale_pt(cy + m, s120);
}

/* Window-space logical point -> physical pixel in the buffer, which the
 * transparent grab ring offsets. Every drawing helper goes through this, so
 * the decorations and overlay stay written in window coordinates. Render
 * thread. */
static inline int surf_pt(const App *a, int v, int s120)
{
	return scale_pt(v + a->draw_margin, s120);
}

/* ---- the pump thread's units ----------------------------------------------
 * Pixels from a window server become logical points here, so the rounding is
 * in one place: a window asked for 1280 points at 150% is 2000 pixels, and
 * 2000 pixels reported back must not come out as 1279 points and provoke a
 * resize. scale_pt() above is the other direction. */

/* The scale the pump thread converts against. want_scale120 is written by
 * that thread alone, and is the only figure that is current at the moment an
 * event is being turned into points. */
static inline int want_s120(const App *a)
{
	return a->want_scale120 > 0 ? a->want_scale120 : 120;
}

static inline int px_to_pt(int px, int s120)
{
	return (px * 120 + s120 / 2) / s120;
}

/* Unrounded, for a pointer position or a delta. */
static inline double px_to_ptf(double px, int s120)
{
	return px * 120.0 / s120;
}

static inline double pt_to_pxf(double pt, int s120)
{
	return pt * s120 / 120.0;
}

/* The window to open at, in logical points: FnlInitDesc's presentable area
 * where the caller gave one -- so the titlebar is added back on -- and
 * DEFAULT_AREA_W/H at this density otherwise, raised to the floor. Raised
 * here and not left to the window server: X11 states the floor only as a
 * hint for a manager's drag, and with no manager a window opened under it
 * stays there, its controls dropped. Pump thread. */
static inline void deco_default_window_pt(const App *a, int s120, int *w,
										  int *h)
{
	const int bar = deco_bar_h(a, false);
	*w = a->init_w_pt > 0 ? a->init_w_pt
						  : res_window_w_pt(DEFAULT_AREA_W, s120);
	*h = a->init_h_pt > 0 ? a->init_h_pt + bar
						  : res_window_h_pt(DEFAULT_AREA_H, s120, bar);
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	if (*w < min_w) *w = min_w;
	if (*h < min_h) *h = min_h;
}

/* What the pointer wears over a zone. The order is the precedence: a capture
 * hides it whatever is underneath, a resize edge says which way it resizes,
 * and only then is it the caller's -- over what the caller draws, and the
 * arrow over what the library does. A backend maps the answer through its
 * own tables. Pump thread only: it reads App::pointer_captured and
 * App::cursor. */
typedef enum {
	DECO_CURSOR_HIDDEN, DECO_CURSOR_EDGE, DECO_CURSOR_SHAPE
} DecoCursorKind;

typedef struct {
	DecoCursorKind kind;
	Edge           edge;      /* DECO_CURSOR_EDGE  */
	FnlCursor      shape;     /* DECO_CURSOR_SHAPE */
} DecoCursor;

static inline DecoCursor deco_cursor(const App *a, Zone z, Edge edge)
{
	DecoCursor c = { DECO_CURSOR_SHAPE, EDGE_NONE, FNL_CURSOR_DEFAULT };
	if (a->pointer_captured)
		c.kind = DECO_CURSOR_HIDDEN;
	else if (z == ZONE_RESIZE) {
		c.kind = DECO_CURSOR_EDGE;
		c.edge = edge;
	} else if (deco_cursor_is_callers(a, z))
		c.shape = a->cursor;
	return c;
}

/* The sRGB transfer function, for a colour authored in sRGB and blended on a
 * surface that encodes on write. Alpha is linear in both and never goes
 * through it. */
static inline float srgb_to_linear(float c)
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* ---- app.c -----------------------------------------------------------------
 * What the backends call into. [main], on the pump's thread, except
 * app_wakeup(). */

/* One key event, already translated. The backend calls this and the library's
 * own bindings live behind it, so every backend inherits them by translating
 * its codes. */
void app_key(App *a, FnlKey k, bool pressed);
/* A key that is already down as focus arrives: the held set only, no binding
 * and no event. Asks for a frame when the set moved, since nothing else says
 * it did. Every backend seeds through this; see app.c. */
void app_key_seed(App *a, FnlKey k);
/* Focus has gone, or the keyboard has: nothing is held any more. The releases
 * are never delivered, so every backend says it here; a frame is asked for
 * when anything was held. */
void app_keys_clear(App *a);
/* A held key coming round again. An event and nothing else -- no binding acts
 * on a repeat -- and only while text entry is on, so a backend may call this
 * for every repeat it sees without asking first. */
void app_key_repeat(App *a, FnlKey k);
/* The window was asked to close -- by the window server or by the library's
 * own close button. The run ends, and the caller is told and cannot veto.
 * Pump thread; the pump re-reads the flag, so there is no wakeup. */
void app_close_requested(App *a);

/* ---- pointer input, as every backend delivers it -------------------------
 * Window points, after the backend has done what is its own: converted the
 * coordinates, and kept whatever serial or root position a move or resize
 * will be started from. The event is posted first and the library acts
 * second, so a caller sees what the window was told before what it did. */
/* Where the pointer is. Nothing while captured: a captured pointer has no
 * position to report, and FNL_EVENT_RELATIVE carries it. */
void app_pointer_motion(App *a, double x_pt, double y_pt);
/* How far a captured pointer moved, in window points. The event only: the
 * reticle is the backend's to move, in its own units (app_reticle_move()). */
void app_pointer_relative(App *a, double dx_pt, double dy_pt);
/* A button. Left is the library's too: a press goes to deco_press() and a
 * release to deco_release(). */
void app_pointer_button(App *a, FnlButton b, bool pressed, double x_pt,
						double y_pt, uint32_t time_ms);
/* A scroll at the pointer, in notches (fenestal.h). */
void app_pointer_scroll(App *a, double x_pt, double y_pt, double dx,
						double dy);
/* Whether a held key repeats at all: false for the modifiers and the locks,
 * which are held to be held. app_key_repeat() drops the rest; a backend that
 * times its own repeats (Wayland) asks before arming the timer. */
bool app_key_repeats(FnlKey k);
/* One code point the layout made of a key press. Dropped unless text entry is
 * on, and dropped if it is not something a text field would insert; both
 * tests are here so that no two backends can disagree about either. */
void app_text(App *a, uint32_t codepoint);
/* Whether text entry is on. What a backend asks before doing work that only
 * text entry wants: translating a key through the layout, arming a repeat. */
bool app_text_input(const App *a);
/* A whole string of committed text, one FNL_EVENT_TEXT per code point. What an
 * input method hands over; a malformed sequence, over-long forms included,
 * types nothing. */
void app_text_utf8(App *a, const char *utf8);
/* The composition is now this -- NULL or empty for none. Posts
 * FNL_EVENT_COMPOSE when it differs from what was last reported, and only
 * then, so a backend may say it as often as it is told. */
void app_compose_set(App *a, const char *utf8);
/* The clipboard's answer, from the pump: `len` bytes that need not be
 * terminated, or NULL and 0 for no text. Always posts FNL_EVENT_CLIPBOARD,
 * because a request that is never answered is a paste that hangs. */
void app_clipboard_deliver(App *a, const char *data, size_t len);
/* The same, for an answer already on the heap: `owned` is malloc'd with room
 * for `len` bytes *and a terminator*, and is the core's from here. NULL is no
 * text. */
void app_clipboard_take(App *a, char *owned, size_t len);
/* Where the caret is, in window points; false when the caller never said. */
bool app_text_rect(const App *a, int *x, int *y, int *w, int *h);
/* Spend whatever the [any]-thread knobs asked for, on the pump's own thread.
 * Called once per turn from fnl_run(); see App::req. */
void app_drain_requests(App *a);
/* A change that must be answered by the next frame rather than by the next
 * invitation to draw. Core because every backend raises it. */
void app_mark_urgent(App *a);
/* Wake the event loop from another thread, through the backend table
 * (FnlBackendVtbl::wakeup; what a wakeup *is* is the backend's). */
void app_wakeup(App *a);
/* [any] Ask the pump for FnlBackendVtbl::nudge_size on its next turn. */
void app_request_size_nudge(App *a);

/* What a backend publishes to the render thread. All [main], from the pump:
 * see the note above them in app.c. */

/* fnl_request_size_pt() and its internal callers, gated and with the titlebar
 * added; FnlBackendVtbl::request_size is handed the window. */
void app_request_size(App *a, int w_pt, int h_pt);
/* The window's size, scale and state after a resize that has already
 * happened, with cur_*, win_* and the reticle brought into line. Not for a
 * server whose configure has to be acknowledged. */
void app_publish_size(App *a, int w_pt, int h_pt, int s120);
/* The same, with a configure serial the frame that draws this size answers
 * (FnlBackendVtbl::ack_configure): published under the one lock with the
 * size, so the render thread never latches one without the other. */
void app_publish_size_serial(App *a, int w_pt, int h_pt, int s120,
							 uint32_t serial);
/* A window minimized: no size, which is how the frame loop is told to idle.
 * cur_* and win_* keep the last good one, so restoring costs one
 * configure. */
void app_publish_hidden(App *a);
/* A surface scale that may have moved, published for the render thread and
 * signalled, so a late-latch wait gives way to it. True if it moved. */
bool app_publish_scale(App *a, int s120);
/* Whether the window has the keyboard, for the titlebar's palette. */
void app_publish_focus(App *a, bool focused);
/* FNL_EVENT_FOCUS, after the held set has been cleared or seeded; a loss
 * also disarms a titlebar press (deco_release()). */
void app_post_focus(App *a, bool focused);
/* The output's name and the room it has in points; NULL keeps the name. */
void app_publish_output(App *a, const char *name, int w_pt, int h_pt);
/* The output's name and its own mode in pixels, under one lock, for a
 * backend whose room is not the output's own (Wayland's bounds). */
void app_publish_output_mode(App *a, const char *name, int mode_w,
							 int mode_h);
/* The refresh rate of the window's monitor in millihertz, 0 for unknown. */
void app_publish_refresh(App *a, unsigned int mhz);
/* A window server's monitors, by the names fnl_output_name_at() gives, and
 * the one the window is on (-1 for none known), for the titlebar's output
 * box and the public list. From the pump, whenever either changes: a
 * monitor plugged in or out, or the window moved onto another -- by the
 * titlebar, by fnl_request_output() or by the window server itself. A list
 * with nothing new costs a comparison. Names that are empty are left out,
 * since nothing could ask for them. */
void app_publish_outputs(App *a, char (*names)[OUTPUT_NAME_MAX], int n,
						 int current);
/* Where a window at x, y of size w by h, in the room `from` of the output
 * it is on, goes in the room `to` of another: at the same offset from the
 * room's top-left, pulled back inside where it would overhang, and at the
 * room's top-left on an axis where the window is larger than the room. One
 * rule for every backend that places a window; coordinates are the
 * backend's own, y down. */
void app_place_on_output(int x, int y, int w, int h, const FnlRect *from,
						 const FnlRect *to, int *nx, int *ny);
/* The machine came back from a sleep: the frame rebuilds its swapchain and
 * learns its hold again. From the pump thread. */
void app_resumed(App *a);
/* The backend's surface must be made again before the next frame; from the
 * pump thread, ahead of publishing the size that goes with it. */
void app_surface_remake(App *a);
/* The presentable area in pixels, from the pump thread's own state; any
 * out-parameter may be NULL. */
void app_content_px_rect(const App *a, int *x, int *y, int *w, int *h);
/* The captured pointer's reticle, App::cur_px/cur_py: put it where the click
 * landed, move it by a delta in pixels, keep it inside an area that changed,
 * and publish it -- and App::pointer_captured -- when either moved. Move and
 * clamp publish for themselves. */
void app_reticle_start(App *a);
void app_reticle_move(App *a, double dx_px, double dy_px);
void app_reticle_clamp(App *a);
void app_reticle_publish(App *a);
/* Wait FRAME_WAIT_S for anything that could bring a window back -- a quit,
 * or with `break_on_urgent` an urgent request -- before the loop goes round
 * again: the park of a window nobody can see. Render thread. */
void app_park(App *a, bool break_on_urgent);

/* ---- events.c and echo.c ------------------------------------------------ */

/* Queue one event for the application, stamping its time. Every input path in
 * every backend goes through this; see src/core/events.c. */
void app_event(App *a, FnlEvent *ev);
/* How long ago the input the backend is about to hand in was made, by the
 * window server's or the device's own stamp on it: the start of what
 * fnl_input_latency_ms() measures, for the next counted event app_event()
 * queues within INPUT_STAMP_FRESH. Negative, or past INPUT_AGE_MAX, is no
 * stamp, and that input counts from when the pump read it. Pump thread. */
void app_input_age(App *a, double age);
/* The same for an event that stands for several inputs -- a Win32
 * WM_MOUSEMOVE for every movement since the last -- by their ages summed,
 * the oldest and how many; each is counted. Taken by one event only.
 * Pump thread. */
void app_input_ages(App *a, double sum, double oldest, uint32_t n);
/* fnl_input_latency_ms()'s figure, from what fnl_poll_event() has handed
 * over since the last: with the cost figures' window (`window`), at once
 * where there was none, and at rest after a COST_WINDOW. Render thread. */
void events_latency_publish(App *a, double now, bool window);
/* Diff the frame's published figures against what was last reported and post
 * one notification per change. Called from app_publish_window(), which is the
 * one place every such change is visible; see src/core/events.c. */
void events_post_state(App *a, const FnlDrawState *ds, int area_w_pt,
					   int area_h_pt, int area_w_px, int area_h_px);
/* Whether anything is queued for the application. Asked once a turn by the
 * frame policy, which records a frame for it: on_overlay is where a caller
 * drains, and on_overlay runs only from a re-record. See events.c. */
bool events_pending(App *a);
/* One line on stderr for an event fnl_poll_event() is about to return, and
 * whether the environment asked for that. See src/core/echo.c. */
void echo_event(const App *a, const FnlEvent *ev);
bool echo_wanted_by_env(void);
/* Whether `k` is one of FnlKey's values other than FNL_KEY_UNKNOWN, by
 * echo.c's KEY_NAMES, which tools/check-tables.py holds to every key: the
 * range check for a key a caller hands in, which no list here has to
 * follow the enum for. Any thread. */
bool echo_key_known(FnlKey k);

/* ---- api_overlay.c ------------------------------------------------------ */

/* Put a caller's titlebar in force, on the pump's thread: the height and floor
 * under the lock, then the two backend calls that follow from them. What
 * fnl_titlebar_custom() records a request for. */
void deco_apply_custom(App *a, int height_pt, int min_width_pt);

/* Nothing of a backend's is declared here: every implementation is
 * static and reached through FnlBackendVtbl, so a call added to src/core/
 * cannot compile against one backend and fail to link against the others. */

/* ---- decor.c: the client titlebar's layout --------------------------------
 * All in window points, all pure arithmetic on the constants above plus the
 * title's measured width. Both threads call the first three -- the render
 * thread to draw, the input thread to hit-test -- and they agree because they
 * compute the same function of the same two numbers rather than sharing an
 * answer. */
void deco_button_rects(const App *a, int w, int *min_x, int *max_x,
					   int *full_x, int *close_x, int *by, bool *visible);
/* The caller's run of the titlebar: right-to-left from the library's buttons,
 * granted only while it clears the title. */
void deco_reserved_rect(const App *a, int w, int want_w, int *x, int *y,
						int *rw, int *rh, bool *visible);
/* Whether the title clears the controls at this width, or the button strip
 * where the controls are not shown. A caller's run hides itself to stay off
 * the title; the buttons and the controls do not, so past a point the title
 * is what has to give way. */
bool deco_title_fits(const App *a, int w);
/* The whole hit test, shared by every backend: a point in window points --
 * unrounded, origin at the window's top-left, so the grab ring is outside 0
 * and the size -- to the zone it lands in and, for ZONE_RESIZE, the edge. */
/* A frame reached the screen (fnl_frame_shown()): its FnlFrame::index and
 * the display's stamp in seconds of CLOCK_MONOTONIC, now_seconds()'s
 * clock. Any thread; kept only while the caller counts. api_state.c. */
void app_frame_shown(App *a, uint64_t index, double mono, bool vblank);
void app_frame_take_back(App *a);                                  /* app.c */
/* The same for a swapchain that numbers its presents and says when a
 * number was shown -- DXGI's present count and frame statistics, which
 * tell only the last shown, and Vulkan's present ids with
 * VK_EXT_present_timing, which tell each: the renderer names each
 * present's number as it makes it, with the frame's index, and a number
 * shown with its stamp; a number shown again stamps nothing, and a number
 * going backwards is a new swapchain, whose numbers start again. The last
 * sixteen are kept. `at` is when the present was begun, and a stamp from
 * before it is not when the frame was shown and stamps nothing: WARP in a
 * Windows 11 guest reported frames shown minutes before they were drawn.
 * Render thread. api_state.c. */
void app_present_numbered(App *a, uint64_t index, uint32_t number,
						  double at);
void app_present_number_shown(App *a, uint32_t number, double mono,
							  bool vblank);
/* The window server has answered for the frame with this FnlFrame::index,
 * shown or skipped: what the frame loop's hold waits on
 * (FnlBackendVtbl::display_waits). Main thread. api_state.c. */
void app_frame_answered(App *a, uint64_t index);

Zone deco_hit(App *a, double px, double py, Edge *edge);
/* The pointer is at this point: deco_hit(), plus the hover and the open
 * list's row. deco_publish_hover() alone is for a pointer that has left. */
Zone deco_pointer_at(App *a, double px, double py, Edge *edge);
void deco_publish_hover(App *a, Zone z);
/* The left button's bindings, acted on through FnlBackendVtbl. A backend
 * posts its FNL_EVENT_BUTTON first, then says press or release; and says
 * every uncaptured motion to deco_drag(), which starts the window move once
 * a held titlebar press has travelled DECO_DRAG_SLOP. */
void deco_press(App *a, double px, double py, uint32_t time_ms);
void deco_release(App *a);
void deco_drag(App *a, double px, double py);

/* ---- controls.c: the library's own titlebar controls -----------------------
 * All in window points. Both threads call the layout half, for
 * deco_reserved_rect()'s reason. */
/* The run's width as the floor holds it, and at window width w, which has
 * the output box beside it where there is room (ctl_output_in()). */
int  ctl_run_w(const App *a);
int  ctl_run_w_at(const App *a, int w);
/* Where the run starts at window width w, or -1 where the buttons beside it
 * are hidden or it would cross the bar's left margin. It does not give way
 * to the title, and deco_min_window() holds it, so -1 is a window a
 * compositor let below its floor. */
int  ctl_run_x(const App *a, int w);
void ctl_draw(App *a, const FnlDrawState *ds, int win_w,
			  int area_w, int area_h);
/* The drop-down's open list, drawn from the *end* of the frame rather than
 * with the box it belongs to: it floats over everything, and over is an
 * append order here. What issuing the glyphs before it said. */
FNL_MUST_USE FnlResult ctl_draw_popup(App *a, const FnlDrawState *ds,
									  int win_w);
/* Which control is at a point inside the bar: -1, a checkbox index,
 * CTL_CHECK_COUNT for the size box, one past it for the output box, two
 * past it for the GPU box. */
int  ctl_hit(const App *a, int win_w, int x, int y);
/* Which row of the open drop-down is at a point *anywhere in the window*, or
 * -1. Asked by deco_press() and ctl_motion(), and made modal by deco_hit()
 * ahead of the resize and content tests, because an open list floats over
 * the window and has to be hit where it is drawn. */
int  ctl_popup_hit(const App *a, int win_w, int x, int y);
/* Whether the controls are holding the pointer: true while a drop-down is
 * open and the library's bar it drops from is shown, and then *every* press
 * belongs to them wherever it lands -- the one that picks a row, and the one
 * anywhere else that puts the list away, which is consumed rather than also
 * acting on what is under it. A press on a drop-down's own box opens, shuts
 * or swaps the list instead (ctl_press()). */
bool ctl_modal(const App *a);
void ctl_press(App *a, int win_w, int x, int y, int row) FNL_NONNULL;
void ctl_motion(App *a, int win_w, int x, int y);

/* ---- text_measure.c -------------------------------------------------------
 * The portable half of the text. Safe from any thread: the cell table is
 * written once, before any thread exists, and read-only after. */

/* Decode the baked glyph table. Must run before anything measures or draws
 * text, and before any thread starts. */
FNL_MUST_USE FnlResult text_load_cells(void);
/* The width to *reserve* for a string, in logical points: what no baked scale
 * would draw it wider than. It never reads the scale in force, so both
 * threads get the same answer. There is deliberately no measure in pixels;
 * see the foot of text_measure.c. */
int  text_width(const char *s, TextSize sz);
/* Which baked atlas is nearest this surface scale. */
int  scale_index(int s120);
/* Decode one UTF-8 code point, returning the next position. utf8_decode()
 * says whether the sequence was well formed; utf8_next() is the same walk for
 * drawing, where malformed input is simply U+FFFD. */
const char *utf8_decode(const char *s, uint32_t *out, bool *ok);
const char *utf8_next(const char *s, uint32_t *out);
/* How much of `len` bytes fits in `size` with a terminator, cut on a
 * character boundary. app.c. */
size_t utf8_fit(const char *s, size_t len, size_t size);

/* The other direction: the bytes written, with no terminator, or 0 for what
 * is not a code point -- a surrogate, or past U+10FFFF. Inline because
 * src/backend/keymap.c encodes too and is linked into tools/verifykeymap
 * without src/core. */
static inline int utf8_encode(uint32_t cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp >= 0xd800 && cp <= 0xdfff)
		return 0;
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	if (cp > 0x10ffff)
		return 0;
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}
/* One row of the cell table -- this scale and size, indexed by glyph -- and
 * the cell for a code point in it, or NULL when neither it nor U+FFFD was
 * baked. */
const GlyphCell *cell_row(int si, TextSize sz);
const GlyphCell *cell_for(const GlyphCell *row, uint32_t cp);
/* Whether the scale in force is off the end of the baked ladder, so the
 * atlas nearest it is being stretched rather than matched. */
bool text_resampled(const App *a);

/* ---- drawing the text -----------------------------------------------------
 * Coordinates and sizes are in logical points; strings are UTF-8. Each size
 * is baked for a declared set of code points (SIZE_RANGES in
 * tools/mkfont.c), and one outside it draws U+FFFD. See DESIGN *Text*.
 *
 * Drawing is deferred: text_draw() appends to the frame's glyph buffer and
 * text_end() issues what was appended since it last did, in one draw: once
 * at the end of the frame, and before anything that must cover the glyphs
 * appended so far, as the rectangles' flush is. `color` is already
 * converted for the surface: an entry of App::pal, or a caller's colour
 * through api_overlay.c. All three are in src/core/overlay.c, and
 * text_end() hands on to the running renderer's. */
void text_begin(App *a, uint32_t img);
void text_draw(App *a, int x, int y, TextSize sz, const float color[4],
			   const char *s);
FNL_MUST_USE FnlResult text_end(App *a);

/* ---- overlay.c --------------------------------------------------------------
 * Rectangles and strokes, batched as text is: append, then one draw per kind.
 * Coordinates and widths are in logical points; `color` is as for text. The
 * two flushes and overlay_scissor() hand on to the running renderer's. */
void app_palette_init(App *a);
/* The library's titlebar fill, focused or not, as 8-bit sRGB: for a window
 * server that paints a window's background before any frame can. */
void app_bar_srgb8(bool active, uint8_t rgb[3]);
/* Open and close a recording's three arenas. Everything between them may
 * append; everything outside them is refused rather than written into an image
 * that is no longer this frame's -- see App::overlay_open. */
void overlay_begin(App *a, uint32_t img);
void overlay_end(App *a);
/* A logical-point rectangle as a framebuffer-clipped pixel rectangle. False
 * when nothing of it is left. */
bool overlay_clip(const App *a, int x, int y, int w, int h, FnlRect *out);
/* A logical-point rectangle, clipped to the framebuffer. Solid: the pipeline
 * does not blend, so this writes exactly what it is given. */
void overlay_rect(App *a, int x, int y, int w, int h, const float color[4]);
/* Four of them: a one-point outline just inside the rectangle. */
void overlay_frame(App *a, int x, int y, int w, int h, const float color[4]);
void overlay_stroke(App *a, float x0, float y0, float x1, float y1,
					float width, const float color[4]);
/* Two segments meeting at (x1,y1), as *one* stroke. Two arms drawn
 * separately blend their antialiased fringes over each other at the join,
 * which leaves a brighter pixel on its inner diagonal. */
void overlay_elbow(App *a, float x0, float y0, float x1, float y1,
				   float x2, float y2, float width, const float color[4]);
/* A hollow rectangle through the opposite corners (x0,y0) and (x1,y1), as one
 * stroke, for overlay_elbow()'s reason: four corners are four joins. */
void overlay_box(App *a, float x0, float y0, float x1, float y1,
				 float width, const float color[4]);
/* Draw everything appended since the last flush of that kind, into whatever
 * the running renderer is recording into. A second flush picks up where the
 * first stopped rather than redrawing or rebinding anything. */
void overlay_flush_rects(App *a);
void overlay_flush_shapes(App *a);

/* Restrict what the *next* flush touches to this rectangle, or to the whole
 * framebuffer when it is NULL. What fnl_overlay_clip() reaches; nothing the
 * library draws for itself uses one. */
void overlay_scissor(App *a, const FnlRect *r);

/* ---- draw.c ---------------------------------------------------------------
 * What the library draws for itself, and the geometry both the drawing and
 * the renderer's recording place things by. Render thread. */
int  logical_w(const App *a);
int  logical_h(const App *a);
/* Recompute the pub_* block from what the render thread has just built, and
 * publish it under App::lock. Called once a frame, after the rebuild that
 * settles App::extent and before the frame is recorded, so on_overlay is
 * answered with this frame's figures. */
void app_publish_window(App *a, const FnlDrawState *ds);

void content_px(const App *a, const FnlDrawState *ds, int *x, int *y,
				int *w, int *h);
/* What every renderer sizes the same way: the window's buffer, under the
 * renderer's own limits (a zero `max` axis is no limit), and where a scene
 * rendered smaller lands in the area. See draw.c. */
Extent app_buffer_extent(App *a, Extent min, Extent max);
Extent app_buffer_room(const App *a, Extent e, Extent max);
bool   app_buffer_room_fits(const App *a, Extent e, Extent room);
/* Whether the window's size has stood long enough for images its own size
 * (App::size_at). */
bool   app_buffer_at_rest(const App *a);
void scene_dst_rect(int scene_w, int scene_h, int area_w, int area_h,
					int *x, int *y, int *w, int *h);

/* The whole overlay in one call, in the order the flush invariant requires.
 * The renderer sets where the drawing lands before calling and clears it
 * after. */
FNL_MUST_USE FnlResult draw_overlay(App *a, const FnlDrawState *ds,
									uint32_t img);

/* frame.c -- the frame policy, shared by every renderer. */
void *render_loop(void *arg);
/* [render] Whether a move the library is about to make may be made now:
 * at once with no veil asked for, else once the veil has come down and a
 * frame of it has been presented. Starts the fade where none is under
 * way, and the caller asks again each turn until it answers yes. Takes
 * App::lock, so never asked with it held. */
bool app_veil_gate(App *a);
/* [render] The renderer's answer to App::snap_owed: `px` the frame it just
 * drew, `pitch` bytes a row, `bgra` its byte order, copied and kept as
 * App::snap, BGRA; or a decline, where no frame can be read back here. */
void app_snapshot_set(App *a, const void *px, int w, int h, size_t pitch,
					  bool bgra);
void app_snapshot_decline(App *a);
/* [render] What the window server is told about the buffer about to be
 * committed: its geometry, its scale and, when `cfg_pending`, the ack of the
 * configure it answers. Called by the renderer immediately before the
 * present and nowhere else: the geometry and scale are double-buffered, so
 * sent any earlier they ride whatever commit comes first -- an older frame's,
 * under the older configure. */
void app_surface_sync(App *a, uint32_t cfg_serial, bool cfg_pending);

#endif  /* APP_H */
