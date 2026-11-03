/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Put the machine to sleep and have it wake itself, then keep it awake:
 * nap <seconds> [<hold>]. What a window does across a suspend is a
 * laptop's question, and one that wants nobody at the lid.
 *
 * A waitable timer set to resume the system is what wakes it, which the
 * power plan may forbid ("allow wake timers") and firmware may not have;
 * the first is said before anything is suspended, and a machine that did
 * not wake has to be woken by hand.
 *
 * A machine a timer wakes is unattended until input arrives: its displays
 * stay off, and Windows puts it back to sleep after the hidden "System
 * unattended sleep timeout", two minutes by default. A request to keep the
 * display on does not turn it on. So once it is back a pointer move of a
 * pixel and back is sent, which ends that state as a hand would, and the
 * power plan's own timeouts rule again. The system and the display are then
 * held on for <hold> seconds as well (600 unless given, 0 for none), for a
 * plan whose timeouts are short. Both want a desktop: started over ssh, in
 * the services session, the move goes nowhere and the display request is
 * accepted and ignored. tools/probe/native_nap.py starts it on the laptop's
 * desktop and reads its log back.
 *
 *     x86_64-w64-mingw32-gcc -O2 -o nap.exe nap.c -lpowrprof -luser32 */
#include <windows.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	const double seconds = argc > 1 ? atof(argv[1]) : 30.0;
	const double hold = argc > 2 ? atof(argv[2]) : 600.0;

	setvbuf(stdout, NULL, _IONBF, 0);
	HANDLE timer = CreateWaitableTimerA(NULL, TRUE, NULL);
	if (!timer) {
		printf("no timer (%lu)\n", (unsigned long)GetLastError());
		return 1;
	}
	/* Relative, in 100 ns, and negative for that. */
	LARGE_INTEGER due;
	due.QuadPart = -(LONGLONG)(seconds * 1e7);
	SetLastError(0);
	if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, TRUE) ||
		GetLastError() == ERROR_NOT_SUPPORTED) {
		printf("this machine will not be woken by a timer (%lu); not "
			   "suspended\n", (unsigned long)GetLastError());
		return 1;
	}

	const ULONGLONG t0 = GetTickCount64();
	printf("suspending for %.0f s\n", seconds);
	/* Sleep and not hibernate, not forced, and wake events left on: the
	 * timer is one. Returns once the machine is back. */
	if (!SetSuspendState(FALSE, FALSE, FALSE)) {
		printf("would not suspend (%lu)\n", (unsigned long)GetLastError());
		return 1;
	}
	WaitForSingleObject(timer, (DWORD)(seconds * 1e3) + 30000);
	/* GetTickCount64 does not count the time asleep on every Windows, so
	 * what is printed is only that this line was reached. */
	printf("awake, %.1f s of ticks later\n",
		   (double)(GetTickCount64() - t0) / 1e3);

	/* Relative, and back again, so that the pointer ends where it was. */
	INPUT move[2] = {0};
	move[0].type = move[1].type = INPUT_MOUSE;
	move[0].mi.dwFlags = move[1].mi.dwFlags = MOUSEEVENTF_MOVE;
	move[0].mi.dx = 1;
	move[1].mi.dx = -1;
	if (SendInput(2, move, sizeof move[0]) != 2)
		printf("no input sent (%lu); the machine is still unattended\n",
			   (unsigned long)GetLastError());
	else
		printf("input sent\n");
	if (hold <= 0)
		return 0;

	/* Asked for only after the wake, so that nothing held can stand in
	 * the suspend's way. */
	if (!SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
								 ES_DISPLAY_REQUIRED)) {
		printf("could not hold the machine awake; it may sleep again\n");
		return 1;
	}
	printf("holding the machine awake for %.0f s\n", hold);
	Sleep((DWORD)(hold * 1e3));
	SetThreadExecutionState(ES_CONTINUOUS);
	printf("released\n");
	return 0;
}
