/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The library's lifetime: bring up the window and the device, start the render
 * thread, pump the backend's events until the window closes, tear it down.
 *
 * None of it exits: every failure path unwinds to fnl_init(), which gives back
 * a code and leaves nothing half-built. The one #ifdef is about signals,
 * which are an operating system's rather than a window server's.
 *
 * Threading: the calling thread owns the backend's connection and input; the
 * render thread owns everything the graphics API has. They share only the
 * state guarded by App::lock (see app.h).
 */

#include "core/app.h"

#include <signal.h>

/* ---- signals --------------------------------------------------------------
 *
 * Opt-in: a host with its own SIGINT handling would find this had replaced
 * it. What it buys is that a terminating signal becomes the ordinary shutdown
 * path, which releases a captured pointer.
 *
 * Everything the handler touches is allowed there: lock-free atomics (C11
 * restricts a handler to volatile sig_atomic_t and those) and a write() to
 * the wakeup fd. errno is put back because that write() can fail -- EAGAIN
 * on a saturated counter -- inside whatever call the signal interrupted.
 *
 * signal_busy counts the handlers between their load of signal_app and their
 * last use of it. A signal is delivered to any thread, so clearing the
 * pointer does not stop a handler that loaded it a moment before from
 * reaching the App after teardown frees it; teardown waits for the count to
 * fall to zero after the clear. Incremented before the load, so a handler
 * that has not been counted yet will load NULL. */
static FnlAtomicPtr signal_app;
static FnlAtomicU32 signal_busy;

static void on_terminate(int sig)
{
	(void)sig;
	const int saved_errno = errno;
	fnl_atomic_add_u32(&signal_busy, 1);
	App *a = fnl_atomic_load_ptr(&signal_app);
	if (a) {
		fnl_atomic_store_bool(&a->should_close, true);
		app_wakeup(a);
	}
	fnl_atomic_add_u32(&signal_busy, UINT32_MAX);
	errno = saved_errno;
}

/* After signal_app is cleared: no handler entering from here on can reach an
 * App, and one already inside finishes in a few instructions. It cannot
 * deadlock against a handler on this thread, which runs to completion before
 * the loop resumes.
 *
 * Bounded, because a handler's thread can be stopped where it stands -- by a
 * debugger, or SIGSTOP -- and teardown must not hang on it for ever. A second
 * is millions of times what the handler takes; past it the wait is given up
 * with a line in the log, the one case in which the free that follows can
 * still race a handler. Sleeps rather than spins, so a handler on a thread
 * this one is starving still gets the core. */
static bool signal_handlers_drain(void)
{
	const double give_up = now_seconds() + 1.0;
	while (fnl_atomic_load_u32(&signal_busy) != 0) {
		if (now_seconds() > give_up) {
			fnl_log(FNL_LOG_WARN, "a signal handler is still running after "
					"a second; tearing down regardless");
			return false;
		}
		fnl_sleep_seconds(1e-4);
	}
	return true;
}

/* Which signals are taken, and what they were taken from. File scope because
 * a disposition belongs to the process, not to an App. Windows has no
 * sigaction and no SIGHUP; its runtime's signal() covers the other two. */
#if defined(_WIN32)
static const int SIGS[] = { SIGINT, SIGTERM };
static void (*prev_sig[sizeof SIGS / sizeof SIGS[0]])(int);
#else
static const int SIGS[] = { SIGINT, SIGTERM, SIGHUP };
static struct sigaction prev_sig[sizeof SIGS / sizeof SIGS[0]];
#endif

/* Whether prev_sig holds the *host's* dispositions. There is one signal_app,
 * so a second app asking replaces the first, and this stops it saving
 * on_terminate over what has to be put back. [main] only. */
static bool signals_taken;

/* The one #ifdef in the core: signals are an operating system's, not a window
 * server's, so routing this through the backend table would put a windowing
 * name on something that has nothing to do with windows. */
static void install_signal_handlers(App *a)
{
	fnl_atomic_store_ptr(&signal_app, a);

	for (size_t i = 0; i < sizeof SIGS / sizeof SIGS[0]; i++) {
#if defined(_WIN32)
		void (*old)(int) = signal(SIGS[i], on_terminate);
		if (!signals_taken)
			prev_sig[i] = old;
#else
		struct sigaction sa = { .sa_handler = on_terminate };
		sigemptyset(&sa.sa_mask);
		sigaction(SIGS[i], &sa, signals_taken ? NULL : &prev_sig[i]);
#endif
	}
	signals_taken = true;
}

/* Given back at teardown: left installed, the handler finds signal_app null
 * and the process ignores its terminating signals for good. Before
 * signal_app is cleared, so the handler is never installed with nothing
 * behind it; one already entered is waited out by signal_handlers_drain(). */
static void restore_signal_handlers(void)
{
	if (!signals_taken)
		return;
	for (size_t i = 0; i < sizeof SIGS / sizeof SIGS[0]; i++) {
#if defined(_WIN32)
		/* SIG_ERR is what signal() answers when the install failed, and it is
		 * not a disposition to put back. */
		if (prev_sig[i] != SIG_ERR)
			signal(SIGS[i], prev_sig[i]);
#else
		sigaction(SIGS[i], &prev_sig[i], NULL);
#endif
	}
	signals_taken = false;
}

/* ---- whose thread -------------------------------------------------------- */

static FNL_THREAD_LOCAL AppThread this_thread;

void app_thread_set(AppThread t) { this_thread = t; }
AppThread app_thread(void)       { return this_thread; }

/* render_thread_started is the main thread's own, written there alone, so
 * reading it is no race on the one thread that asks. */
bool app_renderer_state_ours(const App *a)
{
	return this_thread == APP_THREAD_RENDER ||
		   (this_thread == APP_THREAD_MAIN && !a->render_thread_started);
}

/* ---- teardown ------------------------------------------------------------ */

/* Ask the render thread to finish and wait for it. */
static void render_thread_stop(App *a)
{
	if (!a->render_thread_started)
		return;
	fnl_mutex_lock(&a->lock);
	a->quit = true;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
	fnl_thread_join(a->render_thread);
	a->render_thread_started = false;
	/* A move under way is waited for to its end: stopped halfway it would
	 * leave the GPU with no driver. */
	app_gpu_driver_join(a);
	if (a->lister_started) {
		fnl_thread_join(a->lister);
		a->lister_started = false;
	}
}

/* Every step is conditional, because this is also the failure path out of
 * fnl_init(): an app that got as far as a display but not as far as a device
 * comes through here with most of these still NULL. */
static void app_teardown(App *a)
{
	/* First, and it must stay first. A terminating signal arrives on any
	 * thread at any moment, and the handler follows this pointer into
	 * app_wakeup(), which reaches the backend's state -- freed by close()
	 * below. The disposition goes back before the pointer is cleared. Only
	 * for the app that installed them: any other would be clearing somebody
	 * else's registration. */
	if (a->installed_signals) {
		restore_signal_handlers();
		if (fnl_atomic_load_ptr(&signal_app) == a)
			fnl_atomic_store_ptr(&signal_app, NULL);
		/* Drained, no handler can reach this App again, and the backends'
		 * close() may shut the wakeup descriptor it would have written to;
		 * they keep it open while installed_signals says a handler might.
		 * Not drained -- one is stopped where it stands -- they still do. */
		if (signal_handlers_drain())
			a->installed_signals = false;
	}

	render_thread_stop(a);
	parts_close(a);

	/* The frame kept for a move's cover, which nothing paints from now:
	 * the render thread is joined and the window's thread is this one. */
	free(a->snap_px);
	a->snap_px = NULL;
	a->snap_valid = false;

	/* Both seams, renderer first: the surface belongs to the graphics API and
	 * is built on an object the backend owns, so it has to go before the window
	 * it was made from. */
	if (a->renderer && a->renderer_ready) {
		/* The same teardown device-loss recovery uses, so the two cannot
		 * drift. The cache is worth keeping only if the device is still there
		 * to read it back from. */
		RENDERER(a)->destroy_device_objects(a, !a->device_lost);
		RENDERER(a)->destroy_surface(a);
		RENDERER(a)->destroy_instance(a);
	}
	if (a->backend)
		BACKEND(a)->close(a);

	if (a->sync_ready) {
		fnl_cond_destroy(&a->cond);
		fnl_mutex_destroy(&a->lock);
		fnl_mutex_destroy(&a->ev_lock);
		fnl_mutex_destroy(&a->text_lock);
		a->sync_ready = false;
	}
	free(a->clip_in);
	free(a->clip_pending);
	free(a->clip_out);
	a->clip_in = a->clip_pending = a->clip_out = NULL;
}

/* A dispatch: what a wakeup *is* differs per backend. */
void app_wakeup(App *a)
{
	BACKEND(a)->wakeup(a);
}

void app_request_size_nudge(App *a)
{
	fnl_mutex_lock(&a->lock);
	a->req.nudge_size = true;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

/* Spend what the [any]-thread knobs recorded, on the pump's thread; see
 * App::req. The block is taken under one lock and released before anything
 * is spent: the backend calls below reach back into state this lock guards,
 * and holding it would put every knob's caller behind a compositor round
 * trip. */
void app_drain_requests(App *a)
{
	FNL_ASSERT_THREAD(APP_THREAD_MAIN);
	fnl_mutex_lock(&a->lock);
	const PendingReq r = a->req;
	a->req = (PendingReq){ 0 };
	char title[TITLE_MAX];
	if (r.title)
		memcpy(title, a->title, sizeof title);
	char *clip = a->clip_pending;
	a->clip_pending = NULL;
	fnl_mutex_unlock(&a->lock);

	/* The toggles first: a size asked for beside a fullscreen or maximize
	 * request must be answered against the state after the toggle. */
	if (r.fullscreen)  BACKEND(a)->set_fullscreen(a, r.fullscreen_val);
	if (r.maximized)   BACKEND(a)->set_maximized(a, r.maximized_val);
	if (r.minimize)    BACKEND(a)->minimize(a);
	if (r.bar)         deco_apply_custom(a, r.bar_h, r.bar_min_w);
	if (r.size)        app_request_size(a, r.size_w, r.size_h);
	/* Not beside a size: a new size is what the nudge is for, and the
	 * nudge's way back would be to the size before it. */
	if (r.nudge_size && !r.size)
		BACKEND(a)->nudge_size(a);
	/* The output before the mode: a mode is the output's, so one asked for
	 * beside a move is looked for on the output moved to. */
	if (r.output)      BACKEND(a)->set_output(a, r.output_name);
	if (r.mode)        BACKEND(a)->set_mode(a, &r.mode_val);
	if (r.release_ptr) BACKEND(a)->release_pointer(a);
	/* A caller's move or resize ends any double-click the titlebar was
	 * waiting on, as the library's own drag does (deco_drag): the press
	 * that started it was not a first click. Here, once, for every backend. */
	if (r.move || r.resize) {
		a->titlebar_armed = false;
		a->last_titlebar_click_ms = 0;
	}
	if (r.move)        BACKEND(a)->begin_move(a);
	if (r.resize)      BACKEND(a)->begin_resize(a, r.resize_edge);
	if (r.cursor) {
		a->cursor = r.cursor_val;
		BACKEND(a)->cursor_changed(a);
	}
	if (r.text_mode)
		BACKEND(a)->text_input_changed(a);
	if (r.title)
		BACKEND(a)->set_title(a, title);
	/* The string changes hands here and is the pump's from now on. Before
	 * the request for one, so that copy-then-paste in one turn pastes what
	 * was copied. */
	if (clip) {
		free(a->clip_out);
		a->clip_out     = clip;
		a->clip_out_len = strlen(clip);
		BACKEND(a)->clipboard_set(a);
	}
	if (r.clip_get)
		BACKEND(a)->clipboard_request(a);
}

/* ---- the held set ---------------------------------------------------------
 *
 * One bit per FnlKey, for fnl_key_down(). FNL_KEY_UNKNOWN has none: it is
 * every key the table does not name. */
static bool key_tracked(FnlKey k)
{
	return k > FNL_KEY_UNKNOWN && (unsigned)k < 32u * KEY_HELD_WORDS;
}

/* Load and store rather than an atomic OR, which is sound because the input
 * thread is the only writer. */
static void key_held_set(App *a, FnlKey k, bool down)
{
	if (!key_tracked(k))
		return;
	FnlAtomicU32  *w   = &a->keys_held[(unsigned)k / 32u];
	const uint32_t bit = 1u << ((unsigned)k % 32u);
	const uint32_t cur = fnl_atomic_load_u32(w);
	fnl_atomic_store_u32(w, down ? (cur | bit) : (cur & ~bit));
}

static bool key_held_get(const App *a, FnlKey k)
{
	if (!key_tracked(k))
		return false;
	return (fnl_atomic_load_u32(&a->keys_held[(unsigned)k / 32u]) >>
			((unsigned)k % 32u)) & 1u;
}

/* Flag a change the next frame has to carry rather than the next one the
 * compositor asks for (see App::want_urgent). The signal is what breaks the
 * render thread out of a pacing or late-latch wait to take it on. */
void app_mark_urgent(App *a)
{
	fnl_mutex_lock(&a->lock);
	a->want_urgent = true;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

/* ---- publishing to the render thread --------------------------------------
 *
 * All from the pump thread, the only writer of the want_* block: a compare
 * may be made outside App::lock, and the store takes it because the render
 * thread latches the block whole. */

/* A size the caller asked for, as the window it means: the request names the
 * presentable area, so the titlebar is added. Refused while fullscreen or
 * maximized, which a window server cannot be relied on to decline itself --
 * and so always where the window is the output, which is maximized for its
 * whole life and whose size is a mode (set_mode). */
void app_request_size(App *a, int w_pt, int h_pt)
{
	if (w_pt <= 0 || h_pt <= 0 || a->is_fullscreen || a->is_maximized)
		return;
	/* The floor, here and once: X11 leaves it to the manager's size hints and
	 * Win32 grows the window back, but a floating Wayland window would
	 * publish whatever it was handed until the next configure, which may
	 * never come. */
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	int w = w_pt, h = h_pt + deco_bar_h(a, false);
	if (w < min_w) w = min_w;
	if (h < min_h) h = min_h;
	BACKEND(a)->request_size(a, w, h);
}

/* The window's size and state as the window server last reported them, with
 * the serial of the configure the frame drawing them answers, if the server
 * keeps one. A flip of fullscreen or maximized is urgent here, for every
 * backend, as Wayland's configure marks it: the frame that shows the new
 * state is not one to pace. */
static void publish_size(App *a, int w_pt, int h_pt, int s120,
						 bool has_serial, uint32_t serial)
{
	fnl_mutex_lock(&a->lock);
	if (a->want_fullscreen != a->is_fullscreen ||
		a->want_maximized  != a->is_maximized)
		a->want_urgent = true;
	a->want_w          = w_pt;
	a->want_h          = h_pt;
	a->want_scale120   = s120;
	a->want_fullscreen = a->is_fullscreen;
	a->want_maximized  = a->is_maximized;
	a->cfg_pending     = has_serial;
	if (has_serial)
		a->cfg_serial  = serial;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);

	a->cur_w = w_pt;
	a->cur_h = h_pt;
	a->win_w = w_pt;
	a->win_h = h_pt;
	a->configured = true;

	/* After cur_* and the scale, which the area is measured from. */
	app_reticle_clamp(a);
}

bool app_publish_scale(App *a, int s120)
{
	fnl_mutex_lock(&a->lock);
	const bool moved = a->want_scale120 != s120;
	if (moved) {
		a->want_scale120 = s120;
		fnl_cond_signal(&a->cond);
	}
	fnl_mutex_unlock(&a->lock);
	return moved;
}

void app_publish_size(App *a, int w_pt, int h_pt, int s120)
{
	publish_size(a, w_pt, h_pt, s120, false, 0);
}

void app_publish_size_serial(App *a, int w_pt, int h_pt, int s120,
							 uint32_t serial)
{
	publish_size(a, w_pt, h_pt, s120, true, serial);
}

void app_publish_hidden(App *a)
{
	fnl_mutex_lock(&a->lock);
	a->want_w = 0;
	a->want_h = 0;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

void app_publish_focus(App *a, bool focused)
{
	fnl_mutex_lock(&a->lock);
	a->want_active = focused;
	fnl_mutex_unlock(&a->lock);
	app_mark_urgent(a);
}

/* After the held set has been cleared or seeded, so a caller that re-reads
 * fnl_key_down() from the event sees the set the focus change left.
 *
 * A titlebar press armed when the focus goes is disarmed here, for every
 * backend: whatever took the focus -- a popup, a manager's switcher -- may
 * also take the release, and the next motion would start a move the button
 * no longer holds. Not left to the pointer's leave, which a press on the bar
 * of a window losing focus beneath the pointer never sees. */
void app_post_focus(App *a, bool focused)
{
	if (!focused)
		deco_release(a);
	FnlEvent ev = { .type = FNL_EVENT_FOCUS };
	ev.focus.focused = focused;
	app_event(a, &ev);
}

/* Which output the window is on and how much room it has there, in logical
 * points. A NULL name leaves the one last published standing. */
void app_publish_output(App *a, const char *name, int w_pt, int h_pt)
{
	a->out_w_pt = w_pt;
	a->out_h_pt = h_pt;

	if (a->want_out_w_pt == w_pt && a->want_out_h_pt == h_pt &&
		(!name || !strcmp(a->want_output, name)))
		return;

	fnl_mutex_lock(&a->lock);
	a->want_out_w_pt = w_pt;
	a->want_out_h_pt = h_pt;
	if (name)
		snprintf(a->want_output, sizeof a->want_output, "%s", name);
	fnl_cond_signal(&a->cond);             /* for a loop at rest */
	fnl_mutex_unlock(&a->lock);
}

void app_publish_output_mode(App *a, const char *name, int mode_w,
							 int mode_h)
{
	fnl_mutex_lock(&a->lock);
	a->want_out_mode_w = mode_w;
	a->want_out_mode_h = mode_h;
	if (name)
		snprintf(a->want_output, sizeof a->want_output, "%s", name);
	fnl_cond_signal(&a->cond);             /* for a loop at rest */
	fnl_mutex_unlock(&a->lock);
}

/* A backend that cannot say leaves it at zero, which keeps the spacing on
 * the period it measures. */
void app_publish_refresh(App *a, unsigned int mhz)
{
	fnl_atomic_store_u32(&a->out_refresh_mhz, mhz);
}

/* A change is a frame owed as well: the output box reads this list, and
 * a move between two monitors of one size and scale changes nothing else
 * the frame compares (fnl_invalidate()'s bump, under the same lock). */
void app_publish_outputs(App *a, char (*names)[OUTPUT_NAME_MAX], int n,
						 int current)
{
	char kept[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int  count = 0, cur = -1;
	for (int i = 0; i < n && count < OUTPUTS_MAX; i++) {
		if (!names[i][0])
			continue;
		if (i == current)
			cur = count;
		snprintf(kept[count++], OUTPUT_NAME_MAX, "%s", names[i]);
	}

	fnl_mutex_lock(&a->lock);
	bool same = a->out_count == count && a->out_current == cur;
	for (int i = 0; same && i < count; i++)
		same = strcmp(a->out_names[i], kept[i]) == 0;
	if (!same) {
		memcpy(a->out_names, kept, (size_t)count * sizeof kept[0]);
		a->out_count   = count;
		a->out_current = cur;
		a->want_user_gen++;
		fnl_cond_signal(&a->cond);         /* for a loop at rest */
	}
	fnl_mutex_unlock(&a->lock);
}

/* Each axis alone: the offset kept, then the far edge pulled in, then the
 * near one, so a window larger than the room starts at its top-left. */
static int place_axis(int at, int len, int from_at, int to_at, int to_len)
{
	int v = to_at + (at - from_at);
	if (v + len > to_at + to_len)
		v = to_at + to_len - len;
	if (v < to_at)
		v = to_at;
	return v;
}

void app_place_on_output(int x, int y, int w, int h, const FnlRect *from,
						 const FnlRect *to, int *nx, int *ny)
{
	*nx = place_axis(x, w, from->x, to->x, to->w);
	*ny = place_axis(y, h, from->y, to->y, to->h);
}

/* One writer, so a load and a store are the increment. The invalidation is
 * for a window that is not animating, whose loop would otherwise wait for
 * a reason to draw before it saw the count. */
void app_resumed(App *a)
{
	fnl_atomic_store_u32(&a->resumes, fnl_atomic_load_u32(&a->resumes) + 1);
	fnl_invalidate(a);
}

/* The backend's surface no longer describes the output -- KMS set another
 * mode -- and the render thread is to make it again. Counted as resumes is,
 * and before the size that goes with it is published: the render thread
 * asks for the count after each latch of the size (surface_remake_owed()),
 * so no swapchain of the new size is built on the old surface, which a
 * display surface refuses. Counted after, the publish's own signal woke
 * the render thread into that rebuild before the count moved. */
void app_surface_remake(App *a)
{
	fnl_atomic_store_u32(&a->surface_remakes,
						 fnl_atomic_load_u32(&a->surface_remakes) + 1);
	app_mark_urgent(a);
}

/* ---- the presentable area and the reticle ----------------------------------
 *
 * The area in pixels as the pump thread knows it: where it starts inside the
 * window, grab ring excluded, and how big it is. Any out-parameter may be
 * NULL.
 *
 * The size is content_px_for()'s, the one the frame draws: both edges are
 * rounded after the titlebar and the ring have moved them. deco_bar_h() and
 * never deco_client(): the calls take the band's *height*, and a flag
 * converts to 1 with nothing to warn of it. */
void app_content_px_rect(const App *a, int *x, int *y, int *w, int *h)
{
	const int s120 = want_s120(a);
	const int bar  = deco_bar_h(a, a->is_fullscreen);
	int cx, cy, cw, ch, pw, ph;

	content_rect(a->cur_w, a->cur_h, bar, &cx, &cy, &cw, &ch);
	content_px_for(a->cur_w, a->cur_h, s120,
				   deco_margin(a, a->is_fullscreen), bar, &pw, &ph);
	if (x) *x = scale_pt(cx, s120);
	if (y) *y = scale_pt(cy, s120);
	if (w) *w = pw;
	if (h) *h = ph;
}

/* Where the reticle is, and whether there is one, published only on a
 * change. Only a captured pointer has one: its position published the rest
 * of the time would re-record the frame for nothing. A capture beginning or
 * ending is answered at once where nothing invites a frame. */
void app_reticle_publish(App *a)
{
	const int  x  = (int)(a->cur_px + 0.5);
	const int  y  = (int)(a->cur_py + 0.5);
	const bool on = a->pointer_captured;

	if (!on && !a->want_captured)
		return;
	if (a->want_captured == on && a->want_cursor_x == x &&
		a->want_cursor_y == y)
		return;

	const bool flipped = a->want_captured != on;
	fnl_mutex_lock(&a->lock);
	a->want_captured = on;
	a->want_cursor_x = x;
	a->want_cursor_y = y;
	fnl_cond_signal(&a->cond);             /* for a loop at rest */
	fnl_mutex_unlock(&a->lock);
	if (flipped && !BACKEND(a)->invites)
		app_mark_urgent(a);
}

/* Into the area's last pixel on every side and no further. The upper bound
 * first and zero last, so an area that measures to nothing leaves the reticle
 * at the origin rather than at -1. */
static void reticle_bound(App *a, int w, int h)
{
	if (a->cur_px > w - 1) a->cur_px = w - 1;
	if (a->cur_py > h - 1) a->cur_py = h - 1;
	if (a->cur_px < 0.0)   a->cur_px = 0.0;
	if (a->cur_py < 0.0)   a->cur_py = 0.0;
}

/* The reticle starts where the click that asked for the capture landed.
 * App::ptr_x/ptr_y are surface points, ring included. Not published: the
 * backend does that once the capture has actually been taken. */
void app_reticle_start(App *a)
{
	const int s120 = want_s120(a);
	const int m    = deco_margin(a, a->is_fullscreen);
	int cx, cy, cw, ch;

	app_content_px_rect(a, &cx, &cy, &cw, &ch);
	a->cur_px = pt_to_pxf(a->ptr_x - m, s120) - cx;
	a->cur_py = pt_to_pxf(a->ptr_y - m, s120) - cy;
	reticle_bound(a, cw, ch);
}

/* A captured pointer's movement, in pixels. */
void app_reticle_move(App *a, double dx_px, double dy_px)
{
	int cw, ch;
	app_content_px_rect(a, NULL, NULL, &cw, &ch);
	a->cur_px += dx_px;
	a->cur_py += dy_px;
	reticle_bound(a, cw, ch);
	app_reticle_publish(a);
}

/* For an area that changed under the reticle. Clamped and not re-centred:
 * cur_px/cur_py also decide where the desktop cursor is put back when the
 * capture ends. An area of nothing -- a minimize in passing -- moves
 * nothing. */
void app_reticle_clamp(App *a)
{
	int cw, ch;
	if (!a->pointer_captured)
		return;
	app_content_px_rect(a, NULL, NULL, &cw, &ch);
	if (cw < 1 || ch < 1)
		return;
	reticle_bound(a, cw, ch);
	app_reticle_publish(a);
}

/* ---- seam entries a backend may name rather than write ---------------------
 * See backend.h. */

/* Where nothing invites a frame. A window on screen is answered at once and
 * the present is the throttle. For one that is not, this *waits* before
 * saying so: the loop answers false by going straight round again. */
bool backend_frame_wait_shown(App *a, bool break_on_urgent)
{
	if (BACKEND(a)->window_on_screen(a))
		return true;
	app_park(a, break_on_urgent);
	return false;
}

void app_park(App *a, bool break_on_urgent)
{
	fnl_mutex_lock(&a->lock);
	if (!a->quit && !(break_on_urgent && a->want_urgent))
		fnl_cond_wait_for(&a->cond, &a->lock, FRAME_WAIT_S);
	fnl_mutex_unlock(&a->lock);
}

void backend_noop(App *a)
{
	(void)a;
}

void renderer_noop(App *a)
{
	(void)a;
}

const char *backend_noop_host(const App *a)
{
	(void)a;
	return "";
}

bool backend_noop_shared_driver(const App *a)
{
	(void)a;
	return false;
}

int backend_noop_gpu_drivers(const App *a, GpuDriver *out, int max)
{
	(void)a;
	(void)out;
	(void)max;
	return 0;
}

bool backend_noop_gpu_driver_set(const App *a, const GpuDriver *g,
								 const char *driver)
{
	(void)a;
	(void)g;
	(void)driver;
	return false;
}

bool backend_noop_gpu_driver_held(const App *a, const GpuDriver *g,
								  bool every_nvidia, char *what,
								  size_t size)
{
	(void)a;
	(void)g;
	(void)every_nvidia;
	(void)what;
	(void)size;
	return false;
}

const char *backend_always_present(void)
{
	return NULL;
}

/* ---- choosing the backend -------------------------------------------------
 * DESIGN *One build per operating system*. */

#define BACKEND_ENTRY(key) &fnl_backend_##key,
static const FnlBackendVtbl *const BACKENDS[] = {
	FNL_BACKENDS(BACKEND_ENTRY)
};
#undef BACKEND_ENTRY
#define BACKEND_COUNT (sizeof BACKENDS / sizeof BACKENDS[0])

/* "a: why; b: why", for the one line that says what was passed over. */
static void note_append(char *out, size_t size, const char *name,
						const char *why)
{
	const size_t at = strlen(out);
	if (at < size)
		snprintf(out + at, size - at, "%s%s: %s", at ? "; " : "", name, why);
}

/* A variable under the name it had before the backend and the renderer
 * were called so: refused, with the new name, rather than read or ignored
 * -- either would run something other than what the line asked for. */
static FnlResult refuse_old_name(const char *old, const char *now)
{
	const char *v = getenv(old);
	if (!v || !v[0])
		return FNL_OK;
	return fnl_fail(FNL_ERR_UNSUPPORTED, "%s is %s now: %s=%s", old, now,
					now, v);
}

/* A variable one backend reads, found set where another opened: said once,
 * since what it names is not what the window will type with. */
static void note_other_backends_vars(const FnlBackendVtbl *p)
{
	const char *v = getenv("FNL_KMS_LAYOUT");
	if (v && v[0] && p->id != BACKEND_KMS)
		fnl_log(FNL_LOG_INFO, "FNL_KMS_LAYOUT is KMS's alone; on %s the "
				"layout is the session's", p->name);
}

FnlResult backend_choose_and_open(App *a)
{
	FNL_TRY(refuse_old_name("FNL_PLATFORM", "FNL_BACKEND"));
	/* Named: that one, or a refusal -- never a step down. A forced run that
	 * quietly ran another backend is a figure that lies. */
	const char *want = getenv("FNL_BACKEND");
	if (want && want[0]) {
		for (size_t i = 0; i < BACKEND_COUNT; i++)
			if (strcmp(BACKENDS[i]->key, want) == 0) {
				a->backend = BACKENDS[i];
				fnl_log(FNL_LOG_INFO, "backend: %s, named by FNL_BACKEND",
						BACKENDS[i]->name);
				note_other_backends_vars(BACKENDS[i]);
				return BACKENDS[i]->open(a);
			}
		char have[64] = "";
		for (size_t i = 0; i < BACKEND_COUNT; i++) {
			const size_t at = strlen(have);
			snprintf(have + at, sizeof have - at, "%s%s", i ? ", " : "",
					 BACKENDS[i]->key);
		}
		return fnl_fail(FNL_ERR_UNSUPPORTED,
						"FNL_BACKEND is '%s'; this build has %s", want,
						have);
	}

	/* Chosen: the first that is not absent, stepping past one that finds no
	 * window server. Its close() is its open()'s failure path, so what it
	 * built is gone before the next one starts. */
	char passed[512] = "";
	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		const FnlBackendVtbl *p = BACKENDS[i];
		FNL_ASSERT(p->id != 0 && p->key && p->absent);
		const char *why = p->absent();
		if (why) {
			note_append(passed, sizeof passed, p->name, why);
			continue;
		}
		a->backend = p;
		const FnlResult rc = FNL_FAULT("backend-no-display")
			? fnl_fail(FNL_ERR_NO_DISPLAY, "%s refused (FNL_FAULT)", p->name)
			: p->open(a);
		if (rc == FNL_OK) {
			fnl_log(FNL_LOG_INFO, "backend: %s%s%s", p->name,
					passed[0] ? "; passed over " : "", passed);
			note_other_backends_vars(p);
			return FNL_OK;
		}
		if (rc != FNL_ERR_NO_DISPLAY)
			return rc;       /* app_teardown() closes it */
		note_append(passed, sizeof passed, p->name, fnl_last_error());
		p->close(a);
		a->backend = NULL;
	}
	return fnl_fail(FNL_ERR_NO_DISPLAY, "no backend here -- %s",
					passed[0] ? passed : "this build has none");
}

/* ---- the renderer ------------------------------------------------------ */

/* Every renderer this build has, in the order tried (renderer.h,
 * FNL_RENDERERS). */
#define RENDERER_ENTRY(key) &fnl_renderer_##key,
static const FnlRendererVtbl *const RENDERERS[] = {
	FNL_RENDERERS(RENDERER_ENTRY)
};
#undef RENDERER_ENTRY
/* A test of it is a constant of the build, parenthesised where it decides
 * a branch: that is clang's mark for a constant on purpose, which
 * -Wunreachable-code otherwise reads as dead code. */
#define RENDERER_COUNT (sizeof RENDERERS / sizeof RENDERERS[0])
_Static_assert(RENDERER_COUNT <= GPU_SPAN_MAX,
			   "GPU_SPAN_MAX is below the renderers a build has");

/* The GPU list's parts, below with the rest of it. */
static int gpu_slot(App *a, const FnlRendererVtbl *r) FNL_REQUIRES(a->lock);
static void gpu_merge(App *a) FNL_REQUIRES(a->lock);

/* "d3d12, vulkan, gl": what a refusal says the build has. */
static void renderer_keys(char *out, size_t size)
{
	out[0] = '\0';
	for (size_t i = 0; i < RENDERER_COUNT; i++) {
		const size_t at = strlen(out);
		if (at < size)
			snprintf(out + at, size - at, "%s%s", i ? ", " : "",
					 RENDERERS[i]->key);
	}
}

/* The renderers this caller may be handed, in the build's order, and what
 * named one where one was -- FNL_RENDERER, the environment's, which wins, or
 * FnlInitDesc::renderer. A caller that wrote one scene must never be handed
 * another API, so with neither `renderer` nor `renderers` a build of several
 * refuses rather than guessing. Asked twice, before open() and again after
 * it, and the same both times: it reads nothing that open() changes. */
static FnlResult renderer_candidates(const FnlInitDesc *d,
									 const FnlRendererVtbl *out[], size_t *n,
									 const char **named)
{
	char have[64];
	renderer_keys(have, sizeof have);
	*n = 0;
	*named = NULL;
	FNL_TRY(refuse_old_name("FNL_GPU_API", "FNL_RENDERER"));

	uint32_t allowed;
	if (d->renderer != FNL_RENDERER_DEFAULT)
		/* Out of range is a renderer no build has, and not a shift past
		 * the word. */
		allowed = (unsigned)d->renderer < 32
				? FNL_RENDERER_BIT(d->renderer) : 0;
	else if (d->renderers)
		allowed = d->renderers;
	else if ((RENDERER_COUNT == 1))
		allowed = FNL_RENDERER_BIT(RENDERERS[0]->api);
	else
		return fnl_fail(FNL_ERR_UNSUPPORTED,
						"this build has several renderers (%s); "
						"FnlInitDesc::renderers names those the program has "
						"a scene for, and FNL_RENDERERS_ANY says it has none",
						have);

	const char *want = getenv("FNL_RENDERER");
	if (want && want[0]) {
		const FnlRendererVtbl *g = NULL;
		for (size_t i = 0; i < RENDERER_COUNT; i++)
			if (strcmp(RENDERERS[i]->key, want) == 0)
				g = RENDERERS[i];
		if (!g)
			return fnl_fail(FNL_ERR_UNSUPPORTED,
							"FNL_RENDERER is '%s'; this build has %s", want,
							have);
		if (!(allowed & FNL_RENDERER_BIT(g->api)))
			return fnl_fail(FNL_ERR_UNSUPPORTED,
							"FNL_RENDERER is '%s', which this program has "
							"no scene for", want);
		out[(*n)++] = g;
		*named = "FNL_RENDERER";
	} else {
		for (size_t i = 0; i < RENDERER_COUNT; i++)
			if (allowed & FNL_RENDERER_BIT(RENDERERS[i]->api))
				out[(*n)++] = RENDERERS[i];
		if (d->renderer != FNL_RENDERER_DEFAULT)
			*named = "FnlInitDesc::renderer";
	}
	if (*n == 0 && (RENDERER_COUNT == 1))
		return fnl_fail(FNL_ERR_UNSUPPORTED,
						"this build has the %s renderer, not the one "
						"asked for", RENDERERS[0]->name);
	if (*n == 0)
		return fnl_fail(FNL_ERR_UNSUPPORTED,
						"this build has %s, and none of them was asked for",
						have);
	/* Refused rather than degraded, and before open(), which takes the
	 * window server's caption away: a renderer that cannot draw the bar
	 * would open a window with neither. */
	for (size_t i = 0; i < *n; i++)
		if (!out[i]->draws_overlay)
			return fnl_fail(FNL_ERR_UNSUPPORTED,
							"the %s renderer cannot draw the overlay, and "
							"every window here wears a titlebar drawn "
							"from it", out[i]->name);
	return FNL_OK;
}

FnlResult renderer_check_choice(const FnlInitDesc *d)
{
	const FnlRendererVtbl *cand[RENDERER_COUNT];
	size_t n;
	const char *named;
	return renderer_candidates(d, cand, &n, &named);
}

/* The size the backend configured, adopted before anything is built
 * that depends on it. */
static void adopt_size(App *a)
{
	fnl_mutex_lock(&a->lock);
	a->draw_w = a->want_w;
	a->draw_h = a->want_h;
	a->draw_scale120 = a->want_scale120;
	/* The grab ring belongs to this first build too: at zero the first frame
	 * would rebuild the swapchain it had just been handed, and the build
	 * would declare the surface opaque when it is not. */
	a->draw_margin = deco_margin(a, a->want_fullscreen);
	fnl_mutex_unlock(&a->lock);
}

/* App::renderer and the copy other threads read, together. A null is not
 * published: a reader keeps the table it had, which is never freed. */
static void renderer_set(App *a, const FnlRendererVtbl *g)
{
	a->renderer = g;
	if (g)
		fnl_atomic_store_ptr(&a->renderer_pub, (void *)g);
}

const FnlRendererVtbl *renderer_any(const App *a)
{
	return (const FnlRendererVtbl *)fnl_atomic_load_ptr(&a->renderer_pub);
}

const FnlRendererVtbl *renderer_of_api(FnlRenderer api)
{
	for (size_t i = 0; i < RENDERER_COUNT; i++)
		if (RENDERERS[i]->api == api)
			return RENDERERS[i];
	return NULL;
}

static char ascii_lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool has_part(const char *name, const char *part)
{
	for (; *name; name++) {
		size_t i = 0;
		while (part[i] && ascii_lower(name[i]) == ascii_lower(part[i]))
			i++;
		if (!part[i])
			return true;
	}
	return false;
}

bool app_name_has(const char *name, const char *part)
{
	if (has_part(name, part))
		return true;
	/* And with the vendors' marks left out, as app_gpu_short() leaves
	 * them: "Intel HD Graphics 630" names "Intel(R) HD Graphics 630". */
	char bare[GPU_NAME_MAX];
	size_t o = 0;
	for (size_t i = 0; name[i] && o + 1 < sizeof bare; ) {
		if (strncmp(name + i, "(R)", 3) == 0) {
			i += 3;
		} else if (strncmp(name + i, "(TM)", 4) == 0) {
			i += 4;
		} else {
			bare[o++] = name[i++];
		}
	}
	bare[o] = '\0';
	return has_part(bare, part);
}

/* One renderer, built whole on the open window or not at all: a failure
 * tears down what it built, as app_teardown() would, and leaves App with no
 * renderer, so the next one starts from where this one did. */
static FnlResult renderer_build(App *a, const FnlRendererVtbl *g,
								bool may_fault)
{
	renderer_set(a, g);
	a->device_lost = a->surface_lost = false;
	double t = now_seconds();
	FnlResult rc = may_fault && FNL_FAULT("renderer-refused")
		? fnl_fail(FNL_ERR_NO_DEVICE, "%s refused (FNL_FAULT)", g->name)
		: g->create_instance(a);
	a->build_ms[0] = (now_seconds() - t) * 1e3;
	if (rc < 0) {
		a->renderer = NULL;
		return rc;
	}
	a->renderer_ready = true;
	t = now_seconds();
	if ((rc = g->create_surface(a)) == FNL_OK) {
		a->build_ms[1] = (now_seconds() - t) * 1e3;
		adopt_size(a);
		a->record_gen = 1; /* img_cmd_gen starts at 0, so everything records */
		t = now_seconds();
		rc = g->create_device_objects(a);
		a->build_ms[2] = (now_seconds() - t) * 1e3;
	}
	if (rc < 0) {
		g->destroy_device_objects(a, false);
		g->destroy_surface(a);
		g->destroy_instance(a);
		a->renderer_ready = false;
		a->renderer = NULL;
	}
	return rc;
}

/* The renderer drawing, torn down as app_teardown() does. */
static void renderer_unbuild(App *a)
{
	if (!a->renderer || !a->renderer_ready)
		return;
	RENDERER(a)->destroy_device_objects(a, !a->device_lost);
	RENDERER(a)->destroy_surface(a);
	RENDERER(a)->destroy_instance(a);
	a->renderer_ready = false;
}

/* Built on the render thread, which then holds it: the build made it
 * current there already where a renderer has a current context, and
 * thread_enter() says so in that renderer's own terms. */
static FnlResult renderer_build_here(App *a, const FnlRendererVtbl *g)
{
	/* The frames go into a window no driver before has claimed
	 * (FnlBackendVtbl::present_window_renew), as at a move between two
	 * GPUs of one renderer. */
	BACKEND(a)->present_window_renew(a);
	const FnlResult rc = renderer_build(a, g, false);
	if (rc == FNL_OK) {
		g->thread_enter(a);
		/* Never the generation last shown, which the loop would take the
		 * new renderer's first frame for; and never zero. */
		a->record_gen = a->shown_gen + 1 ? a->shown_gen + 1 : 1;
	}
	return rc;
}

/* Whether the caller has a scene for `r`: one the list spans. */
static bool gpu_spanned(App *a, const FnlRendererVtbl *r)
{
	bool found = false;
	fnl_mutex_lock(&a->lock);
	for (int k = 0; k < a->gpu_span_n; k++)
		found |= a->gpu_span[k] == r;
	fnl_mutex_unlock(&a->lock);
	return found;
}

/* A move to another kernel driver drawn across (DESIGN *Drawn across the
 * move*): Vulkan built on the drivers that open nothing of the GPU moved
 * -- Mesa's for Intel's and AMD's GPUs and its CPU rasteriser, every
 * instance made meanwhile of them alone (App::gpu_cross) -- shown to hold
 * nothing of it, and the move started on a thread of its own. Vulkan
 * whichever renderer the move lands on: OpenGL's glvnd loads NVIDIA's EGL
 * beside Mesa's for every display, and that holds the GPU. True where all
 * three are done; false with nothing built and the choice as it was, the
 * move then made with the frames paused, and every later move with them,
 * since what would not cross once will not the next time -- or this one
 * alone, for a caller with no Vulkan scene to draw across with. */
static bool renderer_cross(App *a, const GpuDriver *g)
{
	const FnlRendererVtbl *to = renderer_of_api(FNL_RENDERER_VULKAN);
	if (a->gpu_cross_off)
		return false;
	if (!to || !gpu_spanned(a, to)) {
		fnl_log(FNL_LOG_INFO, "GPU driver: the frames pause for this move, "
				"with no Vulkan scene to draw across it with");
		return false;
	}
	memcpy(a->gpu_cross_want, a->gpu_want, sizeof a->gpu_cross_want);
	const int index = a->gpu_index;
	a->gpu_want[0] = '\0';
	a->gpu_index = 0;
	a->gpu_cross = true;
	char held[768];
	if (FNL_FAULT("gpu-driver-uncrossed")) {
		fnl_log(FNL_LOG_INFO, "GPU driver: not drawn across the move "
				"(FNL_FAULT)");
	} else if (renderer_build_here(a, to) < 0) {
		fnl_log(FNL_LOG_INFO, "GPU driver: nothing here to draw on across "
				"the move (%s)", fnl_last_error());
	} else {
		if (BACKEND(a)->gpu_driver_held(a, g, false, held, sizeof held)) {
			fnl_log(FNL_LOG_WARN, "GPU driver: the renderer drawn across "
					"the move holds %s, which the move needs let go", held);
		} else if (app_gpu_driver_start_move(a, g)) {
			fnl_log(FNL_LOG_INFO, "GPU driver: drawn on %s, built in %.0f "
					"ms, while %s moves to %s", a->gpu_name,
					a->build_ms[0] + a->build_ms[1] + a->build_ms[2], g->pci,
					g->other);
			return true;
		} else {
			fnl_log(FNL_LOG_WARN, "GPU driver: no thread for the move");
		}
		renderer_unbuild(a);
	}
	fnl_log(FNL_LOG_INFO, "GPU driver: the frames pause for this move and "
			"every later one");
	a->gpu_cross_off = true;
	a->gpu_cross = false;
	memcpy(a->gpu_want, a->gpu_cross_want, sizeof a->gpu_want);
	a->gpu_index = index;
	return false;
}

/* Whether something of NVIDIA's driver is still in the process, with no
 * renderer built and the listing joined, when nothing should be: a
 * library a caller loaded, or one of ours not closed. NVIDIA's userspace
 * loaded then holds the GPU on its module, and on nouveau leaves its
 * Vulkan driver here blind to the GPU after the move, a move that would
 * draw elsewhere and whose way back would be refused -- so the move is not
 * made, and none is offered again. */
static bool gpu_driver_leftover(App *a, const GpuDriver *g)
{
	char held[768];
	if (FNL_FAULT("gpu-driver-leftover"))
		snprintf(held, sizeof held, "(FNL_FAULT)");
	else if (!BACKEND(a)->gpu_driver_held(a, g, true, held, sizeof held))
		return false;
	fnl_log(FNL_LOG_WARN, "GPU driver: %s is open in this process with no "
			"renderer built, so %s stays on %s", held, g->pci, g->bound);
	app_gpu_drivers_pin(a, "NVIDIA's driver is held by something the "
						"library did not open, or did not close");
	return true;
}

static void gpu_list_early(App *a, uint32_t scenes,
						   const FnlRendererVtbl *drawing);

/* The GPU on another driver and the renderer not yet built on it: what the
 * other renderers listed before is the old driver's, so it is dropped, and
 * listed again here where gpu_list_early() would have listed it at the
 * window's start -- for an OpenGL landing, since a Vulkan instance made
 * beside NVIDIA's live EGL display terminates it, and the listing after
 * the first frame (app_gpu_list_others()) would be that. */
static void gpu_relist_landing(App *a, const FnlRendererVtbl *to)
{
	uint32_t scenes = 0;
	fnl_mutex_lock(&a->lock);
	for (int k = 0; k < a->gpu_span_n; k++)
		scenes |= FNL_RENDERER_BIT(a->gpu_span[k]->api);
	fnl_mutex_unlock(&a->lock);
	a->gpu_early_n = 0;
	gpu_list_early(a, scenes, to);
}

void renderer_swap(App *a)
{
	const FnlRendererVtbl *from = a->renderer;
	const FnlRendererVtbl *to = a->renderer_next;
	a->renderer_next = NULL;
	/* With the moves offered, the listing ends first: OpenGL torn down
	 * while it lists Vulkan's GPUs closes glvnd with NVIDIA's Vulkan
	 * driver mapped, which the close then keeps for the process's life
	 * (egl_dyn.c), where a move rests on each instance starting it
	 * afresh. And before the GPUs are listed again after one came or went
	 * (gpu_relist_landing()): that rewrites what a listing still running
	 * reads, and the listing after it is started over this one's handle,
	 * which nothing would join then. */
	if ((a->gpu_drivers_on || a->gpu_hotplug_relist) && a->lister_started) {
		fnl_thread_join(a->lister);
		a->lister_started = false;
	}
	/* The gap's picture under the swapchain before it goes
	 * (FnlBackendVtbl::present_window_hold). */
	if (a->renderer && a->renderer_ready)
		BACKEND(a)->present_window_hold(a);
	renderer_unbuild(a);

	/* A move to another kernel driver is made here, where nothing of the
	 * renderer holds the GPU open: the instance is gone with the rest. Or
	 * drawn across, and then this is the swap onto the GPU moved, the move
	 * made already on its own thread (app_gpu_driver_poll()). */
	bool stayed = false;
	if (a->gpu_cross_landing) {
		/* The way back is the renderer the window was on before the move,
		 * whose choice gpu_back_* is, and not the one drawn across on. */
		a->gpu_cross_landing = false;
		from = a->gpu_cross_from;
		stayed = !a->mover_moved;
	} else if (a->gpu_driver_next >= 0) {
		GpuDriver g;
		if (!app_gpu_driver_take(a, &g))
			stayed = true;
		else if (gpu_driver_leftover(a, &g))
			stayed = true;
		else if (renderer_cross(a, &g)) {
			a->gpu_cross_from = from;
			return;
		} else {
			stayed = !app_gpu_driver_make(a, &g);
		}
	}
	FnlResult rc;
	/* The GPUs listed again, the others first: after a move to another
	 * kernel driver, and after a GPU came or went (app_gpu_hotplug()). */
	const bool relist = a->gpu_driver_moved || a->gpu_hotplug_relist;
	a->gpu_hotplug_relist = false;
	if (!stayed && relist)
		gpu_relist_landing(a, to);
	if (stayed)
		rc = fnl_fail(FNL_ERR_NO_DEVICE, "the GPU did not move");
	else if (FNL_FAULT("renderer-move-refused"))
		rc = fnl_fail(FNL_ERR_NO_DEVICE, "%s refused (FNL_FAULT)", to->name);
	else
		rc = renderer_build_here(a, to);
	/* A GPU moved to another driver and then not drawn on -- its name
	 * matched nothing, and the pick took another -- is no move: the window
	 * goes back, and the GPU to the driver it left (below). */
	if (rc == FNL_OK && !app_gpu_driver_landed(a)) {
		rc = fnl_fail(FNL_ERR_NO_DEVICE, "drawn on %s, not on the GPU "
					  "moved", a->gpu_name);
		renderer_unbuild(a);
		app_gpu_driver_unseen(a);
	}
	if (rc == FNL_OK) {
		/* The other renderers' GPUs listed again once a GPU is on another
		 * driver, or a GPU came or went: what it is called, and who can
		 * draw on it, are the driver's. Not on the way back, where the
		 * list stands -- and a listing's Vulkan instance beside OpenGL's
		 * display would terminate it (gpu_list_early()). */
		if (relist)
			a->gpu_others_listed = false;
		a->gpu_driver_moved = false;
		fnl_log(FNL_LOG_INFO, "rebuilt on %s, drawn with %s in %.0f ms "
				"(instance %.0f, surface %.0f, objects %.0f)", a->gpu_name,
				to->name, a->build_ms[0] + a->build_ms[1] + a->build_ms[2],
				a->build_ms[0], a->build_ms[1], a->build_ms[2]);
		return;
	}
	/* Back to the renderer before, on the GPU it drew on, in this turn:
	 * there is nothing of the one asked for to come back to. Marked as the
	 * way back, so that device_recover() goes back no further. A GPU moved
	 * to another driver for it goes back to its own first. */
	fnl_log(FNL_LOG_WARN, "%s would not draw here (%s); back to %s",
			to->name, fnl_last_error(), from->name);
	app_gpu_driver_undo(a);
	a->gpu_index = a->gpu_back_index;
	memcpy(a->gpu_want, a->gpu_back_want, sizeof a->gpu_want);
	a->gpu_switch_back = true;
	if (renderer_build_here(a, from) < 0) {
		a->renderer = NULL;
		a->renderer_owed = from;
	}
}

bool renderer_owed_retry(App *a)
{
	const FnlRendererVtbl *g = a->renderer_owed;
	if (renderer_build_here(a, g) < 0) {
		a->renderer = NULL;
		return false;
	}
	a->renderer_owed = NULL;
	return true;
}

/* The renderers the caller has a scene for, whatever named the one to
 * start with: the one FnlInitDesc::renderer names, else
 * FnlInitDesc::renderers, else the build's only one -- the order
 * renderer_candidates() reads them in, since a caller that names one has
 * written that scene alone and ::renderers applies only where it names
 * none. FNL_RENDERER chooses the start and not the scenes, so the list
 * still spans every renderer here. */
static uint32_t renderer_scenes(const FnlInitDesc *d)
{
	if (d->renderer != FNL_RENDERER_DEFAULT)
		return (unsigned)d->renderer < 32 ? FNL_RENDERER_BIT(d->renderer)
										  : 0;
	if (d->renderers)
		return d->renderers;
	return (RENDERER_COUNT == 1) ? FNL_RENDERER_BIT(RENDERERS[0]->api) : 0;
}

/* Whether renderer `i` of the build is one the list spans: the caller has
 * a scene for it, and it draws the titlebar, as every one chosen must. */
static bool renderer_spanned(uint32_t scenes, size_t i)
{
	return (scenes & FNL_RENDERER_BIT(RENDERERS[i]->api)) &&
		   RENDERERS[i]->draws_overlay && RENDERERS[i]->list_gpus;
}

/* The list's renderers once one is built: that one first, whose part its
 * pick has published, then the others the caller has a scene for, in the
 * build's order, their parts empty until app_gpu_list_others(). */
static void gpu_span_set(App *a, uint32_t scenes)
{
	fnl_mutex_lock(&a->lock);
	(void)gpu_slot(a, a->renderer);
	int n = 1;
	for (size_t i = 0; i < RENDERER_COUNT && n < GPU_SPAN_MAX; i++)
		if (RENDERERS[i] != a->renderer && renderer_spanned(scenes, i)) {
			a->gpu_span[n] = RENDERERS[i];
			a->gpu_part_n[n] = 0;
			n++;
		}
	a->gpu_span_n = n;
	gpu_merge(a);
	fnl_mutex_unlock(&a->lock);
}

/* A candidate that failed may have published its part; the next starts
 * the list again. */
static void gpu_span_clear(App *a)
{
	fnl_mutex_lock(&a->lock);
	a->gpu_span_n = 0;
	a->gpu_part_n[0] = 0;
	a->gpu_part_cur = -1;
	a->gpu_list_n = 0;
	a->gpu_list_cur = -1;
	fnl_atomic_store_u32(&a->gpu_count, 0);
	fnl_mutex_unlock(&a->lock);
}

/* Whether the GPU the renderer just built is drawing on is a software
 * rasteriser, by its pick's list. */
static bool gpu_in_use_soft(App *a)
{
	fnl_mutex_lock(&a->lock);
	const int cur = a->gpu_list_cur;
	const bool soft = cur >= 0 &&
					  a->gpu_list[cur].kind == FNL_GPU_KIND_SOFTWARE;
	fnl_mutex_unlock(&a->lock);
	return soft;
}

/* What `r` lists: what it listed before the one drawing was built where it
 * was (gpu_list_early()), or listed now -- timed and said where `timed`,
 * since listing loads that renderer's driver: what deciding among
 * renderers would cost at startup, said where it is paid instead. */
static int gpu_listed(App *a, const FnlRendererVtbl *r, GpuEntry *out,
					  bool timed)
{
	for (int k = 0; k < a->gpu_early_n; k++)
		if (a->gpu_early_r[k] == r) {
			memcpy(out, a->gpu_early[k],
				   (size_t)a->gpu_early_c[k] * sizeof *out);
			return a->gpu_early_c[k];
		}
	const double t0 = now_seconds();
	/* Cleared, so a field the renderer does not fill reads as none: a
	 * Direct3D 12 entry names no way through another API, and read off
	 * the stack it named garbage -- a ranking key no figure matched, and
	 * the calibration ran at every start. */
	memset(out, 0, GPU_LIST_MAX * sizeof *out);
	const int c = r->list_gpus(a, out, GPU_LIST_MAX);
	if (timed)
		fnl_log(FNL_LOG_INFO, "%s lists %d GPU%s here, in %.0f ms",
				r->name, c, c == 1 ? "" : "s",
				(now_seconds() - t0) * 1000.0);
	return c;
}

/* Where the renderers share one driver underneath (FnlBackendVtbl::
 * shared_driver), and wherever OpenGL is the renderer to draw, every
 * renderer the list spans but `drawing` is listed before `drawing` is
 * built, and what each said is what app_gpu_list_others() publishes.
 * Listed after the first frame, beside it, a Vulkan instance made and
 * destroyed while vkd3d-proton waits on a present took NVIDIA's Wayland
 * driver down with a null call; and one made and destroyed beside
 * NVIDIA's live EGL display leaves that display terminated -- every
 * eglMakeCurrent after it EGL_NOT_INITIALIZED, the frames going on only
 * while the context stays current (tools/probe/eglmove.c, 580.196.07, no
 * window server) -- so Vulkan's drivers are asked before OpenGL opens
 * one. One that fails to build is listed before the next is tried, so
 * each is listed once. */
static void gpu_list_early(App *a, uint32_t scenes,
						   const FnlRendererVtbl *drawing)
{
	if (!BACKEND(a)->shared_driver(a) &&
		drawing->api != FNL_RENDERER_OPENGL)
		return;
	for (size_t i = 0; i < RENDERER_COUNT; i++) {
		const FnlRendererVtbl *r = RENDERERS[i];
		bool listed = false;
		for (int k = 0; k < a->gpu_early_n; k++)
			listed |= a->gpu_early_r[k] == r;
		if (r == drawing || listed || !renderer_spanned(scenes, i) ||
			a->gpu_early_n == GPU_SPAN_MAX)
			continue;
		const int k = a->gpu_early_n;
		a->gpu_early_c[k] = gpu_listed(a, r, a->gpu_early[k], true);
		a->gpu_early_r[k] = r;
		a->gpu_early_n = k + 1;
	}
}

/* The first of cand[from..] that lists a GPU that is not software, with
 * its name in `name`; NULL where none does. */
static const FnlRendererVtbl *hardware_in(App *a,
										  const FnlRendererVtbl **cand,
										  size_t from, size_t n,
										  char *name, size_t size)
{
	for (size_t j = from; j < n; j++) {
		if (!cand[j]->list_gpus)
			continue;
		GpuEntry list[GPU_LIST_MAX];
		const int c = gpu_listed(a, cand[j], list, false);
		for (int k = 0; k < c; k++)
			if (list[k].kind != FNL_GPU_KIND_SOFTWARE) {
				snprintf(name, size, "%s", list[k].name);
				return cand[j];
			}
	}
	return NULL;
}

/* The order's correction (DESIGN *The choice is an order*): a renderer
 * that built on a software rasteriser gives way to a later one that lists
 * hardware -- Vulkan with lavapipe alone where OpenGL has the GPU. Asked
 * only then, so a machine whose first renderer found hardware lists
 * nothing at startup; and only where no GPU was named, which a software
 * one may be on purpose. Where the other will not build, the first is
 * built again. True with `*g` the renderer that stands. */
static bool renderer_correct(App *a, const FnlRendererVtbl **cand,
							 size_t i, size_t n, uint32_t scenes,
							 const FnlRendererVtbl **g)
{
	*g = cand[i];
	if (a->gpu_index > 0 || a->gpu_want[0] || !gpu_in_use_soft(a))
		return true;
	char soft[GPU_NAME_MAX], hard[GPU_NAME_MAX];
	snprintf(soft, sizeof soft, "%s", a->gpu_name);
	const FnlRendererVtbl *hw = hardware_in(a, cand, i + 1, n, hard,
											sizeof hard);
	if (!hw)
		return true;
	renderer_unbuild(a);
	gpu_span_clear(a);
	gpu_list_early(a, scenes, hw);
	if (renderer_build(a, hw, true) == FNL_OK) {
		fnl_log(FNL_LOG_INFO, "%s would draw on software here (%s), and "
				"%s has hardware (%s)", cand[i]->name, soft, hw->name, hard);
		*g = hw;
		return true;
	}
	fnl_log(FNL_LOG_WARN, "%s has hardware here (%s) and would not draw "
			"(%s); %s draws on software", hw->name, hard, fnl_last_error(),
			cand[i]->name);
	gpu_span_clear(a);
	return renderer_build(a, cand[i], false) == FNL_OK;
}

/* Every candidate in order, on the window that is open, stepping past one
 * that fails on this machine -- no driver, no device, a device that will
 * not present -- since the next has a scene of the caller's too. Memory
 * running out is not this renderer's and would meet the next as well. The
 * window is kept: closing it and opening another shows a window that
 * vanishes. A named renderer is taken or refused. */
FnlResult renderer_choose_and_build(App *a, const FnlInitDesc *d)
{
	const FnlRendererVtbl *cand[RENDERER_COUNT];
	size_t n;
	const char *named;
	FnlResult rc = renderer_candidates(d, cand, &n, &named);
	if (rc < 0)
		return rc;
	/* The ranked first entry's renderer tried first, where nothing named
	 * one (app_rank_start()). */
	for (size_t i = 1; i < n && !named && a->renderer_first; i++)
		if (cand[i]->api == a->renderer_first) {
			const FnlRendererVtbl *g = cand[i];
			memmove(&cand[1], &cand[0], i * sizeof cand[0]);
			cand[0] = g;
		}
	const uint32_t scenes = renderer_scenes(d);
	int spanned = 0;
	for (size_t i = 0; i < RENDERER_COUNT; i++)
		spanned += renderer_spanned(scenes, i);
	a->gpu_spans = spanned > 1;
	a->renderer_named = named ? cand[0]->api : FNL_RENDERER_DEFAULT;
	if (named) {
		fnl_log(FNL_LOG_INFO, "renderer: %s, named by %s", cand[0]->name,
				named);
		gpu_list_early(a, scenes, cand[0]);
		rc = renderer_build(a, cand[0], false);
		if (rc == FNL_OK)
			gpu_span_set(a, scenes);
		return rc;
	}
	char passed[512] = "";
	for (size_t i = 0; i < n; i++) {
		gpu_list_early(a, scenes, cand[i]);
		rc = renderer_build(a, cand[i], true);
		const FnlRendererVtbl *g;
		if (rc == FNL_OK && renderer_correct(a, cand, i, n, scenes, &g)) {
			fnl_log(FNL_LOG_INFO, "renderer: %s%s%s", g->name,
					passed[0] ? "; passed over " : "", passed);
			gpu_span_set(a, scenes);
			return FNL_OK;
		}
		if (rc == FNL_OK)
			rc = fnl_fail(FNL_ERR_NO_DEVICE, "%s would not build again",
						  cand[i]->name);
		gpu_span_clear(a);
		if (rc == FNL_ERR_OUT_OF_MEMORY)
			return rc;
		note_append(passed, sizeof passed, cand[i]->name, fnl_last_error());
	}
	return n == 1 ? rc
				  : fnl_fail(rc, "no renderer would draw here -- %s", passed);
}

void backend_noop_set_geometry(App *a, int w, int h, int margin)
{
	(void)a; (void)w; (void)h; (void)margin;
}

void backend_noop_ack_configure(App *a, uint32_t serial)
{
	(void)a; (void)serial;
}

void backend_noop_present_pixels(App *a, const void *px, int w, int h,
								 size_t pitch, bool bgra)
{
	(void)a; (void)px; (void)w; (void)h; (void)pitch; (void)bgra;
}

bool backend_noop_apply_scale(App *a)
{
	(void)a;
	return false;
}

uint32_t backend_noop_buffer_granularity(const App *a, int s120)
{
	(void)a; (void)s120;
	return 1;
}

int backend_noop_modes(const App *a, FnlMode *out, int max, int *current)
{
	(void)a; (void)out; (void)max;
	*current = -1;
	return 0;
}

int backend_noop_outputs(const App *a, char (*names)[OUTPUT_NAME_MAX],
						 int max, int *current)
{
	(void)a; (void)names; (void)max;
	*current = -1;
	return 0;
}

int backend_published_outputs(const App *a, char (*names)[OUTPUT_NAME_MAX],
							  int max, int *current)
{
	App *m = (App *)a;                 /* the lock is not const */
	fnl_mutex_lock(&m->lock);
	const int n = a->out_count < max ? a->out_count : max;
	memcpy(names, a->out_names, (size_t)(n > 0 ? n : 0) * OUTPUT_NAME_MAX);
	*current = a->out_current < n ? a->out_current : -1;
	fnl_mutex_unlock(&m->lock);
	return n;
}

void backend_noop_set_title(App *a, const char *utf8)
{
	(void)a;
	(void)utf8;
}

void backend_noop_set_mode(App *a, const FnlMode *mode)
{
	(void)a; (void)mode;
}

void backend_noop_set_output(App *a, const char *name)
{
	(void)a; (void)name;
}

FnlResult backend_noop_check_error(App *a)
{
	(void)a;
	return FNL_OK;
}

/* Seed the held set from a key that is *already* down as focus arrives,
 * without acting on it: replayed through app_key(), alt-tabbing in with
 * Escape held would close the window.
 *
 * No event either, so the frame a held-key readout needs is asked for here,
 * where the set moves. */
void app_key_seed(App *a, FnlKey k)
{
	if (!key_tracked(k) || key_held_get(a, k))
		return;
	key_held_set(a, k, true);
	fnl_invalidate(a);
}

/* For app_key_seed()'s reason: a keyboard that goes away takes the held keys
 * with it and nothing is posted to say so. */
void app_keys_clear(App *a)
{
	uint32_t held = 0;
	for (int i = 0; i < KEY_HELD_WORDS; i++) {
		held |= fnl_atomic_load_u32(&a->keys_held[i]);
		fnl_atomic_store_u32(&a->keys_held[i], 0);
	}
	if (held)
		fnl_invalidate(a);
}

/* ---- text entry ---------------------------------------------------------- */

bool app_text_input(const App *a)
{
	return fnl_atomic_load_bool(&a->text_input);
}

void app_pointer_motion(App *a, double x_pt, double y_pt)
{
	if (a->pointer_captured || app_rank_holds_input(a))
		return;
	FnlEvent ev = { .type = FNL_EVENT_MOTION };
	ev.motion.x = x_pt;
	ev.motion.y = y_pt;
	app_event(a, &ev);
}

/* Held back by a calibration as a position is: a capture taken before one
 * began goes on through it. */
/* [render] The present just made did not commit, by the graphics API's
 * word, so the backend's frame request is taken back -- unless the server
 * had answered that present already, which only the backend sees
 * (App::present_answered_anyway): then it was shown, and is counted, so
 * that the next frame does not draw under the same index. */
void app_frame_take_back(App *a)
{
	BACKEND(a)->frame_cancel(a);
	if (a->present_answered_anyway) {
		a->present_answered_anyway = false;
		a->presents++;
	}
}

void app_pointer_relative(App *a, double dx_pt, double dy_pt)
{
	if (app_rank_holds_input(a))
		return;
	FnlEvent ev = { .type = FNL_EVENT_RELATIVE };
	ev.relative.dx = dx_pt;
	ev.relative.dy = dy_pt;
	app_event(a, &ev);
}

void app_pointer_button(App *a, FnlButton b, bool pressed, double x_pt,
						double y_pt, uint32_t time_ms)
{
	/* A calibration's: a press would move the window off the GPU it
	 * measures. A release goes only for a press that went, before it --
	 * one whose press was held back would be noise to the caller. */
	const uint32_t bit = (unsigned)b < 32 ? 1u << (unsigned)b : 0;
	if (app_rank_holds_input(a) && (pressed || !(a->buttons_down & bit)))
		return;
	if (pressed)
		a->buttons_down |= bit;
	else
		a->buttons_down &= ~bit;
	FnlEvent ev = { .type = FNL_EVENT_BUTTON };
	ev.button.x = x_pt;
	ev.button.y = y_pt;
	ev.button.button = b;
	ev.button.pressed = pressed;
	app_event(a, &ev);

	if (b != FNL_BUTTON_LEFT)
		return;
	if (pressed)
		deco_press(a, x_pt, y_pt, time_ms);
	else
		deco_release(a);                        /* it stayed a click */
}

void app_pointer_scroll(App *a, double x_pt, double y_pt, double dx,
						double dy)
{
	if (app_rank_holds_input(a))
		return;
	FnlEvent ev = { .type = FNL_EVENT_SCROLL };
	ev.scroll.x = x_pt;
	ev.scroll.y = y_pt;
	ev.scroll.dx = dx;
	ev.scroll.dy = dy;
	app_event(a, &ev);
}

void app_close_requested(App *a)
{
	fnl_atomic_store_bool(&a->should_close, true);
	FnlEvent ev = { .type = FNL_EVENT_CLOSE };
	app_event(a, &ev);
}

bool app_key_repeats(FnlKey k)
{
	switch (k) {
	case FNL_KEY_UNKNOWN:
	case FNL_KEY_LSHIFT:    case FNL_KEY_RSHIFT:
	case FNL_KEY_LCTRL:     case FNL_KEY_RCTRL:
	case FNL_KEY_LALT:      case FNL_KEY_RALT:
	case FNL_KEY_LSUPER:    case FNL_KEY_RSUPER:
	case FNL_KEY_CAPS_LOCK: case FNL_KEY_NUM_LOCK:
	case FNL_KEY_SCROLL_LOCK:
		return false;
	default:
		return true;
	}
}

void app_key_repeat(App *a, FnlKey k)
{
	/* A held modifier is held, not typed again: Win32 auto-repeats Shift and
	 * Ctrl, and X11 does whatever the server's per-key setting says, so the
	 * filter is here and not in the one backend that times its own. */
	if (!app_text_input(a) || !app_key_repeats(k) ||
		app_rank_holds_input(a))
		return;
	/* Straight to the queue. app_key() is where the bindings are, and a
	 * repeat must reach none of them: the held set already has the key, and
	 * a held F11 is still one request for fullscreen. */
	FnlEvent ev = { .type = FNL_EVENT_KEY };
	ev.key.key = k;
	ev.key.pressed = true;
	ev.key.repeat = true;
	app_event(a, &ev);
}

void app_text(App *a, uint32_t cp)
{
	if (!app_text_input(a) || app_rank_holds_input(a))
		return;
	/* What a text field inserts, and nothing else. The C0 and C1 controls go
	 * because Enter, Tab, Backspace and Escape are keys, and Win32 would
	 * otherwise deliver each twice, the second time as a character. */
	if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) ||
		(cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
		return;

	FnlEvent ev = { .type = FNL_EVENT_TEXT };
	ev.text.codepoint = cp;
	/* Terminated by hand rather than left to the initialiser, which promises
	 * zeroes for the union's first member and says nothing about the rest. */
	ev.text.utf8[utf8_encode(cp, ev.text.utf8)] = '\0';
	app_event(a, &ev);
}

/* Malformed input types nothing: a replacement character put into a caller's
 * field is worse than a character missing from it. A genuine U+FFFD is not
 * malformed and arrives. */
void app_text_utf8(App *a, const char *utf8)
{
	while (utf8 && *utf8) {
		uint32_t cp;
		bool     ok;
		utf8 = utf8_decode(utf8, &cp, &ok);
		if (ok)
			app_text(a, cp);
	}
}

/* How much of a string fits in `size` bytes with its terminator, cut on a
 * character boundary. A continuation byte is never where a cut may fall. */
size_t utf8_fit(const char *s, size_t len, size_t size)
{
	if (size == 0)
		return 0;
	size_t n = len < size - 1 ? len : size - 1;
	while (n > 0 && n < len && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	return n;
}

void app_compose_set(App *a, const char *utf8)
{
	if (!utf8)
		utf8 = "";
	fnl_mutex_lock(&a->text_lock);
	const size_t n = utf8_fit(utf8, strlen(utf8), sizeof a->compose);
	const bool moved = strlen(a->compose) != n ||
					   memcmp(a->compose, utf8, n) != 0;
	if (moved) {
		memcpy(a->compose, utf8, n);
		a->compose[n] = '\0';
	}
	fnl_mutex_unlock(&a->text_lock);
	if (!moved)
		return;

	FnlEvent ev = { .type = FNL_EVENT_COMPOSE };
	ev.compose.length = (unsigned)n;
	app_event(a, &ev);
}

void app_clipboard_take(App *a, char *owned, size_t len)
{
	/* To the first terminator, if there is one inside: a C string is what
	 * the caller is handed, and some sources pad their answer. */
	if (owned) {
		const char *nul = memchr(owned, '\0', len);
		if (nul)
			len = (size_t)(nul - owned);
		owned[len] = '\0';
	} else {
		len = 0;        /* reported as an empty clipboard, which is honest */
	}

	fnl_mutex_lock(&a->text_lock);
	free(a->clip_in);
	a->clip_in     = owned;
	a->clip_in_len = len;
	fnl_mutex_unlock(&a->text_lock);

	FnlEvent ev = { .type = FNL_EVENT_CLIPBOARD };
	ev.clipboard.length = (unsigned)len;
	app_event(a, &ev);
}

void app_clipboard_deliver(App *a, const char *data, size_t len)
{
	if (!data)
		len = 0;
	char *copy = malloc(len + 1);
	if (!copy) {
		app_clipboard_take(a, NULL, 0);
		return;
	}
	if (len)
		memcpy(copy, data, len);
	app_clipboard_take(a, copy, len);
}

bool app_text_rect(const App *a, int *x, int *y, int *w, int *h)
{
	*x = (int)fnl_atomic_load_u32(&a->text_rect[0]);
	*y = (int)fnl_atomic_load_u32(&a->text_rect[1]);
	*w = (int)fnl_atomic_load_u32(&a->text_rect[2]);
	*h = (int)fnl_atomic_load_u32(&a->text_rect[3]);
	return fnl_atomic_load_bool(&a->text_rect_set);
}

/* The public half of the two strings: a copy out, on the terms fenestal.h
 * gives. Here beside what writes them, so the lock has one file. */
static size_t text_copy_out(const char *src, size_t len, char *buf,
							size_t size)
{
	if (buf && size) {
		const size_t n = src ? utf8_fit(src, len, size) : 0;
		if (n)
			memcpy(buf, src, n);
		buf[n] = '\0';
	}
	return src ? len : 0;
}

static size_t text_fetch(const FnlApp *app, bool clipboard, char *buf,
						 size_t size)
{
	App *a = (App *)app;
	if (!a)
		return text_copy_out(NULL, 0, buf, size);
	fnl_mutex_lock(&a->text_lock);
	const char  *src = clipboard ? a->clip_in : a->compose;
	const size_t len = clipboard ? a->clip_in_len : strlen(a->compose);
	const size_t n = text_copy_out(src, len, buf, size);
	fnl_mutex_unlock(&a->text_lock);
	return n;
}

size_t fnl_compose_text(const FnlApp *app, char *buf, size_t size)
{
	return text_fetch(app, false, buf, size);
}

size_t fnl_clipboard_text(const FnlApp *app, char *buf, size_t size)
{
	return text_fetch(app, true, buf, size);
}

/* Every binding this library has, in one place: a backend translates its own
 * codes into FnlKey and calls this, so the backends cannot drift over what a
 * key does. The event is posted alongside the library acting, not instead of
 * it: an application cannot suppress a binding. */
void app_key(App *a, FnlKey k, bool pressed)
{
	/* A calibration's, as a press of the pointer is (app_pointer_button()):
	 * no binding acts and nothing is posted. A release goes only for a key
	 * the caller was told went down. */
	if (app_rank_holds_input(a) && (pressed || !key_held_get(a, k)))
		return;
	FnlEvent ev = { .type = FNL_EVENT_KEY };
	ev.key.key = k;
	ev.key.pressed = pressed;
	/* A repeat never comes through here; see app_key_repeat(). */
	ev.key.repeat = false;

	key_held_set(a, k, pressed);

	if (!pressed) {
		/* No binding acts on a release, but the caller still gets it: a chord
		 * or a press-and-hold of its own needs both edges. */
		app_event(a, &ev);
		return;
	}

	/* The metrics panel's key, which is the caller's to choose
	 * (fnl_set_metrics_key()), so it is tested ahead of the fixed bindings
	 * and does not stop them: bound to F11 it toggles both. */
	if (k != FNL_KEY_UNKNOWN &&
		(uint32_t)k == fnl_atomic_load_u32(&a->metrics_key))
		fnl_set_metrics_panel(a, !fnl_atomic_load_bool(&a->metrics_shown));

	switch (k) {
	case FNL_KEY_ESCAPE:
		/* Not while a text field has the keyboard: Escape is how a field is
		 * cancelled. The event below still goes. */
		if (!app_text_input(a))
			fnl_atomic_store_bool(&a->should_close, true);
		break;
	/* F1 is deliberately absent: it belongs to whatever panel a caller draws,
	 * and reaches the application through the event queue like any other. */
	case FNL_KEY_F2:
		/* The scene's animation clock starts stopped, so a still window
		 * re-records nothing (see FnlDrawState::scene_time). */
		fnl_atomic_store_bool(&a->scene_motion,
							  !fnl_atomic_load_bool(&a->scene_motion));
		break;
	case FNL_KEY_F9:
		/* Release only. A capture starts by clicking into the presentable area
		 * and nothing the pointer does can end one, so the keyboard is the only
		 * way out -- but it is not a second way in. */
		BACKEND(a)->release_pointer(a);
		break;
	case FNL_KEY_F11:
		BACKEND(a)->set_fullscreen(a, !a->is_fullscreen);
		break;
	default:
		break;
	}

	app_event(a, &ev);
}

/* ---- public entry points ------------------------------------------------ */

/* The held set, as a question rather than a stream: for keys with a
 * duration. A press and release between two frames is an event in the queue
 * and nothing here. */
bool fnl_key_down(const FnlApp *app, FnlKey k)
{
	App *a = (App *)app;
	if (!a)
		return false;
	return key_held_get(a, k);
}

uint32_t fnl_version(void)
{
	return FNL_VERSION;
}

/* The App's, and with no window the build's where it has only one. */
FnlRenderer fnl_renderer(const App *a)
{
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	if (r)
		return r->api;
	if ((RENDERER_COUNT == 1))
		return RENDERERS[0]->api;
	return FNL_RENDERER_DEFAULT;
}

const char *fnl_gpu_name(const App *a)
{
	/* The empty string with no window, matching fnl_output_name() and
	 * fnl_backend_name(); "(no device)" is the seam's answer for a window that
	 * exists but has no device yet, which is a different thing to say. */
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	return r ? r->device_name(a) : "";
}

void app_title_copy(char dst[TITLE_MAX], const char *utf8)
{
	if (!utf8 || !utf8[0])
		utf8 = "Fenestal";
	size_t n = strlen(utf8);
	if (n > TITLE_MAX - 1) {
		n = TITLE_MAX - 1;
		/* Back to the start of the character the cut fell in: a byte
		 * 10xxxxxx continues one. */
		while (n > 0 && ((unsigned char)utf8[n] & 0xC0u) == 0x80u)
			n--;
	}
	memcpy(dst, utf8, n);
	dst[n] = '\0';
}

/* Whether `s` at `i` reads `word`. */
static bool at(const char *s, size_t i, const char *word)
{
	return strncmp(s + i, word, strlen(word)) == 0;
}

const char *app_gpu_short(char *out, size_t size, const char *name)
{
	size_t i = at(name, 0, "Mesa ") ? 5 : 0, o = 0;
	/* Up to the driver's detail, which is the first " (" -- or NVIDIA's
	 * "/PCIe/SSE2" behind its GL renderer, cut at "/PCI" and not at any
	 * slash, which a model name may have ("280/280X"). "(R)" and "(TM)"
	 * follow a word with no space, and are left out as they come. */
	while (name[i] && o + 1 < size) {
		if (at(name, i, " (") || at(name, i, "/PCI"))
			break;
		if (at(name, i, "(R)") || at(name, i, "(TM)")) {
			i += name[i + 1] == 'R' ? 3 : 4;
			continue;
		}
		out[o++] = name[i++];
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = '\0';
	/* A name that is nothing but detail, "(no device)", is kept whole. */
	return o ? out : name;
}

const char *fnl_gpu_short_name(const App *a)
{
	static FNL_THREAD_LOCAL char out[GPU_NAME_MAX];
	return app_gpu_short(out, sizeof out, fnl_gpu_name(a));
}

/* ---- the GPU list ----------------------------------------------------------
 *
 * One list over every renderer the caller has a scene for: the one the
 * window started with first, so that FnlInitDesc::gpu_index and FNL_GPU,
 * which count that renderer's own list, count this one; then the others in
 * the order fnl_init() tries them. The order is fixed for the window's
 * life where no ranking is kept (app_rank_sort() orders it by the display's
 * figures otherwise), so an index names the same GPU across a move to
 * another renderer, and across one to another kernel driver, whose entries
 * swap the GPU's rows to keep it so (app_gpu_driver_entries()). DESIGN
 * *Moving to another renderer*. */

/* `r`'s part of the list, taking slot 0 for the first renderer that
 * publishes. */
static int gpu_slot(App *a, const FnlRendererVtbl *r) FNL_REQUIRES(a->lock)
{
	for (int k = 0; k < a->gpu_span_n; k++)
		if (a->gpu_span[k] == r)
			return k;
	if (a->gpu_span_n == 0) {
		a->gpu_span[0] = r;
		a->gpu_span_n = 1;
	}
	return 0;
}

/* gpu_list made again from the parts, the GPU in use being gpu_part_cur
 * of the renderer drawing; at most GPU_LIST_MAX, the rest left off. */
static void gpu_merge(App *a) FNL_REQUIRES(a->lock)
{
	/* The published one: the listing thread merges too. */
	const int k = gpu_slot(a, renderer_any(a));
	int m = 0;
	/* A renderer's list_gpus sets a GPU's name, kind and renderer and no
	 * more; what it listed into may be a stack's, so driver_move, which
	 * makes an entry a move to another kernel driver, and the driver an
	 * entry draws on are zeroed here, where every part passes, and set by
	 * app_gpu_driver_entries() alone. */
	for (int j = 0; j < a->gpu_span_n; j++)
		for (int i = 0; i < a->gpu_part_n[j] && m < GPU_LIST_MAX; i++) {
			a->gpu_list[m] = a->gpu_part[j][i];
			a->gpu_list[m].driver[0] = '\0';
			a->gpu_list[m].slot = j;
			a->gpu_list[m].local = i;
			a->gpu_list[m++].driver_move = 0;
		}
	m += app_gpu_driver_entries(a, a->gpu_list, m, GPU_LIST_MAX);
	app_rank_sort(a, a->gpu_list, m);
	/* Found by where it came from: the moves and the ranking may have
	 * placed it anywhere. */
	int cur = -1;
	for (int i = 0; i < m && a->gpu_part_cur >= 0; i++)
		if (a->gpu_list[i].slot == k && a->gpu_list[i].local ==
				a->gpu_part_cur)
			cur = i;
	a->gpu_list_n = m;
	a->gpu_list_cur = cur;
	fnl_atomic_store_u32(&a->gpu_count, (uint32_t)m);
}

void app_gpu_list_merge(App *a)
{
	fnl_mutex_lock(&a->lock);
	gpu_merge(a);
	fnl_mutex_unlock(&a->lock);
}

/* No move to another kernel driver offered from now on, the list merged
 * again without it, and `why` said once, with the list after it: the move
 * entries gone, every entry behind one has a new number. */
void app_gpu_drivers_pin(App *a, const char *why)
{
	fnl_mutex_lock(&a->lock);
	const bool was = a->gpu_drivers_pinned;
	a->gpu_drivers_pinned = true;
	gpu_merge(a);
	fnl_mutex_unlock(&a->lock);
	if (!was) {
		fnl_log(FNL_LOG_WARN, "GPU driver: no move offered from now on: %s",
				why);
		app_gpu_list_say(a);
	}
}

static void inventory_say(App *a, int k);

void app_gpu_list_publish(App *a, const GpuEntry *list, int n, int cur)
{
	if (n > GPU_LIST_MAX)
		n = GPU_LIST_MAX;
	if (cur >= n)
		cur = -1;
	fnl_mutex_lock(&a->lock);
	const int k = gpu_slot(a, a->renderer);
	for (int i = 0; i < n; i++)
		a->gpu_part[k][i] = list[i];
	a->gpu_part_n[k] = n;
	a->gpu_part_cur = cur;
	gpu_merge(a);
	fnl_mutex_unlock(&a->lock);
	inventory_say(a, k);
}

/* What the inventory and the titlebar's list call a GPU of `k`. */
const char *app_gpu_kind_word(FnlGpuKind k)
{
	switch (k) {
	case FNL_GPU_KIND_INTEGRATED: return "integrated";
	case FNL_GPU_KIND_DISCRETE:   return "discrete";
	case FNL_GPU_KIND_VIRTUAL:    return "virtual";
	case FNL_GPU_KIND_SOFTWARE:   return "software";
	default:                      return "hardware";
	}
}

/* "Vulkan" and "vulkan" for FNL_RENDERER_VULKAN, from the build's own
 * tables. */
const char *app_renderer_name(FnlRenderer r)
{
	const FnlRendererVtbl *g = renderer_of_api(r);
	return g ? g->name : "?";
}

const char *app_renderer_key(FnlRenderer r)
{
	const FnlRendererVtbl *g = renderer_of_api(r);
	return g ? g->key : "-";
}

void app_gpu_inventory(const App *a, int i, const char *name,
					   const char *kind)
{
	/* Held until the list is merged (app_gpu_list_publish()), which is
	 * what numbers it: the moves and the ranking place a GPU anywhere. */
	App *m = (App *)a;                 /* the lock is not const */
	if (i < 0 || i >= GPU_LIST_MAX)
		return;
	if (i == 0)
		m->inv_n = 0;
	snprintf(m->inv_name[i], sizeof m->inv_name[i], "%s", name);
	snprintf(m->inv_kind[i], sizeof m->inv_kind[i], "%s", kind);
	if (i >= m->inv_n)
		m->inv_n = i + 1;
}

/* The list's number for GPU `i` of slot `k`'s part, from 1; 0 where the
 * list holds no such entry. */
static int gpu_number(const App *a, int k, int i) FNL_REQUIRES(a->lock)
{
	for (int j = 0; j < a->gpu_list_n; j++)
		if (a->gpu_list[j].slot == k && a->gpu_list[j].local == i)
			return j + 1;
	return 0;
}

/* The inventory held since the pick, said with the numbers the merged
 * list gives it. */
static void inventory_say(App *a, int k)
{
	int number[GPU_LIST_MAX];
	char via[GPU_LIST_MAX][GPU_VIA_MAX];
	const int n = a->inv_n;
	fnl_mutex_lock(&a->lock);
	for (int i = 0; i < n; i++) {
		number[i] = gpu_number(a, k, i);
		memcpy(via[i], i < a->gpu_part_n[k] ? a->gpu_part[k][i].via : "",
			   sizeof via[i]);
	}
	const bool spans = a->gpu_spans;
	fnl_mutex_unlock(&a->lock);
	for (int i = 0; i < n; i++) {
		if (via[i][0])
			fnl_log(FNL_LOG_INFO, "GPU %d: %s (%s, %s via %s)", number[i],
					a->inv_name[i], a->inv_kind[i], a->renderer->name,
					via[i]);
		else if (spans)
			fnl_log(FNL_LOG_INFO, "GPU %d: %s (%s, %s)", number[i],
					a->inv_name[i], a->inv_kind[i], a->renderer->name);
		else
			fnl_log(FNL_LOG_INFO, "GPU %d: %s (%s)", number[i],
					a->inv_name[i], a->inv_kind[i]);
	}
	a->inv_n = 0;
}

/* ---- what a frame costs ----------------------------------------------------
 *
 * Apart from how often frames are drawn, which for a window that draws
 * only when something changed is all fnl_fps() can say. The CPU's half is
 * the render thread's CPU time from the end of its waits to the present's
 * return (app_frame_mark()); the GPU's is the renderers' timestamps. Both
 * are running means over the frames drawn, kept while nothing is.
 *
 * Published (cost_*_us) and not handed out as they move: the published pair
 * is in the draw state, so a change re-records the frame that shows it,
 * which a caller could not ask for itself -- on_overlay runs only once a
 * frame is being re-recorded. At most once a COST_WINDOW while frames are
 * drawn, as fnl_fps() moves; at once where a figure was unknown, after a
 * start or a move to another GPU; and once more when the window has come to
 * rest, for the last frames' times, which the renderer reads only once they
 * are done (gpu_time_poll). That frame, drawn for the figures alone, does
 * not owe another (App::cost_counts), or a still window would draw five
 * times a second for good; nor are its times in the means, since it is
 * drawn to show them, and so is every frame the metrics panel draws at
 * rest -- unless the caller asks for exactly that while it shows them
 * (fnl_set_cost_sampling()): then a window at rest draws a frame every
 * COST_WINDOW and each counts, so the figures stay measured. Either is
 * drawn with the window left at rest (rest_owed()). */


uint64_t app_gpu_timed(App *a)
{
	a->gpu_timed = true;
	a->gpu_uncounted[a->presents % GPU_UNCOUNTED].index = a->presents;
	a->gpu_uncounted[a->presents % GPU_UNCOUNTED].uncounted =
		!a->cost_counts;
	return a->presents;
}

void app_gpu_time(App *a, double seconds, uint64_t index)
{
	if (!(seconds >= 0.0) || seconds > 4.0)
		return;
	const uint32_t us = (uint32_t)(seconds * 1e6 + 0.5);
	/* Every frame's, for the metrics panel's share of the GPU, which is
	 * what the window's frames take whatever they were drawn for. */
	a->met_gpu_s += seconds;
	/* A frame drawn only to show the figures is no frame of theirs to
	 * measure (App::cost_counts): its time stays out of the mean, and
	 * stays in the totals and its row, which account for every frame. */
	if (a->gpu_uncounted[index % GPU_UNCOUNTED].uncounted &&
		a->gpu_uncounted[index % GPU_UNCOUNTED].index == index) {
		if (a->counting) {
			a->st.gpu_us += us;
			a->st.gpu_samples++;
		}
	} else {
		fnl_atomic_store_u32(&a->gpu_sample_us, us);
		(void)fnl_atomic_add_u32(&a->gpu_sample_seq, 1);
	}
	/* For the ring's row, once the turn publishes. A turn reads at most a
	 * swapchain's images, under GPU_LATE_MAX; one past it is dropped and
	 * the row goes out without, as an unread one does. */
	if (a->counting && a->gpu_late_n < GPU_LATE_MAX) {
		a->gpu_late[a->gpu_late_n].index = index;
		a->gpu_late[a->gpu_late_n].ms = (float)(seconds * 1e3);
		a->gpu_late_n++;
	}
}

/* The newest GPU sample into the mean; true where there was one. */
static bool cost_fold(App *a)
{
	const uint32_t seq = fnl_atomic_load_u32(&a->gpu_sample_seq);
	if (seq == a->gpu_cost_seq)
		return false;
	a->gpu_cost_seq = seq;
	const uint32_t us = fnl_atomic_load_u32(&a->gpu_sample_us);
	const double s = us * 1e-6;
	if (a->counting) {
		a->st.gpu_us += us;
		a->st.gpu_samples++;
	}
	a->gpu_cost = a->gpu_cost > 0.0 ? a->gpu_cost * 0.9 + s * 0.1 : s;
	return true;
}

/* Every sample folded is a counted frame's -- one drawn only to show the
 * figures never reaches it (app_gpu_time()) -- so a fold owes a publish
 * whatever the frame being drawn now is: a counted frame's time read late,
 * after the window went to rest, is still published. */
void app_frame_cost(App *a)
{
	if (cost_fold(a) || a->cost_counts)
		a->cost_owed = true;
}

void app_cost_poll(App *a)
{
	RENDERER(a)->gpu_time_poll(a);
	if (cost_fold(a))
		a->cost_owed = true;
}

void app_cost_publish(App *a, double now, bool window)
{
	/* With the frame-rate window where it runs; at rest without sampling it
	 * does not, and the time since the last publish stands in. */
	const bool due = window ||
		(a->resting && !fnl_atomic_load_bool(&a->cost_sampling) &&
		 now - a->cost_published_at >= COST_WINDOW);
	const bool gpu_new = a->gpu_cost > 0.0 &&
		fnl_atomic_load_u32(&a->cost_gpu_us) == COST_UNKNOWN;
	const double cpu = app_cpu_cost(a);
	const bool cpu_new = cpu > 0.0 &&
		fnl_atomic_load_u32(&a->cost_cpu_us) == COST_UNKNOWN;
	if (!gpu_new && !cpu_new && !(a->cost_owed && due))
		return;
	if (a->gpu_cost > 0.0)
		fnl_atomic_store_u32(&a->cost_gpu_us,
							 (uint32_t)(a->gpu_cost * 1e6 + 0.5));
	if (cpu > 0.0)
		fnl_atomic_store_u32(&a->cost_cpu_us,
							 (uint32_t)(cpu * 1e6 + 0.5));
	a->cost_published_at = now;
	a->cost_owed = false;
}

/* A new device's figures are its own -- a GPU that takes the output and
 * one that copies onto it differ most in the CPU's -- so both means start
 * again, and say nothing until it has drawn. late_cost is the pacing's
 * and carries over a rebuild; wall_cost, the figure's, does not. */
void app_frame_cost_reset(App *a)
{
	a->gpu_cost = 0.0;
	a->gpu_cost_seq = fnl_atomic_load_u32(&a->gpu_sample_seq);
	fnl_atomic_store_u32(&a->cost_gpu_us, COST_UNKNOWN);
	a->met_gpu_at = 0.0;         /* the panel's share, from this device's */
	a->cpu_cost = 0.0;
	a->cpu_marked = false;
	a->wall_cost = 0.0;
	a->wall_mark = -1.0;
	fnl_atomic_store_u32(&a->cost_cpu_us, COST_UNKNOWN);
}

void app_frame_mark(App *a, FrameMark m)
{
	const bool cost = m != FRAME_MARK_PRESENT;
	const double cpu = cost || a->parts
					 ? thread_cpu_seconds(&a->cpu_clock) : -1.0;
	/* The wall clock only where the thread's CPU time is not known, the
	 * one place it is read (app_cpu_cost()). */
	if (a->cpu_cost <= 0.0) {
		if (m == FRAME_MARK_REC) {
			a->wall_mark = now_seconds();
		} else if (m == FRAME_MARK_PRESENT && a->wall_mark >= 0.0) {
			const double s = now_seconds() - a->wall_mark;
			if (a->cost_counts)
				a->wall_cost = a->wall_cost > 0.0
							 ? a->wall_cost * 0.9 + s * 0.1 : s;
			a->wall_mark = -1.0;
		}
	}
	if (m == FRAME_MARK_REC) {
		a->cpu_mark = cpu;
		a->cpu_marked = cpu >= 0.0;
	} else if (m == FRAME_MARK_PRESENTED && a->cpu_marked && cpu >= 0.0) {
		/* Not a frame drawn only to show the figures, as for the GPU's
		 * (app_gpu_time()). */
		const double s = cpu - a->cpu_mark;
		if (a->cost_counts)
			a->cpu_cost = a->cpu_cost > 0.0 ? a->cpu_cost * 0.9 + s * 0.1
											: s;
		a->cpu_marked = false;
	}
	parts_mark(a, m, cpu);
}

double app_cpu_cost(const App *a)
{
	return a->cpu_cost > 0.0 ? a->cpu_cost : a->wall_cost;
}

static double cost_ms(const FnlAtomicU32 *v)
{
	const uint32_t us = fnl_atomic_load_u32(v);
	return us == COST_UNKNOWN ? -1.0 : us * 1e-3;
}

double fnl_frame_gpu_ms(const App *a)
{
	return a ? cost_ms(&a->cost_gpu_us) : -1.0;
}

double fnl_frame_cpu_ms(const App *a)
{
	return a ? cost_ms(&a->cost_cpu_us) : -1.0;
}

/* Each listed by its renderer's list_gpus() (gpu_listed()), or published
 * from what it listed before the one drawing was built. On a thread of
 * its own (app_gpu_list_start()), so the renderer drawing reads the
 * published pointer. Then the whole list said, every entry with its
 * number: the parts joining may have moved the ones said at the pick (the
 * ranking orders across them), and the moves to another kernel driver are
 * said nowhere else -- `GPU 7: <GPU>, on nouveau (discrete, Vulkan)`. */
void app_gpu_list_others(App *a)
{
	const FnlRendererVtbl *span[GPU_SPAN_MAX];
	fnl_mutex_lock(&a->lock);
	const int n = a->gpu_span_n;
	for (int k = 0; k < n; k++)
		span[k] = a->gpu_span[k];
	fnl_mutex_unlock(&a->lock);

	for (int k = 0; k < n; k++) {
		if (span[k] == renderer_any(a))
			continue;
		GpuEntry got[GPU_LIST_MAX];
		const int c = gpu_listed(a, span[k], got, true);
		fnl_mutex_lock(&a->lock);
		for (int i = 0; i < c; i++)
			a->gpu_part[k][i] = got[i];
		a->gpu_part_n[k] = c;
		gpu_merge(a);
		fnl_mutex_unlock(&a->lock);
	}

	app_gpu_list_say(a);
}

void app_gpu_list_say(App *a)
{
	GpuEntry list[GPU_LIST_MAX];
	fnl_mutex_lock(&a->lock);
	const int m = a->gpu_list_n;
	memcpy(list, a->gpu_list, (size_t)m * sizeof list[0]);
	fnl_mutex_unlock(&a->lock);
	for (int i = 0; i < m; i++)
		fnl_log(FNL_LOG_INFO, "GPU %d: %s (%s, %s%s%s)", i + 1, list[i].name,
				app_gpu_kind_word(list[i].kind),
				app_renderer_name(list[i].renderer),
				list[i].via[0] ? " via " : "", list[i].via);
}

static void *gpu_lister(void *arg)
{
	App *a = arg;
	app_gpu_list_others(a);
	fnl_atomic_store_bool(&a->lister_done, true);
	return NULL;
}

/* Off the render thread: listing loads drivers, which took up to 456 ms
 * on the laptops' Windows, a frame held that long just after the window
 * opened; and a driver loaded there may leave that thread's context to
 * itself -- Vulkan's, NVIDIA's 580 among them, left OpenGL's not current.
 * Joined in render_thread_stop(), before anything it reads is torn down.
 * Where no thread will start, listed here and the renderer entered again. */
void app_gpu_list_start(App *a)
{
	a->gpu_others_listed = true;
	fnl_atomic_store_bool(&a->lister_done, false);
	a->lister_started = fnl_thread_start(&a->lister, gpu_lister, a);
	if (!a->lister_started) {
		app_gpu_list_others(a);
		RENDERER(a)->thread_enter(a);
	}
}

bool app_gpu_entry_of(App *a, int i, const FnlRendererVtbl **r, int *local)
{
	bool found = false;
	fnl_mutex_lock(&a->lock);
	if (i >= 0 && i < a->gpu_list_n && a->gpu_list[i].slot >= 0 &&
			a->gpu_list[i].slot < a->gpu_span_n) {
		*r = a->gpu_span[a->gpu_list[i].slot];
		*local = a->gpu_list[i].local;
		found = true;
	}
	fnl_mutex_unlock(&a->lock);
	return found;
}

/* Exposed because the Ubuntu Font Licence requires every copy to carry both,
 * and a host that cannot get at the text cannot comply. */
extern const char ubuntu_copyright_txt[];
extern const char ubuntu_licence_txt[];

const char *fnl_font_notice(void)  { return ubuntu_copyright_txt; }
const char *fnl_font_licence(void) { return ubuntu_licence_txt; }

/* Which device, from the caller and then from the environment. The caller
 * comes first because a program that chose a device has a reason the person
 * starting it may not know; FNL_GPU is for the program that chose nothing,
 * which is every one a harness has to put on a laptop's second GPU. */
static void gpu_choice(App *a, const FnlInitDesc *d)
{
	/* No GPU is in use until a renderer has picked one. */
	a->gpu_list_cur = -1;
	a->gpu_part_cur = -1;
	app_frame_cost_reset(a);
	fnl_atomic_store_u32(&a->cost_cpu_us, COST_UNKNOWN);
	a->cost_counts = true;
	a->gpu_index = d->gpu_index > 0 ? d->gpu_index : 0;
	a->gpu_want[0] = '\0';
	if (d->gpu_name && d->gpu_name[0]) {
		snprintf(a->gpu_want, sizeof a->gpu_want, "%s", d->gpu_name);
		return;
	}
	const char *v = getenv("FNL_GPU");
	if (a->gpu_index > 0 || !v || !v[0])
		return;
	/* A small number all the way through is an index and anything else a
	 * name: no device is called "2", and plenty are called "1050". */
	char *end;
	long n = strtol(v, &end, 10);
	if (!*end && n >= 1 && n <= 8)
		a->gpu_index = (int)n;
	else if (*end || n > 8)
		snprintf(a->gpu_want, sizeof a->gpu_want, "%s", v);
}

FnlResult fnl_init(const FnlInitDesc *desc, App **out)
{
	FnlResult rc;

	/* Each refusal recorded, as every one below is: the header sends a
	 * caller to fnl_last_error() for the reason. */
	if (!out)
		return fnl_fail(FNL_ERR_INVALID_ARGUMENT, "fnl_init(): out is NULL");
	*out = NULL;
	app_thread_set(APP_THREAD_MAIN);
	FNL_FAULT_INIT();
	if (!desc)
		return fnl_fail(FNL_ERR_INVALID_ARGUMENT, "fnl_init(): desc is NULL");
	/* A caller from the future is refused rather than read to the end of
	 * what this build knows. A smaller descriptor is an older caller, and
	 * everything past its end takes the default. */
	if (desc->size > sizeof *desc)
		return fnl_fail(FNL_ERR_VERSION_MISMATCH,
						"FnlInitDesc is %zu bytes; this build knows %zu",
						desc->size, sizeof *desc);
	/* Too small to carry the two fields every version of this struct has
	 * had, so there is nothing to read and nothing to default from. */
	if (desc->size < offsetof(FnlInitDesc, log))
		return fnl_fail(FNL_ERR_INVALID_ARGUMENT,
						"FnlInitDesc::size is %zu; pass sizeof "
						"(FnlInitDesc)", desc->size);
	/* And a whole number of fields: the copy below takes `size` bytes, so a
	 * size landing inside a field leaves it partly written -- the log
	 * callback, for one. Every sizeof is a multiple of its type's alignment
	 * and no member here is larger than its own, so the test is sufficient. */
	if (desc->size % FNL_ALIGNOF(FnlInitDesc) != 0)
		return fnl_fail(FNL_ERR_INVALID_ARGUMENT,
						"FnlInitDesc::size is %zu, which is not a size this "
						"struct has ever had; pass sizeof (FnlInitDesc)",
						desc->size);
	/* Before 1.0 a minor release may break the one before it (fenestal.h
	 * *stability*), so there the minor has to match as well as the major. */
	unsigned vshift = FNL_VERSION_MAJOR == 0 ? 8 : 16;
	if ((desc->header_version >> vshift) != ((uint32_t)FNL_VERSION >> vshift))
		return fnl_fail(FNL_ERR_VERSION_MISMATCH,
						"header is version %u.%u, library is %u.%u",
						desc->header_version >> 16,
						(desc->header_version >> 8) & 0xff,
						FNL_VERSION_MAJOR, FNL_VERSION_MINOR);

	/* Copied into a zeroed one of this build's size before any field is
	 * read: an older caller's descriptor stops short of the newer fields. */
	FnlInitDesc d;
	memset(&d, 0, sizeof d);
	memcpy(&d, desc, desc->size);

	/* The log sink first, so that everything below can report through it --
	 * including the failures of this very function. */
	fnl_set_log_fn(d.log, d.log_ud);

	/* Aligned rather than plain malloc: App has _Alignas(64) members keeping
	 * what each thread writes on cache lines of its own, and malloc guarantees
	 * only max_align_t. See compiler.h for the wrapper. */
	App *a = fnl_aligned_alloc(FNL_ALIGNOF(App), sizeof(App));
	if (!a)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "out of memory for the app");
	memset(a, 0, sizeof(App));

	/* Vsync is the window's for its life; FNL_VSYNC=0 is the one way out,
	 * a diagnostic read once, as FNL_ECHO_EVENTS is. */
	const char *vs = getenv("FNL_VSYNC");
	fnl_atomic_store_bool(&a->vsync, !(vs && vs[0] == '0' && !vs[1]));
	fnl_atomic_store_u32(&a->veil_out_ms, VEIL_MS);
	fnl_atomic_store_u32(&a->veil_in_ms, VEIL_MS);
	/* The metrics panel hidden, on F5 until the caller binds another key,
	 * and none of its process figures known yet. */
	fnl_atomic_store_u32(&a->metrics_key, (uint32_t)FNL_KEY_F5);
	a->met_cpu_pm = a->met_ram_mib = a->met_vram_mib = METRICS_UNKNOWN;
	a->met_gpu_pm = METRICS_UNKNOWN;
	a->met_cpu_s = -1.0;
	a->wall_mark = -1.0;
	a->want_scale120 = a->draw_scale120 = a->applied_scale120 = 120;

	/* The library's own bar until a caller replaces it. Zero would be a
	 * window with no titlebar band at all, a state this library does not
	 * have and every geometry site would then compute from. */
	a->deco_h            = DECO_TITLEBAR_H;
	a->late_latch        = !d.no_late_latch;
	a->late_fixed        = d.late_latch_fixed;
	/* "No measurement", which the memset cannot express: zero is a
	 * legitimate reading for either. See App::present_block. */
	a->present_block     = -1.0;
	a->late_held         = -1.0;
	/* The one figure every layout number is multiplied by, so it is held to
	 * app.h's range first: past it scale_pt() is a signed overflow. Clamped
	 * and said out loud rather than refused. */
	a->scale_override120 = d.scale_override120;
	if (a->scale_override120 > 0) {
		const int s = clamp_scale120(a->scale_override120);
		if (s != a->scale_override120)
			fnl_log(FNL_LOG_WARN, "scale_override120 is %d, which is outside "
								  "%d to %d; using %d",
					a->scale_override120, SCALE120_MIN, SCALE120_MAX, s);
		a->scale_override120 = s;
	}
	gpu_choice(a, &d);
	a->frame_times       = d.frame_times;
	parts_open(a);
	a->echo_events       = d.echo_events || echo_wanted_by_env();

	/* Copied: the caller's string may be a local, and it is read long after
	 * fnl_init() has returned. Truncation is silent. */
	app_title_copy(a->title, d.title);

	/* Bounded like every point figure a caller hands over (see clamp_pt in
	 * app.h); at or below zero is the library's default. */
	a->init_w_pt = d.width_pt  > 0 ? clamp_pt(d.width_pt)  : 0;
	a->init_h_pt = d.height_pt > 0 ? clamp_pt(d.height_pt) : 0;

	/* Which renderers this caller may be handed, checked before anything is
	 * built: one it has no scene for is refused rather than substituted.
	 * The backend is chosen at open() and the renderer on its window,
	 * below. */
	if ((rc = renderer_check_choice(&d)) < 0)
		goto fail;

	/* Before the window: the decoration layout measures the title while
	 * handling the very first configure. Unconditional -- the table is the
	 * library's, not whichever renderer draws text, and cell_for()
	 * dereferences it. */
	if ((rc = text_load_cells()) < 0)
		goto fail;

	/* Measured here, on the one thread that exists, because both of them ask
	 * afterwards: the render thread draws the title and the input thread
	 * hit-tests around it (see deco_title_fits). */
	fnl_atomic_store_u32(&a->title_w,
						 (uint32_t)text_width(a->title, TEXT_UI));

	fnl_mutex_init(&a->lock);
	fnl_mutex_init(&a->ev_lock);
	fnl_mutex_init(&a->text_lock);
	/* Which clock the waits run on is the shim's business -- see thread.h. */
	fnl_cond_init(&a->cond);
	a->sync_ready = true;

	/* The clock every event and every frame is stamped against. Started
	 * before open(), which dispatches while it blocks on a round trip and so
	 * can queue events from inside it. */
	a->started_at = now_seconds();
	fnl_atomic_store_u32(&a->input_lat_us, COST_UNKNOWN);

	if ((rc = backend_choose_and_open(a)) < 0)
		goto fail;
	/* Recorded as well as done: the backends' teardown asks whether there is a
	 * handler that could still reach app_wakeup() before it decides whether
	 * its wakeup descriptors may be closed. See App::installed_signals. */
	if (d.install_signal_handlers) {
		a->installed_signals = true;
		install_signal_handlers(a);
	}
	BACKEND(a)->update_scale(a);

	/* Before any renderer: a GPU moved to another driver is a GPU every
	 * renderer then finds on it. */
	app_gpu_drivers_start(a);
	/* Before the pick, which a ranking may take an index from. */
	app_rank_start(a);

	if ((rc = renderer_choose_and_build(a, &d)) < 0)
		goto fail;

	/* The floor grew with the GPU list the build published: the GPU box
	 * joins the bar once a renderer has listed its GPUs (ctl_run_w()), and
	 * open() told the window server the floor without it. */
	BACKEND(a)->apply_size_bounds(a);

	/* The buffer's ceiling is the device's, so the scale is resolved against
	 * it only from here on (see clamp_device_scale120). The render thread
	 * rebuilds on its first turn if the answer moved. */
	BACKEND(a)->update_scale(a);

	*out = a;
	return FNL_OK;

fail:
	app_teardown(a);
	fnl_aligned_free(a);
	return rc;
}

void fnl_shutdown(App *a)
{
	if (!a)
		return;
	/* From a callback, which the header forbids: teardown joins the render
	 * thread, and joining it from itself never returns; and from the
	 * outermost pair, on this thread, it frees the App fnl_run() goes on
	 * using. Refused with a line rather than hung or freed. */
	if (app_thread() == APP_THREAD_RENDER || a->in_run) {
		fnl_log(FNL_LOG_ERROR, "fnl_shutdown() called from a callback; "
				"it must be called after fnl_run() returns");
		return;
	}
	app_teardown(a);
	fnl_aligned_free(a);
}

/* Null-tolerant, like every public entry point: a caller holding a handle
 * past fnl_shutdown(), or one fnl_init() never gave it, gets nothing done
 * rather than a fault. */
void fnl_quit(App *a)
{
	if (!a)
		return;
	fnl_atomic_store_bool(&a->should_close, true);
	app_wakeup(a);
}

FnlResult fnl_run(App *a, const FnlCallbacks *cb)
{
	if (!a)
		return FNL_ERR_INVALID_ARGUMENT;
	/* The pump runs here, and everything the window server delivers is
	 * attributed to the thread fnl_init() ran on; another thread would pump
	 * a connection it did not open. */
	if (app_thread() != APP_THREAD_MAIN)
		return fnl_fail(FNL_ERR_INVALID_ARGUMENT, "fnl_run() must be called "
						"on the thread that called fnl_init()");
	if (a->in_run)
		return fnl_fail(FNL_ERR_INVALID_ARGUMENT, "fnl_run() called from a "
						"callback of its own");

	/* Copied, not referenced: the render thread reads these every frame, and
	 * the caller's FnlCallbacks may be a local. The three size tests are
	 * fnl_init()'s, for its reasons. */
	if (cb) {
		if (cb->size > sizeof *cb)
			return fnl_fail(FNL_ERR_VERSION_MISMATCH,
							"FnlCallbacks is %zu bytes; this build knows %zu",
							cb->size, sizeof *cb);
		if (cb->size < offsetof(FnlCallbacks, on_gpu_created))
			return fnl_fail(FNL_ERR_INVALID_ARGUMENT,
							"FnlCallbacks::size is %zu; set it to sizeof "
							"(FnlCallbacks)", cb->size);
		if (cb->size % FNL_ALIGNOF(FnlCallbacks) != 0)
			return fnl_fail(FNL_ERR_INVALID_ARGUMENT,
							"FnlCallbacks::size is %zu, which is not a size "
							"this struct has ever had; set it to sizeof "
							"(FnlCallbacks)", cb->size);
		memcpy(&a->cb, cb, cb->size);
	}

	/* fnl_init() built the device, but a caller cannot have handed over
	 * callbacks before fnl_run(), so the first on_gpu_created is here. The
	 * flag is set with or without a callback, so that a caller which
	 * registered only on_gpu_lost gets exactly one per loss. See
	 * App::cb_gpu_created. */
	app_aa_settle(a);
	a->in_run = true;
	a->cb_gpu_created = true;
	if (a->cb.on_gpu_created)
		a->cb.on_gpu_created(a->cb.ud, a);

	/* Anything asked for between fnl_init() and here, spent before the
	 * render thread can latch the state without it -- or a titlebar replaced
	 * before fnl_run() gets a first frame wearing the library's own. */
	app_drain_requests(a);

	/* Handed to the render thread, which takes it at the top of its loop;
	 * taken back below, after the join. See FnlRendererVtbl::thread_enter. */
	RENDERER(a)->thread_leave(a);
	if (!fnl_thread_start(&a->render_thread, render_loop, a)) {
		RENDERER(a)->thread_enter(a);
		/* on_gpu_created has run, so the pair is balanced here:
		 * fnl_shutdown() destroys the device objects and never calls back. */
		a->cb_gpu_created = false;
		if (a->cb.on_gpu_lost) {
			RENDERER(a)->wait_idle(a);
			a->cb.on_gpu_lost(a->cb.ud, a);
		}
		a->in_run = false;
		return fnl_fail(FNL_ERR_SYSTEM, "could not start the render thread");
	}
	a->render_thread_started = true;

	FnlResult rc = FNL_OK;
	while (!fnl_atomic_load_bool(&a->should_close)) {
		BACKEND(a)->pump(a);
		/* After the pump rather than before it: a request made from the frame
		 * that was drawn while the pump blocked should be spent on the turn
		 * that wakes, not held for the one after. */
		app_drain_requests(a);
		if ((rc = BACKEND(a)->check_error(a)) < 0)
			break;
	}

	/* Joined here rather than left to fnl_shutdown, so that a caller which
	 * runs, inspects and only then shuts down is not reading state a live
	 * render thread is still writing. */
	render_thread_stop(a);
	/* For the last on_gpu_lost below and for the teardown, both of which
	 * are this thread's. None where a move to another renderer left one
	 * owed and unbuilt (renderer_swap()). */
	if (a->renderer && a->renderer_ready)
		RENDERER(a)->thread_enter(a);

	/* After the join *and* after the device has drained: the join says
	 * nothing about the queue, and a caller destroying its pipelines here
	 * would destroy objects a submitted command buffer is still executing
	 * (VUID-vkDestroyPipeline-pipeline-00765). Only where the caller still
	 * holds the device: an unrecovered loss has delivered its on_gpu_lost. */
	if (a->renderer_ready && a->cb_gpu_created) {
		RENDERER(a)->wait_idle(a);
		a->cb_gpu_created = false;
		if (a->cb.on_gpu_lost)
			a->cb.on_gpu_lost(a->cb.ud, a);
	}

	a->in_run = false;
	return rc;
}
