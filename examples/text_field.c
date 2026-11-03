/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A one-line text field: text entry on while it has focus, the
 * composition drawn at the caret, and copy and paste.
 *
 * Shows: fnl_set_text_input fnl_is_text_input fnl_compose_text
 *        fnl_set_text_rect_pt fnl_clipboard_set fnl_clipboard_request
 *        fnl_clipboard_text fnl_text_advance fnl_set_cursor
 *        fnl_content_rect_pt
 */

#include <fenestal/fenestal.h>

#include <string.h>

typedef struct {
	char   text[256];
	size_t len;
} Field;

static void append(Field *f, const char *utf8, size_t n)
{
	/* Cut to the room left, and back to a character's start: half of one
	 * is no character. */
	if (n > sizeof f->text - 1 - f->len) {
		n = sizeof f->text - 1 - f->len;
		while (n > 0 && ((unsigned char)utf8[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(f->text + f->len, utf8, n);
	f->len += n;
	f->text[f->len] = '\0';
}

/* Characters, not bytes: the body face is monospaced, one cell each. */
static int chars(const char *s)
{
	int n = 0;
	for (; *s; s++)
		n += ((unsigned char)*s & 0xc0) != 0x80;
	return n;
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Field *f = ud;
	(void)frame;

	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	const int fx = cx + 20, fy = cy + 20;

	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		switch (ev.type) {
		case FNL_EVENT_TEXT:            /* the layout has had its say */
			append(f, ev.text.utf8, strlen(ev.text.utf8));
			break;
		case FNL_EVENT_KEY:
			if (!ev.key.pressed)
				break;
			if (ev.key.key == FNL_KEY_BACKSPACE && f->len) {
				/* Back over one UTF-8 character, not one byte. */
				do
					f->len--;
				while (f->len && ((unsigned char)f->text[f->len] & 0xc0) ==
									 0x80);
				f->text[f->len] = '\0';
			}
			/* Shortcuts are keys: with Ctrl down nothing is typed. */
			if (fnl_key_down(app, FNL_KEY_LCTRL) && !ev.key.repeat) {
				if (ev.key.key == FNL_KEY_C)
					fnl_clipboard_set(app, f->text);
				if (ev.key.key == FNL_KEY_V)
					fnl_clipboard_request(app);  /* answered by an event */
			}
			break;
		case FNL_EVENT_CLIPBOARD: {
			/* The length is in the event; the text is fetched. */
			char buf[256];
			const size_t n = fnl_clipboard_text(app, buf, sizeof buf);
			append(f, buf, n < sizeof buf ? n : strlen(buf));
			break;
		}
		default:
			break;
		}
		fnl_invalidate(app);
	}

	/* Text entry is a mode: on while this field is the one being typed in. */
	if (!fnl_is_text_input(app)) {
		fnl_set_text_input(app, true);
		fnl_set_cursor(app, FNL_CURSOR_TEXT);
	}

	const FnlColor box  = { 0.15f, 0.15f, 0.18f, 1.0f };
	const FnlColor ink  = { 0.95f, 0.95f, 0.95f, 1.0f };
	const FnlColor pend = { 0.60f, 0.75f, 1.00f, 1.0f };
	const int line = fnl_text_line(FNL_TEXT_BODY);
	fnl_overlay_rect(app, fx - 6, fy - 4, cw - 40, line + 8, box);
	fnl_overlay_text(app, fx, fy, FNL_TEXT_BODY, ink, f->text);

	/* The composition, provisional, at the caret -- and the caret's place
	 * told to the input method, so its candidates open beside it. */
	const int caret = fx + fnl_text_advance(FNL_TEXT_BODY) * chars(f->text);
	char comp[64];
	if (fnl_compose_text(app, comp, sizeof comp) > 0)
		fnl_overlay_text(app, caret, fy, FNL_TEXT_BODY, pend, comp);
	fnl_set_text_rect_pt(app, caret, fy, 1, line);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "text field";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	Field f = { "", 0 };
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &f;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}
