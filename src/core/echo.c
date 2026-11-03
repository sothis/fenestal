/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The event echo: every FnlEvent handed to the application, as a line on
 * stderr, for a harness that drives the window from outside.
 * tools/drive/README.md *Driving a window* has what it is for.
 *
 * It runs inside fnl_poll_event(), on the event that call is returning, and
 * every figure after the bar comes from the public getters: a *caller's*
 * view. An event the application never polls is never echoed, which is the
 * property a drain check rests on.
 *
 * Straight to stderr and not through the log sink, which is the caller's.
 * Fields are space-separated and the prefix is "EV "; flushed per line,
 * because anything reading it is reading it live.
 */

#include "core/app.h"

#include <stdarg.h>

/* Set and not "0". Read once at init: a variable changed under a running
 * program is not something anybody means. */
bool echo_wanted_by_env(void)
{
	const char *v = getenv("FNL_ECHO_EVENTS");
	return v && v[0] && !(v[0] == '0' && !v[1]);
}

/* One name per key, by position: "Q" is the key right of Tab. Every FnlKey
 * has a row, so a harness can tell any of them apart; a new key without one
 * is "unknown" on the line, which is how the omission is noticed. */
static const char *const KEY_NAMES[] = {
	[FNL_KEY_W]               = "W",
	[FNL_KEY_A]               = "A",
	[FNL_KEY_S]               = "S",
	[FNL_KEY_D]               = "D",
	[FNL_KEY_UP]              = "Up",
	[FNL_KEY_DOWN]            = "Down",
	[FNL_KEY_LEFT]            = "Left",
	[FNL_KEY_RIGHT]           = "Right",
	[FNL_KEY_LSHIFT]          = "LShift",
	[FNL_KEY_RSHIFT]          = "RShift",
	[FNL_KEY_ESCAPE]          = "Escape",
	[FNL_KEY_F1]              = "F1",
	[FNL_KEY_F2]              = "F2",
	[FNL_KEY_F9]              = "F9",
	[FNL_KEY_F10]             = "F10",
	[FNL_KEY_F11]             = "F11",
	[FNL_KEY_B]               = "B",
	[FNL_KEY_C]               = "C",
	[FNL_KEY_E]               = "E",
	[FNL_KEY_F]               = "F",
	[FNL_KEY_G]               = "G",
	[FNL_KEY_H]               = "H",
	[FNL_KEY_I]               = "I",
	[FNL_KEY_J]               = "J",
	[FNL_KEY_K]               = "K",
	[FNL_KEY_L]               = "L",
	[FNL_KEY_M]               = "M",
	[FNL_KEY_N]               = "N",
	[FNL_KEY_O]               = "O",
	[FNL_KEY_P]               = "P",
	[FNL_KEY_Q]               = "Q",
	[FNL_KEY_R]               = "R",
	[FNL_KEY_T]               = "T",
	[FNL_KEY_U]               = "U",
	[FNL_KEY_V]               = "V",
	[FNL_KEY_X]               = "X",
	[FNL_KEY_Y]               = "Y",
	[FNL_KEY_Z]               = "Z",
	[FNL_KEY_0]               = "0",
	[FNL_KEY_1]               = "1",
	[FNL_KEY_2]               = "2",
	[FNL_KEY_3]               = "3",
	[FNL_KEY_4]               = "4",
	[FNL_KEY_5]               = "5",
	[FNL_KEY_6]               = "6",
	[FNL_KEY_7]               = "7",
	[FNL_KEY_8]               = "8",
	[FNL_KEY_9]               = "9",
	[FNL_KEY_F3]              = "F3",
	[FNL_KEY_F4]              = "F4",
	[FNL_KEY_F5]              = "F5",
	[FNL_KEY_F6]              = "F6",
	[FNL_KEY_F7]              = "F7",
	[FNL_KEY_F8]              = "F8",
	[FNL_KEY_F12]             = "F12",
	[FNL_KEY_SPACE]           = "Space",
	[FNL_KEY_ENTER]           = "Enter",
	[FNL_KEY_TAB]             = "Tab",
	[FNL_KEY_BACKSPACE]       = "Backspace",
	[FNL_KEY_MINUS]           = "Minus",
	[FNL_KEY_EQUAL]           = "Equal",
	[FNL_KEY_LBRACKET]        = "LBracket",
	[FNL_KEY_RBRACKET]        = "RBracket",
	[FNL_KEY_BACKSLASH]       = "Backslash",
	[FNL_KEY_SEMICOLON]       = "Semicolon",
	[FNL_KEY_APOSTROPHE]      = "Apostrophe",
	[FNL_KEY_GRAVE]           = "Grave",
	[FNL_KEY_COMMA]           = "Comma",
	[FNL_KEY_PERIOD]          = "Period",
	[FNL_KEY_SLASH]           = "Slash",
	[FNL_KEY_INTL_BACKSLASH]  = "IntlBackslash",
	[FNL_KEY_LCTRL]           = "LCtrl",
	[FNL_KEY_RCTRL]           = "RCtrl",
	[FNL_KEY_LALT]            = "LAlt",
	[FNL_KEY_RALT]            = "RAlt",
	[FNL_KEY_LSUPER]          = "LSuper",
	[FNL_KEY_RSUPER]          = "RSuper",
	[FNL_KEY_MENU]            = "Menu",
	[FNL_KEY_INSERT]          = "Insert",
	[FNL_KEY_DELETE]          = "Delete",
	[FNL_KEY_HOME]            = "Home",
	[FNL_KEY_END]             = "End",
	[FNL_KEY_PAGE_UP]         = "PageUp",
	[FNL_KEY_PAGE_DOWN]       = "PageDown",
	[FNL_KEY_CAPS_LOCK]       = "CapsLock",
	[FNL_KEY_NUM_LOCK]        = "NumLock",
	[FNL_KEY_SCROLL_LOCK]     = "ScrollLock",
	[FNL_KEY_PRINT_SCREEN]    = "PrintScreen",
	[FNL_KEY_PAUSE]           = "Pause",
	[FNL_KEY_KP_0]            = "KP0",
	[FNL_KEY_KP_1]            = "KP1",
	[FNL_KEY_KP_2]            = "KP2",
	[FNL_KEY_KP_3]            = "KP3",
	[FNL_KEY_KP_4]            = "KP4",
	[FNL_KEY_KP_5]            = "KP5",
	[FNL_KEY_KP_6]            = "KP6",
	[FNL_KEY_KP_7]            = "KP7",
	[FNL_KEY_KP_8]            = "KP8",
	[FNL_KEY_KP_9]            = "KP9",
	[FNL_KEY_KP_DECIMAL]      = "KPDecimal",
	[FNL_KEY_KP_DIVIDE]       = "KPDivide",
	[FNL_KEY_KP_MULTIPLY]     = "KPMultiply",
	[FNL_KEY_KP_SUBTRACT]     = "KPSubtract",
	[FNL_KEY_KP_ADD]          = "KPAdd",
	[FNL_KEY_KP_ENTER]        = "KPEnter",
};

bool echo_key_known(FnlKey k)
{
	const size_t n = sizeof KEY_NAMES / sizeof KEY_NAMES[0];
	return (size_t)k < n && KEY_NAMES[k];
}

static const char *key_name(FnlKey k)
{
	return echo_key_known(k) ? KEY_NAMES[k] : "unknown";
}

static const char *button_name(FnlButton b)
{
	switch (b) {
	case FNL_BUTTON_LEFT:   return "left";
	case FNL_BUTTON_RIGHT:  return "right";
	case FNL_BUTTON_MIDDLE: return "middle";
	default:                return "other";
	}
}

/* The window's state is appended to every line: a harness that has just
 * pressed F11 would otherwise be asking at a different moment. */
static void echo(const App *app, const char *fmt, ...) FNL_PRINTF(2, 3);
static void echo(const App *app, const char *fmt, ...)
{
	va_list ap;
	int cw = 0, ch = 0, ow = 0, oh = 0;
	const char *out;
	fnl_content_size_px(app, &cw, &ch);
	fnl_output_size_pt(app, &ow, &oh);
	/* The output the window is on, which is the only way anything outside the
	 * process can tell that a window has crossed to another monitor. Name and
	 * size both: the name says *which* monitor, the size is what a size picker
	 * filters against and what maximizing has to land on. */
	out = fnl_output_name(app);

	/* Built whole and written once: stderr is unbuffered, and a line from the
	 * render thread's log must not land inside one the reader parses by
	 * shape. */
	char   line[640];
	size_t n = 3;
	memcpy(line, "EV ", 3);
	va_start(ap, fmt);
	int w = vsnprintf(line + n, sizeof line - n, fmt, ap);
	va_end(ap);
	if (w > 0)
		n += (size_t)w < sizeof line - n ? (size_t)w : sizeof line - n - 1;
	w = snprintf(line + n, sizeof line - n,
				 " | area %dx%d fs %d max %d cap %d scale %d out %s %dx%d",
				 cw, ch, (int)fnl_is_fullscreen(app),
				 (int)fnl_is_maximized(app), (int)fnl_is_pointer_captured(app),
				 fnl_scale120(app), out && *out ? out : "?", ow, oh);
	if (w > 0)
		n += (size_t)w < sizeof line - n ? (size_t)w : sizeof line - n - 1;
	/* n is at most sizeof line - 1 here, so the newline always has room. */
	line[n++] = '\n';
	fwrite(line, 1, n, stderr);
	fflush(stderr);
}

/* Somebody else's text, on a line a reader parses by shape: a newline in a
 * paste would end the line early and begin another that says anything. */
static void one_line(char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7f)
			*s = ' ';
}

void echo_event(const App *app, const FnlEvent *ev)
{
	switch (ev->type) {
	case FNL_EVENT_KEY:
		/* A repeat is its own word rather than a press with a flag after it,
		 * so a harness counting presses cannot count one by not looking. */
		echo(app, "key %s %s", key_name(ev->key.key),
			 !ev->key.pressed ? "release"
			 : ev->key.repeat ? "repeat" : "press");
		break;
	case FNL_EVENT_BUTTON:
		echo(app, "button %s %s %.1f %.1f", button_name(ev->button.button),
			 ev->button.pressed ? "press" : "release",
			 ev->button.x, ev->button.y);
		break;
	case FNL_EVENT_MOTION:
		echo(app, "motion %.1f %.1f", ev->motion.x, ev->motion.y);
		break;
	case FNL_EVENT_RELATIVE:
		echo(app, "relative %.1f %.1f", ev->relative.dx, ev->relative.dy);
		break;
	case FNL_EVENT_SCROLL:
		echo(app, "scroll %.2f %.2f at %.1f %.1f", ev->scroll.dx,
			 ev->scroll.dy, ev->scroll.x, ev->scroll.y);
		break;
	case FNL_EVENT_FOCUS:
		echo(app, "focus %d", (int)ev->focus.focused);
		break;
	case FNL_EVENT_CLOSE:
		echo(app, "close");
		break;

	/* The notifications. Echoed on the same terms as the input above -- a
	 * harness driving this program cannot see the titlebar's own controls act,
	 * so what they changed has to arrive as a line it can read. */
	case FNL_EVENT_RESIZE:
		echo(app, "resize %d %d pt %d %d px", ev->resize.w_pt, ev->resize.h_pt,
			 ev->resize.w_px, ev->resize.h_px);
		break;
	case FNL_EVENT_SCALE:
		echo(app, "scale %d", ev->scale.scale120);
		break;
	case FNL_EVENT_WINDOW:
		echo(app, "window fs %d max %d", (int)ev->window.fullscreen,
			 (int)ev->window.maximized);
		break;
	case FNL_EVENT_ANTIALIAS:
		echo(app, "antialias %s", ev->antialias.aa == FNL_AA_MSAA ? "fsaa"
								: ev->antialias.aa == FNL_AA_MLAA ? "mlaa"
																  : "none");
		break;
	case FNL_EVENT_SCENE_SIZE:
		echo(app, "scene %d %d", ev->scene.w_px, ev->scene.h_px);
		break;
	case FNL_EVENT_CAPTURE:
		echo(app, "capture %d", (int)ev->capture.captured);
		break;
	case FNL_EVENT_LOST:
		echo(app, "lost %u", ev->lost.count);
		break;
	case FNL_EVENT_COMPOSE: {
		char buf[128];
		fnl_compose_text(app, buf, sizeof buf);
		one_line(buf);
		echo(app, "compose %u %s", ev->compose.length, buf);
		break;
	}
	case FNL_EVENT_CLIPBOARD: {
		/* The head of it. A paste can be a megabyte and this is one line. */
		char buf[128];
		fnl_clipboard_text(app, buf, sizeof buf);
		one_line(buf);
		echo(app, "clipboard %u %s", ev->clipboard.length, buf);
		break;
	}
	case FNL_EVENT_OUTPUT: {
		char name[OUTPUT_NAME_MAX];
		snprintf(name, sizeof name, "%s", fnl_output_name(app));
		one_line(name);
		echo(app, "output %d %d pt %s", ev->output.w_pt, ev->output.h_pt,
			 name);
		break;
	}
	case FNL_EVENT_GPU: {
		/* The index, the renderer's key and the name: what a harness moving
		 * the window between GPUs checks it landed on. */
		char name[GPU_NAME_MAX];
		snprintf(name, sizeof name, "%s", fnl_gpu_name(app));
		one_line(name);
		echo(app, "gpu %d %s %s", ev->gpu.index,
			 app_renderer_key(ev->gpu.renderer), name);
		break;
	}
	case FNL_EVENT_TEXT:
		/* The number as well as the character: a harness comparing against
		 * what it typed should not depend on its terminal's encoding, and a
		 * space is otherwise a field that is not there. */
		echo(app, "text U+%04X %s", (unsigned)ev->text.codepoint,
			 ev->text.utf8);
		break;

	default:
		/* A type added without a case here. Printed rather than dropped,
		 * because a harness noticing an unfamiliar number is how the next
		 * one gets a line of its own. */
		echo(app, "type%d", (int)ev->type);
		break;
	}
}
