/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The driver for a harness built without libFuzzer: every file named on the
 * command line, or every file in a directory named there, once as it is and
 * then -runs=N damaged copies, chosen by a fixed seed so that a build that
 * fails fails again the same way.
 *
 *   fuzz_keymap tools/fuzz/corpus/keymap -runs=20000
 *
 * The damage is what DESIGN.md's campaigns used: flipped bits, a copy cut
 * short, arbitrary bytes run on or written over, and two inputs spliced.
 * Not coverage-guided -- that is libFuzzer's or afl++'s job, in a campaign;
 * this is a regression net that runs in seconds on every build.
 */

#include "core/app.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "fuzz.h"

#define MAX_INPUTS 256
#define MAX_INPUT  (1u << 20)

typedef struct {
	unsigned char *data;
	size_t         len;
} Input;

static Input  inputs[MAX_INPUTS];
static size_t input_count;

static uint64_t rng = 0x9e3779b97f4a7c15u;

static uint32_t next_rand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 32);
}

static int load(const char *path)
{
	if (input_count == MAX_INPUTS) {
		fprintf(stderr, "replay: more than %d inputs\n", MAX_INPUTS);
		return 0;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "replay: cannot open %s: %s\n", path, strerror(errno));
		return 0;
	}
	/* One byte over the limit, so that an input cut to fit is refused
	 * rather than replayed as a shorter one nobody wrote. */
	unsigned char *buf = malloc(MAX_INPUT + 1);
	if (!buf) {
		fclose(f);
		fprintf(stderr, "replay: out of memory reading %s\n", path);
		return 0;
	}
	size_t n = fread(buf, 1, MAX_INPUT + 1, f);
	int bad = ferror(f);
	fclose(f);
	if (bad || n > MAX_INPUT) {
		fprintf(stderr, bad ? "replay: cannot read %s\n"
							: "replay: %s is over the 1 MiB an input may be\n",
				path);
		free(buf);
		return 0;
	}
	inputs[input_count].data = buf;
	inputs[input_count].len  = n;
	input_count++;
	return 1;
}

static int load_path(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "replay: no such input %s\n", path);
		return 0;
	}
	if (!S_ISDIR(st.st_mode))
		return load(path);

	/* Sorted, so that the seed meets the inputs in the same order on every
	 * machine; readdir's order is the filesystem's. */
	struct dirent **list;
	int n = scandir(path, &list, NULL, alphasort);
	if (n < 0) {
		fprintf(stderr, "replay: cannot list %s: %s\n", path, strerror(errno));
		return 0;
	}
	int ok = 1;
	for (int i = 0; i < n; i++) {
		if (list[i]->d_name[0] != '.') {
			char full[4096];
			snprintf(full, sizeof full, "%s/%s", path, list[i]->d_name);
			ok = ok && load(full);
		}
		free(list[i]);
	}
	free(list);
	return ok;
}

/* One call, from an allocation of exactly `len` bytes. */
static void run(const unsigned char *src, size_t len)
{
	unsigned char *copy = malloc(len ? len : 1);
	if (!copy)
		return;
	if (len)
		memcpy(copy, src, len);
	LLVMFuzzerTestOneInput(copy, len);
	free(copy);
}

static size_t damage(unsigned char *buf, size_t len, size_t cap)
{
	const Input *other = &inputs[next_rand() % input_count];
	switch (next_rand() % 5) {
	case 0:                                     /* flip one to eight bits */
		for (unsigned k = 1 + next_rand() % 8; k && len; k--)
			buf[next_rand() % len] ^= (unsigned char)(1u << (next_rand() % 8));
		break;
	case 1:                                     /* cut short */
		if (len)
			len = next_rand() % len;
		break;
	case 2:                                     /* run on */
		for (unsigned k = next_rand() % 64; k && len < cap; k--)
			buf[len++] = (unsigned char)next_rand();
		break;
	case 3:                                     /* overwrite a run */
		if (len) {
			size_t at = next_rand() % len;
			for (unsigned k = next_rand() % 16; k && at < len; k--)
				buf[at++] = (unsigned char)next_rand();
		}
		break;
	default:                                    /* splice another's tail */
		if (other->len) {
			size_t from = next_rand() % other->len;
			size_t at   = len ? next_rand() % len : 0;
			size_t n    = other->len - from;
			if (n > cap - at)
				n = cap - at;
			memcpy(buf + at, other->data + from, n);
			len = at + n;
		}
		break;
	}
	return len;
}

int main(int argc, char **argv)
{
	unsigned long runs = 0;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "-runs=", 6) == 0)
			runs = strtoul(argv[i] + 6, NULL, 10);
		else if (!load_path(argv[i]))
			return 1;
	}
	if (input_count == 0) {
		fprintf(stderr, "usage: %s <corpus dir or file>... [-runs=N]\n",
				argv[0]);
		return 1;
	}

	for (size_t i = 0; i < input_count; i++)
		run(inputs[i].data, inputs[i].len);

	unsigned char *buf = malloc(MAX_INPUT);
	if (!buf) {
		fprintf(stderr, "replay: out of memory\n");
		return 1;
	}
	for (unsigned long r = 0; r < runs; r++) {
		const Input *in = &inputs[next_rand() % input_count];
		size_t len = in->len;
		memcpy(buf, in->data, len);
		for (unsigned k = 1 + next_rand() % 3; k; k--)
			len = damage(buf, len, MAX_INPUT);
		run(buf, len);
	}
	free(buf);
	for (size_t i = 0; i < input_count; i++)
		free(inputs[i].data);
	return 0;
}
