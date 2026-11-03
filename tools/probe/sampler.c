/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* A user-mode sampling profiler: every thread of one process, its
 * instruction pointer by module, its CPU time, and for a thread caught
 * outside a chosen module the nearest return address into it.
 *
 * It exists because a guest command is not elevated, so Windows' own
 * profiler will not start there. No symbols: it prints RVAs, to be read
 * against a disassembly of the module.
 *
 *     sampler [image-name [seconds [focus-module]]] */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAXT 64
#define MAXM 256
#define MAXH 4096
#define STACK_WORDS 96

typedef struct {
	DWORD64 base, size;
	char    name[64];
} Mod;

/* bymod[MAXM] counts what is in no module: generated code. */
typedef struct {
	DWORD     id;
	HANDLE    h;
	ULONGLONG u0, k0;
	int       n, bymod[MAXM + 1];
} Thr;

typedef struct {
	DWORD64 key;
	int     n;
} Hot;

static Mod mods[MAXM];
static int nmods;
static Thr thr[MAXT];
static int nthr;
static Hot hot_ip[MAXH], hot_wait[MAXH], hot_nt[MAXH];

static int mod_of(DWORD64 a)
{
	for (int i = 0; i < nmods; i++)
		if (a >= mods[i].base && a < mods[i].base + mods[i].size)
			return i;
	return MAXM;
}

static void bump(Hot *h, DWORD64 key)
{
	unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52) % MAXH;
	for (int p = 0; p < MAXH; p++, i = (i + 1) % MAXH) {
		if (h[i].n == 0 || h[i].key == key) {
			h[i].key = key;
			h[i].n++;
			return;
		}
	}
}

static int cmp(const void *a, const void *b)
{
	return ((const Hot *)b)->n - ((const Hot *)a)->n;
}

static ULONGLONG ft(FILETIME f)
{
	return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static double seconds_since(LARGE_INTEGER t0)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

static DWORD find_process(const char *image)
{
	DWORD pid = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 pe = { .dwSize = sizeof pe };
	for (BOOL ok = Process32First(snap, &pe); ok;
		 ok = Process32Next(snap, &pe))
		if (!_stricmp(pe.szExeFile, image))
			pid = pe.th32ProcessID;
	CloseHandle(snap);
	return pid;
}

/* The focus module's index, or -1. */
static int read_modules(DWORD pid, const char *focus)
{
	int focus_i = -1;
	HANDLE snap = CreateToolhelp32Snapshot(
		TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	MODULEENTRY32 me = { .dwSize = sizeof me };
	for (BOOL ok = Module32First(snap, &me); ok && nmods < MAXM;
		 ok = Module32Next(snap, &me)) {
		mods[nmods].base = (DWORD64)(ULONG_PTR)me.modBaseAddr;
		mods[nmods].size = me.modBaseSize;
		snprintf(mods[nmods].name, sizeof mods[0].name, "%.63s", me.szModule);
		if (!_stricmp(me.szModule, focus))
			focus_i = nmods;
		nmods++;
	}
	CloseHandle(snap);
	return focus_i;
}

static void open_threads(DWORD pid)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te = { .dwSize = sizeof te };
	for (BOOL ok = Thread32First(snap, &te); ok && nthr < MAXT;
		 ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid)
			continue;
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
							  THREAD_QUERY_INFORMATION,
							  FALSE, te.th32ThreadID);
		if (!h)
			continue;
		FILETIME c, e, k, u;
		GetThreadTimes(h, &c, &e, &k, &u);
		thr[nthr] = (Thr){ .id = te.th32ThreadID, .h = h,
						   .u0 = ft(u), .k0 = ft(k) };
		nthr++;
	}
	CloseHandle(snap);
}

static void sample(Thr *t, HANDLE proc, int focus_i)
{
	CONTEXT cx;
	cx.ContextFlags = CONTEXT_CONTROL;
	if (SuspendThread(t->h) == (DWORD)-1)
		return;
	if (GetThreadContext(t->h, &cx)) {
		int m = mod_of(cx.Rip);
		t->n++;
		t->bymod[m]++;
		if (m != MAXM && !_stricmp(mods[m].name, "ntdll.dll"))
			bump(hot_nt, cx.Rip - mods[m].base);
		if (m == focus_i) {
			bump(hot_ip, cx.Rip - mods[m].base);
		} else if (m != MAXM && focus_i >= 0) {
			/* Outside the focus module: who in it is on the stack? The
			 * first word that points into it, which is a guess a frame
			 * walk would not need and symbols would. */
			DWORD64 st[STACK_WORDS];
			SIZE_T  got = 0;
			if (ReadProcessMemory(proc, (void *)cx.Rsp, st, sizeof st, &got))
				for (unsigned s = 0; s < got / 8; s++)
					if (mod_of(st[s]) == focus_i) {
						bump(hot_wait, ((DWORD64)m << 40) |
									   (st[s] - mods[focus_i].base));
						break;
					}
		}
	}
	ResumeThread(t->h);
}

static int total(const Hot *h)
{
	int tot = 0;
	for (int i = 0; i < MAXH; i++)
		tot += h[i].n;
	return tot;
}

static void report_threads(double wall)
{
	for (int i = 0; i < nthr; i++) {
		FILETIME c, e, k, u;
		GetThreadTimes(thr[i].h, &c, &e, &k, &u);
		double um = (double)(ft(u) - thr[i].u0) / 1e4;
		double km = (double)(ft(k) - thr[i].k0) / 1e4;
		if (um + km < wall * 10)        /* under 1% of a core */
			continue;
		printf("thread %5lu  user %5.1f%%  kernel %5.1f%%  samples:",
			   thr[i].id, um / wall / 10, km / wall / 10);
		for (int pass = 0; pass < 5; pass++) {
			int best = -1;
			for (int m = 0; m <= MAXM; m++)
				if (thr[i].bymod[m] > 0 &&
					(best < 0 || thr[i].bymod[m] > thr[i].bymod[best]))
					best = m;
			if (best < 0)
				break;
			printf(" %s %d%%",
				   best == MAXM ? "[jit/anon]" : mods[best].name,
				   thr[i].bymod[best] * 100 / thr[i].n);
			thr[i].bymod[best] = 0;
		}
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	const char *image = argc > 1 ? argv[1] : "fenestal-testbed.exe";
	double secs = argc > 2 ? atof(argv[2]) : 5.0;
	const char *focus = argc > 3 ? argv[3] : "vulkan_lvp.dll";

	DWORD pid = find_process(image);
	if (!pid) {
		printf("no %s\n", image);
		return 1;
	}
	HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
							  FALSE, pid);
	int focus_i = read_modules(pid, focus);
	open_threads(pid);

	/* Above what it samples, or it only runs once they have gone idle. */
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	HANDLE tm = CreateWaitableTimerExW(NULL, NULL, 2, TIMER_ALL_ACCESS);

	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	int rounds = 0;
	while (seconds_since(t0) <= secs) {
		for (int i = 0; i < nthr; i++)
			sample(&thr[i], proc, focus_i);
		rounds++;
		/* Off the scheduler's tick, or every sample lands where a thread
		 * wakes: 0.3 to 1.2 ms on a high-resolution timer. */
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)(3000 + rand() % 9000);
		if (tm && SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE))
			WaitForSingleObject(tm, INFINITE);
		else
			Sleep(1);
	}
	double wall = seconds_since(t0);

	printf("pid %lu, %d threads, %d rounds in %.2f s; focus %s base %llx\n",
		   pid, nthr, rounds, wall, focus,
		   focus_i >= 0 ? (unsigned long long)mods[focus_i].base : 0ull);
	report_threads(wall);

	qsort(hot_ip, MAXH, sizeof(Hot), cmp);
	qsort(hot_wait, MAXH, sizeof(Hot), cmp);
	qsort(hot_nt, MAXH, sizeof(Hot), cmp);
	printf("hot addresses inside %s (%d samples):\n", focus, total(hot_ip));
	for (int i = 0; i < 12 && hot_ip[i].n; i++)
		printf("  rva %08llx  %d\n", (unsigned long long)hot_ip[i].key,
			   hot_ip[i].n);
	printf("addresses inside ntdll.dll (%d samples):\n", total(hot_nt));
	for (int i = 0; i < 14 && hot_nt[i].n; i++)
		printf("  ntrva %08llx  %d\n", (unsigned long long)hot_nt[i].key,
			   hot_nt[i].n);
	printf("outside it, called from it (%d samples): module <- return rva\n",
		   total(hot_wait));
	for (int i = 0; i < 8 && hot_wait[i].n; i++)
		printf("  %-18s <- %08llx  %d\n", mods[hot_wait[i].key >> 40].name,
			   (unsigned long long)(hot_wait[i].key & 0xffffffffffull),
			   hot_wait[i].n);
	return 0;
}
