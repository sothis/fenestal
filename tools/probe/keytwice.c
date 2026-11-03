/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Whether wine hands a window each key once, with nothing of fenestal in
 * it: a window that prints every keyboard message it is sent, with the
 * fields that tell a second copy from a repeat, and its focus changes,
 * which keytwice.py beside it waits on before it types. Under wine 11.21
 * the testbed's echo had every press and release twice, on the X11 and the
 * Wayland driver alike, and under 11.0 once.
 *
 *   keytwice [raw] [ime] [seconds]
 *
 *   raw      also registers for raw keyboard input and prints each
 *            WM_INPUT key, which says whether the doubling is before the
 *            window's queue or in it
 *   ime      takes the composition as the library does: the system's
 *            composition window hidden at WM_IME_SETCONTEXT and
 *            WM_IME_STARTCOMPOSITION not passed on (win32.c)
 *   seconds  until the window closes (60)
 *
 * Each line: the message, the virtual key, the scan code with E0's bit,
 * the repeat count, the previous-state bit (a repeat has it, a second
 * press does not), GetMessageTime() and a running count. Built by hand:
 *
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mconsole \
 *       -o keytwice.exe tools/probe/keytwice.c
 */
#include <windows.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int downs, ups, chars, raws, ime;

static void say(const char *what, WPARAM wp, LPARAM lp)
{
	printf("%s vk 0x%02x scan 0x%03x repeat %u prev %u time %lu "
		   "downs %d ups %d\n", what, (unsigned)wp,
		   (unsigned)((lp >> 16) & 0x1ff), (unsigned)(lp & 0xffff),
		   (unsigned)((lp >> 30) & 1), (unsigned long)GetMessageTime(),
		   downs, ups);
	fflush(stdout);
}

static void raw_key(LPARAM lp)
{
	RAWINPUT ri;
	UINT size = sizeof ri;
	if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
						sizeof(RAWINPUTHEADER)) == (UINT)-1 ||
		ri.header.dwType != RIM_TYPEKEYBOARD)
		return;
	raws++;
	printf("raw vk 0x%02x make 0x%02x flags 0x%x message 0x%04x raws %d\n",
		   ri.data.keyboard.VKey, ri.data.keyboard.MakeCode,
		   ri.data.keyboard.Flags, ri.data.keyboard.Message, raws);
	fflush(stdout);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		downs++;
		say(msg == WM_KEYDOWN ? "down" : "sysdown", wp, lp);
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		ups++;
		say(msg == WM_KEYUP ? "up" : "sysup", wp, lp);
		break;
	case WM_CHAR:
		chars++;
		printf("char U+%04X chars %d\n", (unsigned)wp, chars);
		fflush(stdout);
		return 0;
	case WM_INPUT:
		raw_key(lp);
		break;
	case WM_IME_SETCONTEXT:
		if (ime)
			lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
		break;
	case WM_IME_STARTCOMPOSITION:
		if (ime)
			return 0;
		break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		printf("focus %d\n", msg == WM_SETFOCUS);
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
	int raw = 0, seconds = 60;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "raw"))
			raw = 1;
		else if (!strcmp(argv[i], "ime"))
			ime = 1;
		else
			seconds = atoi(argv[i]);
	}
	HINSTANCE inst = GetModuleHandleW(NULL);
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"keytwice";
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowW(L"keytwice", L"keytwice",
							  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
							  CW_USEDEFAULT, CW_USEDEFAULT, 480, 320,
							  NULL, NULL, inst, NULL);
	if (!hwnd) {
		printf("no window: %lu\n", GetLastError());
		return 1;
	}
	if (raw) {
		RAWINPUTDEVICE rid = { .usUsagePage = 0x01, .usUsage = 0x06,
							   .hwndTarget = hwnd };
		if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
			printf("no raw keyboard: %lu\n", GetLastError());
	}
	SetForegroundWindow(hwnd);
	SetTimer(hwnd, 1, (UINT)seconds * 1000, NULL);
	printf("ready\n");
	fflush(stdout);
	MSG m;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	printf("done downs %d ups %d chars %d raws %d\n", downs, ups, chars,
		   raws);
	return 0;
}
