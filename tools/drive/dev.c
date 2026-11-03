/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * fenestal-dev -- the development instruments, one program with a mode each.
 *
 *   fenestal-dev pace [<sizes/s> [<seconds>]] [options]     (pace.c)
 *   fenestal-dev lifecycle [<cycles>]                       (lifecycle.c)
 *
 * Neither draws a scene, and both are consumers of the installed header
 * alone, so linking this proves two things about the archive at once: the
 * plain-window half of its link line, with no graphics API in the program,
 * and that it exports only the API -- lifecycle.c defines functions named as
 * the library's internals are. One program and not two because they share
 * those proofs and nothing else; each mode stays a file of its own, its
 * main() under its mode's name. Built into every tree; nothing in the build
 * runs it. A console program on Windows: the output of either mode is its
 * whole result.
 */

#include "dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
	const char *name;
	int (*run)(int argc, char **argv);
	const char *what;
} MODES[] = {
	{ "pace",      pace_main,
	  "a window resized from a thread of its own, its frames counted" },
	{ "lifecycle", lifecycle_main,
	  "the API called wrongly, then many lifetimes, what each holds" },
};
#define MODE_COUNT (sizeof MODES / sizeof MODES[0])

static void usage(FILE *to)
{
	fprintf(to, "fenestal-dev <mode> [arguments]; <mode> --help for its "
				"own\n\n");
	for (size_t i = 0; i < MODE_COUNT; i++)
		fprintf(to, "  %-10s %s\n", MODES[i].name, MODES[i].what);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return EXIT_SUCCESS;
	}
	for (size_t i = 0; i < MODE_COUNT; i++)
		if (!strcmp(argv[1], MODES[i].name))
			return MODES[i].run(argc - 1, argv + 1);
	fprintf(stderr, "fenestal-dev: no mode '%s'\n\n", argv[1]);
	usage(stderr);
	return EXIT_FAILURE;
}
