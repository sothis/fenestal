/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* wine's server losing a wheel step it merges into a WM_MOUSEWHEEL the
 * window's thread already holds, with nothing of fenestal in it:
 * docs/wine-wheel-merge.md's reproducer. A maximized window that prints
 * every WM_MOUSEWHEEL with the running total; wheelpairs.py beside it
 * sends the steps, in pairs 1 ms apart, from a uinput mouse.
 *
 * The server hands the thread the first step's message and keeps it until
 * accept_hardware_message; a second step queued meanwhile is added to that
 * message, which the accept then frees. A thread-local WH_MOUSE hook runs
 * inside that gap, so one that sleeps holds it open and the loss is every
 * pair's and not one in forty.
 *
 * wine 11.20 on weston 15.0.1, winewayland.drv, the thinkpad (Core i3
 * M 370), wineserver built from the 11.20 tarball as released and with
 * the draft's patch, 100 pairs each: with a hook of 1 or 5 ms 100 messages
 * and 100 steps lost as released, 200 messages and none lost patched;
 * with no hook 400 messages of 400 from both.
 *
 *   wheelcount [hook_ms] [seconds]   hook_ms 0 (no hook) by default; the
 *                                    window closes after seconds (120)
 *
 * Built by hand, run under wine with DISPLAY unset:
 *
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mconsole \
 *       -o wheelcount.exe tools/probe/wheelcount.c
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int total, messages, hook_ms;

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
	if (code >= 0 && wp == WM_MOUSEWHEEL)
		Sleep(hook_ms);
	return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_MOUSEWHEEL:
		total += GET_WHEEL_DELTA_WPARAM(wp);
		messages++;
		printf("wheel %d total %d messages %d\n",
			   GET_WHEEL_DELTA_WPARAM(wp), total, messages);
		fflush(stdout);
		return 0;
	case WM_TIMER:
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char **argv)
{
	WNDCLASSW wc = {0};
	MSG m;
	HWND hwnd;
	int seconds;

	hook_ms = argc > 1 ? atoi(argv[1]) : 0;
	seconds = argc > 2 ? atoi(argv[2]) : 120;

	wc.lpfnWndProc   = wndproc;
	wc.hInstance     = GetModuleHandleW(NULL);
	wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"wheelcount";
	RegisterClassW(&wc);
	hwnd = CreateWindowW(L"wheelcount", L"wheelcount", WS_OVERLAPPEDWINDOW,
						 CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
						 NULL, NULL, wc.hInstance, NULL);
	ShowWindow(hwnd, SW_MAXIMIZE);
	UpdateWindow(hwnd);
	if (hook_ms > 0)
		SetWindowsHookExW(WH_MOUSE, mouse_hook, NULL, GetCurrentThreadId());
	SetTimer(hwnd, 1, seconds * 1000, NULL);
	printf("ready hook_ms %d\n", hook_ms);
	fflush(stdout);

	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	printf("done total %d messages %d\n", total, messages);
	fflush(stdout);
	return 0;
}
