/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * This process's footprint for --footprint-log: its CPU, as the share of one
 * core its threads used since the last sample -- 100 is a core's worth, and
 * several busy threads go past it -- and its resident memory, in MiB. The
 * system's own figures, read the way each system keeps them: getrusage()
 * and /proc/self/statm on Linux, getrusage() and the task's basic info on
 * macOS, GetProcessTimes() and K32GetProcessMemoryInfo() -- kernel32's, so
 * nothing more is linked -- on Windows. The library's metrics panel reads
 * the same figures the same way (src/core/metrics.c) for its own drawing;
 * these are the program's, for a line a harness reads and for the record.
 *
 * And for the benchmark's record (bench.h): the CPU split into user and
 * system time, the peak resident set, the thread count, and the machine
 * the figures were taken on, each "unknown" where its system will not say.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#  endif
#endif

#include "testbed.h"
#include "bench.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <sys/utsname.h>
#  include <time.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach/mach.h>
#    include <sys/sysctl.h>
#  endif
#endif

/* Seconds of CPU this process has used, and seconds of a clock that only
 * goes forward; false where the system will not say. */
static bool times_now(double *cpu, double *wall)
{
#if defined(_WIN32)
	FILETIME created, ended, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel,
						 &user))
		return false;
	const ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) |
						kernel.dwLowDateTime;
	const ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) |
						user.dwLowDateTime;
	*cpu = (double)(k + u) * 1e-7;          /* 100 ns units */
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	*wall = (double)c.QuadPart / (double)f.QuadPart;
	return true;
#else
	struct rusage ru;
	struct timespec ts;
	if (getrusage(RUSAGE_SELF, &ru) != 0 ||
		clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return false;
	*cpu = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 +
		   (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
	*wall = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
	return true;
#endif
}

/* The resident set, in MiB; -1 where the system will not say. */
double testbed_proc_rss_mib(void)
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS pmc;
	if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
		return -1.0;
	return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
#elif defined(__APPLE__)
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
				  (task_info_t)&info, &n) != KERN_SUCCESS)
		return -1.0;
	return (double)info.resident_size / (1024.0 * 1024.0);
#else
	FILE *f = fopen("/proc/self/statm", "r");
	if (!f)
		return -1.0;
	long size = 0, resident = 0;
	const int got = fscanf(f, "%ld %ld", &size, &resident);
	fclose(f);
	const long page = sysconf(_SC_PAGESIZE);
	if (got != 2 || page <= 0)
		return -1.0;
	return (double)resident * (double)page / (1024.0 * 1024.0);
#endif
}

/* At most every PROC_EVERY seconds: a share of CPU over a few frames is
 * noise, and the panel is redrawn far more often than that. */
#define PROC_EVERY 0.5

void testbed_proc_sample(double *cpu_pct, double *rss)
{
	static double last_cpu = -1.0, last_wall, pct = -1.0, mib = -1.0;
	double cpu, wall;
	if (!times_now(&cpu, &wall)) {
		*cpu_pct = *rss = -1.0;
		return;
	}
	if (last_cpu < 0.0) {
		last_cpu = cpu;
		last_wall = wall;
		mib = testbed_proc_rss_mib();
	} else if (wall - last_wall >= PROC_EVERY) {
		pct = 100.0 * (cpu - last_cpu) / (wall - last_wall);
		last_cpu = cpu;
		last_wall = wall;
		mib = testbed_proc_rss_mib();
	}
	*cpu_pct = pct;
	*rss = mib;
}

/* ---- for the benchmark's record ------------------------------------------ */

bool testbed_proc_times(double *user_s, double *sys_s)
{
#if defined(_WIN32)
	FILETIME created, ended, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel,
						 &user))
		return false;
	*sys_s  = (double)(((ULONGLONG)kernel.dwHighDateTime << 32) |
					   kernel.dwLowDateTime) * 1e-7;
	*user_s = (double)(((ULONGLONG)user.dwHighDateTime << 32) |
					   user.dwLowDateTime) * 1e-7;
	return true;
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return false;
	*user_s = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6;
	*sys_s  = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
	return true;
#endif
}

double testbed_proc_peak_rss_mib(void)
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS pmc;
	if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
		return -1.0;
	return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return -1.0;
#  if defined(__APPLE__)
	return (double)ru.ru_maxrss / (1024.0 * 1024.0);    /* bytes there */
#  else
	return (double)ru.ru_maxrss / 1024.0;               /* KiB on Linux */
#  endif
#endif
}

int testbed_proc_threads(void)
{
#if defined(_WIN32)
	return -1;
#elif defined(__APPLE__)
	thread_act_array_t list;
	mach_msg_type_number_t n = 0;
	if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
		return -1;
	for (mach_msg_type_number_t i = 0; i < n; i++)
		mach_port_deallocate(mach_task_self(), list[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)list,
				  n * sizeof list[0]);
	return (int)n;
#else
	FILE *f = fopen("/proc/self/status", "r");
	if (!f)
		return -1;
	char line[256];
	int n = -1;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "Threads: %d", &n) == 1)
			break;
	fclose(f);
	return n;
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
/* The value of the first line of `path` that starts with `key`, past the
 * separator, its newline and quotes taken off; "" where there is none. */
static void file_field(const char *path, const char *key, char sep,
					  char *out, size_t n)
{
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[512];
	const size_t klen = strlen(key);
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, key, klen) != 0)
			continue;
		const char *v = strchr(line + klen, sep);
		if (!v)
			continue;
		v++;
		while (*v == ' ' || *v == '\t' || *v == '"')
			v++;
		snprintf(out, n, "%s", v);
		size_t k = strlen(out);
		while (k && (out[k - 1] == '\n' || out[k - 1] == '"' ||
					 out[k - 1] == ' '))
			out[--k] = '\0';
		break;
	}
	fclose(f);
}
#endif

void testbed_machine(char *host, size_t host_n, char *os, size_t os_n,
					 char *cpu, size_t cpu_n, int *cpus, double *mem_mib)
{
	host[0] = os[0] = cpu[0] = '\0';
	*cpus = -1;
	*mem_mib = -1.0;
#if defined(_WIN32)
	DWORD hn = (DWORD)host_n;
	if (!GetComputerNameA(host, &hn))
		host[0] = '\0';
	snprintf(os, os_n, "Windows");
	(void)cpu_n;
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	*cpus = (int)si.dwNumberOfProcessors;
	MEMORYSTATUSEX ms;
	ms.dwLength = sizeof ms;
	if (GlobalMemoryStatusEx(&ms))
		*mem_mib = (double)ms.ullTotalPhys / (1024.0 * 1024.0);
#else
	if (gethostname(host, host_n) != 0)
		host[0] = '\0';
	host[host_n - 1] = '\0';
	struct utsname u;
	const bool named = uname(&u) == 0;
	const long online = sysconf(_SC_NPROCESSORS_ONLN);
	*cpus = online > 0 ? (int)online : -1;
#  if defined(__APPLE__)
	if (named)
		snprintf(os, os_n, "%s %s", u.sysname, u.release);
	size_t len = cpu_n;
	if (sysctlbyname("machdep.cpu.brand_string", cpu, &len, NULL, 0) != 0)
		cpu[0] = '\0';
	uint64_t mem = 0;
	len = sizeof mem;
	if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0)
		*mem_mib = (double)mem / (1024.0 * 1024.0);
#  else
	char pretty[128];
	file_field("/etc/os-release", "PRETTY_NAME", '=', pretty, sizeof pretty);
	if (named)
		snprintf(os, os_n, "%s%s%s %s %s", pretty, pretty[0] ? ", " : "",
				 u.sysname, u.release, u.machine);
	file_field("/proc/cpuinfo", "model name", ':', cpu, cpu_n);
	const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page > 0)
		*mem_mib = (double)pages * (double)page / (1024.0 * 1024.0);
#  endif
#endif
}
