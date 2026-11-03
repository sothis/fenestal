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

struct BackendState {
	/* NSWindow, the view that is its content, and the CAMetalLayer that
	 * backs the view: owned, each one reference. The layer is the one
	 * thing the bridges take; it is made with the view and outlives every
	 * device, so a device rebuilt after a loss presents into the same one. */
	void *window;
	void *view;
	void *layer;
	/* The window's delegate and the application's, owned: AppKit holds
	 * both weakly. The delegate is also what the menu's Quit asks. */
	void *window_delegate;
	void *app_delegate;
	/* The local monitor that hands a key release with Command held to the
	 * window: AppKit swallows those, and a key the caller saw go down would
	 * never come up. */
	void *key_up_monitor;

	/* The wakeup: a pipe whose read end is a run-loop source, so a write --
	 * the one thing a signal handler may do -- wakes the pump in every
	 * run-loop mode, the tracking loops of a drag and a resize included.
	 * The source is owned (CFFileDescriptorRef, CFRunLoopSourceRef). */
	int   wake_fd[2];
	void *wake_cf;
	void *wake_source;

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
	/* Inside fullscreen's animation, from the will to the did or a failure,
	 * where AppKit ignores a toggle: one asked there goes to fs_owed. Its
	 * resize is published like any other. Main thread. */
	bool fs_transition;
	/* A fullscreen asked for inside that animation, which AppKit ignores
	 * there: kept, the last one asked, and asked of AppKit as it ends
	 * (fs_settle()). 0 none, else 1 on or 2 off. Main thread. */
	int  fs_owed;
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
