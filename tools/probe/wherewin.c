/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Where Windows says a Fenestal window and the pointer are, a line whenever
 * either answer changes: wherewin [<seconds>]. Run beside the window, in
 * the same session -- under wine, the same prefix and the same driver. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HWND found;

static BOOL CALLBACK each_window(HWND h, LPARAM unused)
{
	char title[128];
	(void)unused;
	if (!IsWindowVisible(h) || !GetWindowTextA(h, title, sizeof title))
		return TRUE;
	if (!strstr(title, "Fenestal"))
		return TRUE;
	found = h;
	return FALSE;
}

static BOOL CALLBACK each_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM unused)
{
	MONITORINFOEXA mi;
	(void)dc; (void)r; (void)unused;
	mi.cbSize = sizeof mi;
	if (GetMonitorInfoA(m, (MONITORINFO *)&mi))
		printf("monitor %s%s %ld,%ld %ldx%ld work %ld,%ld %ldx%ld\n",
			   mi.szDevice, mi.dwFlags & MONITORINFOF_PRIMARY ? "*" : "",
			   mi.rcMonitor.left, mi.rcMonitor.top,
			   mi.rcMonitor.right - mi.rcMonitor.left,
			   mi.rcMonitor.bottom - mi.rcMonitor.top,
			   mi.rcWork.left, mi.rcWork.top,
			   mi.rcWork.right - mi.rcWork.left,
			   mi.rcWork.bottom - mi.rcWork.top);
	return TRUE;
}

int main(int argc, char **argv)
{
	const double seconds = argc > 1 ? atof(argv[1]) : 20.0;
	char was[256] = "";

	setvbuf(stdout, NULL, _IONBF, 0);
	/* Unaware, the answers are divided by the scale and so are not the
	 * ones the window under test is given. */
	SetProcessDPIAware();
	printf("virtual %d,%d %dx%d\n", GetSystemMetrics(SM_XVIRTUALSCREEN),
		   GetSystemMetrics(SM_YVIRTUALSCREEN),
		   GetSystemMetrics(SM_CXVIRTUALSCREEN),
		   GetSystemMetrics(SM_CYVIRTUALSCREEN));
	EnumDisplayMonitors(NULL, NULL, each_monitor, 0);

	const DWORD end = GetTickCount() + (DWORD)(seconds * 1e3);
	while ((LONG)(end - GetTickCount()) > 0) {
		char now[256];
		RECT w = { 0, 0, 0, 0 }, c = { 0, 0, 0, 0 }, clip = { 0, 0, 0, 0 };
		POINT o = { 0, 0 }, p = { -1, -1 };
		MONITORINFOEXA mi;

		if (!IsWindow(found)) {
			found = NULL;
			EnumWindows(each_window, 0);
		}
		memset(&mi, 0, sizeof mi);
		mi.cbSize = sizeof mi;
		if (found) {
			GetWindowRect(found, &w);
			GetClientRect(found, &c);
			ClientToScreen(found, &o);
			GetMonitorInfoA(MonitorFromWindow(found, MONITOR_DEFAULTTONEAREST),
							(MONITORINFO *)&mi);
		}
		const BOOL have = GetCursorPos(&p);
		GetClipCursor(&clip);
		snprintf(now, sizeof now,
				 "window %ld,%ld %ldx%ld client %ld,%ld %ldx%ld%s on %s "
				 "pointer %s%ld,%ld in-client %ld,%ld clip %ld,%ld..%ld,%ld",
				 w.left, w.top, w.right - w.left, w.bottom - w.top,
				 o.x, o.y, c.right, c.bottom,
				 found && IsZoomed(found) ? " zoomed" : "",
				 found ? mi.szDevice : "nothing",
				 have ? "" : "refused ", p.x, p.y, p.x - o.x, p.y - o.y,
				 clip.left, clip.top, clip.right, clip.bottom);
		if (strcmp(now, was)) {
			printf("%7.2f %s\n", (double)(GetTickCount() % 1000000) / 1e3,
				   now);
			strcpy(was, now);
		}
		Sleep(20);
	}
	return 0;
}
