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
 * So far: the probe, which says whether a session can be made. The calls
 * that carry input and frames follow.
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
	if (!dbus_call(b, "org.freedesktop.DBus", "/org/freedesktop/DBus",
				   "org.freedesktop.DBus", "Hello", "")) {
		printf("the bus answered no Hello\n");
		dbus_close(b);
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

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--probe"))
		return probe();
	fprintf(stderr, "portal: --probe is the only mode so far\n");
	return 2;
}
