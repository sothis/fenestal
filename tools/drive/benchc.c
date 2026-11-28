/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * bench: one environment's benchmark run, brought home.
 *
 *   bench [--variant v] [--backend x11|wayland] [--out dir] [--label name]
 *         [--seconds s] [--] [--bench-<key>=<value> ...]
 *
 * The testbed's own benchmark mode does the measuring (DESIGN *Benchmarking*);
 * this starts it with --bench-emit, so each record arrives on stderr between
 * the two marker lines, keeps the program's log beside the record, writes
 * each record where the reports look for them and puts a line in the ledger.
 * Arguments after `--` go to the program as they are.
 *
 * Where bench.py drives every environment in turn, this is the one it is run
 * in: the cell, as pace and round are their own.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mcp_client.h"

#define BEGIN "==== fenestal-bench/1 begin "
#define END   "==== fenestal-bench/1 end "

/* The record files' directory: FNL_BENCH_OUT, --out, or the state
 * directory's bench/, as bench.py keeps them. */
static void out_dir(char *path, size_t n, const char *given)
{
	if (given) {
		snprintf(path, n, "%s", given);
		return;
	}
	const char *env = getenv("FNL_BENCH_OUT");
	if (env && *env) {
		snprintf(path, n, "%s", env);
		return;
	}
	const char *state = getenv("XDG_STATE_HOME");
	const char *home  = getenv("HOME");
	if (state && *state)
		snprintf(path, n, "%.200s/fenestal/bench", state);
	else if (home)
		snprintf(path, n, "%.200s/.local/state/fenestal/bench", home);
	else
		snprintf(path, n, ".");
}

/* One record, written where the reports read them: the name says when it was
 * taken, where, and what it was called. */
static int write_record(const char *dir, const char *label, const char *json,
						size_t n)
{
	char stamp[32];
	time_t t = time(NULL);
	struct tm tmv;
	gmtime_r(&t, &tmv);
	strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tmv);
	char path[1024];
	snprintf(path, sizeof path, "%.800s/bench-%s-%s.json", dir, stamp,
			 *label ? label : "run");
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "bench: cannot write %s: %s\n", path,
				strerror(errno));
		return -1;
	}
	fwrite(json, 1, n, f);
	fputc('\n', f);
	fclose(f);
	printf("record: %s\n", path);
	return 0;
}

/* The records of a log, written out: each one is the JSON between the two
 * markers, which the program wrote whole. */
static int records_from(const char *log_path, const char *dir,
						const char *label)
{
	FILE *f = fopen(log_path, "r");
	if (!f) {
		fprintf(stderr, "bench: no log at %s\n", log_path);
		return 0;
	}
	char  line[8192];
	Str   cur = { 0 };
	bool  in  = false;
	int   n   = 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, BEGIN, strlen(BEGIN))) {
			in    = true;
			cur.n = 0;
			if (cur.p)
				cur.p[0] = '\0';
			continue;
		}
		if (!strncmp(line, END, strlen(END))) {
			if (in && cur.n && write_record(dir, label, cur.p ? cur.p : "",
										   cur.n) == 0)
				n++;
			in = false;
			continue;
		}
		if (in)
			str_add(&cur, line, strlen(line));
	}
	fclose(f);
	str_free(&cur);
	return n;
}

int main(int argc, char **argv)
{
	const char *variant  = "release";
	const char *backend  = "x11";
	const char *label    = "";
	const char *out      = NULL;
	const char *seconds  = "20";
	char       *passthru[32];
	int         np = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--")) {
			for (int j = i + 1; j < argc && np < 31; j++)
				passthru[np++] = argv[j];
			break;
		} else if (!strcmp(a, "--variant") && i + 1 < argc) {
			variant = argv[++i];
		} else if (!strcmp(a, "--backend") && i + 1 < argc) {
			backend = argv[++i];
		} else if (!strcmp(a, "--out") && i + 1 < argc) {
			out = argv[++i];
		} else if (!strcmp(a, "--label") && i + 1 < argc) {
			label = argv[++i];
		} else if (!strcmp(a, "--seconds") && i + 1 < argc) {
			seconds = argv[++i];
		} else if (!strncmp(a, "--bench-", 8)) {
			passthru[np++] = argv[i];
		} else {
			fprintf(stderr, "bench: %s is not an option (--variant, "
							"--backend, --out, --label, --seconds, --, "
							"--bench-*)\n", a);
			return 2;
		}
	}

	char exe[1024];
	char self[1024];
	const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0) {
		fprintf(stderr, "bench: cannot find this program's own path\n");
		return 2;
	}
	self[n] = '\0';
	char *slash = strrchr(self, '/');
	if (!slash) {
		fprintf(stderr, "bench: cannot find this program's directory\n");
		return 2;
	}
	*slash = '\0';
	snprintf(exe, sizeof exe, "%.900s/fenestal-testbed", self);

	static char run_w[] = "--bench-run";
	static char quit_w[] = "--bench-quit";
	static char emit_w[] = "--bench-emit";
	/* One token: the program's options take their value after an `=`. */
	char sec_val[64];
	snprintf(sec_val, sizeof sec_val, "--bench-measure-s=%.20s", seconds);

	char *argv_prog[48];
	int   an = 0;
	argv_prog[an++] = exe;
	argv_prog[an++] = run_w;
	argv_prog[an++] = quit_w;
	argv_prog[an++] = emit_w;
	argv_prog[an++] = sec_val;
	for (int i = 0; i < np; i++)
		argv_prog[an++] = passthru[i];
	argv_prog[an] = NULL;

	/* The environment: the backend named, the port off (nothing here speaks
	 * it), the ranking left as the run asks. */
	char env_off[] = "FNL_MCP=0";
	char env_backend[64];
	snprintf(env_backend, sizeof env_backend, "FNL_BACKEND=%s", backend);
	static char *envp[512];
	int          ne = 0;
	for (char **e = environ; *e && ne < 400; e++) {
		if (!strncmp(*e, "FNL_MCP=", 8) || !strncmp(*e, "FNL_BACKEND=", 12))
			continue;
		envp[ne++] = *e;
	}
	envp[ne++] = env_off;
	envp[ne++] = env_backend;
	envp[ne]   = NULL;

	char dir[512];
	out_dir(dir, sizeof dir, out);
	char mk[600];
	snprintf(mk, sizeof mk, "mkdir -p %.500s 2>/dev/null", dir);
	Str  o  = { 0 };
	int  st = -1;
	sh_run(mk, &o, &st);
	str_free(&o);

	char log_path[512];
	snprintf(log_path, sizeof log_path, "build/drive/bench-%s-%s.log", backend,
			 variant);
	char logdir[512];
	snprintf(logdir, sizeof logdir, "%s", log_path);
	char *d = strrchr(logdir, '/');
	if (d) {
		*d = '\0';
		mkdir(logdir, S_IRWXU);
	}

	Prog pr;
	memset(&pr, 0, sizeof pr);
	prog_start(&pr, argv_prog, envp, log_path);
	int status = 0;
	while (waitpid(pr.pid, &status, 0) < 0 && errno == EINTR)
		;
	pr.ended = true;
	pr.rc    = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

	lines_read(&pr);
	const int wrote = records_from(log_path, dir, label);
	if (!wrote) {
		printf("no record: the program said none (rc %d)\n", pr.rc);
		return 1;
	}
	printf("%d record(s)\n", wrote);
	return 0;
}
