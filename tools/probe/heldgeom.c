/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* weston applying a content update held for a wp_fifo_v1 barrier with
 * xdg_surface state sent after it, with nothing of fenestal in it:
 * docs/weston-held-update-xdg-state.md's reproducer. A wl_shm client, so
 * no GPU and no Vulkan WSI: it maximizes, then answers the restore's
 * configure the way the WSI and a toolkit between them did --
 *
 *   1  attach, damage, wp_fifo_v1.set_barrier, commit   the last frame
 *   2  wp_fifo_v1.wait_barrier, commit                  held for the barrier
 *   3  xdg_surface.set_window_geometry(restored), flush
 *      (here a pause of HELDGEOM_GAP_MS, 100 by default, which the latch
 *      falls into)
 *   4  xdg_surface.ack_configure(restore), attach, commit
 *
 * -- and reports whether the server disconnected it. Commit 2 was sent
 * before the geometry, so by xdg-shell it carries none, and the commit
 * that does (4) carries the ack too. weston 15 applies commit 2 at the
 * next output latch with the geometry pending by then and the maximized
 * state still acked, and posts xdg_wm_base.error invalid_surface_state.
 *
 * weston 15.0.1 (Arch 15.0.1-3), DRM backend, 60 Hz, the output idle
 * before the restore, ten runs each: on the thinkpad (Ironlake) every run
 * with a gap of 60 ms or more and none at 50 ms or less -- the held commit
 * is applied between the two, and when the ack is in first, both go in
 * with it and the check passes; on schenker (Kaby Lake) every run at
 * 100 ms and 3 at 20 ms. `nowait` none on either.
 *
 *   heldgeom            the sequence above
 *   heldgeom nowait     commit 2 without wait_barrier: nothing is held,
 *                       the same requests otherwise -- the control
 *
 * Exit status 0 is no error, 1 the invalid_surface_state error (the bug),
 * 2 anything else. Run on a weston with WAYLAND_DISPLAY set; the window
 * needs an output that repaints, so not on a headless backend without one.
 *
 *   P=/usr/share/wayland-protocols G=build/heldgeom-proto
 *   mkdir -p $G
 *   for x in stable/xdg-shell/xdg-shell staging/fifo/fifo-v1; do
 *     n=$(basename $x)
 *     wayland-scanner client-header $P/$x.xml $G/$n-client-protocol.h
 *     wayland-scanner private-code $P/$x.xml $G/$n-protocol.c
 *   done
 *   gcc -std=c11 -O2 -Wall -Wextra -I$G -o build/heldgeom \
 *       tools/probe/heldgeom.c $G/xdg-shell-protocol.c \
 *       $G/fifo-v1-protocol.c -lwayland-client */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "fifo-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FLOAT_W 640
#define FLOAT_H 480

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_fifo_manager_v1 *fifo_manager;

/* The toplevel's last configure, and the xdg_surface serial that ends it. */
static int32_t cfg_w, cfg_h;
static bool cfg_maximized;
static uint32_t cfg_serial;
static bool cfg_done;

static void
die(const char *what)
{
	fprintf(stderr, "heldgeom: %s\n", what);
	exit(2);
}

static struct wl_buffer *
buffer(int w, int h, uint32_t argb)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("heldgeom", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		die("memfd");
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		die("mmap");
	for (int i = 0; i < w * h; i++)
		px[i] = argb;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
													WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
				const char *iface, uint32_t version)
{
	(void)data;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, wp_fifo_manager_v1_interface.name))
		fifo_manager = wl_registry_bind(reg, name,
										&wp_fifo_manager_v1_interface, 1);
}

static void
registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global, registry_remove,
};

static void
wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void
surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	(void)xs;
	cfg_serial = serial;
	cfg_done = true;
}

static const struct xdg_surface_listener surface_listener = {
	surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
				   struct wl_array *states)
{
	(void)data;
	(void)top;
	cfg_w = w;
	cfg_h = h;
	cfg_maximized = false;
	uint32_t *s;
	wl_array_for_each(s, states) if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED)
		cfg_maximized = true;
}

static void
toplevel_close(void *data, struct xdg_toplevel *top)
{
	(void)data;
	(void)top;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

/* Dispatches until a configure whose maximized state is `maximized`. */
static void
await_configure(struct wl_display *dpy, bool maximized)
{
	cfg_done = false;
	while (!(cfg_done && cfg_maximized == maximized))
		if (wl_display_dispatch(dpy) < 0)
			return;
}

/* Waits for the surface's next frame callback, which says it was shown. */
static void
frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	(void)time;
	*(bool *)data = true;
	wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener = { frame_done };

static void
present(struct wl_display *dpy, struct wl_surface *surf, struct wl_buffer *b,
		int w, int h)
{
	bool shown = false;
	wl_surface_attach(surf, b, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, w, h);
	wl_callback_add_listener(wl_surface_frame(surf), &frame_listener, &shown);
	wl_surface_commit(surf);
	while (!shown)
		if (wl_display_dispatch(dpy) < 0)
			return;
}

/* 0 when the connection is fine, 1 for invalid_surface_state, 2 else. */
static int
verdict(struct wl_display *dpy)
{
	int err = wl_display_get_error(dpy);
	if (!err)
		return 0;
	const struct wl_interface *iface;
	uint32_t id, code = wl_display_get_protocol_error(dpy, &iface, &id);
	if (err == EPROTO && iface == &xdg_wm_base_interface
		&& code == XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE) {
		printf("disconnected: xdg_wm_base.error invalid_surface_state\n");
		return 1;
	}
	printf("disconnected: %s error %u (%s)\n",
		   iface ? iface->name : "?", code, strerror(err));
	return 2;
}

int
main(int argc, char **argv)
{
	bool wait = !(argc > 1 && !strcmp(argv[1], "nowait"));
	const char *gap_env = getenv("HELDGEOM_GAP_MS");
	long gap_ms = gap_env ? atol(gap_env) : 100;

	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		die("no Wayland display (WAYLAND_DISPLAY)");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base)
		die("no wl_compositor, wl_shm or xdg_wm_base");
	if (!fifo_manager)
		die("no wp_fifo_manager_v1");
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);

	struct wl_surface *surf = wl_compositor_create_surface(compositor);
	struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(fifo_manager, surf);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
	xdg_surface_add_listener(xs, &surface_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
	xdg_toplevel_set_title(top, "heldgeom");
	wl_surface_commit(surf);

	/* Floating, shown. */
	await_configure(dpy, false);
	xdg_surface_ack_configure(xs, cfg_serial);
	xdg_surface_set_window_geometry(xs, 0, 0, FLOAT_W, FLOAT_H);
	present(dpy, surf, buffer(FLOAT_W, FLOAT_H, 0xff3060c0), FLOAT_W, FLOAT_H);

	/* Maximized, shown at the configured size. */
	xdg_toplevel_set_maximized(top);
	await_configure(dpy, true);
	if (wl_display_get_error(dpy))
		return verdict(dpy);
	int max_w = cfg_w, max_h = cfg_h;
	if (max_w <= 0 || max_h <= 0)
		die("the maximized configure has no size");
	xdg_surface_ack_configure(xs, cfg_serial);
	xdg_surface_set_window_geometry(xs, 0, 0, max_w, max_h);
	present(dpy, surf, buffer(max_w, max_h, 0xffc06030), max_w, max_h);
	for (int i = 0; i < 3; i++)
		present(dpy, surf, buffer(max_w, max_h, 0xffc06030 + i), max_w, max_h);
	if (wl_display_get_error(dpy))
		return verdict(dpy);
	printf("maximized at %dx%d; restoring, commit 2 %s\n", max_w, max_h,
		   wait ? "behind wait_barrier" : "without wait_barrier");

	/* The restore. */
	xdg_toplevel_unset_maximized(top);
	await_configure(dpy, false);
	if (wl_display_get_error(dpy))
		return verdict(dpy);
	uint32_t restore = cfg_serial;
	int w = cfg_w > 0 ? cfg_w : FLOAT_W, h = cfg_h > 0 ? cfg_h : FLOAT_H;

	/* 1: the last frame at the maximized size, setting the barrier. */
	wl_surface_attach(surf, buffer(max_w, max_h, 0xff30c060), 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, max_w, max_h);
	wp_fifo_v1_set_barrier(fifo);
	wl_surface_commit(surf);
	/* 2: empty, held until the barrier clears at the next latch. */
	if (wait)
		wp_fifo_v1_wait_barrier(fifo);
	wl_surface_commit(surf);
	/* 3: the restored geometry, for the commit after the ack. */
	xdg_surface_set_window_geometry(xs, 0, 0, w, h);
	wl_display_flush(dpy);
	struct timespec gap = { gap_ms / 1000, gap_ms % 1000 * 1000000L };
	nanosleep(&gap, NULL);
	/* 4: the ack and the restored frame. */
	xdg_surface_ack_configure(xs, restore);
	wl_surface_attach(surf, buffer(w, h, 0xff3060c0), 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, w, h);
	/* A roundtrip does not wait for commits queued behind the held one;
	 * this callback fires only once commit 4 is applied, so after the
	 * check on commit 2. */
	bool shown = false;
	wl_callback_add_listener(wl_surface_frame(surf), &frame_listener, &shown);
	wl_surface_commit(surf);
	while (!shown)
		if (wl_display_dispatch(dpy) < 0)
			break;

	int v = verdict(dpy);
	if (!v)
		printf("no error: restored at %dx%d\n", w, h);
	wl_display_disconnect(dpy);
	return v;
}
