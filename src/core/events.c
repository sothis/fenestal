/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The event queue: one stream carrying what the user did and what the library
 * did about it, produced by two threads and drained by one. DESIGN *The event
 * queue* has why it is a queue and not a callback.
 *
 *   input thread    everything the window server delivers, from app_event().
 *   render thread   the notifications, once a frame from events_post_state().
 *
 * Both push under ev_lock, which is *not* App::lock: that one is held across
 * the frame's whole latch, and an input burst blocking on it would put
 * pointer latency behind the render thread's turn.
 *
 * Motion coalesces and nothing else does: the newest position *is* the
 * position and summed deltas *are* the movement, while merging a press with
 * anything would lose an edge.
 *
 * On overflow the newest is dropped, not the oldest -- that would leave a
 * release for a press nobody saw -- and the count comes back as one
 * FNL_EVENT_LOST.
 *
 * Each slot also carries when the inputs in it were made, so that handing
 * them over measures fnl_input_latency_ms(): from the window server's or the
 * device's stamp on each (app_input_age()), or from when the pump read it,
 * to fnl_poll_event(). DESIGN *Input latency*.
 */

#include "core/app.h"

/* ---- the ring ------------------------------------------------------------ */

/* Every queue_* takes ev_lock as held; -Wthread-safety checks the callers. */
static bool queue_empty(const App *a) FNL_REQUIRES(a->ev_lock)
{
	return a->ev_head == a->ev_tail;
}

static int queue_count(const App *a) FNL_REQUIRES(a->ev_lock)
{
	return (a->ev_head - a->ev_tail + EVENT_QUEUE_MAX) % EVENT_QUEUE_MAX;
}

/* The event most recently pushed, or NULL when there is none. What coalescing
 * merges into. */
static FnlEvent *queue_back(App *a) FNL_REQUIRES(a->ev_lock)
{
	if (queue_empty(a))
		return NULL;
	return &a->ev_ring[(a->ev_head + EVENT_QUEUE_MAX - 1) % EVENT_QUEUE_MAX];
}

/* The inputs `in` carries into what slot `b` does. */
static void born_add(EvBorn *b, const EvBorn *in)
{
	if (in->n == 0)
		return;
	if (b->n == 0 || in->oldest < b->oldest)
		b->oldest = in->oldest;
	b->sum += in->sum;
	b->n += in->n;
	b->stamped += in->stamped;
}

/* FnlEvent::input_age: how long before an event's `time` the oldest input
 * it carries happened, none for a notification. Never below zero, which a
 * stamp in the rounding of its millisecond can be. */
static float input_age(double time, const EvBorn *in)
{
	if (in->n == 0 || !(time > in->oldest))
		return 0.0f;
	return (float)(time - in->oldest);
}

/* `in` is the inputs in `ev`, none for a notification (app_event()). */
static bool queue_push(App *a, const FnlEvent *ev, const EvBorn *in)
	FNL_REQUIRES(a->ev_lock)
{
	FNL_ASSERT(a->ev_head >= 0 && a->ev_head < EVENT_QUEUE_MAX);
	FNL_ASSERT(a->ev_tail >= 0 && a->ev_tail < EVENT_QUEUE_MAX);

	/* The merged event takes the newer time stamp: it describes the state as
	 * of now. Its inputs are each counted, so the latency is every motion's
	 * and not the newest's alone, which would read short by however long
	 * the queue held the rest. */
	/* Nothing after a gap goes in until the gap is reported: a slot the
	 * consumer frees meanwhile would otherwise take the release of a press
	 * that was dropped, ahead of FNL_EVENT_LOST, and a motion would merge
	 * into one from before it. fnl_poll_event() clears the count only from
	 * an empty ring. */
	if (a->ev_lost) {
		a->ev_lost++;
		a->ev_clip_owed |= ev->type == FNL_EVENT_CLIPBOARD;
		return false;
	}

	FnlEvent *back = queue_back(a);
	if (back && back->type == ev->type) {
		EvBorn *b = &a->ev_born[(a->ev_head + EVENT_QUEUE_MAX - 1) %
								EVENT_QUEUE_MAX];
		if (ev->type == FNL_EVENT_MOTION) {
			back->motion = ev->motion;
			back->time   = ev->time;
			born_add(b, in);
			back->input_age = input_age(back->time, b);
			return true;
		}
		if (ev->type == FNL_EVENT_RELATIVE) {
			back->relative.dx += ev->relative.dx;
			back->relative.dy += ev->relative.dy;
			back->time = ev->time;
			born_add(b, in);
			back->input_age = input_age(back->time, b);
			return true;
		}
	}

	/* One slot is always left free, which is what makes head == tail mean
	 * empty rather than ambiguous. */
	if (queue_count(a) >= EVENT_QUEUE_MAX - 1) {
		a->ev_lost++;
		a->ev_clip_owed |= ev->type == FNL_EVENT_CLIPBOARD;
		return false;
	}

	a->ev_ring[a->ev_head] = *ev;
	memset(&a->ev_born[a->ev_head], 0, sizeof a->ev_born[0]);
	born_add(&a->ev_born[a->ev_head], in);
	a->ev_ring[a->ev_head].input_age = input_age(ev->time, in);
	a->ev_head = (a->ev_head + 1) % EVENT_QUEUE_MAX;
	return true;
}

int app_events_pending(App *a)
{
	fnl_mutex_lock(&a->ev_lock);
	const int n = queue_count(a);
	fnl_mutex_unlock(&a->ev_lock);
	return n;
}

/* ---- what the backends call ---------------------------------------------- */

/* What fnl_input_latency_ms() counts: what a hand did, once. Not a repeat,
 * which the library times; not text or a composition, which the key that
 * made it has counted already; and none of the library's notifications. */
static bool counts_as_input(const FnlEvent *ev)
{
	switch (ev->type) {
	case FNL_EVENT_KEY:
		return !ev->key.repeat;
	case FNL_EVENT_BUTTON:
	case FNL_EVENT_MOTION:
	case FNL_EVENT_RELATIVE:
	case FNL_EVENT_SCROLL:
		return true;
	default:
		return false;
	}
}

void app_input_age(App *a, double age)
{
	FNL_ASSERT_THREAD(APP_THREAD_MAIN);
	if (!(age >= -INPUT_AGE_SLACK) || age > INPUT_AGE_MAX) {
		a->in_stamped_at = 0.0;
		return;
	}
	const double now = now_seconds();
	const double born = now - a->started_at - (age > 0.0 ? age : 0.0);
	a->in_stamped_at = now;
	const EvBorn stamp = { .sum = born, .oldest = born, .n = 1 };
	a->in_born = stamp;
}

void app_input_ages(App *a, double sum, double oldest, uint32_t n)
{
	FNL_ASSERT_THREAD(APP_THREAD_MAIN);
	if (n == 0 || !(sum >= 0.0) || !(oldest >= 0.0) ||
		oldest > INPUT_AGE_MAX) {
		a->in_stamped_at = 0.0;
		return;
	}
	const double now = now_seconds();
	const double t = now - a->started_at;
	a->in_stamped_at = now;
	const EvBorn stamp = { .sum = t * n - sum, .oldest = t - oldest,
						   .n = n };
	a->in_born = stamp;
}

void app_event(App *a, FnlEvent *ev)
{
	FNL_ASSERT_THREAD(APP_THREAD_MAIN);
	const double now = now_seconds();
	ev->time = now - a->started_at;

	/* From the input's own stamp where the backend gave one for it, and
	 * from now where not. Never later than now, nor before the clock's
	 * start, which a stamp from before fnl_init() would be. */
	EvBorn in = { 0 };
	if (counts_as_input(ev)) {
		const EvBorn one = { .sum = ev->time, .oldest = ev->time, .n = 1 };
		in = one;
		if (a->in_stamped_at > 0.0 &&
			now - a->in_stamped_at <= INPUT_STAMP_FRESH &&
			a->in_born.oldest >= 0.0) {
			in = a->in_born;
			in.stamped = in.n;
			/* One input's stamp stands while its handler runs, for each
			 * event that handler posts; several inputs' are one event's. */
			if (in.n > 1)
				a->in_stamped_at = 0.0;
		}
	}

	fnl_mutex_lock(&a->ev_lock);
	queue_push(a, ev, &in);
	fnl_mutex_unlock(&a->ev_lock);

	/* A loop at rest draws the frame that drains it (see events_pending).
	 * After ev_lock, never inside it: the two are not nested anywhere. The
	 * wake is one a batch: rest_wait() clears ev_wake when it next comes
	 * to rest, so an event with the flag already set -- a motion report,
	 * mostly -- wakes nothing. */
	/* The flag is atomic, so the burst after the first report is not a
	 * lock and a signal apiece: only the post that finds it clear takes
	 * App::lock. The signal is still sent under that lock after the flag
	 * is set, so a poster and the waiter clearing it cannot lose each
	 * other (rest_wait()). */
	if (!fnl_atomic_load_bool(&a->ev_wake) &&
	    fnl_atomic_add_u32(&a->ev_wake, 1) == 0) {
		/* The lock is only the barrier rest_wait() relies on -- the waiter
		 * is inside its wait, or has not yet read the flag -- and the
		 * signal goes after it: sent under the lock, the woken thread
		 * finds it held and sleeps again, a second wake on the path an
		 * input event takes to a loop at rest. */
		fnl_mutex_lock(&a->lock);
		fnl_mutex_unlock(&a->lock);
		fnl_cond_signal(&a->cond);
	}
}

/* The library's own notification, posted from the render thread: the same
 * queue as app_event(), without its input stamping, which is the main
 * thread's bookkeeping and not the renderer's to touch. Today the one caller
 * is the image lost beside a device's (frame.c's device_recover()). */
void app_event_post(App *a, FnlEvent *ev)
{
	FNL_ASSERT_THREAD(APP_THREAD_RENDER);
	static const EvBorn none;
	ev->time = now_seconds() - a->started_at;
	fnl_mutex_lock(&a->ev_lock);
	queue_push(a, ev, &none);
	fnl_mutex_unlock(&a->ev_lock);
	/* As app_event(): a loop at rest draws the frame that drains it. */
	/* The flag is atomic, so the burst after the first report is not a
	 * lock and a signal apiece: only the post that finds it clear takes
	 * App::lock. The signal is still sent under that lock after the flag
	 * is set, so a poster and the waiter clearing it cannot lose each
	 * other (rest_wait()). */
	if (!fnl_atomic_load_bool(&a->ev_wake) &&
	    fnl_atomic_add_u32(&a->ev_wake, 1) == 0) {
		/* The lock is only the barrier rest_wait() relies on -- the waiter
		 * is inside its wait, or has not yet read the flag -- and the
		 * signal goes after it: sent under the lock, the woken thread
		 * finds it held and sleeps again, a second wake on the path an
		 * input event takes to a loop at rest. */
		fnl_mutex_lock(&a->lock);
		fnl_mutex_unlock(&a->lock);
		fnl_cond_signal(&a->cond);
	}
}

/* ---- what the frame calls ------------------------------------------------
 *
 * Everything the library can change behind an application's back, diffed once
 * a frame against what was last reported. A diff and not a post at each call
 * site: the sites are spread across five backends, the frame loop and the
 * public setters, and all they share is that the change shows up in the
 * figures the render thread publishes. So a change is reported on the frame
 * after the one that made it. */
void events_post_state(App *a, const FnlDrawState *ds, int area_w_pt,
					   int area_h_pt, int area_w_px, int area_h_px)
{
	FNL_ASSERT_THREAD(APP_THREAD_RENDER);
	const int s120 = scale120(a);
	/* The GPU in use, taken under App::lock before ev_lock, never inside
	 * it. */
	char gpu_now[GPU_NAME_MAX] = "";
	fnl_mutex_lock(&a->lock);
	const int gpu_cur = a->gpu_list_cur;
	if (gpu_cur >= 0)
		memcpy(gpu_now, a->gpu_list[gpu_cur].name, sizeof gpu_now);
	const FnlRenderer gpu_rnd = gpu_cur >= 0
							  ? a->gpu_list[gpu_cur].renderer
							  : FNL_RENDERER_DEFAULT;
	const int gpu_n = a->gpu_list_n;
	/* The output's colour, under the same lock: the renderer writes the
	 * space and the unit when it builds the swapchain, and a backend or
	 * renderer the peak as it learns it. */
	const FnlOutputColor color = a->pub_color;
	/* And the output list's generation and length, for the same reason. */
	const uint32_t out_list_gen = a->out_list_gen;
	const int out_list_n        = a->out_count;
	fnl_mutex_unlock(&a->lock);

	/* Seeded rather than compared on the first frame, or an application
	 * would open on a burst of notifications describing the state it was
	 * handed at init. */
	if (!a->ev_state_seeded) {
		a->ev_state_seeded = true;
		a->ev_last_w_pt = area_w_pt;   a->ev_last_h_pt = area_h_pt;
		a->ev_last_w_px = area_w_px;   a->ev_last_h_px = area_h_px;
		a->ev_last_scale120  = s120;
		a->ev_last_fullscreen = ds->is_fullscreen;
		a->ev_last_maximized  = ds->is_maximized;
		a->ev_last_aa         = ds->aa;
		a->ev_last_scene_w    = ds->scene_w;
		a->ev_last_scene_h    = ds->scene_h;
		a->ev_last_captured   = ds->pointer_captured;
		memcpy(a->ev_last_output, ds->output, sizeof a->ev_last_output);
		a->ev_last_out_w_pt   = ds->out_w_pt;
		a->ev_last_out_h_pt   = ds->out_h_pt;
		a->ev_last_out_list_gen = out_list_gen;
		memcpy(a->ev_last_gpu, gpu_now, sizeof a->ev_last_gpu);
		a->ev_last_gpu_renderer = gpu_rnd;
		a->ev_last_gpu_n = gpu_n;
		a->ev_last_gpu_cur = gpu_cur;
		a->ev_last_color_space = (int)color.space;
		a->ev_last_sdr_scale   = color.sdr_scale;
		return;
	}

	/* The ev_last_* block is the render thread's alone, so the diff needs no
	 * lock; the lock and the clock are for a frame where something moved. */
	const bool d_size    = area_w_pt != a->ev_last_w_pt ||
						   area_h_pt != a->ev_last_h_pt ||
						   area_w_px != a->ev_last_w_px ||
						   area_h_px != a->ev_last_h_px;
	const bool d_scale   = s120 != a->ev_last_scale120;
	const bool d_window  = ds->is_fullscreen != a->ev_last_fullscreen ||
						   ds->is_maximized  != a->ev_last_maximized;
	const bool d_aa      = ds->aa != a->ev_last_aa;
	/* What was built, not what was asked for: ds->scene_* is the extent that
	 * exists. */
	const bool d_scene   = ds->scene_w != a->ev_last_scene_w ||
						   ds->scene_h != a->ev_last_scene_h;
	const bool d_capture = ds->pointer_captured != a->ev_last_captured;
	/* By name and by size: two monitors alike in size are still two. */
	const bool d_output  = strcmp(ds->output, a->ev_last_output) != 0 ||
						   ds->out_w_pt != a->ev_last_out_w_pt ||
						   ds->out_h_pt != a->ev_last_out_h_pt;
	/* By name, renderer and place in the list: a rebuild back onto the
	 * same GPU, after a loss or a move that went back, is no event, and
	 * the list ordered again for another display, which moves the GPU in
	 * use to another place, is one -- the caller's index is stale. And not
	 * while the veil is up: the event says frames are drawn on the GPU, and
	 * a caller or a harness that reads one then should find the frame and
	 * not the fade; the difference stands until the veil is down
	 * (App::veil_phase). */
	const bool d_gpu     = (strcmp(gpu_now, a->ev_last_gpu) != 0 ||
							gpu_rnd != a->ev_last_gpu_renderer ||
							gpu_n != a->ev_last_gpu_n ||
							gpu_cur != a->ev_last_gpu_cur) &&
						   a->veil_phase == VEIL_NONE;
	/* By the space and the target's unit: a peak that moved with the
	 * display's headroom is the same space, and the frames carry it. */
	const bool d_color   = (int)color.space != a->ev_last_color_space ||
						   color.sdr_scale != a->ev_last_sdr_scale;
	/* The list's own names, which the current output's name above does not
	 * see: a monitor that came or went is not the one the window is on. */
	const bool d_out_list = out_list_gen != a->ev_last_out_list_gen;
	if (!d_size && !d_scale && !d_window && !d_aa && !d_scene && !d_capture &&
		!d_output && !d_out_list && !d_gpu && !d_color)
		return;

	FnlEvent ev;
	const EvBorn born0 = { 0 };
	const double t = now_seconds() - a->started_at;

	fnl_mutex_lock(&a->ev_lock);

	if (d_size) {
		const FnlEvent e = { .type = FNL_EVENT_RESIZE, .time = t };
		ev = e;
		ev.resize.w_pt = area_w_pt;    ev.resize.h_pt = area_h_pt;
		ev.resize.w_px = area_w_px;    ev.resize.h_px = area_h_px;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_w_pt = area_w_pt;   a->ev_last_h_pt = area_h_pt;
			a->ev_last_w_px = area_w_px;   a->ev_last_h_px = area_h_px;
		}
	}

	if (d_scale) {
		const FnlEvent e = { .type = FNL_EVENT_SCALE, .time = t };
		ev = e;
		ev.scale.scale120 = s120;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_scale120 = s120;
		}
	}

	if (d_window) {
		const FnlEvent e = { .type = FNL_EVENT_WINDOW, .time = t };
		ev = e;
		ev.window.fullscreen = ds->is_fullscreen;
		ev.window.maximized  = ds->is_maximized;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_fullscreen = ds->is_fullscreen;
			a->ev_last_maximized  = ds->is_maximized;
		}
	}

	if (d_aa) {
		const FnlEvent e = { .type = FNL_EVENT_ANTIALIAS, .time = t };
		ev = e;
		ev.antialias.aa = ds->aa;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_aa = ds->aa;
		}
	}

	if (d_scene) {
		const FnlEvent e = { .type = FNL_EVENT_SCENE_SIZE, .time = t };
		ev = e;
		ev.scene.w_px = ds->scene_w;
		ev.scene.h_px = ds->scene_h;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_scene_w = ds->scene_w;
			a->ev_last_scene_h = ds->scene_h;
		}
	}

	if (d_capture) {
		const FnlEvent e = { .type = FNL_EVENT_CAPTURE, .time = t };
		ev = e;
		ev.capture.captured = ds->pointer_captured;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_captured = ds->pointer_captured;
		}
	}

	if (d_output) {
		const FnlEvent e = { .type = FNL_EVENT_OUTPUT, .time = t };
		ev = e;
		ev.output.w_pt = ds->out_w_pt;
		ev.output.h_pt = ds->out_h_pt;
		if (queue_push(a, &ev, &born0)) {
			memcpy(a->ev_last_output, ds->output, sizeof a->ev_last_output);
			a->ev_last_out_w_pt = ds->out_w_pt;
			a->ev_last_out_h_pt = ds->out_h_pt;
		}
	}

	if (d_out_list) {
		const FnlEvent e = { .type = FNL_EVENT_OUTPUT_LIST, .time = t };
		ev = e;
		ev.output_list.count = out_list_n;
		if (queue_push(a, &ev, &born0))
			a->ev_last_out_list_gen = out_list_gen;
	}

	if (d_gpu) {
		const FnlEvent e = { .type = FNL_EVENT_GPU, .time = t };
		ev = e;
		ev.gpu.index = gpu_cur;
		ev.gpu.renderer = fnl_renderer(a);
		if (queue_push(a, &ev, &born0)) {
			memcpy(a->ev_last_gpu, gpu_now, sizeof a->ev_last_gpu);
			a->ev_last_gpu_renderer = gpu_rnd;
			a->ev_last_gpu_n = gpu_n;
			a->ev_last_gpu_cur = gpu_cur;
		}
	}

	if (d_color) {
		const FnlEvent e = { .type = FNL_EVENT_COLOR, .time = t };
		ev = e;
		ev.color.color = color;
		if (queue_push(a, &ev, &born0)) {
			a->ev_last_color_space = (int)color.space;
			a->ev_last_sdr_scale   = color.sdr_scale;
		}
	}

	fnl_mutex_unlock(&a->ev_lock);
}

/* Whether anything is waiting to be handed over. The frame loop asks once a
 * turn and a yes is a reason to record: an application drains in its
 * callbacks, which run only from a re-record, and most events move nothing in
 * FnlDrawState. fnl_invalidate() is no answer from a drain that is not
 * running. The loss counter counts, because FNL_EVENT_LOST is delivered from
 * an empty ring and is still owed. */
bool events_pending(App *a)
{
	fnl_mutex_lock(&a->ev_lock);
	const bool any = !queue_empty(a) || a->ev_lost > 0 || a->ev_clip_owed;
	fnl_mutex_unlock(&a->ev_lock);
	return any;
}

/* ---- what the application calls ------------------------------------------ */

bool fnl_poll_event(App *a, FnlEvent *out)
{
	if (!a || !out)
		return false;

	fnl_mutex_lock(&a->ev_lock);

	if (queue_empty(a)) {
		/* The loss report, last: it is the newest that is dropped (see
		 * queue_push), so the gap is at the end of the run, and a caller that
		 * resets a gesture on FNL_EVENT_LOST must not then be handed input
		 * from before it. Cleared as it is reported, so a second overflow is
		 * a second report. */
		if (a->ev_lost > 0) {
			const FnlEvent lost = {
				.type = FNL_EVENT_LOST,
				.time = now_seconds() - a->started_at,
			};
			*out = lost;
			out->lost.count = a->ev_lost;
			a->ev_lost = 0;
			fnl_mutex_unlock(&a->ev_lock);
			if (a->echo_events || a->mcp)
				echo_event(a, out);
			return true;
		}
		/* A clipboard answer is a one-shot nothing re-asks for: its event
		 * was dropped in a gap and comes now, after the report of it. */
		if (a->ev_clip_owed) {
			a->ev_clip_owed = false;
			FnlEvent clip   = { .type = FNL_EVENT_CLIPBOARD,
								.time = now_seconds() - a->started_at };
			fnl_mutex_lock(&a->text_lock);
			clip.clipboard.length = (unsigned)a->clip_in_len;
			fnl_mutex_unlock(&a->text_lock);
			*out = clip;
			fnl_mutex_unlock(&a->ev_lock);
			if (a->echo_events || a->mcp)
				echo_event(a, out);
			return true;
		}
		fnl_mutex_unlock(&a->ev_lock);
		return false;
	}

	*out = a->ev_ring[a->ev_tail];
	/* The inputs it carries have reached the caller: each one's latency
	 * is from when it was made to now. */
	const EvBorn b = a->ev_born[a->ev_tail];
	if (b.n > 0) {
		const double t = now_seconds() - a->started_at;
		a->lat_sum += t * b.n - b.sum;
		if (t - b.oldest > a->lat_worst)
			a->lat_worst = t - b.oldest;
		a->lat_n += b.n;
		a->lat_stamped += b.stamped;
	}
	a->ev_tail = (a->ev_tail + 1) % EVENT_QUEUE_MAX;
	fnl_mutex_unlock(&a->ev_lock);
	/* After the unlock: the echo writes to a pipe, and a reader that has
	 * stopped reading must not be able to stall the producers. */
	if (a->echo_events || a->mcp)
		echo_event(a, out);
	return true;
}

/* ---- input latency ------------------------------------------------------- */

void events_latency_publish(App *a, double now, bool window)
{
	FNL_ASSERT_THREAD(APP_THREAD_RENDER);
	const bool unknown =
		fnl_atomic_load_u32(&a->input_lat_us) == COST_UNKNOWN;
	if (!window && !unknown &&
		!(a->resting && now - a->lat_published_at >= COST_WINDOW))
		return;

	fnl_mutex_lock(&a->ev_lock);
	const double sum = a->lat_sum, worst = a->lat_worst;
	const uint32_t n = a->lat_n, stamped = a->lat_stamped;
	a->lat_sum = a->lat_worst = 0.0;
	a->lat_n = a->lat_stamped = 0;
	fnl_mutex_unlock(&a->ev_lock);
	/* Nothing handed over since: the figure stands, as the costs' do while
	 * nothing is drawn. */
	if (n == 0)
		return;

	const double mean = sum / n;
	const double us = mean > 0.0 ? mean * 1e6 + 0.5 : 0.0;
	fnl_atomic_store_u32(&a->input_lat_us,
						 us < (double)(COST_UNKNOWN - 1)
							 ? (uint32_t)us : COST_UNKNOWN - 1);
	a->lat_published_at = now;
	/* How many were stamped says whether the window server's clock is
	 * the library's, which Wayland does not promise. */
	if (a->frame_times)
		fnl_log(FNL_LOG_INFO, "input %.2f ms over %u inputs, %u stamped, "
				"longest %.2f", mean * 1e3, n, stamped, worst * 1e3);
}

double fnl_input_latency_ms(const App *a)
{
	if (!a)
		return -1.0;
	const uint32_t us = fnl_atomic_load_u32(&a->input_lat_us);
	return us == COST_UNKNOWN ? -1.0 : us * 1e-3;
}
