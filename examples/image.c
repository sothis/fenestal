/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A picture in the overlay: pixels the library copies, keeps and draws again
 * after a device rebuild, so the caller's own buffer may go on return.
 *
 * Shows: fnl_image_supported fnl_image_create fnl_overlay_image
 *        fnl_image_destroy fnl_overlay_rect fnl_overlay_box fnl_overlay_text
 *        fnl_poll_event fnl_quit fnl_invalidate fnl_window_size_pt
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	FnlImage *img;   /* NULL until the first overlay, and after a toggle */
	bool      want;  /* whether the picture is asked for at all */
} State;

/* A checkerboard with a solid border, RGBA8 and not premultiplied, which is
 * the one format there is. Made in the caller's own buffer: fnl_image_create()
 * reads it once and copies. */
static void put(unsigned char *px, int w, int x, int y, unsigned char r,
				unsigned char g, unsigned char b)
{
	unsigned char *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
	p[0] = r; p[1] = g; p[2] = b; p[3] = 0xFF;
}

/* The marker blocks are what says which way up the picture landed: the orange
 * one is at the top-left of the buffer, the magenta at the bottom-right, and a
 * draw that flipped either axis shows it. */
static void make_pixels(unsigned char *px, int w, int h)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const bool edge = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
			const bool on = ((x / 8) + (y / 8)) & 1;
			put(px, w, x, y, edge ? 0x20 : on ? 0xE8 : 0x30,
				edge ? 0x80 : on ? 0xE8 : 0x30,
				edge ? 0xC0 : on ? 0xE8 : 0x30);
		}
	for (int y = 3; y < 11; y++)
		for (int x = 3; x < 11; x++)
			put(px, w, x, y, 0xE0, 0x80, 0x20);        /* top-left */
	for (int y = h - 11; y < h - 3; y++)
		for (int x = w - 11; x < w - 3; x++)
			put(px, w, x, y, 0xC0, 0x20, 0xC0);        /* bottom-right */
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	State *s = ud;
	(void)frame;

	FnlEvent ev;
	while (fnl_poll_event(app, &ev))
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed && !ev.key.repeat) {
			if (ev.key.key == FNL_KEY_ESCAPE || ev.key.key == FNL_KEY_Q) {
				/* On the render thread, in the overlay, as the create was:
				 * fnl_image_destroy() is that thread's too. What is left at
				 * fnl_shutdown() is freed there anyway. */
				if (s->img) {
					fnl_image_destroy(s->img);
					s->img = NULL;
				}
				fnl_quit(app);
			}
			else if (ev.key.key == FNL_KEY_I)
				s->want = !s->want;   /* the picture off and on */
			fnl_invalidate(app);
		}

	/* Made here, in the overlay, because the calls are the render thread's
	 * ([overlay] in fenestal.h) -- a worker that has pixels posts them with
	 * fnl_post() and creates the image there. The buffer is the caller's and
	 * freed on return; the library kept its own copy. */
	if (s->want && !s->img) {
		if (!fnl_image_supported(app)) {
			/* A renderer that cannot draw a picture refuses the create; the
			 * app draws from strokes instead, as it did before there was
			 * one. */
			s->want = false;
		} else {
			const int w = 64, h = 64;
			unsigned char *px = malloc((size_t)w * (size_t)h * 4u);
			if (px) {
				make_pixels(px, w, h);
				s->img = fnl_image_create(app, w, h, FNL_IMAGE_RGBA8, px);
				free(px);
			}
			if (!s->img)
				s->want = false;
		}
	} else if (!s->want && s->img) {
		fnl_image_destroy(s->img);
		s->img = NULL;
	}

	int w_pt, h_pt;
	fnl_window_size_pt(app, &w_pt, &h_pt);
	const FnlColor panel = { 0.10f, 0.10f, 0.12f, 1.00f };
	const FnlColor ink   = { 0.95f, 0.95f, 0.90f, 1.00f };
	const int side = 128;   /* logical points the picture is drawn into */
	const int x = (w_pt - side) / 2, y = (h_pt - side) / 2;

	fnl_overlay_rect(app, x - 8, y - 8, side + 16, side + 16, panel);
	if (s->img) {
		/* Scaled to fit the rectangle: 64 pixels of picture in 128 points,
		 * which is two pixels a point at scale 1. */
		fnl_overlay_image(app, x, y, side, side, s->img);
	} else {
		fnl_overlay_box(app, (float)x, (float)y, (float)(x + side),
						(float)(y + side), 1.0f, ink);
	}
	fnl_overlay_text(app, x - 8, y + side + 12, FNL_TEXT_UI, ink,
					 "press i to toggle the picture, q to quit");
}

int main(int argc, char **argv)
{
	/* The baked font's licence travels with every binary that ships it. */
	if (argc > 1 && !strcmp(argv[1], "--licence")) {
		printf("%s\n\n%s\n", fnl_font_notice(), fnl_font_licence());
		return 0;
	}

	unsigned shift = FNL_VERSION_MAJOR == 0 ? 8 : 16;
	if ((fnl_version() >> shift) != ((uint32_t)FNL_VERSION >> shift)) {
		fprintf(stderr, "libfenestal is another release\n");
		return 1;
	}

	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "image";
	desc.width_pt       = 480;
	desc.height_pt      = 360;
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers      = FNL_RENDERERS_ANY;

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		char why[256];
		fnl_error_copy(why, sizeof why);
		fprintf(stderr, "fnl_init: %s: %s\n", fnl_result_str(rc), why);
		return 1;
	}

	State s;
	memset(&s, 0, sizeof s);
	s.want = true;

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size       = sizeof cb;
	cb.ud         = &s;
	cb.on_overlay = on_overlay;
	rc = fnl_run(app, &cb);
	if (rc < 0)
		fprintf(stderr, "fnl_run: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());

	fnl_shutdown(app);
	return rc < 0;
}
