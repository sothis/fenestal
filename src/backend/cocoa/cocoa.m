/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Cocoa backend: one implementation of core/backend.h, for macOS.
 * Objective-C, under ARC, and the one directory beside src/renderer/metal/
 * that is: AppKit has no C interface, and its objects cross into the C
 * state as owned `void *` (cocoa_state.h).
 *
 * The window is borderless and the library draws all of it, as on Win32: no
 * system titlebar sits over the library's own, which would take the presses
 * meant for its buttons, so the resize edges are an inner margin deco_hit()
 * answers for and a resize is run here (cocoa_begin_resize()). A move is
 * AppKit's own, handed the press (performWindowDragWithEvent:).
 *
 * AppKit is the process's main thread's and no other's. fnl_init() is
 * refused anywhere else, and the render thread reads nothing of AppKit:
 * what it asks of the window is two atomics (cocoa_state.h), and the layer
 * the bridges present into is a CAMetalLayer, which any thread may use.
 *
 * Nothing is acknowledged, queued or invited: the size is the window's as
 * soon as AppKit has given it, the present is the throttle, and so
 * set_geometry, ack_configure, frame_request, frame_cancel, flush and
 * apply_scale do nothing. DESIGN *macOS* has the rest.
 */

#include "core/app.h"

#include "backend/cocoa/cocoa_state.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

/* The window's content view, declared here because everything below reaches
 * the events through it; its methods are further down, after the functions
 * they call. */
@interface FnlCocoaView : NSView <NSTextInputClient>
{
@public
	App *app;
	NSTrackingArea *tracking;
}
@end

/* The objects behind the state's pointers. */
#define WIN(p)   ((__bridge NSWindow *)(p)->window)
#define VIEW(p)  ((__bridge FnlCocoaView *)(p)->view)
#define LAYER(p) ((__bridge CAMetalLayer *)(p)->layer)

/* The subtype of the application-defined event a wakeup posts, which only
 * ends the pump's wait and is never dispatched. */
#define FNL_WAKE_SUBTYPE 0x464e

/* ---- keys ------------------------------------------------------------------
 *
 * The virtual key code to FnlKey. A key code names a key by where it is on
 * the board -- kVK_ANSI_Q is the key right of Tab whatever the layout makes
 * of it -- which is what FnlKey names, so the table is the whole
 * translation. The codes are Carbon's kVK_* (HIToolbox's Events.h), written
 * out: that header is Carbon's, which is not on the link line.
 *
 * A PC keyboard on a Mac sends codes for keys a Mac board does not have, and
 * these rows are where its keys land: Insert is Help (0x72), Num Lock the
 * keypad's Clear (0x47), Print Screen, Scroll Lock and Pause are F13, F14
 * and F15, and the context-menu key is 0x6e. Fn has no FnlKey.
 *
 * Two rows swap on an ISO board, as macOS reports them: the key left of 1
 * arrives as 0x0a there and the one between left Shift and Z as 0x32, the
 * reverse of an ANSI board's. iso_swap below puts them back where FnlKey
 * names them, as SDL does. */
_Static_assert(FNL_KEY_UNKNOWN == 0 && FNL_KEY_KP_ENTER <= UINT8_MAX,
			   "MAC_KEYS holds a FnlKey in a byte, 0 for none");

static const uint8_t MAC_KEYS[128] = {
	[0x00] = FNL_KEY_A,
	[0x01] = FNL_KEY_S,
	[0x02] = FNL_KEY_D,
	[0x03] = FNL_KEY_F,
	[0x04] = FNL_KEY_H,
	[0x05] = FNL_KEY_G,
	[0x06] = FNL_KEY_Z,
	[0x07] = FNL_KEY_X,
	[0x08] = FNL_KEY_C,
	[0x09] = FNL_KEY_V,
	[0x0a] = FNL_KEY_INTL_BACKSLASH,  /* kVK_ISO_Section; see iso_swap */
	[0x0b] = FNL_KEY_B,
	[0x0c] = FNL_KEY_Q,
	[0x0d] = FNL_KEY_W,
	[0x0e] = FNL_KEY_E,
	[0x0f] = FNL_KEY_R,
	[0x10] = FNL_KEY_Y,
	[0x11] = FNL_KEY_T,
	[0x12] = FNL_KEY_1,
	[0x13] = FNL_KEY_2,
	[0x14] = FNL_KEY_3,
	[0x15] = FNL_KEY_4,
	[0x16] = FNL_KEY_6,
	[0x17] = FNL_KEY_5,
	[0x18] = FNL_KEY_EQUAL,
	[0x19] = FNL_KEY_9,
	[0x1a] = FNL_KEY_7,
	[0x1b] = FNL_KEY_MINUS,
	[0x1c] = FNL_KEY_8,
	[0x1d] = FNL_KEY_0,
	[0x1e] = FNL_KEY_RBRACKET,
	[0x1f] = FNL_KEY_O,
	[0x20] = FNL_KEY_U,
	[0x21] = FNL_KEY_LBRACKET,
	[0x22] = FNL_KEY_I,
	[0x23] = FNL_KEY_P,
	[0x24] = FNL_KEY_ENTER,           /* kVK_Return */
	[0x25] = FNL_KEY_L,
	[0x26] = FNL_KEY_J,
	[0x27] = FNL_KEY_APOSTROPHE,
	[0x28] = FNL_KEY_K,
	[0x29] = FNL_KEY_SEMICOLON,
	[0x2a] = FNL_KEY_BACKSLASH,
	[0x2b] = FNL_KEY_COMMA,
	[0x2c] = FNL_KEY_SLASH,
	[0x2d] = FNL_KEY_N,
	[0x2e] = FNL_KEY_M,
	[0x2f] = FNL_KEY_PERIOD,
	[0x30] = FNL_KEY_TAB,
	[0x31] = FNL_KEY_SPACE,
	[0x32] = FNL_KEY_GRAVE,           /* kVK_ANSI_Grave; see iso_swap */
	[0x33] = FNL_KEY_BACKSPACE,       /* kVK_Delete */
	[0x35] = FNL_KEY_ESCAPE,
	[0x36] = FNL_KEY_RSUPER,          /* kVK_RightCommand */
	[0x37] = FNL_KEY_LSUPER,          /* kVK_Command */
	[0x38] = FNL_KEY_LSHIFT,
	[0x39] = FNL_KEY_CAPS_LOCK,
	[0x3a] = FNL_KEY_LALT,            /* kVK_Option */
	[0x3b] = FNL_KEY_LCTRL,
	[0x3c] = FNL_KEY_RSHIFT,
	[0x3d] = FNL_KEY_RALT,            /* kVK_RightOption */
	[0x3e] = FNL_KEY_RCTRL,
	[0x41] = FNL_KEY_KP_DECIMAL,
	[0x43] = FNL_KEY_KP_MULTIPLY,
	[0x45] = FNL_KEY_KP_ADD,
	[0x47] = FNL_KEY_NUM_LOCK,        /* kVK_ANSI_KeypadClear */
	[0x4b] = FNL_KEY_KP_DIVIDE,
	[0x4c] = FNL_KEY_KP_ENTER,
	[0x4e] = FNL_KEY_KP_SUBTRACT,
	[0x52] = FNL_KEY_KP_0,
	[0x53] = FNL_KEY_KP_1,
	[0x54] = FNL_KEY_KP_2,
	[0x55] = FNL_KEY_KP_3,
	[0x56] = FNL_KEY_KP_4,
	[0x57] = FNL_KEY_KP_5,
	[0x58] = FNL_KEY_KP_6,
	[0x59] = FNL_KEY_KP_7,
	[0x5b] = FNL_KEY_KP_8,
	[0x5c] = FNL_KEY_KP_9,
	[0x60] = FNL_KEY_F5,
	[0x61] = FNL_KEY_F6,
	[0x62] = FNL_KEY_F7,
	[0x63] = FNL_KEY_F3,
	[0x64] = FNL_KEY_F8,
	[0x65] = FNL_KEY_F9,
	[0x67] = FNL_KEY_F11,
	[0x69] = FNL_KEY_PRINT_SCREEN,    /* kVK_F13 */
	[0x6b] = FNL_KEY_SCROLL_LOCK,     /* kVK_F14 */
	[0x6d] = FNL_KEY_F10,
	[0x6e] = FNL_KEY_MENU,            /* kVK_ContextualMenu */
	[0x6f] = FNL_KEY_F12,
	[0x71] = FNL_KEY_PAUSE,           /* kVK_F15 */
	[0x72] = FNL_KEY_INSERT,          /* kVK_Help */
	[0x73] = FNL_KEY_HOME,
	[0x74] = FNL_KEY_PAGE_UP,
	[0x75] = FNL_KEY_DELETE,          /* kVK_ForwardDelete */
	[0x76] = FNL_KEY_F4,
	[0x77] = FNL_KEY_END,
	[0x78] = FNL_KEY_F2,
	[0x79] = FNL_KEY_PAGE_DOWN,
	[0x7a] = FNL_KEY_F1,
	[0x7b] = FNL_KEY_LEFT,
	[0x7c] = FNL_KEY_RIGHT,
	[0x7d] = FNL_KEY_DOWN,
	[0x7e] = FNL_KEY_UP,
};

#define KC_ISO_SECTION 0x0a
#define KC_GRAVE       0x32
#define KC_CAPS_LOCK   0x39

/* The modifiers, which arrive as flagsChanged with the key's code and the
 * flags after it: which side is down is in the device-dependent low bits
 * of the flags (IOKit's NX_DEVICE*KEYMASK), the same on every Mac. */
static const struct {
	unsigned short code;
	FnlKey         key;
	NSUInteger     mask;
} MODIFIERS[] = {
	{ 0x3b, FNL_KEY_LCTRL,  0x0001 },
	{ 0x38, FNL_KEY_LSHIFT, 0x0002 },
	{ 0x3c, FNL_KEY_RSHIFT, 0x0004 },
	{ 0x37, FNL_KEY_LSUPER, 0x0008 },
	{ 0x36, FNL_KEY_RSUPER, 0x0010 },
	{ 0x3a, FNL_KEY_LALT,   0x0020 },
	{ 0x3d, FNL_KEY_RALT,   0x0040 },
	{ 0x3e, FNL_KEY_RCTRL,  0x2000 },
};

/* Whether the board in use is an ISO one, asked of HIToolbox, which AppKit
 * has already loaded: by name, so that Carbon stays off the link line. An
 * answer nobody can give is ANSI's, which swaps nothing. */
static bool iso_board(void)
{
	typedef uint32_t (*LayoutTypeFn)(int16_t);
	static const uint32_t KEYBOARD_ISO = 0x49534f20;     /* 'ISO ' */
	LayoutTypeFn layout =
		(LayoutTypeFn)dlsym(RTLD_DEFAULT, "KBGetLayoutType");
	if (!layout)
		return false;
	CGEventSourceRef src =
		CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
	if (!src)
		return false;
	const CGEventSourceKeyboardType type = CGEventSourceGetKeyboardType(src);
	CFRelease(src);
	return layout((int16_t)type) == KEYBOARD_ISO;
}

/* Asked once a focus and not per key: the board can change between two
 * windows' worth of typing and not inside one. Pump thread. */
static bool iso_swap;

static FnlKey key_from_code(unsigned short code)
{
	if (iso_swap && code == KC_ISO_SECTION)
		return FNL_KEY_GRAVE;
	if (iso_swap && code == KC_GRAVE)
		return FNL_KEY_INTL_BACKSLASH;
	if (code >= sizeof MAC_KEYS)
		return FNL_KEY_UNKNOWN;
	return (FnlKey)MAC_KEYS[code];
}

/* The keys already down as focus arrives, which fenestal.h promises: asked
 * per key of the session's state. Caps Lock is a lock and not a key held,
 * and is left out. */
static void seed_held_keys(App *a)
{
	for (unsigned short code = 0; code < sizeof MAC_KEYS; code++) {
		const FnlKey k = key_from_code(code);
		if (k == FNL_KEY_UNKNOWN || code == KC_CAPS_LOCK)
			continue;
		if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,
								  code))
			app_key_seed(a, k);
	}
}

/* ---- units -----------------------------------------------------------------
 *
 * AppKit speaks points with the backing store's scale beside them, 1 or 2;
 * the library speaks its own points at a scale in 120ths. Without an
 * override the two points are one: the scale is the backing's. With one,
 * the window's pixels stay what AppKit gave and the library's points are
 * those pixels at the override, as on Win32 -- so every figure crossing the
 * seam goes through these two. */
static int resolve_scale120(const App *a, double backing)
{
	if (a->scale_override120 > 0)
		return a->scale_override120;
	return clamp_scale120((int)(backing * 120.0 + 0.5));
}

/* AppKit points to the library's, unrounded. */
static double to_pt(const App *a, double v)
{
	return v * a->backend_state->backing * 120.0 / want_s120(a);
}

/* And back. */
static double from_pt(const App *a, double v)
{
	return v * want_s120(a) / (a->backend_state->backing * 120.0);
}

/* Where an event landed, in the library's window points: the view is
 * flipped, so y counts down from the top as the library's does. */
static NSPoint event_pt(const App *a, NSEvent *e)
{
	const NSPoint v = [VIEW(a->backend_state) convertPoint:e.locationInWindow
										 fromView:nil];
	return NSMakePoint(to_pt(a, v.x), to_pt(a, v.y));
}

/* The window server's clock, in milliseconds: what a double click on the bar
 * is measured on. Seconds since boot, so the wrap is decades away. */
static uint32_t event_ms(NSEvent *e)
{
	return (uint32_t)(uint64_t)(e.timestamp * 1000.0);
}

/* ---- publishing to the render thread ------------------------------------- */

/* The display a screen shows, which outlives the NSScreen object for it. */
static uint32_t screen_id(NSScreen *s)
{
	NSNumber *num = s.deviceDescription[@"NSScreenNumber"];
	return num ? num.unsignedIntValue : 0;
}

/* The screens by AppKit's names for them, in NSScreen.screens' order. A
 * name two screens share -- two monitors of one model -- is told apart by
 * its count, "DELL U2720Q (2)", or a move to it by name could not say
 * which. */
static int screens_named(NSArray<NSScreen *> *screens,
						 char (*names)[OUTPUT_NAME_MAX], int max)
{
	const int n = screens.count < (NSUInteger)max ? (int)screens.count : max;
	for (int i = 0; i < n; i++) {
		NSString *ln = screens[i].localizedName;
		const char *u = ln ? ln.UTF8String : NULL;
		int same = 0;
		for (int j = 0; j < i && ln; j++)
			if ([screens[j].localizedName isEqualToString:ln])
				same++;
		if (same)
			snprintf(names[i], OUTPUT_NAME_MAX, "%s (%d)", u ? u : "",
					 same + 1);
		else
			snprintf(names[i], OUTPUT_NAME_MAX, "%s", u ? u : "");
	}
	return n;
}

/* The screen the window is on, its name, and the room on it -- the visible
 * frame, which leaves out the menu bar and the Dock, as Win32's work area
 * leaves out the taskbar. `force` re-reads an unchanged screen, which a
 * scale or a Dock that moved needs. */
static void publish_output(App *a, bool force)
{
	struct BackendState *p = a->backend_state;
	NSScreen *s = WIN(p).screen;
	if (!s)
		return;
	const uint32_t id = screen_id(s);
	if (id == p->display && !force)
		return;
	p->display = id;

	/* The output is published under its name in the list, a shared one's
	 * count included, which is the name cocoa_set_output() matches. */
	NSArray<NSScreen *> *screens = NSScreen.screens;
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	const int n = screens_named(screens, names, OUTPUTS_MAX);
	int cur = -1;
	for (int i = 0; i < n && cur < 0; i++)
		if (screen_id(screens[i]) == id)
			cur = i;

	const NSRect vis = s.visibleFrame;
	const int w_pt = (int)(to_pt(a, vis.size.width) + 0.5);
	const int h_pt = (int)(to_pt(a, vis.size.height) + 0.5);
	const char *name = s.localizedName.UTF8String;
	app_publish_output(a, cur >= 0 ? names[cur] : (name ? name : ""), w_pt,
					   h_pt);

	/* A screen that names no rate -- some panels -- leaves the spacing on
	 * the period the loop measures. */
	const NSInteger fps = s.maximumFramesPerSecond;
	app_publish_refresh(a, fps > 0 ? (unsigned int)fps * 1000u : 0u);

	/* The titlebar's output box follows the window from here. */
	app_publish_outputs(a, names, n, cur);
}

/* The window's size and state as AppKit has them now. Called from every
 * delegate message that moves either.
 *
 * A miniaturized window has no size, and zero is how the frame loop is told
 * to idle, as on Win32. The size comes from the view's bounds, in the
 * library's points rounded once from pixels: px_to_pt() of what the frame
 * will draw, so a size asked for reads back as itself. */
static void publish_size(App *a)
{
	struct BackendState *p = a->backend_state;
	NSWindow *w = WIN(p);
	if (w.miniaturized) {
		app_publish_hidden(a);
		return;
	}
	const int s120 = resolve_scale120(a, p->backing);
	const NSRect b = VIEW(p).bounds;
	const int w_px = (int)(b.size.width * p->backing + 0.5);
	const int h_px = (int)(b.size.height * p->backing + 0.5);
	if (w_px <= 0 || h_px <= 0)
		return;
	/* isZoomed compares the frame with the standard one, which a
	 * fullscreen window also fills. */
	a->is_maximized = !a->is_fullscreen && w.zoomed;
	app_publish_size(a, px_to_pt(w_px, s120), px_to_pt(h_px, s120), s120);
	publish_output(a, false);
}

/* Anywhere a frame could go, for the render thread's window_on_screen: not
 * miniaturized, not hidden with the application, and ordered in. */
static void publish_on_screen(App *a)
{
	NSWindow *w = WIN(a->backend_state);
	fnl_atomic_store_bool(&a->backend_state->on_screen,
						  w.visible && !w.miniaturized && !NSApp.hidden);
}

/* ---- the size floor --------------------------------------------------------
 * Held on the library's own resize, which never goes below it, and once
 * after anything else -- a size the caller asked for is already above it
 * (app_request_size()), so what is left is a floor that rose past the
 * window, which apply_size_bounds() grows it back from. */
static NSSize floor_size(const App *a)
{
	int min_w, min_h;
	deco_min_window(a, a->is_fullscreen, &min_w, &min_h);
	return NSMakeSize(ceil(from_pt(a, min_w)), ceil(from_pt(a, min_h)));
}

/* And told to the window, for AppKit's own edge resize, which a resizable
 * borderless window has beside the library's and which holds minSize and
 * nothing else: a drag the harness posted at the corner took the window
 * to 43 points wide (macOS 26.7.1). Again whenever the floor moves, and
 * with the scale it is in Cocoa points of. */
static void hold_floor(App *a)
{
	WIN(a->backend_state).minSize = floor_size(a);
}

/* ---- pointer capture ------------------------------------------------------
 *
 * The cursor detached from the mouse and hidden: it stays where the click
 * left it, and the movement arrives as the events' deltas. The capture is
 * the library's and lasts until F9; the detachment is the desktop's and is
 * handed back whenever this window is not the key one. NSCursor's hide is a
 * counter, which cursor_hidden keeps balanced. */
static void capture_apply(struct BackendState *p, bool on)
{
	if (on == p->cursor_hidden)
		return;
	CGAssociateMouseAndMouseCursorPosition(on ? false : true);
	if (on)
		[NSCursor hide];
	else
		[NSCursor unhide];
	p->cursor_hidden = on;
}

static void cocoa_capture(App *a)
{
	if (a->pointer_captured)
		return;
	app_reticle_start(a);
	a->pointer_captured = true;
	capture_apply(a->backend_state, WIN(a->backend_state).keyWindow);
	app_reticle_publish(a);
}

/* The desktop cursor comes back where the reticle is, in the display
 * space's coordinates, whose origin is the top left of the main display. */
static void cocoa_release(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!a->pointer_captured)
		return;
	int cx, cy;
	app_content_px_rect(a, &cx, &cy, NULL, NULL);
	const int s120 = want_s120(a);
	const double x_pt = px_to_ptf(cx + a->cur_px, s120);
	const double y_pt = px_to_ptf(cy + a->cur_py, s120);
	const NSPoint in_view = NSMakePoint(from_pt(a, x_pt), from_pt(a, y_pt));
	const NSPoint in_win = [VIEW(p) convertPoint:in_view toView:nil];
	const NSPoint on_screen = [WIN(p) convertPointToScreen:in_win];
	const CGFloat top = NSScreen.screens.firstObject.frame.size.height;
	if (p->cursor_hidden)
		CGWarpMouseCursorPosition(CGPointMake(on_screen.x,
											  top - on_screen.y));
	capture_apply(p, false);
	a->pointer_captured = false;
	a->ptr_x = x_pt;
	a->ptr_y = y_pt;
	app_reticle_publish(a);
	[WIN(p) invalidateCursorRectsForView:VIEW(p)];
}

/* ---- the pointer's shape ------------------------------------------------- */

static NSCursor *caller_cursor(FnlCursor c)
{
	switch (c) {
	case FNL_CURSOR_TEXT:        return NSCursor.IBeamCursor;
	case FNL_CURSOR_POINTER:     return NSCursor.pointingHandCursor;
	case FNL_CURSOR_CROSSHAIR:   return NSCursor.crosshairCursor;
	/* No four-way arrow among AppKit's; the open hand is what a Mac drags
	 * a thing about with. */
	case FNL_CURSOR_MOVE:        return NSCursor.openHandCursor;
	case FNL_CURSOR_EW_RESIZE:   return NSCursor.resizeLeftRightCursor;
	case FNL_CURSOR_NS_RESIZE:   return NSCursor.resizeUpDownCursor;
	case FNL_CURSOR_NOT_ALLOWED: return NSCursor.operationNotAllowedCursor;
	/* AppKit's own wait is the spinning wheel the window server puts up
	 * over an application that stops answering, and is not one to set. */
	case FNL_CURSOR_WAIT:
	case FNL_CURSOR_DEFAULT:
	default:                     return NSCursor.arrowCursor;
	}
}

/* What macOS added after the oldest this targets, asked of the class or the
 * object rather than through @available: that is a call into clang's own
 * Darwin runtime (__isPlatformVersionAtLeast), which the Linux toolchain the
 * tree is built with does not have. The warning about an unguarded call is
 * off for exactly the lines the question guards. */
#define FNL_NEWER_API_BEGIN                                                   \
	_Pragma("clang diagnostic push")                                          \
	_Pragma("clang diagnostic ignored \"-Wunguarded-availability-new\"")
#define FNL_NEWER_API_END _Pragma("clang diagnostic pop")

/* A resize edge's arrows: AppKit's own frame cursors from macOS 15, and
 * before that the two straight ones, the diagonals as the nearer of them. */
static NSCursor *edge_cursor(Edge e)
{
	FNL_NEWER_API_BEGIN
	if ([NSCursor respondsToSelector:
					  @selector(frameResizeCursorFromPosition:inDirections:)]) {
		NSCursorFrameResizePosition pos;
		switch (e) {
		case EDGE_T:  pos = NSCursorFrameResizePositionTop;         break;
		case EDGE_B:  pos = NSCursorFrameResizePositionBottom;      break;
		case EDGE_L:  pos = NSCursorFrameResizePositionLeft;        break;
		case EDGE_R:  pos = NSCursorFrameResizePositionRight;       break;
		case EDGE_TL: pos = NSCursorFrameResizePositionTopLeft;     break;
		case EDGE_TR: pos = NSCursorFrameResizePositionTopRight;    break;
		case EDGE_BL: pos = NSCursorFrameResizePositionBottomLeft;  break;
		case EDGE_BR: pos = NSCursorFrameResizePositionBottomRight; break;
		default:      return NSCursor.arrowCursor;
		}
		return [NSCursor frameResizeCursorFromPosition:pos
			inDirections:NSCursorFrameResizeDirectionsAll];
	}
	FNL_NEWER_API_END
	return e == EDGE_T || e == EDGE_B ? NSCursor.resizeUpDownCursor
									  : NSCursor.resizeLeftRightCursor;
}

/* Put up what deco_cursor() says for the zone under the pointer. Hidden is
 * the capture's, whose counter capture_apply() keeps. */
static void cursor_apply(App *a, Zone z, Edge edge)
{
	const DecoCursor c = deco_cursor(a, z, edge);
	if (c.kind == DECO_CURSOR_HIDDEN)
		return;
	[(c.kind == DECO_CURSOR_EDGE ? edge_cursor(c.edge)
								 : caller_cursor(c.shape)) set];
}

/* ---- the pointer --------------------------------------------------------- */

/* The pointer is here, in window points: the event, the hover, the drag a
 * held press on the bar may become, and the shape. */
static void pointer_at(App *a, NSPoint pt)
{
	a->ptr_x = pt.x;
	a->ptr_y = pt.y;
	app_pointer_motion(a, pt.x, pt.y);
	Edge edge;
	const Zone z = deco_pointer_at(a, pt.x, pt.y, &edge);
	deco_drag(a, pt.x, pt.y);
	cursor_apply(a, z, edge);
}

/* A captured pointer's movement: the event's deltas, AppKit points, which
 * the reticle takes in pixels and the caller in the library's points. */
static void pointer_relative(App *a, NSEvent *e)
{
	const double dx = to_pt(a, e.deltaX), dy = to_pt(a, e.deltaY);
	if (dx == 0.0 && dy == 0.0)
		return;
	const int s120 = want_s120(a);
	app_reticle_move(a, pt_to_pxf(dx, s120), pt_to_pxf(dy, s120));
	app_pointer_relative(a, dx, dy);
}

/* The left button came up somewhere the view was not told: AppKit's window
 * drag and the resize below both keep the release, and the caller saw the
 * press. left_down is that press, for a move asked for after it was
 * dispatched (fnl_begin_move() from a callback, drained a turn later). */
static bool left_held;
static NSEvent *left_down;

static void left_release(App *a, NSPoint pt)
{
	if (!left_held)
		return;
	left_held = false;
	left_down = nil;
	app_pointer_button(a, FNL_BUTTON_LEFT, false, pt.x, pt.y,
					   (uint32_t)(uint64_t)(NSProcessInfo.processInfo
												.systemUptime * 1000.0));
}

/* ---- the view --------------------------------------------------------------
 *
 * The window's whole content, hosting the CAMetalLayer the frame goes into,
 * flipped so that y counts down, and the first responder: every event the
 * window takes comes through here. The layer is handed to the view rather
 * than made by it on demand, which AppKit does only once the view is in a
 * window and on screen -- and the bridges want it at open. */

@implementation FnlCocoaView

- (BOOL)isFlipped                 { return YES; }
- (BOOL)acceptsFirstResponder     { return YES; }
- (BOOL)mouseDownCanMoveWindow    { return NO; }

/* The first click on a window that is not in front is the bar's, where the
 * system's own bar would have taken it, or a claim's (fnl_claim()), which is
 * a control -- and not the content's, where it would take the pointer before
 * the window was even in front. */
- (BOOL)acceptsFirstMouse:(NSEvent *)e
{
	if (!app)
		return NO;
	const NSPoint pt = event_pt(app, e);
	Edge edge;
	return deco_hit(app, pt.x, pt.y, &edge) != ZONE_CONTENT;
}

/* The area is this view's the while it has an App: AppKit routes enter and
 * exit through it, and their handlers read the App. A view outliving its
 * App -- the window is on its way out -- gets none. */
- (void)updateTrackingAreas
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		tracking = nil;
	}
	if (app) {
		tracking = [[NSTrackingArea alloc]
			initWithRect:NSZeroRect
				 options:NSTrackingMouseEnteredAndExited |
						 NSTrackingMouseMoved | NSTrackingCursorUpdate |
						 NSTrackingActiveAlways | NSTrackingInVisibleRect
				   owner:self
				userInfo:nil];
		[self addTrackingArea:tracking];
	}
	[super updateTrackingAreas];
}

/* The window is going. Its orderOut makes AppKit an exit to route, which
 * the dequeue monitor may deliver on the next run -- after this App is
 * gone -- so the area that would take it goes here, before the window,
 * and the App with it: a late event is a no-op and not a read of a freed
 * App (the bench's repeats, three lifetimes in one process, faulted in
 * mouseExited: here). */
- (void)detach
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		tracking = nil;
	}
	app = NULL;
}

- (void)viewDidChangeBackingProperties
{
	[super viewDidChangeBackingProperties];
	if (app && app->backend_state)
		BACKEND(app)->update_scale(app);
}

/* ---- the pointer --------------------------------------------------------- */

- (void)cursorUpdate:(NSEvent *)e
{
	if (!app)
		return;
	const NSPoint pt = event_pt(app, e);
	Edge edge;
	const Zone z = deco_hit(app, pt.x, pt.y, &edge);
	cursor_apply(app, z, edge);
}

- (void)pointer:(NSEvent *)e
{
	if (!app)
		return;
	if (app->pointer_captured)
		pointer_relative(app, e);
	else
		pointer_at(app, event_pt(app, e));
}

/* A move with no button down, so a release kept from the view is owed. A
 * drag is not one: the press it carries is the one a move is handed. */
- (void)mouseMoved:(NSEvent *)e
{
	if (!app)
		return;
	if (!app->pointer_captured)
		left_release(app, event_pt(app, e));
	[self pointer:e];
}

- (void)mouseDragged:(NSEvent *)e      { [self pointer:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self pointer:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self pointer:e]; }

- (void)mouseEntered:(NSEvent *)e
{
	if (!app || app->pointer_captured)
		return;
	/* A caller should hear where: a hover that waits for the pointer to
	 * move again is stale for one that enters and stops. */
	pointer_at(app, event_pt(app, e));
}

- (void)mouseExited:(NSEvent *)e
{
	(void)e;
	if (!app || app->pointer_captured)
		return;
	deco_release(app);              /* the press is someone else's now */
	deco_publish_hover(app, ZONE_CONTENT);
}

- (void)button:(FnlButton)b pressed:(bool)down event:(NSEvent *)e
{
	const NSPoint pt = event_pt(app, e);
	if (b == FNL_BUTTON_LEFT) {
		left_held = down;
		left_down = down ? e : nil;
	}
	if (!app->pointer_captured) {
		app->ptr_x = pt.x;
		app->ptr_y = pt.y;
	}
	app_pointer_button(app, b, down, pt.x, pt.y, event_ms(e));
}

- (void)mouseDown:(NSEvent *)e
{
	[self button:FNL_BUTTON_LEFT pressed:true event:e];
}

- (void)mouseUp:(NSEvent *)e
{
	[self button:FNL_BUTTON_LEFT pressed:false event:e];
}

- (void)rightMouseDown:(NSEvent *)e
{
	[self button:FNL_BUTTON_RIGHT pressed:true event:e];
}

- (void)rightMouseUp:(NSEvent *)e
{
	[self button:FNL_BUTTON_RIGHT pressed:false event:e];
}

/* The middle button is 2; AppKit numbers the rest on from there, and they
 * have no FnlButton. */
- (void)otherMouseDown:(NSEvent *)e
{
	if (e.buttonNumber == 2)
		[self button:FNL_BUTTON_MIDDLE pressed:true event:e];
}

- (void)otherMouseUp:(NSEvent *)e
{
	if (e.buttonNumber == 2)
		[self button:FNL_BUTTON_MIDDLE pressed:false event:e];
}

/* In notches, positive down and to the right (fenestal.h). A wheel's
 * deltas are its steps already, which the user's natural scrolling has
 * turned or not; a trackpad's are points, at Wayland's ten to the step. */
- (void)scrollWheel:(NSEvent *)e
{
	double dx = -e.scrollingDeltaX, dy = -e.scrollingDeltaY;
	if (e.hasPreciseScrollingDeltas) {
		dx /= 10.0;
		dy /= 10.0;
	}
	if (dx == 0.0 && dy == 0.0)
		return;
	app_pointer_scroll(app, app->ptr_x, app->ptr_y, dx, dy);
}

/* ---- keys ---------------------------------------------------------------- */

/* A repeat is never a press: it reaches the caller only as the repeat a
 * text field asks for, which app_key_repeat() drops otherwise. The layout
 * is AppKit's, and is applied only in text entry: interpretKeyEvents is
 * what turns keys into insertText:, dead keys and an input method's
 * composition included, and outside text entry a key types nothing. Never
 * handed to super, whose answer to a key nobody wants is a beep. */
- (void)keyDown:(NSEvent *)e
{
	const FnlKey k = key_from_code(e.keyCode);
	if (e.ARepeat)
		app_key_repeat(app, k);
	else
		app_key(app, k, true);
	if (app_text_input(app))
		[self interpretKeyEvents:@[ e ]];
}

- (void)keyUp:(NSEvent *)e
{
	app_key(app, key_from_code(e.keyCode), false);
}

/* A modifier: down or up is in the flags that come with it. Caps Lock
 * reports the lock and not the key, so each change is one press of it. */
- (void)flagsChanged:(NSEvent *)e
{
	if (e.keyCode == KC_CAPS_LOCK) {
		app_key(app, FNL_KEY_CAPS_LOCK, true);
		app_key(app, FNL_KEY_CAPS_LOCK, false);
		return;
	}
	for (size_t i = 0; i < sizeof MODIFIERS / sizeof MODIFIERS[0]; i++) {
		if (MODIFIERS[i].code != e.keyCode)
			continue;
		const bool down = (e.modifierFlags & MODIFIERS[i].mask) != 0;
		if (down != fnl_key_down(app, MODIFIERS[i].key))
			app_key(app, MODIFIERS[i].key, down);
		return;
	}
}

/* ---- text: NSTextInputClient ----
 *
 * What interpretKeyEvents hands back. The library keeps no text of its own,
 * so the client is a sink: committed text goes out as FNL_EVENT_TEXT, the
 * marked text as the composition, and every editing command -- Return,
 * Backspace, the arrows -- is ignored here because the caller has already
 * had it as a key. */

static NSString *plain(id s)
{
	return [s isKindOfClass:NSAttributedString.class]
			   ? ((NSAttributedString *)s).string : (NSString *)s;
}

/* A composition still up is thrown away, as X11's and Wayland's are: the
 * input method is told, so it neither lingers on the caller's screen nor
 * commits into whatever takes the text next. */
static void compose_discard(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->marked_len)
		return;
	[VIEW(p).inputContext discardMarkedText];
	p->marked_len = 0;
	app_compose_set(a, NULL);
}

- (void)insertText:(id)s replacementRange:(NSRange)r
{
	(void)r;
	struct BackendState *p = app->backend_state;
	if (p->marked_len) {
		p->marked_len = 0;
		app_compose_set(app, NULL);
	}
	const char *u = plain(s).UTF8String;
	if (u)
		app_text_utf8(app, u);
}

- (void)setMarkedText:(id)s selectedRange:(NSRange)sel
	 replacementRange:(NSRange)r
{
	(void)sel;
	(void)r;
	NSString *t = plain(s);
	app->backend_state->marked_len = t.length;
	app_compose_set(app, t.length ? t.UTF8String : NULL);
}

- (void)unmarkText
{
	app->backend_state->marked_len = 0;
	app_compose_set(app, NULL);
}

/* The client keeps no document, so the marked text is all of it: it starts
 * at 0 for as long as it is up. */
- (BOOL)hasMarkedText  { return app->backend_state->marked_len > 0; }
- (NSRange)markedRange
{
	const size_t n = app->backend_state->marked_len;
	return n ? NSMakeRange(0, n) : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText
{
	return @[];
}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r
	actualRange:(NSRangePointer)actual
{
	(void)r;
	(void)actual;
	return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)pt
{
	(void)pt;
	return NSNotFound;
}
- (void)doCommandBySelector:(SEL)sel { (void)sel; }

/* Where the caret is, for the input method's window: the caller's
 * rectangle (fnl_set_text_rect_pt()), on the screen. */
- (NSRect)firstRectForCharacterRange:(NSRange)r
						 actualRange:(NSRangePointer)actual
{
	(void)r;
	(void)actual;
	int x = 0, y = 0, w = 0, h = 0;
	if (!app_text_rect(app, &x, &y, &w, &h))
		return NSZeroRect;
	const NSRect v = NSMakeRect(from_pt(app, x), from_pt(app, y),
								from_pt(app, w), from_pt(app, h));
	return [self.window convertRectToScreen:[self convertRect:v
													   toView:nil]];
}

@end

/* ---- the window -------------------------------------------------------------
 *
 * Borderless windows decline the key and main status unless they say
 * otherwise, and this one is the application. */

@interface FnlCocoaWindow : NSWindow
@end

@implementation FnlCocoaWindow
- (BOOL)canBecomeKeyWindow  { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

/* What AppKit tells the window, each turned into the publish it moves. */
@interface FnlCocoaWindowDelegate : NSObject <NSWindowDelegate>
{
@public
	App *app;
}
@end

@implementation FnlCocoaWindowDelegate

- (BOOL)windowShouldClose:(NSWindow *)w
{
	(void)w;
	app_close_requested(app);
	return NO;
}

- (void)windowDidResize:(NSNotification *)n
{
	(void)n;
	publish_size(app);
}

- (void)windowDidMove:(NSNotification *)n
{
	(void)n;
	publish_output(app, false);
}

- (void)windowDidChangeScreen:(NSNotification *)n
{
	(void)n;
	publish_output(app, true);
}

- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
	(void)n;
	BACKEND(app)->update_scale(app);
}

- (void)windowDidMiniaturize:(NSNotification *)n
{
	(void)n;
	publish_on_screen(app);
	publish_size(app);
}

- (void)windowDidDeminiaturize:(NSNotification *)n
{
	(void)n;
	publish_on_screen(app);
	publish_size(app);
}

- (void)windowDidChangeOcclusionState:(NSNotification *)n
{
	(void)n;
	const bool seen = (WIN(app->backend_state).occlusionState &
					   NSWindowOcclusionStateVisible) != 0;
	fnl_atomic_store_bool(&app->backend_state->occluded, !seen);
	publish_on_screen(app);
}

/* Focus loss never delivers the matching key releases, so every held key
 * is dropped, and a composition goes with the keyboard; the keys already
 * down are seeded before the caller hears about the focus. The detachment
 * goes with the focus and the capture does not. */
- (void)focus:(bool)in
{
	struct BackendState *p = app->backend_state;
	app_keys_clear(app);
	if (in) {
		iso_swap = iso_board();
		seed_held_keys(app);
	} else {
		compose_discard(app);
	}
	if (app->pointer_captured)
		capture_apply(p, in);
	app_publish_focus(app, in);
	app_post_focus(app, in);
}

/* The view takes the keys, and is given them back each time the window
 * takes the keyboard or changes state: AppKit's fullscreen leaves the
 * window itself the first responder, and a key then reached nothing --
 * clicks, which find the view by where they land, went on arriving. */
- (void)keysToView
{
	NSWindow *w = WIN(app->backend_state);
	if (w.firstResponder != (NSResponder *)VIEW(app->backend_state))
		[w makeFirstResponder:VIEW(app->backend_state)];
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
	(void)n;
	[self keysToView];
	[self focus:true];
}

- (void)windowDidResignKey:(NSNotification *)n
{
	(void)n;
	[self focus:false];
}

/* What the layer shows of a frame that is not at its size. Pinned to the
 * top left, so that the library's own resize never stretches one --
 * kCAGravityBottomLeft, since an unflipped layer's "top" is its bottom
 * edge. Stretched to the window through AppKit's fullscreen transition,
 * which drops every frame presented between its will and its did: the last
 * one shown is all there is to show, and pinned it would stand small on the
 * window's background colour, or be cropped. */
static void layer_hold(struct BackendState *p, bool stretch)
{
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	LAYER(p).contentsGravity = stretch ? kCAGravityResize
									   : kCAGravityBottomLeft;
	[CATransaction commit];
}

/* The fullscreen state asked for inside AppKit's animation, which AppKit
 * ignores there, asked of it now that the animation has ended -- from the
 * did, or a transition that failed -- where the window is not already
 * so. */
static void fs_settle(App *a)
{
	struct BackendState *p = a->backend_state;
	const int owed = p->fs_owed;
	p->fs_owed = 0;
	if (!owed || !p->window)
		return;
	NSWindow *w = WIN(p);
	const bool now = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
	if ((owed == 1) != now)
		[w toggleFullScreen:nil];
}

/* Fullscreen is AppKit's own, a space of its own with the menu bar and the
 * Dock away. The state turns as the transition starts; AppKit resizes the
 * window once, to the size it ends at, within 60 ms of the will (macOS
 * 26.7.1), and that size is published then like any other, so the frame
 * at it is drawn while the window server is still dropping presents --
 * the first one it takes at the did is already the right size. */
- (void)windowWillEnterFullScreen:(NSNotification *)n
{
	(void)n;
	layer_hold(app->backend_state, true);
	app->is_fullscreen = true;
	app->backend_state->fs_transition = true;
	app_mark_urgent(app);
}

- (void)windowDidEnterFullScreen:(NSNotification *)n
{
	(void)n;
	layer_hold(app->backend_state, false);
	app->backend_state->fs_transition = false;
	[self keysToView];
	publish_size(app);
	fs_settle(app);
}

- (void)windowWillExitFullScreen:(NSNotification *)n
{
	(void)n;
	layer_hold(app->backend_state, true);
	app->is_fullscreen = false;
	app->backend_state->fs_transition = true;
	app_mark_urgent(app);
}

- (void)windowDidExitFullScreen:(NSNotification *)n
{
	(void)n;
	layer_hold(app->backend_state, false);
	app->backend_state->fs_transition = false;
	[self keysToView];
	publish_size(app);
	fs_settle(app);
}

- (void)windowDidFailToEnterFullScreen:(NSWindow *)w
{
	(void)w;
	layer_hold(app->backend_state, false);
	app->is_fullscreen = false;
	app->backend_state->fs_transition = false;
	publish_size(app);
	fs_settle(app);
}

- (void)windowDidFailToExitFullScreen:(NSWindow *)w
{
	(void)w;
	layer_hold(app->backend_state, false);
	app->is_fullscreen = true;
	app->backend_state->fs_transition = false;
	publish_size(app);
	fs_settle(app);
}

@end

/* The application's delegate: Quit from the menu, or Command-Q, is a close
 * request like the bar's button, and the run ends through it rather than
 * through AppKit's terminate, which would exit() under the caller. */
@interface FnlCocoaAppDelegate : NSObject <NSApplicationDelegate>
{
@public
	App *app;
}
@end

@implementation FnlCocoaAppDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s
{
	(void)s;
	if (app)
		app_close_requested(app);
	return NSTerminateCancel;
}

- (void)applicationDidHide:(NSNotification *)n
{
	(void)n;
	if (app && app->backend_state)
		publish_on_screen(app);
}

- (void)applicationDidUnhide:(NSNotification *)n
{
	(void)n;
	if (app && app->backend_state)
		publish_on_screen(app);
}

/* Back from a sleep, as Win32's PBT_APMRESUMEAUTOMATIC. */
- (void)didWake:(NSNotification *)n
{
	(void)n;
	if (app && app->backend_state)
		app_resumed(app);
}

/* A Dock or a menu bar that moved, or a screen added: same window,
 * different room. */
- (void)screensChanged:(NSNotification *)n
{
	(void)n;
	if (app && app->backend_state)
		publish_output(app, true);
}

@end

/* ---- the wakeup ------------------------------------------------------------
 *
 * A byte down a pipe, whose read end is a run-loop source in the common
 * modes: so it wakes the pump's wait in the default mode, and the library's
 * resize loop, AppKit's menus and window drags in the tracking mode, where
 * the pump is not the one waiting. In the first an event posted from here
 * ends the wait and fnl_run() drains; in the others nobody would, so this
 * drains, as Win32's FNL_WM_WAKEUP does inside its modal loops. */

/* Whether the pump is the one waiting, which only it sets, around its own
 * wait. Main thread. */
static bool pump_waiting;

static void wake_cb(CFFileDescriptorRef f, CFOptionFlags flags, void *info)
{
	(void)flags;
	App *a = info;
	struct BackendState *p = a->backend_state;
	char buf[64];
	while (read(p->wake_fd[0], buf, sizeof buf) > 0)
		;
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	if (pump_waiting) {
		NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
										location:NSZeroPoint
								   modifierFlags:0
									   timestamp:0
									windowNumber:0
										 context:nil
										 subtype:FNL_WAKE_SUBTYPE
										   data1:0
										   data2:0];
		[NSApp postEvent:e atStart:YES];
	} else {
		app_drain_requests(a);
	}
}

static bool wake_open(App *a)
{
	struct BackendState *p = a->backend_state;
	if (pipe(p->wake_fd) != 0) {
		p->wake_fd[0] = p->wake_fd[1] = -1;
		return false;
	}
	for (int i = 0; i < 2; i++) {
		fcntl(p->wake_fd[i], F_SETFL,
			  fcntl(p->wake_fd[i], F_GETFL) | O_NONBLOCK);
		fcntl(p->wake_fd[i], F_SETFD, FD_CLOEXEC);
	}
	CFFileDescriptorContext ctx = { .info = a };
	CFFileDescriptorRef f = CFFileDescriptorCreate(
		kCFAllocatorDefault, p->wake_fd[0], false, wake_cb, &ctx);
	if (!f)
		return false;
	CFRunLoopSourceRef src = CFFileDescriptorCreateRunLoopSource(
		kCFAllocatorDefault, f, 0);
	if (!src) {
		CFRelease(f);
		return false;
	}
	CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	p->wake_cf = (void *)f;
	p->wake_source = (void *)src;
	return true;
}

/* The descriptors stay open while a signal handler could still write to
 * them (App::installed_signals), as the Linux backends' do and for their
 * reason. */
static void wake_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->wake_source) {
		CFRunLoopRemoveSource(CFRunLoopGetMain(),
							  (CFRunLoopSourceRef)p->wake_source,
							  kCFRunLoopCommonModes);
		CFRelease((CFRunLoopSourceRef)p->wake_source);
		p->wake_source = NULL;
	}
	if (p->wake_cf) {
		CFFileDescriptorInvalidate((CFFileDescriptorRef)p->wake_cf);
		CFRelease((CFFileDescriptorRef)p->wake_cf);
		p->wake_cf = NULL;
	}
	if (!a->installed_signals) {
		for (int i = 0; i < 2; i++)
			if (p->wake_fd[i] >= 0)
				close(p->wake_fd[i]);
		p->wake_fd[0] = p->wake_fd[1] = -1;
	}
}

/* ---- the application -------------------------------------------------------
 *
 * Once a process: a program that opens and closes the window twenty times
 * (fenestal-dev lifecycle) is one application throughout, and finishLaunching
 * is said once. The menu is the application menu and nothing else, so that
 * Command-Q and the Dock's Quit reach the delegate. */
static bool app_launched;

static void app_launch(void)
{
	if (app_launched)
		return;
	app_launched = true;
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	/* A held letter repeats, as fenestal.h promises of text entry: macOS's
	 * press-and-hold holds it back instead, to offer its accents -- the
	 * first five repeats of a held A typed nothing. Registered as this
	 * program's default, so that a user who set it says otherwise; the
	 * accents are still the dead keys'. */
	[NSUserDefaults.standardUserDefaults
		registerDefaults:@{ @"ApplePressAndHoldEnabled" : @NO }];

	NSMenu *bar = [[NSMenu alloc] init];
	NSMenuItem *item = [[NSMenuItem alloc] init];
	[bar addItem:item];
	NSMenu *menu = [[NSMenu alloc] init];
	NSString *name = NSProcessInfo.processInfo.processName;
	[menu addItemWithTitle:[@"Hide " stringByAppendingString:name]
					action:@selector(hide:)
			 keyEquivalent:@"h"];
	[menu addItem:NSMenuItem.separatorItem];
	[menu addItemWithTitle:[@"Quit " stringByAppendingString:name]
					action:@selector(terminate:)
			 keyEquivalent:@"q"];
	item.submenu = menu;
	NSApp.mainMenu = bar;

	[NSApp finishLaunching];
}

/* ---- the backend seam -------------------------------------------------------
 *
 * FnlBackendVtbl says what an entry may assume: all but close() and
 * wakeup() run with a->backend_state and its window in place, and do not
 * test for them. */

static FnlResult cocoa_open(App *a)
{
	if (!NSThread.isMainThread)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "AppKit runs on the process's "
						"main thread only, and fnl_init() was called on "
						"another");
	a->backend_state = calloc(1, sizeof *a->backend_state);
	if (!a->backend_state)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the Cocoa backend state");
	struct BackendState *p = a->backend_state;
	p->wake_fd[0] = p->wake_fd[1] = -1;

	@autoreleasepool {
		app_launch();
		if (!wake_open(a))
			return fnl_fail(FNL_ERR_NO_DISPLAY, "the wakeup pipe could not "
							"be made (%s)", strerror(errno));

		FnlCocoaAppDelegate *ad = [[FnlCocoaAppDelegate alloc] init];
		ad->app = a;
		NSApp.delegate = ad;
		p->app_delegate = (void *)CFBridgingRetain(ad);
		NSNotificationCenter *ws = NSWorkspace.sharedWorkspace
									   .notificationCenter;
		[ws addObserver:ad selector:@selector(didWake:)
				   name:NSWorkspaceDidWakeNotification object:nil];
		[NSNotificationCenter.defaultCenter
			addObserver:ad selector:@selector(screensChanged:)
				   name:NSApplicationDidChangeScreenParametersNotification
				 object:nil];

		/* Opened on the screen with the pointer's attention, the main one,
		 * at its scale: the size asked for is the presentable area in the
		 * library's points, the bar added on top, then fitted to the room
		 * there is -- nothing on a Mac declines a borderless window's size,
		 * so the answer a window manager would give is given here, as on
		 * Win32. */
		NSScreen *screen = NSScreen.mainScreen;
		p->backing = screen ? screen.backingScaleFactor : 1.0;
		const int s120 = resolve_scale120(a, p->backing);
		a->want_scale120 = s120;
		int w_pt, h_pt;
		deco_default_window_pt(a, s120, &w_pt, &h_pt);
		NSSize sz = NSMakeSize(from_pt(a, w_pt), from_pt(a, h_pt));
		const NSSize least = floor_size(a);
		if (screen) {
			const NSSize room = screen.visibleFrame.size;
			sz.width = fmin(sz.width, room.width);
			sz.height = fmin(sz.height, room.height);
		}
		sz.width = fmax(sz.width, least.width);
		sz.height = fmax(sz.height, least.height);

		FnlCocoaWindow *w = [[FnlCocoaWindow alloc]
			initWithContentRect:NSMakeRect(0, 0, sz.width, sz.height)
					  styleMask:NSWindowStyleMaskBorderless |
								NSWindowStyleMaskResizable |
								NSWindowStyleMaskMiniaturizable
						backing:NSBackingStoreBuffered
						  defer:NO];
		if (!w)
			return fnl_fail(FNL_ERR_NO_DISPLAY, "the window was not made");
		p->window = (void *)CFBridgingRetain(w);
		w.releasedWhenClosed = NO;
		w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
		w.hasShadow = YES;
		w.opaque = YES;
		w.acceptsMouseMovedEvents = YES;
		w.title = [NSString stringWithUTF8String:a->title] ?: @"";
		/* What shows before the first frame and under a resize the frame
		 * has not caught up with: the bar's own colour, as Win32's class
		 * brush would be. */
		uint8_t rgb[3];
		app_bar_srgb8(false, rgb);
		w.backgroundColor = [NSColor colorWithSRGBRed:rgb[0] / 255.0
												green:rgb[1] / 255.0
												 blue:rgb[2] / 255.0
												alpha:1.0];

		/* The layer the bridges present into, hosted by the view: the
		 * layer first and wantsLayer after is what makes the view its host
		 * and not its owner. Opaque, a frame not at its size held as
		 * layer_hold() says, and its pixels the backing's. */
		CAMetalLayer *layer = [CAMetalLayer layer];
		if (!layer)
			return fnl_fail(FNL_ERR_NO_DISPLAY, "no CAMetalLayer was made");
		layer.opaque = YES;
		layer.contentsGravity = kCAGravityBottomLeft;
		layer.contentsScale = p->backing;
		/* What the frame's values are: sRGB, which the window server then
		 * matches to the display's own profile. Untagged, they were shown
		 * as the panel's own colours unconverted -- the bar's sRGB 41 came
		 * back from the screen as 45,45,49 -- and a wide-gamut panel
		 * would oversaturate everything. MoltenVK tags the layer the
		 * same way from the swapchain's colour space. */
		CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		layer.colorspace = srgb;
		CGColorSpaceRelease(srgb);
		p->layer = (void *)CFBridgingRetain(layer);

		FnlCocoaView *v = [[FnlCocoaView alloc]
			initWithFrame:NSMakeRect(0, 0, sz.width, sz.height)];
		v->app = a;
		v.layer = layer;
		v.wantsLayer = YES;
		w.contentView = v;
		p->view = (void *)CFBridgingRetain(v);

		FnlCocoaWindowDelegate *wd = [[FnlCocoaWindowDelegate alloc] init];
		wd->app = a;
		w.delegate = wd;
		p->window_delegate = (void *)CFBridgingRetain(wd);

		/* AppKit sends no key release while Command is held; handed to the
		 * window here, the caller's held set comes back up. */
		id mon = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
			handler:^NSEvent *(NSEvent *e) {
				if (e.modifierFlags & NSEventModifierFlagCommand)
					[e.window sendEvent:e];
				return e;
			}];
		p->key_up_monitor = (void *)CFBridgingRetain(mon);

		[w center];
		[w makeKeyAndOrderFront:nil];
		FNL_NEWER_API_BEGIN
		if ([NSApp respondsToSelector:@selector(activate)])
			[NSApp activate];
		else
			[NSApp activateIgnoringOtherApps:YES];
		FNL_NEWER_API_END

		fnl_atomic_store_bool(&p->occluded, false);
		publish_on_screen(a);
		/* Prime the size: nothing has asked for a frame yet, and the render
		 * thread must not start against a zero. */
		publish_size(a);
	}
	return FNL_OK;
}

static void cocoa_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p)
		return;
	@autoreleasepool {
		if (p->window)
			cocoa_release(a);
		/* File-scope, so they outlive the window into the next open: a
		 * press held as this one went would be released into that one. */
		left_held = false;
		left_down = nil;
		if (p->key_up_monitor)
			[NSEvent removeMonitor:CFBridgingRelease(p->key_up_monitor)];
		/* Before the window goes: its orderOut is an exit for the
		 * tracking area, and the monitor may deliver it in the next
		 * lifetime, when this App is gone. */
		if (p->view)
			[VIEW(p) detach];
		if (p->window) {
			NSWindow *w = CFBridgingRelease(p->window);
			p->window = NULL;
			w.delegate = nil;
			[w orderOut:nil];
			[w close];
		}
		if (p->view)
			CFBridgingRelease(p->view);
		if (p->layer)
			CFBridgingRelease(p->layer);
		if (p->window_delegate)
			CFBridgingRelease(p->window_delegate);
		if (p->app_delegate) {
			FnlCocoaAppDelegate *ad = CFBridgingRelease(p->app_delegate);
			[NSWorkspace.sharedWorkspace.notificationCenter
				removeObserver:ad];
			[NSNotificationCenter.defaultCenter removeObserver:ad];
			ad->app = NULL;
			if (NSApp.delegate == ad)
				NSApp.delegate = nil;
		}
		wake_close(a);
	}
	free(a->backend_state);
	a->backend_state = NULL;
}

/* Whether an event is the user's input, which alone is stamped for
 * fnl_input_latency_ms(), as Win32 stamps its key and mouse messages alone:
 * an AppKit-defined or system-defined event's age is not an input's, and a
 * counted event it led to would be measured from it. */
static bool is_input_event(NSEventType t)
{
	switch (t) {
	case NSEventTypeKeyDown:
	case NSEventTypeKeyUp:
	case NSEventTypeFlagsChanged:
	case NSEventTypeLeftMouseDown:
	case NSEventTypeLeftMouseUp:
	case NSEventTypeLeftMouseDragged:
	case NSEventTypeRightMouseDown:
	case NSEventTypeRightMouseUp:
	case NSEventTypeRightMouseDragged:
	case NSEventTypeOtherMouseDown:
	case NSEventTypeOtherMouseUp:
	case NSEventTypeOtherMouseDragged:
	case NSEventTypeMouseMoved:
	case NSEventTypeScrollWheel:
		return true;
	default:
		return false;
	}
}

/* One turn: wait for an event, then dispatch it and whatever else is queued
 * rather than going back to sleep between each. The wakeup's own event ends
 * the wait and is dispatched to nobody. */
static void cocoa_pump(App *a)
{
	@autoreleasepool {
		pump_waiting = true;
		NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
										untilDate:NSDate.distantFuture
										   inMode:NSDefaultRunLoopMode
										  dequeue:YES];
		pump_waiting = false;
		while (e) {
			/* Stamped for fnl_input_latency_ms() with the event's own
			 * time, which is the system's uptime when it was made. */
			if (!(e.type == NSEventTypeApplicationDefined &&
				  e.subtype == FNL_WAKE_SUBTYPE)) {
				if (is_input_event(e.type))
					app_input_age(a, NSProcessInfo.processInfo.systemUptime -
									 e.timestamp);
				[NSApp sendEvent:e];
			}
			pump_waiting = true;
			e = [NSApp nextEventMatchingMask:NSEventMaskAny
								   untilDate:NSDate.distantPast
									  inMode:NSDefaultRunLoopMode
									 dequeue:YES];
			pump_waiting = false;
		}
	}
}

/* Any thread, or a signal handler: a write and nothing else. */
static void cocoa_wakeup(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p && p->wake_fd[1] >= 0) {
		const char b = 1;
		ssize_t n = write(p->wake_fd[1], &b, 1);
		(void)n;              /* a full pipe is a wakeup already owed */
	}
}

static bool cocoa_window_on_screen(const App *a)
{
	return fnl_atomic_load_bool(&a->backend_state->on_screen);
}

static void cocoa_update_scale(App *a)
{
	struct BackendState *p = a->backend_state;
	NSWindow *w = WIN(p);
	const double backing = w.backingScaleFactor > 0.0 ? w.backingScaleFactor
													  : 1.0;
	const bool moved = backing != p->backing;
	p->backing = backing;
	LAYER(p).contentsScale = backing;
	(void)app_publish_scale(a, resolve_scale120(a, backing));
	hold_floor(a);
	/* The output is reported in points, so a scale change restates it; and
	 * the size, whose points the backing just moved under. */
	publish_output(a, true);
	if (moved)
		publish_size(a);
}

/* The window asked for, in the library's points: app_request_size() has
 * gated it and added the bar. The top left stays where it is, which AppKit's
 * bottom-left origin does not do by itself. */
static void cocoa_request_size(App *a, int w_pt, int h_pt)
{
	NSWindow *w = WIN(a->backend_state);
	NSRect f = w.frame;
	const CGFloat top = NSMaxY(f);
	f.size = NSMakeSize(from_pt(a, w_pt), from_pt(a, h_pt));
	f.origin.y = top - f.size.height;
	[w setFrame:f display:YES animate:NO];
}

/* Inside AppKit's animation a toggle is ignored, and the caller -- told
 * the new state as the animation starts (windowWill...) -- may ask again
 * before it ends: kept then, and asked as it ends (fs_settle()). */
static void cocoa_set_fullscreen(App *a, bool on)
{
	struct BackendState *p = a->backend_state;
	if (p->fs_transition) {
		p->fs_owed = on ? 1 : 2;
		return;
	}
	NSWindow *w = WIN(p);
	const bool now = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
	if (on != now)
		[w toggleFullScreen:nil];
}

static void cocoa_set_maximized(App *a, bool on)
{
	NSWindow *w = WIN(a->backend_state);
	if (a->is_fullscreen || on == (bool)w.zoomed)
		return;
	[w zoom:nil];
}

/* A frame in the y-down points app_place_on_output() works in: AppKit's
 * origin is the bottom-left, so the top edge is the y, negated. */
static FnlRect rect_down(NSRect r)
{
	const FnlRect f = { (int)lround(r.origin.x), (int)lround(-NSMaxY(r)),
						(int)lround(r.size.width),
						(int)lround(r.size.height) };
	return f;
}

/* To the screen of this name, keeping the window's size, where
 * app_place_on_output() puts it in that screen's visible frame. A zoomed
 * window is zoomed there: its frame is the visible frame. A fullscreen
 * window is a Space of its own, which AppKit leaves and enters through an
 * animation of its own (fs_settle()), so it stays where it is, with a line
 * saying so; the bar, and its output box, are not shown then. The change
 * of screen arrives as windowDidChangeScreen, which publishes it. */
static void cocoa_set_output(App *a, const char *name)
{
	struct BackendState *p = a->backend_state;
	NSWindow *w = WIN(p);
	NSArray<NSScreen *> *screens = NSScreen.screens;
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	const int n = screens_named(screens, names, OUTPUTS_MAX);
	NSScreen *to = nil;
	for (int i = 0; i < n && !to; i++)
		if (strcmp(names[i], name) == 0)
			to = screens[i];
	NSScreen *from = w.screen;
	if (!to || !from || screen_id(to) == screen_id(from))
		return;
	if (a->is_fullscreen) {
		fnl_log(FNL_LOG_INFO, "a fullscreen window is a Space of its own "
				"and stays where it is: not moved to %s", name);
		return;
	}
	if (w.zoomed) {
		[w setFrame:to.visibleFrame display:YES animate:NO];
		return;
	}
	const NSRect f = w.frame;
	const FnlRect fr = rect_down(from.visibleFrame);
	const FnlRect tr = rect_down(to.visibleFrame);
	const FnlRect wr = rect_down(f);
	int nx, ny;
	app_place_on_output(wr.x, wr.y, wr.w, wr.h, &fr, &tr, &nx, &ny);
	[w setFrameOrigin:NSMakePoint(nx, -ny - f.size.height)];
}

static void cocoa_minimize(App *a)
{
	[WIN(a->backend_state) miniaturize:nil];
}

/* ---- the seam's move and resize ------------------------------------------ */

/* AppKit's own drag, which snaps and tiles as a titlebar's does, handed the
 * press that is still held: the move is asked for past the slop, a turn or
 * more after the mouse-down, and AppKit takes that one as late as this. */
static void cocoa_begin_move(App *a)
{
	NSEvent *e = left_held ? left_down : nil;
	if (!e || !(NSEvent.pressedMouseButtons & 1))
		return;
	[WIN(a->backend_state) performWindowDragWithEvent:e];
}

/* No AppKit call resizes a window from an edge it does not draw, so the
 * resize is run here: the frame follows the pointer from the press until
 * the button comes up, held at the floor, in the tracking mode, where the
 * wakeup still reaches the drain. Each step is a setFrame, whose
 * windowDidResize publishes it. */
static void cocoa_begin_resize(App *a, Edge edge)
{
	struct BackendState *p = a->backend_state;
	/* Only with the button down: the loop ends at its release, and one
	 * started without a press would wait for the next. */
	if (edge <= EDGE_NONE || edge >= EDGE_COUNT || p->resizing ||
		a->is_fullscreen || !(NSEvent.pressedMouseButtons & 1))
		return;
	NSWindow *w = WIN(p);
	const NSRect start = w.frame;
	const NSPoint m0 = NSEvent.mouseLocation;
	const NSSize least = floor_size(a);
	const bool L = edge == EDGE_L || edge == EDGE_TL || edge == EDGE_BL;
	const bool R = edge == EDGE_R || edge == EDGE_TR || edge == EDGE_BR;
	const bool T = edge == EDGE_T || edge == EDGE_TL || edge == EDGE_TR;
	const bool B = edge == EDGE_B || edge == EDGE_BL || edge == EDGE_BR;

	p->resizing = true;
	for (;;) {
		@autoreleasepool {
			NSEvent *e = [NSApp
				nextEventMatchingMask:NSEventMaskLeftMouseDragged |
									  NSEventMaskLeftMouseUp
							untilDate:NSDate.distantFuture
							   inMode:NSEventTrackingRunLoopMode
							  dequeue:YES];
			if (!e || e.type == NSEventTypeLeftMouseUp)
				break;
			const NSPoint m = NSEvent.mouseLocation;
			const CGFloat dx = m.x - m0.x, dy = m.y - m0.y;
			NSRect f = start;
			/* Screen y counts up, so the top edge is the frame's far end. */
			if (R)
				f.size.width = fmax(start.size.width + dx, least.width);
			if (L) {
				f.size.width = fmax(start.size.width - dx, least.width);
				f.origin.x = NSMaxX(start) - f.size.width;
			}
			if (T)
				f.size.height = fmax(start.size.height + dy, least.height);
			if (B) {
				f.size.height = fmax(start.size.height - dy, least.height);
				f.origin.y = NSMaxY(start) - f.size.height;
			}
			if (!NSEqualRects(f, w.frame))
				[w setFrame:f display:YES animate:NO];
		}
	}
	p->resizing = false;
	/* The release was this loop's, and the caller saw the press. */
	const NSPoint end =
		[VIEW(p) convertPoint:w.mouseLocationOutsideOfEventStream
					 fromView:nil];
	left_release(a, NSMakePoint(to_pt(a, end.x), to_pt(a, end.y)));
}

/* The floor moved: a custom bar asked for more room than the window has. */
static void cocoa_apply_size_bounds(App *a)
{
	hold_floor(a);
	NSWindow *w = WIN(a->backend_state);
	if (a->is_fullscreen || a->is_maximized || w.miniaturized)
		return;
	const NSSize least = floor_size(a);
	NSRect f = w.frame;
	if (f.size.width >= least.width && f.size.height >= least.height)
		return;
	const CGFloat top = NSMaxY(f);
	f.size.width = fmax(f.size.width, least.width);
	f.size.height = fmax(f.size.height, least.height);
	f.origin.y = top - f.size.height;
	[w setFrame:f display:YES animate:NO];
}

/* ---- the clipboard ----------------------------------------------------------
 *
 * The general pasteboard, as plain text both ways. Synchronous: AppKit
 * holds the text or asks its owner for it with a bound of its own. */

static void cocoa_clipboard_set(App *a)
{
	NSString *s = [[NSString alloc] initWithBytes:a->clip_out
										   length:a->clip_out_len
										 encoding:NSUTF8StringEncoding];
	if (!s)
		return;
	NSPasteboard *pb = NSPasteboard.generalPasteboard;
	[pb clearContents];
	[pb setString:s forType:NSPasteboardTypeString];
}

/* Delivered even when there was nothing, because a request always gets its
 * event. */
static void cocoa_clipboard_request(App *a)
{
	@autoreleasepool {
		NSString *s = [NSPasteboard.generalPasteboard
			stringForType:NSPasteboardTypeString];
		const char *u = s.UTF8String;
		app_clipboard_deliver(a, u, u ? strlen(u) : 0);
	}
}

/* ---- the rest ------------------------------------------------------------ */

static void cocoa_set_title(App *a, const char *utf8)
{
	WIN(a->backend_state).title = [NSString stringWithUTF8String:utf8] ?: @"";
}

/* fnl_set_cursor() moved the shape: put it up now if the pointer is over
 * the window, rather than at its next move. */
static void cocoa_cursor_changed(App *a)
{
	struct BackendState *p = a->backend_state;
	const NSPoint m = [WIN(p) mouseLocationOutsideOfEventStream];
	const NSPoint v = [VIEW(p) convertPoint:m fromView:nil];
	if (!NSPointInRect(v, VIEW(p).bounds))
		return;
	Edge edge;
	const Zone z = deco_hit(a, to_pt(a, v.x), to_pt(a, v.y), &edge);
	cursor_apply(a, z, edge);
}

/* Text entry went off with a composition up, or the caret moved: the input
 * method is told, so its window follows or goes. */
static void cocoa_text_input_changed(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!app_text_input(a))
		compose_discard(a);
	[VIEW(p).inputContext invalidateCharacterCoordinates];
}

const FnlBackendVtbl fnl_backend_cocoa = {
	.name               = "Cocoa",
	.id                 = BACKEND_COCOA,
	.key                = "cocoa",
	.absent             = backend_always_present,
	.host               = backend_noop_host,
	.shared_driver      = backend_noop_shared_driver,
	.gpu_drivers        = backend_noop_gpu_drivers,
	.gpu_driver_set     = backend_noop_gpu_driver_set,
	.gpu_driver_held    = backend_noop_gpu_driver_held,
	/* No ring: the window is its frame, and the edges are an inner margin
	 * deco_hit() answers for. */
	.grab_ring          = 0,
	/* No invitation to draw: the layer's nextDrawable is the throttle. */
	.invites            = false,
	/* The size is AppKit's the moment it is set, with nobody to hold an
	 * answer for. */
	.ack_when_shown     = false,
	.is_output          = false,
	.open               = cocoa_open,
	.close              = cocoa_close,
	.pump               = cocoa_pump,
	.wakeup             = cocoa_wakeup,
	.check_error        = backend_noop_check_error,
	.flush              = backend_noop,
	.present_pixels     = backend_noop_present_pixels,
	.set_geometry       = backend_noop_set_geometry,
	.ack_configure      = backend_noop_ack_configure,
	.frame_request      = backend_noop,
	.frame_cancel       = backend_noop,
	.present_window_renew = backend_noop,
	.present_window_hold  = backend_noop,
	.frame_wait         = backend_frame_wait_shown,
	.window_on_screen   = cocoa_window_on_screen,
	.apply_scale        = backend_noop_apply_scale,
	.request_size       = cocoa_request_size,
	.nudge_size         = backend_noop,
	.set_fullscreen     = cocoa_set_fullscreen,
	.set_maximized      = cocoa_set_maximized,
	.minimize           = cocoa_minimize,
	.modes              = backend_noop_modes,
	.outputs            = backend_published_outputs,
	.set_mode           = backend_noop_set_mode,
	.set_output         = cocoa_set_output,
	.begin_move         = cocoa_begin_move,
	.begin_resize       = cocoa_begin_resize,
	.apply_size_bounds  = cocoa_apply_size_bounds,
	.buffer_granularity = backend_noop_buffer_granularity,
	.update_scale       = cocoa_update_scale,
	.capture_pointer    = cocoa_capture,
	.release_pointer    = cocoa_release,
	.cursor_changed     = cocoa_cursor_changed,
	.text_input_changed = cocoa_text_input_changed,
	.clipboard_set      = cocoa_clipboard_set,
	.clipboard_request  = cocoa_clipboard_request,
	.set_title          = cocoa_set_title,
};
