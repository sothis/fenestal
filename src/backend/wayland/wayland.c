/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Wayland backend: registry, xdg-shell surface, outputs and scale,
 * keyboard, pointer, input method, clipboard. Main (pump) thread throughout,
 * except the seam entries the render thread calls: set_geometry,
 * ack_configure, the three frame_* ones, apply_scale, flush and
 * present_window_renew.
 */

/* For pipe2() and memfd_create(), which glibc declares only under it. A
 * pipe made and then marked close-on-exec is inherited by a child forked in
 * between, and a paste whose write end a stranger holds never sees end of
 * file. This file is Linux's already (epoll, timerfd), so the wider feature
 * set costs it nothing; it has to be asked for ahead of app.h, whose own
 * pin is the narrower one every other file builds under. */
#define _GNU_SOURCE

#include "core/app.h"

#include "backend/wayland/wayland_state.h"
#include "backend/evdev_keys.h"
#include "backend/loop_linux.h"

/* For the pointer's buttons. The keys are evdev_keys.h's. */
#include <linux/input-event-codes.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <poll.h>

/* mmap and fstat for the keymap, timerfd for key repeat. */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

/* The clipboard's: a write to a pipe whose reader may have gone. */
#include <signal.h>
#include <pthread.h>

static void pointer_update(App *a);

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static WlOutput *output_info_for(App *a, struct wl_output *o)
{
	struct BackendState *p = a->backend_state;
	for (int i = 0; i < p->output_count; i++)
		if (p->outputs[i].handle == o)
			return &p->outputs[i];
	return NULL;
}

/* [render] The frame's opaque part, on the surface the frames go into: the
 * window's own pixel behind a subsurface is transparent. */
static void present_opaque(App *a, int w, int h, int m)
{
	struct BackendState *p = a->backend_state;
	struct wl_region *opaque = wl_compositor_create_region(p->compositor);
	wl_region_add(opaque, m, m, w, h);
	wl_surface_set_opaque_region(p->present, opaque);
	wl_region_destroy(opaque);
}

/* [render] Window geometry is what the compositor positions and tiles by, so
 * the transparent grab ring is left out of it and of the opaque region. */
static void surface_set_geometry(App *a, int w, int h, int m)
{
	struct BackendState *p = a->backend_state;
	xdg_surface_set_window_geometry(p->xdg_surface, m, m, w, h);
	present_opaque(a, w, h, m);
	if (p->present != p->surface)
		p->parent_dirty = true;
}

/* The room a window has, in points: xdg_toplevel.configure_bounds where the
 * compositor gave one, else the output's mode, which overstates it by whatever
 * the shell reserves. */
static void refresh_size_bounds(App *a)
{
	struct BackendState *p = a->backend_state;
	const WlOutput *cur = p->current_output
						? output_info_for(a, p->current_output) : NULL;
	/* The followed output's rate, which the GPU ranking keys a display by
	 * (rank.c). Nothing is spaced by it where frames are invited; a loop
	 * at rest waits a period of it for a signal instead of a 60 Hz one. */
	app_publish_refresh(a, cur ? cur->refresh_mhz : 0);
	if (p->bounds_w > 0 && p->bounds_h > 0) {
		a->out_w_pt = p->bounds_w;
		a->out_h_pt = p->bounds_h;
	} else {
		if (cur && cur->mode_w > 0) {
			int os = p->preferred_scale120 > 0 ? p->preferred_scale120
				   : cur->scale > 0            ? cur->scale * 120 : 120;
			a->out_w_pt = cur->mode_w * 120 / os;
			a->out_h_pt = cur->mode_h * 120 / os;
		}
	}
	/* Here and not from a pointer event: none of what moves these is one. */
	app_publish_output(a, NULL, a->out_w_pt, a->out_h_pt);
}

/* Follow the highest-scale output the surface is on, so a window straddling
 * two stays sharp on either. On none at all -- minimized, another workspace
 * -- the last choice stands, which saves two swapchain rebuilds on the way
 * back. */
static void pick_current_output(App *a)
{
	struct BackendState *p = a->backend_state;
	struct wl_output *best = NULL;
	int best_scale = 0;

	for (int i = 0; i < p->output_count; i++) {
		if (!p->outputs[i].on_surface)
			continue;
		if (!best || p->outputs[i].scale > best_scale) {
			best = p->outputs[i].handle;
			best_scale = p->outputs[i].scale;
		}
	}
	/* Bounds describe the output they arrived for and are not resent when the
	 * window moves to another, so a *change* drops them and the mode stands
	 * in. Not the first enter: those are the bounds for the monitor it opened
	 * on. */
	if (best && p->current_output && best != p->current_output)
		p->bounds_w = p->bounds_h = 0;
	if (best)
		p->current_output = best;

	refresh_size_bounds(a);

	/* The followed output first, then the others the surface is on. An empty
	 * set leaves the last name standing, and its mode with it: the two are
	 * published together, since the GPU ranking names a display by the
	 * followed output's name and its own mode, never by the bounds above,
	 * which the compositor sends for wherever it reckons the window is --
	 * another output's, while the window straddles two (rank.c). */
	char names[sizeof a->want_output] = "";
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < p->output_count; i++) {
			const WlOutput *oi = &p->outputs[i];
			bool followed = oi->handle == p->current_output;
			if (!oi->on_surface || followed != (pass == 0))
				continue;
			size_t n = strlen(names);
			snprintf(names + n, sizeof names - n, "%s%s%s%s%s",
					 n ? " + " : "", oi->name,
					 oi->model[0] ? " (" : "", oi->model,
					 oi->model[0] ? ")" : "");
		}
	}
	if (names[0]) {
		const WlOutput *cur = p->current_output
							? output_info_for(a, p->current_output) : NULL;
		app_publish_output_mode(a, names, cur ? cur->mode_w : 0,
								cur ? cur->mode_h : 0);
	}

	/* Every output by its connector, the followed one current: what the
	 * titlebar's output box names, and follows as the window is dragged
	 * from one to another. An output that has not said its name (wl_output
	 * before v4) is left out, since nothing could ask for it. */
	char list[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	const int n = p->output_count < OUTPUTS_MAX ? p->output_count
												: OUTPUTS_MAX;
	int at = -1;
	for (int i = 0; i < n; i++) {
		snprintf(list[i], sizeof list[i], "%s", p->outputs[i].name);
		if (p->outputs[i].handle == p->current_output)
			at = i;
	}
	app_publish_outputs(a, list, n, at);
}

/* Two scales, kept apart so that an override changes the buffer and never the
 * window (DESIGN *Scale*):
 *
 *   compositor_scale120()   the density this desktop has
 *   window_scale120()       what the window's point geometry is sized against
 *   resolve_scale120()      what is rendered at; the override wins here only
 *   clamp_buffer_scale120() what the surface can express
 *   clamp_device_scale120() what the device will allocate */

/* Without a viewport the buffer scale is an integer, so fractions round up; a
 * pre-v3 wl_compositor has no set_buffer_scale at all and gets 1.0. */
static int clamp_buffer_scale120(const App *a, int s120)
{
	if (!a->backend_state->viewport) {
		if (!wl_can_set_buffer_scale(a->backend_state))
			s120 = 120;
		else if (s120 % 120)
			s120 = (s120 / 120 + 1) * 120;
	}
	return s120;
}

/* Unclamped: both callers clamp what they build from it, and its two inputs
 * are bounded where they arrive. */
static int compositor_scale120(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->fractional && p->preferred_scale120 > 0)
		return p->preferred_scale120;
	if (p->current_output) {
		WlOutput *oi = output_info_for(a, p->current_output);
		if (oi && oi->scale > 0)
			return oi->scale * 120;
	}
	return 120;
}

/* The desktop's own scale, never the override. */
static int window_scale120(App *a)
{
	return clamp_buffer_scale120(a, compositor_scale120(a));
}

/* Cut the *scale* to one whose buffer fits the device's maxImageExtent, not
 * the extent: everything else lays out at the scale, and the two would then
 * describe different windows. */
static int clamp_device_scale120(const App *a, int s120)
{
	uint32_t mw = fnl_atomic_load_u32(&a->max_extent_w);
	uint32_t mh = fnl_atomic_load_u32(&a->max_extent_h);
	if (!mw || !mh)
		return s120;             /* no device to ask yet; nothing to bound */

	/* The whole surface, grab ring included -- that is what is allocated. */
	const int m = deco_margin(a, a->is_fullscreen);
	int w = a->cur_w + 2 * m, h = a->cur_h + 2 * m;
	if (w < 1 || h < 1)
		return s120;             /* nothing configured yet */

	/* The largest scale that fits on whichever axis binds first. Floor
	 * division, so scale_pt()'s rounding cannot carry it back over. */
	int sw  = (int)((long long)mw * 120 / w);
	int sh  = (int)((long long)mh * 120 / h);
	int cap = sw < sh ? sw : sh;
	if (cap >= s120)
		return s120;

	/* Without a viewport only whole scales are expressible, rounded down. 1.0
	 * is the floor: past that create_swapchain() clamps the extent. */
	if (!a->backend_state->viewport)
		return cap < 120 ? 120 : (cap / 120) * 120;
	return cap < SCALE120_MIN ? SCALE120_MIN : cap;
}

static int resolve_scale120(App *a)
{
	int s120 = clamp_buffer_scale120(a, a->scale_override120 > 0
											? a->scale_override120
											: compositor_scale120(a));
	return clamp_device_scale120(a, s120);
}

static void update_scale(App *a)
{
	(void)app_publish_scale(a, resolve_scale120(a));
}

static void outputs_changed(App *a)
{
	pick_current_output(a);
	update_scale(a);
}

static void output_geometry(void *data, struct wl_output *o, int32_t x,
							int32_t y, int32_t pw, int32_t ph, int32_t sub,
							const char *make, const char *model,
							int32_t transform)
{
	(void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)transform;
	App *a = data;
	WlOutput *oi = output_info_for(a, o);
	if (oi && (make || model))
		snprintf(oi->model, sizeof oi->model, "%s%s%s", make ? make : "",
				 make && model ? " " : "", model ? model : "");
}

/* A size off the wire, or 0 for one outside OVERLAY_COORD_MAX. Each reaches a
 * multiplication or is published as the room a window has. Dropped, not
 * clamped: zero already means "no figure" to every reader, and what they fall
 * back to beats an invented size. */
static int wire_pt(int32_t v)
{
	return (v > 0 && v <= OVERLAY_COORD_MAX) ? v : 0;
}

/* A mode out of range leaves the output's last good one standing. */
static void output_mode(void *data, struct wl_output *o, uint32_t f, int32_t w,
						int32_t h, int32_t r)
{
	App *a = data;
	WlOutput *oi = output_info_for(a, o);
	const int mode_w = wire_pt(w), mode_h = wire_pt(h);
	if (oi && (f & WL_OUTPUT_MODE_CURRENT) && mode_w && mode_h) {
		oi->mode_w = mode_w;
		oi->mode_h = mode_h;
		oi->refresh_mhz = r > 0 ? (unsigned int)r : 0;
	}
}
/* Re-picked once per property burst: the mode and the name are events of their
 * own, and both feed what is published. */
static void output_done(void *data, struct wl_output *o)
{
	(void)o;
	App *a = data;
	outputs_changed(a);
}
/* Bounded on the way in: every point is multiplied by 120 times it. */
static void output_scale(void *data, struct wl_output *o, int32_t s)
{
	App *a = data;
	WlOutput *oi = output_info_for(a, o);
	if (oi) {
		/* In whole steps, so the bound is applied before the multiply. */
		const int max = SCALE120_MAX / 120;
		oi->scale = s < 1 ? 1 : s > max ? max : s;
	}
	outputs_changed(a);
}
static void output_name(void *data, struct wl_output *o, const char *name)
{
	App *a = data;
	WlOutput *oi = output_info_for(a, o);
	if (oi && name)
		snprintf(oi->name, sizeof oi->name, "%s", name);
}
static void output_description(void *d, struct wl_output *o, const char *desc)
{ (void)d; (void)o; (void)desc; }
static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode, .done = output_done,
	.scale = output_scale, .name = output_name,
	.description = output_description,
};

/* ---- keyboard -------------------------------------------------------------
 * Keys are evdev codes and need no keymap (backend/evdev_keys.h). Text entry
 * does, and the keymap is read as it arrives: the descriptor is only good
 * now. */
static void kb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int fd,
					  uint32_t size)
{
	(void)kb;
	App *a = d;
	struct BackendState *p = a->backend_state;
	bool parsed = false;

	if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
		/* A file shorter than the size it came with maps all the same, and
		 * reading past its end is SIGBUS rather than an error. */
		struct stat st;
		void *map = MAP_FAILED;
		if (fstat(fd, &st) < 0)
			fnl_log(FNL_LOG_WARN, "the compositor's keymap could not be "
					"measured (%s); text entry will type nothing",
					strerror(errno));
		else if (st.st_size < 0 || (uint64_t)st.st_size < size)
			fnl_log(FNL_LOG_WARN, "the compositor's keymap is shorter than "
					"it says; text entry will type nothing");
		else {  /* Private: required since wl_keyboard 7, allowed before. */
			map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
			if (map == MAP_FAILED)
				fnl_log(FNL_LOG_WARN, "the compositor's keymap could not be "
						"mapped (%s); text entry will type nothing",
						strerror(errno));
		}
		if (map != MAP_FAILED) {
			parsed = true;
			if (!keymap_parse_xkb(&p->keymap, map, size))
				fnl_log(FNL_LOG_WARN, "the compositor's keymap could not be "
						"read; text entry will type nothing");
			munmap(map, size);
		}
	}
	/* The parser empties the map itself. A keymap it was never shown replaces
	 * the last one all the same. */
	if (!parsed)
		memset(&p->keymap, 0, sizeof p->keymap);
	close(fd);
}

/* The core protocol's eight modifier bits, which an XKB keymap lists first and
 * in this order. That Mod2 is Num Lock and Mod5 AltGr is xkeyboard-config's
 * convention, not the wire's. */
#define KB_SHIFT   0x01u
#define KB_LOCK    0x02u
#define KB_CONTROL 0x04u
#define KB_MOD1    0x08u            /* Alt */
#define KB_MOD2    0x10u            /* Num Lock */
#define KB_MOD4    0x40u            /* Super */
#define KB_MOD5    0x80u            /* AltGr */

static KeyMods kb_mods(const struct BackendState *p)
{
	const uint32_t m = p->kb_mods;
	return (KeyMods){
		.shift   = (m & KB_SHIFT) != 0,
		.lock    = (m & KB_LOCK) != 0,
		.num     = (m & KB_MOD2) != 0,
		.level3  = (m & KB_MOD5) != 0,
		.command = (m & (KB_CONTROL | KB_MOD1 | KB_MOD4)) != 0,
		.group   = p->kb_group,
	};
}

/* For a wl_seat before version 4, which sends no repeat_info. */
#define REPEAT_RATE_DEFAULT  25     /* per second */
#define REPEAT_DELAY_DEFAULT 600    /* milliseconds */

static void key_repeat_stop(struct BackendState *p)
{
	if (!p->repeat_armed)
		return;
	struct itimerspec its;
	memset(&its, 0, sizeof its);
	timerfd_settime(p->repeat_fd, 0, &its, NULL);
	p->repeat_armed = false;
}

static void key_repeat_start(struct BackendState *p, uint32_t key)
{
	if (p->repeat_fd < 0 || p->repeat_rate <= 0)
		return;

	const long period = 1000000000L / p->repeat_rate;
	struct itimerspec its;
	memset(&its, 0, sizeof its);
	its.it_interval.tv_sec  = period / 1000000000L;
	its.it_interval.tv_nsec = period % 1000000000L;
	/* A zero it_value disarms the timer rather than firing it at once, so a
	 * compositor that asks for no delay gets one period of it. */
	if (p->repeat_delay > 0) {
		its.it_value.tv_sec  = p->repeat_delay / 1000;
		its.it_value.tv_nsec = (long)(p->repeat_delay % 1000) * 1000000L;
	} else {
		its.it_value = its.it_interval;
	}
	if (timerfd_settime(p->repeat_fd, 0, &its, NULL) == 0) {
		p->repeat_key   = key;
		p->repeat_armed = true;
	}
}

/* An input method takes a dead key before it is delivered as a key, so an
 * accent that arrives here is the fallback composer's to hold. */
static void key_type(App *a, uint32_t key, bool repeat)
{
	struct BackendState *p = a->backend_state;
	const KeyMods mods = kb_mods(p);
	const uint32_t held = p->compose.dead;
	const uint32_t c = keymap_type(&p->keymap, &p->compose, key + 8u, &mods,
								   repeat);
	app_text(a, c);
	/* Only on a change, and not with the input method enabled: it reports
	 * its own composition, and an empty string from here would wipe its
	 * candidate. */
	if (p->compose.dead != held && !p->ti_enabled) {
		char shown[8];
		keymap_composing(&p->compose, shown);
		app_compose_set(a, shown);
	}
}

/* Called after the turn's dispatch, so a release that arrived beside the timer
 * has already disarmed it. One repeat however many periods were missed. */
static void key_repeat_fire(App *a)
{
	struct BackendState *p = a->backend_state;
	uint64_t periods;
	const ssize_t got = read(p->repeat_fd, &periods, sizeof periods);
	if (got != (ssize_t)sizeof periods)
		return;
	if (!p->repeat_armed)
		return;
	if (!app_text_input(a)) {
		key_repeat_stop(p);
		return;
	}
	app_key_repeat(a, key_from_evdev(p->repeat_key));
	key_type(a, p->repeat_key, true);
}

/* The keys already down arrive with focus. Seeded, not replayed through
 * app_key(): a replay fires one-shot bindings, and Escape held across an
 * alt-tab would close the window. */
static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
					 struct wl_surface *s, struct wl_array *keys)
{
	(void)kb; (void)serial; (void)s;
	App *a = data;

	app_keys_clear(a);
	uint32_t *k;
	wl_array_for_each(k, keys)
		app_key_seed(a, key_from_evdev(*k));

	/* After the seeding, so fnl_key_down() is right when the event is read. */
	a->backend_state->kb_focused = true;
	app_post_focus(a, true);
}

/* The keyboard has gone, by focus or by capability, and no release will arrive
 * for what it held. */
static void kb_drop_held(App *a)
{
	struct BackendState *p = a->backend_state;
	app_keys_clear(a);
	key_repeat_stop(p);
	p->compose.dead = 0;
	if (!p->ti_enabled)
		app_compose_set(a, NULL);
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
					 struct wl_surface *s)
{
	(void)kb; (void)serial; (void)s;
	App *a = data;
	kb_drop_held(a);

	/* After the drop, for kb_enter()'s reason. */
	a->backend_state->kb_focused = false;
	app_post_focus(a, false);
}
static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
				   uint32_t time, uint32_t key, uint32_t state)
{
	(void)kb;
	App *a = data;
	struct BackendState *p = a->backend_state;
	const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	p->input_serial = serial;
	const FnlKey k = key_from_evdev(key);
	loop_input_ms(a, time);

	/* Key first, text second: the order fenestal.h promises. */
	app_key(a, k, pressed);

	if (!pressed) {
		if (p->repeat_armed && key == p->repeat_key)
			key_repeat_stop(p);
		return;
	}
	if (!app_text_input(a))
		return;

	key_type(a, key, false);
	/* The newest key takes the repeat over; a modifier does not. */
	if (keymap_held_repeats(&p->keymap, key + 8u, k))
		key_repeat_start(p, key);
}
static void kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
						 uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp)
{
	(void)kb; (void)serial;
	App *a = d;
	a->backend_state->kb_mods  = dep | lat | lck;
	a->backend_state->kb_group = grp;
}
static void kb_repeat(void *d, struct wl_keyboard *kb, int32_t rate,
					  int32_t delay)
{
	(void)kb;
	App *a = d;
	struct BackendState *p = a->backend_state;
	p->repeat_rate  = rate;
	p->repeat_delay = delay;
	if (rate <= 0)
		key_repeat_stop(p);
}
static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
	.modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

/* ---- pointer / decorations --------------------------------------------- */

/* Wayland's positions are surface space, grab ring included; everything drawn,
 * and every FnlEvent, is window space. */
static double win_pt(const App *a, double surface_pt)
{
	return surface_pt - deco_margin(a, a->is_fullscreen);
}

/* The directional cursor a resize edge wears. */
#define SHAPE(dir) WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_##dir##_RESIZE

static const uint32_t EDGE_SHAPES[EDGE_COUNT] = {
	[EDGE_NONE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
	[EDGE_T]    = SHAPE(N),
	[EDGE_B]    = SHAPE(S),
	[EDGE_L]    = SHAPE(W),
	[EDGE_R]    = SHAPE(E),
	[EDGE_TL]   = SHAPE(NW),
	[EDGE_TR]   = SHAPE(NE),
	[EDGE_BL]   = SHAPE(SW),
	[EDGE_BR]   = SHAPE(SE),
};

#undef SHAPE

/* WL_CURSOR_HIDDEN is wl_pointer.set_cursor with a NULL surface: the shape
 * protocol has no way to say "none". */
static void set_cursor_shape(App *a, uint32_t shape)
{
	struct BackendState *p = a->backend_state;
	if (p->current_shape == shape)
		return;

	if (shape == WL_CURSOR_HIDDEN) {
		/* Only when the shape protocol can bring it back: core wl_pointer has
		 * no "the default again", so a hide without it is one-way. */
		if (!p->pointer || !p->cursor_device)
			return;
		p->current_shape = shape;
		wl_pointer_set_cursor(p->pointer, p->pointer_serial, NULL, 0, 0);
		return;
	}
	if (!p->cursor_device)
		return;
	p->current_shape = shape;
	wp_cursor_shape_device_v1_set_shape(p->cursor_device, p->pointer_serial,
										shape);
}

/* fnl_set_cursor()'s shapes; the compositor draws them from the theme. */
#define SHAPE(name) WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_##name

static const uint32_t CALLER_SHAPES[] = {
	[FNL_CURSOR_DEFAULT]     = SHAPE(DEFAULT),
	[FNL_CURSOR_TEXT]        = SHAPE(TEXT),
	[FNL_CURSOR_POINTER]     = SHAPE(POINTER),
	[FNL_CURSOR_CROSSHAIR]   = SHAPE(CROSSHAIR),
	[FNL_CURSOR_MOVE]        = SHAPE(MOVE),
	[FNL_CURSOR_EW_RESIZE]   = SHAPE(EW_RESIZE),
	[FNL_CURSOR_NS_RESIZE]   = SHAPE(NS_RESIZE),
	[FNL_CURSOR_NOT_ALLOWED] = SHAPE(NOT_ALLOWED),
	[FNL_CURSOR_WAIT]        = SHAPE(WAIT),
};
_Static_assert(sizeof CALLER_SHAPES / sizeof CALLER_SHAPES[0] ==
			   FNL_CURSOR_WAIT + 1,
			   "a FnlCursor was appended without a shape for it here");

#undef SHAPE

static void cursor_update(App *a, Zone z, Edge edge)
{
	const DecoCursor c = deco_cursor(a, z, edge);
	set_cursor_shape(a, c.kind == DECO_CURSOR_HIDDEN ? WL_CURSOR_HIDDEN
					  : c.kind == DECO_CURSOR_EDGE   ? EDGE_SHAPES[c.edge]
													 : CALLER_SHAPES[c.shape]);
}

/* No-ops, deliberately. The lock is PERSISTENT, so `unlocked` is a suspension
 * the compositor ends by itself; ending the capture there would make
 * alt-tabbing away release it. */
static void locked_locked(void *d, struct zwp_locked_pointer_v1 *l)
{ (void)d; (void)l; }
static void locked_unlocked(void *d, struct zwp_locked_pointer_v1 *l)
{ (void)d; (void)l; }
static const struct zwp_locked_pointer_v1_listener locked_listener = {
	.locked = locked_locked, .unlocked = locked_unlocked,
};

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *rp,
							uint32_t hi, uint32_t lo, wl_fixed_t dx,
							wl_fixed_t dy, wl_fixed_t dxu, wl_fixed_t dyu)
{
	(void)rp;
	App *a = data;
	if (!a->pointer_captured)
		return;
	/* Microseconds, where wl_pointer's are milliseconds. */
	loop_input_us(a, (uint64_t)hi << 32 | lo);

	/* Unaccelerated, as Win32's raw input and KMS's evdev are: a camera
	 * turned by the mouse wants the hand's movement, not the desktop's curve
	 * for a pointer. Both deltas are in wl_pointer.motion's dimension, so
	 * points either way. A compositor with no unaccelerated figure for the
	 * device sends zeros beside a movement, and the accelerated one is then
	 * the only movement there is. */
	const int s120 = want_s120(a);
	double mx = wl_fixed_to_double(dxu), my = wl_fixed_to_double(dyu);
	if ((mx == 0.0 && my == 0.0 && (dx || dy)) ||
		FNL_FAULT("wl-accelerated-relative")) {
		mx = wl_fixed_to_double(dx);
		my = wl_fixed_to_double(dy);
		if (!a->backend_state->relative_accel_said) {
			a->backend_state->relative_accel_said = true;
			fnl_log(FNL_LOG_INFO, "relative motion: the compositor gives "
								  "no unaccelerated delta; accelerated "
								  "instead");
		}
	}
	app_pointer_relative(a, mx, my);

	/* Deltas arrive in logical points; the reticle lives in pixels. */
	app_reticle_move(a, pt_to_pxf(mx, s120), pt_to_pxf(my, s120));

	/* Where the desktop cursor reappears. Double-buffered, latched by the next
	 * wl_surface.commit, so it is set as the reticle moves: set at release it
	 * would be destroyed with the lock, unread. */
	int cx, cy;
	app_content_px_rect(a, &cx, &cy, NULL, NULL);
	const int m = deco_margin(a, a->is_fullscreen);
	zwp_locked_pointer_v1_set_cursor_position_hint(
		a->backend_state->locked,
		wl_fixed_from_double(px_to_ptf(cx + a->cur_px, s120) + m),
		wl_fixed_from_double(px_to_ptf(cy + a->cur_py, s120) + m));
}
static const struct zwp_relative_pointer_v1_listener relative_listener = {
	.relative_motion = relative_motion,
};

/* Lock the pointer and draw our own. PERSISTENT, so it re-arms after a focus
 * change; only F9 or exit ends it. */
static void capture_pointer(App *a)
{
	struct BackendState *p = a->backend_state;
	/* Both protocols or neither: a lock without zwp_relative_pointer_v1 is a
	 * hidden cursor and a reticle nothing can move. */
	if (a->pointer_captured || !p->pointer_constraints ||
		!p->relative_manager || !p->pointer)
		return;

	app_reticle_start(a);

	/* No region: it says where the pointer must be for the lock to *engage*,
	 * not where it may go. The reticle is bounded in app_reticle_move(). */
	p->locked = zwp_pointer_constraints_v1_lock_pointer(
		p->pointer_constraints, p->surface, p->pointer, NULL,
		ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	zwp_locked_pointer_v1_add_listener(p->locked, &locked_listener, a);

	if (!p->relative) {
		p->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(
			p->relative_manager, p->pointer);
		zwp_relative_pointer_v1_add_listener(p->relative, &relative_listener,
											 a);
	}

	a->pointer_captured = true;
	set_cursor_shape(a, WL_CURSOR_HIDDEN);
	app_reticle_publish(a);
}

static void release_pointer_capture(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!a->pointer_captured)
		return;

	/* No hint here: relative_motion() set it, and a commit has latched it. */
	zwp_locked_pointer_v1_destroy(p->locked);
	p->locked = NULL;
	if (p->relative) {
		zwp_relative_pointer_v1_destroy(p->relative);
		p->relative = NULL;
	}
	a->pointer_captured = false;
	/* current_shape still says HIDDEN; the update below asks for another. */
	pointer_update(a);
	app_reticle_publish(a);
}

static void pointer_update(App *a)
{
	Edge edge;
	const Zone z = deco_pointer_at(a, win_pt(a, a->ptr_x), win_pt(a, a->ptr_y),
								   &edge);
	cursor_update(a, z, edge);
}

/* Decide the shape again where the pointer already is. Only over the window:
 * elsewhere no enter serial is in force to send it under, and ptr_enter()
 * decides afresh. */
static void wl_cursor_changed(App *a)
{
	if (!a->backend_state->pointer || !a->backend_state->pointer_inside)
		return;
	Edge edge;
	const Zone z = deco_hit(a, win_pt(a, a->ptr_x), win_pt(a, a->ptr_y), &edge);
	cursor_update(a, z, edge);
}

/* The position, to the caller, in window points. On enter as well as on
 * motion, so a hover is live for a pointer that arrives and stops; not while
 * captured, when FNL_EVENT_RELATIVE carries the movement. */
static void post_motion(App *a)
{
	app_pointer_motion(a, win_pt(a, a->ptr_x), win_pt(a, a->ptr_y));
}

static void ptr_enter(void *data, struct wl_pointer *wp, uint32_t serial,
					  struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)wp; (void)s;
	App *a = data;
	struct BackendState *p = a->backend_state;
	p->pointer_serial = serial;
	a->ptr_x = wl_fixed_to_double(sx);
	a->ptr_y = wl_fixed_to_double(sy);
	p->current_shape = 0;      /* a cursor is set per enter, hidden included */
	p->pointer_inside = true;
	p->left_held = false;       /* a release elsewhere is never delivered */
	pointer_update(a);
	app_reticle_publish(a);
	/* An enter carries no time: the motion it posts counts from now. */
	app_input_age(a, -1.0);
	post_motion(a);
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
					  struct wl_surface *s)
{
	(void)p; (void)serial; (void)s;
	App *a = data;
	a->backend_state->current_shape = 0;
	a->backend_state->pointer_inside = false;
	a->backend_state->left_held = false;
	deco_release(a);              /* the press is someone else's problem now */
	app_reticle_publish(a);
	deco_publish_hover(a, ZONE_CONTENT);
}
static void ptr_motion(void *data, struct wl_pointer *wp, uint32_t time,
					   wl_fixed_t sx, wl_fixed_t sy)
{
	(void)wp;
	App *a = data;
	a->ptr_x = wl_fixed_to_double(sx);
	a->ptr_y = wl_fixed_to_double(sy);
	loop_input_ms(a, time);
	post_motion(a);
	deco_drag(a, win_pt(a, a->ptr_x), win_pt(a, a->ptr_y));
	pointer_update(a);
	app_reticle_publish(a);
}
static void ptr_button(void *data, struct wl_pointer *wp, uint32_t serial,
					   uint32_t time, uint32_t button, uint32_t state)
{
	(void)wp;
	App *a = data;
	struct BackendState *p = a->backend_state;
	p->input_serial = serial;

	/* The three buttons FnlButton names. Any other -- a side button -- is
	 * reported to nobody and acts on nothing: it is not a left click. */
	FnlButton b;
	switch (button) {
	case BTN_LEFT:   b = FNL_BUTTON_LEFT;   break;
	case BTN_RIGHT:  b = FNL_BUTTON_RIGHT;  break;
	case BTN_MIDDLE: b = FNL_BUTTON_MIDDLE; break;
	default:         return;
	}
	const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

	/* The serials first: a press the library answers with a move, or a
	 * caller with fnl_begin_move(), starts the grab from them. */
	if (pressed)
		p->button_serial = serial;
	if (b == FNL_BUTTON_LEFT) {
		p->left_held = pressed;
		if (pressed)
			p->left_serial = serial;
	}
	loop_input_ms(a, time);
	app_pointer_button(a, b, pressed, win_pt(a, a->ptr_x),
					   win_pt(a, a->ptr_y), time);
}
/* In notches, as fenestal.h gives and the other backends deliver: a
 * wheel's count from axis_discrete where one came with this event, and a
 * smooth source's travel divided by WL_AXIS_PER_NOTCH otherwise -- the scale
 * wl_pointer's axis values are on, ten to a wheel's step on the compositors
 * that send both, and what GTK divides by. */
#define WL_AXIS_PER_NOTCH 10.0

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t ax,
					 wl_fixed_t v)
{
	(void)p;
	App *a = d;
	const int i = ax == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1 : 0;
	loop_input_ms(a, t);
	const double notches = a->backend_state->axis_discrete[i]
		? (double)a->backend_state->axis_discrete[i]
		: wl_fixed_to_double(v) / WL_AXIS_PER_NOTCH;
	a->backend_state->axis_discrete[i] = 0;

	app_pointer_scroll(a, win_pt(a, a->ptr_x), win_pt(a, a->ptr_y),
					   i ? notches : 0.0, i ? 0.0 : notches);
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
						  uint32_t a)
{ (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t ax,
							  int32_t v)
{
	(void)p;
	App *a = d;
	const int i = ax == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1 : 0;
	a->backend_state->axis_discrete[i] = v;
}
static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
	.axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void release_keyboard(struct wl_keyboard *kb)
{
	if (wl_keyboard_get_version(kb) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
		wl_keyboard_release(kb);
	else
		wl_keyboard_destroy(kb);
}
static void release_pointer(struct wl_pointer *p)
{
	if (wl_pointer_get_version(p) >= WL_POINTER_RELEASE_SINCE_VERSION)
		wl_pointer_release(p);
	else
		wl_pointer_destroy(p);
}
static void release_output(struct wl_output *o)
{
	if (wl_output_get_version(o) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
		wl_output_release(o);
	else
		wl_output_destroy(o);
}

/* ---- the input method ------------------------------------------------------
 *
 * zwp_text_input_v3, for what keymap.c cannot do: Compose sequences and the
 * input methods a CJK layout is typed through. It never overlaps the key
 * path: a key it consumes is not delivered as wl_keyboard.key at all.
 * Enabled only while text entry is on *and* this surface has its focus --
 * left on for a game, it would be composing the movement keys. */

/* FNL_NO_INPUT_METHOD in the environment, set and not "0", leaves it
 * unbound, and text is then typed as it is on a compositor that has none:
 * through keymap.c's own composer. A diagnostic, as FNL_ECHO_EVENTS is --
 * the one way to type through that composer on a desktop whose compositor
 * always has an input method. */
static bool ti_refused(void)
{
	const char *v = getenv("FNL_NO_INPUT_METHOD");
	return v && v[0] && !(v[0] == '0' && !v[1]);
}

static void ti_drop(struct BackendState *p)
{
	p->ti_has_commit = false;
	p->ti_commit[0] = p->ti_preedit[0] = '\0';
}

/* Cut on a character boundary if it must be. NULL is the protocol's none. */
static void ti_keep(char *dst, size_t size, const char *text)
{
	const size_t n = text ? utf8_fit(text, strlen(text), size) : 0;
	if (n)
		memcpy(dst, text, n);
	dst[n] = '\0';
}

static void ti_apply(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->ti)
		return;

	if (p->ti_focus && app_text_input(a)) {
		/* enable resets everything said before it, so the content type goes
		 * with the first one and the rectangle goes every time. */
		if (!p->ti_enabled) {
			zwp_text_input_v3_enable(p->ti);
			zwp_text_input_v3_set_content_type(
				p->ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
				ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
			/* The fallback's held accent is the input method's business from
			 * here. On the way in only: this runs again for every caret move,
			 * and an accent the input method passed through must outlive
			 * those. */
			p->compose.dead = 0;
		}
		/* The caret in surface space: window points plus the grab ring. A
		 * caller that never said gets the window's corner. */
		int x, y, w, h;
		if (!app_text_rect(a, &x, &y, &w, &h)) {
			x = y = 0;
			w = h = 1;
		}
		const int m = deco_margin(a, a->is_fullscreen);
		zwp_text_input_v3_set_cursor_rectangle(p->ti, x + m, y + m, w, h);
		zwp_text_input_v3_commit(p->ti);
		p->ti_enabled = true;
	} else if (p->ti_enabled) {
		zwp_text_input_v3_disable(p->ti);
		zwp_text_input_v3_commit(p->ti);
		p->ti_enabled = false;
		ti_drop(p);
		app_compose_set(a, NULL);
	}
}

static void ti_enter(void *data, struct zwp_text_input_v3 *ti,
					 struct wl_surface *s)
{
	(void)ti; (void)s;
	App *a = data;
	a->backend_state->ti_focus = true;
	ti_apply(a);
}
static void ti_leave(void *data, struct zwp_text_input_v3 *ti,
					 struct wl_surface *s)
{
	(void)ti; (void)s;
	App *a = data;
	a->backend_state->ti_focus = false;
	ti_apply(a);
}
static void ti_preedit(void *data, struct zwp_text_input_v3 *ti,
					   const char *text, int32_t begin, int32_t end)
{
	(void)ti; (void)begin; (void)end;
	App *a = data;
	ti_keep(a->backend_state->ti_preedit, sizeof a->backend_state->ti_preedit,
			text);
}
static void ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
							 const char *text)
{
	(void)ti;
	App *a = data;
	struct BackendState *p = a->backend_state;
	ti_keep(p->ti_commit, sizeof p->ti_commit, text);
	p->ti_has_commit = text != NULL;
}
/* Never asked for: it deletes around a caret this client has not described,
 * since set_surrounding_text is never sent. */
static void ti_delete(void *data, struct zwp_text_input_v3 *ti,
					  uint32_t before, uint32_t after)
{ (void)data; (void)ti; (void)before; (void)after; }

/* In the protocol's order: the commit is typed, then the composition becomes
 * whatever this round said, which may be nothing. */
static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial)
{
	(void)ti; (void)serial;
	App *a = data;
	struct BackendState *p = a->backend_state;
	/* One already on its way when composing was switched off. */
	if (!p->ti_enabled) {
		ti_drop(p);
		return;
	}
	if (p->ti_has_commit)
		app_text_utf8(a, p->ti_commit);
	app_compose_set(a, p->ti_preedit);
	ti_drop(p);
}
static const struct zwp_text_input_v3_listener text_input_listener = {
	.enter = ti_enter, .leave = ti_leave,
	.preedit_string = ti_preedit, .commit_string = ti_commit_string,
	.delete_surrounding_text = ti_delete, .done = ti_done,
};

static void wl_text_input_changed(App *a)
{
	ti_apply(a);
	if (!app_text_input(a)) {
		a->backend_state->compose.dead = 0;
		app_compose_set(a, NULL);
	}
}

/* ---- the clipboard --------------------------------------------------------
 *
 * wl_data_device's selection, text only, and none of its drag and drop.
 * CLIP_MIME is in order of preference, best last.
 *
 * Neither direction holds the bytes. Offering is a promise to write them down
 * a pipe when asked, so App::clip_out stays for as long as the offer stands;
 * asking reads a pipe from the epoll set and never in a loop here, because
 * the pump does not wait on other programs. */
static const char *const CLIP_MIME[] = {
	NULL, "text/plain", "UTF8_STRING", "text/plain;charset=utf-8" };
#define CLIP_KINDS ((int)(sizeof CLIP_MIME / sizeof CLIP_MIME[0]))
#define CLIP_CHUNK ((size_t)65536)
/* How long a copy's reader is given for the whole string, however it
 * paces its reads; a paste's owner has CLIP_PATIENCE_S (clip_linux.h).
 * Another program's pace must not become this window's. */
#define CLIP_WRITE_MS    250

/* The paste's pipe out of the epoll set and closed: at its end, or given
 * up. The owner then sees its write fail, which is how it learns. */
static void clip_close(struct BackendState *p)
{
	if (p->clip_fd >= 0) {
		epoll_ctl(p->epoll_fd, EPOLL_CTL_DEL, p->clip_fd, NULL);
		close(p->clip_fd);
		p->clip_fd = -1;
	}
}

/* `whole` is whether all of it arrived: a paste cut short may end inside a
 * character, and is reported as no text. */
static void clip_finish(App *a, bool whole)
{
	struct BackendState *p = a->backend_state;
	clip_close(p);
	clip_in_finish(a, &p->clip, whole);
}

/* One read a turn, straight into the buffer the answer is handed over in. The
 * epoll set is level-triggered, so what this leaves brings the pump back; it
 * never loops on another program's descriptor, and -fanalyzer loses a buffer
 * grown inside a loop. */
static void clip_readable(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!clip_in_room(&p->clip, CLIP_CHUNK)) {
		clip_finish(a, false);
		return;
	}
	const ssize_t n = read(p->clip_fd, p->clip.buf + p->clip.len, CLIP_CHUNK);
	if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
		return;
	if (n > 0) {
		if (!clip_in_grew(&p->clip, (size_t)n))
			clip_finish(a, false);
		return;
	}
	clip_finish(a, n == 0);
}

static void wl_clipboard_request(App *a)
{
	struct BackendState *p = a->backend_state;
	/* Our own selection is answered from the string: asked for the long way,
	 * this pump would wait on a pipe only it can write to. So is one set and
	 * not yet answered, whose offer_sel is the one before (clip_sync).
	 * Before the test for a paste in flight: that one is the owner before
	 * this window copied, and its text is no answer to a request made since.
	 * It is dropped unanswered, as requests share one answer (fenestal.h)
	 * and this is it. */
	if (a->clip_out &&
		(p->clip_sync || (p->offer_sel && p->offer_sel_ours))) {
		if (p->clip_fd >= 0) {
			clip_close(p);
			clip_in_free(&p->clip);
		}
		app_clipboard_deliver(a, a->clip_out, a->clip_out_len);
		return;
	}
	if (p->clip_fd >= 0)
		return;                 /* one already on its way answers both */

	int fds[2];
	if (!p->offer_sel || !p->offer_sel_kind || pipe2(fds, O_CLOEXEC) < 0) {
		app_clipboard_deliver(a, NULL, 0);
		return;
	}
	/* Our end only: the write end is the owner's, and it expects to block. */
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

	wl_data_offer_receive(p->offer_sel, CLIP_MIME[p->offer_sel_kind], fds[1]);
	/* Ours has to close, or end of file never comes. */
	close(fds[1]);

	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[0] };
	if (epoll_ctl(p->epoll_fd, EPOLL_CTL_ADD, fds[0], &ev) < 0) {
		close(fds[0]);
		app_clipboard_deliver(a, NULL, 0);
		return;
	}
	p->clip_fd = fds[0];
	clip_in_renew(&p->clip);
}

/* Write the whole string to a reader that may have gone. That raises SIGPIPE,
 * whose default ends the process, so the signal is blocked for the write and
 * collected before the mask goes back -- which leaves the host's own handling
 * of SIGPIPE alone. */
static void clip_write(int fd, const char *s, size_t n)
{
	sigset_t only_pipe, before;
	sigemptyset(&only_pipe);
	sigaddset(&only_pipe, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &only_pipe, &before);

	/* Never a blocking write: a reader that asked and then stopped reading
	 * would hold the pump. Waited on while the pipe is full, up to one
	 * deadline for the whole string rather than a bound per wait, which a
	 * reader taking a page at a time would renew for ever; one that has not
	 * taken it all by then gets what it took. */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	const double deadline = now_seconds() + CLIP_WRITE_MS * 1e-3;
	while (n > 0) {
		const ssize_t w = write(fd, s, n);
		if (w > 0) {
			s += w;
			n -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			const int ms = loop_wait_ms(true, deadline);
			struct pollfd pf = { .fd = fd, .events = POLLOUT };
			if (ms > 0 && poll(&pf, 1, ms) > 0)
				continue;
		}
		break;
	}

	if (!sigismember(&before, SIGPIPE)) {
		const struct timespec now = { 0, 0 };
		sigtimedwait(&only_pipe, NULL, &now);
	}
	pthread_sigmask(SIG_SETMASK, &before, NULL);
}

static void source_target(void *d, struct wl_data_source *s, const char *mime)
{ (void)d; (void)s; (void)mime; }
static void source_send(void *data, struct wl_data_source *s, const char *mime,
						int32_t fd)
{
	(void)s; (void)mime;
	App *a = data;
	if (a->clip_out)
		clip_write(fd, a->clip_out, a->clip_out_len);
	close(fd);
}
/* Somebody else copied, and this offer is over. */
static void source_cancelled(void *data, struct wl_data_source *s)
{
	App *a = data;
	if (a->backend_state->clip_source == s)
		a->backend_state->clip_source = NULL;
	wl_data_source_destroy(s);
}
static const struct wl_data_source_listener source_listener = {
	.target = source_target, .send = source_send,
	.cancelled = source_cancelled,
};

/* The compositor has taken the set, and sent the selection event it caused
 * ahead of this: offer_sel is the selection in force again. */
static void clip_sync_done(void *data, struct wl_callback *cb, uint32_t t)
{
	(void)t;
	App *a = data;
	a->backend_state->clip_sync = NULL;
	wl_callback_destroy(cb);
}
static const struct wl_callback_listener clip_sync_listener = {
	.done = clip_sync_done,
};

static void wl_clipboard_set(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->ddm || !p->dd)
		return;
	struct wl_data_source *src =
		wl_data_device_manager_create_data_source(p->ddm);
	if (!src)
		return;
	wl_data_source_add_listener(src, &source_listener, a);
	for (int k = CLIP_KINDS - 1; k >= 1; k--)
		wl_data_source_offer(src, CLIP_MIME[k]);
	wl_data_source_offer(src, p->clip_marker);
	wl_data_device_set_selection(p->dd, src, p->input_serial);
	/* A paste asked before the compositor has taken this -- a copy and a
	 * paste in one turn, which core promises pastes the copy -- would ask
	 * the owner before. Answered from the string until a sync says the
	 * set is taken, and no longer: having asked is not owning, and a set
	 * the compositor refuses leaves the selection where it was. One sync
	 * at a time; the one it replaces is destroyed unanswered. */
	if (p->clip_sync)
		wl_callback_destroy(p->clip_sync);
	p->clip_sync = wl_display_sync(p->display);
	if (p->clip_sync)
		wl_callback_add_listener(p->clip_sync, &clip_sync_listener, a);
	/* The one it replaces is destroyed here rather than left for its
	 * cancelled event, which is then never sent for it. */
	if (p->clip_source)
		wl_data_source_destroy(p->clip_source);
	p->clip_source = src;
}

static void offer_mime(void *data, struct wl_data_offer *o, const char *mime)
{
	App *a = data;
	struct BackendState *p = a->backend_state;
	if (o != p->offer_new)
		return;
	if (!strcmp(mime, p->clip_marker))
		p->offer_new_ours = true;
	for (int k = 1; k < CLIP_KINDS; k++)
		if (k > p->offer_new_kind && !strcmp(mime, CLIP_MIME[k]))
			p->offer_new_kind = k;
}
static const struct wl_data_offer_listener offer_listener = {
	.offer = offer_mime,
};

static void dd_data_offer(void *data, struct wl_data_device *dd,
						  struct wl_data_offer *offer)
{
	(void)dd;
	App *a = data;
	struct BackendState *p = a->backend_state;
	/* One that was described and then never named as anything. */
	if (p->offer_new)
		wl_data_offer_destroy(p->offer_new);
	p->offer_new = offer;
	p->offer_new_kind = 0;
	p->offer_new_ours = false;
	wl_data_offer_add_listener(offer, &offer_listener, a);
}
/* A drag's offer. Nothing is accepted, so it is let go at once. */
static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
					 struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
					 struct wl_data_offer *offer)
{
	(void)dd; (void)serial; (void)s; (void)x; (void)y;
	App *a = data;
	if (!offer)
		return;
	if (a->backend_state->offer_new == offer)
		a->backend_state->offer_new = NULL;
	wl_data_offer_destroy(offer);
}
static void dd_leave(void *d, struct wl_data_device *dd)
{ (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t,
					  wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd)
{ (void)d; (void)dd; }
static void dd_selection(void *data, struct wl_data_device *dd,
						 struct wl_data_offer *offer)
{
	(void)dd;
	App *a = data;
	struct BackendState *p = a->backend_state;
	if (p->offer_sel && p->offer_sel != offer)
		wl_data_offer_destroy(p->offer_sel);
	p->offer_sel = offer;
	p->offer_sel_kind = 0;
	p->offer_sel_ours = false;
	if (offer && offer == p->offer_new) {
		p->offer_sel_kind = p->offer_new_kind;
		p->offer_sel_ours = p->offer_new_ours;
		p->offer_new = NULL;
	}
}
static const struct wl_data_device_listener data_device_listener = {
	.data_offer = dd_data_offer, .enter = dd_enter, .leave = dd_leave,
	.motion = dd_motion, .drop = dd_drop, .selection = dd_selection,
};

/* The per-seat objects that need a seat and a manager both. Globals arrive in
 * no promised order, so this is asked after each. */
static void seat_extras(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->seat)
		return;
	if (p->ddm && !p->dd) {
		p->dd = wl_data_device_manager_get_data_device(p->ddm, p->seat);
		wl_data_device_add_listener(p->dd, &data_device_listener, a);
	}
	if (p->ti_manager && !p->ti) {
		p->ti = zwp_text_input_manager_v3_get_text_input(p->ti_manager,
														 p->seat);
		zwp_text_input_v3_add_listener(p->ti, &text_input_listener, a);
	}
}

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	App *a = data;
	struct BackendState *p = a->backend_state;
	bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (has_kb && !p->keyboard) {
		p->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(p->keyboard, &keyboard_listener, a);
	} else if (!has_kb && p->keyboard) {
		release_keyboard(p->keyboard);
		p->keyboard = NULL;
		kb_drop_held(a);
		/* The leave that would have said so never comes for a released
		 * keyboard; once, since a keyboard that never entered had nothing to
		 * lose. */
		if (p->kb_focused) {
			p->kb_focused = false;
			app_post_focus(a, false);
		}
	}
	bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	if (has_ptr && !p->pointer) {
		p->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(p->pointer, &pointer_listener, a);

		if (p->cursor_shape_manager)
			p->cursor_device = wp_cursor_shape_manager_v1_get_pointer(
				p->cursor_shape_manager, p->pointer);
	} else if (!has_ptr && p->pointer) {
		/* The lock and the relative pointer were made from this pointer, and
		 * must go before it does. */
		release_pointer_capture(a);
		if (p->cursor_device) {
			wp_cursor_shape_device_v1_destroy(p->cursor_device);
			p->cursor_device = NULL;
		}
		release_pointer(p->pointer);
		p->pointer = NULL;
		p->pointer_inside = false;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps, .name = seat_name,
};

/* ---- when a frame reached the screen (fnl_frame_shown()) -----------------
 *
 * A wp_presentation_feedback asked for just before a present rides on the
 * commit it makes, as the frame callback does (surface_frame_request()),
 * and is answered on the main thread when the compositor showed that
 * commit -- or discarded it for a later one. The stamp is the compositor's,
 * kept only on CLOCK_MONOTONIC, the frame clock's. */

static void presentation_clock(void *data, struct wp_presentation *pr,
							   uint32_t clk)
{
	(void)pr;
	App *a = data;
	a->backend_state->presentation_mono = clk == CLOCK_MONOTONIC;
}

static const struct wp_presentation_listener presentation_listener = {
	.clock_id = presentation_clock,
};

/* The slot's frame, and the slot let go; false where the frame is not to
 * be stamped. */
static bool feedback_take(WlFeedback *f, uint64_t *index, bool shown)
{
	App *a = f->a;
	fnl_mutex_lock(&a->lock);
	const bool keep = f->live && !f->cancelled;
	f->shown = keep && shown;
	*index = f->index;
	if (f->fb)
		wp_presentation_feedback_destroy(f->fb);
	f->fb = NULL;
	f->live = false;
	fnl_mutex_unlock(&a->lock);
	return keep;
}

static void feedback_sync_output(void *data,
								 struct wp_presentation_feedback *fb,
								 struct wl_output *o)
{
	(void)data; (void)fb; (void)o;
}

static void feedback_presented(void *data, struct wp_presentation_feedback *fb,
							   uint32_t sec_hi, uint32_t sec_lo,
							   uint32_t nsec, uint32_t refresh,
							   uint32_t seq_hi, uint32_t seq_lo,
							   uint32_t flags)
{
	(void)fb; (void)refresh; (void)seq_hi; (void)seq_lo;
	WlFeedback *f = data;
	App *a = f->a;
	uint64_t index;
	if (!feedback_take(f, &index, true) ||
		!a->backend_state->presentation_mono)
		return;
	const double t = (double)(((uint64_t)sec_hi << 32) | sec_lo) +
					 (double)nsec * 1e-9;
	app_frame_shown(a, index, t,
					(flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC) != 0);
}

static void feedback_discarded(void *data,
							   struct wp_presentation_feedback *fb)
{
	(void)fb;
	uint64_t index;
	(void)feedback_take(data, &index, false);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
	.sync_output = feedback_sync_output,
	.presented   = feedback_presented,
	.discarded   = feedback_discarded,
};

/* [render, under App::lock] This frame's feedback, where a caller counts
 * frames (fnl_set_frame_stats()) and a slot is free. */
static void feedback_request(App *a)
{
	struct BackendState *p = a->backend_state;
	p->feedback_cur = -1;
	if (!p->presentation || !fnl_atomic_load_bool(&a->stats_on))
		return;
	for (int i = 0; i < WL_FEEDBACK_SLOTS; i++) {
		WlFeedback *f = &p->feedback[i];
		if (f->live)
			continue;
		f->fb = wp_presentation_feedback(p->presentation, p->present);
		if (!f->fb)
			return;
		f->a = a;
		f->index = a->presents;
		f->live = true;
		f->cancelled = false;
		f->shown = false;
		wp_presentation_feedback_add_listener(f->fb, &feedback_listener, f);
		p->feedback_cur = i;
		return;
	}
}

/* Bind at the version written to, or the compositor's if that is older. */
#define IS(i)        (!strcmp(iface, i##_interface.name))
#define BIND(i, max) \
	wl_registry_bind(r, name, &i##_interface, version < (max) ? version : (max))

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
							const char *iface, uint32_t version)
{
	App *a = data;
	struct BackendState *p = a->backend_state;
	if (IS(wl_compositor)) {
		p->compositor = BIND(wl_compositor, 4);
	} else if (IS(wl_subcompositor)) {
		p->subcompositor = BIND(wl_subcompositor, 1);
	} else if (IS(wl_shm)) {
		p->shm = BIND(wl_shm, 1);
	} else if (IS(xdg_wm_base)) {
		/* v4 for xdg_toplevel.configure_bounds; nothing above that is used. */
		p->wm_base = BIND(xdg_wm_base, 4);
		xdg_wm_base_add_listener(p->wm_base, &wm_base_listener, a);
	} else if (IS(wl_seat) && !p->seat) {
		/* The first seat only. wl_seat is the one global here a compositor
		 * may advertise more than once, and a second bound over the first
		 * leaks a proxy whose listener still runs against this App. */
		p->seat = BIND(wl_seat, 5);
		wl_seat_add_listener(p->seat, &seat_listener, a);
	} else if (IS(wp_cursor_shape_manager_v1)) {
		p->cursor_shape_manager = BIND(wp_cursor_shape_manager_v1, 1);
	} else if (IS(zxdg_decoration_manager_v1)) {
		p->decoration_manager = BIND(zxdg_decoration_manager_v1, 1);
	} else if (IS(zwp_relative_pointer_manager_v1)) {
		p->relative_manager = BIND(zwp_relative_pointer_manager_v1, 1);
	} else if (IS(zwp_pointer_constraints_v1)) {
		p->pointer_constraints = BIND(zwp_pointer_constraints_v1, 1);
	} else if (IS(wp_viewporter) && !FNL_FAULT("no-viewporter")) {
		p->viewporter = BIND(wp_viewporter, 1);
	} else if (IS(wp_presentation) && !p->presentation) {
		p->presentation = BIND(wp_presentation, 1);
		wp_presentation_add_listener(p->presentation,
									 &presentation_listener, a);
	} else if (IS(wp_fractional_scale_manager_v1)) {
		p->fractional_manager = BIND(wp_fractional_scale_manager_v1, 1);
	} else if (IS(wl_output)) {
		if (p->output_count < WL_MAX_OUTPUTS) {
			struct wl_output *o = BIND(wl_output, 4);
			/* The whole slot: registry_remove() compacts with a swap, so it
			 * may still hold a departed output's `on_surface`, which only
			 * enter and leave write. */
			p->outputs[p->output_count] =
				(WlOutput){ .handle = o, .global = name };
			p->output_count++;
			wl_output_add_listener(o, &output_listener, a);
		}
	} else if (IS(wl_data_device_manager) && !p->ddm) {
		/* v1 is the selection; everything later is drag and drop. */
		p->ddm = BIND(wl_data_device_manager, 1);
	} else if (IS(zwp_text_input_manager_v3) && !p->ti_manager &&
			   !ti_refused()) {
		p->ti_manager = BIND(zwp_text_input_manager_v3, 1);
	}
	seat_extras(a);
}

#undef IS
#undef BIND

static void registry_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)r;
	App *a = d;
	struct BackendState *p = a->backend_state;
	for (int i = 0; i < p->output_count; i++) {
		if (p->outputs[i].global != name)
			continue;
		if (p->current_output == p->outputs[i].handle)
			p->current_output = NULL;   /* re-picked below, from what is left */
		release_output(p->outputs[i].handle);
		p->outputs[i] = p->outputs[--p->output_count];
		/* Unconditionally: the published names carry every output the surface
		 * is on, followed or not. */
		outputs_changed(a);
		break;
	}
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

/* Maintains the set only. A leave must not clear the current output: a dragged
 * window leaves one while still on another. */
static void surface_on_output(App *a, struct wl_output *o, bool on)
{
	WlOutput *oi = output_info_for(a, o);
	if (oi)
		oi->on_surface = on;
	outputs_changed(a);
}
static void surface_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
	(void)s;
	surface_on_output(data, o, true);
}
static void surface_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
	(void)s;
	surface_on_output(data, o, false);
}
static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter, .leave = surface_leave,
};

static void fractional_preferred_scale(void *data,
									   struct wp_fractional_scale_v1 *f,
									   uint32_t scale)
{
	(void)f;
	App *a = data;
	struct BackendState *p = a->backend_state;
	/* Bounded on the way in. Zero is the compositor declining to say, and
	 * stays zero: both readers test for it. */
	if (scale == 0)
		p->preferred_scale120 = 0;
	else if (scale > (uint32_t)SCALE120_MAX)
		p->preferred_scale120 = SCALE120_MAX;
	else
		p->preferred_scale120 = clamp_scale120((int)scale);
	update_scale(a);
}
static const struct wp_fractional_scale_v1_listener fractional_listener = {
	.preferred_scale = fractional_preferred_scale,
};

/* The compositor's answer "must be obeyed by the client", and a server-side
 * one cannot be, so it is reported once. No ack of its own: it is part of the
 * sequence xdg_surface.configure closes. */
static void decoration_configure(void *data,
								 struct zxdg_toplevel_decoration_v1 *d,
								 uint32_t mode)
{
	(void)d;
	App *a = data;
	if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
		a->backend_state->deco_ssd_warned)
		return;
	a->backend_state->deco_ssd_warned = true;
	fnl_log(FNL_LOG_WARN,
			"the compositor imposed server-side decorations; this backend "
			"only draws its own, so the titlebar is doubled");
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
	.configure = decoration_configure,
};

/* Not acked here: the commit after an ack_configure must carry a buffer of the
 * acked size, and the render thread is still drawing the previous one. Only
 * the size is latched; the render thread acks on the frame that commits it.
 * DESIGN *Configure handshake*. */
static void xdg_surface_configure(void *data, struct xdg_surface *xs,
								  uint32_t serial)
{
	App *a = data;
	struct BackendState *p = a->backend_state;
	(void)xs;

	a->configured = true;

	/* This configure's states become current here; the floating-size
	 * bookkeeping below reads them. */
	bool was_maximized  = a->is_maximized;
	bool was_fullscreen = a->is_fullscreen;
	a->is_fullscreen = p->cfg_fullscreen;
	a->is_maximized  = p->cfg_maximized;
	/* The caret was reported in surface space, grab ring included, and the
	 * ring has just come or gone. */
	if (was_fullscreen != a->is_fullscreen && p->ti_enabled)
		ti_apply(a);

	/* Zero is "pick your own": the last floating size, then the opening one. */
	int def_w, def_h;
	deco_default_window_pt(a, window_scale120(a), &def_w, &def_h);
	int w = p->cfg_w > 0 ? p->cfg_w : (a->win_w > 0 ? a->win_w : def_w);
	int h = p->cfg_h > 0 ? p->cfg_h : (a->win_h > 0 ? a->win_h : def_h);

	/* A request stands only against the configure echoing the size it
	 * replaced; see req_from_w in wayland_state.h. */
	if (p->req_w > 0 && !p->cfg_fullscreen && !p->cfg_maximized &&
		p->cfg_w == p->req_from_w && p->cfg_h == p->req_from_h) {
		w = p->req_w;
		h = p->req_h;
	}
	p->req_w = p->req_h = 0;

	/* The floor, enforced on arrival: xdg_toplevel_set_min_size() is a
	 * request, and a tiling compositor does not negotiate. Not while
	 * fullscreen or maximized, where a different buffer would not fit the
	 * output. */
	if (!a->is_fullscreen && !a->is_maximized) {
		int min_w, min_h;
		deco_min_window(a, false, &min_w, &min_h);
		if (w < min_w) w = min_w;
		if (h < min_h) h = min_h;
	}

	if (!a->is_fullscreen && !a->is_maximized) {
		a->win_w = w;
		a->win_h = h;
	}
	a->cur_w = w;
	a->cur_h = h;

	/* After cur_*: the device ceiling bounds this size times this scale.
	 * Published with the size in one locked update, so the render thread never
	 * pairs one configure's size with another's scale. */
	int s120 = resolve_scale120(a);

	fnl_mutex_lock(&a->lock);
	a->want_w = w;
	a->want_h = h;
	a->want_scale120 = s120;
	/* With the size, for the same reason. */
	if (was_fullscreen != a->is_fullscreen || was_maximized != a->is_maximized)
		a->want_urgent = true;
	a->want_fullscreen = p->cfg_fullscreen;
	a->want_active     = p->cfg_activated;
	a->want_maximized  = p->cfg_maximized;
	/* A newer configure supersedes an unanswered one: only the newest need be
	 * acked. */
	a->cfg_serial  = serial;
	a->cfg_pending = true;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);

	/* Any configure can shrink the area under the reticle. */
	app_reticle_clamp(a);
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

/* ---- frame pacing ---------------------------------------------------------
 *
 * wl_surface.frame is the throttle that survives a swapchain rebuild: FIFO
 * throttles against a swapchain's own history, and a drag-resize rebuilds on
 * every configure (DESIGN *Frame pacing and the off-screen skip*). One
 * callback is outstanding at a time: the render thread asks for the next as
 * it presents, and this thread hands it back when it fires. */

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	(void)time;
	App *a = data;
	struct BackendState *p = a->backend_state;

	fnl_mutex_lock(&a->lock);
	a->frame_cb_at = now_seconds();
	/* Destroyed here only if taken from a slot here: one in neither was
	 * displaced, and present_drop() destroys it (frame_cb_orphan). */
	bool taken = false;
	if (p->frame_cb == cb) {
		taken = true;
		p->frame_cb    = NULL;
		p->frame_ready = true;
		/* The first callback asked of a surface made at a renew: the
		 * frame it rode is on screen, and what was kept under it may go
		 * (present_settle()). */
		if (p->present_old_marked)
			p->present_old_shown = true;
		fnl_cond_signal(&a->cond);
	}
	if (p->frame_cb_orphan == cb) {
		taken = true;
		p->frame_cb_orphan = NULL;
	}
	fnl_mutex_unlock(&a->lock);
	if (taken)
		wl_callback_destroy(cb);
}

/* [render] The surface kept from before the last renew, gone; nothing
 * where there is none. */
static void present_old_drop(struct BackendState *p)
{
	if (!p->present_old)
		return;
	wp_viewport_destroy(p->present_old_viewport);
	wl_subsurface_destroy(p->present_old_sub);
	wl_surface_destroy(p->present_old);
	p->present_old_viewport = NULL;
	p->present_old_sub = NULL;
	p->present_old = NULL;
}

/* [render] Once the surface made at the last renew has shown a frame: the
 * one kept under it is dropped, and the window's own surface, where its
 * last frame was kept the same way at the first renew, takes the
 * transparent pixel it shows from then on, with the window's next commit
 * (wl_flush()). Before each frame request, which is where the render
 * thread next passes. */
static void present_settle(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->present_old && !p->backing_owed)
		return;
	fnl_mutex_lock(&a->lock);
	const bool shown = p->present_old_shown;
	if (shown)
		p->present_old_marked = p->present_old_shown = false;
	fnl_mutex_unlock(&a->lock);
	if (!shown)
		return;
	present_old_drop(p);
	if (p->backing_owed) {
		p->backing_owed = false;
		if (p->src_w > 0) {
			wp_viewport_set_source(p->viewport, wl_fixed_from_int(-1),
								   wl_fixed_from_int(-1),
								   wl_fixed_from_int(-1),
								   wl_fixed_from_int(-1));
			p->src_w = p->src_h = 0;
		}
		wl_surface_attach(p->surface, p->backing, 0, 0);
		wl_surface_damage(p->surface, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_set_opaque_region(p->surface, NULL);
		p->parent_dirty = true;
	}
}
static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

/* [render] Just before the present: the request rides on the commit
 * vkQueuePresentKHR makes.
 *
 * The listener is attached a statement after the callback exists, and a
 * `done` the main thread dispatched in between would be dropped. Left open:
 * closing it needs a wl_event_queue of the render thread's own, and the
 * compositor does not answer before it next composites. */
static void surface_frame_request(App *a)
{
	struct BackendState *p = a->backend_state;
	present_settle(a);
	fnl_mutex_lock(&a->lock);
	feedback_request(a);
	if (!p->frame_cb) {
		p->frame_ready = false;
		p->frame_cb = wl_surface_frame(p->present);
		wl_callback_add_listener(p->frame_cb, &frame_listener, a);
		/* Asked of a surface made at a renew, whose first frame this is:
		 * answered, that frame is on screen (frame_done()). */
		if (p->present_old || p->backing_owed)
			p->present_old_marked = true;
	}
	fnl_mutex_unlock(&a->lock);
	p->presenting = true;
}

/* [render] `break_on_urgent`: a configure into or out of fullscreen or
 * maximize makes the compositor hold its frame, and the invitation with it,
 * until it is answered with a buffer -- a stand-off only the timeout would
 * end. */
static bool surface_frame_wait(App *a, bool break_on_urgent)
{
	struct BackendState *p = a->backend_state;
	bool invited = true;
	/* What is left, not what it started as: a wake that does not satisfy the
	 * predicate must not restart the timeout. */
	double left = FRAME_WAIT_S;

	fnl_mutex_lock(&a->lock);
	/* With no frame_cb outstanding there is nothing to wait for, which is what
	 * carries the first frame of all. */
	while (!p->frame_ready && p->frame_cb && !a->quit) {
		if (break_on_urgent && a->want_urgent)
			break;                 /* the compositor is waiting on us */
		const double t0 = now_seconds();
		if (!fnl_cond_wait_for(&a->cond, &a->lock, left)) {
			invited = false;       /* nobody is watching */
			break;
		}
		left -= now_seconds() - t0;
		if (left <= 0.0) {
			/* Spent, but that is still frame_ready's to say: an invitation
			 * that landed on the wake that used the last of it is one. */
			invited = p->frame_ready;
			break;
		}
	}
	p->frame_ready = false;
	fnl_mutex_unlock(&a->lock);
	return invited;
}

/* [render] The present came back out of date without committing, so the
 * invitation just asked for is not coming before the next commit. Counted as
 * answered, or the next frame waits out the whole timeout. */
static void surface_frame_cancel(App *a)
{
	fnl_mutex_lock(&a->lock);
	/* The present never committed, so this frame's feedback rides on the
	 * next commit, another frame's: it stamps nothing. Unless the
	 * compositor said already that this frame was shown -- a present the
	 * driver refused and committed all the same, as NVIDIA's X11 one does
	 * -- and then it was, and is counted (app_frame_take_back()), so that
	 * the next frame does not draw under its index. Mesa's Wayland WSI
	 * refuses before it commits, and on NVIDIA's here no present was
	 * refused at all (aspire's weston, 2026-11-02). */
	struct BackendState *p = a->backend_state;
	if (p->feedback_cur >= 0) {
		WlFeedback *f = &p->feedback[p->feedback_cur];
		if (!f->live && f->shown && f->index == a->presents)
			a->present_answered_anyway = true;
		else
			f->cancelled = true;
	}
	p->feedback_cur = -1;
	a->backend_state->frame_ready = true;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

/* [render] A configure that landed while this frame was being drawn stays
 * pending: it wants a different size, and gets its own frame. */
static void surface_ack_configure(App *a, uint32_t serial)
{
	struct BackendState *p = a->backend_state;
	xdg_surface_ack_configure(p->xdg_surface, serial);
	if (p->present != p->surface)
		p->parent_dirty = true;

	fnl_mutex_lock(&a->lock);
	if (a->cfg_serial == serial)
		a->cfg_pending = false;
	fnl_mutex_unlock(&a->lock);
}

/* A size out of range becomes zero, xdg-shell's own "pick your own", each axis
 * on its own. */
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
								   int32_t w, int32_t h,
								   struct wl_array *states)
{
	(void)tl;
	App *a = data;
	struct BackendState *p = a->backend_state;
	p->cfg_w = wire_pt(w);
	p->cfg_h = wire_pt(h);

	bool fs = false, mx = false, act = false;
	enum xdg_toplevel_state *st;
	wl_array_for_each(st, states) {
		if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) fs = true;
		if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED)  mx = true;
		if (*st == XDG_TOPLEVEL_STATE_ACTIVATED)  act = true;
	}
	/* Pending, like the size: states become current when their configure is
	 * answered. Applied here they would reach the render thread ahead of the
	 * size. */
	p->cfg_fullscreen = fs;
	p->cfg_maximized  = mx;
	p->cfg_activated  = act;
}
static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl)
{
	(void)tl;
	app_close_requested(data);
}
/* May change at any time. Zero, or a figure out of range, is no figure, and
 * the output's mode stands in. */
static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
										  int32_t w, int32_t h)
{
	(void)t;
	App *a = data;
	a->backend_state->bounds_w = wire_pt(w);
	a->backend_state->bounds_h = wire_pt(h);
	refresh_size_bounds(a);
}
static void xdg_toplevel_wm_capabilities(void *d, struct xdg_toplevel *t,
										 struct wl_array *c)
{ (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
	.configure_bounds = xdg_toplevel_configure_bounds,
	.wm_capabilities = xdg_toplevel_wm_capabilities,
};

/* A window size the app picks for itself, in points. Nothing is sent: the next
 * commit carries a buffer of that size and no configure follows to correct it,
 * which is why app_request_size() has to refuse a fullscreen or maximized
 * window. */
static void request_window_size(App *a, int w, int h)
{
	struct BackendState *p = a->backend_state;
	/* Before cur_* is overwritten: the size the compositor still believes. */
	p->req_from_w = a->cur_w;
	p->req_from_h = a->cur_h;
	p->req_w = w;
	p->req_h = h;
	a->win_w = w;
	a->win_h = h;
	a->cur_w = w;
	a->cur_h = h;

	fnl_mutex_lock(&a->lock);
	a->want_w = w;
	a->want_h = h;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

/* No output is named: the compositor picks the one the window is mostly on,
 * and restores it there. current_output is chosen for sharpness and must not
 * decide this. */
static void wl_set_fullscreen(App *a, bool on)
{
	if (on)
		xdg_toplevel_set_fullscreen(a->backend_state->xdg_toplevel, NULL);
	else
		xdg_toplevel_unset_fullscreen(a->backend_state->xdg_toplevel);
}

/* A Wayland client places no window: the one output it can name is the
 * one a fullscreen window fills. So a fullscreen window goes there, and
 * one that is not stays where it is, with a line saying so; the
 * titlebar's box is dim here (outputs_fullscreen_only), and this is
 * fnl_request_output()'s alone. Where the window comes back to on leaving
 * fullscreen is the compositor's to say. */
static void wl_set_output(App *a, const char *name)
{
	struct BackendState *p = a->backend_state;
	for (int i = 0; i < p->output_count; i++) {
		if (!p->outputs[i].name[0] || strcmp(p->outputs[i].name, name) != 0)
			continue;
		if (a->is_fullscreen)
			xdg_toplevel_set_fullscreen(p->xdg_toplevel,
										p->outputs[i].handle);
		else
			fnl_log(FNL_LOG_INFO, "the compositor places the window: only a "
					"fullscreen one moves to %s", name);
		return;
	}
}

static void wl_set_maximized(App *a, bool on)
{
	if (on)
		xdg_toplevel_set_maximized(a->backend_state->xdg_toplevel);
	else
		xdg_toplevel_unset_maximized(a->backend_state->xdg_toplevel);
}

static void wl_minimize(App *a)
{
	xdg_toplevel_set_minimized(a->backend_state->xdg_toplevel);
}

/* The optional globals left unbound, said once and only when there are any:
 * each has a fallback, and the log is how a run on a compositor that lacks
 * one knows the fallback is what it drove. Not text input where
 * FNL_NO_INPUT_METHOD declined it. */
static void log_unbound_globals(const struct BackendState *p)
{
	const struct { bool bound; const char *name; } globals[] = {
		{ p->cursor_shape_manager != NULL, "cursor-shape" },
		{ p->decoration_manager   != NULL, "xdg-decoration" },
		{ p->relative_manager     != NULL, "relative-pointer" },
		{ p->pointer_constraints  != NULL, "pointer-constraints" },
		{ p->viewporter           != NULL, "viewporter" },
		{ p->presentation         != NULL, "presentation-time" },
		{ p->fractional_manager   != NULL, "fractional-scale" },
		{ p->ddm                  != NULL, "data-device" },
		{ p->ti_manager != NULL || ti_refused(), "text-input-v3" },
		{ p->subcompositor        != NULL, "subcompositor" },
		{ p->shm                  != NULL, "shm" },
	};
	char list[192] = "";
	size_t used = 0;
	for (size_t i = 0; i < sizeof globals / sizeof globals[0]; i++) {
		if (globals[i].bound)
			continue;
		const int n = snprintf(list + used, sizeof list - used, "%s%s",
							   used ? ", " : "", globals[i].name);
		if (n > 0 && (size_t)n < sizeof list - used)
			used += (size_t)n;
	}
	if (used)
		fnl_log(FNL_LOG_INFO, "not bound, so their fallbacks are in use: %s",
				list);
}

static FNL_MUST_USE FnlResult create_window(App *a)
{
	struct BackendState *p = a->backend_state;
	p->display = wl_display_connect(NULL);
	if (!p->display)
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"no Wayland display (is WAYLAND_DISPLAY set?)");
	p->registry = wl_display_get_registry(p->display);
	wl_registry_add_listener(p->registry, &registry_listener, a);
	/* Two roundtrips: the first delivers the globals, the second the events
	 * (seat caps, output scale) emitted when we bound them. */
	wl_display_roundtrip(p->display);
	wl_display_roundtrip(p->display);

	if (!p->compositor || !p->wm_base)
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"compositor lacks wl_compositor / xdg_wm_base");
	log_unbound_globals(p);

	p->surface = wl_compositor_create_surface(p->compositor);
	wl_surface_add_listener(p->surface, &surface_listener, a);
	p->present = p->surface;

	if (p->viewporter)
		p->viewport = wp_viewporter_get_viewport(p->viewporter, p->surface);
	/* Before the render thread starts, which reads it. */
	a->buffer_crops = p->viewport != NULL;
	if (p->fractional_manager && p->viewport) {
		p->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
			p->fractional_manager, p->surface);
		wp_fractional_scale_v1_add_listener(p->fractional,
											&fractional_listener, a);
	}

	p->xdg_surface = xdg_wm_base_get_xdg_surface(p->wm_base, p->surface);
	xdg_surface_add_listener(p->xdg_surface, &xdg_surface_listener, a);
	p->xdg_toplevel = xdg_surface_get_toplevel(p->xdg_surface);
	xdg_toplevel_add_listener(p->xdg_toplevel, &xdg_toplevel_listener, a);
	/* The string the titlebar draws, so the two cannot disagree. */
	xdg_toplevel_set_title(p->xdg_toplevel, a->title);
	/* A desktop file's name, which a title cannot be turned into. No
	 * descriptor field carries one, so this is the library's own. */
	xdg_toplevel_set_app_id(p->xdg_toplevel, "fenestal");
	/* The titlebar's own floor. A request, so enforced again on arrival. */
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	xdg_toplevel_set_min_size(p->xdg_toplevel, min_w, min_h);

	/* Ask for client-side and listen: the compositor's answer is what counts.
	 * Without the extension xdg-shell's default is client-side. */
	if (p->decoration_manager) {
		p->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
			p->decoration_manager, p->xdg_toplevel);
		zxdg_toplevel_decoration_v1_add_listener(p->decoration,
												 &decoration_listener, a);
		zxdg_toplevel_decoration_v1_set_mode(
			p->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	}

	/* Before any configure the scale is 1.0 in practice; the first one
	 * resizes. */
	deco_default_window_pt(a, window_scale120(a), &a->win_w, &a->win_h);

	wl_surface_commit(p->surface);
	while (!a->configured && wl_display_dispatch(p->display) != -1)
		;
	if (!a->configured)
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"the compositor never configured the window");
	return FNL_OK;
}

/* ---- the backend seam ---------------------------------------------------- */

/* After create_window(), which opens the connection. The set is
 * level-triggered and has to be: wl_display_read_events() does one read, not a
 * drain to EAGAIN. */
static FNL_MUST_USE FnlResult create_epoll(App *a)
{
	FNL_TRY(loop_epoll_create(&a->backend_state->epoll_fd,
							  wl_display_get_fd(a->backend_state->display),
							  a->backend_state->wake_fd));

	/* The one descriptor allowed to be missing: a held key then types once.
	 * Non-blocking, because a timer disarmed after epoll reported it has
	 * nothing to read. */
	a->backend_state->repeat_fd = timerfd_create(CLOCK_MONOTONIC,
										TFD_NONBLOCK | TFD_CLOEXEC);
	if (a->backend_state->repeat_fd >= 0) {
		struct epoll_event ev = { .events = EPOLLIN,
								  .data.fd = a->backend_state->repeat_fd };
		if (epoll_ctl(a->backend_state->epoll_fd, EPOLL_CTL_ADD,
					  a->backend_state->repeat_fd, &ev) < 0) {
			close(a->backend_state->repeat_fd);
			a->backend_state->repeat_fd = -1;
		}
	}
	if (a->backend_state->repeat_fd < 0)
		fnl_log(FNL_LOG_WARN, "no timer for key repeat; a held key will type "
				"once");
	return FNL_OK;
}

/* Without a viewport the only scale expressible is the integer buffer scale,
 * and the buffer must be an exact multiple of it. With one, any size goes. */
static uint32_t wl_buffer_granularity(const App *a, int s120)
{
	if (a->backend_state->viewport || s120 <= 120)
		return 1;
	return (uint32_t)(s120 / 120);
}

/* [main] From the drain; the pump's turn flushes it. */
static void wl_set_title(App *a, const char *utf8)
{
	if (a->backend_state->xdg_toplevel)
		xdg_toplevel_set_title(a->backend_state->xdg_toplevel, utf8);
}

static FnlResult wl_open(App *a)
{
	/* Before anything is built, so that a machine without the library is
	 * refused as a session without a compositor is, with nothing shown, and
	 * fnl_init() steps down to the next backend. a->backend_state stays
	 * NULL, which wl_close() takes for nothing to release. */
	const char *missing = wl_dyn_load();
	if (missing)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "%s", missing);

	struct BackendState *p = calloc(1, sizeof *p);
	if (!p)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "out of memory for the "
											   "Wayland backend state");
	a->backend_state = p;
	/* Not the calloc's zero, which is stdin: wl_close() closes whatever is not
	 * negative. */
	p->wake_fd = p->epoll_fd = p->repeat_fd = -1;
	p->clip_fd = -1;
	p->feedback_cur = -1;
	snprintf(p->clip_marker, sizeof p->clip_marker,
			 "application/x-fenestal-owner-%ld", (long)getpid());
	p->repeat_rate  = REPEAT_RATE_DEFAULT;
	p->repeat_delay = REPEAT_DELAY_DEFAULT;

	/* The wakeup first. Any failure from here unwinds through wl_close(),
	 * which copes with every prefix of what this builds. */
	FNL_TRY(loop_wake_create(&p->wake_fd));
	FNL_TRY(create_window(a));
	FNL_TRY(create_epoll(a));
	return FNL_OK;
}

static void wl_wakeup(App *a)
{
	/* Reachable from a signal handler at any point, and wl_close() leaves NULL
	 * behind. */
	if (a->backend_state)
		loop_wake(a->backend_state->wake_fd);
}

/* A flush that found the socket full leaves what this thread buffered -- a
 * title, a size floor -- for the next wake, which may be a long way off; so
 * the display is watched for room until a flush gets through. Armed only
 * then: the set is level-triggered, and EPOLLOUT on a socket with room would
 * end every wait at once. A refused epoll_ctl leaves it as it was, and the
 * requests wait for the next wake as before. */
static void display_watch_out(struct BackendState *p, bool on)
{
	if (p->display_out == on)
		return;
	const int fd = wl_display_get_fd(p->display);
	struct epoll_event ev = { .events = EPOLLIN | (on ? EPOLLOUT : 0u),
							  .data.fd = fd };
	if (epoll_ctl(p->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0)
		p->display_out = on;
}

/* prepare_read/read_events is the form that lets the render thread call
 * wl_display_flush concurrently without losing a wakeup. */
static void wl_pump(App *a)
{
	struct BackendState *p = a->backend_state;
	struct wl_display *d = p->display;

	/* prepare_read refuses while the queue holds events, and dispatch_pending
	 * stops draining once the display has latched an error: return without
	 * arming a read, and let the caller's error check end the loop. */
	while (wl_display_prepare_read(d) != 0)
		if (wl_display_dispatch_pending(d) < 0)
			return;
	display_watch_out(p, wl_display_flush(d) < 0 && errno == EAGAIN);

	/* For ever, unless a paste's owner owes its next piece. */
	const int wait_ms = loop_wait_ms(p->clip_fd >= 0, p->clip.deadline);
	struct epoll_event evs[4];
	int n = epoll_wait(p->epoll_fd, evs, 4, wait_ms);

	/* n < 0 (EINTR) leaves all of these clear, and the read is cancelled. */
	uint32_t display_ev = 0;
	bool     woken      = false;
	bool     repeat     = false;
	bool     paste      = false;
	for (int i = 0; i < n; i++) {
		if (evs[i].data.fd == p->wake_fd)
			woken = true;
		else if (evs[i].data.fd == p->repeat_fd)
			repeat = true;
		else if (evs[i].data.fd == p->clip_fd)
			paste = true;
		else
			display_ev |= evs[i].events;
	}

	/* On hangup and error too: wl_display_read_events() then latches the
	 * display error, which ends the main loop. A bare cancel_read would leave
	 * epoll_wait returning the same events for ever. Room alone (EPOLLOUT) is
	 * a cancel: the next turn's flush is what it was for. */
	if (display_ev & (EPOLLIN | EPOLLERR | EPOLLHUP))
		wl_display_read_events(d);
	else
		wl_display_cancel_read(d);   /* woken by the eventfd, or epoll failed */

	if (woken)
		loop_wake_drain(p->wake_fd);

	wl_display_dispatch_pending(d);

	/* After the dispatch: a release in this same turn has then stopped the
	 * timer. */
	if (repeat)
		key_repeat_fire(a);
	/* Guarded again: a dispatch above may have finished or dropped it. */
	if (paste && p->clip_fd >= 0)
		clip_readable(a);
	if (p->clip_fd >= 0 && clip_in_expired(&p->clip))
		clip_finish(a, false);
}

/* [render] Safe against the pump's prepare_read and dispatch. After the
 * present, where the frames go into a subsurface, the window's own state is
 * committed too: the frame is the subsurface's and already shown, so the
 * ack that answers it and the geometry that describes it follow it. */
static void wl_flush(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->presenting) {
		p->presenting = false;
		if (p->parent_dirty) {
			p->parent_dirty = false;
			wl_surface_commit(p->surface);
		}
	}
	wl_display_flush(p->display);
}

/* ---- a surface of their own for the frames -------------------------------
 *
 * A surface one GPU has presented into is not always one another can: on
 * GNOME Shell 50.1 a window moved from RADV or radeonsi to lavapipe keeps
 * the last hardware frame for the rest of its life -- every lavapipe frame
 * reported presented and none shown, and the way back to RADV not shown
 * either -- while lavapipe on a new window draws, and so does the same move
 * with Mesa's commit timing off (MESA_VK_WSI_DEBUG=nowlts). So from the
 * first move on the frames go into a subsurface, made anew at each move, as
 * Win32's go into a child window (FnlBackendVtbl::present_window_renew).
 *
 * The window's surface keeps everything else. Its buffer is one transparent
 * pixel stretched over the surface by its viewport, so the input, the
 * outputs it is on, the pointer lock and the fractional scale stay its, and
 * the subsurface takes no input. Desynchronized: the driver commits the
 * subsurface on its own, FIFO barriers and all, which a synchronized one
 * would hold for a commit of the window's. Without a viewporter the pixel
 * cannot cover the window, and the frames stay on the window's surface. */

/* One transparent pixel, ARGB8888 -- the format every compositor takes. */
static struct wl_buffer *backing_buffer(struct BackendState *p)
{
	const int fd = memfd_create("fenestal-backing", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct wl_buffer *b = NULL;
	if (ftruncate(fd, 4) == 0) {
		struct wl_shm_pool *pool = wl_shm_create_pool(p->shm, fd, 4);
		b = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4,
									  WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
	}
	close(fd);
	return b;
}

/* The subsurface the frames went into, kept under the one made next with
 * the last frame it showed -- the compositor keeps a buffer its client
 * destroyed on screen, measured on GNOME (above) -- until that one has
 * shown a frame (present_settle()). Where it has shown none -- a second
 * renew in one move, a surface or a device that would not build and the
 * way back -- the one kept before it is still the last frame on screen,
 * and stays; the empty one goes. The window's own surface is named again.
 * A frame callback asked of the surface left is answered as if it had
 * come, or the next frame would wait it out; the proxy is left to
 * frame_done() and close(), and one left there before it is destroyed
 * here (BackendState::frame_cb_orphan). */
static void present_drop(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->present == p->surface)
		return;
	fnl_mutex_lock(&a->lock);
	/* The first renew keeps the window's own surface rather than one in
	 * present_old, and owes it the backing until this one has shown. */
	const bool unshown = (p->present_old || p->backing_owed) &&
						 !p->present_old_shown;
	struct wl_callback *displaced = NULL;
	if (p->frame_cb) {
		displaced = p->frame_cb_orphan;
		p->frame_cb_orphan = p->frame_cb;
		p->frame_cb = NULL;
		p->frame_ready = true;
		fnl_cond_signal(&a->cond);
	}
	p->present_old_marked = p->present_old_shown = false;
	fnl_mutex_unlock(&a->lock);
	if (displaced)
		wl_callback_destroy(displaced);
	if (unshown) {
		wp_viewport_destroy(p->present_viewport);
		wl_subsurface_destroy(p->present_sub);
		wl_surface_destroy(p->present);
	} else {
		present_old_drop(p);
		p->present_old_viewport = p->present_viewport;
		p->present_old_sub = p->present_sub;
		p->present_old = p->present;
	}
	p->present_viewport = NULL;
	p->present_sub = NULL;
	p->present = p->surface;
}

/* [render] Between the old renderer's surface and the new one's. */
static void wl_present_window_renew(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->viewport || !p->subcompositor || !p->shm) {
		if (!p->renew_refused)
			fnl_log(FNL_LOG_INFO, "no %s here, so the frames of every GPU "
					"go into the window's own surface",
					!p->viewport        ? "viewporter"
					: !p->subcompositor ? "subcompositor"
										: "shm");
		p->renew_refused = true;
		return;
	}
	if (!p->backing) {
		p->backing = backing_buffer(p);
		if (!p->backing) {
			fnl_log(FNL_LOG_WARN, "no buffer for the window's own surface "
					"(%s), so the frames stay on it", strerror(errno));
			return;
		}
		/* Attached once the first subsurface has shown a frame
		 * (present_settle()), not now: until then the window's own last
		 * frame stays on its surface, under that subsurface. */
		p->backing_owed = true;
	}
	present_drop(a);

	struct wl_surface *s = wl_compositor_create_surface(p->compositor);
	struct wl_region *none = wl_compositor_create_region(p->compositor);
	wl_surface_set_input_region(s, none);
	wl_region_destroy(none);
	p->present_sub = wl_subcompositor_get_subsurface(p->subcompositor, s,
													 p->surface);
	wl_subsurface_set_desync(p->present_sub);
	p->present_viewport = wp_viewporter_get_viewport(p->viewporter, s);
	p->present_src_w = p->present_src_h = 0;
	if (p->applied_vp_w > 0)
		wp_viewport_set_destination(p->present_viewport, p->applied_vp_w,
									p->applied_vp_h);
	p->present = s;
	if (a->applied_geo_w > 0)
		present_opaque(a, a->applied_geo_w, a->applied_geo_h,
					   a->applied_geo_m);
	/* The subsurface joins the window with the window's next commit. */
	p->parent_dirty = false;
	wl_surface_commit(p->surface);
	wl_display_flush(p->display);
}

/* [render] The frames' surface shows the top-left `extent` of a buffer
 * larger than it (App::buffer), and the whole buffer otherwise. In buffer
 * pixels, which beside a viewport are the surface's with no buffer scale.
 * Sent just before the present, whose commit carries the buffer, so the
 * rectangle never meets a smaller one. The window's own surface, once it
 * shows the backing pixel instead, has its rectangle taken off with that
 * attach (present_settle()). */
static bool crop_apply(App *a)
{
	struct BackendState *p = a->backend_state;
	const bool own = p->present == p->surface;
	struct wp_viewport *vp = own ? p->viewport : p->present_viewport;
	int *aw = own ? &p->src_w : &p->present_src_w;
	int *ah = own ? &p->src_h : &p->present_src_h;
	const int w = a->buffer.w > 0 ? a->extent.w : 0;
	const int h = a->buffer.w > 0 ? a->extent.h : 0;
	if (!vp || (w == *aw && h == *ah))
		return false;
	if (w > 0)
		wp_viewport_set_source(vp, 0, 0, wl_fixed_from_int(w),
							   wl_fixed_from_int(h));
	else
		wp_viewport_set_source(vp, wl_fixed_from_int(-1),
							   wl_fixed_from_int(-1), wl_fixed_from_int(-1),
							   wl_fixed_from_int(-1));
	*aw = w;
	*ah = h;
	return true;
}

/* [render] Double-buffered either way: it takes effect with the commit that
 * carries the buffer it describes. */
static bool wl_apply_scale(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->viewport) {
		bool changed = crop_apply(a);
		/* Onto the *surface*: the window plus the grab ring. */
		int vw = a->draw_w + 2 * a->draw_margin;
		int vh = a->draw_h + 2 * a->draw_margin;
		if (vw == p->applied_vp_w && vh == p->applied_vp_h)
			return changed;
		wp_viewport_set_destination(p->viewport, vw, vh);
		/* And the frames' own, where they have a surface of their own: the
		 * window's pixel and the frame cover the same surface. */
		if (p->present_viewport) {
			wp_viewport_set_destination(p->present_viewport, vw, vh);
			p->parent_dirty = true;
		}
		p->applied_vp_w = vw;
		p->applied_vp_h = vh;
		return true;
	}

	if (a->draw_scale120 == a->applied_scale120)
		return false;
	/* clamp_buffer_scale120() made this a whole multiple of 120, and 120
	 * itself where there is no set_buffer_scale. */
	wl_surface_set_buffer_scale(p->surface, a->draw_scale120 / 120);
	a->applied_scale120 = a->draw_scale120;
	return true;
}

static FnlResult wl_check_error(App *a)
{
	struct BackendState *p = a->backend_state;
	int err = wl_display_get_error(p->display);
	if (!err)
		return FNL_OK;

	uint32_t id = 0;
	const struct wl_interface *iface = NULL;
	uint32_t code = wl_display_get_protocol_error(p->display, &iface, &id);
	return fnl_fail(FNL_ERR_NO_DISPLAY,
					"wayland error %d: %s code %u on object %u", err,
					iface ? iface->name : "(none)", code, id);
}

/* Innermost first, and every step conditional: this is also the failure path
 * out of wl_open(), so it runs against every prefix of what that builds. */
static void wl_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p)
		return;      /* open() failed before it had anything to release */

	release_pointer_capture(a);

	if (p->ti)
		zwp_text_input_v3_destroy(p->ti);
	if (p->ti_manager)
		zwp_text_input_manager_v3_destroy(p->ti_manager);
	ti_drop(p);
	if (p->clip_source)
		wl_data_source_destroy(p->clip_source);
	if (p->clip_sync)
		wl_callback_destroy(p->clip_sync);
	if (p->offer_new)
		wl_data_offer_destroy(p->offer_new);
	if (p->offer_sel)
		wl_data_offer_destroy(p->offer_sel);
	if (p->dd)
		wl_data_device_destroy(p->dd);
	if (p->ddm)
		wl_data_device_manager_destroy(p->ddm);
	if (p->clip_fd >= 0)
		close(p->clip_fd);
	clip_in_free(&p->clip);

	if (p->decoration)
		zxdg_toplevel_decoration_v1_destroy(p->decoration);
	if (p->decoration_manager)
		zxdg_decoration_manager_v1_destroy(p->decoration_manager);
	if (p->pointer_constraints)
		zwp_pointer_constraints_v1_destroy(p->pointer_constraints);
	if (p->relative_manager)
		zwp_relative_pointer_manager_v1_destroy(p->relative_manager);
	/* The frames' subsurfaces before the surface they are over. */
	present_old_drop(p);
	if (p->present && p->present != p->surface) {
		wp_viewport_destroy(p->present_viewport);
		wl_subsurface_destroy(p->present_sub);
		wl_surface_destroy(p->present);
	}
	if (p->frame_cb)
		wl_callback_destroy(p->frame_cb);
	if (p->frame_cb_orphan)
		wl_callback_destroy(p->frame_cb_orphan);
	if (p->backing)
		wl_buffer_destroy(p->backing);
	if (p->shm)
		wl_shm_destroy(p->shm);
	if (p->subcompositor)
		wl_subcompositor_destroy(p->subcompositor);
	if (p->fractional)
		wp_fractional_scale_v1_destroy(p->fractional);
	if (p->fractional_manager)
		wp_fractional_scale_manager_v1_destroy(p->fractional_manager);
	if (p->viewport)
		wp_viewport_destroy(p->viewport);
	if (p->viewporter)
		wp_viewporter_destroy(p->viewporter);
	for (int i = 0; i < WL_FEEDBACK_SLOTS; i++)
		if (p->feedback[i].fb)
			wp_presentation_feedback_destroy(p->feedback[i].fb);
	if (p->presentation)
		wp_presentation_destroy(p->presentation);
	if (p->xdg_toplevel)
		xdg_toplevel_destroy(p->xdg_toplevel);
	if (p->xdg_surface)
		xdg_surface_destroy(p->xdg_surface);
	if (p->surface)
		wl_surface_destroy(p->surface);
	if (p->cursor_device)
		wp_cursor_shape_device_v1_destroy(p->cursor_device);
	if (p->cursor_shape_manager)
		wp_cursor_shape_manager_v1_destroy(p->cursor_shape_manager);
	if (p->pointer)
		release_pointer(p->pointer);
	if (p->keyboard)
		release_keyboard(p->keyboard);
	if (p->seat)
		wl_seat_destroy(p->seat);
	for (int i = 0; i < p->output_count; i++)
		release_output(p->outputs[i].handle);
	if (p->wm_base)
		xdg_wm_base_destroy(p->wm_base);
	if (p->compositor)
		wl_compositor_destroy(p->compositor);
	if (p->registry)
		wl_registry_destroy(p->registry);
	if (p->display)
		wl_display_disconnect(p->display);

	/* The wakeup and epoll descriptors close unless a signal handler could
	 * still be on its way to them: one that loaded the App pointer on some
	 * other thread before teardown cleared it must not write() to a closed --
	 * or recycled -- descriptor number. app_teardown() waits every handler
	 * out and then clears installed_signals; only a handler stopped where it
	 * stands, past the wait's bound, leaves the two open until exit. */
	if (!a->installed_signals) {
		if (p->epoll_fd >= 0) close(p->epoll_fd);
		if (p->wake_fd  >= 0) close(p->wake_fd);
	}
	/* The repeat timer is the pump's alone, so it closes either way. */
	if (p->repeat_fd >= 0) close(p->repeat_fd);
	p->epoll_fd = p->wake_fd = p->repeat_fd = -1;

	free(p);
	a->backend_state = NULL;
}

/* A Wayland client is not told whether it is shown: the compositor stops
 * inviting frames, and surface_frame_wait() timing out is that fact arriving.
 * So this says yes. */
static bool wl_window_on_screen(const App *a)
{
	(void)a;
	return true;
}

/* EDGE_NONE maps to the protocol's zero, "the compositor picks", which
 * wl_begin_resize() never passes. */
static const uint32_t WL_EDGE[EDGE_COUNT] = {
	[EDGE_NONE] = XDG_TOPLEVEL_RESIZE_EDGE_NONE,
	[EDGE_T]    = XDG_TOPLEVEL_RESIZE_EDGE_TOP,
	[EDGE_B]    = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
	[EDGE_L]    = XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
	[EDGE_R]    = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
	[EDGE_TL]   = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,
	[EDGE_TR]   = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
	[EDGE_BL]   = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT,
	[EDGE_BR]   = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
};

/* The serial of the press being handed over: the left button's while it is
 * held, and the last press of any button otherwise. A compositor checks the
 * serial against the press that began the pointer's grab, and a second button
 * pressed inside a titlebar press's drag slop must not replace it. */
static uint32_t grab_serial(const struct BackendState *p)
{
	return p->left_held ? p->left_serial : p->button_serial;
}

static void wl_begin_move(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->xdg_toplevel || !p->seat)
		return;
	xdg_toplevel_move(p->xdg_toplevel, p->seat, grab_serial(p));
}

static void wl_begin_resize(App *a, Edge edge)
{
	struct BackendState *p = a->backend_state;
	if (!p->xdg_toplevel || !p->seat || edge <= EDGE_NONE ||
		edge >= EDGE_COUNT)
		return;
	xdg_toplevel_resize(p->xdg_toplevel, p->seat, grab_serial(p),
						WL_EDGE[edge]);
}

/* Double-buffered: the render thread's next commit applies it, so there is
 * nothing to flush. */
static void wl_apply_size_bounds(App *a)
{
	if (!a->backend_state->xdg_toplevel)
		return;
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	xdg_toplevel_set_min_size(a->backend_state->xdg_toplevel, min_w, min_h);
}

/* A session with neither variable is not taken for a Wayland one, although
 * wl_display_connect(NULL) would try "wayland-0" all the same: that guess
 * reaches the desktop's compositor from an ssh session or a console, which
 * have no business opening a window on it unasked. Named, it is still
 * tried (FNL_BACKEND=wayland). */
static const char *wl_absent(void)
{
	const char *d = getenv("WAYLAND_DISPLAY");
	const char *s = getenv("WAYLAND_SOCKET");
	return (d && d[0]) || (s && s[0]) ? NULL : "no WAYLAND_DISPLAY";
}

const FnlBackendVtbl fnl_backend_wayland = {
	.name             = "Wayland",
	.id               = BACKEND_WAYLAND,
	.key              = "wayland",
	.absent           = wl_absent,
	.host             = backend_noop_host,
	.shared_driver    = backend_noop_shared_driver,
	.gpu_drivers      = backend_noop_gpu_drivers,
	.gpu_driver_set   = backend_noop_gpu_driver_set,
	.gpu_driver_held  = backend_noop_gpu_driver_held,
	/* A Wayland client owns its resize edges, so the window carries a
	 * transparent ring outside its geometry to be grabbed by. */
	.grab_ring        = DECO_GRAB,
	/* wl_surface.frame; surface_frame_wait() blocks on it. */
	.invites          = true,
	/* xdg_toplevel.set_fullscreen names an output; nothing else does. */
	.outputs_fullscreen_only = true,
	.open             = wl_open,
	.close            = wl_close,
	.pump             = wl_pump,
	.wakeup           = wl_wakeup,
	.check_error      = wl_check_error,
	.flush            = wl_flush,
	.present_pixels = backend_noop_present_pixels,
	.set_geometry     = surface_set_geometry,
	.ack_configure    = surface_ack_configure,
	.frame_request    = surface_frame_request,
	.frame_cancel     = surface_frame_cancel,
	.present_window_renew = wl_present_window_renew,
	.present_window_hold  = backend_noop,
	.frame_wait       = surface_frame_wait,
	.window_on_screen = wl_window_on_screen,
	.apply_scale      = wl_apply_scale,
	.request_size     = request_window_size,
	.nudge_size       = backend_noop,
	.set_fullscreen   = wl_set_fullscreen,
	.set_maximized    = wl_set_maximized,
	.minimize         = wl_minimize,
	.modes            = backend_noop_modes,
	.outputs          = backend_published_outputs,
	.set_mode         = backend_noop_set_mode,
	.set_output       = wl_set_output,
	.begin_move       = wl_begin_move,
	.begin_resize     = wl_begin_resize,
	.apply_size_bounds = wl_apply_size_bounds,
	.buffer_granularity = wl_buffer_granularity,
	.update_scale     = update_scale,
	.capture_pointer  = capture_pointer,
	.release_pointer  = release_pointer_capture,
	.cursor_changed   = wl_cursor_changed,
	.text_input_changed = wl_text_input_changed,
	.clipboard_set      = wl_clipboard_set,
	.clipboard_request  = wl_clipboard_request,
	.set_title          = wl_set_title,
};
