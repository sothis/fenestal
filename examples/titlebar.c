/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A titlebar of the caller's own: drawn by the caller, while the library
 * keeps the drag, the double-click and the resize edges. The controls are
 * claimed every frame, beside the drawing they describe.
 *
 * Shows: fnl_titlebar_custom fnl_titlebar_claim fnl_titlebar_height
 *        fnl_titlebar_owned fnl_minimize
 *        fnl_set_maximized fnl_is_maximized fnl_is_active fnl_begin_move
 *        fnl_begin_resize fnl_titlebar_reserve
 */

#include <fenestal/fenestal.h>

#include <string.h>

#define BAR_H 32
#define BTN_W 40
/* The strip ends this far from the window's edge. The library's resize edge
 * wins over a claim at the border (fnl_titlebar_claim()), so a button flush
 * with the edge would start a resize where its outer points are. */
#define BTN_MARGIN 8

typedef struct {
	int buttons_x;      /* where this frame drew the three buttons */
} Bar;

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Bar *b = ud;
	(void)frame;
	int w, h;
	fnl_window_size_pt(app, &w, &h);
	b->buttons_x = w - 3 * BTN_W - BTN_MARGIN;

	const FnlColor active = { 0.20f, 0.22f, 0.28f, 1.0f };
	const FnlColor idle   = { 0.16f, 0.16f, 0.18f, 1.0f };
	const FnlColor bar    = fnl_is_active(app) ? active : idle;
	const FnlColor ink  = { 0.9f, 0.9f, 0.9f, 1.0f };
	/* The band as the library reports it, rather than the height asked
	 * for: 0 in fullscreen, where there is none and nothing of the bar
	 * may be drawn or claimed. */
	const int bar_h = fnl_titlebar_owned(app) ? fnl_titlebar_height(app) : 0;
	fnl_overlay_rect(app, 0, 0, w, bar_h, bar);
	if (bar_h > 0) {
		fnl_overlay_text(app, 12, 8, FNL_TEXT_UI, ink, "my own titlebar");
		fnl_overlay_text(app, b->buttons_x + 14, 8, FNL_TEXT_UI, ink, "_");
		fnl_overlay_text(app, b->buttons_x + BTN_W + 14, 8, FNL_TEXT_UI, ink,
						 fnl_is_maximized(app) ? "o" : "O");
		fnl_overlay_text(app, b->buttons_x + 2 * BTN_W + 14, 8, FNL_TEXT_UI,
						 ink, "x");
		/* Claimed, so a press there is an event and not a window drag. The
		 * rest of the bar stays the drag strip. */
		fnl_titlebar_claim(app, b->buttons_x, BTN_MARGIN, 3 * BTN_W,
						   bar_h - BTN_MARGIN);
	}

	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		/* The strip's top is an inset too: above it is the resize edge. */
		if (ev.type != FNL_EVENT_BUTTON || !ev.button.pressed ||
			ev.button.y < BTN_MARGIN || ev.button.y >= bar_h)
			continue;
		/* Division truncates toward zero, so a press just left of the
		 * buttons would otherwise read as the first. */
		const int i = ev.button.x < b->buttons_x
					? -1 : ((int)ev.button.x - b->buttons_x) / BTN_W;
		if (ev.button.button == FNL_BUTTON_RIGHT) {
			/* A drag of our own choosing, from the press still held. */
			if (ev.button.x < b->buttons_x)
				fnl_begin_move(app);
			else
				fnl_begin_resize(app, FNL_EDGE_TOP_RIGHT);
		} else if (i == 0) {
			fnl_minimize(app);
		} else if (i == 1) {
			fnl_set_maximized(app, !fnl_is_maximized(app));
		} else if (i == 2) {
			fnl_quit(app);
		}
	}
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "titlebar";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	/* Before fnl_run(): the bar is ours from the first frame. With the
	 * library's own bar, fnl_titlebar_reserve() would lend a run of it. */
	if (!fnl_titlebar_custom(app, BAR_H, 3 * BTN_W + 120)) {
		int x, y, w, h;
		(void)fnl_titlebar_reserve(app, 100, &x, &y, &w, &h);
	}
	Bar b = { 0 };
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &b;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}
