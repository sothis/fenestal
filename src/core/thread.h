/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Threading, in the three shapes this library uses: a mutex, a condition
 * variable with a bounded wait, and one thread. pthreads on POSIX; on
 * Windows an SRW lock, and an event beside a timer for the condition.
 *
 * The timed wait is *relative*. A waitable timer takes an interval from now;
 * pthread_cond_timedwait takes an absolute deadline on the condvar's own
 * clock, so POSIX converts once here rather than at each call site. A
 * relative seam also needs no pthread_condattr_setclock, which macOS lacks.
 */

#ifndef FNL_THREAD_H
#define FNL_THREAD_H

#include "core/compiler.h"

#if defined(_WIN32)

#include <windows.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <intrin.h>     /* __rdtsc(), for thread_cpu_seconds() */
#endif

typedef SRWLOCK            FnlMutex FNL_CAPABILITY("mutex");
typedef HANDLE             FnlThread;

/* Not a CONDITION_VARIABLE: its timeout expires on the scheduler's tick,
 * 15.6 ms unless the process raises it, and the frame loop's holds are
 * fractions of a display period. Raising it is timeBeginPeriod, which is
 * winmm on the link line. A high-resolution timer is kernel32's and can only
 * be waited on as a handle, so the wake is a handle too.
 *
 * **One waiter per condition**: the timer is the condition's, not the
 * wait's. Every signal is made under the mutex, and an auto-reset event
 * stays set until a wait takes it, so one made between the unlock and the
 * wait is not lost. */
typedef struct {
	HANDLE wake;
	HANDLE timer;      /* NULL before Windows 10 1803: the tick it is, then */
} FnlCond;

/* Absent from mingw-w64 headers older than the flag. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static inline void fnl_mutex_init(FnlMutex *m)   { InitializeSRWLock(m); }
static inline void fnl_mutex_lock(FnlMutex *m) FNL_ACQUIRE(*m) FNL_NO_TSA
{
	AcquireSRWLockExclusive(m);
}
static inline void fnl_mutex_unlock(FnlMutex *m) FNL_RELEASE(*m) FNL_NO_TSA
{
	ReleaseSRWLockExclusive(m);
}
/* An SRWLOCK has nothing to release; a call all the same, for the POSIX
 * side's sake. */
static inline void fnl_mutex_destroy(FnlMutex *m) { (void)m; }

static inline void fnl_cond_init(FnlCond *c)
{
	c->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	c->timer = CreateWaitableTimerExW(NULL, NULL,
									  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
									  TIMER_ALL_ACCESS);
}

static inline void fnl_cond_signal(FnlCond *c)
{
	if (c->wake)
		SetEvent(c->wake);
}

static inline void fnl_cond_destroy(FnlCond *c)
{
	if (c->wake)
		CloseHandle(c->wake);
	if (c->timer)
		CloseHandle(c->timer);
	c->wake = c->timer = NULL;
}

/* False iff the wait timed out. Spurious wakeups are possible on both
 * systems, so every caller loops on its own predicate. A wait that failed
 * reads as timed out: "woken" would have the caller's loop spin on it. A
 * negative wait, or NaN, is none: either converted below is undefined. */
static inline bool fnl_cond_wait_for(FnlCond *c, FnlMutex *m, double seconds)
	FNL_REQUIRES(*m) FNL_NO_TSA
{
	if (!(seconds > 0.0))
		seconds = 0.0;
	HANDLE h[2] = { c->wake, c->timer };
	DWORD  n = 1;
	DWORD  ms = (DWORD)(seconds * 1e3 + 0.5);

	/* Negative is relative, in 100 ns; zero would be an absolute time long
	 * past, which is the same wait. */
	LARGE_INTEGER due;
	due.QuadPart = -(LONGLONG)(seconds * 1e7);
	if (c->timer && SetWaitableTimer(c->timer, &due, 0, NULL, NULL, FALSE)) {
		n = 2;
		ms = INFINITE;
	}

	ReleaseSRWLockExclusive(m);
	DWORD r;
	if (c->wake) {
		r = WaitForMultipleObjects(n, h, FALSE, ms);
	} else {
		/* No event could be made. The wait is still a wait, deaf to its
		 * signal; none here is longer than FRAME_WAIT_S. */
		Sleep(ms == INFINITE ? (DWORD)(seconds * 1e3 + 0.5) : ms);
		r = WAIT_TIMEOUT;
	}
	AcquireSRWLockExclusive(m);
	return r == WAIT_OBJECT_0;
}

/* The thread entry keeps the POSIX signature, which is what render_loop is;
 * the trampoline adapts it. */
typedef void *(*FnlThreadFn)(void *);

typedef struct {
	FnlThreadFn fn;
	void       *arg;
} FnlThreadStart;

static inline DWORD WINAPI fnl_thread_trampoline(LPVOID p)
{
	FnlThreadStart *s = (FnlThreadStart *)p;
	FnlThreadStart  local = *s;
	free(s);
	local.fn(local.arg);
	return 0;
}

static inline bool fnl_thread_start(FnlThread *t, FnlThreadFn fn, void *arg)
{
	FnlThreadStart *s = (FnlThreadStart *)malloc(sizeof *s);
	if (!s)
		return false;
	s->fn = fn;
	s->arg = arg;
	*t = CreateThread(NULL, 0, fnl_thread_trampoline, s, 0, NULL);
	if (!*t) {
		free(s);
		return false;
	}
	return true;
}

static inline void fnl_thread_join(FnlThread t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}

/* ---- time ----------------------------------------------------------------
 *
 * The monotonic clock every timestamp in this library is on.
 * QueryPerformanceCounter rather than clock_gettime, which on mingw lives in
 * winpthreads -- a pthread library this system exists not to link.
 *
 * The frequency is deliberately not cached: a lazily-filled static would be an
 * unsynchronised write racing a read from both threads, on the one target
 * with no ThreadSanitizer to catch it. */
static inline double now_seconds(void)
{
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	/* QPF cannot fail on anything this targets; the test keeps a zero from
	 * being a division by it. */
	return freq.QuadPart ? (double)now.QuadPart / (double)freq.QuadPart : 0.0;
}

/* The calling thread's CPU time, or negative where it is not known.
 * GetThreadTimes() counts in the scheduler's 15.6 ms ticks, too coarse for
 * a frame; QueryThreadCycleTime() counts the processor's time-stamp
 * counter while the thread runs, which is turned into seconds at the
 * counter's rate, measured against the performance counter from the
 * clock's first reading -- not known for its first quarter second, and
 * finer with every reading after. One clock per thread, kept by its
 * caller, since the rate is state (the frequency above is not cached for
 * the same reason). Wine 10.0's QueryThreadCycleTime() fails, and the
 * time is then not known. */
typedef struct FnlCpuClock {
	uint64_t tsc0;
	double   qpc0;
	double   hz;
} FnlCpuClock;

static inline double thread_cpu_seconds(FnlCpuClock *c)
{
#if defined(__x86_64__) || defined(_M_X64)
	ULONG64 cycles = 0;
	if (!QueryThreadCycleTime(GetCurrentThread(), &cycles))
		return -1.0;
	const uint64_t tsc = __rdtsc();
	const double q = now_seconds();
	if (c->qpc0 == 0.0) {
		c->tsc0 = tsc;
		c->qpc0 = q;
	} else if (q - c->qpc0 >= 0.25 && tsc > c->tsc0) {
		c->hz = (double)(tsc - c->tsc0) / (q - c->qpc0);
	}
	return c->hz > 0.0 ? (double)cycles / c->hz : -1.0;
#else
	(void)c;
	return -1.0;
#endif
}

/* Sleep() takes whole milliseconds, and 0 would be a busy spin. */
static inline void fnl_sleep_seconds(double seconds)
{
	DWORD ms = (DWORD)(seconds * 1e3 + 0.5);
	Sleep(ms ? ms : 1);
}

#else /* POSIX */

#include <pthread.h>
#include <time.h>
#include <errno.h>

/* A capability for clang's -Wthread-safety: the shim's lock and unlock say
 * what they do to it, and the bodies are exempt because pthreads' own calls
 * carry no annotations to check them against. */
typedef pthread_mutex_t FnlMutex FNL_CAPABILITY("mutex");
typedef pthread_t       FnlThread;
typedef pthread_cond_t  FnlCond;

static inline void fnl_mutex_init(FnlMutex *m)
{
	pthread_mutex_init(m, NULL);
}
static inline void fnl_mutex_lock(FnlMutex *m) FNL_ACQUIRE(*m) FNL_NO_TSA
{
	pthread_mutex_lock(m);
}
static inline void fnl_mutex_unlock(FnlMutex *m) FNL_RELEASE(*m) FNL_NO_TSA
{
	pthread_mutex_unlock(m);
}
static inline void fnl_mutex_destroy(FnlMutex *m)
{
	pthread_mutex_destroy(m);
}

#if defined(__APPLE__)

/* macOS has no pthread_condattr_setclock, and so no absolute deadline on the
 * monotonic clock; its relative wait is measured on one, which is what the
 * clock attribute buys elsewhere. */
static inline void fnl_cond_init(FnlCond *c) { pthread_cond_init(c, NULL); }

/* As on Windows, false iff the wait timed out, and a wait that failed --
 * EINVAL from a malformed time -- reads as timed out. A negative wait, or
 * NaN, is none: its nanoseconds would be negative, which is malformed. */
static inline bool fnl_cond_wait_for(FnlCond *c, FnlMutex *m, double seconds)
	FNL_REQUIRES(*m) FNL_NO_TSA
{
	if (!(seconds > 0.0))
		seconds = 0.0;
	const struct timespec rel = {
		.tv_sec  = (time_t)seconds,
		.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9),
	};
	return pthread_cond_timedwait_relative_np(c, m, &rel) == 0;
}

#else

static inline void fnl_cond_init(FnlCond *c)
{
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	/* CLOCK_MONOTONIC, not the default CLOCK_REALTIME: a wall clock that
	 * steps (NTP, suspend) would turn a short timeout into an arbitrary one.
	 * fnl_cond_wait_for() reads the same clock. */
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(c, &attr);
	pthread_condattr_destroy(&attr);
}

/* As the macOS one above: a failed wait timed out, a negative one is none. */
static inline bool fnl_cond_wait_for(FnlCond *c, FnlMutex *m, double seconds)
	FNL_REQUIRES(*m) FNL_NO_TSA
{
	if (!(seconds > 0.0))
		seconds = 0.0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	long add_ns = (long)((seconds - (double)(time_t)seconds) * 1e9);
	ts.tv_sec  += (time_t)seconds;
	ts.tv_nsec += add_ns;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_nsec -= 1000000000L;
		ts.tv_sec++;
	}
	return pthread_cond_timedwait(c, m, &ts) == 0;
}

#endif

static inline void fnl_cond_signal(FnlCond *c)  { pthread_cond_signal(c); }
static inline void fnl_cond_destroy(FnlCond *c) { pthread_cond_destroy(c); }

typedef void *(*FnlThreadFn)(void *);

#if defined(__APPLE__)

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

/* On macOS a thread of the library's waits precisely, which is to say under
 * the time-constraint policy, the one AppKit's own display link and every
 * audio thread run under. The kernel coalesces an ordinary thread's timer by
 * a share of its interval set by the thread's tier, a quarter or a half of
 * it for a program started from a shell: a 16.7 ms hold came back 3.5 to
 * 7 ms late, past what the spacing forgives (PACE_LATE_MAX), every frame was
 * spaced from the late return, and the testbed drew 48 frames a second on a
 * 60 Hz panel. Asking for the user-interactive class instead moved nothing
 * there. Under this policy the same wait came back 0.18 ms late.
 *
 * Aperiodic, and a share of the CPU a frame's work fits in: a thread that
 * runs past it is put back among the others by the kernel rather than left
 * to starve them. Refused, the thread runs as it would have, its waits late
 * again, which is a frame rate and not a fault. */
typedef struct {
	FnlThreadFn fn;
	void       *arg;
} FnlThreadStart;

static inline void *fnl_thread_trampoline(void *p)
{
	FnlThreadStart s = *(FnlThreadStart *)p;
	free(p);
	mach_timebase_info_data_t tb;
	if (mach_timebase_info(&tb) == KERN_SUCCESS && tb.numer) {
		const double per_ms = 1e6 * tb.denom / tb.numer;
		thread_time_constraint_policy_data_t pol = {
			.period      = 0,
			.computation = (uint32_t)(5.0 * per_ms),
			.constraint  = (uint32_t)(10.0 * per_ms),
			.preemptible = 1,
		};
		thread_policy_set(pthread_mach_thread_np(pthread_self()),
						  THREAD_TIME_CONSTRAINT_POLICY,
						  (thread_policy_t)&pol,
						  THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	}
	return s.fn(s.arg);
}

static inline bool fnl_thread_start(FnlThread *t, FnlThreadFn fn, void *arg)
{
	FnlThreadStart *s = malloc(sizeof *s);
	if (!s)
		return false;
	s->fn = fn;
	s->arg = arg;
	if (pthread_create(t, NULL, fnl_thread_trampoline, s) != 0) {
		free(s);
		return false;
	}
	return true;
}

#else

static inline bool fnl_thread_start(FnlThread *t, FnlThreadFn fn, void *arg)
{
	return pthread_create(t, NULL, fn, arg) == 0;
}

#endif

static inline void fnl_thread_join(FnlThread t) { pthread_join(t, NULL); }

/* ---- time ---------------------------------------------------------------- */

/* The monotonic clock every timestamp in this library is on. */
static inline double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The calling thread's CPU time, or negative where the clock is refused.
 * A system call on Linux, about a microsecond: not for every draw call.
 * The clock is Windows' state (its counter's rate) and nothing here. */
typedef struct FnlCpuClock {
	char unused;
} FnlCpuClock;

static inline double thread_cpu_seconds(FnlCpuClock *c)
{
	(void)c;
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return -1.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline void fnl_sleep_seconds(double seconds)
{
	struct timespec ts = {
		.tv_sec  = (time_t)seconds,
		.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9),
	};
	nanosleep(&ts, NULL);
}

#endif

#endif /* FNL_THREAD_H */
