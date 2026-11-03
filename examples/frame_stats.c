/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * Where a moving scene's frames spend their time: the frame loop's totals,
 * read once a second and subtracted, said on stderr as each phase's mean
 * per frame. F2 still switches the motion, which this turns on at start.
 *
 * Shows: fnl_set_frame_stats fnl_frame_stats fnl_set_scene_motion
 *        fnl_scene_motion fnl_output_refresh_mhz
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>

typedef struct {
	FnlFrameStats last;
	double        at;
} Watch;

static double per_frame_ms(uint64_t now_us, uint64_t was_us, uint64_t n)
{
	return n ? (double)(now_us - was_us) / 1e3 / (double)n : 0.0;
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Watch *w = ud;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
	}
	/* Asked for every frame, so the loop never rests while it is watched;
	 * a still scene would otherwise draw nothing to count. */
	fnl_invalidate(app);
	if (frame->time - w->at < 1.0)
		return;
	w->at = frame->time;

	FnlFrameStats now;
	memset(&now, 0, sizeof now);
	now.size = sizeof now;
	if (!fnl_frame_stats(app, &now))
		return;
	const uint64_t n = now.frames - w->last.frames;
	fprintf(stderr, "%llu frames, motion %s: pace %.2f fence %.2f acquire "
			"%.2f record %.2f present %.2f ms, gpu %.2f ms, period %.2f ms "
			"(the display says %.2f)\n",
			(unsigned long long)n, fnl_scene_motion(app) ? "on" : "off",
			per_frame_ms(now.pace_us, w->last.pace_us, n),
			per_frame_ms(now.fence_us, w->last.fence_us, n),
			per_frame_ms(now.acquire_us, w->last.acquire_us, n),
			per_frame_ms(now.record_us, w->last.record_us, n),
			per_frame_ms(now.present_us, w->last.present_us, n),
			per_frame_ms(now.gpu_us, w->last.gpu_us,
						 now.gpu_samples - w->last.gpu_samples),
			now.period_ms,
			fnl_output_refresh_mhz(app)
				? 1e6 / fnl_output_refresh_mhz(app) : 0.0);
	w->last = now;
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "frame stats";
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	fnl_set_frame_stats(app, true);
	fnl_set_scene_motion(app, true);
	Watch w;
	memset(&w, 0, sizeof w);
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &w;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}
