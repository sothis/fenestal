/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A panel of options drawn over the scene and worked with the mouse: each
 * row is claimed every frame, beside its drawing, so a press on it is an
 * event and captures nothing, while a click anywhere else in the scene
 * still takes the pointer.
 *
 * Shows: fnl_claim fnl_is_pointer_captured
 */

#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <string.h>

#define ROW_H  24
#define ROW_W  220
#define ROWS   3

typedef struct {
	bool on[ROWS];
	int  hover;          /* the row under the pointer, or -1 */
} Panel;

static const char *const NAMES[ROWS] = { "vsync", "motion", "load" };

/* Which row a window point is on, or -1. The same arithmetic the drawing
 * and the claims use, so a press lands where the row is shown. */
static int row_at(int px, int py, double x, double y)
{
	if (x < px || x >= px + ROW_W || y < py || y >= py + ROWS * ROW_H)
		return -1;
	return ((int)y - py) / ROW_H;
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Panel *p = ud;
	(void)frame;
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	const int px = cx + 16, py = cy + 16;

	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type == FNL_EVENT_MOTION) {
			p->hover = row_at(px, py, ev.motion.x, ev.motion.y);
			fnl_set_cursor(app, p->hover >= 0 ? FNL_CURSOR_POINTER
											  : FNL_CURSOR_DEFAULT);
		} else if (ev.type == FNL_EVENT_BUTTON && ev.button.pressed &&
				   ev.button.button == FNL_BUTTON_LEFT &&
				   !fnl_is_pointer_captured(app)) {
			/* A press outside the rows is the scene's, and the library
			 * has captured the pointer for it by the next frame. */
			const int r = row_at(px, py, ev.button.x, ev.button.y);
			if (r >= 0)
				p->on[r] = !p->on[r];
		}
	}

	/* While captured there is no panel to point at: the cursor is hidden
	 * and a claim catches nothing until the capture is released. */
	if (fnl_is_pointer_captured(app))
		return;

	const FnlColor bg  = { 0.10f, 0.10f, 0.12f, 0.85f };
	const FnlColor lit = { 0.22f, 0.24f, 0.30f, 0.95f };
	const FnlColor ink = { 0.90f, 0.90f, 0.90f, 1.0f };
	for (int r = 0; r < ROWS; r++) {
		const int y = py + r * ROW_H;
		fnl_overlay_rect(app, px, y, ROW_W, ROW_H, r == p->hover ? lit : bg);
		fnl_overlay_text(app, px + 8, y + 5, FNL_TEXT_UI, ink, NAMES[r]);
		fnl_overlay_text(app, px + ROW_W - 40, y + 5, FNL_TEXT_UI, ink,
						 p->on[r] ? "on" : "off");
	}
	/* One rectangle for the whole panel: claims are few and coarse, and
	 * row_at() tells the rows apart. */
	fnl_claim(app, px, py, ROW_W, ROWS * ROW_H);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "panel";
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	Panel p = { .hover = -1 };
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &p;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}
