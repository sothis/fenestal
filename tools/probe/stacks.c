/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Where every thread of one process is: its call stack, a frame a line, as
 * module and offset.
 *
 *     stacks <image-name>
 *
 * For a program that has stopped and not ended. sampler.c says which module
 * a thread spends its time in; this says how it got there, which is the
 * question when nothing is spending any. No symbols: the offsets are read
 * against the linker's map or addr2line on the build that was copied over
 * (the image's base in the file is added to them).
 *
 *     x86_64-w64-mingw32-gcc -O2 -o stacks.exe stacks.c -ldbghelp */
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#define MAX_FRAMES 48

static DWORD find_pid(const char *image)
{
	PROCESSENTRY32 pe = { .dwSize = sizeof pe };
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	DWORD pid = 0;

	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	for (BOOL ok = Process32First(snap, &pe); ok && !pid;
		 ok = Process32Next(snap, &pe))
		if (!_stricmp(pe.szExeFile, image))
			pid = pe.th32ProcessID;
	CloseHandle(snap);
	return pid;
}

static void walk(HANDLE proc, HANDLE thread)
{
	CONTEXT ctx = { .ContextFlags = CONTEXT_FULL };
	STACKFRAME64 f = { 0 };

	if (!GetThreadContext(thread, &ctx))
		return;
	f.AddrPC.Offset    = ctx.Rip;
	f.AddrFrame.Offset = ctx.Rbp;
	f.AddrStack.Offset = ctx.Rsp;
	f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;

	for (int i = 0; i < MAX_FRAMES; i++) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &ctx,
						 NULL, SymFunctionTableAccess64, SymGetModuleBase64,
						 NULL) || !f.AddrPC.Offset)
			break;
		IMAGEHLP_MODULE64 m = { .SizeOfStruct = sizeof m };
		if (SymGetModuleInfo64(proc, f.AddrPC.Offset, &m))
			printf("    %-28s +%llx\n", m.ModuleName,
				   (unsigned long long)(f.AddrPC.Offset - m.BaseOfImage));
		else
			printf("    %016llx\n", (unsigned long long)f.AddrPC.Offset);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: stacks <image-name>\n");
		return 2;
	}
	const DWORD pid = find_pid(argv[1]);
	if (!pid) {
		fprintf(stderr, "no process called %s\n", argv[1]);
		return 1;
	}
	HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
							  FALSE, pid);
	if (!proc || !SymInitialize(proc, NULL, TRUE)) {
		fprintf(stderr, "cannot open process %lu (%lu)\n", pid,
				GetLastError());
		return 1;
	}

	THREADENTRY32 te = { .dwSize = sizeof te };
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid)
			continue;
		HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
							  THREAD_QUERY_INFORMATION, FALSE,
							  te.th32ThreadID);
		if (!t)
			continue;
		printf("thread %lu\n", te.th32ThreadID);
		if (SuspendThread(t) != (DWORD)-1) {
			walk(proc, t);
			ResumeThread(t);
		}
		CloseHandle(t);
	}
	CloseHandle(snap);
	SymCleanup(proc);
	CloseHandle(proc);
	return 0;
}
