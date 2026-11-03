/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* How late three kinds of timed wait come back on this Windows. */
#include <windows.h>
#include <stdio.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
static double now(void)
{
	LARGE_INTEGER f, n;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&n);
	return (double)n.QuadPart / (double)f.QuadPart;
}
int main(void)
{
	SRWLOCK lock = SRWLOCK_INIT;
	CONDITION_VARIABLE cv;
	InitializeConditionVariable(&cv);
	HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE hr = CreateWaitableTimerExW(NULL, NULL,
					CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	HANDLE lo = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	printf("high-resolution timer: %s\n", hr ? "created" : "refused");
	const double asks[] = { 0.001, 0.004, 0.011, 0.0125 };
	for (int k = 0; k < 4; k++) {
		const double ask = asks[k];
		for (int kind = 0; kind < 3; kind++) {
			HANDLE t = kind == 1 ? hr : lo;
			if (kind && !t) continue;
			double sum = 0, mx = 0, mn = 1;
			const int n = 150;
			for (int i = 0; i < n; i++) {
				/* off the tick's phase, as a frame loop is */
				volatile double spin = now() + 0.0007 * (i % 13);
				while (now() < spin) ;
				double t0 = now();
				if (kind == 0) {
					AcquireSRWLockExclusive(&lock);
					SleepConditionVariableSRW(&cv, &lock,
											  (DWORD)(ask * 1e3 + 0.5), 0);
					ReleaseSRWLockExclusive(&lock);
				} else {
					HANDLE h[2] = { ev, t };
					LARGE_INTEGER due;
					due.QuadPart = -(LONGLONG)(ask * 1e7);
					SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
					WaitForMultipleObjects(2, h, FALSE, INFINITE);
				}
				double d = now() - t0;
				sum += d; if (d > mx) mx = d; if (d < mn) mn = d;
			}
			printf("ask %5.1f ms  %-22s mean %6.2f  min %6.2f  max %6.2f\n",
				   ask * 1e3,
				   kind == 0 ? "condition variable" :
				   kind == 1 ? "event + hires timer" : "event + plain timer",
				   sum / n * 1e3, mn * 1e3, mx * 1e3);
		}
	}
	return 0;
}
