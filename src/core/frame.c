/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The frame loop: what to draw, when, and whether to draw at all. Policy
 * only; the renderer is asked to acquire, record, submit and present.
 * DESIGN *Frame pacing and the off-screen skip* and *Late latching* hold the
 * reasoning and the measurements behind every figure here.
 */

#include "core/app.h"

/* ---- late latching -------------------------------------------------------
 *
 * The loop waits out most of the display period before sampling what it
 * draws from. One hold, sized two ways by FnlBackendVtbl::invites:
 *
 *   late_latch_feedback()  frames are invited: the delay is probed for,
 *                          walked up until a frame misses its cycle.
 *   late_latch_slack()     they are not (X11, Win32): how long the present
 *                          blocks is the spare time, measured. */

/* The fraction of a turn's idle the hold may take; the rest is headroom. */
#define LATE_SLACK_SPEND 0.75
/* The climb towards that, per frame. The fall is one step. */
#define LATE_SLACK_RISE  0.05
/* Above this a sample is not a display period: 20 Hz. Seconds. */
#define LATE_SLACK_MAX   0.05
/* Frames after a swapchain rebuild that say nothing about the period: FIFO
 * does not throttle a new swapchain until its queue has filled. An image
 * count, above any this builds. */
#define LATE_SLACK_SETTLE 8
/* Unheld turns measured before anything is held: what a held turn is read
 * against has to exist first. */
#define LATE_BASE_TURNS 16
/* Held turns averaging this many unheld ones long are being lengthened by
 * their hold. The margin is one stall's worth: a 40 ms turn moves the
 * average 7%. */
#define LATE_OVERRUN 1.1
/* Invitations averaging this many periods apart likewise. Tighter, since
 * the gap is the frame rate itself, where a turn is not: 3% is 3% of the
 * frames gone, and weston's gaps keep within 1.5% of the period. */
#define LATE_SLIDE 1.03
/* The weight of one held turn in that average. One turn says nothing: on
 * X11 a long one and a short one arrive as a pair, 19.6 ms then 13.8, with
 * nothing wrong. */
#define LATE_TURN_WEIGHT 0.05
/* Where nothing invites, a delay under this is not waited for: what is left
 * of one that overruns must be able to reach "none". Seconds. */
#define LATE_HOLD_MIN 0.001
/* How long the loop goes on pacing itself after a rebuild. A duration, not
 * LATE_SLACK_SETTLE's count: it outlasts the gap between two rebuilds of one
 * drag. Seconds. */
#define PACE_HOLDOVER 0.5
/* A spacing hold that came back later than this was not a late timer but a
 * stall, and the next turn is spaced from when it ended. Seconds. */
#define PACE_LATE_MAX 0.002
/* A measured period under this share of the monitor's refresh is no period:
 * nothing blocked, and what was summed is the frame's own cost. Not nearer
 * the refresh: the late-latch probe's deliberate miss puts a few short
 * samples beside it, which on amdgpu's display path took a true 16.5 ms
 * estimate to 13.7 (0.82); once under, the loop spaces every turn itself,
 * FIFO never fills, the turns sampled block for a few milliseconds, and
 * the estimate went on down to 2 ms and stayed, late latching off with
 * it. A swapchain that throttles nothing measures the frame's own cost,
 * far under (DESIGN *Late latching*). */
#define PACE_UNTHROTTLED 0.6
/* Presents before any estimator listens: a window still being mapped is not
 * describing the period it will run at. */
#define LATE_SETTLE_PRESENTS 60
/* Before that, samples in a row that must agree for the spacing to borrow
 * their mean, and how nearly: a window being mapped gives no four alike. */
#define PERIOD_EARLY_RUN 4
#define PERIOD_EARLY_TOL 0.1
/* Below this a sample is not a display period either: 500 Hz. It is a
 * swapchain that is not being throttled, saying so four times. Seconds. */
#define PERIOD_EARLY_MIN 0.002
/* A gap this many periods long missed its cycle. Every estimator drops it
 * rather than averaging it in, or misses inflate the period until none
 * registers. */
#define LATE_MISS_FACTOR 1.5
/* Samples in a row that an estimate under PACE_UNTHROTTLED of the refresh
 * drops as missed cycles, agreeing to PERIOD_EARLY_TOL, that replace it: a
 * missed cycle is one sample, and this many alike is a swap that blocks
 * after all. Mesa's OpenGL on schenker's Xorg lets the first half second
 * through unthrottled, 254 fps, and blocks for the refresh ever after; the
 * first settled sample was the burst's 1.06 ms. DESIGN *Late latching*. */
#define PERIOD_RELEARN_RUN 8
/* And for the samples to come at all while that estimate has the loop
 * spacing every turn, which samples nothing: a burst of turns let through
 * unspaced, its first blocking for part of a cycle only, after this many
 * spaced turns, the gap doubling after each burst that changed nothing, up
 * to the last. A swap that does throttle nothing pays a burst of frames a
 * minute once the gap has grown. */
#define PERIOD_PROBE_LEN   (PERIOD_RELEARN_RUN + 3)
#define PERIOD_PROBE_FIRST 120
#define PERIOD_PROBE_MAX   3840
/* A gap longer than this is a stall, not a sample. Seconds. */
#define LATE_STALL 0.5
/* Good frames a miss is remembered for: two seconds at 60 Hz. */
#define LATE_COOLDOWN 120
/* Steady frames before the probe starts walking the delay up. */
#define LATE_STABLE 60
/* Short gaps in a row before a shorter period is adopted. */
#define LATE_SHORT 10
/* The frame-rate window: the cost figures' (COST_WINDOW), so the three
 * move together. Seconds. */
#define FPS_WINDOW COST_WINDOW
/* The --frame-times line's, a clock of its own: pace.py's border row
 * counts the lines a drag of so many seconds yields. Seconds. */
#define FRAME_TIMES_WINDOW 0.5
/* Idle retry while the window has no usable size. Seconds. */
#define NO_SIZE_RETRY 0.010
/* How long after a state change a missing invitation is read as the
 * compositor being busy rather than the window being off screen. Seconds. */
#define TRANSITION_GRACE 0.3
/* How much longer than its fade the veil is waited for before the move is
 * made regardless: a window nobody can see draws no frame to fade. Seconds. */
#define VEIL_STALL 0.5
/* Presents before the loop may rest: the period has to be learnt first, or
 * the first drag after a rest is spaced by nothing. Two seconds at 60 Hz,
 * past LATE_SETTLE_PRESENTS. */
#define REST_AFTER_PRESENTS 120
/* Frames still drawn after the last change: a retired swapchain is freed
 * only by submitted frames counting it down. */
#define REST_SETTLE MAX_FRAMES_IN_FLIGHT
/* How long a resting loop waits for a signal before looking for itself, where
 * no period is known. Seconds. */
#define REST_POLL (1.0 / 60.0)

/* Hold for `seconds`, waking early for shutdown, an urgent change and --
 * where `for_configure` -- a configure. The late-latch hold exists to collect
 * input and the spacing to coalesce configures, so neither wakes for its own.
 *
 * `left` is what remains, not what the hold started as: a spurious wake, or a
 * signal that moves nothing the predicate tests, must not restart it. A hold
 * that ran twice is a missed cycle, which the probe reads as too much delay. */
static void hold_wait(App *a, double seconds, bool for_configure)
{
	double left = seconds;

	fnl_mutex_lock(&a->lock);
	while (!a->quit && !a->want_urgent &&
		   !(for_configure &&
			 (a->want_w != a->draw_w || a->want_h != a->draw_h ||
			  a->want_scale120 != a->draw_scale120))) {
		const double t0 = now_seconds();
		if (!fnl_cond_wait_for(&a->cond, &a->lock, left))
			break;
		left -= now_seconds() - t0;
		if (left <= 0.0)
			break;
	}
	fnl_mutex_unlock(&a->lock);
}

/* The delay in force: the pinned one where there is one. */
static double late_hold(const App *a)
{
	if (a->late_fixed > 0.0)
		return a->late_fixed;
	if (!BACKEND(a)->invites && a->late_delay < LATE_HOLD_MIN)
		return 0.0;
	return a->late_delay;
}

/* Fold one display timestamp into the delay. A gap of about two periods, or
 * gaps a little long on average, is the only sign of where the compositor's
 * deadline is: back off, remember the level as a ceiling, and creep back up
 * under it. */
static void late_latch_feedback(App *a, double displayed)
{
	double gap = displayed - a->late_prev_disp;
	a->late_prev_disp = displayed;
	if (gap <= 0.0 || gap > LATE_STALL)          /* first sample, or a stall */
		return;

	/* Average only the gaps that look like a period, and adopt a shorter one
	 * only when it keeps happening: tracking the minimum lets one short gap
	 * make every frame read as a miss. */
	if (a->late_period <= 0.0) {
		a->late_period = gap;
		a->late_turn = gap;
		a->late_ceiling = gap;                   /* until a slip says lower */
		return;
	}

	if (gap > a->late_period * LATE_MISS_FACTOR) {
		/* One miss is not evidence -- a compositor hiccup produces them at
		 * any delay -- but two inside the cooldown is the delay's doing.
		 * Proportional: a fixed step drives the ceiling negative. */
		if (a->late_recent++ && a->late_delay > 0.001) {
			a->late_ceiling = a->late_delay * 0.9;
			a->late_delay = a->late_ceiling * 0.9;
			a->late_recent = 0;
		}
		a->late_cooldown = LATE_COOLDOWN;
		return;
	}
	/* Exactly one decrement per good frame and one site that clears
	 * late_recent, or a single later hiccup reads as the second miss. */
	if (a->late_cooldown > 0 && --a->late_cooldown == 0)
		a->late_recent = 0;                      /* the window passed quietly */
	if (gap < a->late_period * 0.75) {
		if (++a->late_short >= LATE_SHORT) {     /* it is the new period */
			a->late_period = gap;
			a->late_turn = gap;
			a->late_short = 0;
		}
		return;
	}
	a->late_short = 0;

	/* Where the compositor holds a late commit for a FIFO barrier or a
	 * commit time, the invitation slides rather than doubles (weston and
	 * NVIDIA's WSI: 16.7 ms gaps became 17.3, 18, 20, and a missed cycle
	 * 25), so the test above never fires and the slide, averaged in,
	 * raised the cap with the hold. Read the gaps as the measured
	 * controller reads its turns instead: an average LATE_SLIDE longer
	 * than the period is the hold lengthening them, unless nothing is held
	 * that could, when it is a slower display. Only gaps inside that margin
	 * teach the period. */
	a->late_turn += (gap - a->late_turn) * LATE_TURN_WEIGHT;
	if (a->late_turn > a->late_period * LATE_SLIDE) {
		if (a->late_delay > LATE_HOLD_MIN) {
			a->late_ceiling = a->late_delay * 0.9;
			a->late_delay = a->late_ceiling * 0.9;
			a->late_turn = a->late_period;
			return;
		}
		a->late_period = a->late_turn;
	}
	if (gap <= a->late_period * LATE_SLIDE)
		a->late_period = a->late_period * 0.95 + gap * 0.05;

	/* Probing costs a missed cycle each time it finds the edge, so it waits
	 * for the presentation to look steady. */
	if (a->late_stable < LATE_STABLE) {
		a->late_stable++;
		return;
	}

	/* Just under the last level that missed, less the frame's own cost, so
	 * the delay gives way to a scene rather than rediscovering it through
	 * misses. */
	double cap = a->late_period - 0.002 - a->late_cost * 1.5;
	if (a->late_ceiling * 0.9 < cap)
		cap = a->late_ceiling * 0.9;
	if (cap < 0.0)
		cap = 0.0;

	a->late_ceiling += 0.000005;                 /* re-probe, 0.3 ms a second */
	if (a->late_delay < cap)
		a->late_delay += 0.00005;                /* 3 ms a second */
	if (a->late_delay > cap)
		a->late_delay = cap;
}

/* The miss detector without the control, on an estimate of its own (see
 * App::inst_period): the figure that says whether a delay is safe, read
 * against the --no-late-latch floor, which is not zero. */
static void inst_sample(App *a, double displayed)
{
	const double gap = displayed - a->inst_prev;
	a->inst_prev = displayed;
	if (gap <= 0.0 || gap > LATE_STALL)
		return;
	if (a->inst_period <= 0.0) {
		a->inst_period = gap;
		return;
	}
	a->inst_frames++;
	if (a->counting)
		a->st.cycles++;
	if (gap > a->inst_period * LATE_MISS_FACTOR) {
		a->inst_miss++;
		if (a->counting)
			a->st.missed++;
		return;
	}
	a->inst_period = a->inst_period * 0.98 + gap * 0.02;
}

/* The same hold, sized from a measurement: LATE_SLACK_SPEND of what the turn
 * held plus what its present then blocked for.
 *
 * That sum is spare time only if holding longer makes the block shorter, and
 * neither figure says so: a hold that came back late, or a present that
 * blocks on work rather than on the display, reads as more slack the more
 * is held. So held turns are checked, as an average (App::late_turn),
 * against what a turn costs with nothing held (App::late_base). Longer by
 * LATE_OVERRUN and the holds are lengthening the turns: the delay is cut
 * under a ceiling that creeps back, as the probe's is for a missed cycle. A
 * GPU that cannot keep up still decays it to nothing, since the fence wait
 * is not in App::present_block.
 *
 * Not for a backend that invites, where the block is nearly zero; and an
 * acquire timestamp is no input to late_latch_feedback(), because a FIFO gap
 * slides rather than doubles and a miss never registers. */
static void late_latch_slack(App *a, double held, double blocked)
{
	double slack = held + blocked;
	const double turn = slack + a->late_cost;

	if (held <= 0.0) {
		if (slack > LATE_SLACK_MAX)
			return;
		/* A missed cycle is folded in rather than dropped: it loosens the
		 * test for a few turns, and dropping it would never adopt a slower
		 * display. */
		a->late_base = a->late_base > 0.0
					 ? a->late_base * 0.9 + turn * 0.1 : turn;
		if (a->late_base_n < LATE_BASE_TURNS) {
			if (++a->late_base_n == LATE_BASE_TURNS) {
				a->late_ceiling = a->late_base;
				a->late_turn = a->late_base;
			}
			return;
		}
	} else {
		if (turn > LATE_STALL)
			return;
		a->late_turn += (turn - a->late_turn) * LATE_TURN_WEIGHT;
		if (a->late_turn > a->late_base * LATE_OVERRUN) {
			a->late_ceiling = a->late_delay * 0.9;
			a->late_delay = a->late_ceiling * 0.9;
			/* The next cut wants evidence of its own. */
			a->late_turn = a->late_base;
			return;
		}
		/* The same margin the other way is a faster display. The average,
		 * not a turn: the turns after a stall are short ones catching up. */
		if (a->late_turn * LATE_OVERRUN < a->late_base)
			a->late_base = a->late_turn;
	}

	if (slack > LATE_SLACK_MAX)
		return;
	/* A hold that came back late inside the margin is not slack either. */
	if (slack > a->late_base)
		slack = a->late_base;

	a->late_ceiling += 0.000005;                 /* re-probe, 0.3 ms a second */
	double target = slack * LATE_SLACK_SPEND;
	if (target > a->late_ceiling * 0.9)
		target = a->late_ceiling * 0.9;
	if (target < a->late_delay)
		a->late_delay = target;
	else
		a->late_delay += (target - a->late_delay) * LATE_SLACK_RISE;
}

/* The measured controller's figures are a device's: what its turn costs
 * with nothing held, and the hold sized against that. Forgotten for
 * another device and learnt again from no hold, or a software device,
 * which measures nothing that could cut it, keeps the last GPU's hold for
 * good -- and an idle gap each frame keeps the CPU's clock down under a
 * CPU rasteriser. DESIGN *Late latching*. */
static void late_slack_forget(App *a)
{
	a->late_delay = a->late_ceiling = 0.0;
	a->late_base = a->late_turn = 0.0;
	a->late_base_n = 0;
}

/* ---- the display period, where nothing announces it -----------------------
 *
 * What pace_space() spaces by, on a backend told nothing about the display.
 * Measured from four figures that partition a throttled turn: what it held,
 * what it waited on the device's fences, what its present blocked for, and
 * what the frame cost.
 *
 * The fence wait is in it although it is no spare time, because where a
 * FIFO swapchain is held to the display is the driver's choice and one
 * holds it there: NVIDIA's, presenting through another adapter's panel,
 * blocks in neither the acquire nor the present. Left out, the sum on that
 * device is the frame's cost alone, no period is ever learnt and nothing is
 * spaced -- 104 fps on a 60 Hz panel at fifteen sizes a second. A device
 * that really is behind reads as a longer period, and spacing by that is
 * spacing by the rate the loop runs at anyway.
 *
 * slack_learn() samples only where each means what it says, so the estimate
 * stands at its last good value through a drag, which is when it is read. */
/* Whether a sample is a missed cycle and no period at all: over
 * LATE_MISS_FACTOR of the refresh the output reports, where it reports one.
 * The first swap after rest can span two refreshes, and taken as the first
 * estimate it spaced a drag on X11 to 30 fps for the drag's length: a
 * rebuilt swapchain blocks on nothing, so no sample came to put it right. */
static bool period_missed(const App *a, double period)
{
	const uint32_t mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
	return mhz != 0 && period > 1000.0 / mhz * LATE_MISS_FACTOR;
}

static bool disp_period_sample(App *a, double period)
{
	if (period <= 0.0 || period > LATE_SLACK_MAX)
		return false;
	/* The first from a settled window replaces what was borrowed before
	 * it, whole: averaged in, a borrowed period that was short would drop
	 * every true one as a missed cycle and stand for good. Never a missed
	 * cycle, which would stand as long. */
	if (a->disp_period <= 0.0 || a->disp_early) {
		if (period_missed(a, period))
			return false;
		a->disp_period = period;
		a->disp_early = false;
		return true;
	}

	/* A missed cycle arrives as one sample two periods long, which
	 * LATE_SLACK_MAX is nowhere near catching. Under an estimate that says
	 * nothing blocks, a run of them that agree is the estimate that was
	 * wrong (PERIOD_RELEARN_RUN): learnt again from the run. Not under any
	 * other: a device falling behind for a while is not a new display. */
	if (period > a->disp_period * LATE_MISS_FACTOR) {
		const uint32_t mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
		if (mhz == 0 ||
			a->disp_period >= 1000.0 / mhz * PACE_UNTHROTTLED) {
			a->disp_long_n = 0;
			return false;
		}
		const double off = period > a->disp_long ? period - a->disp_long
												 : a->disp_long - period;
		if (a->disp_long_n > 0 && off <= a->disp_long * PERIOD_EARLY_TOL) {
			a->disp_long_n++;
			a->disp_long += (period - a->disp_long) / a->disp_long_n;
		} else {
			a->disp_long = period;
			a->disp_long_n = 1;
		}
		if (a->disp_long_n < PERIOD_RELEARN_RUN)
			return false;
		fnl_log(FNL_LOG_INFO, "display period learnt again: %.2f ms, where "
				"%.2f stood -- a swap that did not block at first",
				a->disp_long * 1e3, a->disp_period * 1e3);
		a->disp_period = a->disp_long;
		a->disp_long_n = 0;
		a->probe_left = a->probe_gap = 0;
		return true;
	}

	a->disp_long_n = 0;
	a->disp_period = a->disp_period * 0.95 + period * 0.05;
	return true;
}

/* Whether this turn goes unspaced to let the period estimate hear the swap
 * (PERIOD_PROBE_LEN): asked only while the spacing holds every turn on an
 * estimate under PACE_UNTHROTTLED of the refresh, on a device whose
 * present is timed. A software device's never is, and its spacing is for
 * good. */
static bool period_probe(App *a)
{
	if (a->present_untimed || a->disp_period <= 0.0)
		return false;
	if (a->probe_left > 0) {
		if (--a->probe_left == 0) {
			a->probe_gap = a->probe_gap * 2 < PERIOD_PROBE_MAX
						 ? a->probe_gap * 2 : PERIOD_PROBE_MAX;
			a->probe_wait = a->probe_gap;
		}
		return true;
	}
	if (a->probe_gap == 0) {
		a->probe_gap = PERIOD_PROBE_FIRST;
		a->probe_wait = a->probe_gap;
	}
	if (--a->probe_wait > 0)
		return false;
	a->probe_left = PERIOD_PROBE_LEN - 1;
	return true;
}

/* Whether a pin applies to an area of this many pixels. Larger on either axis
 * is ignored rather than fitted; equal is native. */
static bool scene_pin_applies(int pin_w, int pin_h, int area_w, int area_h)
{
	return pin_w > 0 && pin_h > 0 && pin_w <= area_w && pin_h <= area_h &&
		   (pin_w != area_w || pin_h != area_h);
}

/* A present mode cannot be changed in place, so a different one is a rebuild
 * too. */
static bool swapchain_stale(const App *a, int w, int h, int s, int m,
							bool vsync)
{
	return w != a->draw_w || h != a->draw_h ||
		   s != a->draw_scale120 || m != a->draw_margin ||
		   !RENDERER(a)->present_mode_matches(a, vsync) ||
		   a->resized;
}

/* ---- the veil ---------------------------------------------------------------
 *
 * A move to another GPU, renderer or kernel driver leaves a gap on screen
 * that is the window server's or the kernel's to fill, and each fills it
 * with black or with nothing: the dumb buffer the KMS backend takes an
 * output back with, a mode set, a child window nobody has painted. So the
 * window is drawn down to black over the last frames before such a move
 * and lifted over the first frames after it, and the gap, whatever its
 * length, is the dark between two halves of one fade rather than a window
 * that went away. One full-window stroke at the end of the overlay
 * (draw_veil() in draw.c); its alpha is in the draw state, so each step
 * re-records as a moving scene does and a veil that stands costs nothing.
 * DESIGN *The veil*. */

static double veil_seconds(const App *a, bool out)
{
	return fnl_atomic_load_u32(out ? &a->veil_out_ms : &a->veil_in_ms) / 1e3;
}

/* The frame read back, let go. It is the cover and the gap's picture for
 * the one move it was read for and no other: kept, the next gap with no
 * frame of its own -- a move whose readback failed or was not asked for,
 * a KMS take-back at a mode change or a loss -- would show it, an old
 * frame where black is owed (scan_fill(), present_erase()). Under
 * App::lock for the Win32 window thread, which paints from it, so never
 * called with the lock held. */
static void snapshot_drop(App *a)
{
	fnl_mutex_lock(&a->lock);
	unsigned char *old = a->snap_px;
	a->snap_px = NULL;
	a->snap_valid = false;
	fnl_mutex_unlock(&a->lock);
	free(old);
}

/* The window is not on screen -- the backend or the renderer says so, or
 * on Wayland, where both say shown, nothing invites a frame: a lift nobody
 * can see is over, and so is the wait for the first present that starts
 * one, which a hidden window does not make. Its frames come back without
 * the cover, and FNL_EVENT_GPU, which follows the lift, with the first
 * turn shown. A move asked for meanwhile is made at once
 * (app_veil_gate()). */
static void veil_unseen(App *a)
{
	if (a->veil_phase != VEIL_ARMED && a->veil_phase != VEIL_IN)
		return;
	a->veil_phase = VEIL_NONE;
	snapshot_drop(a);
}

/* Whether the backend or the renderer knows the window to be hidden, which
 * draws no frame to read back, to fade or to wait for: the gate then makes
 * the move at once, whichever phase it is in. Wayland's says shown, and is
 * waited out by the stalls instead. */
static bool veil_hidden(App *a)
{
	return !BACKEND(a)->window_on_screen(a) || RENDERER(a)->occluded(a);
}

/* The veil this frame is drawn with, in 1/255 steps, and the phase moved
 * on where the frame before earned it: down to VEIL_HELD once a frame at
 * full veil is latched, which app_veil_gate() lets through once that frame
 * has been presented; VEIL_ARMED to VEIL_IN once the rebuilt device has
 * presented its first, black, frame. `now` is the turn's latch time. */
static float veil_step(App *a, double now)
{
	float v = 0.0f;
	switch (a->veil_phase) {
	case VEIL_NONE:
		return 0.0f;
	case VEIL_SNAP:
		/* Where the lift had reached, already in steps: the frame read
		 * back is the one on screen, cover and all. */
		return a->veil_snap;
	case VEIL_OUT: {
		const double s = veil_seconds(a, true);
		v = s > 0.0 ? (float)((now - a->veil_t0) / s) : 1.0f;
		if (v >= 1.0f) {
			v = 1.0f;
			a->veil_phase = VEIL_HELD;
			a->veil_presents = a->presents;
		}
		break;
	}
	case VEIL_HELD:
		/* Black where black is what came down. With a frame read back
		 * nothing did, and no frame is drawn here anyway: the move is made
		 * the turn the gate lets it through. */
		v = a->veil_black ? 1.0f : 0.0f;
		break;
	case VEIL_ARMED:
		v = 1.0f;
		if (a->presents > a->veil_presents) {
			a->veil_phase = VEIL_IN;
			a->veil_t0 = now;
		}
		break;
	case VEIL_IN: {
		const double s = veil_seconds(a, false);
		v = s > 0.0 ? 1.0f - (float)((now - a->veil_t0) / s) : 0.0f;
		if (v <= 0.0f) {
			v = 0.0f;
			a->veil_phase = VEIL_NONE;
			snapshot_drop(a);
		}
		break;
	}
	}
	return (float)(int)(v * 255.0f + 0.5f) / 255.0f;
}

/* The veil back up from wherever it is, for a move that will not be made
 * after all: from the level the fade-out had reached, so the frames turn
 * round rather than jump. Not from VEIL_ARMED: that move was made, and
 * its lift waits for the rebuilt device's first present (veil_step()). */
static void veil_lift(App *a)
{
	const double now = now_seconds();
	const double out = veil_seconds(a, true), in = veil_seconds(a, false);
	double v = 1.0;
	switch (a->veil_phase) {
	case VEIL_NONE:
	case VEIL_IN:
	case VEIL_ARMED:
		return;
	case VEIL_SNAP:
		a->snap_owed = false;
		if (a->snap_fresh && a->veil_snap > 0.0f) {
			/* The frame on screen, cover and all, read back: lifted from
			 * in full, which is what is on screen. With no lift under it
			 * the frame read back is no veil at all, and drawing it over
			 * a scene that moves on would be a ghost of it. */
			a->snap_fresh = false;
			a->veil_black = false;
			break;
		}
		a->snap_fresh = false;
		if (a->veil_snap > 0.0f) {
			/* A lift stopped for the readback: on from where it stood,
			 * over the same cover or black. */
			v = (double)a->veil_snap;
			break;
		}
		/* Nothing drawn yet: nothing to lift. */
		a->veil_phase = VEIL_NONE;
		snapshot_drop(a);
		return;
	case VEIL_OUT:
		v = out > 0.0 ? (now - a->veil_t0) / out : 1.0;
		if (v > 1.0)
			v = 1.0;
		break;
	case VEIL_HELD:
		break;
	}
	a->veil_phase = VEIL_IN;
	a->veil_t0 = now - (1.0 - v) * in;
}

bool app_veil_gate(App *a)
{
	const double now = now_seconds();
	const double out = veil_seconds(a, true);
	switch (a->veil_phase) {
	case VEIL_NONE:
	case VEIL_IN: {
		/* The next frame drawn reads itself back (App::snap_owed), and
		 * the device built after the move dissolves from it -- with no
		 * transition too, where the frame is still what the gap shows
		 * (scan_fill(), present_hold()), and is not drawn over the new
		 * device's. A move asked for as the last one lifts holds the
		 * lift where it stands for that frame (App::veil_snap), cover or
		 * black and all, so that what is read back is what is on screen
		 * and nothing jumps; the cover it is drawn from is kept until the
		 * frame replaces it. The frame is owed even at rest, where
		 * nothing else would draw one -- a landing of a move to another
		 * kernel driver comes from the mover's thread with nothing
		 * changed -- so the recording is marked stale, which is a change
		 * to the rest rule (rest_owed()). */
		/* Hidden, it would draw no frame to read back, nor one to fade:
		 * the move at once, its lift let go with the first turn skipped
		 * (veil_unseen()). */
		if (veil_hidden(a)) {
			snapshot_drop(a);
			a->veil_black = false;
			a->veil_phase = VEIL_HELD;
			a->veil_presents = a->presents;
			return true;
		}
		const bool lifting = a->veil_phase == VEIL_IN;
		a->veil_snap = lifting ? a->last_draw.veil : 0.0f;
		if (!lifting) {
			snapshot_drop(a);
			a->veil_black = false;
		}
		a->record_gen++;
		a->snap_owed = true;
		a->snap_fresh = false;
		a->veil_phase = VEIL_SNAP;
		a->veil_t0 = now;
		return false;
	}
	case VEIL_SNAP:
		if (a->snap_owed && now - a->veil_t0 <= VEIL_STALL &&
			!veil_hidden(a))
			return false;
		a->snap_owed = false;
		if (a->snap_fresh) {
			/* Held across the move: nothing comes down first. */
			a->snap_fresh = false;
			a->veil_black = false;
			a->veil_phase = VEIL_HELD;
			a->veil_presents = a->presents;
			return true;
		}
		/* No frame to hold -- none could be read back here, or none was
		 * drawn -- so black it is: down over the frames before the move
		 * and up after it, or at once where no fade down is asked for;
		 * from where a lift stopped for the readback had reached. A cover
		 * kept for that lift is let go: the gap is black's now. */
		snapshot_drop(a);
		a->veil_black = true;
		if (out <= 0.0) {
			a->veil_phase = VEIL_HELD;
			a->veil_presents = a->presents;
			return true;
		}
		a->veil_phase = VEIL_OUT;
		a->veil_t0 = now - (double)a->veil_snap * out;
		return false;
	case VEIL_OUT:
		if (now - a->veil_t0 <= out + VEIL_STALL && !veil_hidden(a))
			return false;
		/* No frame has come down: the window is off screen or uninvited,
		 * and the move is not held for it. */
		a->veil_phase = VEIL_HELD;
		return true;
	case VEIL_HELD:
		return a->presents > a->veil_presents ||
			   now - a->veil_t0 > out + VEIL_STALL || veil_hidden(a);
	case VEIL_ARMED:
		/* Covered still, by the frame read back or by black: the move may
		 * be made at once, and the lift armed again after it
		 * (device_recover()), from the same cover. */
		return true;
	}
	return true;
}

void app_snapshot_set(App *a, const void *px, int w, int h, size_t pitch,
					  bool bgra)
{
	a->snap_owed = false;
	if (!px || w <= 0 || h <= 0)
		return;
	const size_t row = (size_t)w * 4u;
	if ((size_t)h > SIZE_MAX / row)
		return;
	unsigned char *fresh = malloc(row * (size_t)h);
	if (!fresh)
		return;
	/* Kept as BGRA whatever the renderer's order: swizzled here once, on
	 * this thread, rather than by every reader -- the Win32 erase under
	 * App::lock at each paint, the KMS take-back -- and every cover
	 * uploads that order. */
	for (int y = 0; y < h; y++) {
		unsigned char *dst = fresh + (size_t)y * row;
		const unsigned char *src = (const unsigned char *)px +
								   (size_t)y * pitch;
		if (bgra) {
			memcpy(dst, src, row);
			continue;
		}
		for (size_t x = 0; x < row; x += 4) {
			dst[x + 0] = src[x + 2];
			dst[x + 1] = src[x + 1];
			dst[x + 2] = src[x + 0];
			dst[x + 3] = src[x + 3];
		}
	}
	/* Swapped in under the lock: the Win32 window thread paints from the
	 * block it finds there. */
	fnl_mutex_lock(&a->lock);
	unsigned char *old = a->snap_px;
	a->snap_px = fresh;
	a->snap_w = w;
	a->snap_h = h;
	a->snap_valid = true;
	a->snap_gen++;
	fnl_mutex_unlock(&a->lock);
	free(old);
	a->snap_fresh = true;
}

void app_snapshot_decline(App *a)
{
	a->snap_owed = false;
}

/* The caller's objects were built against the device that just went. Told
 * here rather than in each renderer's recover(), so none can forget; the drain
 * is for surface loss, where the queue may still be busy. Once per loss, not
 * per attempt, and created again only if the device came back
 * (App::cb_gpu_created). */
static void device_recover(App *a)
{
	if (a->cb_gpu_created) {
		RENDERER(a)->wait_idle(a);
		a->cb_gpu_created = false;
		if (a->cb.on_gpu_lost)
			a->cb.on_gpu_lost(a->cb.ud, a);
	}

	/* A renderer owed after a swap that failed both ways, a move to
	 * another renderer's GPU, or a rebuild of the renderer drawing. */
	if (a->renderer_owed) {
		if (!renderer_owed_retry(a))
			return;
	} else if (a->renderer_next) {
		renderer_swap(a);
		if (a->renderer_owed)
			return;
	} else {
		RENDERER(a)->recover(a);
	}

	if (!RENDERER(a)->lost(a)) {
		a->gpu_switching = a->gpu_switch_back = false;
		app_frame_cost_reset(a);
		if (!BACKEND(a)->invites)
			late_slack_forget(a);
		/* The veil that came down for this move lifts over the first
		 * frames drawn on the device, where a lift is asked for: armed
		 * here, started by the device's first present (veil_step()). A
		 * loss outside a transition had none and gets none. One during a
		 * lift, where the gap showed the cover whole (App::snap_shown),
		 * lifts it again from there, or the first frame would jump back
		 * from the whole cover to where the lift had reached; where the
		 * window server kept the last frame, the lift goes on. */
		fnl_mutex_lock(&a->lock);
		const bool shown = a->snap_shown;
		a->snap_shown = false;
		fnl_mutex_unlock(&a->lock);
		/* A loss while a frame is read back from a lift held, where the
		 * gap showed the cover whole: the frame is read back at the whole
		 * cover too, which is what was last on screen. */
		if (a->veil_phase == VEIL_SNAP && shown && a->veil_snap > 0.0f &&
			!a->veil_black)
			a->veil_snap = 1.0f;
		if (a->veil_phase == VEIL_HELD || a->veil_phase == VEIL_ARMED ||
			(a->veil_phase == VEIL_IN && shown)) {
			if (veil_seconds(a, false) > 0.0) {
				a->veil_phase = VEIL_ARMED;
				a->veil_presents = a->presents;
			} else {
				a->veil_phase = VEIL_NONE;
				snapshot_drop(a);
			}
		}
		/* The antialiasing asked for, as this device can take it: a GPU
		 * without FSAA drops it, and the next with it puts it back. */
		app_aa_settle(a);
		a->cb_gpu_created = true;
		if (a->cb.on_gpu_created)
			a->cb.on_gpu_created(a->cb.ud, a);
		return;
	}
	/* A GPU moved to that will not build: back to the one before, by the
	 * choice that made it, at the next turn. Once -- one that will not
	 * build either is a loss like any other from here. The way back is a
	 * move as the way there was, the surface made again with the rest
	 * (gpu_switch_owed()): one that failed after its surface was made has
	 * left only the device's loss raised, and a recovery from that alone
	 * keeps the GPU that failed, on OpenGL whose display is one device's. */
	if (a->gpu_switching && !a->gpu_switch_back) {
		fnl_log(FNL_LOG_WARN, "the GPU asked for would not draw here (%s); "
				"back to the one before", fnl_last_error());
		a->gpu_index = a->gpu_back_index;
		memcpy(a->gpu_want, a->gpu_back_want, sizeof a->gpu_want);
		a->gpu_switch_back = true;
		a->surface_lost = true;
	} else if (a->gpu_switching) {
		a->gpu_switching = a->gpu_switch_back = false;
	}
}

/* Whether list entry `x` is the one `w` was: the same renderer and name,
 * or for a move to another kernel driver the move to the same driver --
 * the entry is named by the GPU's address until Vulkan has listed it. */
static bool gpu_entry_same(const GpuEntry *x, const GpuEntry *w)
{
	if (x->renderer != w->renderer || !x->driver_move != !w->driver_move)
		return false;
	if (!w->driver_move)
		return strcmp(x->name, w->name) == 0;
	const char *p = strstr(x->name, ", on ");
	const char *q = strstr(w->name, ", on ");
	return p && q && strcmp(p, q) == 0;
}

/* fnl_request_gpu()'s entry in the list as it is now, -1 for none: where
 * it was asked for, or where the parts listed since have put it -- the
 * move entries come last, after every renderer's GPUs. */
static int gpu_req_find(const App *a) FNL_REQUIRES(a->lock)
{
	const int at = a->gpu_req - 1;
	if (at < 0)
		return -1;
	if (at < a->gpu_list_n && gpu_entry_same(&a->gpu_list[at],
											 &a->gpu_req_entry))
		return at;
	for (int i = 0; i < a->gpu_list_n; i++)
		if (gpu_entry_same(&a->gpu_list[i], &a->gpu_req_entry))
			return i;
	return -1;
}

/* A GPU asked for through fnl_request_gpu(): the rebuild a loss would make,
 * raised on purpose, with the GPU's place in the list as the choice it is
 * made by -- gpu_index, which every renderer's pick counts the same way,
 * from 1 where the list counts from 0. Not while a loss is being answered,
 * which keeps the wish for the turn after; and not on the GPU in use. The
 * choice before is kept for the way back (device_recover()).
 *
 * A surface loss and not a device loss: the surface is made again too.
 * A driver leaves state of its own on the window through the surface --
 * Mesa's Wayland WSI binds the wl_surface to explicit sync for RADV, and
 * lavapipe's wl_shm buffers on that surface are a protocol error
 * (wp_linux_drm_syncobj_surface_v1 unsupported_buffer), the window gone. */
static void gpu_switch_owed(App *a)
{
	/* Nor while a move is drawn across, until the window is on the GPU
	 * moved: the wish is kept for the turn after (app_gpu_driver_poll()). */
	if (RENDERER(a)->lost(a) || a->mover_started || a->gpu_cross_landing)
		return;
	/* A wish for nowhere is spent at once; one for another GPU stands
	 * while the veil comes down over the frames this renderer still draws
	 * (app_veil_gate()), and is read again then, so the latest wish is
	 * the one made. The gate is asked outside the lock, which it takes to
	 * let go of a frame read back (snapshot_drop()). */
	fnl_mutex_lock(&a->lock);
	int want = gpu_req_find(a);
	const bool gated = want >= 0 && want != a->gpu_list_cur;
	fnl_mutex_unlock(&a->lock);
	if (gated && !app_veil_gate(a))
		return;
	fnl_mutex_lock(&a->lock);
	want = gpu_req_find(a);
	const bool other = want >= 0 && want != a->gpu_list_cur;
	/* A wish that came between the two reads has not been through the
	 * gate: the next turn takes it there. */
	if (other && !gated) {
		fnl_mutex_unlock(&a->lock);
		return;
	}
	/* Only a wish spent lifts a veil: with none, this runs every turn,
	 * and the lift of a move just made is the first present's. */
	const bool wished = a->gpu_req != 0;
	a->gpu_req = 0;
	char name[GPU_NAME_MAX] = "";
	const int move = other ? a->gpu_list[want].driver_move - 1 : -1;
	const FnlRenderer lands = other ? a->gpu_list[want].renderer
									: FNL_RENDERER_DEFAULT;
	if (other)
		memcpy(name, a->gpu_list[want].name, sizeof name);
	fnl_mutex_unlock(&a->lock);
	if (move >= 0) {
		app_gpu_driver_owe(a, move, name, lands);
		return;
	}
	const FnlRendererVtbl *r;
	int local;
	if (!other || !app_gpu_entry_of(a, want, &r, &local)) {
		if (wished)
			veil_lift(a);       /* a veil that came down for nothing */
		return;
	}

	fnl_log(FNL_LOG_INFO, "moving to GPU %d: %s", want + 1, name);
	a->gpu_back_index = a->gpu_index;
	memcpy(a->gpu_back_want, a->gpu_want, sizeof a->gpu_back_want);
	/* gpu_index counts the list of the renderer that will pick, which is
	 * another renderer's where the GPU is one of its (renderer_swap()). */
	a->gpu_index = local + 1;
	a->gpu_want[0] = '\0';
	a->renderer_next = r != a->renderer ? r : NULL;
	a->gpu_switching = true;
	a->gpu_switch_back = false;
	a->surface_lost = true;
}

/* A period to space by before the window has settled, so that a border
 * taken in its first second is not drawn unspaced for as long as it is
 * held: a run of rebuilds samples nothing, and where the driver blocks
 * nowhere that was 93 fps on a 60 Hz panel until the hand let go.
 *
 * Borrowed and not learnt. It is the mean of PERIOD_EARLY_RUN samples in a
 * row that agree, which a window still being mapped does not give, and it
 * is dropped for the first sample a settled window gives. It feeds the
 * spacing alone: the late-latch controllers wait for the settled window,
 * since a hold from a wrong period is a late frame and spacing by one is
 * safe either way. */
static void disp_period_early(App *a, double period)
{
	if (a->disp_period > 0.0)
		return;
	if (period < PERIOD_EARLY_MIN || period > LATE_SLACK_MAX ||
		period_missed(a, period)) {
		a->disp_try_n = 0;
		return;
	}
	const double off = period > a->disp_try ? period - a->disp_try
											: a->disp_try - period;
	if (a->disp_try_n > 0 && off > a->disp_try * PERIOD_EARLY_TOL)
		a->disp_try_n = 0;
	a->disp_try = a->disp_try_n == 0
				? period
				: a->disp_try + (period - a->disp_try) / (a->disp_try_n + 1);
	if (++a->disp_try_n >= PERIOD_EARLY_RUN) {
		a->disp_period = a->disp_try;
		a->disp_early = true;
	}
}

/* The period the spacing holds a turn to, and whether nothing is throttling
 * the loop at all.
 *
 * The measured one, where it is at least the refresh the window server
 * reports. Shorter is not a faster monitor but a present that blocked on
 * nothing -- lavapipe on a bare X server, whose software path holds no
 * swapchain to the display, measured the frame's own 7 ms and was spaced to
 * 140 fps on a 60 Hz panel. So a period under PACE_UNTHROTTLED of the
 * refresh is replaced by the refresh, and `*every` says the turn is spaced
 * from the last one even with no rebuild about: FIFO is not holding the
 * loop between rebuilds either.
 *
 * None measured yet is two things. On a device whose present is never
 * measured (App::present_untimed: lavapipe, llvmpipe, WARP) it is for
 * good, and spacing every turn is what holds it to the refresh. Anywhere
 * else it is a window still learning, and the swapchain is left to
 * throttle: spaced from the start, FIFO never fills, and the early
 * learner measures a few milliseconds of it -- 7.5 ms on amdgpu's display
 * path at 60 Hz, which kept the spacing on for good. A swapchain that does
 * not throttle shows it in its first samples, and is spaced from then.
 * A backend that reports no refresh has the measured period alone. */
static double pace_period(const App *a, bool *every)
{
	const uint32_t mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
	*every = false;
	if (mhz == 0)
		return a->disp_period;
	const double refresh = 1000.0 / mhz;
	const bool unthrottled = a->disp_period > 0.0
		? a->disp_period < refresh * PACE_UNTHROTTLED
		: a->present_untimed;
	if (unthrottled) {
		*every = true;
		return refresh;
	}
	return a->disp_period > refresh ? a->disp_period : refresh;
}

/* What stands in for the invitation where frame_wait() answers at once: space
 * the turn a display period from the later of the last rebuild and, within
 * PACE_HOLDOVER of it, when the last paced turn was due. True when it held.
 *
 * From the last rebuild, not this turn's `rebuild`: that flag is the first
 * look's, and in a drag a turn usually becomes a rebuild at the second. From
 * the last paced turn while rebuilds keep arriving, because a rebuilt
 * swapchain does not throttle; once they stop, FIFO has the loop back and it
 * must not be paced twice. */
static bool pace_space(App *a, double period, bool every)
{
	bool held = false;
	const double t_now = now_seconds();
	double anchor = a->rebuild_at;
	if ((every || t_now - a->rebuild_at < PACE_HOLDOVER) &&
		a->pace_at > anchor)
		anchor = a->pace_at;
	const double since = t_now - anchor;
	if (since < period) {
		hold_wait(a, period - since, false);
		held = true;
	}
	/* After the wait, and on every turn that reaches here, or the anchor
	 * goes stale the moment one is let straight through. */
	a->pace_at = now_seconds();
	a->pace_ran = true;
	/* A held turn is anchored where its hold was due, not where it came
	 * back: no timed wait returns early, so anchoring on the return spaces
	 * every turn by the period plus the wait's lateness, and half a
	 * millisecond of that is two frames a second. Not for a hold cut short,
	 * which is early and means it. */
	if (held) {
		const double late = a->pace_at - (anchor + period);
		if (late > 0.0 && late < PACE_LATE_MAX)
			a->pace_at -= late;
	}
	return held;
}

/* ---- the queue ahead of the screen -----------------------------------------
 *
 * Where the present is the only throttle, the graphics API keeps what it
 * likes waiting ahead of the screen: Mesa's X11 Vulkan WSI three frames,
 * 54 ms from a frame's latch to the screen on schenker's Xorg and this
 * desk's XWayland, where OpenGL's swap keeps one, 21 ms. So the frame is
 * held ahead of its latch until the window server says the one before it
 * is on the screen (app_frame_answered()), and is latched, drawn and
 * presented in the refresh that follows: 16.4 to 16.5 ms there for Vulkan,
 * OpenGL and NVIDIA's own driver alike, the rate unchanged. Not by the
 * graphics API's own wait for a present: asked every frame, Mesa's X11
 * WSI stalled the loop under ThreadSanitizer, and NVIDIA's driver has
 * none. That leaves the frame no overlap with the one before; one whose
 * work outruns the refresh misses every other, where a frame more ahead
 * would have kept the rate. So two waits in a row that come back over
 * LATE_MISS_FACTOR of the period after the last are a queue too short,
 * and one more frame is let wait for DISPLAY_RETRY_FIRST, the gap doubling
 * after each try that missed again, to DISPLAY_RETRY_MAX. Only between
 * frames drawn one after the other: a turn after rest waits for nothing
 * that could miss. And not while rebuilds keep arriving (PACE_HOLDOVER):
 * a drag is held to the size handshake, whose frame between two sizes
 * waited for the one before on this desk's XWayland, 44 fps in pace.py's
 * border row where it was 59. */
#define DISPLAY_AHEAD        1
#define DISPLAY_AHEAD_SLOW   2
#define DISPLAY_MISS_RUN     2
#define DISPLAY_RETRY_FIRST  2.0
#define DISPLAY_RETRY_MAX    64.0
/* Bounded, at a period for each frame still to be answered and half a
 * period more, and no longer than the second: a frame the window server
 * never answers costs the turn this, not the loop, and then nothing is
 * waited for until an answer comes -- a server with no Present, or a
 * driver that presents some other way, pays it once. By the frames owed
 * and not at one period and a half: on this desk's XWayland the hold
 * began with three frames already waiting, each wait ran out before the
 * last of them was shown, and the queue was never drained, 47 ms from
 * latch to screen where it would be 16.5. */
#define DISPLAY_WAIT_SLACK   0.5
#define DISPLAY_WAIT_MAX_S   0.1

/* Until the window server has answered for all but the last `ahead` - 1
 * frames presented (app_frame_answered()), or the bound has gone by. False
 * where nothing was waited for: too few frames yet, or the server not
 * answering. On App::cond, which the answer signals from the main thread;
 * never on the graphics API's own wait, whose X11 WSI the loop stalled on
 * when asked every frame. */
static bool display_wait(App *a, int ahead, double period)
{
	if (a->presents < (uint64_t)ahead)
		return false;
	const uint32_t seen = fnl_atomic_load_u32(&a->frame_answered);
	if (a->display_unanswered) {
		if (seen == a->display_unanswered_at)
			return false;
		a->display_unanswered = false;
	}
	/* Answered is the frame's index and one, so the last presented frame,
	 * presents - 1, is answered at presents. */
	const uint32_t want = (uint32_t)(a->presents - (uint64_t)ahead + 1);
	const int32_t owed = (int32_t)(want - seen);
	double left = period > 0.0 ? period * ((owed > 0 ? owed : 0) +
										   DISPLAY_WAIT_SLACK)
							   : DISPLAY_WAIT_MAX_S;
	if (left > DISPLAY_WAIT_MAX_S)
		left = DISPLAY_WAIT_MAX_S;
	fnl_mutex_lock(&a->lock);
	while (!a->quit &&
		   (int32_t)(fnl_atomic_load_u32(&a->frame_answered) - want) < 0) {
		const double t0 = now_seconds();
		if (!fnl_cond_wait_for(&a->cond, &a->lock, left))
			break;
		left -= now_seconds() - t0;
		if (left <= 0.0)
			break;
	}
	const uint32_t now_seen = fnl_atomic_load_u32(&a->frame_answered);
	fnl_mutex_unlock(&a->lock);
	if ((int32_t)(now_seen - want) < 0) {
		a->display_unanswered = true;
		a->display_unanswered_at = now_seen;
	}
	return true;
}

static void display_hold(App *a)
{
	if (a->display_ahead == 0) {
		a->display_ahead = DISPLAY_AHEAD;
		a->display_retry_gap = DISPLAY_RETRY_FIRST;
	}
	const double t0 = now_seconds();
	if (a->display_ahead > DISPLAY_AHEAD && t0 >= a->display_retry_at) {
		a->display_ahead = DISPLAY_AHEAD;
		a->display_misses = 0;
	}
	const uint32_t mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
	const double period = a->disp_period > 0.0 ? a->disp_period
						: mhz ? 1000.0 / mhz : 0.0;
	const bool waited = display_wait(a, a->display_ahead, period);
	const double t1 = now_seconds();
	a->display_block = t1 - t0;
	if (!waited)
		return;

	const bool next = a->presents == a->display_last_presents + 1;
	if (period > 0.0 && next && a->display_ahead == DISPLAY_AHEAD &&
		t1 - a->display_last > period * LATE_MISS_FACTOR) {
		if (++a->display_misses >= DISPLAY_MISS_RUN) {
			a->display_ahead = DISPLAY_AHEAD_SLOW;
			a->display_misses = 0;
			a->display_retry_at = t1 + a->display_retry_gap;
			a->display_retry_gap = a->display_retry_gap * 2.0 >
								   DISPLAY_RETRY_MAX
								 ? DISPLAY_RETRY_MAX
								 : a->display_retry_gap * 2.0;
		}
	} else if (next && a->display_ahead == DISPLAY_AHEAD) {
		a->display_misses = 0;
		/* Held at the shallow queue past the gap it was let go after: the
		 * misses were the frames', not the queue's, and the next try is
		 * the first again. */
		if (t1 > a->display_retry_at + a->display_retry_gap)
			a->display_retry_gap = DISPLAY_RETRY_FIRST;
	}
	a->display_last = t1;
	a->display_last_presents = a->presents;
}

/* The turn's throttle: the invitation, then the loop's own spacing, then the
 * late-latch hold. Answers whether a frame was invited. `*spaced` is whether
 * the spacing held the turn: its timings then measure the loop's own clock
 * and must not feed the period estimate. */
static bool pace_turn(App *a, bool rebuild, bool urgent, bool vsync,
					  bool *spaced)
{
	*spaced = false;

	const bool invited = BACKEND(a)->frame_wait(a, !rebuild);
	a->display_block = 0.0;
	if (invited && vsync && !rebuild && BACKEND(a)->display_waits &&
		now_seconds() - a->rebuild_at >= PACE_HOLDOVER)
		display_hold(a);

	/* Every vsynced turn and, tearing, every rebuild; never an urgent one. */
	bool every;
	const double period = pace_period(a, &every);
	const bool probe = every && vsync && !rebuild && period_probe(a);
	if (invited && !urgent && !BACKEND(a)->invites && period > 0.0 &&
		(vsync || rebuild) && !probe)
		*spaced = pace_space(a, period, every);

	/* Not a rebuild, which is answering the compositor, and not on top of the
	 * spacing, which has already spent the period. */
	if (invited && !*spaced && a->late_latch && !rebuild && vsync) {
		double d = late_hold(a);
		/* Zero is still a measurement: it is where the slack controller
		 * starts. */
		a->late_held = 0.0;
		if (d > 0.0) {
			/* What the hold cost, not what it asked for: it gives way to a
			 * configure or an urgent change. */
			const double t_hold = now_seconds();
			hold_wait(a, d, true);
			a->late_held = now_seconds() - t_hold;
		}
	}
	return invited;
}

/* Everything the frame is drawn from, taken under App::lock as one set. */
typedef struct {
	int          w, h, s;
	bool         fs, act, mx, cap;
	Zone         hb;
	int          ow, oh, curx, cury;
	int          pin_w, pin_h;
	FnlAntialias aa;
	char         outname[OUTPUT_NAME_MAX];
	char         title[TITLE_MAX];
	uint32_t     cfg_serial, user_gen;
	bool         cfg_pending;
	int          bar_h;
	bool         custom_bar;
} Latch;

/* The second look, taken after the wait: the newest input and configure are
 * the only ones worth drawing. Answers App::quit. An urgent change that
 * landed during the wait is spent here, so this frame answers it. */
static bool latch_take(App *a, Latch *l, bool *urgent, double *cb_at)
{
	fnl_mutex_lock(&a->lock);
	const bool quit = a->quit;
	l->w    = a->want_w;
	l->h    = a->want_h;
	l->s    = want_s120(a);
	l->hb   = a->hovered_button;
	l->fs   = a->want_fullscreen;
	l->act  = a->want_active;
	l->mx   = a->want_maximized;
	l->cap  = a->want_captured;
	l->ow   = a->want_out_w_pt;    l->oh   = a->want_out_h_pt;
	l->curx = a->want_cursor_x;    l->cury = a->want_cursor_y;
	l->pin_w = a->want_scene_w;    l->pin_h = a->want_scene_h;
	l->aa = a->want_aa;
	/* deco_apply_custom() writes the pair under this lock; latched together,
	 * a frame cannot be laid out at one height and painted at the other. */
	l->bar_h = deco_client(l->fs) ? a->deco_h : 0;
	l->custom_bar = a->deco_custom;
	l->user_gen = a->want_user_gen;
	memcpy(l->outname, a->want_output, sizeof l->outname);
	memcpy(l->title, a->title, sizeof l->title);
	*urgent |= a->want_urgent;
	a->want_urgent = false;
	l->cfg_serial  = a->cfg_serial;
	l->cfg_pending = a->cfg_pending;
	/* The invitation this frame answers: read ahead of the wait, the probe
	 * would react to a missed cycle a frame late. */
	*cb_at = a->frame_cb_at;
	fnl_mutex_unlock(&a->lock);
	return quit;
}

/* What the scene is rendered at, and so whether it needs an image of its
 * own: a pin smaller than the area, or either antialiasing mode at the area's
 * size. From the latched numbers, not the built extent, which this decides. */
static void scene_resolve(const Latch *l, int m, int *scene_w, int *scene_h)
{
	int area_w, area_h;
	content_px_for(l->w, l->h, l->s, m, l->bar_h, &area_w, &area_h);

	*scene_w = SCENE_NATIVE;
	*scene_h = SCENE_NATIVE;
	if (l->pin_w > 0 &&
		scene_pin_applies(l->pin_w, l->pin_h, area_w, area_h)) {
		*scene_w = l->pin_w;
		*scene_h = l->pin_h;
	} else if (l->aa != FNL_AA_NONE) {
		*scene_w = area_w;
		*scene_h = area_h;
	}
}

/* False when the rebuild failed, and the turn draws nothing. */
static bool swapchain_adopt(App *a, const Latch *l, int m)
{
	if (l->w != a->draw_w || l->h != a->draw_h ||
		l->s != a->draw_scale120 || m != a->draw_margin) {
		a->size_at = now_seconds();
		a->buffer_settled = false;
	}
	a->draw_w = l->w;
	a->draw_h = l->h;
	a->draw_scale120 = l->s;
	a->draw_margin = m;
	/* Glyphs are rasterised for a pixel size. A failure costs the text, not
	 * the frame: none is drawn until a later scale change succeeds. */
	if (RENDERER(a)->scale_changed(a, l->s) < 0)
		fnl_log(FNL_LOG_WARN,
				"no glyph atlas for this scale; text is not drawn "
				"until one loads");
	if (!RENDERER(a)->recreate_swapchain(a)) {
		/* Ask again next turn: the draw_* above have taken the new size, so
		 * a failure that was not a device loss would otherwise leave the old
		 * extent for good. */
		a->resized = true;
		return false;
	}
	a->resized = false;
	return true;
}

static void ack(App *a, uint32_t serial)
{
	BACKEND(a)->ack_configure(a, serial);
	a->acked_fullscreen = a->last_draw.is_fullscreen;
	a->acked_maximized  = a->last_draw.is_maximized;
}

/* Whether the ack goes ahead of the geometry. A compositor may hold the
 * geometry against the acked state between the two -- weston 15 applies a
 * content update held back for a FIFO barrier with whatever xdg_surface
 * state has arrived since -- so the first of them must leave a valid pair
 * on its own: maximized wants the configured size exactly, fullscreen at
 * most the configured size, floating anything. Leaving for floating, the
 * new state goes first; entering maximized or fullscreen from floating, the
 * new geometry. Between the two, fullscreen is the looser, so it is the one
 * that goes first. */
static bool ack_goes_first(const App *a)
{
	const bool fs = a->last_draw.is_fullscreen;
	const bool mx = a->last_draw.is_maximized;
	if (!fs && !mx)
		return true;
	return fs && a->acked_maximized && !a->acked_fullscreen;
}

/* What the window server is told about the buffer this frame commits; see
 * app.h for where it is called and why only there. weston 15 closes the
 * connection over two alternatives: the geometry of a restore sent at the
 * top of the turn rode the commit of a frame still answering the maximized
 * configure, and the geometry of a restore sent ahead of its ack was applied
 * by an earlier commit weston had held back (DESIGN *Wayland*). */
void app_surface_sync(App *a, uint32_t cfg_serial, bool cfg_pending)
{
	bool surface_changed = false;
	const bool ack_first = cfg_pending && ack_goes_first(a);

	if (ack_first)
		ack(a, cfg_serial);

	if (a->draw_w != a->applied_geo_w || a->draw_h != a->applied_geo_h ||
		a->draw_margin != a->applied_geo_m) {
		BACKEND(a)->set_geometry(a, a->draw_w, a->draw_h, a->draw_margin);
		a->applied_geo_w = a->draw_w;
		a->applied_geo_h = a->draw_h;
		a->applied_geo_m = a->draw_margin;
		surface_changed = true;
	}

	surface_changed |= BACKEND(a)->apply_scale(a);

	if (cfg_pending && !ack_first)
		ack(a, cfg_serial);
	/* The present's commit flushes too; this keeps the requests ahead of
	 * it whatever the driver's own queue does, and after the ack, so the
	 * pair leaves in one write. Thread-safe against the main thread's
	 * prepare_read and dispatch. */
	if (surface_changed)
		BACKEND(a)->flush(a);
}

/* A phase's seconds as FnlFrameStats' whole microseconds. */
static uint64_t stats_us(double seconds)
{
	return seconds > 0.0 ? (uint64_t)(seconds * 1e6 + 0.5) : 0;
}

/* The GPU times read this turn into the rows still waiting for them.
 * Searched from the newest, among the GPU_HOLD_FRAMES-and-some a time can
 * be for: a row handed over already, or overwritten, keeps none. A frame
 * drawn and not presented shares its index with the next, and is never
 * waiting (frame_row's gpu_ms), so the match is the presented one. Under
 * App::lock. */
static void gpu_late_fold(App *a) FNL_REQUIRES(a->lock)
{
	for (uint32_t k = 0; k < a->gpu_late_n; k++) {
		const uint32_t look = a->ring_n < 4 * GPU_HOLD_FRAMES
								  ? a->ring_n : 4 * GPU_HOLD_FRAMES;
		for (uint32_t i = 1; i <= look; i++) {
			FnlFrameSample *r = &a->ring[(a->ring_head + FRAME_RING - i) %
										 FRAME_RING];
			if (r->index == a->gpu_late[k].index) {
				if (r->gpu_ms == GPU_PENDING)
					r->gpu_ms = a->gpu_late[k].ms;
				break;
			}
		}
	}
	a->gpu_late_n = 0;
}

/* The running totals copied out for fnl_frame_stats(), with the two
 * figures that are now's rather than sums. Once a drawn frame. */
static void stats_publish(App *a, bool vsync, const FnlFrameSample *drawn)
{
	a->st.period_ms = a->disp_period * 1e3;
	a->st.hold_ms = (a->late_latch && vsync ? late_hold(a) : 0.0) * 1e3;
	fnl_mutex_lock(&a->lock);
	a->pub_st = a->st;
	if (drawn) {
		a->ring[a->ring_head] = *drawn;
		a->ring_head = (a->ring_head + 1) % FRAME_RING;
		if (a->ring_n < FRAME_RING)
			a->ring_n++;
		else
			a->ring_lost++;
	}
	gpu_late_fold(a);
	fnl_mutex_unlock(&a->lock);
}

/* One --frame-times line per FRAME_TIMES_WINDOW. The first figure is the whole
 * iteration and the seven after it are its parts; `fence` and `acquire` are
 * the two parts of the wait; `rebuild` is what making and
 * retiring swapchains cost, spread over the window's frames. `hold` is zero
 * in a tearing mode, where nothing is held whatever the controller last
 * settled on. `period` is the spacing's estimate of the display period
 * (disp_period_sample), zero until one is learnt. `other` is the frame
 * less `pace`, `draw` and `rebuild`: the loop's turns outside every part,
 * the time a frame spends waiting for what is not a part -- a turn that
 * drew nothing, the next size -- named, where the parts would not add up
 * to the frame and nothing would say why. */
static void frame_times_log(App *a, double dt, bool vsync)
{
	const double n = a->fc_frames;
	const double parts = a->fc_pace_sum + a->fc_draw_sum + a->fc_rebuild_sum;
	const double other = dt > parts ? dt - parts : 0.0;
	fnl_log(FNL_LOG_INFO,
			"frame %.2f ms: pace %.2f fence %.2f acquire %.2f record %.2f "
			"submit %.2f present %.2f rebuild %.2f other %.2f (draw %.2f, "
			"hold %.2f, %d fps, gpu idle %d/%d, miss %d/%d, period %.2f)",
			dt * 1e3 / n, a->fc_pace_sum * 1e3 / n,
			a->fc_fence_sum * 1e3 / n,
			(a->fc_wait_sum - a->fc_fence_sum) * 1e3 / n,
			a->fc_record_sum * 1e3 / n,
			a->fc_submit_sum * 1e3 / n,
			a->fc_present_sum * 1e3 / n,
			a->fc_rebuild_sum * 1e3 / n,
			other * 1e3 / n,
			a->fc_draw_sum * 1e3 / n,
			(a->late_latch && vsync ? late_hold(a) : 0.0) * 1e3,
			(int)fnl_atomic_load_u32(&a->fps_display),
			a->fc_idle, a->fc_frames,
			a->inst_miss, a->inst_frames, a->disp_period * 1e3);
	parts_log(a);
	a->inst_miss = a->inst_frames = 0;
	a->fc_pace_sum = a->fc_wait_sum = a->fc_record_sum = 0.0;
	a->fc_fence_sum = 0.0;
	a->fc_submit_sum = a->fc_present_sum = a->fc_draw_sum = 0.0;
	a->fc_rebuild_sum = 0.0;
	a->fc_frames = a->fc_idle = 0;
}

/* Frames that reached the screen, not turns of this loop. `now` is the
 * turn's reading from before the pacing wait, so both ends of the window are
 * read at the same point in the loop. True where a window closed, which is
 * when the cost figures are published too (app_cost_publish()): the three
 * move in one re-record. */
static bool fps_update(App *a, double now, bool vsync)
{
	if (a->frame_times && a->fc_frames > 0 &&
		now - a->fc_last_t >= FRAME_TIMES_WINDOW) {
		frame_times_log(a, now - a->fc_last_t, vsync);
		a->fc_last_t = now;
	}
	const double dt = now - a->fps_last_t;
	if (dt < FPS_WINDOW)
		return false;
	fnl_atomic_store_u32(&a->fps_display,
		(uint32_t)((double)(a->presents - a->fps_presents) / dt + 0.5));
	a->fps_presents = a->presents;
	a->fps_last_t = now;
	return true;
}

/* What this frame is drawn from; a difference from the last one's is what
 * makes every image re-record. True where something moved besides the
 * figures -- the frame rate, the two costs, the input latency and the
 * metrics panel's process figures -- which is what App::cost_counts is
 * told. */
static bool draw_state_update(App *a, const Latch *l)
{
	/* Frozen at 0 while motion is off, so the draw state stops changing and
	 * an idle window presents the command buffer it has. A calibration
	 * runs it whatever the caller set (rank.c's begin()), read here and
	 * not stored into App::scene_motion: a store put back at its end would
	 * undo a fnl_set_scene_motion() or an F2 made meanwhile. */
	float scene_time = 0.0f;
	if (fnl_atomic_load_bool(&a->scene_motion) ||
		fnl_atomic_load_bool(&a->calibrating)) {
		if (a->scene_motion_t0 == 0.0)
			a->scene_motion_t0 = a->latch_time;
		scene_time = (float)(a->latch_time - a->scene_motion_t0);
	} else {
		a->scene_motion_t0 = 0.0;
	}

	FnlDrawState ds = {
		.is_fullscreen  = l->fs,
		.is_active      = l->act,
		.is_maximized   = l->mx,
		.pointer_captured = l->cap,
		.cursor_x       = l->curx,
		.cursor_y       = l->cury,
		.hovered_button = l->hb,
		.bar_h          = l->bar_h,
		.custom_bar     = l->custom_bar,
		.user_gen       = l->user_gen,
		.out_w_pt       = l->ow,
		.out_h_pt       = l->oh,
		/* So a caller drawing fnl_fps() gets a frame when it moves, and
		 * one drawing fnl_frame_gpu_ms(), fnl_frame_cpu_ms() and
		 * fnl_input_latency_ms() too. */
		.fps            = (int)fnl_atomic_load_u32(&a->fps_display),
		.cost_gpu_us    = fnl_atomic_load_u32(&a->cost_gpu_us),
		.cost_cpu_us    = fnl_atomic_load_u32(&a->cost_cpu_us),
		.input_lat_us   = fnl_atomic_load_u32(&a->input_lat_us),
		/* The panel, and the process's figures it shows (metrics.c),
		 * which stand still while it is hidden. */
		.metrics        = fnl_atomic_load_bool(&a->metrics_shown),
		.met_cpu_pm     = a->met_cpu_pm,
		.met_ram_mib    = a->met_ram_mib,
		.met_vram_mib   = a->met_vram_mib,
		.met_gpu_pm     = a->met_gpu_pm,
		/* What was built, not what was asked for; zero is the native path. */
		.scene_w        = (int)a->scene_extent.w,
		.scene_h        = (int)a->scene_extent.h,
		.scene_time     = scene_time,
		.veil           = veil_step(a, a->latch_time),
	};
	ds.aa = l->aa;
	memcpy(ds.output, l->outname, sizeof ds.output);
	memcpy(ds.title, l->title, sizeof ds.title);
	/* The width belongs to the string the frame draws, not to whatever was
	 * set since: measured here, before anything of this frame is laid out,
	 * and read by the hit test as the width of the bar on screen. */
	if (strcmp(ds.title, a->last_draw.title) != 0)
		fnl_atomic_store_u32(&a->title_w,
							 (uint32_t)text_width(ds.title, TEXT_UI));
	FnlDrawState other = ds;
	other.fps = a->last_draw.fps;
	other.cost_gpu_us = a->last_draw.cost_gpu_us;
	other.cost_cpu_us = a->last_draw.cost_cpu_us;
	other.input_lat_us = a->last_draw.input_lat_us;
	other.met_cpu_pm = a->last_draw.met_cpu_pm;
	other.met_ram_mib = a->last_draw.met_ram_mib;
	other.met_vram_mib = a->last_draw.met_vram_mib;
	other.met_gpu_pm = a->last_draw.met_gpu_pm;
	const bool moved = !fnl_draw_state_eq(&other, &a->last_draw);
	if (!fnl_draw_state_eq(&ds, &a->last_draw)) {
		a->last_draw = ds;
		a->record_gen++;          /* every image re-records on next acquire */
	}
	return moved;
}

/* The late-latch signal where nothing invites. Last in the turn: the block
 * happens inside draw_frame(), and both halves must come from one turn. A
 * negative present_block reached no presentation engine. */
static void slack_learn(App *a, bool rebuilt, bool vsync, bool spaced)
{
	/* A frame drawn at rest to show the figures (rest_owed()) ran no pace
	 * turn and met an empty queue: its blocks are no period. A rebuild is
	 * never one, since it wakes the window first. */
	if (a->resting)
		return;
	if (rebuilt) {
		a->late_settle = LATE_SLACK_SETTLE;
	} else if (!BACKEND(a)->invites && vsync &&
			   a->present_block >= 0.0) {
		if (a->late_settle > 0) {
			a->late_settle--;
		} else if (!spaced) {
			/* A hold of -1.0 with vsync on is late latching switched off: the
			 * acquire blocked for what the hold would have taken, and the sum
			 * is still the period. Not a turn the spacing held, or the
			 * estimator is fed its own output. */
			const double held = a->late_held > 0.0 ? a->late_held : 0.0;
			const double period = held + a->display_block + a->device_block
								+ a->present_block + a->late_cost;
			if (a->presents <= LATE_SETTLE_PRESENTS) {
				disp_period_early(a, period);
				return;
			}
			const bool taken = disp_period_sample(a, period);
			const double parts[4] = { held, a->device_block,
									  a->present_block, a->late_cost };
			parts_period(a, parts, taken);

			if (a->late_latch && a->late_fixed <= 0.0 &&
				a->late_held >= 0.0)
				late_latch_slack(a, a->late_held, a->present_block);
		}
	}
}

/* After a sleep. Intel's driver on Windows can bring a swapchain back that
 * no longer blocks under FIFO: a window suspended before it had learnt a
 * period then draws unspaced, at about 200 fps on a 60 Hz panel, until
 * something rebuilds it. And the stall across the wake reads to the
 * measured controller as holds lengthening the turns, which cuts its
 * ceiling to nothing to creep back at 0.3 ms a second. So the swapchain is
 * rebuilt and the hold learnt again from where a new window starts. The
 * period is kept: the wake's first turns would teach it nonsense, and
 * spacing by the old one is safe either way. */
static void frame_resumed(App *a)
{
	const uint32_t n = fnl_atomic_load_u32(&a->resumes);
	if (n == a->resumes_seen)
		return;
	a->resumes_seen = n;
	fnl_log(FNL_LOG_INFO, "back from a sleep: the swapchain is rebuilt and "
			"the hold learnt again");
	a->resized = true;
	late_slack_forget(a);
}

/* ---- at rest ---------------------------------------------------------------
 *
 * A frame that would show what the last one showed is not drawn. Presenting
 * the command buffer it already has is cheap on a GPU and the whole cost on a
 * software one: lavapipe rasterised the same image sixty times a second, and
 * the X server took each one in, for 85% of a core between them on a window
 * nobody touched. DESIGN *At rest*. */

/* Whether this turn has anything to show: a change -- something to answer,
 * or a recording the last submitted frame did not draw -- frames still owed
 * after one, or a window too new to have learnt its period. Answered no, the
 * turn draws nothing and the next one starts in rest_wait(). The turn that
 * draws again restarts what would read the gap as a measurement: the
 * frame-rate window and --frame-times' sums, the spacing's anchor, and the
 * slack controller, since FIFO's queue is empty and its first turns block on
 * nothing. `sampling` (fnl_set_cost_sampling()) keeps the frame-rate window:
 * the counter then goes on through the rest, which is the rate it reports.
 *
 * `figures` is a frame drawn at rest only to show the figures, or to sample
 * them: drawn, and the window stays at rest. Woken, the next turn would run
 * the pacing for a frame it does not draw, and the display hold
 * (display_hold()) would take each such frame, a fifth or a half of a
 * second apart, for a missed one and deepen the queue the hold keeps
 * shallow. `vsync` is the turn's, which the frames it may still report
 * were paced by. */
static bool rest_owed(App *a, bool changed, bool figures, double latched,
					  bool sampling, bool vsync)
{
	if (figures && a->resting && a->rest_settle == 0 &&
		a->presents > REST_AFTER_PRESENTS)
		return true;
	if (changed || a->rest_settle > 0 ||
		a->presents <= REST_AFTER_PRESENTS) {
		if (a->resting) {
			a->resting = false;
			a->late_settle = LATE_SLACK_SETTLE;
			a->pace_at = latched;
			if (!sampling) {
				a->fps_last_t = a->fc_last_t = latched;
				a->fps_presents = a->presents;
			}
			a->fc_pace_sum = a->fc_wait_sum = a->fc_record_sum = 0.0;
			a->fc_fence_sum = 0.0;
			a->fc_submit_sum = a->fc_present_sum = a->fc_draw_sum = 0.0;
			a->fc_rebuild_sum = 0.0;
			a->fc_frames = a->fc_idle = 0;
			parts_reset(a);
		}
		return true;
	}
	/* The frames drawn moving, said as such before the rest's lines count
	 * frames of their own. */
	if (!a->resting && a->frame_times && a->fc_frames > 0) {
		frame_times_log(a, latched - a->fc_last_t, vsync);
		a->fc_last_t = latched;
	}
	a->resting = true;
	/* So a reader of --frame-times sees a rate, not silence. fnl_fps() keeps
	 * the one the window last drew at, unless sampling, when fps_update()
	 * goes on counting. The frames drawn at rest to show the figures, which
	 * leave the window at rest, are said as such: none answered a change. */
	if (a->frame_times && !sampling &&
		latched - a->fc_last_t >= FRAME_TIMES_WINDOW) {
		if (a->fc_frames > 0)
			fnl_log(FNL_LOG_INFO, "frame at rest: %d drawn to show the "
					"figures, none for a change", a->fc_frames);
		else
			fnl_log(FNL_LOG_INFO, "frame at rest: 0 fps, nothing drawn");
		a->fc_last_t = latched;
		a->fc_frames = a->fc_idle = 0;
	}
	return false;
}

/* For a signal on App::cond, which a change the frame shows sends, or one
 * period. The period is for a change that signals nothing, and also for one
 * signalled between rest_owed() and here: either waits as long for its frame
 * as it would have for the next present. */
static void rest_wait(App *a)
{
	bool every;
	double t = pace_period(a, &every);
	if (t <= 0.0)
		t = REST_POLL;
	fnl_mutex_lock(&a->lock);
	if (!a->quit && !a->want_urgent)
		fnl_cond_wait_for(&a->cond, &a->lock, t);
	fnl_mutex_unlock(&a->lock);
}

/* Whether the backend asked for its surface to be made again since the
 * last look, and if so hand the request to the renderer, which raises what
 * lost() reports. Asked twice a turn, at the top and after the latch: the
 * backend counts the request before it publishes the size that goes with
 * it, so a latch that reads the new size also sees the count, and the turn
 * goes round to the rebuild instead of building a swapchain of the new size
 * on the old surface -- which a display surface refuses. */
static bool surface_remake_owed(App *a)
{
	const uint32_t remakes = fnl_atomic_load_u32(&a->surface_remakes);
	if (remakes == a->surface_remakes_seen)
		return false;
	a->surface_remakes_seen = remakes;
	RENDERER(a)->remake_surface(a);
	return true;
}

/* Whether the renderer says a present would be swallowed (FnlRendererVtbl's
 * occluded), asked afresh each time: after the park it is what notices the
 * window is shown again. `forced` is FNL_FAULT("occluded"), sampled once a
 * turn so that a counted spec forces the whole turn -- no window here can
 * be made occluded on demand: one covered by another is not reported so on
 * Windows 11, only one nothing is shown of. */
static bool renderer_covered(App *a, bool forced)
{
	return RENDERER(a)->occluded(a) || forced;
}

void *render_loop(void *arg)
{
	App *a = arg;
	app_thread_set(APP_THREAD_RENDER);
	a->fps_last_t = a->fc_last_t = now_seconds();
	fnl_mutex_lock(&a->lock);
	a->ring_done = false;
	fnl_mutex_unlock(&a->lock);
	a->gpu_late_n = 0;
	/* fnl_run() left the device before starting this thread, and enters it
	 * again after the join. */
	RENDERER(a)->thread_enter(a);

	for (;;) {
		if (a->resting)
			rest_wait(a);

		/* First look, deliberately narrow: whether there is a frame to draw
		 * and whether it waits. Everything the frame is *drawn from* is taken
		 * after that wait. */
		fnl_mutex_lock(&a->lock);
		bool quit = a->quit;
		int  w    = a->want_w;
		int  h    = a->want_h;
		int  s    = want_s120(a);
		bool fs   = a->want_fullscreen;
		bool urgent = a->want_urgent;
		a->want_urgent = false;
		double cb_at = a->frame_cb_at;
		fnl_mutex_unlock(&a->lock);

		if (quit)
			break;

		/* Read once a turn, so a turn's phases are all timed or none. */
		a->counting = fnl_atomic_load_bool(&a->stats_on);
		a->timing = a->frame_times || a->counting;

		/* No renderer at all until the one owed builds: nothing below may
		 * be asked of it. */
		if (a->renderer_owed) {
			device_recover(a);
			if (a->renderer_owed) {
				fnl_sleep_seconds(0.2);
				continue;
			}
		}

		surface_remake_owed(a);
		app_gpu_driver_poll(a);
		app_gpu_hotplug_poll(a);
		app_rank_tick(a);
		gpu_switch_owed(a);

		/* Here rather than where it was noticed: the frame that hit it has
		 * unwound, and the quit check above means a GPU stuck in a reset loop
		 * cannot keep the window from closing. */
		if (RENDERER(a)->lost(a)) {
			device_recover(a);
			/* No frame was drawn, so the urgent flag is still owed. Put back
			 * rather than left standing at the read: hold_wait() tests it,
			 * and would cut short the hold of every frame that answers one. */
			if (urgent)
				app_mark_urgent(a);
			continue;     /* still set = try again, paced by the sleep inside */
		}

		if (w == 0 || h == 0) {
			if (urgent)          /* no frame drawn, so still owed; see above */
				app_mark_urgent(a);
			/* Minimized, on Win32 and Cocoa: no lift is seen. */
			veil_unseen(a);
			fnl_sleep_seconds(NO_SIZE_RETRY);
			continue;
		}

		double now = now_seconds();

		/* Adopted at the rebuild, not here, so present_mode names the
		 * swapchain that exists and a toggle and back costs no rebuild. */
		bool vsync = fnl_atomic_load_bool(&a->vsync);

		int m = deco_margin(a, fs);

		frame_resumed(a);
		/* Images kept larger than the window through a drag are made its
		 * own size once the size has stood (app_buffer_at_rest()): one
		 * rebuild, and the window is flipped onto a plane where Windows
		 * composed it. Asked with the latch's own test below as well, which
		 * replaces this one's. */
		const bool settle = a->buffer.w > 0 && !a->buffer_settled &&
							app_buffer_at_rest(a);
		bool rebuild = swapchain_stale(a, w, h, s, m, vsync) || settle;

		/* Every vsynced frame, not only the rebuilt ones: pacing only
		 * rebuilds leaves the frames between them blocking in the FIFO
		 * acquire. Tearing, only a rebuild. Never a rebuild answering
		 * something urgent (App::want_urgent): the compositor holds the next
		 * invitation until it has a buffer at the new size, a stand-off only
		 * the timeout ends. */
		bool pace = rebuild ? !urgent : vsync;

		/* The backend knows about minimized and hidden, the renderer about a
		 * present that would be swallowed. Wayland answers true to both and
		 * says it by not inviting, which the frame_cb_at terms read. */
		const bool forced_cover = FNL_FAULT("occluded");
		const bool shown = BACKEND(a)->window_on_screen(a);
		const bool covered = shown && renderer_covered(a, forced_cover);
		bool on_screen = shown && !covered;

		/* A tearing mode never waits, so by itself it would present flat out
		 * to a window nobody can see. Once nothing has invited for the length
		 * of the pacing wait, or the backend or the renderer says the window
		 * is not shown, take the wait after all: it parks until the window is
		 * back. */
		if (!pace && ((cb_at > 0.0 &&
					   now - cb_at > FRAME_WAIT_S) ||
					  !on_screen))
			pace = true;
		bool invited = true;
		bool spaced  = false;
		const double t_pace = a->timing ? now_seconds() : 0.0;
		/* This turn's frame for fnl_frame_samples(), filled as its phases
		 * end; pushed at the turn's end where a frame was drawn. */
		FnlFrameSample frame_row;
		memset(&frame_row, 0, sizeof frame_row);
		bool row_drawn = false;
		/* Nothing measurable until pace_turn() says so; see App::late_held.
		 * The display hold's block too: a turn parked or at rest runs no
		 * pace_turn(), and slack_learn() would sum an earlier turn's. */
		a->late_held = -1.0;
		a->display_block = 0.0;
		a->pace_ran = false;
		/* At rest the wait was rest_wait()'s, and a frame that answers it is
		 * held for nothing more: the queue is empty, so it is shown at the
		 * next refresh however late in the period it is submitted. */
		/* Covered, by the renderer's account and not the backend's: the
		 * backend's wait answers at once for a window it counts as shown
		 * (Win32's covered window is), and a covered window's present
		 * returns at once too, so with vsync off nothing else would hold
		 * the loop. Parked here as a minimized window is, and the skip
		 * below draws nothing while it stays covered. Not with a rebuild
		 * owed: a size is still answered at once (on Win32 the window's
		 * thread is waiting for it, size_await()). */
		if (covered && !rebuild) {
			app_park(a, true);
			invited = false;
		} else if (pace) {
			invited = a->resting ? BACKEND(a)->frame_wait(a, !rebuild)
								 : pace_turn(a, rebuild, urgent, vsync,
											 &spaced);
		}
		/* Whether or not either ran: a tearing frame that waited for nothing
		 * reads as zero rather than being left out of the mean. */
		if (a->timing) {
			const double dt = now_seconds() - t_pace;
			a->fc_pace_sum += dt;
			if (a->counting) {
				a->st.pace_us += stats_us(dt);
				frame_row.pace_ms = (float)(dt * 1e3);
			}
		}

		Latch l;
		if (latch_take(a, &l, &urgent, &cb_at))
			break;
		if (surface_remake_owed(a)) {
			/* No frame drawn, so still owed; the top recovers. */
			if (urgent)
				app_mark_urgent(a);
			continue;
		}
		if (l.w == 0 || l.h == 0) {
			/* A minimize landed during the wait; the flag is still owed. */
			if (urgent)
				app_mark_urgent(a);
			veil_unseen(a);
			continue;
		}

		/* One reading for everything the frame dates from here on; `now` is
		 * from before the wait (see fps_update). */
		const double latched = now_seconds();

		/* Opens TRANSITION_GRACE for the skip below. */
		if (urgent)              /* including one that landed mid-wait */
			a->transition_at = latched;

		vsync = fnl_atomic_load_bool(&a->vsync);
		m = deco_margin(a, l.fs);
		rebuild = swapchain_stale(a, l.w, l.h, l.s, m, vsync) || settle;

		int scene_w, scene_h;
		scene_resolve(&l, m, &scene_w, &scene_h);
		/* Apart from the swapchain's rebuild: this replaces one image. */
		const bool scene_rebuild = (scene_w != a->draw_scene_w ||
									scene_h != a->draw_scene_h);

		/* No invitation came and nothing has to be answered: the surface is
		 * not on screen, so no frame. A configure, a rebuild or the
		 * TRANSITION_GRACE after a state change still gets one.
		 *
		 * on_screen is asked again: a window restored during the wait is
		 * back by now. `cb_at > 0.0` is Wayland saying an invitation has
		 * ever arrived; only Wayland writes frame_cb_at, so `!on_screen` is
		 * what reaches this on the others. */
		on_screen = BACKEND(a)->window_on_screen(a) &&
					!renderer_covered(a, forced_cover);
		if (!invited && (cb_at > 0.0 || !on_screen) &&
			!rebuild && !scene_rebuild &&
			!l.cfg_pending &&
			latched - a->transition_at > TRANSITION_GRACE) {
			/* Uninvited for VEIL_STALL and not merely this turn's
			 * FRAME_WAIT_S: a compositor may take that long to show a new
			 * surface's first frame, which the lift is there for. */
			if (!on_screen || latched - cb_at > VEIL_STALL)
				veil_unseen(a);
			/* The frame-rate window restarts, or the idle time would read as
			 * a near-zero rate once back on screen. */
			a->fps_last_t = a->fc_last_t = latched;
			a->fps_presents = a->presents;
			continue;
		}

		/* The scene's image first. Both rebuilds drain the queue (see
		 * scene_set_size), so together they cost one drain. */
		const double t_rebuild = a->timing ? now_seconds() : 0.0;
		if (rebuild || scene_rebuild) {
			/* For pace_space(). A rebuild made by a paced turn is that
			 * turn and dates from where it was anchored: `latched` is from
			 * after the hold came back, so it carries the wait's lateness
			 * and would add it to the cycle once per rebuild. */
			a->rebuild_at = a->pace_ran ? a->pace_at : latched;
			RENDERER(a)->set_scene_size(a, scene_w, scene_h);
			/* What was asked for, not what came of it: a refusal recorded as
			 * "none current" would ask again every turn and keep
			 * scene_rebuild true, which defeats the skip above. */
			a->draw_scene_w = scene_w;
			a->draw_scene_h = scene_h;
		}

		if (rebuild && !swapchain_adopt(a, &l, m))
			continue;
		/* Not where the latch brought a size after all: that one has not
		 * stood, and its own rest asks again. */
		if (settle && app_buffer_at_rest(a))
			a->buffer_settled = true;
		if (rebuild && a->counting)
			a->st.rebuilds++;
		/* With the retiring further down, which is the other half of what
		 * a new swapchain costs and comes turns later: on a driver whose
		 * create and destroy each take most of a period this is where a
		 * resize's frame rate went, and no other column shows it. */
		if (a->timing) {
			const double dt = now_seconds() - t_rebuild;
			a->fc_rebuild_sum += dt;
			if (a->counting) {
				a->st.rebuild_us += stats_us(dt);
				frame_row.rebuild_ms += (float)(dt * 1e3);
			}
		}

		/* Before the draw state is built: the reading is one of the fields
		 * compared, so the frame that moves it re-records with it. At rest
		 * too where the caller keeps the figures measured: the counter is
		 * then one of them, and reads the sampled frames' rate. The caller's
		 * ask alone: the metrics panel shown keeps none, or its own frames
		 * would be the frames its figures measure (DESIGN *The metrics
		 * panel*). */
		const bool sampling = fnl_atomic_load_bool(&a->cost_sampling);
		const bool window = !a->resting || sampling
						  ? fps_update(a, now, vsync) : false;

		/* The instrument runs under --no-late-latch and a pinned delay too.
		 * Only where frames are invited, so it reads 0/0 on every backend
		 * but Wayland. */
		if (a->timing && BACKEND(a)->invites && cb_at > 0.0 &&
			cb_at != a->inst_prev && a->presents > LATE_SETTLE_PRESENTS)
			inst_sample(a, cb_at);

		/* The probe. Gated on `invites`, not on `cb_at > 0.0` alone: a field
		 * only one backend writes is a permanent zero on the others, which
		 * reads as "no signal" rather than "no invitations". cb_at keeps it
		 * off the frames before the first callback. */
		if (a->late_latch && a->late_fixed <= 0.0 && BACKEND(a)->invites &&
			cb_at > 0.0 &&
			cb_at != a->late_prev_disp &&
			a->presents > LATE_SETTLE_PRESENTS)
			late_latch_feedback(a, cb_at);

		/* Here rather than at `latched`: late_cost is the frame's own cost,
		 * and a rebuild folded into it would walk the delay down. */
		a->latch_time = now_seconds();

		/* The frames finished since the last turn, at rest too, and the
		 * figures they move published at their own pace -- ahead of the
		 * draw state, which latches them. */
		app_cost_poll(a);
		app_cost_publish(a, a->latch_time, window);
		events_latency_publish(a, a->latch_time, window);
		/* The metrics panel's process figures here only at rest, where no
		 * hold was run and a sample delays nothing that is paced. A
		 * drawing window takes its sample after the present, below: here
		 * it would be past the hold and outside late_cost, a part of the
		 * frame the pacing never budgets for (NVML's query is half a
		 * millisecond). */
		if (a->resting)
			app_metrics_sample(a, a->latch_time);

		const bool moved = draw_state_update(a, &l);

		/* A pending event is a reason to record (see events_pending). Gated
		 * on a callback that could drain it, or the first event would
		 * re-record every frame for good. */
		const bool events = (a->cb.on_overlay || a->cb.on_record) &&
							events_pending(a);
		if (events)
			a->record_gen++;

		/* After the rebuild and before draw_frame: the one point in the turn
		 * where the extent, margin, scale and latched state agree, and ahead
		 * of on_overlay. */
		app_publish_window(a, &a->last_draw);

		const bool changed = rebuild || scene_rebuild || l.cfg_pending ||
							 urgent || a->record_gen != a->shown_gen;
		const bool other = rebuild || scene_rebuild || l.cfg_pending ||
						   urgent || moved || events;
		/* A resting window that draws for the figures alone: one that shows
		 * them moved -- the metrics panel's process figures among them, at
		 * most once a METRICS_WINDOW -- or a sample a caller keeps them
		 * measured with (fnl_set_cost_sampling()), one frame each
		 * COST_WINDOW. Neither is a change that frames are still owed after
		 * (REST_SETTLE). */
		const bool figures = changed && !other && a->resting;
		const bool sample = sampling && !changed && a->resting &&
							latched - a->cost_sampled_at >= COST_WINDOW;
		/* Whether the frames drawn from here are the figures' to measure --
		 * a publish of their own owed, and their times in the means: not
		 * where a resting window draws only to show them, unless that is
		 * what it was asked to keep doing. */
		if (other || sampling)
			a->cost_counts = true;
		else if (figures)
			a->cost_counts = false;
		if (!rest_owed(a, changed || sample, figures || sample, latched,
					   sampling, vsync))
			continue;

		const double t_draw = a->timing ? now_seconds() : 0.0;
		const int idle_was = a->fc_idle;
		a->fc_wait = a->fc_record = a->fc_submit = a->fc_present = 0.0;
		/* Each renderer sets it early in draw_frame(); zeroed so a frame that
		 * stops before then adds no other frame's wait to `fence`. */
		a->device_block = 0.0;
		a->gpu_timed = false;
		/* The frame's index is the presents before it, and it was presented
		 * where that count moved: a submit whose present the display
		 * refused -- out of date -- counts nothing, and is a frame drawn and
		 * not shown, with its own index and not the one before. */
		const uint64_t presents_before = a->presents;
		bool submitted = RENDERER(a)->draw_frame(a, a->frame, &a->last_draw,
												 l.cfg_serial, l.cfg_pending);
		const bool presented = a->presents != presents_before;
		/* `draw` minus the four phase fields the renderer fills is time in
		 * the renderer outside any phase named here. */
		if (a->timing) {
			a->fc_draw_sum    += now_seconds() - t_draw;
			a->fc_wait_sum    += a->fc_wait;
			/* The fence share of `wait`, which the line reports apart from
			 * the rest of it. Bounded by it: `wait` is the whole span. */
			a->fc_fence_sum   += a->device_block < a->fc_wait
									 ? a->device_block : a->fc_wait;
			a->fc_record_sum  += a->fc_record;
			a->fc_submit_sum  += a->fc_submit;
			a->fc_present_sum += a->fc_present;
			a->fc_frames++;
		}
		if (a->counting) {
			FnlFrameStats *st = &a->st;
			const double fence = a->device_block < a->fc_wait
									 ? a->device_block : a->fc_wait;
			st->frames++;
			st->presents += presented;
			st->draw_us    += stats_us(now_seconds() - t_draw);
			st->fence_us   += stats_us(fence);
			st->acquire_us += stats_us(a->fc_wait - fence);
			st->record_us  += stats_us(a->fc_record);
			st->submit_us  += stats_us(a->fc_submit);
			st->present_us += stats_us(a->fc_present);
			st->gpu_idle   += (uint64_t)(a->fc_idle - idle_was);
			frame_row.index      = presents_before;
			frame_row.time       = latched - a->started_at;
			frame_row.fence_ms   = (float)(fence * 1e3);
			frame_row.acquire_ms = (float)((a->fc_wait - fence) * 1e3);
			frame_row.record_ms  = (float)(a->fc_record * 1e3);
			frame_row.submit_ms  = (float)(a->fc_submit * 1e3);
			frame_row.present_ms = (float)(a->fc_present * 1e3);
			frame_row.draw_ms    = (float)((now_seconds() - t_draw) * 1e3);
			frame_row.presented  = presented;
			frame_row.gpu_ms     = a->gpu_timed && submitted ? GPU_PENDING
															 : -1.0f;
			row_drawn = true;
		}
		a->frame = (a->frame + 1) % MAX_FRAMES_IN_FLIGHT;

		/* Only count down on real submissions. Each submit is queue-ordered
		 * after the retired swapchain's final present (so its render_done
		 * semaphore wait has completed) and waits the frame's in_flight fence
		 * at draw_frame entry (so its command buffers are idle). Decrementing
		 * on skipped frames -- e.g. acquire timeouts -- would free those
		 * resources while they may still be in use. */
		if (submitted) {
			a->shown_gen = a->record_gen;
			a->cost_sampled_at = latched;
			if (changed && !figures)
				a->rest_settle = REST_SETTLE;
			else if (a->rest_settle > 0)
				a->rest_settle--;
			const double t_retire = a->timing ? now_seconds() : 0.0;
			RENDERER(a)->retire_tick(a);
			/* The veil is down: the cover and the readback are let go,
			 * once the device is done with the frames that read them. */
			if (a->veil_phase == VEIL_NONE)
				RENDERER(a)->cover_release(a);
			if (a->timing) {
				const double dt = now_seconds() - t_retire;
				a->fc_rebuild_sum += dt;
				if (a->counting) {
					a->st.rebuild_us += stats_us(dt);
					frame_row.rebuild_ms += (float)(dt * 1e3);
				}
			}
			/* The other renderers' GPUs, once the window's first frame is
			 * up: listing them loads their drivers, which that frame does
			 * not wait for. Not while a move to another kernel driver runs,
			 * which a driver loaded would hold up or blind. */
			if (!a->gpu_others_listed && !a->mover_started &&
				!a->gpu_cross_landing)
				app_gpu_list_start(a);
			app_frame_cost(a);
		}

		/* The panel's sample for a drawing window, with the frame handed
		 * over: the next turn's latch takes it, and its cost is outside the
		 * frame's, between a present and the next latch. Where frames are
		 * invited (Wayland) it falls in the wait for the next invitation;
		 * where they are not, the backend's wait answers at once and the
		 * hold after it starts that much later (see above). */
		app_metrics_sample(a, now_seconds());

		if (a->counting)
			stats_publish(a, vsync, row_drawn ? &frame_row : NULL);

		slack_learn(a, rebuild || scene_rebuild, vsync, spaced);
	}
	/* The last frames' GPU times, those already done, and then every row
	 * handed over: nothing will be drawn to read the rest by. */
	if (a->renderer && a->renderer_ready && a->counting)
		RENDERER(a)->gpu_time_poll(a);
	fnl_mutex_lock(&a->lock);
	gpu_late_fold(a);
	a->ring_done = true;
	fnl_mutex_unlock(&a->lock);
	if (a->renderer && a->renderer_ready)
		RENDERER(a)->thread_leave(a);
	return NULL;
}
