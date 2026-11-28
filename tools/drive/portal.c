/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * portal: the compositor's own account of a window, for the checks the
 * library cannot make from inside.
 *
 *   portal --probe            what this session's portals offer
 *
 * What a round cannot ask through the MCP port is what the *window server*
 * did with the window: where it put it, what it drew, and input that went
 * through the compositor rather than into the program. GNOME answers both
 * through D-Bus: org.gnome.Mutter.RemoteDesktop for input and
 * org.gnome.Mutter.ScreenCast for pictures. This is the client for them, on
 * the bus client beside it (dbus.c).
 *
 * So far: the probe, which says whether a session can be made, and the two
 * notify calls against Mutter's own interface, which it refuses -- a session
 * of `org.gnome.Mutter.RemoteDesktop` takes its input from the xdg portal's
 * `org.freedesktop.portal.RemoteDesktop` instead, whose calls carry an
 * `a{sv}` of options and answer through a `org.freedesktop.portal.Request`
 * object's Response signal rather than a reply (tools/drive/wayland_driver.py
 * is the working account of it). That is the next step here, and with it the
 * ScreenCast call that brings the composited frame back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbus.h"

#define MUTTER_RD "org.gnome.Mutter.RemoteDesktop"
#define MUTTER_RD_PATH "/org/gnome/Mutter/RemoteDesktop"

static int probe(void)
{
	Dbus *b = dbus_open();
	if (!b) {
		printf("no session bus (DBUS_SESSION_BUS_ADDRESS)\n");
		return 1;
	}
	const bool rd = dbus_call(b, MUTTER_RD, MUTTER_RD_PATH, MUTTER_RD,
							  "CreateSession", "");
	char path[256] = "";
	if (rd && dbus_reply_string(b, path, sizeof path))
		printf("RemoteDesktop: session %s\n", path);
	else
		printf("RemoteDesktop: %s\n",
			   rd ? "a reply with no session path" : "no session");
	dbus_close(b);
	return rd ? 0 : 1;
}

#define MUTTER_RD_SESSION "org.gnome.Mutter.RemoteDesktop.Session"

/* A number from a word, or `dflt` where it is not one: the conversion says
 * what it did instead of reading a mistake as nought. */
static double number(const char *word, double dflt)
{
	char        *end = NULL;
	const double v   = strtod(word, &end);
	if (!word || *word == '\0' || !end || *end != '\0')
		return dflt;
	return v;
}

/* A session made and started: its object path in `path`. False where the
 * compositor would not make one. */
static bool session(Dbus *b, char *path, size_t n)
{
	if (!dbus_call(b, MUTTER_RD, MUTTER_RD_PATH, MUTTER_RD, "CreateSession",
				   "") ||
		!dbus_reply_string(b, path, n))
		return false;
	/* Started before anything is notified: a session that has not started
	 * has nothing to notify. */
	return dbus_call(b, MUTTER_RD, path, MUTTER_RD_SESSION, "Start", "");
}

/* A relative motion, as a device reports one: the two deltas, in pixels. */
static int move(double dx, double dy)
{
	Dbus *b = dbus_open();
	if (!b)
		return 1;
	char path[256];
	if (!session(b, path, sizeof path)) {
		fprintf(stderr, "portal: no session to move the pointer in\n");
		dbus_close(b);
		return 1;
	}
	DBuf body = { 0 };
	dbuf_double(&body, dx);
	dbuf_double(&body, dy);
	const bool ok = dbus_call_with(b, MUTTER_RD, path, MUTTER_RD_SESSION,
								   "NotifyPointerMotion", "dd", &body);
	dbuf_free(&body);
	dbus_close(b);
	printf("pointer: %s%g %g\n", ok ? "moved " : "refused ", dx, dy);
	return ok ? 0 : 1;
}

/* A key by its evdev code, pressed or released. */
static int key(int code, int down)
{
	Dbus *b = dbus_open();
	if (!b)
		return 1;
	char path[256];
	if (!session(b, path, sizeof path)) {
		fprintf(stderr, "portal: no session to press a key in\n");
		dbus_close(b);
		return 1;
	}
	DBuf body = { 0 };
	dbuf_i32(&body, code);
	dbuf_u32_arg(&body, (uint32_t)(down ? 1 : 0));
	const bool ok = dbus_call_with(b, MUTTER_RD, path, MUTTER_RD_SESSION,
								   "NotifyKeyboardKeycode", "iu", &body);
	dbuf_free(&body);
	dbus_close(b);
	printf("key %d: %s\n", code, ok ? "sent" : "refused");
	return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--probe"))
		return probe();
	if (argc > 3 && !strcmp(argv[1], "--move"))
		return move(number(argv[2], 0.0), number(argv[3], 0.0));
	if (argc > 3 && !strcmp(argv[1], "--key"))
		return key((int)number(argv[2], -1.0), (int)number(argv[3], -1.0));
	fprintf(stderr, "portal: --probe, --move <dx> <dy>, --key <evdev> <0|1>\n");
	return 2;
}
