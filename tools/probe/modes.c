/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Which displays a Windows desktop has, on which adapter, at what rate, and
 * the rates each offers at the size it is at: modes. With a display and a
 * rate, `modes \\.\DISPLAY2 50`, that display is put at that rate -- for the
 * session and not in the registry, so a logoff undoes it.
 *
 * Run on the desktop: a service's session, which is what ssh gives, has no
 * displays to enumerate. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every rate the display offers at the size and depth it is at, once each. */
static void rates(const char *device, const DEVMODEA *now)
{
	DWORD seen[64];
	int   n = 0;
	DEVMODEA m;

	m.dmSize = sizeof m;
	m.dmDriverExtra = 0;
	for (DWORD i = 0; EnumDisplaySettingsA(device, i, &m); i++) {
		if (m.dmPelsWidth != now->dmPelsWidth ||
			m.dmPelsHeight != now->dmPelsHeight ||
			m.dmBitsPerPel != now->dmBitsPerPel)
			continue;
		int known = 0;
		for (int k = 0; k < n; k++)
			known |= seen[k] == m.dmDisplayFrequency;
		if (!known && n < 64)
			seen[n++] = m.dmDisplayFrequency;
	}
	printf("  offers");
	for (int k = 0; k < n; k++)
		printf(" %lu", (unsigned long)seen[k]);
	printf(" Hz at %lux%lu\n", (unsigned long)now->dmPelsWidth,
		   (unsigned long)now->dmPelsHeight);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 2) {
		DEVMODEA m;
		m.dmSize = sizeof m;
		m.dmDriverExtra = 0;
		if (!EnumDisplaySettingsA(argv[1], ENUM_CURRENT_SETTINGS, &m)) {
			printf("no display %s\n", argv[1]);
			return 1;
		}
		m.dmDisplayFrequency = (DWORD)atoi(argv[2]);
		m.dmFields = DM_DISPLAYFREQUENCY;
		/* No CDS_UPDATEREGISTRY: the mode is this session's and is not
		 * what the machine comes back to. */
		const LONG rc = ChangeDisplaySettingsExA(argv[1], &m, NULL, 0, NULL);
		printf("%s to %s Hz: %s (%ld)\n", argv[1], argv[2],
			   rc == DISP_CHANGE_SUCCESSFUL ? "done" : "refused", (long)rc);
	}

	DISPLAY_DEVICEA a;
	a.cb = sizeof a;
	for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &a, 0); i++) {
		DISPLAY_DEVICEA mon;
		DEVMODEA        m;

		if (!(a.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
			continue;
		m.dmSize = sizeof m;
		m.dmDriverExtra = 0;
		if (!EnumDisplaySettingsA(a.DeviceName, ENUM_CURRENT_SETTINGS, &m))
			continue;
		mon.cb = sizeof mon;
		if (!EnumDisplayDevicesA(a.DeviceName, 0, &mon, 0))
			strcpy(mon.DeviceString, "?");
		printf("%s%s on %s, %s: %lux%lu at %ld,%ld, %lu Hz\n", a.DeviceName,
			   a.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE ? "*" : "",
			   a.DeviceString, mon.DeviceString,
			   (unsigned long)m.dmPelsWidth, (unsigned long)m.dmPelsHeight,
			   (long)m.dmPosition.x, (long)m.dmPosition.y,
			   (unsigned long)m.dmDisplayFrequency);
		rates(a.DeviceName, &m);
	}
	return 0;
}
