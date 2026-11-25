/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the Cocoa backend that no other file may see, and what
 * the three bridges -- src/renderer/metal/surface_cocoa.m,
 * src/renderer/vulkan/surface_cocoa.c and src/renderer/gl/cgl.m -- read of
 * it: the layer the frame is presented into, and whether the window is
 * covered.
 *
 * C as well as Objective-C, because the Vulkan bridge is a .c file: every
 * AppKit object is held here as a `void *` owning one reference
 * (CFBridgingRetain), and only cocoa.m turns them back into objects. The
 * layout is then the same in both languages, which it must be.
 */

#ifndef FNL_COCOA_STATE_H
#define FNL_COCOA_STATE_H

/* For FnlAtomicBool, bool and uint32_t. Every includer includes it first
 * anyway; naming it here keeps the file readable alone, and the atomics are
 * core's. */
#include "core/app.h"

/* What the host owns, beside the windows: the wakeup pipe and its run-loop
 * source, and the application delegate. One NSApplication is a process's
 * already (app_launch()), and one wakeup pipe is the host's, so that a wake
 * drains every window's requests. The window's own state -- its NSWindow,
 * view, layer and delegates -- is in BackendState. See DESIGN *A second
 * window*. */
struct BackendHostState {
	/* The wakeup: a pipe whose read end is a run-loop source, so a write --
	 * the one thing a signal handler may do -- wakes the pump in every
	 * run-loop mode, the tracking loops of a drag and a resize included.
	 * The source is owned (CFFileDescriptorRef, CFRunLoopSourceRef). */
	int wake_fd[2];
	void *wake_cf;
	void *wake_source;
	/* The application delegate, owned; AppKit holds it weakly. One per host,
	 * asking the frontmost window -- not one per window, which would have
	 * the second steal Command-Q and the Dock's Quit from the first. */
	void *app_delegate;
};

struct BackendState {
	/* NSWindow, the view that is its content, and the CAMetalLayer that
	 * backs the view: owned, each one reference. The layer is the one
	 * thing the bridges take; it is made with the view and outlives every
	 * device, so a device rebuilt after a loss presents into the same one. */
	void *window;
	void *view;
	void *layer;
	/* The window's delegate, owned: AppKit holds it weakly. It is also what
	 * the menu's Quit asks, through the host's frontmost window. */
	void *window_delegate;
	/* The local monitor that hands a key release with Command held to the
	 * window: AppKit swallows those, and a key the caller saw go down would
	 * never come up. */
	void *key_up_monitor;

	/* The left button's press, which AppKit's window drag and the resize
	 * below both keep the release of: a move asked for after the press was
	 * dispatched (fnl_begin_move() from a callback) needs the event. [main]
	 * Per window -- a press belongs to the window it was in -- and so here
	 * rather than at file scope, where a second window's press would
	 * overwrite the first's mid-drag. */
	bool left_held;
	/* NSEvent *, owned (one reference, as every object here is): AppKit's
	 * drag is handed it after the event may have died. [main] */
	void *left_down;

	/* What the render thread asks without touching AppKit, which it may
	 * not: whether the window is anywhere a frame could go, and whether it
	 * is covered (NSWindowOcclusionStateVisible clear). Both written on the
	 * main thread from AppKit's notifications. */
	FnlAtomicBool on_screen;
	FnlAtomicBool occluded;

	/* The scale the window is on, as the backing store says: 1 or 2. */
	double backing;

	/* Whether this backend hid the cursor and detached it from the mouse:
	 * NSCursor's hide is a counter, kept balanced through this. */
	bool cursor_hidden;
	/* The shape last handed to AppKit. An -[NSCursor set] is a cursor update
	 * the window server answers, and a pointer crossing the window makes a
	 * motion event after another, so only a change is sent. Zero is
	 * DECO_CURSOR_HIDDEN, which cursor_apply() never stores, so nothing set
	 * reads as none; cursorUpdate: puts it back, for a cursor the desktop
	 * changed behind us. */
	int cursor_kind;
	int cursor_shape;
	int cursor_edge;
	/* Inside fullscreen's animation, from the will to the did or a failure,
	 * where AppKit ignores a toggle: one asked there goes to fs_owed. Its
	 * resize is published like any other. Main thread. */
	bool fs_transition;
	/* A fullscreen asked for inside that animation, which AppKit ignores
	 * there: kept, the last one asked, and asked of AppKit as it ends
	 * (fs_settle()). 0 none, else 1 on or 2 off. Main thread. */
	int fs_owed;
	/* And a maximize asked for there, where AppKit ignores a zoom as it
	 * ignores a toggle: kept and asked as the animation ends, once the
	 * window is out of fullscreen. 0 none, else 1 on or 2 off. Main
	 * thread. */
	int fs_max_owed;
	/* Inside the library's own resize loop (cocoa_begin_resize()), which
	 * reads the pointer itself. */
	bool resizing;

	/* The screen the window was last seen on, by its display id, so that
	 * publishing the output costs a comparison while it has not changed. */
	uint32_t display;

	/* AppKit's marked text, which a dead key starts, as its length in
	 * UTF-16 units -- NSRange's -- for markedRange; 0 is no composition. */
	size_t marked_len;
};

#endif /* FNL_COCOA_STATE_H */
