/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What a Mac's desktop can be asked about a window, and what it can be made
 * to do, for a harness that is not on it: windows_agent.ps1's questions, put
 * to macOS, one line in and one line out until told to go.
 *
 * It is run over ssh as the logged-in user, and a process started that way
 * reaches the window server, so there is no scheduled task as on Windows and
 * no socket: the ssh session's stdin and stdout are the line, and the harness
 * holds it for the round (mac_driver.py). Cross-compiled on the harness's
 * machine as the library is, and copied over each round with the program.
 *
 * Two of macOS's permissions stand between this and the desktop, and both
 * are granted by hand, once, to what an ssh session is to the privacy
 * database -- /usr/libexec/sshd-keygen-wrapper: Screen Recording, without
 * which a window's title is withheld and a picture is the wallpaper; and
 * Accessibility, without which a posted event is dropped without a word and
 * another program's window can be neither raised nor moved. `trusted` says
 * whether the second is there, which a round asks first.
 *
 * The answers are in the window server's own coordinates: points, the origin
 * at the main display's top left, y down -- which is what CGEvent takes and
 * what the window list gives, and not AppKit's y-up frames, which are turned
 * over here.
 */

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <IOKit/IOKitLib.h>
#import <ImageIO/ImageIO.h>

#include <dlfcn.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>

/* The window found, by its process and its number in the window list. */
static pid_t      g_pid;
static CGWindowID g_wid;
/* What is held down as far as this agent's events say: the buttons, which
 * make a move a drag, and the modifiers, whose flags every event carries --
 * the library reads which side of a modifier is down from the flags'
 * device-dependent bits. */
static int        g_buttons;
static CGEventFlags g_flags;
/* When this agent last sent anything, for telling its input from a hand's. */
static double     g_last_input;

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static NSString *b64(const char *s)
{
	NSData *d = [[NSData alloc]
		initWithBase64EncodedString:[NSString stringWithUTF8String:s]
							options:0];
	return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
			 : @"";
}

static NSString *to_b64(NSString *s)
{
	return [[s dataUsingEncoding:NSUTF8StringEncoding]
		base64EncodedStringWithOptions:0];
}

/* ---- the screens --------------------------------------------------------- */

/* AppKit's frame turned into the window server's: the same width and height,
 * y measured down from the main display's top. */
static CGRect flip(NSRect r)
{
	const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
	return CGRectMake(r.origin.x, top - NSMaxY(r), r.size.width,
					  r.size.height);
}

static NSString *rect_words(CGRect r)
{
	return [NSString stringWithFormat:@"%d %d %d %d", (int)r.origin.x,
			(int)r.origin.y, (int)r.size.width, (int)r.size.height];
}

/* The screen a point of the window server's is on, or the main one. */
static NSScreen *screen_at(CGPoint p)
{
	for (NSScreen *s in NSScreen.screens)
		if (CGRectContainsPoint(flip(s.frame), p))
			return s;
	return NSScreen.screens.firstObject;
}

/* A screen's name, which has spaces in it, as a word of the line. */
static NSString *screen_name(NSScreen *s)
{
	return to_b64(s.localizedName ?: @"display");
}

/* ---- the window ---------------------------------------------------------- */

static NSDictionary *window_info(void)
{
	CFArrayRef list = CGWindowListCopyWindowInfo(
		kCGWindowListOptionIncludingWindow, g_wid);
	NSArray *a = CFBridgingRelease(list);
	return a.count ? a[0] : nil;
}

static bool window_rect(CGRect *r)
{
	NSDictionary *w = window_info();
	if (!w)
		return false;
	return CGRectMakeWithDictionaryRepresentation(
		(__bridge CFDictionaryRef)w[(id)kCGWindowBounds], r);
}

/* The window of a process named `image` whose title has `title` in it. The
 * title is withheld without Screen Recording, and then the process's own
 * window on the ordinary layer is taken: every program here opens one. But
 * AppKit gives each program untitled windows of its own besides, off screen
 * on the same layer, so one with the title comes first, then an untitled
 * one on screen, and then any. */
static bool find(const char *image, const char *title)
{
	NSArray *all = CFBridgingRelease(CGWindowListCopyWindowInfo(
		kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
		kCGNullWindowID));
	NSString *want = [NSString stringWithUTF8String:image];
	NSString *part = [NSString stringWithUTF8String:title];
	for (int pass = 0; pass < 3; pass++) {
		for (NSDictionary *w in all) {
			if (![w[(id)kCGWindowOwnerName] isEqualToString:want] ||
				[w[(id)kCGWindowLayer] intValue] != 0)
				continue;
			NSString *name = w[(id)kCGWindowName];
			const bool named = name.length != 0;
			const bool fits =
				pass == 0 ? named && [name rangeOfString:part].location !=
										 NSNotFound
				: pass == 1 ? !named &&
								  [w[(id)kCGWindowIsOnscreen] boolValue]
							: !named;
			if (!fits)
				continue;
			CGRect r;
			if (!CGRectMakeWithDictionaryRepresentation(
					(__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r) ||
				r.size.width < 8 || r.size.height < 8)
				continue;
			g_pid = [w[(id)kCGWindowOwnerPID] intValue];
			g_wid = [w[(id)kCGWindowNumber] unsignedIntValue];
			return true;
		}
	}
	return false;
}

/* The window as the Accessibility API has it: the program's window whose
 * frame is the one found, or its first where none matches -- in fullscreen
 * AppKit gives it a second, the strip its titlebar would slide down into,
 * and raising that one takes the keyboard from the window. Owned by the
 * caller. */
static AXUIElementRef ax_window(void)
{
	AXUIElementRef app = AXUIElementCreateApplication(g_pid);
	CFTypeRef wins = NULL;
	AXUIElementRef w = NULL;
	CGRect want;
	const bool have = window_rect(&want);
	if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins) ==
			kAXErrorSuccess && CFArrayGetCount(wins) > 0) {
		const CFIndex n = CFArrayGetCount(wins);
		for (CFIndex i = 0; i < n && have && !w; i++) {
			AXUIElementRef c = (AXUIElementRef)CFArrayGetValueAtIndex(wins, i);
			CFTypeRef pv = NULL, sv = NULL;
			CGPoint p;
			CGSize sz;
			if (AXUIElementCopyAttributeValue(c, kAXPositionAttribute, &pv) ==
					kAXErrorSuccess &&
				AXUIElementCopyAttributeValue(c, kAXSizeAttribute, &sv) ==
					kAXErrorSuccess &&
				AXValueGetValue(pv, kAXValueTypeCGPoint, &p) &&
				AXValueGetValue(sv, kAXValueTypeCGSize, &sz) &&
				fabs(p.x - want.origin.x) < 2 &&
				fabs(p.y - want.origin.y) < 2 &&
				fabs(sz.width - want.size.width) < 2 &&
				fabs(sz.height - want.size.height) < 2)
				w = c;
			if (pv)
				CFRelease(pv);
			if (sv)
				CFRelease(sv);
		}
		if (!w)
			w = (AXUIElementRef)CFArrayGetValueAtIndex(wins, 0);
		CFRetain(w);
	}
	if (wins)
		CFRelease(wins);
	CFRelease(app);
	return w;
}

static bool ax_bool(AXUIElementRef e, CFStringRef attr)
{
	CFTypeRef v = NULL;
	bool on = false;
	if (AXUIElementCopyAttributeValue(e, attr, &v) == kAXErrorSuccess && v) {
		on = CFBooleanGetValue(v);
		CFRelease(v);
	}
	return on;
}

/* In front: the program owns the first window on the ordinary layer, by
 * the window list's order, which is the front-to-back one. Its owner and
 * not its number: in fullscreen AppKit puts a strip of the program's own in
 * front of the window, where its titlebar would slide down. */
static bool foreground(void)
{
	NSArray *on = CFBridgingRelease(CGWindowListCopyWindowInfo(
		kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
		kCGNullWindowID));
	for (NSDictionary *w in on)
		if ([w[(id)kCGWindowLayer] intValue] == 0)
			return [w[(id)kCGWindowOwnerPID] intValue] == g_pid;
	return false;
}

static bool minimized(void)
{
	AXUIElementRef w = ax_window();
	if (!w)
		return false;
	const bool on = ax_bool(w, kAXMinimizedAttribute);
	CFRelease(w);
	return on;
}

/* Brought back if it is in the Dock, raised, and its program made the
 * active one -- what a click on it would do. */
static bool focus(void)
{
	AXUIElementRef w = ax_window();
	if (w) {
		if (ax_bool(w, kAXMinimizedAttribute))
			AXUIElementSetAttributeValue(w, kAXMinimizedAttribute,
										 kCFBooleanFalse);
		AXUIElementPerformAction(w, kAXRaiseAction);
		AXUIElementSetAttributeValue(w, kAXMainAttribute, kCFBooleanTrue);
		CFRelease(w);
	}
	AXUIElementRef app = AXUIElementCreateApplication(g_pid);
	AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
	CFRelease(app);
	[[NSRunningApplication runningApplicationWithProcessIdentifier:g_pid]
		activateWithOptions:NSApplicationActivateAllWindows];
	for (int i = 0; i < 50 && !foreground(); i++)
		usleep(10000);
	return foreground();
}

static bool move_by(int dx, int dy)
{
	AXUIElementRef w = ax_window();
	if (!w)
		return false;
	CFTypeRef v = NULL;
	CGPoint p;
	bool ok = false;
	if (AXUIElementCopyAttributeValue(w, kAXPositionAttribute, &v) ==
			kAXErrorSuccess &&
		AXValueGetValue(v, kAXValueTypeCGPoint, &p)) {
		p.x += dx;
		p.y += dy;
		AXValueRef nv = AXValueCreate(kAXValueTypeCGPoint, &p);
		ok = AXUIElementSetAttributeValue(w, kAXPositionAttribute, nv) ==
			 kAXErrorSuccess;
		CFRelease(nv);
	}
	if (v)
		CFRelease(v);
	CFRelease(w);
	return ok;
}

/* Processor time the program has used, user and system, in seconds. */
static double cpu_seconds(void)
{
	struct rusage_info_v2 ri;
	if (proc_pid_rusage(g_pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0)
		return -1.0;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	const double ticks = (double)(ri.ri_user_time + ri.ri_system_time);
	return ticks * tb.numer / tb.denom * 1e-9;
}

/* Whether the program has a library of that name mapped, by its file's
 * name: the one way to know a layer the environment asked for is there,
 * since a loader that cannot load one says so to nobody. Read region by
 * region from the kernel's account of the process (libproc), which a
 * process of the same user is given; vmmap would want task_for_pid. */
static bool has_module(const char *name)
{
	uint64_t at = 0;
	struct proc_regionwithpathinfo ri;
	while (proc_pidinfo(g_pid, PROC_PIDREGIONPATHINFO, at, &ri, sizeof ri)
		   == (int)sizeof ri) {
		const char *path = ri.prp_vip.vip_path;
		const char *base = strrchr(path, '/');
		if (path[0] && strstr(base ? base + 1 : path, name))
			return true;
		const uint64_t next = ri.prp_prinfo.pri_address
							+ ri.prp_prinfo.pri_size;
		if (next <= at)
			break;
		at = next;
	}
	return false;
}

/* ---- input --------------------------------------------------------------- */

static CGPoint pointer(void)
{
	CGEventRef e = CGEventCreate(NULL);
	const CGPoint p = CGEventGetLocation(e);
	CFRelease(e);
	return p;
}

/* What marks an event as the agent's own, for the hold below to let
 * through: a field no hardware event sets. */
static const int64_t FNL_MARK = 0x666e6c64;            /* "fnld" */

static void post(CGEventRef e)
{
	CGEventSetIntegerValueField(e, kCGEventSourceUserData, FNL_MARK);
	CGEventPost(kCGHIDEventTap, e);
	CFRelease(e);
	g_last_input = now_s();
}

/* ---- the Mac's own input, held for a round --------------------------------
 *
 * An event tap at the session's head, on a thread of its own and its run
 * loop: every key, button, motion and scroll that is not marked the
 * agent's is dropped, so the Mac's keyboard and touchpad reach nothing
 * while a round types and points. Control+Option+Escape gives them back
 * (held 2). The tap goes with the process, so an agent that ends lets go;
 * one macOS disables for being slow is enabled again, and one that is
 * never released ends after HOLD_LIMIT_S regardless, as input_hold.py's
 * does. Accessibility, which the round already needs, is what lets it drop
 * events. */

#define HOLD_LIMIT_S 1800.0

static CFMachPortRef g_tap;
/* The holder's run loop, retained by it and released by whoever joins it,
 * so that a stop sent as the holder ends never reaches a freed loop. */
static CFRunLoopRef volatile g_tap_loop;
static pthread_t g_holder;
static bool g_holding;
/* 0 not held, 1 held, 2 given back by hand, -1 refused. */
static volatile int g_held;
static volatile bool g_hold_done;
/* Set by release_input() and checked by the holder once its tap is made: a
 * stop sent before its loop was there is lost, and this still stops it. */
static volatile bool g_hold_cancel;

static CGEventRef tap_event(CGEventTapProxy proxy, CGEventType type,
							CGEventRef e, void *unused)
{
	(void)proxy;
	(void)unused;
	if (type == kCGEventTapDisabledByTimeout ||
		type == kCGEventTapDisabledByUserInput) {
		if (g_held == 1)
			CGEventTapEnable(g_tap, true);
		return e;
	}
	if (g_held != 1 ||
		CGEventGetIntegerValueField(e, kCGEventSourceUserData) == FNL_MARK)
		return e;
	if (type == kCGEventKeyDown &&
		CGEventGetIntegerValueField(e, kCGKeyboardEventKeycode) == 53) {
		const CGEventFlags f = CGEventGetFlags(e);
		if ((f & kCGEventFlagMaskControl) && (f & kCGEventFlagMaskAlternate)) {
			g_held = 2;
			CFRunLoopStop(g_tap_loop);
		}
	}
	return NULL;
}

static void *holding(void *unused)
{
	(void)unused;
	const CGEventMask m =
		CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
		CGEventMaskBit(kCGEventFlagsChanged) |
		CGEventMaskBit(kCGEventLeftMouseDown) |
		CGEventMaskBit(kCGEventLeftMouseUp) |
		CGEventMaskBit(kCGEventRightMouseDown) |
		CGEventMaskBit(kCGEventRightMouseUp) |
		CGEventMaskBit(kCGEventOtherMouseDown) |
		CGEventMaskBit(kCGEventOtherMouseUp) |
		CGEventMaskBit(kCGEventMouseMoved) |
		CGEventMaskBit(kCGEventLeftMouseDragged) |
		CGEventMaskBit(kCGEventRightMouseDragged) |
		CGEventMaskBit(kCGEventOtherMouseDragged) |
		CGEventMaskBit(kCGEventScrollWheel);
	g_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
							 kCGEventTapOptionDefault, m, tap_event, NULL);
	if (!g_tap) {
		g_held = -1;
		g_hold_done = true;
		return NULL;
	}
	CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, g_tap, 0);
	g_tap_loop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
	CFRunLoopAddSource(g_tap_loop, src, kCFRunLoopCommonModes);
	if (!g_hold_cancel) {
		CGEventTapEnable(g_tap, true);
		g_held = 1;
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, HOLD_LIMIT_S, false);
	}
	CGEventTapEnable(g_tap, false);
	CFRunLoopRemoveSource(g_tap_loop, src, kCFRunLoopCommonModes);
	CFRelease(src);
	CFMachPortInvalidate(g_tap);
	CFRelease(g_tap);
	g_tap = NULL;
	if (g_held == 1)
		g_held = 0;
	g_hold_done = true;
	return NULL;
}

static NSString *release_input(void);

static NSString *hold_input(void)
{
	if (g_holding && g_held == 1)
		return @"ok";
	/* One given back by hand, or still starting: ended first, and never
	 * joined while it might yet hold for HOLD_LIMIT_S. */
	release_input();
	if (g_holding)
		return @"err the last hold has not let go";
	g_held = 0;
	g_hold_done = false;
	g_hold_cancel = false;
	if (pthread_create(&g_holder, NULL, holding, NULL) != 0)
		return @"err no thread for the hold";
	g_holding = true;
	for (int i = 0; i < 100 && g_held == 0; i++)
		usleep(20000);
	if (g_held == 1)
		return @"ok";
	/* Not one that starts holding after the round was told it did not. */
	release_input();
	return @"err the event tap was refused";
}

static NSString *release_input(void)
{
	g_hold_cancel = true;
	if (g_holding) {
		/* Again until it ends: a stop sent before the loop runs is lost. */
		for (int i = 0; i < 200 && !g_hold_done; i++) {
			if (g_tap_loop)
				CFRunLoopStop(g_tap_loop);
			usleep(10000);
		}
		if (!g_hold_done)
			return @"err the hold would not end";
		pthread_join(g_holder, NULL);
		g_holding = false;
		if (g_tap_loop) {
			CFRelease(g_tap_loop);
			g_tap_loop = NULL;
		}
	}
	return @"ok";
}

/* A move, which with a button held is that button's drag. */
static void warp(CGFloat x, CGFloat y)
{
	CGEventType t = kCGEventMouseMoved;
	CGMouseButton b = kCGMouseButtonLeft;
	if (g_buttons & 1) {
		t = kCGEventLeftMouseDragged;
	} else if (g_buttons & 2) {
		t = kCGEventRightMouseDragged;
		b = kCGMouseButtonRight;
	} else if (g_buttons & 4) {
		t = kCGEventOtherMouseDragged;
		b = kCGMouseButtonCenter;
	}
	CGEventRef e = CGEventCreateMouseEvent(NULL, t, CGPointMake(x, y), b);
	CGEventSetFlags(e, g_flags);
	post(e);
}

/* Movement with nowhere to go: the deltas a captured pointer is read by,
 * the cursor left where it is -- it is detached from the mouse then, and
 * a move that took it off the window would take the events with it. */
static void nudge(int dx, int dy)
{
	CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved,
										   pointer(), kCGMouseButtonLeft);
	CGEventSetIntegerValueField(e, kCGMouseEventDeltaX, dx);
	CGEventSetIntegerValueField(e, kCGMouseEventDeltaY, dy);
	CGEventSetFlags(e, g_flags);
	post(e);
}

/* 1 left, 2 right, 3 middle, as the harness numbers them. */
static void button(int which, bool down)
{
	static const CGEventType DOWN[] = { kCGEventLeftMouseDown,
										kCGEventRightMouseDown,
										kCGEventOtherMouseDown };
	static const CGEventType UP[] = { kCGEventLeftMouseUp,
									  kCGEventRightMouseUp,
									  kCGEventOtherMouseUp };
	static const CGMouseButton BTN[] = { kCGMouseButtonLeft,
										 kCGMouseButtonRight,
										 kCGMouseButtonCenter };
	if (which < 1 || which > 3)
		return;
	const int i = which - 1;
	CGEventRef e = CGEventCreateMouseEvent(NULL, down ? DOWN[i] : UP[i],
										   pointer(), BTN[i]);
	CGEventSetIntegerValueField(e, kCGMouseEventClickState, 1);
	CGEventSetFlags(e, g_flags);
	if (down)
		g_buttons |= 1 << i;
	else
		g_buttons &= ~(1 << i);
	post(e);
}

/* A step towards the user is a positive one to the harness, and a negative
 * line to the window server. */
static void wheel(int steps)
{
	CGEventRef e = CGEventCreateScrollWheelEvent(
		NULL, kCGScrollEventUnitLine, 1, -steps);
	CGEventSetFlags(e, g_flags);
	post(e);
}

/* The modifiers by key code: the flag every application reads, and the
 * device-dependent bit that says which side (IOKit's NX_DEVICE*KEYMASK). */
static const struct {
	CGKeyCode    code;
	CGEventFlags flag, side;
} MODS[] = {
	{ 0x38, kCGEventFlagMaskShift,     0x0002 },
	{ 0x3c, kCGEventFlagMaskShift,     0x0004 },
	{ 0x3b, kCGEventFlagMaskControl,   0x0001 },
	{ 0x3e, kCGEventFlagMaskControl,   0x2000 },
	{ 0x3a, kCGEventFlagMaskAlternate, 0x0020 },
	{ 0x3d, kCGEventFlagMaskAlternate, 0x0040 },
	{ 0x37, kCGEventFlagMaskCommand,   0x0008 },
	{ 0x36, kCGEventFlagMaskCommand,   0x0010 },
};
#define KC_CAPS_LOCK 0x39

/* ---- the system's key repeat, for the keys this agent holds -----------------
 *
 * macOS repeats a key held on a keyboard and never one posted: a posted
 * key-down is one press however long it is left down. So the agent stands
 * in for the system's repeat for the key it holds -- a key-down marked as a
 * repeat, at the delay and the rate the user's keyboard settings give, from
 * a thread of its own -- as the hardware would have it repeated. Any other
 * key, or that one's release, ends it, as on a keyboard. */
static pthread_mutex_t g_rep_lock = PTHREAD_MUTEX_INITIALIZER;
static CGKeyCode g_rep_code;
static bool      g_rep_on;
static double    g_rep_next;

/* InitialKeyRepeat and KeyRepeat, the settings' own units of 15 ms; macOS's
 * defaults where the user set none. */
static double repeat_pref(CFStringRef name, double fallback)
{
	CFTypeRef v = CFPreferencesCopyAppValue(name,
											kCFPreferencesAnyApplication);
	double ticks = fallback;
	if (v && CFGetTypeID(v) == CFNumberGetTypeID())
		CFNumberGetValue(v, kCFNumberDoubleType, &ticks);
	if (v)
		CFRelease(v);
	return ticks * 0.015;
}

static void *repeater(void *unused)
{
	(void)unused;
	const double delay = repeat_pref(CFSTR("InitialKeyRepeat"), 25);
	const double every = repeat_pref(CFSTR("KeyRepeat"), 6);
	for (;;) {
		usleep(5000);
		pthread_mutex_lock(&g_rep_lock);
		if (g_rep_on && g_rep_next == 0.0)
			g_rep_next = now_s() + delay;
		if (g_rep_on && now_s() >= g_rep_next) {
			CGEventRef e = CGEventCreateKeyboardEvent(NULL, g_rep_code, true);
			CGEventSetIntegerValueField(e, kCGKeyboardEventAutorepeat, 1);
			CGEventSetFlags(e, g_flags);
			post(e);
			g_rep_next += every;
		}
		pthread_mutex_unlock(&g_rep_lock);
	}
	return NULL;
}

/* Called with g_rep_lock held, by the key's own edge. */
static void repeat_hold(CGKeyCode code, bool down)
{
	if (down) {
		g_rep_code = code;
		g_rep_on = true;
		g_rep_next = 0.0;
	} else if (g_rep_code == code) {
		g_rep_on = false;
	}
}

/* One edge of a key, by the Mac's key code. A modifier is a flags change
 * carrying the flags after it, as the hardware's is; every other key
 * carries the modifiers held. Called with g_rep_lock held: see key(). */
static void key_locked(CGKeyCode code, bool down)
{
	for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++) {
		if (MODS[i].code != code)
			continue;
		if (down)
			g_flags |= MODS[i].side;
		else
			g_flags &= ~MODS[i].side;
		/* The side-free flag, while either side is down. */
		CGEventFlags any = 0;
		for (size_t j = 0; j < sizeof MODS / sizeof MODS[0]; j++)
			if (MODS[j].flag == MODS[i].flag)
				any |= MODS[j].side;
		if (g_flags & any)
			g_flags |= MODS[i].flag;
		else
			g_flags &= ~MODS[i].flag;
		CGEventRef e = CGEventCreateKeyboardEvent(NULL, code, down);
		CGEventSetType(e, kCGEventFlagsChanged);
		CGEventSetFlags(e, g_flags);
		post(e);
		return;
	}
	if (code == KC_CAPS_LOCK) {
		if (!down)
			return;
		g_flags ^= kCGEventFlagMaskAlphaShift;
		CGEventRef e = CGEventCreateKeyboardEvent(NULL, code, true);
		CGEventSetType(e, kCGEventFlagsChanged);
		CGEventSetFlags(e, g_flags);
		post(e);
		return;
	}
	CGEventRef e = CGEventCreateKeyboardEvent(NULL, code, down);
	CGEventSetFlags(e, g_flags);
	post(e);
	repeat_hold(code, down);
}

/* Every edge under the repeater's lock: the repeater reads g_flags for each
 * repeat it posts, which a modifier's edge writes, and a repeat must not
 * fall between a key's release and the end of its repeating. The other
 * readers of g_flags are this thread's. */
static void key(CGKeyCode code, bool down)
{
	pthread_mutex_lock(&g_rep_lock);
	key_locked(code, down);
	pthread_mutex_unlock(&g_rep_lock);
}

/* How long ago a hand was on the machine, in milliseconds, or -1 when the
 * last input was this agent's own: the system's idle time, read against the
 * note the agent leaves of its last input, here and across rounds. */
static long long idle_ms(const char *note)
{
	io_service_t hid = IOServiceGetMatchingService(
		kIOMainPortDefault, IOServiceMatching("IOHIDSystem"));
	if (!hid)
		return -1;
	CFTypeRef v = IORegistryEntryCreateCFProperty(hid, CFSTR("HIDIdleTime"),
												  kCFAllocatorDefault, 0);
	IOObjectRelease(hid);
	long long ns = 0;
	if (!v || !CFNumberGetValue(v, kCFNumberSInt64Type, &ns)) {
		if (v)
			CFRelease(v);
		return -1;
	}
	CFRelease(v);
	double ours = g_last_input;
	FILE *f = fopen(note, "r");
	if (f) {
		double then = 0.0;
		if (fscanf(f, "%lf", &then) == 1 && then > ours)
			ours = then;
		fclose(f);
	}
	/* Ours when nothing came after the agent's last event. Not "at the
	 * same moment": the idle clock is reset by some posted events and not
	 * others, and was seen 1.3 s short of the agent's last button-up. */
	const double idle = (double)ns * 1e-9;
	if (ours > 0.0 && now_s() - idle <= ours + 0.5)
		return -1;
	return (long long)(idle * 1000.0);
}

static void leave_note(const char *note)
{
	if (!note[0] || g_last_input <= 0.0)
		return;
	FILE *f = fopen(note, "w");
	if (f) {
		fprintf(f, "%.3f\n", g_last_input);
		fclose(f);
	}
}

/* ---- the clipboard ------------------------------------------------------- */

static NSString *clip_get(void)
{
	return [NSPasteboard.generalPasteboard
			   stringForType:NSPasteboardTypeString] ?: @"";
}

static void clip_put(NSString *s)
{
	NSPasteboard *pb = NSPasteboard.generalPasteboard;
	[pb clearContents];
	[pb setString:s forType:NSPasteboardTypeString];
}

/* ---- a picture --------------------------------------------------------------
 *
 * screencapture writes the display's own colours, with the display's profile
 * beside them. Matched to sRGB through that profile anywhere but here, the
 * dark end comes back wrong -- the testbed's field, sRGB 14, read 10 -- as
 * every rendering intent had it. So it is matched here, by ColorSync, which
 * undoes its own conversion: drawn into an sRGB bitmap, the field reads 14
 * and the bar 41,41,46, what was drawn, and the PNG written out is the
 * plain sRGB a round reads pixel values as. */
static bool shot_srgb(NSString *path)
{
	NSString *raw = [path stringByAppendingString:@".raw.png"];
	NSTask *t = [[NSTask alloc] init];
	t.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
	t.arguments = @[ @"-x", raw ];
	/* Stdout is the harness's, a line an answer; nothing else writes it. */
	t.standardOutput = NSFileHandle.fileHandleWithNullDevice;
	t.standardError = NSFileHandle.fileHandleWithNullDevice;
	if (![t launchAndReturnError:nil])
		return false;
	[t waitUntilExit];
	if (t.terminationStatus != 0)
		return false;
	CGImageSourceRef src = CGImageSourceCreateWithURL(
		(__bridge CFURLRef)[NSURL fileURLWithPath:raw], NULL);
	CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL)
						 : NULL;
	if (src)
		CFRelease(src);
	if (!img)
		return false;
	const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
	CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef cx = CGBitmapContextCreate(
		NULL, w, h, 8, 0, srgb,
		(CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault);
	bool ok = false;
	if (cx) {
		CGContextSetRenderingIntent(cx, kCGRenderingIntentRelativeColorimetric);
		CGContextDrawImage(cx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
		CGImageRef out = CGBitmapContextCreateImage(cx);
		CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
			(__bridge CFURLRef)[NSURL fileURLWithPath:path],
			CFSTR("public.png"), 1, NULL);
		if (out && dst) {
			CGImageDestinationAddImage(dst, out, NULL);
			ok = CGImageDestinationFinalize(dst);
		}
		if (dst)
			CFRelease(dst);
		if (out)
			CGImageRelease(out);
		CGContextRelease(cx);
	}
	CGColorSpaceRelease(srgb);
	CGImageRelease(img);
	[NSFileManager.defaultManager removeItemAtPath:raw error:nil];
	return ok;
}

/* ---- the input source ---------------------------------------------------- *
 *
 * windows_agent.ps1's layout questions, put to the Text Input Sources: a
 * layout is named by its source ID (`com.apple.keylayout.US`), and the
 * selection is the session's, so the window types through the one selected
 * here. A source that is not enabled cannot be selected, and enabling one
 * puts it in the user's list -- which is the harness's to take out again
 * afterwards, and only when it was not there before (`layoutdrop`). */

static TISInputSourceRef source_by_id(NSString *sid)
{
	NSArray *all = CFBridgingRelease(TISCreateInputSourceList(
		(__bridge CFDictionaryRef)@{
			(__bridge id)kTISPropertyInputSourceID : sid }, true));
	if (!all.count)
		return NULL;
	return (TISInputSourceRef)CFRetain((__bridge CFTypeRef)all[0]);
}

static NSString *source_id(TISInputSourceRef src)
{
	NSString *sid = src ? (__bridge NSString *)TISGetInputSourceProperty(
							  src, kTISPropertyInputSourceID)
						: nil;
	return sid ?: @"none";
}

static bool source_enabled(TISInputSourceRef src)
{
	CFBooleanRef on = TISGetInputSourceProperty(
		src, kTISPropertyInputSourceIsEnabled);
	return on && CFBooleanGetValue(on);
}

static NSString *layout_now(void)
{
	TISInputSourceRef cur = TISCopyCurrentKeyboardInputSource();
	NSString *sid = [source_id(cur) copy];
	if (cur)
		CFRelease(cur);
	return [@"ok " stringByAppendingString:sid];
}

/* Enabled, so that it can be selected; "ok <id> <1 if it was already>". */
static NSString *layout_load(NSString *sid)
{
	TISInputSourceRef src = source_by_id(sid);
	if (!src)
		return [@"err no input source " stringByAppendingString:sid];
	const bool had = source_enabled(src);
	const OSStatus st = had ? noErr : TISEnableInputSource(src);
	CFRelease(src);
	if (st != noErr)
		return [NSString stringWithFormat:@"err %@ not enabled: %d", sid,
				(int)st];
	return [NSString stringWithFormat:@"ok %@ %d", sid, had];
}

static NSString *layout_select(NSString *sid)
{
	TISInputSourceRef src = source_by_id(sid);
	if (!src)
		return [@"err no input source " stringByAppendingString:sid];
	const OSStatus st = TISSelectInputSource(src);
	CFRelease(src);
	return st == noErr ? @"ok"
					   : [NSString stringWithFormat:@"err %@ not selected: "
						  "%d", sid, (int)st];
}

static NSString *layout_drop(NSString *sid)
{
	TISInputSourceRef src = source_by_id(sid);
	if (!src)
		return @"ok";
	TISDisableInputSource(src);
	CFRelease(src);
	return @"ok";
}

/* ---- what is over the window --------------------------------------------- *
 *
 * A picture of the screen is a picture of the window only while nothing is
 * in front of it, and macOS puts a dialog of its own over everything when it
 * wants a grant confirmed again -- Screen Recording's, from time to time --
 * which a round would otherwise read as the frame. So: the owners of the
 * windows on screen in front of the one found, with their layer, that cross
 * its rectangle and can be seen, less the program's own and the system's
 * chrome, which covers no client. */

static const char *const CHROME[] = {
	"Window Server",            /* the menu bar's backdrop, the cursor */
	"SystemUIServer", "Control Center", "Spotlight",
	"Dock",                     /* its strip, over the screen's bottom */
};

static NSString *over_window(void)
{
	NSMutableString *out = [NSMutableString stringWithString:@"ok"];
	CGRect r;
	/* A window off the screen -- minimized, or on another Space -- has
	 * nothing in front of it that a picture of it would show. */
	if (!g_wid || !window_rect(&r) ||
		![window_info()[(id)kCGWindowIsOnscreen] boolValue])
		return out;
	NSArray *above = CFBridgingRelease(CGWindowListCopyWindowInfo(
		kCGWindowListOptionOnScreenAboveWindow, g_wid));
	for (NSDictionary *w in above) {
		CGRect b = CGRectZero;
		CGRectMakeWithDictionaryRepresentation(
			(__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &b);
		if (!CGRectIntersectsRect(b, r) ||
			[w[(id)kCGWindowAlpha] doubleValue] <= 0.0 ||
			[w[(id)kCGWindowOwnerPID] intValue] == g_pid)
			continue;
		NSString *owner = w[(id)kCGWindowOwnerName] ?: @"?";
		bool chrome = false;
		for (size_t i = 0; i < sizeof CHROME / sizeof CHROME[0]; i++)
			chrome = chrome || [owner isEqualToString:@(CHROME[i])];
		if (chrome)
			continue;
		[out appendFormat:@" %@:%d", [owner
			stringByReplacingOccurrencesOfString:@" " withString:@"_"],
		 [w[(id)kCGWindowLayer] intValue]];
	}
	return out;
}

/* ---- the line ------------------------------------------------------------ */

static int run_quiet(NSString *path, NSArray<NSString *> *args)
{
	NSTask *t = [[NSTask alloc] init];
	t.executableURL = [NSURL fileURLWithPath:path];
	t.arguments = args;
	t.standardOutput = NSFileHandle.fileHandleWithNullDevice;
	t.standardError = NSFileHandle.fileHandleWithNullDevice;
	if (![t launchAndReturnError:nil])
		return -1;
	[t waitUntilExit];
	return t.terminationStatus;
}

static char g_note[1024];

static NSString *answer(NSArray<NSString *> *a)
{
	NSString *v = a[0];
	const char *w1 = a.count > 1 ? a[1].UTF8String : "";
	if ([v isEqualToString:@"hold"])
		return hold_input();
	if ([v isEqualToString:@"held"])
		return [NSString stringWithFormat:@"ok %d", g_held];
	if ([v isEqualToString:@"release"])
		return release_input();
	if ([v isEqualToString:@"hello"])
		return [NSString stringWithFormat:@"ok dpi %d",
				(int)lround(96.0 * NSScreen.mainScreen.backingScaleFactor)];
	/* With "prompt", asked as macOS wants it asked: the entry is put in
	 * System Settings' Accessibility list and a dialog says where, on the
	 * Mac, for a hand there to switch it on. */
	if ([v isEqualToString:@"trusted"]) {
		if (a.count > 1 && [a[1] isEqualToString:@"prompt"])
			return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)
					   @{ (__bridge id)kAXTrustedCheckOptionPrompt : @YES })
					   ? @"ok 1" : @"ok 0";
		return AXIsProcessTrusted() ? @"ok 1" : @"ok 0";
	}
	/* Whether Screen Recording is in effect: without it a window's title
	 * is withheld and a picture is the wallpaper, and neither says so. */
	if ([v isEqualToString:@"screen"])
		return CGPreflightScreenCaptureAccess() ? @"ok 1" : @"ok 0";
	/* Whether the board is an ISO one, which the library asks the same way
	 * (iso_board() in cocoa.m) and answers by swapping two key codes: so
	 * the harness swaps them too, to type the key it means. */
	if ([v isEqualToString:@"iso"]) {
		typedef uint32_t (*LayoutTypeFn)(int16_t);
		LayoutTypeFn layout =
			(LayoutTypeFn)dlsym(RTLD_DEFAULT, "KBGetLayoutType");
		CGEventSourceRef src = CGEventSourceCreate(
			kCGEventSourceStateCombinedSessionState);
		bool iso = false;
		if (layout && src)
			iso = layout((int16_t)CGEventSourceGetKeyboardType(src)) ==
				  0x49534f20;                       /* 'ISO ' */
		if (src)
			CFRelease(src);
		return iso ? @"ok 1" : @"ok 0";
	}
	if ([v isEqualToString:@"find"] && a.count > 2)
		return find(w1, a[2].UTF8String)
				   ? [NSString stringWithFormat:@"ok %d", g_pid] : @"ok 0";
	if ([v isEqualToString:@"kill"]) {
		int n = 0;
		for (NSUInteger i = 1; i < a.count; i++)
			if (run_quiet(@"/usr/bin/pkill", @[ @"-x", a[i] ]) == 0)
				n++;
		return [NSString stringWithFormat:@"ok %d", n];
	}
	if ([v isEqualToString:@"ptr"]) {
		const CGPoint p = pointer();
		return [NSString stringWithFormat:@"ok %d %d", (int)lround(p.x),
				(int)lround(p.y)];
	}
	if ([v isEqualToString:@"mon"]) {
		CGRect r;
		const CGPoint c = window_rect(&r)
			? CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r))
			: CGPointMake(1, 1);
		NSScreen *s = screen_at(c);
		return [NSString stringWithFormat:@"ok %@ %@ %@", screen_name(s),
				rect_words(flip(s.frame)), rect_words(flip(s.visibleFrame))];
	}
	if ([v isEqualToString:@"mons"]) {
		NSMutableString *out = [NSMutableString stringWithString:@"ok"];
		NSArray *all = NSScreen.screens;
		for (NSUInteger i = 0; i < all.count; i++)
			[out appendFormat:@" %@ %d %@", screen_name(all[i]), i == 0,
			 rect_words(flip([all[i] frame]))];
		return out;
	}
	if ([v isEqualToString:@"desk"]) {
		CGRect u = CGRectNull;
		for (NSScreen *s in NSScreen.screens)
			u = CGRectUnion(u, flip(s.frame));
		return [@"ok " stringByAppendingString:rect_words(u)];
	}
	if ([v isEqualToString:@"alive"])
		return g_pid && (kill(g_pid, 0) == 0 || errno == EPERM) ? @"ok 1"
																: @"ok 0";
	if ([v isEqualToString:@"clipget"])
		return [@"ok " stringByAppendingString:to_b64(clip_get())];
	if ([v isEqualToString:@"clipput"] && a.count > 1) {
		clip_put(b64(w1));
		return @"ok";
	}
	if ([v isEqualToString:@"warp"] && a.count > 2) {
		warp(a[1].doubleValue, a[2].doubleValue);
		return @"ok";
	}
	if ([v isEqualToString:@"nudge"] && a.count > 2) {
		nudge(a[1].intValue, a[2].intValue);
		return @"ok";
	}
	if ([v isEqualToString:@"button"] && a.count > 2) {
		button(a[1].intValue, a[2].intValue != 0);
		return @"ok";
	}
	if ([v isEqualToString:@"wheel"] && a.count > 1) {
		wheel(a[1].intValue);
		return @"ok";
	}
	if ([v isEqualToString:@"key"] && a.count > 2) {
		key((CGKeyCode)a[1].intValue, a[2].intValue != 0);
		return @"ok";
	}
	if ([v isEqualToString:@"idle"] && a.count > 1) {
		snprintf(g_note, sizeof g_note, "%s", b64(w1).UTF8String);
		return [NSString stringWithFormat:@"ok %lld", idle_ms(g_note)];
	}
	/* The window list front to back, owner, layer and number a window --
	 * for a person reading why `fg` said what it said. */
	if ([v isEqualToString:@"wins"]) {
		NSArray *on = CFBridgingRelease(CGWindowListCopyWindowInfo(
			kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
		NSMutableString *out = [NSMutableString stringWithString:@"ok"];
		NSUInteger n = 0;
		for (NSDictionary *w in on) {
			if (n++ == 12)
				break;
			NSString *owner = [w[(id)kCGWindowOwnerName]
				stringByReplacingOccurrencesOfString:@" " withString:@"_"];
			CGRect r = CGRectZero;
			CGRectMakeWithDictionaryRepresentation(
				(__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r);
			[out appendFormat:@" %@:%d:%u:%dx%d", owner ?: @"?",
			 [w[(id)kCGWindowLayer] intValue],
			 [w[(id)kCGWindowNumber] unsignedIntValue],
			 (int)r.size.width, (int)r.size.height];
		}
		return out;
	}
	if ([v isEqualToString:@"shot"] && a.count > 1)
		return shot_srgb(b64(w1)) ? @"ok" : @"err no picture";
	if ([v isEqualToString:@"over"])
		return over_window();
	if ([v isEqualToString:@"layout"])
		return layout_now();
	if ([v isEqualToString:@"layoutload"] && a.count > 1)
		return layout_load(a[1]);
	if ([v isEqualToString:@"layoutask"] && a.count > 1)
		return layout_select(a[1]);
	if ([v isEqualToString:@"layoutdrop"] && a.count > 1)
		return layout_drop(a[1]);

	/* Everything below is about the window found. */
	if (!g_wid)
		return @"err no window";
	if ([v isEqualToString:@"rect"]) {
		CGRect r;
		return window_rect(&r) ? [@"ok " stringByAppendingString:rect_words(r)]
							   : @"err no window";
	}
	if ([v isEqualToString:@"fg"])
		return foreground() ? @"ok 1" : @"ok 0";
	if ([v isEqualToString:@"focus"])
		return focus() ? @"ok 1" : @"ok 0";
	if ([v isEqualToString:@"iconic"])
		return minimized() ? @"ok 1" : @"ok 0";
	if ([v isEqualToString:@"moveby"] && a.count > 2)
		return move_by(a[1].intValue, a[2].intValue) ? @"ok"
													 : @"err not moved";
	if ([v isEqualToString:@"module"] && a.count > 1)
		return has_module(a[1].UTF8String) ? @"ok 1" : @"ok 0";
	if ([v isEqualToString:@"close"]) {
		/* Asked to quit as the menu's Quit asks, by an Apple event, which the
		 * library takes as a close request: the program leaves by its own
		 * way out, saying what it had to say. */
		NSRunningApplication *app = [NSRunningApplication
			runningApplicationWithProcessIdentifier:g_pid];
		return [app terminate] ? @"ok 1" : @"ok 0";
	}
	if ([v isEqualToString:@"cpu"])
		return [NSString stringWithFormat:@"ok %.3f", cpu_seconds()];
	return [@"err what is " stringByAppendingString:v];
}

/* What a harness that went without saying so left down, let up: a button,
 * a modifier or the repeating key held past the agent would stay down on
 * the Mac's desktop for whoever comes next. */
static void let_go(void)
{
	for (int i = 0; i < 3; i++)
		if (g_buttons & (1 << i))
			button(i + 1, false);
	for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++)
		if (g_flags & MODS[i].side)
			key(MODS[i].code, false);
	pthread_mutex_lock(&g_rep_lock);
	const bool on = g_rep_on;
	const CGKeyCode code = g_rep_code;
	pthread_mutex_unlock(&g_rep_lock);
	if (on)
		key(code, false);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* An AppKit connection, without a Dock icon or a menu: NSScreen and the
	 * pasteboard want the one, and a round's picture wants neither of the
	 * others. */
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
	}
	pthread_t rep;
	pthread_create(&rep, NULL, repeater, NULL);
	pthread_detach(rep);
	/* Any length: a clipboard is a line of base64 of whatever was put on
	 * it, and a round puts a megabyte there. */
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, stdin) > 0) {
		@autoreleasepool {
			line[strcspn(line, "\r\n")] = '\0';
			NSArray *words = [[NSString stringWithUTF8String:line]
				componentsSeparatedByString:@" "];
			if (!words.count || [words[0] isEqualToString:@""])
				continue;
			if ([words[0] isEqualToString:@"quit"])
				break;
			printf("%s\n", answer(words).UTF8String);
		}
	}
	free(line);
	let_go();
	leave_note(g_note);
	return 0;
}
