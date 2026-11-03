/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The conditions a run is measured under that are the machine's and not
 * the window's: background CPU load -- threads of this process, each busy
 * for its duty of every 10 ms slot -- and the CPUs and priority the process
 * and that load may run on.
 *
 * The process's are set on the main thread before fnl_init(), so every
 * thread the library starts after inherits them (the render thread, the
 * lister, the mover; DESIGN *Benchmarking*). The load's are each load
 * thread's own. What is recorded is what was got, read back, and a request
 * the system refused is a note: a lower nice wants a privilege a desktop
 * session seldom has.
 *
 * Affinity and priority are each system's own. Linux has both, a nice
 * per thread. Windows has a mask of CPUs and a priority class for the
 * process, and a mask and a priority level for a thread: the nice the
 * option holds is mapped onto the class or level nearest it, and what is
 * recorded is the nice that class or level maps back to (win_class_of()).
 * macOS has a nice for the whole process and no CPU affinity, so the
 * process's nice is set there and the rest refused with a note and
 * recorded as null; the load runs either way.
 */
#if defined(__linux__)
#  define _GNU_SOURCE
#elif !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "bench.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/resource.h>
#  include <unistd.h>
#endif
#if defined(__linux__)
#  include <sched.h>
#endif

/* How long one duty cycle is: short enough that the load is a steady
 * pressure at the frame's scale, long enough that a slot is many of the
 * scheduler's decisions and not one. */
#define SLOT_S 0.010
#define LOAD_THREADS_MAX 64

/* ---- CPU lists: "0-3,6" -------------------------------------------------- */

#if defined(__linux__)

/* `spec` into `set`; false where it is not a list of CPUs this system can
 * name. */
static bool cpu_list_parse(const char *spec, cpu_set_t *set)
{
	CPU_ZERO(set);
	const char *p = spec;
	if (!*p)
		return false;
	while (*p) {
		char *end;
		const long a = strtol(p, &end, 10);
		if (end == p || a < 0 || a >= CPU_SETSIZE)
			return false;
		long b = a;
		p = end;
		if (*p == '-') {
			b = strtol(p + 1, &end, 10);
			if (end == p + 1 || b < a || b >= CPU_SETSIZE)
				return false;
			p = end;
		}
		for (long i = a; i <= b; i++)
			CPU_SET((int)i, set);
		if (*p == ',')
			p++;
		else if (*p)
			return false;
	}
	return true;
}

static void cpu_list_format(const cpu_set_t *set, char *out, size_t n)
{
	size_t k = 0;
	out[0] = '\0';
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, set))
			continue;
		int j = i;
		while (j + 1 < CPU_SETSIZE && CPU_ISSET(j + 1, set))
			j++;
		const int w = j > i ? snprintf(out + k, n - k, "%s%d-%d",
									   k ? "," : "", i, j)
							: snprintf(out + k, n - k, "%s%d",
									   k ? "," : "", i);
		if (w < 0 || (size_t)w >= n - k)
			break;
		k += (size_t)w;
		i = j;
	}
}

#endif

#if defined(_WIN32)

/* The same lists as a mask of the first 64 CPUs, which is what a process
 * outside a processor group of its own can be put on. */
static bool cpu_mask_parse(const char *spec, DWORD_PTR *mask)
{
	*mask = 0;
	const char *p = spec;
	const long top = (long)(sizeof(DWORD_PTR) * CHAR_BIT);
	if (!*p)
		return false;
	while (*p) {
		char *end;
		const long a = strtol(p, &end, 10);
		if (end == p || a < 0 || a >= top)
			return false;
		long b = a;
		p = end;
		if (*p == '-') {
			b = strtol(p + 1, &end, 10);
			if (end == p + 1 || b < a || b >= top)
				return false;
			p = end;
		}
		for (long i = a; i <= b; i++)
			*mask |= (DWORD_PTR)1 << i;
		if (*p == ',')
			p++;
		else if (*p)
			return false;
	}
	return true;
}

static void cpu_mask_format(DWORD_PTR mask, char *out, size_t n)
{
	const int top = (int)(sizeof mask * CHAR_BIT);
	size_t k = 0;
	out[0] = '\0';
	for (int i = 0; i < top; i++) {
		if (!(mask >> i & 1))
			continue;
		int j = i;
		while (j + 1 < top && (mask >> (j + 1) & 1))
			j++;
		const int w = j > i ? snprintf(out + k, n - k, "%s%d-%d",
									   k ? "," : "", i, j)
							: snprintf(out + k, n - k, "%s%d",
									   k ? "," : "", i);
		if (w < 0 || (size_t)w >= n - k)
			break;
		k += (size_t)w;
		i = j;
	}
}

/* A nice onto Windows' five process classes and back: each class stands
 * for the nice at its middle, the bands' edges halfway between. REALTIME
 * is left out, as a nice below -20 is: it wants the administrator, and
 * starves the window server's own threads. */
static const struct {
	DWORD cls;
	int   nice, thread;
	const char *name;
} WIN_CLASSES[] = {
	{ IDLE_PRIORITY_CLASS,          19, THREAD_PRIORITY_IDLE,  "idle" },
	{ BELOW_NORMAL_PRIORITY_CLASS,  10, THREAD_PRIORITY_BELOW_NORMAL,
	  "below normal" },
	{ NORMAL_PRIORITY_CLASS,         0, THREAD_PRIORITY_NORMAL, "normal" },
	{ ABOVE_NORMAL_PRIORITY_CLASS, -10, THREAD_PRIORITY_ABOVE_NORMAL,
	  "above normal" },
	{ HIGH_PRIORITY_CLASS,         -20, THREAD_PRIORITY_HIGHEST, "high" },
};
#define WIN_CLASSES_N ((int)(sizeof WIN_CLASSES / sizeof WIN_CLASSES[0]))

/* The row whose nice is nearest `nice`, ties toward normal. */
static int win_class_of(int nice)
{
	int best = 2;
	for (int i = 0; i < WIN_CLASSES_N; i++) {
		const int d = abs(WIN_CLASSES[i].nice - nice);
		const int bd = abs(WIN_CLASSES[best].nice - nice);
		if (d < bd || (d == bd && abs(WIN_CLASSES[i].nice) <
								  abs(WIN_CLASSES[best].nice)))
			best = i;
	}
	return best;
}

static int win_nice_of_class(DWORD cls)
{
	for (int i = 0; i < WIN_CLASSES_N; i++)
		if (WIN_CLASSES[i].cls == cls)
			return WIN_CLASSES[i].nice;
	return INT_MIN;
}

#endif

bool bench_cpu_list_valid(const char *spec)
{
	if (!spec[0])
		return true;
#if defined(__linux__)
	cpu_set_t set;
	return cpu_list_parse(spec, &set);
#elif defined(_WIN32)
	DWORD_PTR mask;
	return cpu_mask_parse(spec, &mask);
#else
	/* Refused at the run, with a note, rather than here: a record made on
	 * Linux replays on another system with its options intact. */
	return true;
#endif
}

/* ---- the process --------------------------------------------------------- */

#if defined(__linux__)
static cpu_set_t g_mask_was;
static bool      g_mask_saved;
#elif defined(_WIN32)
static DWORD_PTR g_mask_was;
static bool      g_mask_saved;
static DWORD     g_class_was;
#endif
#if !defined(_WIN32)
static int       g_nice_was;
#endif
static bool      g_nice_saved;

/* What this thread (and so the threads it starts) runs on, and at. */
static void read_back(char *affinity, size_t n, int *nice)
{
	affinity[0] = '\0';
	*nice = INT_MIN;
#if defined(__linux__)
	cpu_set_t set;
	if (sched_getaffinity(0, sizeof set, &set) == 0)
		cpu_list_format(&set, affinity, n);
	errno = 0;
	const int v = getpriority(PRIO_PROCESS, 0);
	if (!(v == -1 && errno))
		*nice = v;
#elif defined(_WIN32)
	DWORD_PTR mine, sys;
	if (GetProcessAffinityMask(GetCurrentProcess(), &mine, &sys))
		cpu_mask_format(mine, affinity, n);
	const DWORD cls = GetPriorityClass(GetCurrentProcess());
	if (cls)
		*nice = win_nice_of_class(cls);
#else
	/* macOS: a nice, and no affinity to read. */
	(void)n;
	errno = 0;
	const int v = getpriority(PRIO_PROCESS, 0);
	if (!(v == -1 && errno))
		*nice = v;
#endif
}

void bench_process_apply(const BenchConfig *c, BenchConditions *out)
{
	const char *aff = bench_opt_text(c, OPT_PROCESS_AFFINITY);
	const bool want_nice = c->set[OPT_PRIORITY];
	const int nice = bench_opt_int(c, OPT_PRIORITY);
#if defined(__linux__)
	if (aff[0]) {
		cpu_set_t set;
		g_mask_saved = sched_getaffinity(0, sizeof g_mask_was,
										 &g_mask_was) == 0;
		if (!cpu_list_parse(aff, &set) ||
			sched_setaffinity(0, sizeof set, &set) != 0) {
			snprintf(out->notes, sizeof out->notes, "the process could "
					 "not be put on CPUs %s (%s)", aff, strerror(errno));
		}
	}
	if (want_nice) {
		errno = 0;
		const int was = getpriority(PRIO_PROCESS, 0);
		g_nice_saved = !(was == -1 && errno);
		g_nice_was = was;
		if (setpriority(PRIO_PROCESS, 0, nice) != 0) {
			const size_t k = strlen(out->notes);
			snprintf(out->notes + k, sizeof out->notes - k, "%sthe "
					 "process's nice %d was refused (%s)", k ? "; " : "",
					 nice, strerror(errno));
		}
	}
#elif defined(_WIN32)
	const HANDLE self = GetCurrentProcess();
	if (aff[0]) {
		DWORD_PTR mask, sys;
		g_mask_saved = GetProcessAffinityMask(self, &g_mask_was, &sys) != 0;
		if (!cpu_mask_parse(aff, &mask) || !SetProcessAffinityMask(self,
																   mask))
			snprintf(out->notes, sizeof out->notes, "the process could "
					 "not be put on CPUs %s (error %lu)", aff,
					 (unsigned long)GetLastError());
	}
	if (want_nice) {
		const int row = win_class_of(nice);
		g_class_was = GetPriorityClass(self);
		g_nice_saved = g_class_was != 0;
		if (!SetPriorityClass(self, WIN_CLASSES[row].cls)) {
			const size_t k = strlen(out->notes);
			snprintf(out->notes + k, sizeof out->notes - k, "%sthe "
					 "process's priority class %s was refused (error %lu)",
					 k ? "; " : "", WIN_CLASSES[row].name,
					 (unsigned long)GetLastError());
		}
	}
#else
	/* macOS: the process's nice is POSIX's; there is no CPU affinity to
	 * ask for, only hints the scheduler may ignore. */
	if (aff[0])
		snprintf(out->notes, sizeof out->notes, "macOS puts no process "
				 "on chosen CPUs");
	if (want_nice) {
		errno = 0;
		const int was = getpriority(PRIO_PROCESS, 0);
		g_nice_saved = !(was == -1 && errno);
		g_nice_was = was;
		if (setpriority(PRIO_PROCESS, 0, nice) != 0) {
			const size_t k = strlen(out->notes);
			snprintf(out->notes + k, sizeof out->notes - k, "%sthe "
					 "process's nice %d was refused (%s)", k ? "; " : "",
					 nice, strerror(errno));
		}
	}
#endif
	read_back(out->process_affinity, sizeof out->process_affinity,
			  &out->process_nice);
}

/* A nice raised for a run can be lowered again only with the privilege a
 * lower nice wants, so without it the process keeps the raised one, and
 * so does every run after this one in it: said, as a refusal is. */
void bench_process_restore(BenchConditions *out)
{
#if defined(__linux__)
	if (g_mask_saved)
		(void)sched_setaffinity(0, sizeof g_mask_was, &g_mask_was);
	g_mask_saved = false;
#elif defined(_WIN32)
	if (g_mask_saved)
		(void)SetProcessAffinityMask(GetCurrentProcess(), g_mask_was);
	g_mask_saved = false;
	if (g_nice_saved && !SetPriorityClass(GetCurrentProcess(),
										  g_class_was)) {
		const size_t k = strlen(out->notes);
		snprintf(out->notes + k, sizeof out->notes - k, "%sthe process's "
				 "priority class could not be set back (error %lu), so "
				 "the runs after this one keep it", k ? "; " : "",
				 (unsigned long)GetLastError());
	}
	g_nice_saved = false;
#endif
#if !defined(_WIN32)
	if (g_nice_saved && setpriority(PRIO_PROCESS, 0, g_nice_was) != 0) {
		const size_t k = strlen(out->notes);
		snprintf(out->notes + k, sizeof out->notes - k, "%sthe process's "
				 "nice could not be set back to %d (%s), so the runs after "
				 "this one keep it", k ? "; " : "", g_nice_was,
				 strerror(errno));
	}
	g_nice_saved = false;
#endif
}

/* ---- the load ------------------------------------------------------------ */

static struct {
	int         n;
	double      duty;
	atomic_bool run;
	atomic_int  failed_affinity, failed_nice;
	const char *affinity;
	bool        want_nice;
	int         nice;
	double      started;
	_Atomic double cpu_s;           /* every thread's, added as each ends */
#if defined(_WIN32)
	HANDLE      t[LOAD_THREADS_MAX];
#else
	pthread_t   t[LOAD_THREADS_MAX];
#endif
} L;

/* This thread's CPU seconds. */
static double thread_cpu(void)
{
#if defined(_WIN32)
	FILETIME c, e, k, u;
	if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
		return 0.0;
	const ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) |
						 k.dwLowDateTime) +
						(((ULONGLONG)u.dwHighDateTime << 32) |
						 u.dwLowDateTime);
	return (double)t * 1e-7;
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return 0.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void nap_s(double s)
{
	if (s <= 0.0)
		return;
#if defined(_WIN32)
	const DWORD ms = (DWORD)(s * 1e3 + 0.5);
	Sleep(ms ? ms : 1);
#else
	struct timespec ts = { 0, (long)(s * 1e9) };
	nanosleep(&ts, NULL);
#endif
}

static void load_body(void)
{
#if defined(__linux__)
	if (L.affinity[0]) {
		cpu_set_t set;
		if (!cpu_list_parse(L.affinity, &set) ||
			pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
			atomic_store(&L.failed_affinity, 1);
	}
	/* On Linux a nice is a thread's, and 0 for "who" names the caller. */
	if (L.want_nice && setpriority(PRIO_PROCESS, 0, L.nice) != 0)
		atomic_store(&L.failed_nice, 1);
#elif defined(_WIN32)
	/* A thread's level is relative to the process's class, so the nice
	 * maps onto the level whose class row it would have chosen. */
	if (L.affinity[0]) {
		DWORD_PTR mask;
		if (!cpu_mask_parse(L.affinity, &mask) ||
			!SetThreadAffinityMask(GetCurrentThread(), mask))
			atomic_store(&L.failed_affinity, 1);
	}
	if (L.want_nice &&
		!SetThreadPriority(GetCurrentThread(),
						   WIN_CLASSES[win_class_of(L.nice)].thread))
		atomic_store(&L.failed_nice, 1);
#endif
	const double t0 = thread_cpu();
	/* Busy, then idle, each slot: the busy part spins on the clock, which
	 * is work no compiler can take away and no cache keeps cheap. */
	while (atomic_load(&L.run)) {
		const double slot = bench_now();
		const double busy_until = slot + SLOT_S * L.duty;
		volatile unsigned x = 1;
		while (bench_now() < busy_until)
			for (int i = 0; i < 1000; i++)
				x = x * 1664525u + 1013904223u;
		nap_s(slot + SLOT_S - bench_now());
	}
	double was = atomic_load(&L.cpu_s);
	while (!atomic_compare_exchange_weak(&L.cpu_s, &was,
										 was + thread_cpu() - t0)) {
	}
}

#if defined(_WIN32)
static DWORD WINAPI load_thread(LPVOID unused)
{
	(void)unused;
	load_body();
	return 0;
}
#else
static void *load_thread(void *unused)
{
	(void)unused;
	load_body();
	return NULL;
}
#endif

void bench_load_start(const BenchConfig *c, BenchConditions *out)
{
	memset(&L, 0, sizeof L);
	const int want = bench_opt_int(c, OPT_LOAD_THREADS);
	out->load_duty_pct = bench_opt_int(c, OPT_LOAD_DUTY);
	if (want <= 0)
		return;
	L.duty = bench_opt_int(c, OPT_LOAD_DUTY) / 100.0;
	L.affinity = bench_opt_text(c, OPT_LOAD_AFFINITY);
	L.want_nice = c->set[OPT_LOAD_PRIORITY];
	L.nice = bench_opt_int(c, OPT_LOAD_PRIORITY);
	atomic_store(&L.run, true);
	L.started = bench_now();
	for (int i = 0; i < want && i < LOAD_THREADS_MAX; i++) {
#if defined(_WIN32)
		L.t[i] = CreateThread(NULL, 0, load_thread, NULL, 0, NULL);
		if (!L.t[i])
			break;
#else
		if (pthread_create(&L.t[i], NULL, load_thread, NULL) != 0)
			break;
#endif
		L.n++;
	}
	out->load_threads = L.n;
#if defined(__APPLE__)
	/* A nice is the whole process's there, and no thread has CPUs. */
	if (L.affinity[0] || L.want_nice) {
		const size_t k = strlen(out->notes);
		snprintf(out->notes + k, sizeof out->notes - k, "%smacOS sets "
				 "neither a thread's CPUs nor a nice of its own", k ? "; "
																	: "");
	}
#endif
}

void bench_load_stop(BenchConditions *out)
{
	if (L.n == 0)
		return;
	atomic_store(&L.run, false);
	for (int i = 0; i < L.n; i++) {
#if defined(_WIN32)
		WaitForSingleObject(L.t[i], INFINITE);
		CloseHandle(L.t[i]);
#else
		pthread_join(L.t[i], NULL);
#endif
	}
	const double wall = bench_now() - L.started;
	if (wall > 0.0)
		out->load_cpu_pct = 100.0 * atomic_load(&L.cpu_s) / (wall * L.n);
	const size_t k = strlen(out->notes);
	if (atomic_load(&L.failed_affinity))
		snprintf(out->notes + k, sizeof out->notes - k, "%sthe load could "
				 "not be put on CPUs %s", k ? "; " : "", L.affinity);
	const size_t k2 = strlen(out->notes);
	if (atomic_load(&L.failed_nice))
		snprintf(out->notes + k2, sizeof out->notes - k2, "%sthe load's "
				 "nice %d was refused", k2 ? "; " : "", L.nice);
	/* What was asked, where it was granted: each thread checked its own.
	 * Windows' is the nice of the level it was given. */
#if defined(__linux__)
	if (L.affinity[0] && !atomic_load(&L.failed_affinity))
		snprintf(out->load_affinity, sizeof out->load_affinity, "%s",
				 L.affinity);
	if (L.want_nice && !atomic_load(&L.failed_nice))
		out->load_nice = L.nice;
#elif defined(_WIN32)
	DWORD_PTR mask;
	if (L.affinity[0] && !atomic_load(&L.failed_affinity) &&
		cpu_mask_parse(L.affinity, &mask))
		cpu_mask_format(mask, out->load_affinity, sizeof out->load_affinity);
	if (L.want_nice && !atomic_load(&L.failed_nice))
		out->load_nice = WIN_CLASSES[win_class_of(L.nice)].nice;
#else
	/* Neither is set there: an asked one is recorded as unknown, not as
	 * the process's. */
	out->load_affinity[0] = '\0';
	if (L.want_nice)
		out->load_nice = INT_MIN;
#endif
	/* Not asked: what they started with, the process's. */
	if (!L.affinity[0])
		snprintf(out->load_affinity, sizeof out->load_affinity, "%s",
				 out->process_affinity);
	if (!L.want_nice)
		out->load_nice = out->process_nice;
	L.n = 0;
}
