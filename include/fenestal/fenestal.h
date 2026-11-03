/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Fenestal -- a window, a device, a frame loop and a text overlay, wrapped
 * around a scene that is yours.
 *
 * ---- scope ----------------------------------------------------------------
 *
 *   - one window, created with the app. Splitting a window handle out belongs
 *     with a second window, because "which window" is only a question once
 *     there is more than one to ask it of.
 *   - events are queued and you drain them, rather than being called back.
 *     One stream carries both what the user did and what the library did
 *     about it, so the two arrive in an order you can rely on. See
 *     fnl_poll_event().
 *   - Wayland, X11, Win32 and Cocoa (macOS) -- and on Linux a bare display
 *     with no window server at all (KMS), where the window is the output --
 *     drawn with Vulkan, OpenGL, Direct3D 12 or Metal.
 *   - the window wears this library's own titlebar, or one you draw in its
 *     place with fnl_titlebar_custom() -- never the window manager's frame,
 *     and never none. Fullscreen drops the bar for as long as it lasts.
 *
 * Nothing below calls exit().
 *
 * ---- stability ------------------------------------------------------------
 *
 * Before 1.0 nothing here is promised. A minor release (0.9 to 0.10) may
 * rename, change or remove anything declared here, at the source and at the
 * binary, and CHANGELOG.md says what moved; a patch release does neither.
 * fnl_init() therefore refuses a header from another minor release, and the
 * shared object's soname carries the minor as well as the major.
 *
 * What is practised already, and becomes the promise at 1.0:
 *
 *   - an enum's values are kept and new values are appended. An event type,
 *     key or cursor you do not recognise is from a newer library: ignore it.
 *     The *_COUNT members are counts, not values, and grow when one is added.
 *   - a struct you pass in (FnlInitDesc, FnlCallbacks) grows at its end, and
 *     its `size` field is how an older caller is told apart.
 *   - a function that is replaced is marked FNL_DEPRECATED, which warns,
 *     before it goes.
 *
 * Two kinds of thing will stay outside the promise, and say so where they
 * are declared: fields marked UNSTABLE, which are test fixtures and may
 * change meaning or stop doing anything in any release, and the text the
 * diagnostics print (echo_events, frame_times), which is for people and the
 * harness, not for parsing.
 *
 * ---- thread affinity ------------------------------------------------------
 *
 *   [main]     the thread that called fnl_init.
 *   [any]      safe from any thread.
 *   [overlay]  only from inside the on_overlay callback, on the thread that
 *              runs it. The overlay calls are these; outside that callback
 *              they draw nothing rather than misbehave.
 *
 * The library owns a render thread of its own once fnl_run() is called. It is
 * not yours to start, stop or join, and the only thing that runs on it that
 * you write is on_record.
 */

#ifndef FENESTAL_H
#define FENESTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hidden by default (-fvisibility=hidden), exported by name. A static build
 * must define FNL_STATIC so that the Windows import attribute is not applied
 * to a symbol that lives in the same image. */
#if defined(_WIN32) && !defined(FNL_STATIC)
#  ifdef FNL_BUILD
#    define FNL_API __declspec(dllexport)
#  else
#    define FNL_API __declspec(dllimport)
#  endif
#elif defined(FNL_BUILD)
#  define FNL_API __attribute__((visibility("default")))
#else
#  define FNL_API
#endif

/* A compile-time assertion, in whichever spelling the including language has:
 * a bare _Static_assert would make this header unusable from C++. */
#ifdef __cplusplus
#  define FNL_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define FNL_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define FNL_VERSION_MAJOR 0
#define FNL_VERSION_MINOR 9
#define FNL_VERSION_PATCH 0
#define FNL_VERSION ((FNL_VERSION_MAJOR << 16) | (FNL_VERSION_MINOR << 8) | \
					 FNL_VERSION_PATCH)

/* A declaration that still works and is going away: it warns where it is
 * used, with the message naming its replacement. */
#if defined(__cplusplus) && __cplusplus >= 201402L
#  define FNL_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(__GNUC__) || defined(__clang__)
#  define FNL_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#  define FNL_DEPRECATED(msg)
#endif

/* The library that actually loaded, which a shared build lets differ from the
 * header that was compiled against. fnl_init compares them. */
FNL_API uint32_t fnl_version(void);                              /* [any] */

/* ---- results -------------------------------------------------------------
 * Negative is failure and zero is success, so `if (r < 0)` is the test and a
 * non-zero success can be added later without breaking a caller. */
typedef enum {
	FNL_OK                   =  0,
	FNL_SKIPPED              =  1,   /* nothing to do; not an error */

	FNL_ERR_UNSUPPORTED      = -1,
	FNL_ERR_NO_DISPLAY       = -2,   /* no compositor / no window server */
	FNL_ERR_NO_DEVICE        = -3,   /* no GPU that can present here */
	FNL_ERR_OUT_OF_MEMORY    = -4,
	FNL_ERR_DEVICE_LOST      = -5,
	FNL_ERR_INVALID_ARGUMENT = -6,
	FNL_ERR_VERSION_MISMATCH = -7,
	FNL_ERR_SYSTEM           = -8,   /* refused; see fnl_last_error */
} FnlResult;

typedef enum {
	FNL_LOG_ERROR = 0, FNL_LOG_WARN = 1, FNL_LOG_INFO = 2, FNL_LOG_DEBUG = 3
} FnlLogLevel;

FNL_API const char *fnl_result_str(FnlResult r);                 /* [any] */
/* The last failure on this thread, in full. Thread-local, and valid until the
 * next Fenestal call on the same thread. */
FNL_API const char *fnl_last_error(void);                        /* [any] */

/* One line, no trailing newline, no ownership. Called from any thread the
 * library owns, the render thread included. */
typedef void (*FnlLogFn)(void *ud, FnlLogLevel lvl, const char *msg);

/* Which renderer draws: Vulkan, OpenGL (4.5 core on Linux, 4.1 on macOS),
 * Direct3D 12 (Windows) or Metal (macOS). A build has one or several --
 * every one its system has -- and the caller says which it has a scene for, in
 * FnlInitDesc::renderer (that one) or ::renderers (any of these). Asking
 * for one this build does not have is FNL_ERR_UNSUPPORTED rather than a
 * fallback; fnl_renderer() reports which it is, and the matching interop
 * header is fnl_vulkan.h, fnl_gl.h, fnl_d3d12.h or fnl_metal.h. */
typedef enum {
	FNL_RENDERER_DEFAULT = 0, FNL_RENDERER_VULKAN = 1, FNL_RENDERER_OPENGL = 2,
	FNL_RENDERER_D3D12 = 3, FNL_RENDERER_METAL = 4
} FnlRenderer;

/* A renderer's bit in FnlInitDesc::renderers. ANY is every renderer, for a
 * caller that draws no scene of its own -- the overlay alone -- and so has
 * no API to name. */
#define FNL_RENDERER_BIT(g) (1u << (unsigned)(g))
#define FNL_RENDERERS_ANY   0xffffffffu

typedef struct FnlApp FnlApp;

typedef struct {
	size_t   size;             /* sizeof(FnlInitDesc) -- see the note below */
	uint32_t header_version;   /* pass FNL_VERSION */

	FnlLogFn log;              /* NULL: one line to stderr */
	void    *log_ud;

	const char *title;         /* UTF-8; NULL for the default */
	int         width_pt, height_pt;  /* presentable area; 0 for the default */

	/* Install handlers for SIGINT/SIGTERM/SIGHUP, so that a captured pointer
	 * can never outlive the process. Off by default: a library that takes
	 * them unasked has stolen its host's shutdown. */
	bool install_signal_handlers;

	/* Which graphics API: that one, or FNL_ERR_UNSUPPORTED. DEFAULT is
	 * whatever ::renderers below allows, and with that zero too, the build's
	 * own renderer where it has one -- a build of several refuses, since a
	 * caller that wrote one scene must not be handed another API. */
	FnlRenderer renderer;

	/* Hold a frame's state until most of the display period it will sit
	 * through has already passed, so what reaches the screen is that much
	 * newer. On unless disabled, and self-tuning unless pinned:
	 * late_latch_fixed above zero holds that delay in seconds instead. The
	 * self-tuned delay differs between window servers and is expected to. */
	bool   no_late_latch;
	double late_latch_fixed;          /* UNSTABLE: a tuning fixture */

	/* Run at this surface scale instead of the one the desktop asks for, as
	 * 120ths (120 = 100%). Zero means the desktop's own preference. A test
	 * fixture rather than a setting -- it exists to put the client through a
	 * density this desktop does not have. UNSTABLE. */
	int scale_override120;

	/* Which GPU, on a machine that has more than one. Zero is automatic: the
	 * highest-ranked device that can present to this window, discrete ahead of
	 * integrated -- except Mesa's NVK on an NVIDIA GPU older than Turing,
	 * which runs at its boot clocks there and ranks after integrated.
	 *
	 * Above zero it is a one-based index over the devices that can present, in
	 * the order the driver enumerates them -- which is *not* the order the
	 * automatic choice ranks them in -- or, once a ranking has figures for
	 * the display, in the ranked list's (below). The library logs that
	 * numbering at startup, a line per device, and fnl_gpu_count() and
	 * fnl_gpu_name_at() read the list back once there is a window
	 * (*choosing the GPU*). An index past the end warns and falls back to
	 * automatic rather than failing, so read back fnl_gpu_name(). UNSTABLE:
	 * the numbering is the driver's and moves (see gpu_name), so this is for
	 * a test that knows the machine; gpu_name is the stable way to choose. */
	int gpu_index;

	/* The same choice by name, for where a number does not hold still: any
	 * part of the device's name as the startup log prints it, letter case
	 * aside -- "intel", "1050". The order of enumeration is the driver's and
	 * Windows', and on a hybrid laptop it follows what the *executable* asks
	 * of them (NvOptimusEnablement), so GPU 1 of one program is GPU 2 of the
	 * next. NULL or empty is no choice; a name wins over gpu_index, and a
	 * name no device carries warns and falls back as an index does.
	 *
	 * On the KMS backend either may name a GPU that does not drive the
	 * display -- a laptop's NVIDIA GPU beside the Intel one with the panel.
	 * Its frames are then read back and copied onto the display, a few
	 * milliseconds a frame more than the display's own GPU takes.
	 *
	 * With neither set the environment's FNL_GPU is read as one or the
	 * other -- 1 to 8 is an index and anything else a name -- which is a
	 * way to steer a program that has no option for it.
	 *
	 * On the KMS backend FNL_GPU_DRIVER in the environment offers a move
	 * of an NVIDIA GPU between nouveau and NVIDIA's own kernel driver with
	 * the window open: an entry more in the list for each renderer,
	 * "<GPU>, on <driver>", which fnl_request_gpu() takes, the window then
	 * drawn on the GPU with that entry's renderer (fnl_gpu_renderer_at()).
	 * "nouveau" or "nvidia" also binds that one before the first
	 * frame, and "switch" leaves the GPU alone. The move is a root
	 * helper's, asked through a socket or through sudo (DESIGN *Another
	 * kernel driver, with the window open*). Under a window server, which
	 * may hold the GPU itself, it is refused with a line.
	 *
	 * The GPU list is ranked by each entry's measured frame rate on each
	 * display and mode, kept in the user's cache directory per program and
	 * backend (the GPU box's last row measures them again):
	 * an entry with no figure for the display the window is on is measured
	 * once the window is up -- the window says so with the time left and
	 * takes no input meanwhile, some 4 s an entry -- and a GPU or display
	 * taken away keeps its figures. The list is ordered by the window's
	 * display's, best first, and a window that names no GPU opened on its
	 * first entry; gpu_index and an index in FNL_GPU then count that list,
	 * a name as before, and each still stands once a calibration ends: the
	 * window goes to that entry of the list, or back to the GPU named,
	 * where one asking for nothing goes to the first. FNL_GPU_RANK in the
	 * environment: calibrate
	 * measures again; keep uses a ranking and never measures; order orders
	 * the list and moves nothing; off reads none (DESIGN *Ranked by
	 * measured frame rate*). */
	const char *gpu_name;

	/* Report where each frame's wall clock went, a line each half second,
	 * through the log sink: the pacing throttle, the wait on the device, the
	 * recording, the submit and the present. A diagnostic, off by default.
	 * `wait` is the CPU sitting out work the GPU has already been given, so a
	 * frame that is slow *on the GPU* is slow there and nowhere else. The
	 * same figures as totals to read and subtract: fnl_frame_stats(). */
	bool frame_times;

	/* Print every event fnl_poll_event() returns, one line each on stderr,
	 * with the window's state stamped on the end:
	 *
	 *     EV motion 530.0 454.0 | area 1280x720 fs 0 max 0 cap 0 scale 120
	 *                             out HDMI-1 3840x2160
	 *
	 * (one line). For a test harness driving the window from outside.
	 * Setting FNL_ECHO_EVENTS=1 in the environment does the same for a
	 * program with no line for it. A diagnostic: the format is the harness's
	 * in tools/drive and is not a stable interface. */
	bool echo_events;

	/* The renderers the caller has a scene for, as FNL_RENDERER_BIT()s, where
	 * `renderer` is DEFAULT. fnl_init() tries those this build has in its own
	 * order -- on Windows Direct3D 12, Vulkan; on macOS Metal, Vulkan,
	 * OpenGL; on Linux Vulkan, OpenGL -- and steps past one that fails on
	 * this machine to the next, on the same window. fnl_renderer() says
	 * which it took, from on_gpu_created on. FNL_RENDERER in the
	 * environment ("vulkan", "gl", "d3d12", "metal") names one of them
	 * instead, which is taken or fails. */
	uint32_t renderers;
} FnlInitDesc;

/* Every descriptor's first field is its own sizeof, which is the ABI story
 * for the shared build: a caller compiled against an older header passes a
 * smaller struct, and the library reads only the fields that existed then and
 * defaults the rest. A size it does not recognise is
 * FNL_ERR_VERSION_MISMATCH. */

/* ---- the frame -----------------------------------------------------------
 *
 * Where the scene is drawn, which decides what a pipeline must be compatible
 * with. A pipeline belongs to the render pass it was built against, and to
 * its sample count, so content that can be scaled or multisampled needs one
 * per target. Build all three at on_gpu_created time and index them by what
 * FnlFrame reports.
 *
 * Which one a frame uses is the library's to decide, from the render scale and
 * the antialiasing mode. */
typedef enum {
	FNL_TARGET_DIRECT       = 0,  /* the swapchain, single-sampled */
	FNL_TARGET_OFFSCREEN    = 1,  /* an image the frame then scales onto it */
	FNL_TARGET_OFFSCREEN_MS = 2,  /* multisampled, resolved into that image */
	FNL_TARGET_COUNT              /* a count; grows if a target is added */
} FnlSceneTarget;

/* What on_record and on_overlay are drawing for. `index` and `time` are
 * the frame this recording is made for, and a renderer that keeps its
 * recordings shows one on later frames too: neither is a clock the picture
 * follows. Anything that moves goes by scene_time, or by fnl_invalidate(). */
typedef struct {
	uint64_t index;                  /* presents, all time */
	double   time;                   /* seconds since the window opened */

	int      area_w_px, area_h_px;   /* the presentable area */
	/* What the scene is being rendered at. Equal to the area under
	 * FNL_TARGET_DIRECT; smaller when a render scale is pinned. */
	int      scene_w_px, scene_h_px;

	FnlSceneTarget target;
	int      samples;                /* what FSAA actually got, or 1 */

	/* Seconds into the scene's animation, or 0 while motion is off. Use this
	 * rather than `time` for anything that moves: a change in it is a reason
	 * to record the frame, so frozen at zero a still window records
	 * nothing. */
	float    scene_time;
} FnlFrame;

/* Antialiasing. The values are pinned for FnlKey's reason; what refuses the
 * two modes is with fnl_set_antialias() further down. */
typedef enum {
	FNL_AA_NONE = 0,
	FNL_AA_MSAA = 1,     /* multisampling; antialiases geometry coverage */
	FNL_AA_MLAA = 2,     /* a post-process over the finished scene       */
} FnlAntialias;

/* ---- input ----------------------------------------------------------------
 *
 * A key by physical *position*, not by what it prints: W is the forward key on
 * a QWERTY keyboard and the same physical key on AZERTY, where it prints Z.
 * Text entry wants the opposite, and has an event of its own that applies the
 * layout: FNL_EVENT_TEXT, under fnl_set_text_input() further down.
 *
 * So every name below is a place, spelled as what a US keyboard prints there:
 * FNL_KEY_Q is the key right of Tab whatever the layout makes of it, and
 * FNL_KEY_INTL_BACKSLASH is the one between left Shift and Z that a US board
 * does not have at all. The list is the 104-key board plus that one; media
 * keys, F13 and up and the Japanese and Korean extras arrive as
 * FNL_KEY_UNKNOWN.
 *
 * The values are pinned, because a caller stores them in its own config and
 * links against a shared object it did not compile. New keys are added at the
 * end; none is ever inserted or renumbered. */
typedef enum {
	FNL_KEY_UNKNOWN        =   0,
	FNL_KEY_W              =   1,
	FNL_KEY_A              =   2,
	FNL_KEY_S              =   3,
	FNL_KEY_D              =   4,
	FNL_KEY_UP             =   5,
	FNL_KEY_DOWN           =   6,
	FNL_KEY_LEFT           =   7,
	FNL_KEY_RIGHT          =   8,
	FNL_KEY_LSHIFT         =   9,
	FNL_KEY_RSHIFT         =  10,
	FNL_KEY_ESCAPE         =  11,
	FNL_KEY_F1             =  12,
	FNL_KEY_F2             =  13,
	FNL_KEY_F9             =  14,
	FNL_KEY_F10            =  15,
	FNL_KEY_F11            =  16,
	FNL_KEY_B              =  17,
	FNL_KEY_C              =  18,
	FNL_KEY_E              =  19,
	FNL_KEY_F              =  20,
	FNL_KEY_G              =  21,
	FNL_KEY_H              =  22,
	FNL_KEY_I              =  23,
	FNL_KEY_J              =  24,
	FNL_KEY_K              =  25,
	FNL_KEY_L              =  26,
	FNL_KEY_M              =  27,
	FNL_KEY_N              =  28,
	FNL_KEY_O              =  29,
	FNL_KEY_P              =  30,
	FNL_KEY_Q              =  31,
	FNL_KEY_R              =  32,
	FNL_KEY_T              =  33,
	FNL_KEY_U              =  34,
	FNL_KEY_V              =  35,
	FNL_KEY_X              =  36,
	FNL_KEY_Y              =  37,
	FNL_KEY_Z              =  38,
	FNL_KEY_0              =  39,   /* the digit row, not the keypad */
	FNL_KEY_1              =  40,
	FNL_KEY_2              =  41,
	FNL_KEY_3              =  42,
	FNL_KEY_4              =  43,
	FNL_KEY_5              =  44,
	FNL_KEY_6              =  45,
	FNL_KEY_7              =  46,
	FNL_KEY_8              =  47,
	FNL_KEY_9              =  48,
	FNL_KEY_F3             =  49,
	FNL_KEY_F4             =  50,
	FNL_KEY_F5             =  51,
	FNL_KEY_F6             =  52,
	FNL_KEY_F7             =  53,
	FNL_KEY_F8             =  54,
	FNL_KEY_F12            =  55,
	FNL_KEY_SPACE          =  56,
	FNL_KEY_ENTER          =  57,
	FNL_KEY_TAB            =  58,
	FNL_KEY_BACKSPACE      =  59,
	FNL_KEY_MINUS          =  60,
	FNL_KEY_EQUAL          =  61,
	FNL_KEY_LBRACKET       =  62,
	FNL_KEY_RBRACKET       =  63,
	FNL_KEY_BACKSLASH      =  64,
	FNL_KEY_SEMICOLON      =  65,
	FNL_KEY_APOSTROPHE     =  66,
	FNL_KEY_GRAVE          =  67,   /* left of 1 */
	FNL_KEY_COMMA          =  68,
	FNL_KEY_PERIOD         =  69,
	FNL_KEY_SLASH          =  70,
	FNL_KEY_INTL_BACKSLASH =  71,   /* between left Shift and Z */
	FNL_KEY_LCTRL          =  72,
	FNL_KEY_RCTRL          =  73,
	FNL_KEY_LALT           =  74,
	FNL_KEY_RALT           =  75,
	FNL_KEY_LSUPER         =  76,   /* the Windows or Command key */
	FNL_KEY_RSUPER         =  77,
	FNL_KEY_MENU           =  78,   /* the context-menu key */
	FNL_KEY_INSERT         =  79,
	FNL_KEY_DELETE         =  80,
	FNL_KEY_HOME           =  81,
	FNL_KEY_END            =  82,
	FNL_KEY_PAGE_UP        =  83,
	FNL_KEY_PAGE_DOWN      =  84,
	FNL_KEY_CAPS_LOCK      =  85,
	FNL_KEY_NUM_LOCK       =  86,
	FNL_KEY_SCROLL_LOCK    =  87,
	FNL_KEY_PRINT_SCREEN   =  88,
	FNL_KEY_PAUSE          =  89,
	FNL_KEY_KP_0           =  90,   /* the keypad, whatever Num Lock says */
	FNL_KEY_KP_1           =  91,
	FNL_KEY_KP_2           =  92,
	FNL_KEY_KP_3           =  93,
	FNL_KEY_KP_4           =  94,
	FNL_KEY_KP_5           =  95,
	FNL_KEY_KP_6           =  96,
	FNL_KEY_KP_7           =  97,
	FNL_KEY_KP_8           =  98,
	FNL_KEY_KP_9           =  99,
	FNL_KEY_KP_DECIMAL     = 100,
	FNL_KEY_KP_DIVIDE      = 101,
	FNL_KEY_KP_MULTIPLY    = 102,
	FNL_KEY_KP_SUBTRACT    = 103,
	FNL_KEY_KP_ADD         = 104,
	FNL_KEY_KP_ENTER       = 105,
} FnlKey;

typedef enum {
	FNL_BUTTON_LEFT   = 0,
	FNL_BUTTON_MIDDLE = 1,
	FNL_BUTTON_RIGHT  = 2,
} FnlButton;

/* Two groups on one queue. *Input* is something the user did, delivered in the
 * order it happened. *Notification* is something the library did that you can
 * see, posted after it took effect.
 *
 * Numbered, appended to and never renumbered, for the reason FnlKey gives.
 * A type you do not recognise is one from a newer library than your header:
 * ignore it. Nothing here is ever removed. */
typedef enum {
	/* ---- input ---- */
	FNL_EVENT_KEY        = 0,   /* ev.key    */
	FNL_EVENT_BUTTON     = 1,   /* ev.button */
	FNL_EVENT_MOTION     = 2,   /* ev.motion -- where the pointer is */
	FNL_EVENT_RELATIVE   = 3,   /* ev.relative -- how far, while captured */
	FNL_EVENT_SCROLL     = 4,   /* ev.scroll */
	FNL_EVENT_FOCUS      = 5,   /* ev.focus  */
	FNL_EVENT_CLOSE      = 6,   /* the window was asked to close */

	/* ---- what the library did ----
	 *
	 * Each is posted once, after the fact, and only when the value actually
	 * moved -- these are edges, not a per-frame report -- so an application
	 * never has to poll a getter to notice. */
	FNL_EVENT_RESIZE     = 7,   /* ev.resize    -- the presentable area */
	FNL_EVENT_SCALE      = 8,   /* ev.scale     -- surface scale */
	FNL_EVENT_WINDOW     = 9,   /* ev.window    -- fullscreen/maximized */
	FNL_EVENT_ANTIALIAS  = 10,  /* ev.antialias -- FSAA/MLAA/off */
	FNL_EVENT_SCENE_SIZE = 11,  /* ev.scene     -- the render-scale pin */
	FNL_EVENT_CAPTURE    = 12,  /* ev.capture   -- taken, or let go */

	/* The queue overflowed and this many events were dropped. Posted once,
	 * in their place, so a gap in the stream is something you are told about
	 * rather than something you infer. See fnl_poll_event(). */
	FNL_EVENT_LOST       = 13,  /* ev.lost */

	/* Input again, and down here because the list is append-only: what a key
	 * typed, once the layout has had its say. Delivered only while text entry
	 * is on -- see fnl_set_text_input(). */
	FNL_EVENT_TEXT       = 14,  /* ev.text */
	/* The text being composed changed: an input method's candidate, or the
	 * accent a dead key is holding. Not typed yet and may never be -- read it
	 * with fnl_compose_text() and draw it as provisional. */
	FNL_EVENT_COMPOSE    = 15,  /* ev.compose */
	/* What fnl_clipboard_request() asked for has arrived. Read it with
	 * fnl_clipboard_text(). */
	FNL_EVENT_CLIPBOARD  = 16,  /* ev.clipboard */
	/* A notification, on the terms of the ones above: the window is on
	 * another output now, or its output changed size. Read the name with
	 * fnl_output_name(). Since 0.9. */
	FNL_EVENT_OUTPUT     = 17,  /* ev.output */
	/* A notification, on the same terms: the window is drawn on another GPU
	 * -- fnl_request_gpu(), or a rebuild after a loss that landed
	 * elsewhere -- or the list grew, by the other renderers' GPUs listed
	 * after the first frame. Read the list again (fnl_gpu_count()). After
	 * a move it follows the dissolve (fnl_set_transition_ms()). Since
	 * 0.9. */
	FNL_EVENT_GPU        = 18,  /* ev.gpu */
} FnlEventType;

/* Each event's own fields, one struct a kind, named so that FnlEvent's union
 * holds members and declares no type: C++ forbids a type declared inside an
 * anonymous union, which clang++ says under -pedantic and g++ does not.
 * Read through the event -- ev.key.pressed -- as ever; the names are there
 * for a caller that hands one kind of event on by itself. */
typedef struct {
	FnlKey key;
	bool   pressed;
	/* An auto-repeat rather than a fresh press. Always false unless text
	 * entry is on. Under fnl_set_text_input() a held key comes round again
	 * as a press with this set and no release between, at the rate the
	 * desktop is configured for. The library's own bindings never act on
	 * one; a caller that toggles on a press should test this whenever it
	 * also takes text. */
	bool   repeat;
} FnlKeyEvent;

typedef struct {
	double    x, y;
	FnlButton button;
	bool      pressed;
} FnlButtonEvent;

typedef struct { double x, y; } FnlMotionEvent;
/* How far a captured pointer moved, in window points. Unaccelerated -- the
 * hand's movement, what a camera turned by the mouse wants -- on Wayland
 * (the relative pointer's unaccelerated delta, or its accelerated one where
 * the compositor has none), Win32 (raw input) and KMS (evdev); the
 * device's counts, so how far a hand's movement goes follows the mouse's
 * resolution. Accelerated, the desktop pointer's own movement, on X11,
 * whose core protocol reports positions alone (raw motion is XInput 2's,
 * which the library does not speak), and on macOS, whose events carry the
 * cursor's deltas. */
typedef struct { double dx, dy; } FnlRelativeEvent;

/* At the pointer, in window points, and by how far in *notches*: one click
 * of a wheel is 1.0 on every backend, and a smooth source such as a
 * touchpad arrives in fractions of one. Positive is down and to the
 * right. */
typedef struct { double x, y, dx, dy; } FnlScrollEvent;

typedef struct { bool focused; } FnlFocusEvent;

/* The presentable area, in logical points and in pixels -- the same pair
 * fnl_content_size_px() and FnlFrame report, so a layout can be rebuilt from
 * this without asking again. */
typedef struct { int w_pt, h_pt, w_px, h_px; } FnlResizeEvent;

/* The surface scale in 120ths: 120 is 100%, 180 is 150%. */
typedef struct { int scale120; } FnlScaleEvent;

typedef struct { bool fullscreen, maximized; } FnlWindowEvent;
typedef struct { FnlAntialias aa; } FnlAntialiasEvent;

/* The render-scale pin in pixels, or 0 by 0 once it is let go and the scene
 * is drawn straight into the window again. */
typedef struct { int w_px, h_px; } FnlSceneEvent;

typedef struct { bool captured; } FnlCaptureEvent;

/* How many events were dropped where this one sits. */
typedef struct { unsigned count; } FnlLostEvent;

/* One code point, both ways round: as a number, for a caller that filters
 * or maps what is typed, and as UTF-8 with its terminator, for one that
 * appends it to a string and wants to do nothing else. Never a control
 * character -- Enter, Tab, Backspace and Escape are keys and arrive as
 * FNL_EVENT_KEY. */
typedef struct { uint32_t codepoint; char utf8[8]; } FnlTextEvent;

/* The composition's and the clipboard's: a length and not the text, because
 * neither has an upper bound and an event is a fixed size -- the bytes
 * fnl_compose_text() and fnl_clipboard_text() will hand over, terminator
 * not counted. Zero is a composition that ended, or a clipboard with no
 * text in it. */
typedef struct { unsigned length; } FnlLengthEvent;

/* The output's size in logical points, as fnl_output_size_pt() reports
 * it. */
typedef struct { int w_pt, h_pt; } FnlOutputEvent;

/* The GPU now in use, as an index into fnl_gpu_count()'s list, and the
 * renderer drawing on it. */
typedef struct { int index; FnlRenderer renderer; } FnlGpuEvent;

/* Pointer positions are in *logical points*, with the origin at the window's
 * top-left -- the same space the overlay draws in, so a hit test compares
 * against the rectangle it drew without converting anything.
 *
 * FNL_EVENT_MOTION stops while the pointer is captured and FNL_EVENT_RELATIVE
 * takes over: a captured pointer has no position to report. There is no
 * warp; the application owns where its reticle is and moves it by these
 * deltas. */
typedef struct {
	FnlEventType type;
	/* How long before `time` the oldest input this event carries happened,
	 * in seconds, so `time - input_age` is when: by the device's own stamp
	 * where the library shares its clock, and 0 where it does not, the
	 * input then counted from when the library read it -- what
	 * fnl_input_latency_ms() counts. A coalesced motion carries every
	 * movement since the last. 0 for what no hand did: a notification,
	 * text, a key's repeat. In what was the padding after `type`, so the
	 * struct is the size it was. Since 0.9. */
	float        input_age;
	double       time;          /* the same clock FnlFrame::time is on */
	union {
		FnlKeyEvent       key;
		FnlButtonEvent    button;
		FnlMotionEvent    motion;
		FnlRelativeEvent  relative;
		FnlScrollEvent    scroll;
		FnlFocusEvent     focus;
		FnlResizeEvent    resize;
		FnlScaleEvent     scale;
		FnlWindowEvent    window;
		FnlAntialiasEvent antialias;
		FnlSceneEvent     scene;
		FnlCaptureEvent   capture;
		FnlLostEvent      lost;
		FnlTextEvent      text;
		FnlLengthEvent    compose;
		FnlLengthEvent    clipboard;
		FnlOutputEvent    output;
		FnlGpuEvent       gpu;
	};
} FnlEvent;

/* The library fills this struct into storage the *caller* declared, so its
 * size is a contract between two separately compiled objects. To grow it:
 * append to the union, bump FNL_VERSION_MINOR, update this and the ABI
 * baseline. */
FNL_STATIC_ASSERT(sizeof(FnlEvent) == 48 || sizeof(FnlEvent) == 40,
				  "FnlEvent changed size -- that is an ABI break; see the note "
				  "above, then update this assertion deliberately");

/* What a caller plugs into the frame. There is no input callback: input is a
 * queue you drain -- see fnl_poll_event().
 *
 * on_gpu_created / on_gpu_lost bracket everything that belongs to the device.
 * They are called once at startup and again around every device loss, and
 * nothing built in the first is valid after the second. They strictly
 * alternate: on_gpu_lost arrives once for a loss however many attempts the
 * rebuild takes, and on_gpu_created only when there is a device to hand back,
 * so a handler need not be idempotent. The outermost pair -- the first
 * on_gpu_created and, on an ordinary close, the last on_gpu_lost -- runs on
 * the thread that called fnl_run(); every pair in between runs on the render
 * thread.
 *
 * on_record is the seam. It runs on the render thread with the scene's pass
 * already begun and the viewport set, and what it records is the scene. Get
 * what to record into from the matching interop header -- fnl_vulkan.h --
 * which is also where the layout to build pipelines against comes from.
 *
 * It runs whenever the frame is recorded: at least each time something it
 * could draw from has moved -- the window's state, FnlFrame's sizes, target
 * and scene_time, an fnl_invalidate(), a waiting event -- and how much more
 * often is the renderer's. One that keeps its recordings submits
 * them again while nothing moves, and records once for each image it
 * presents from after something does; one that cannot keep a recording calls
 * on_record for every frame it shows. The interop header says which. So
 * draw what your state says, move anything by elapsed time rather than per
 * call, and never count calls.
 *
 * on_overlay is where the overlay calls belong, and the only callback that
 * may make them: the scene may be drawn into an offscreen target, while the
 * overlay is always drawn onto the window afterwards, in a different render
 * pass. It runs on the render thread, after on_record and after the library's
 * own titlebar, with the frame the scene was drawn for -- in the same
 * recording, so as often as on_record and on the same terms.
 *
 * There is never more than one thread inside a callback. Any of them may use
 * the knobs and fnl_poll_event(); none may call fnl_run() or
 * fnl_shutdown(). */
typedef struct {
	size_t size;
	void  *ud;

	void (*on_gpu_created)(void *ud, FnlApp *app);
	void (*on_gpu_lost)(void *ud, FnlApp *app);
	void (*on_record)(void *ud, FnlApp *app, const FnlFrame *frame);
	void (*on_overlay)(void *ud, FnlApp *app, const FnlFrame *frame);
} FnlCallbacks;

FNL_API FnlResult fnl_init(const FnlInitDesc *desc, FnlApp **out);  /* [main] */
/* After fnl_run() has returned. Called from a callback it logs a line and
 * does nothing, rather than wait for the render thread from the render
 * thread or free the app fnl_run() is still running on. */
FNL_API void      fnl_shutdown(FnlApp *app);                        /* [main] */

/* Pump the backend on this thread and the frame loop on one of the library's
 * own, until the window closes or fnl_quit() is called. Returns FNL_OK on an
 * ordinary close.
 *
 * On the thread that called fnl_init(), or FNL_ERR_INVALID_ARGUMENT. That
 * should be the thread that entered main(), and on macOS it has to be:
 * AppKit is the main thread's, and fnl_init() refuses any other there. */
FNL_API FnlResult fnl_run(FnlApp *app, const FnlCallbacks *cb);     /* [main] */
FNL_API void      fnl_quit(FnlApp *app);                            /* [any]  */

/* ---- the event queue ------------------------------------------------------
 *
 * Take the next event, or return false when there are none left. Drain it in a
 * loop; what you do not take stays queued.
 *
 *     FnlEvent ev;
 *     while (fnl_poll_event(app, &ev)) {
 *         switch (ev.type) {
 *         case FNL_EVENT_KEY:       ...
 *         case FNL_EVENT_ANTIALIAS: ...
 *         default: break;           // a newer library may add types
 *         }
 *     }
 *
 * **Drain it every frame**, from on_overlay: a waiting event is itself a
 * reason to record the frame, so it runs whenever there is something to
 * take, and it is the turn in which you draw what the input changed. The
 * queue holds a few hundred events; an application that stops draining is
 * told what it lost, through FNL_EVENT_LOST.
 *
 * A program whose input moves its *scene* drains at the top of on_record as
 * well: on_record runs before on_overlay, so a camera turned by
 * FNL_EVENT_RELATIVE in on_overlay is drawn a frame late. Both run on the same
 * thread, and testbed is written that way. Either callback being
 * registered is enough for a waiting event to bring a frame.
 *
 * Order is the order things happened, with one exception: a notification is
 * posted where the library noticed the change, which for anything measured
 * per frame is the top of the next frame. So a click on a titlebar control
 * arrives before the FNL_EVENT_ANTIALIAS it caused, and may arrive a frame
 * before it.
 *
 * Motion is coalesced. Consecutive FNL_EVENT_MOTION collapse to the newest
 * position and consecutive FNL_EVENT_RELATIVE sum their deltas, so a 1000 Hz
 * mouse cannot flood a 60 Hz drain. Every other type is kept whole.
 *
 * [any] thread, but drain from one: two threads draining it each get some of
 * the events. */
FNL_API bool fnl_poll_event(FnlApp *app, FnlEvent *out);            /* [any] */

/* The device's name. A per-thread snapshot, on the same terms as
 * fnl_output_name() below: valid until the next call on that thread. It is
 * not fixed for the window's life -- device-loss recovery may land on a
 * different device. "" with no window, and "(no device)" with a window
 * whose renderer has not yet picked one; the name stays the last device's
 * while a lost one is replaced. */
FNL_API const char *fnl_gpu_name(const FnlApp *app);                /* [any] */

/* The same name made short enough for a title: the vendor's "(R)" and
 * "(TM)", Mesa's "Mesa " in front and the driver's detail behind left out
 * -- "Mesa Intel(R) HD Graphics 530 (SKL GT2)" is "Intel HD Graphics 530",
 * "llvmpipe (LLVM 21.1.8, 256 bits)" is "llvmpipe", and "NVIDIA GeForce
 * GTX 1050 Ti/PCIe/SSE2" is "NVIDIA GeForce GTX 1050 Ti". For
 * showing; fnl_gpu_name() is the one to match against. A per-thread
 * snapshot, on fnl_gpu_name()'s terms. */
FNL_API const char *fnl_gpu_short_name(const FnlApp *app);          /* [any] */
/* Which renderer this window is drawn with, for a caller deciding which
 * interop header to reach for. With no window, the build's renderer where it
 * has one and DEFAULT where it has several. */
FNL_API FnlRenderer fnl_renderer(const FnlApp *app);                /* [any] */

/* ---- choosing the GPU ------------------------------------------------------
 *
 * The GPUs this window can be drawn on -- hardware, and a software
 * rasteriser where one is installed (lavapipe, WARP) -- by every renderer
 * the program has a scene for (FnlInitDesc::renderers): first those of the
 * renderer the window started with, in the order it lists them, which is
 * the order FnlInitDesc::gpu_index and an index in FNL_GPU count from 1;
 * then each other renderer's, in the order fnl_init() tries them. Here
 * they count from 0. The window's own renderer lists at startup; the
 * others are listed once, after the first frame, which loads their
 * drivers. The list is taken again at every rebuild, so it can change
 * across one -- and on KMS when a GPU is plugged in or out with the window
 * open, which is a rebuild -- and FNL_EVENT_GPU says when to read it
 * again. Where a ranking
 * by measured frame rate is kept (FNL_GPU_RANK, see gpu_name) the list is
 * ordered by it instead, the measured entries first, best first.
 *
 * fnl_request_gpu() moves the window onto another GPU of the list without
 * closing it: the device is rebuilt there as after a loss, on_gpu_lost and
 * on_gpu_created run in between, and FNL_EVENT_GPU arrives once frames are
 * drawn on it. A GPU of another renderer moves the window to that renderer
 * too, the one drawing torn down and the other built on the same window,
 * so on_gpu_created asks fnl_renderer() which scene to build. A GPU that
 * will not build is gone back from, with one line through the log sink.
 * An index past the list, or the GPU in use, does nothing. The choice
 * holds for later rebuilds as gpu_index does. OpenGL lists the GPUs its
 * driver will open this window on -- every one where the window is the
 * output, and where it is a window server's, those whose driver takes the
 * GPU it is told, which on X11 is none but the server's own. Since 0.9. */
typedef enum {
	FNL_GPU_KIND_UNKNOWN    = 0,   /* hardware the API does not describe */
	FNL_GPU_KIND_INTEGRATED = 1,   /* shares the CPU's memory */
	FNL_GPU_KIND_DISCRETE   = 2,   /* memory of its own */
	FNL_GPU_KIND_VIRTUAL    = 3,   /* a virtual machine's */
	FNL_GPU_KIND_SOFTWARE   = 4,   /* a rasteriser on the CPU */
} FnlGpuKind;

/* How many GPUs the list has, and which is in use: -1 with no window or no
 * device yet. */
FNL_API int fnl_gpu_count(const FnlApp *app);                       /* [any] */
FNL_API int fnl_gpu_current(const FnlApp *app);                     /* [any] */
/* An entry's name, as fnl_gpu_name() gives the one in use, and "" past the
 * end: a per-thread snapshot on fnl_gpu_name()'s terms. And its kind,
 * FNL_GPU_KIND_UNKNOWN past the end. */
FNL_API const char *fnl_gpu_name_at(const FnlApp *app, int i);      /* [any] */
FNL_API FnlGpuKind fnl_gpu_kind_at(const FnlApp *app, int i);       /* [any] */
/* The renderer that draws on entry `i`, FNL_RENDERER_DEFAULT past the
 * end. Since 0.9. */
FNL_API FnlRenderer fnl_gpu_renderer_at(const FnlApp *app, int i);  /* [any] */
/* What entry `i`'s frames reach the window through where that is another
 * graphics API's swapchain, and "" where it is the renderer's own or past
 * the end: "D3D11" for Vulkan on Windows presenting through a DXGI
 * swapchain, which a GPU that can share images with Direct3D 11 lists
 * beside its plain Vulkan entry. A per-thread snapshot on fnl_gpu_name()'s
 * terms. Since 0.9. */
FNL_API const char *fnl_gpu_via_at(const FnlApp *app, int i);       /* [any] */
/* Draw on entry `i` from the next frame on, through the dissolve
 * fnl_set_transition_ms() sets across a move. */
FNL_API void fnl_request_gpu(FnlApp *app, int i);                   /* [any] */

/* The notice and the licence the baked glyph coverage travels under: Fenestal
 * Glyphs Sans and Mono, renamed Modified Versions of Ubuntu Sans and Ubuntu
 * Sans Mono, which the notice names. The Ubuntu Font Licence requires every
 * copy to carry both, which in a library means every binary a host ships.
 * Both point at static storage and are valid before fnl_init(). */
FNL_API const char *fnl_font_notice(void);                          /* [any] */
FNL_API const char *fnl_font_licence(void);                         /* [any] */

/* ---- the overlay ----------------------------------------------------------
 *
 * Three primitives -- solid rectangles, antialiased strokes and text -- for
 * drawing over the scene: a heads-up display, a debug readout, a menu. All of
 * it is 2D, in *logical points* with the origin at the window's top-left, so a
 * layout written once is the same size on a 200% display as on a 100% one.
 *
 * Call these only from on_overlay, and only on the thread it runs on -- not
 * from on_record, which is a different render pass whenever the scene is drawn
 * offscreen. Outside that they draw nothing.
 *
 * Each kind is collected into its own stream and issued as ONE draw for the
 * whole frame, whatever order the calls came in: rectangles first, then
 * strokes, then text. That reordering is only legal under an invariant, and
 * it is yours to keep:
 *
 *     EVERY FILLED RECTANGLE IS A BACKGROUND.
 *
 * Nothing solid may be painted over a stroke or a glyph. Break it and the
 * rectangle lands *under* the thing it was meant to cover -- silently, with no
 * error anywhere and nothing in a validation layer to catch it.
 *
 * fnl_overlay_flush() is the escape hatch. It issues everything appended so
 * far, so anything drawn after it is strictly on top.
 */

/* Straight (non-premultiplied) RGBA, authored in *sRGB* -- the numbers a colour
 * picker gives. Fenestal converts to whatever space the surface blends in,
 * which is not something a caller can determine or should have to. Alpha is
 * linear in both encodings and is passed through. */
typedef struct { float r, g, b, a; } FnlColor;

/* The three baked faces: two monospaced sizes and one proportional. Baked, not
 * scalable -- each is rasterised and grid-fitted at the exact pixel sizes the
 * supported display scales land on -- which is why this is an enum of three
 * rather than a size in points. */
typedef enum {
	FNL_TEXT_BODY  = 0,  /* monospaced; the workhorse */
	FNL_TEXT_LARGE = 1,  /* monospaced; a readout to read across a room */
	FNL_TEXT_UI    = 2,  /* proportional; chrome that is prose, not data */
	FNL_TEXT_SIZE_COUNT  /* a count; grows if a face is added */
} FnlTextSize;

/* Metrics, in logical points, safe from [any] thread.
 *
 * fnl_text_width() returns the width to *reserve* -- what no supported display
 * scale would draw the string wider than -- and not a measurement of what
 * will be drawn; there is no call that gives one.
 *
 * fnl_text_advance() is a per-face cell step and so answers 0 for the
 * proportional size, which has an advance per glyph instead. */
FNL_API int fnl_text_width(const char *utf8, FnlTextSize sz);       /* [any] */
FNL_API int fnl_text_line(FnlTextSize sz);     /* baseline to baseline [any] */
FNL_API int fnl_text_height(FnlTextSize sz);   /* cap height           [any] */
FNL_API int fnl_text_advance(FnlTextSize sz);  /* one cell; mono only  [any] */

/* `y` is the top of the glyph box, not the baseline, so a line of text and a
 * rectangle behind it take the same y. */
FNL_API void fnl_overlay_text(FnlApp *app, int x, int y, FnlTextSize sz,
							  FnlColor c, const char *utf8);     /* [overlay] */

FNL_API void fnl_overlay_rect(FnlApp *app, int x, int y, int w, int h,
							  FnlColor c);                       /* [overlay] */

/* A stroke of `width` points, with round caps and analytic antialiasing.
 * Coordinates are floats because a stroke is geometry rather than a cell. */
FNL_API void fnl_overlay_stroke(FnlApp *app, float x0, float y0,
								float x1, float y1, float width,
								FnlColor c);                     /* [overlay] */

/* Two arms meeting at (x1,y1), as ONE primitive rather than two strokes: two
 * antialiased strokes blend over each other where they meet and leave a
 * visibly bright pixel on the corner. One primitive computes the join's
 * coverage once. */
FNL_API void fnl_overlay_elbow(FnlApp *app, float x0, float y0,
							   float x1, float y1, float x2, float y2,
							   float width, FnlColor c);         /* [overlay] */

/* A closed rectangle outline, from two opposite corners. Its own primitive for
 * the elbow's reason carried further: four joins is one more than a bend can
 * carry. */
FNL_API void fnl_overlay_box(FnlApp *app, float x0, float y0,
							 float x1, float y1, float width,
							 FnlColor c);                        /* [overlay] */

/* Issue everything appended so far, so that what follows draws strictly on
 * top: the escape hatch from the invariant above. */
FNL_API void fnl_overlay_flush(FnlApp *app);                     /* [overlay] */

/* Confine subsequent rectangles and strokes to a rectangle, in logical points.
 * Passing w or h <= 0 restores the whole window. Everything appended before
 * the call is issued first, so it is drawn unclipped.
 *
 * **Text is not clipped by this**: glyphs are ONE draw for the whole frame, so
 * no clip can apply to some of them. The frame takes the clip back when
 * on_overlay returns, issuing what was appended under it first, so a clip
 * never reaches the frame's text and cannot outlive your callback. Where text
 * has to stay inside something, bound what you draw instead. */
FNL_API void fnl_overlay_clip(FnlApp *app, int x, int y,
							  int w, int h);                     /* [overlay] */

/* The window's size in logical points, titlebar included -- the coordinate
 * space every fnl_overlay_* call above is in. Not an [overlay] call: safe
 * from [any] thread, and it answers with the size as of the last frame that
 * was drawn. */
FNL_API void fnl_window_size_pt(const FnlApp *app,
								int *w_pt, int *h_pt);               /* [any] */

/* Tell the library that what your callbacks draw has changed.
 *
 * The frame re-records only when something the library can see has moved --
 * the window resized, the titlebar's focus changed, the scene's clock
 * advanced, an event is queued -- and otherwise draws nothing: the window
 * keeps what it shows until one of those, or this, says it has changed. It
 * cannot see your state: a panel you opened, a checkbox you ticked. Without
 * this the window goes on showing the frame recorded before the click.
 *
 * Cheap and idempotent: several calls between two frames cost one re-record.
 * Safe from [any] thread. Called from inside on_overlay it means "the frame
 * after this one differs from this one". */
FNL_API void fnl_invalidate(FnlApp *app);                           /* [any] */

/* ---- controls in the titlebar ---------------------------------------------
 *
 * Reserve a run of the library's titlebar for controls of your own, and get
 * back where to draw them. The run is laid out right-to-left from just left of
 * the library's buttons; `want_w` is the width wanted, in logical points, and
 * the height and position are the library's. Pass a `want_w` of 0 to give the
 * strip back.
 *
 * False is "there was no room": the run would have overlapped the window
 * title, and the title wins. Draw nothing and hit-test nothing then -- the
 * rectangle is left untouched. Also false with no window, in fullscreen, and
 * once fnl_titlebar_custom() has taken the bar away.
 *
 * Safe from [any] thread. The rectangle is computed from the window width and
 * `want_w` and nothing else, so a draw and a hit test get the same one; across
 * two threads they can be a frame apart on the width while the window is
 * being resized.
 *
 * Calling it also tells the library not to treat that rectangle as titlebar,
 * so a press on your control does not start a window drag. */
FNL_API bool fnl_titlebar_reserve(FnlApp *app, int want_w,
								  int *x, int *y, int *w, int *h);   /* [any] */

/* ---- replacing the titlebar -----------------------------------------------
 *
 * fnl_titlebar_reserve() above lends you a run of the library's bar. This
 * takes the bar instead: the library stops drawing one, you draw whatever you
 * like across the top `height_pt` points of the window, and the geometry
 * follows -- the presentable area starts below your bar and
 * fnl_content_rect_pt() reports it. Call it with a height of 0 to give the bar
 * back.
 *
 * The library keeps the *behaviour*: a press on your bar still drags the
 * window through the window server's own move, with its snap and
 * double-click-to-maximize, the resize edges still work, and the drag still
 * waits out the slop that stops a plain click from moving the window.
 *
 * `min_width_pt` is the width the window refuses to go below, or 0 for none:
 * the library cannot measure your controls, so this is their only
 * protection. The height floor is always your bar's height.
 *
 * [any], and normally called once before fnl_run(). It records a request the
 * pump spends on its next turn, so the bar changes hands on the frame after
 * the call, not inside it. False means the request was not recorded -- there
 * is no window -- and never that the window server declined. */
FNL_API bool fnl_titlebar_custom(FnlApp *app, int height_pt,
								 int min_width_pt);                  /* [any] */

/* Claim a rectangle of your titlebar as yours, so a press there reaches you
 * as an ordinary event instead of starting a window drag. Everything in the
 * bar you do not claim is drag strip.
 *
 * Coordinates are window points, the same space you drew in. Call it from
 * on_overlay, beside the drawing it describes, and call it again every frame:
 * the list is rebuilt per frame, so a control that stops being drawn stops
 * being claimed in the same frame and can never be clicked where it is no
 * longer shown.
 *
 * Returns false if the bar is not yours, if this is not on_overlay, or once
 * more than 64 rectangles have been claimed in one frame, counting
 * fnl_claim()'s below -- the extra is dropped, with one line through the log
 * sink. Group your controls into a few rectangles rather than claiming each
 * glyph.
 *
 * The buttons are yours too. There is no minimize, maximize, fullscreen or
 * close in a bar the library did not draw, so draw your own and act on them
 * with fnl_minimize(), fnl_set_maximized(), fnl_set_fullscreen() and
 * fnl_quit(). */
FNL_API bool fnl_titlebar_claim(FnlApp *app, int x, int y,
								int w, int h);                   /* [overlay] */

/* Claim a rectangle of the window as yours, anywhere in it: a panel of your
 * own drawn over the scene, whose presses are ordinary events and do not
 * capture the pointer. Everything you do not claim in the presentable area
 * is still the scene's, where a click captures as it always did.
 *
 * The same rules as fnl_titlebar_claim(), whose list this shares: window
 * points, from on_overlay, again every frame, and false outside on_overlay
 * or past 64 rectangles in one frame. It works under any titlebar and in
 * fullscreen. Over the library's own bar it does nothing -- the bar's
 * buttons, controls and drag are not yours to take -- and in a bar of your
 * own it is fnl_titlebar_claim(). The resize edges win over a claim at the
 * window's border, and a captured pointer claims nothing until it is
 * released.
 *
 * The pointer's shape over a claim is yours, fnl_set_cursor()'s. */
FNL_API bool fnl_claim(FnlApp *app, int x, int y, int w, int h); /* [overlay] */

/* The eight resize edges, for fnl_begin_resize(). */
typedef enum {
	FNL_EDGE_NONE         = 0,
	FNL_EDGE_TOP          = 1,
	FNL_EDGE_BOTTOM       = 2,
	FNL_EDGE_LEFT         = 3,
	FNL_EDGE_RIGHT        = 4,
	FNL_EDGE_TOP_LEFT     = 5,
	FNL_EDGE_TOP_RIGHT    = 6,
	FNL_EDGE_BOTTOM_LEFT  = 7,
	FNL_EDGE_BOTTOM_RIGHT = 8,
} FnlEdge;

/* Hand the pointer to the window server and let it move or resize the window,
 * exactly as a press on the library's own titlebar does. For a caller that
 * wants to decide for itself; fnl_titlebar_claim() is the easier path and
 * gets the drag slop and the double-click for nothing.
 *
 * Call them from a press, while the button is still down: the request is
 * spent on the pump's next turn and the grab starts from the press the window
 * server last saw, so it does nothing once the button is up. Both return
 * false with no window, in fullscreen, and while the pointer is captured;
 * fnl_begin_resize() also returns false while maximized. */
FNL_API bool fnl_begin_move(FnlApp *app);                            /* [any] */
FNL_API bool fnl_begin_resize(FnlApp *app, FnlEdge edge);            /* [any] */

/* ---- the knobs ------------------------------------------------------------
 *
 * What the window is doing, and what to ask it to do instead. All of them are
 * safe from [any] thread.
 *
 * The setters that reach the window server -- fnl_set_fullscreen(),
 * fnl_set_maximized(), fnl_minimize(), fnl_request_size_pt(),
 * fnl_release_pointer(), fnl_begin_move(), fnl_begin_resize() and
 * fnl_titlebar_custom() -- record a request the event pump spends on its next
 * turn. The change lands after the call returns, and the FNL_EVENT_* that
 * reports it a frame later still.
 *
 * None of them can fail: asking for fullscreen on a compositor that refuses
 * it, or a scene size the device cannot allocate, leaves the window as it
 * was. Read the value back if it matters.
 */

FNL_API bool fnl_is_fullscreen(const FnlApp *app);                  /* [any] */
/* Whether the window has the keyboard focus. */
FNL_API bool fnl_is_active(const FnlApp *app);                      /* [any] */
FNL_API bool fnl_is_maximized(const FnlApp *app);                   /* [any] */
FNL_API bool fnl_is_pointer_captured(const FnlApp *app);            /* [any] */
/* The best sample count FSAA can use on this device, or 1 when it offers none
 * worth having. */
FNL_API int  fnl_max_samples(const FnlApp *app);                    /* [any] */
/* Whether the scene can be rendered anywhere but straight into the window --
 * the one target the render scale, FSAA and MLAA are all built on. Where it
 * is false, fnl_set_antialias() and fnl_set_scene_size_px() are both no-ops and
 * a control for either should be drawn dim. */
FNL_API bool fnl_offscreen_supported(const FnlApp *app);            /* [any] */
FNL_API FnlAntialias fnl_antialias(const FnlApp *app);              /* [any] */

/* Presents per second, not loop turns, over a fifth of a second at a time. A
 * window drawing nothing because nothing has changed keeps the rate it last
 * drew at -- unless fnl_set_cost_sampling() is on, when the count goes on
 * through the rest and reads the rate of the frames sampling draws. */
FNL_API int fnl_fps(const FnlApp *app);                             /* [any] */

/* What drawing one frame costs, apart from how often a frame is drawn,
 * which for a window that draws only when something changed is all
 * fnl_fps() can say: the GPU's time on a frame, from its first command to
 * the last that draws it, read from the renderer's timestamps -- KMS's
 * copy path's readback in it, which every frame there costs, and a
 * snapshot's copy not; and the render thread's CPU time on it, from the
 * end of its waits for the device and the display to the return of the
 * call that hands the frame over -- what the present does on that thread
 * in it (KMS's copy path copies the frame there), a wait that blocks left
 * out; or, where the system does not say a thread's CPU time (Windows
 * under wine), the wall clock over recording and submitting it. Running
 * means over the frames drawn, in milliseconds -- a frame a window at rest
 * draws only to show them left out, unless fnl_set_cost_sampling() is on
 * -- kept while nothing is drawn and started again
 * on a device that was rebuilt; -1 before there is one, and the GPU's
 * where the device cannot time it. The larger of the two bounds the rate
 * the window could be drawn at.
 * Refreshed five times a second while frames are drawn, as fnl_fps() is,
 * and once more when the window comes to rest; like fnl_fps()'s, a change
 * re-records the frame, so a caller drawing them needs no fnl_invalidate().
 * Since 0.9. */
FNL_API double fnl_frame_gpu_ms(const FnlApp *app);                 /* [any] */
FNL_API double fnl_frame_cpu_ms(const FnlApp *app);                 /* [any] */

/* How long an input takes to reach the caller: from when the key, the
 * button, the wheel or the pointer moved -- by the window server's own
 * stamp on it, or the device's, where the library shares its clock, and
 * from when the library read it where not -- to when fnl_poll_event()
 * hands it over. The mean over the inputs handed over since it was last
 * refreshed -- a fifth of a second while frames are drawn, and longer at
 * rest -- in milliseconds, kept while none are; -1 before the first. A
 * motion that coalesced counts each movement in it, and a repeat, a
 * character typed or a notification none. Refreshed as fnl_frame_gpu_ms()
 * is, and like it a change re-records the frame. Since 0.9. */
FNL_API double fnl_input_latency_ms(const FnlApp *app);             /* [any] */

/* Keep the two figures above, and fnl_fps(), measured while nothing
 * changes. On, a window at rest draws one frame five times a second -- the
 * recording it has, or the overlay again where the figures moved -- and the
 * three are refreshed together with it, so a readout of them stays live;
 * off, the default, a window at rest draws nothing and the figures stay the
 * last drawn frame's. For a caller while it shows them, and turned off when
 * it stops. Since 0.9. */
FNL_API void fnl_set_cost_sampling(FnlApp *app, bool on);           /* [any] */

/* The scene's animation clock, which F2 also switches: on, scene_time runs
 * from zero and every frame records again; off, the default, it stops at
 * zero and a still window records nothing (FnlFrame::scene_time). The GPU
 * ranking's measure runs the clock while it measures whatever this says,
 * and leaves the setting alone: fnl_scene_motion() reads what was last
 * set, by a call or F2, during a measure as well. Since 0.9. */
FNL_API void fnl_set_scene_motion(FnlApp *app, bool on);            /* [any] */
FNL_API bool fnl_scene_motion(const FnlApp *app);                   /* [any] */

/* The metrics panel: the library's own readout of what it measures, drawn
 * over everything else at the presentable area's top right, on every
 * renderer. The frame rate in its header; under it what a frame costs --
 * fnl_frame_gpu_ms(), fnl_frame_cpu_ms(), the rate the slower allows and
 * fnl_input_latency_ms() -- and what this process takes of the machine:
 * its CPU, as the share of one core, its resident memory, the GPU memory
 * it holds on the GPU drawing, and the share of that GPU's time its frames
 * take; and the GPU's name. A figure the system will not say is a dash:
 * GPU memory under OpenGL is NVIDIA's NVML or the kernel's DRM fdinfo on
 * Linux and none elsewhere.
 *
 * Hidden until shown, and shown it keeps nothing measured at rest: a
 * window at rest draws a frame only where the process's figures move, at
 * most twice a second, and that frame is not one the frame's figures
 * measure, so they stay the last drawn frames' as with the panel hidden.
 * fnl_set_cost_sampling() keeps them live, and the panel's frames then
 * count. Hidden it costs nothing. It gives way where the window is too
 * small to hold it, and it is drawn over the caller's own overlay, so a
 * caller with a panel at the top right moves its own while this is shown.
 * Since 0.9. */
FNL_API void fnl_set_metrics_panel(FnlApp *app, bool shown);        /* [any] */
FNL_API bool fnl_metrics_panel(const FnlApp *app);                  /* [any] */

/* The key that shows and hides the panel above: FNL_KEY_F5 until set, and
 * FNL_KEY_UNKNOWN for none, which leaves fnl_set_metrics_panel() the only
 * way. A value that is not an FnlKey is ignored, leaving the key as it
 * was. A binding of the library's like F2: it acts on a press, the event
 * still reaches the caller, and it acts with text entry on too, so a key
 * that types is a poor choice for a program that takes text. Since 0.9. */
FNL_API void   fnl_set_metrics_key(FnlApp *app, FnlKey key);        /* [any] */
FNL_API FnlKey fnl_metrics_key(const FnlApp *app);                  /* [any] */

/* Where the frame loop's time went, as totals a caller reads twice and
 * subtracts: the instrument, where --frame-times' line is the diagnostic.
 * Counted at the same places, from the first fnl_set_frame_stats(app,
 * true) on, and never reset; nothing is counted while it is off, which is
 * the default. Times are microseconds. `fence` and `acquire` are the two
 * halves of the wait before recording: on the device for a frame slot, and
 * for the image; `draw` is the renderer's whole frame, the four phases
 * after `pace` and what lies between them. `gpu` sums the GPU timestamps'
 * frames (fnl_frame_gpu_ms()), as many as `gpu_samples` says. `cycles` and
 * `missed` are the display's cycles and those a frame missed, where the
 * window server invites frames (Wayland) and 0 elsewhere. `period_ms` and
 * `hold_ms` are now's: the display period learnt, 0 while none is, and the
 * late latch's hold. */
typedef struct {
	size_t   size;               /* sizeof(FnlFrameStats), set by the caller */
	uint64_t frames;             /* the renderer was asked to draw */
	uint64_t presents;           /* and handed one to the display */
	uint64_t pace_us, fence_us, acquire_us;
	uint64_t record_us, submit_us, present_us;
	uint64_t rebuild_us, draw_us;
	uint64_t gpu_us, gpu_samples;
	uint64_t gpu_idle;           /* frames whose slot the GPU had finished */
	uint64_t cycles, missed;
	uint64_t rebuilds;           /* swapchains made */
	double   period_ms, hold_ms;
} FnlFrameStats;

/* Start (or stop) counting FnlFrameStats, and keeping FnlFrameSample.
 * Since 0.9. */
FNL_API void fnl_set_frame_stats(FnlApp *app, bool on);             /* [any] */
/* The totals so far, as many bytes of them as `out->size` says, so a caller
 * built against a shorter struct reads its prefix. False with no window, a
 * `size` too small for the field itself, or before counting was ever on.
 * Published once a frame, so up to a frame behind. Since 0.9. */
FNL_API bool fnl_frame_stats(const FnlApp *app,
							 FnlFrameStats *out);                   /* [any] */

/* One drawn frame, as the totals above count it: its phases in
 * milliseconds, the presents before it (FnlFrame::index), and when its
 * state was latched, in seconds since the window opened (FnlFrame::time's
 * clock). `rebuild` is a swapchain made or retired in its turn. `gpu_ms`
 * is the GPU's time on the frame by the renderer's timestamps, which are
 * read frames after it was drawn; -1 where none was read. */
typedef struct {
	uint64_t index;
	double   time;
	float    pace_ms, fence_ms, acquire_ms;
	float    record_ms, submit_ms, present_ms;
	float    rebuild_ms, draw_ms;
	bool     presented;          /* handed to the display, not only drawn */
	float    gpu_ms;
} FnlFrameSample;

/* The frames drawn since the last call, while fnl_set_frame_stats() is on:
 * up to `max` of them into `out`, oldest first, each `stride` bytes after
 * the one before -- sizeof(FnlFrameSample), which lets a caller built
 * against a shorter struct read its prefix. Returns how many. A frame the
 * GPU timed waits for its time, eight frames or a quarter of a second at
 * most, and holds back the ones after it, so the order stays the order
 * drawn; once the window's loop has ended every frame is handed over. The
 * library keeps the last 1024; a caller that drains less often loses the
 * oldest, and `lost`, where not NULL, says how many since the last call.
 * Since 0.9. */
FNL_API int fnl_frame_samples(FnlApp *app, FnlFrameSample *out, int max,
							  size_t stride, uint64_t *lost);       /* [any] */

/* A frame on the screen: which (FnlFrame::index) and when it got there, by
 * the display's own stamp, on FnlFrame::time's clock -- where the window
 * server or the driver says. KMS does for the frames it flips itself, the
 * copy path's and OpenGL's, and for Vulkan's own display path, whose flips
 * the driver makes, where the driver says when (VK_EXT_present_timing).
 * Wayland by wp_presentation, X11 by the Present extension, Win32 by
 * DXGI's frame statistics -- the last frame shown when the next is
 * presented, so at a rate past the display's not every frame -- and macOS
 * by each drawable's presented time, Metal's own or MoltenVK's
 * (VK_GOOGLE_display_timing). `vblank` is a flip at the vertical blank,
 * where false is one that tore. */
typedef struct {
	uint64_t index;
	double   time;
	bool     vblank;
} FnlFrameShown;

/* The frames shown since the last call, while fnl_set_frame_stats() is
 * on, as fnl_frame_samples() hands its frames over: up to `max`, oldest
 * first, `stride` bytes apart, the last 1024 kept and `lost` the rest.
 * Joined on `index` with FnlFrameSample and FnlFrame, a frame's age when
 * shown; with FnlEvent::time, which is when the library read the input,
 * how long from that read to the screen; and with FnlEvent::input_age,
 * from the input itself. Since 0.9. */
FNL_API int fnl_frame_shown(FnlApp *app, FnlFrameShown *out, int max,
							size_t stride, uint64_t *lost);         /* [any] */

/* How the window crosses a move to another GPU, renderer or kernel driver
 * (fnl_request_gpu()). The last frame on the device before is read back
 * and shown through the gap wherever the window server or the bare display
 * lets the library say what shows, and the first frames on the new device
 * dissolve from it over `ms`. Where no frame can be read back the window
 * fades to black over `ms` ahead of the move and up again after it, so the
 * gap, which every window server and the bare display fill with black or
 * with nothing, reads as the dark between two halves of one fade.
 * FNL_EVENT_GPU follows the dissolve. 250 until set, and held to 0 to
 * 5000; 0 is the cut, which is what a harness reading the frame right
 * after a move wants: the last frame is still read back for the gap, and
 * never drawn over the new device's. A window that is not on screen gets
 * no dissolve. A resize, a fullscreen toggle and a loss get none: the
 * first two are the window server's to animate and the frames stay
 * continuous, and a loss leaves nothing to read back. Since 0.9. */
FNL_API void fnl_set_transition_ms(FnlApp *app, int ms);            /* [any] */

/* What the scene is actually being rendered at, in pixels. Equal to the
 * presentable area unless a scene size was pinned below. */
FNL_API void fnl_scene_size_px(const FnlApp *app,
							   int *w_px, int *h_px);                /* [any] */

/* The presentable area -- the region a scene draws into, which is the window
 * minus any titlebar the library is drawing.
 *
 * Two calls because two questions. The size in *pixels* is what to compare a
 * scene resolution against. The rectangle in *logical points* is where to draw
 * over it, and its origin is how an overlay finds the top of the content
 * without knowing whether there is a titlebar above it or how tall it is. */
FNL_API void fnl_content_size_px(const FnlApp *app,
								 int *w_px, int *h_px);              /* [any] */
FNL_API void fnl_content_rect_pt(const FnlApp *app, int *x, int *y,
								 int *w, int *h);                    /* [any] */

/* The smallest presentable area worth *offering*, in logical points: what to
 * filter a resolution menu or a list of presets against. It does not change
 * over a window's life.
 *
 * Advisory. It is not the size the window refuses to go below, which is
 * smaller: the library's floor is the titlebar it draws, with its buttons
 * and its controls on screen. Anything laid out inside the presentable area
 * should give way rather than assume this much room -- the area can be one
 * point tall. */
FNL_API void fnl_min_content_size_pt(const FnlApp *app,
									 int *w_pt, int *h_pt);          /* [any] */

FNL_API void fnl_set_fullscreen(FnlApp *app, bool on);               /* [any] */
/* The other two window states, which the library's own titlebar buttons drive
 * and which a replacement titlebar needs to drive for itself. Asked for, not
 * set: a window server may answer with something else or with nothing, and
 * what happened comes back through fnl_is_maximized() and the state events.
 *
 * There is no fnl_unminimize() to match, and no window server here offers one
 * -- a minimized window is restored by the desktop, not by itself. */
FNL_API void fnl_minimize(FnlApp *app);                              /* [any] */
FNL_API void fnl_set_maximized(FnlApp *app, bool on);                /* [any] */

/* Antialiasing. Both modes are built on the offscreen scene target, so where
 * fnl_offscreen_supported() is false, asking leaves the window at
 * FNL_AA_NONE. fnl_max_samples() is FSAA's further condition. A control for
 * either mode should be drawn dim rather than left to fail quietly.
 *
 * The request is kept across devices, and fnl_antialias() reports what the
 * device drawing now makes of it: a move to a GPU that cannot draw FSAA
 * reads FNL_AA_NONE there, with an FNL_EVENT_ANTIALIAS, and a move back to
 * one that can draws it again, with another. A value that is not an
 * FnlAntialias is taken as FNL_AA_NONE, which turns antialiasing off. */
FNL_API void fnl_set_antialias(FnlApp *app, FnlAntialias aa);       /* [any] */

/* The pointer's shape over what the caller draws. Values are pinned for
 * FnlKey's reason. The list is what every backend can show, which is why
 * there are no diagonal resizes in it. */
typedef enum {
	FNL_CURSOR_DEFAULT     = 0,   /* the arrow */
	FNL_CURSOR_TEXT        = 1,   /* an I-beam: text can be selected */
	FNL_CURSOR_POINTER     = 2,   /* a hand: this can be pressed */
	FNL_CURSOR_CROSSHAIR   = 3,
	FNL_CURSOR_MOVE        = 4,   /* this can be dragged any way */
	FNL_CURSOR_EW_RESIZE   = 5,   /* a divider that moves left and right */
	FNL_CURSOR_NS_RESIZE   = 6,   /* one that moves up and down */
	FNL_CURSOR_NOT_ALLOWED = 7,
	FNL_CURSOR_WAIT        = 8,
} FnlCursor;

/* Choose that shape. It is state, not a region: it stays until the next call,
 * so a caller with a text field sets FNL_CURSOR_TEXT when FNL_EVENT_MOTION
 * enters the field and FNL_CURSOR_DEFAULT when it leaves.
 *
 * It applies over what the caller draws: the presentable area, a titlebar of
 * its own, and the run of the library's bar that fnl_titlebar_reserve() gave
 * it. Elsewhere the shape is the library's: its own titlebar and controls
 * show the arrow, the resize edges show their direction, and a captured
 * pointer shows nothing. None of those forgets the choice.
 *
 * There is no FNL_CURSOR_NONE; hiding the cursor is what a capture does. An
 * unknown value is ignored. On Wayland the shape is a request the compositor
 * draws from the user's theme, and one without wp_cursor_shape_v1 leaves the
 * cursor as it found it. */
FNL_API void fnl_set_cursor(FnlApp *app, FnlCursor shape);          /* [any] */

/* The window's title, which FnlInitDesc::title set at fnl_init(): the
 * window server's and the library's own titlebar's. UTF-8; NULL or empty
 * puts "Fenestal" back, and past 127 bytes it is cut at a character's
 * start. Asked for, as the knobs above are: the window server is told on
 * the pump's next turn and the bar on the next frame. */
FNL_API void fnl_set_title(FnlApp *app, const char *utf8);           /* [any] */

/* Let the pointer go. There is no call that *takes* it: a capture begins with
 * the user clicking into the presentable area. */
FNL_API void fnl_release_pointer(FnlApp *app);                       /* [any] */

/* Render the scene at a fixed size and scale the result over the window -- what
 * a game calls a render scale. (0, 0) is native: the scene is drawn straight
 * into the window at its own resolution.
 *
 * A size larger than the presentable area on either axis is ignored rather than
 * fitted, because fitting means either distorting the aspect or picking a size
 * nobody chose. Needs the offscreen target, so fnl_offscreen_supported() is how
 * to tell before offering the control. */
FNL_API void fnl_set_scene_size_px(FnlApp *app, int w_px, int h_px); /* [any] */

/* Two diagnostics, for a caller drawing a status readout. Both can change
 * between calls -- a window dragged to another monitor changes the first,
 * and a move to another GPU or renderer the second -- and either may be an
 * empty string before there is a window or a device to ask.
 *
 * fnl_output_name() hands back a per-thread snapshot, valid until the next
 * call *on that thread*; FNL_EVENT_OUTPUT says when it would answer
 * differently. fnl_present_mode() is the renderer's own name for what it
 * resolved to ("FIFO", "sync interval 1 (vsync)"), not a normalised enum.
 *
 * A window is always vsynced. FNL_VSYNC=0 in the environment is the one
 * way out, a diagnostic as FNL_ECHO_EVENTS is, read at fnl_init() and held
 * for the window's life. Off asks for the fastest present the surface
 * offers -- MAILBOX, then IMMEDIATE, under Vulkan; a swap or sync interval
 * of 0 elsewhere -- and may still be paced by the refresh where nothing
 * else is on offer (DESIGN *Vsync and present modes*). */
FNL_API const char *fnl_output_name(const FnlApp *app);             /* [any] */
FNL_API const char *fnl_present_mode(const FnlApp *app);            /* [any] */

/* Which backend and which renderer this program is running on -- together,
 * its platform. The backend is "Wayland", "X11", "Win32", "Cocoa" or "KMS"
 * -- a bare display, no window server -- and the renderer is the name of
 * what fnl_renderer() answers: "Vulkan", "OpenGL", "Direct3D 12" or
 * "Metal". A Linux build has Wayland, X11 and KMS, and fnl_init() opens
 * the first the session has -- Wayland where WAYLAND_DISPLAY is set, X11
 * where DISPLAY is, KMS where neither is -- or the one FNL_BACKEND names
 * in the environment ("wayland", "x11", "kms"), which it opens or fails.
 * For showing rather than branching on; the enum is for that.
 * fnl_gpu_name() is the *device*. Neither changes over a window's life, so
 * the pointer keeps. Empty before there is a window. */
FNL_API const char *fnl_backend_name(const FnlApp *app);            /* [any] */

/* What the backend above is itself running on, where that is not the
 * machine: "wine on Wayland", "wine on X11", or "wine" when which of its
 * display drivers is drawing cannot be told. Empty everywhere else, a real
 * Windows included. For showing: a Win32 window under wine is an X11 or a
 * Wayland one underneath, and which it is decides what the window manager
 * does with it. The pointer keeps. */
FNL_API const char *fnl_window_host(const FnlApp *app);             /* [any] */
FNL_API const char *fnl_renderer_name(const FnlApp *app);           /* [any] */
/* What the renderer's frames reach the window through, as fnl_gpu_via_at()
 * says it for the entry in use: "D3D11" where Vulkan presents through a
 * DXGI swapchain on Windows -- chosen from the GPU list, or by itself where
 * a swapchain of Vulkan's own is slow to resize -- and "" everywhere else.
 * For showing beside fnl_renderer_name(). A per-thread snapshot on
 * fnl_gpu_name()'s terms. Since 0.9. */
FNL_API const char *fnl_renderer_via(const FnlApp *app);            /* [any] */
/* The output the window is on, in logical points -- what a size-picker has to
 * filter against, since a window larger than its monitor is one the compositor
 * will clamp and never show whole. Zeroed when no output is known yet. */
FNL_API void fnl_output_size_pt(const FnlApp *app,
								int *w_pt, int *h_pt);               /* [any] */

/* That output's refresh rate in millihertz, as the window server says it
 * -- 60000 for 60 Hz, 59951 for what most panels call 60; on Win32 the
 * display configuration's signal -- and 0 where it says nothing. What the
 * frame loop spaces frames on until it has measured the period itself.
 * Since 0.9. */
FNL_API unsigned fnl_output_refresh_mhz(const FnlApp *app);         /* [any] */

/* ---- display modes and outputs --------------------------------------------
 *
 * On KMS there is no window server: the window is the whole of one output,
 * in one of that output's display modes, and these choose both. Everywhere
 * else a window server sizes the window -- the mode count is 0, the mode
 * getters return false, and fnl_request_mode() does nothing;
 * fnl_request_size_pt() is the knob there, and on KMS it is the one that
 * does nothing. The outputs are every backend's: under a window server they
 * are its monitors, and fnl_request_output() moves the window to one.
 * Since 0.9.
 *
 * A mode is named by value, not by index, so a request still means what it
 * meant if the list changed in between: the list is the output the window
 * is on, and moving to another output gives another. Requests are spent on
 * the pump's next turn, like the knobs above; a mode the output will not
 * show is gone back from, with one line through the log sink, and leaves
 * the list. The window fills the output in the new mode, titlebar and all,
 * and the change arrives as FNL_EVENT_RESIZE where the size moved and as
 * FNL_EVENT_OUTPUT where the output did. Where the GPU drives the display
 * itself, either one rebuilds the device as a lost surface does:
 * on_gpu_lost and on_gpu_created run. */
typedef struct FnlMode {
	int          w_px, h_px;
	unsigned int refresh_mhz;    /* millihertz: 59940 is 59.94 Hz */
} FnlMode;

/* The modes of the output the window is on, largest first and, within a
 * size, fastest first; fnl_mode_get() copies one out, and false past the
 * end. fnl_mode_current() is the one in force. */
FNL_API int  fnl_mode_count(const FnlApp *app);                     /* [any] */
FNL_API bool fnl_mode_get(const FnlApp *app, int i,
						  FnlMode *mode);                            /* [any] */
FNL_API bool fnl_mode_current(const FnlApp *app, FnlMode *mode);     /* [any] */
/* Set this mode: its size exactly, and of those the refresh nearest. A size
 * the output does not have does nothing. */
FNL_API void fnl_request_mode(FnlApp *app, const FnlMode *mode);     /* [any] */

/* The outputs the window can be moved to, the one it is on among them, a
 * monitor plugged in or out with the window open joining or leaving the
 * list. On KMS they are the kernel's names ("eDP-1", "HDMI-A-1") for those
 * connected on the GPU that drives the window, which leaves one that went.
 * Under a window server they are its monitors, by its names for them: the
 * connector on Wayland and X11 ("DP-1"), the GDI device on Win32
 * ("DISPLAY2"), AppKit's name on macOS, with " (2)" after a second of one
 * model. fnl_output_name_at() is a per-thread snapshot, as
 * fnl_output_name() is, and "" past the end.
 * fnl_request_output() moves the window there; a name not in the list, or
 * the output the window is on, does nothing. On KMS the window takes that
 * output's preferred mode. Under a window server it keeps its size and
 * state -- maximized or fullscreen there as here -- at the same offset into
 * the monitor's work area, pulled inside it. Wayland moves only a
 * fullscreen window, its clients placing nothing, and the Mac does not move
 * one: each says so through the log sink. The move arrives as
 * FNL_EVENT_OUTPUT, as one by the user does. */
FNL_API int  fnl_output_count(const FnlApp *app);                   /* [any] */
FNL_API const char *fnl_output_name_at(const FnlApp *app, int i);   /* [any] */
FNL_API void fnl_request_output(FnlApp *app, const char *name);     /* [any] */

/* The surface scale in 120ths: 120 is 100%, 180 is 150%. It is whatever the
 * desktop asks for: the fractional-scale protocol on Wayland, the window's own
 * DPI on Win32, the Xft.dpi resource on X11, the backing scale on macOS
 * (120 or 240). 120ths rather than a float
 * because they are exact, and every layout figure is derived from the scale
 * by integer arithmetic. */
FNL_API int fnl_scale120(const FnlApp *app);                        /* [any] */

/* Ask for a presentable area of this size, in logical points. The window comes
 * out taller by whatever titlebar the library is drawing.
 *
 * A request, not a setting: a compositor may answer with a different size or
 * ignore it entirely, and a maximized or fullscreen window will certainly
 * ignore it. Read the size back from the next frame rather than assuming.
 * On KMS the window is its output's mode, and fnl_request_mode() is how to
 * change that; this does nothing there. */
FNL_API void fnl_request_size_pt(FnlApp *app, int w_pt, int h_pt);   /* [any] */

/* The held keys, as a set. Safe from [any] thread and cheap enough to ask
 * every frame. Asked per key and answered per key: FNL_KEY_W held says
 * nothing about FNL_KEY_UP.
 *
 * Focus arriving seeds the set from the keys that are *already* down, and
 * focus leaving clears it, because the matching releases are never
 * delivered. */
FNL_API bool fnl_key_down(const FnlApp *app, FnlKey k);             /* [any] */

/* ---- text entry -----------------------------------------------------------
 *
 * Switch text entry on or off. Off is how a window starts, and off costs
 * nothing: no key is put through the layout and the event stream is the
 * positional one described at FnlKey.
 *
 * On, three things change, and all three are what a text field needs:
 *
 *   FNL_EVENT_TEXT   each character a key press types, under the layout,
 *                    Shift, Caps Lock, Num Lock and AltGr the user has -- so
 *                    the key FNL_KEY_Q names types "a" on an AZERTY board. It
 *                    arrives after the FNL_EVENT_KEY for the same press. A
 *                    press with Ctrl, Alt or Super down types nothing: that
 *                    is a shortcut, and the key event is how to read it.
 *   repeats          a held key repeats, as FNL_EVENT_KEY with `repeat` set
 *                    and as the text it types again.
 *   Escape           stops closing the window and is only a key, so that a
 *                    field can use it to cancel. The other bindings -- F2,
 *                    F9, F11 and the metrics key -- are left alone.
 *
 * It is a mode because repeats and a live Escape are wrong for a game and
 * right for a text field, and only the caller knows which it is showing. Turn
 * it on when a field takes focus and off when it gives it up.
 *
 * ---- composing -------------------------------------------------------------
 *
 * Some text is typed in more than one step: an accent and then the letter it
 * goes on, or a run of keys an input method turns into something else. What
 * has been typed towards it and not yet settled is the *composition*. It
 * arrives as FNL_EVENT_COMPOSE and is read with fnl_compose_text(); when it
 * settles, the result arrives as ordinary FNL_EVENT_TEXT and the composition
 * goes back to empty. A text field draws it at the caret, marked as
 * provisional, and inserts nothing until the text events come.
 *
 * Who composes depends on where this is running, and the event stream is the
 * same either way:
 *
 *   Wayland   the compositor's input method, through the text-input protocol:
 *             dead keys, Compose and CJK entry, with the composition reported.
 *             Where a compositor has none, the library composes dead keys
 *             itself, as on X11 -- and where FNL_NO_INPUT_METHOD is set in
 *             the environment, which leaves the compositor's unbound. A
 *             diagnostic, as FNL_ECHO_EVENTS is.
 *   X11       the library, for dead keys only: an accent key and the letter
 *             after it become one character, from a table generated out of
 *             Unicode's own decompositions. No Compose key, no input method.
 *   Win32     the system. Dead keys and an input method's result arrive as
 *             text; the composition in progress is drawn by the system's own
 *             window and is not reported here.
 *   Cocoa     the system's input method, through NSTextInputClient: dead
 *             keys, the accent menu's keys and CJK entry, with the
 *             composition -- AppKit's marked text -- reported.
 *   KMS       the library, as on X11, through the layout the console is
 *             configured with (XKBLAYOUT and the rest, in
 *             /etc/default/keyboard or /etc/vconsole.conf), read from
 *             xkeyboard-config's own files, or the one FNL_KMS_LAYOUT in
 *             the environment names, as "de(nodeadkeys)". Without the
 *             files, keys arrive and text entry types nothing.
 *
 * One thing to expect where an input method is composing: a key it takes for
 * itself is not delivered as a press, and its release may still be. A caller
 * pairing presses with releases should ignore a release it saw no press for.
 *
 * [any]. It is a flag the pump reads as each key arrives, so it applies from
 * the next key press on and never to one already delivered -- which is why a
 * key that *opens* a field does not also type into it. */
FNL_API void fnl_set_text_input(FnlApp *app, bool on);              /* [any] */
FNL_API bool fnl_is_text_input(const FnlApp *app);                  /* [any] */

/* The composition, as UTF-8. Copies at most `size` bytes including the
 * terminator, never splitting a character, and returns the length of the
 * whole thing -- so a return of `size` or more says the buffer was short.
 * Empty when nothing is being composed. `buf` may be NULL with a size of 0,
 * to ask only how long it is. */
FNL_API size_t fnl_compose_text(const FnlApp *app, char *buf,
								size_t size);                      /* [any] */

/* Where the caret is, in window points: the rectangle an input method puts
 * its candidate list beside. Only a hint and only used where there is such a
 * list, but a field that does not say gets its candidates at the window's
 * corner. Call it when the caret moves. */
FNL_API void fnl_set_text_rect_pt(FnlApp *app, int x, int y,
								  int w, int h);                     /* [any] */

/* ---- the clipboard --------------------------------------------------------
 *
 * Plain text, as UTF-8, and nothing else: no images, no rich text, no
 * drag and drop.
 *
 * Lines end in LF both ways. Windows' clipboard holds CRLF, and the library
 * turns one into the other at its edge there; elsewhere the text is what the
 * program holding it gave, which by convention is LF already. A lone CR is
 * passed through as it came.
 *
 * fnl_clipboard_set() puts a copy of the string on the desktop's clipboard.
 * False only for no window, no memory or a NULL string. On a bare display
 * (KMS) there is no other program to share one with, and a paste is what
 * this window copied last.
 *
 * Reading is a request and an answer, because on Wayland and X11 the text
 * is in another program and has to be asked for:
 * fnl_clipboard_request() asks, FNL_EVENT_CLIPBOARD says the answer is in --
 * always, with a length of zero when the clipboard held no text -- and
 * fnl_clipboard_text() hands it over, on fnl_compose_text()'s terms. What it
 * returns is the last answer received and stays until the next one.
 *
 * The answer is to the question, not to each asking of it: requests made
 * before one has been answered share that answer and its one event. The
 * program holding the text is given a few seconds to hand it over, and one
 * that does not -- or that sends more than 64 MiB, or stops halfway -- is
 * answered for with an empty clipboard rather than with part of one. The
 * event travels in the queue like any other, so after FNL_EVENT_LOST ask
 * again if an answer was owed.
 *
 * Both act on the pump's next turn, like the other knobs that reach the
 * window server. Call them in answer to a key press: Wayland lets a client
 * touch the clipboard only while it has the keyboard, and only on the
 * strength of an input event it was actually sent. */
FNL_API bool fnl_clipboard_set(FnlApp *app, const char *utf8);      /* [any] */
FNL_API void fnl_clipboard_request(FnlApp *app);                    /* [any] */
FNL_API size_t fnl_clipboard_text(const FnlApp *app, char *buf,
								  size_t size);                    /* [any] */


#ifdef __cplusplus
}
#endif

#endif /* FENESTAL_H */
