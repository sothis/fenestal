/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* What a clock read and a thread hand-off cost on this machine. */
#include <windows.h>
#include <stdio.h>
#include <intrin.h>

#define CALLS  200000
#define TRIPS  20000
#define ROUNDS 5000
#define WORKERS 4

static double now(void)
{
	LARGE_INTEGER f, n;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&n);
	return (double)n.QuadPart / (double)f.QuadPart;
}

static HANDLE ping, pong;
static volatile LONG stop;

static DWORD WINAPI echo(LPVOID p)
{
	(void)p;
	for (;;) {
		WaitForSingleObject(ping, INFINITE);
		if (stop)
			return 0;
		SetEvent(pong);
	}
}

static CRITICAL_SECTION cs;
static CONDITION_VARIABLE cv_go, cv_done;
static volatile LONG gen, done;

/* The shape of a rasteriser's worker: woken with the rest, reports, sleeps. */
static DWORD WINAPI worker(LPVOID p)
{
	LONG seen = 0;
	(void)p;
	for (;;) {
		EnterCriticalSection(&cs);
		while (gen == seen && !stop)
			SleepConditionVariableCS(&cv_go, &cs, INFINITE);
		seen = gen;
		LeaveCriticalSection(&cs);
		if (stop)
			return 0;
		EnterCriticalSection(&cs);
		done++;
		WakeConditionVariable(&cv_done);
		LeaveCriticalSection(&cs);
	}
}

int main(void)
{
	LARGE_INTEGER f, q;
	QueryPerformanceFrequency(&f);
	printf("QueryPerformanceFrequency: %lld Hz\n", (long long)f.QuadPart);

	double t0 = now();
	for (int i = 0; i < CALLS; i++)
		QueryPerformanceCounter(&q);
	printf("QueryPerformanceCounter: %.0f ns a call\n",
		   (now() - t0) / CALLS * 1e9);

	t0 = now();
	unsigned long long x = 0;
	for (int i = 0; i < CALLS; i++)
		x += __rdtsc();
	printf("rdtsc:                   %.0f ns a call\n",
		   (now() - t0) / CALLS * 1e9);

	t0 = now();
	for (int i = 0; i < CALLS; i++)
		SwitchToThread();
	printf("an empty system call:    %.0f ns\n", (now() - t0) / CALLS * 1e9);

	ping = CreateEventW(NULL, FALSE, FALSE, NULL);
	pong = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE th = CreateThread(NULL, 0, echo, NULL, 0, NULL);
	t0 = now();
	for (int i = 0; i < TRIPS; i++) {
		SetEvent(ping);
		WaitForSingleObject(pong, INFINITE);
	}
	printf("event ping-pong, two threads: %.1f us a round trip\n",
		   (now() - t0) / TRIPS * 1e6);
	stop = 1;
	SetEvent(ping);
	WaitForSingleObject(th, INFINITE);

	stop = 0;
	InitializeCriticalSection(&cs);
	InitializeConditionVariable(&cv_go);
	InitializeConditionVariable(&cv_done);
	HANDLE w[WORKERS];
	for (int i = 0; i < WORKERS; i++)
		w[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	Sleep(50);
	t0 = now();
	for (int i = 0; i < ROUNDS; i++) {
		EnterCriticalSection(&cs);
		done = 0;
		gen++;
		WakeAllConditionVariable(&cv_go);
		while (done < WORKERS)
			SleepConditionVariableCS(&cv_done, &cs, INFINITE);
		LeaveCriticalSection(&cs);
	}
	printf("wake four workers and wait for all: %.1f us a round\n",
		   (now() - t0) / ROUNDS * 1e6);
	EnterCriticalSection(&cs);
	stop = 1;
	WakeAllConditionVariable(&cv_go);
	LeaveCriticalSection(&cs);
	WaitForMultipleObjects(WORKERS, w, TRUE, INFINITE);
	/* x is read so the rdtsc loop is not optimised away. */
	return (int)(x & 0);
}
