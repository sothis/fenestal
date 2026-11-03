/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Cocoa-to-Metal bridge: the one file of this renderer that sees the
 * window, and the sibling of src/renderer/vulkan/surface_cocoa.c. There is no
 * surface object to make: the window's view is backed by a CAMetalLayer the
 * backend made with it, and the layer is what the frame presents into.
 */

#include "core/app.h"

#include "renderer/metal/metal_internal.h"

#include "backend/cocoa/cocoa_state.h"

CAMetalLayer *mt_bridge_layer(App *a)
{
	return (__bridge CAMetalLayer *)a->backend_state->layer;
}

/* Covered: AppKit's occlusion state, which the backend keeps where the
 * render thread may read it. A layer of a covered window still hands out
 * drawables, which the window server never shows -- or, once the three are
 * spent, hands out nothing for a second at a time -- so a present there is
 * one to skip, which is what FnlRendererVtbl::occluded is for. */
bool mt_bridge_occluded(const App *a)
{
	return fnl_atomic_load_bool(&a->backend_state->occluded);
}
