/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* What one software present costs: a copy into a DIB section and a blit of
 * it to a window, beside the same copy into plain memory. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)f.QuadPart * 1e3;
}

int main(int argc, char **argv)
{
	int w = argc > 1 ? atoi(argv[1]) : 1280, h = argc > 2 ? atoi(argv[2]) : 754;
	size_t n = (size_t)w * h * 4;
	HWND win = CreateWindowExW(0, L"STATIC", L"dibtest",
							   WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50,
							   w + 16, h + 39, NULL, NULL, NULL, NULL);
	MSG m;
	for (int i = 0; i < 50; i++) {
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&m);
		Sleep(10);
	}
	HDC wdc = GetDC(win);
	HDC mdc = CreateCompatibleDC(wdc);
	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
									0, (DWORD)n, NULL);
	void *bits = NULL;
	HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, map, 0);
	SelectObject(mdc, bmp);
	unsigned char *src = malloc(n), *plain = malloc(n);
	if (!bits || !src || !plain) {
		printf("setup failed\n");
		return 1;
	}
	memset(src, 0x40, n);
	memset(plain, 0, n);
	double tp = 0, td = 0, tb = 0, tf = 0;
	int rounds = 100;
	for (int i = 0; i < rounds; i++) {
		memset(src, i, 4096);
		double a = now();
		memcpy(plain, src, n);
		double b = now();
		memcpy(bits, src, n);
		double c = now();
		BitBlt(wdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
		double d = now();
		GdiFlush();
		double e = now();
		tp += b - a; td += c - b; tb += d - c; tf += e - d;
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&m);
	}
	printf("%dx%d, %d rounds, ms each:\n", w, h, rounds);
	printf("copy to plain memory  %.2f\n", tp / rounds);
	printf("copy to DIB section   %.2f\n", td / rounds);
	printf("BitBlt to the window  %.2f\n", tb / rounds);
	printf("GdiFlush              %.2f\n", tf / rounds);
	fflush(stdout);
	return 0;
}
