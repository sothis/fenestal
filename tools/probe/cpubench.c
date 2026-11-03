/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Raw CPU throughput, one thread and four: scalar float and AVX-width float
 * work of the kind a software rasteriser does, nothing else. */
#include <windows.h>
#include <stdio.h>
#include <math.h>
static double now(void)
{
	LARGE_INTEGER f, n;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&n);
	return (double)n.QuadPart / (double)f.QuadPart;
}
#define N 4096
static DWORD WINAPI work(LPVOID p)
{
	float *buf = (float *)p;
	volatile float sink = 0;
	for (int rep = 0; rep < 60 * 256; rep++) {
		float acc = 0;
		for (int i = 0; i < N; i++) {
			float x = buf[i] * 0.999f + 0.001f * (float)rep;
			buf[i] = x * x * 0.5f + x * 0.25f + 0.125f;
			acc += buf[i];
		}
		sink += acc;
	}
	return 0;
}
static double run(int threads)
{
	HANDLE h[16];
	float *bufs[16];
	for (int t = 0; t < threads; t++) {
		bufs[t] = (float *)VirtualAlloc(NULL, N * sizeof(float),
										MEM_COMMIT | MEM_RESERVE,
										PAGE_READWRITE);
		for (int i = 0; i < N; i++) bufs[t][i] = (float)i / N;
	}
	double t0 = now();
	for (int t = 0; t < threads; t++)
		h[t] = CreateThread(NULL, 0, work, bufs[t], 0, NULL);
	WaitForMultipleObjects((DWORD)threads, h, TRUE, INFINITE);
	double dt = now() - t0;
	for (int t = 0; t < threads; t++) {
		CloseHandle(h[t]);
		VirtualFree(bufs[t], 0, MEM_RELEASE);
	}
	return dt;
}
int main(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	printf("logical processors: %lu\n", si.dwNumberOfProcessors);
	for (int pass = 0; pass < 2; pass++)
		for (int t = 1; t <= 8; t *= 2) {
			double dt = run(t);
			printf("%d thread%s: %7.1f ms  (%.0f Melem/s total)\n", t,
				   t == 1 ? " " : "s", dt * 1e3, 60.0 * N * t / dt / 1e6);
		}
	return 0;
}
