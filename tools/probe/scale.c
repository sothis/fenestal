/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Each display's scale as Settings has it, what Settings recommends and
 * offers for it, and a way to move it: scale, or
 * `scale \\.\DISPLAY1 150`. What a laptop's panel is run at is a setting
 * and not a property, so a round at 150% wants no second laptop and no hand
 * on this one.
 *
 * Windows keeps a display's scale as a number of steps from the one it
 * recommends, and reads and writes it through DisplayConfigGetDeviceInfo
 * and DisplayConfigSetDeviceInfo with two types that are in no header, -3
 * and -4, as Settings does. The older way, SystemParametersInfo with 0x009E
 * and 0x009F, answers on this Windows and means nothing: the same -2 at
 * 125% and at 175%, and a set that is accepted and does not happen.
 *
 * The change is the user's own setting, written where Settings writes it:
 * put it back. Run on the desktop, as modes.c is. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INFO_GET_SCALE ((DISPLAYCONFIG_DEVICE_INFO_TYPE)-3)
#define INFO_SET_SCALE ((DISPLAYCONFIG_DEVICE_INFO_TYPE)-4)

/* The steps Settings has, in order; a display is offered a run of them. */
static const int STEPS[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350,
							 400, 450, 500 };
#define STEP_COUNT ((int)(sizeof STEPS / sizeof STEPS[0]))

/* Steps from the recommended one: the least offered, where it is, and the
 * most offered. The least is always 100%, which is what places the rest. */
typedef struct {
	DISPLAYCONFIG_DEVICE_INFO_HEADER header;
	int least, now, most;
} ScaleGet;

typedef struct {
	DISPLAYCONFIG_DEVICE_INFO_HEADER header;
	int to;
} ScaleSet;

static int percent(int step)
{
	return step >= 0 && step < STEP_COUNT ? STEPS[step] : 0;
}

int main(int argc, char **argv)
{
	UINT32 np = 0, nm = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm)
		!= ERROR_SUCCESS)
		return 1;
	DISPLAYCONFIG_PATH_INFO *paths = calloc(np ? np : 1, sizeof *paths);
	DISPLAYCONFIG_MODE_INFO *modes = calloc(nm ? nm : 1, sizeof *modes);
	if (!paths || !modes ||
		QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes,
						   NULL) != ERROR_SUCCESS)
		return 1;

	int status = 0;
	for (UINT32 i = 0; i < np; i++) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
		ScaleGet                         get;
		char                             name[64] = "?";

		memset(&src, 0, sizeof src);
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof src;
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS)
			WideCharToMultiByte(CP_UTF8, 0, src.viewGdiDeviceName, -1, name,
								sizeof name, NULL, NULL);

		memset(&get, 0, sizeof get);
		get.header.type = INFO_GET_SCALE;
		get.header.size = sizeof get;
		get.header.adapterId = src.header.adapterId;
		get.header.id = src.header.id;
		if (DisplayConfigGetDeviceInfo(&get.header) != ERROR_SUCCESS) {
			printf("%s: this Windows will not say\n", name);
			continue;
		}
		const int recommended = -get.least;
		printf("%s at %d%%, recommended %d%%, offered %d to %d%%\n", name,
			   percent(recommended + get.now), percent(recommended),
			   percent(0), percent(recommended + get.most));

		if (argc < 3 || strcmp(name, argv[1]))
			continue;
		int want = -1;
		for (int k = 0; k < STEP_COUNT; k++)
			if (STEPS[k] == atoi(argv[2]))
				want = k;
		if (want < 0 || want > recommended + get.most) {
			printf("  %s%% is not offered for it\n", argv[2]);
			status = 1;
			continue;
		}
		ScaleSet set;
		memset(&set, 0, sizeof set);
		set.header.type = INFO_SET_SCALE;
		set.header.size = sizeof set;
		set.header.adapterId = get.header.adapterId;
		set.header.id = get.header.id;
		set.to = want - recommended;
		const LONG rc = DisplayConfigSetDeviceInfo(&set.header);
		printf("  to %s%%: %s (%ld)\n", argv[2],
			   rc == ERROR_SUCCESS ? "done" : "refused", (long)rc);
		if (rc != ERROR_SUCCESS)
			status = 1;
	}
	free(paths);
	free(modes);
	return status;
}
