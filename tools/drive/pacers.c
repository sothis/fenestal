/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * pace: what a window's pacing costs, measured from inside it.
 *
 * The instrument is fenestal-dev's `pace` mode -- a caller that asks for its
 * own sizes from a thread of its own -- and what this program adds is the
 * reading: the frames it presents and the CPU it takes, asked of the port
 * (the `state` and `process` tools) instead of a second program's /proc.
 *
 *   pace [--variant v] [--backend x11|wayland] [--rates 0,60,250]
 *        [--seconds 12] [--settle 4] [--draw] [--no-vsync]
 *
 * A row is a rate: the sizes a second the program asks for, `draw` one frame
 * every turn with none asked, and 0 the floor every other row is read
 * against. The figures are the window's own over the measured stretch: what
 * it presented a second, and the share of one core it took.
 *
 * Where Python's pace.py sweeps the whole matrix -- every environment, this
 * desk's and the laptops' -- this measures the one it is run in, which is
 * the cell. tools/drive/round.c is the round beside it.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mcp_client.h"

#define RATES_MAX 16

/* A number of seconds from a word, or `dflt` where it is not one: the
 * conversion reports itself instead of reading a mistake as none. */
static double seconds_of(const char *word, double dflt)
{
	char       *end = NULL;
	const double v  = strtod(word, &end);
	if (!word || *word == '\0' || !end || *end != '\0' || v < 0.0 ||
		v > 3600.0)
		return dflt;
	return v;
}

/* The port's own connection, over the socket the window made. */
static bool connect_port(Port *p, const char *path)
{
	memset(p, 0, sizeof *p);
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	const size_t n = strlen(path);
	if (n >= sizeof addr.sun_path) {
		close(fd);
		return false;
	}
	memcpy(addr.sun_path, path, n + 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		return false;
	}
	p->rfd = p->wfd = fd;
	return rpc(p, "initialize",
			   "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
			   "\"clientInfo\":{\"name\":\"pace\",\"version\":\"1\"}}", NULL);
}

/* A figure out of the port's JSON: the number after `"key":`. */
static double num(const char *json, const char *key, double dflt)
{
	return json ? json_num_at(json, key, dflt) : dflt;
}

/* One rate: the program started, the stretch measured inside it, and what
 * the port said it did. False where the program would not start. */
static bool cell(const char *variant, const char *backend, const char *rate,
				 const char *seconds, const char *settle, const char *extra,
				 bool draw, bool no_vsync)
{
	char exe[1024];
	char self[1024];
	const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0)
		return false;
	self[n] = '\0';
	char *slash = strrchr(self, '/');
	if (!slash)
		return false;
	*slash = '\0';
	snprintf(exe, sizeof exe, "%.900s/fenestal-dev", self);

	/* The environment: the port on (the program opens its own), the backend
	 * and renderer named, the echo off -- nothing here reads the events --
	 * and the ranking off, as a round has it. */
	char env_mcp[] = "FNL_MCP=1";
	char env_backend[64], env_renderer[64];
	snprintf(env_backend, sizeof env_backend, "FNL_BACKEND=%s", backend);
	snprintf(env_renderer, sizeof env_renderer, "FNL_RENDERER=vulkan");
	static char *envp[512];
	int          ne = 0;
	for (char **e = environ; *e && ne < 400; e++) {
		if (!strncmp(*e, "FNL_MCP=", 8) || !strncmp(*e, "FNL_BACKEND=", 12) ||
			!strncmp(*e, "FNL_RENDERER=", 13) ||
			!strncmp(*e, "FNL_GPU_RANK=", 13))
			continue;
		envp[ne++] = *e;
	}
	envp[ne++] = env_mcp;
	envp[ne++] = env_backend;
	envp[ne++] = env_renderer;
	envp[ne++] = "FNL_GPU_RANK=off";
	envp[ne]   = NULL;

	char *argv[12];
	int   an = 0;
	static char pace_w[] = "pace";
	argv[an++] = exe;
	argv[an++] = pace_w;
	/* The program drives for its own seconds, which must outlast the
	 * window measured here: it is stopped, not waited for. */
	char run_for[32];
	snprintf(run_for, sizeof run_for, "%.0f",
			 seconds_of(settle, 4.0) + seconds_of(seconds, 12.0) + 10.0);
	argv[an++] = (char *)rate;
	argv[an++] = run_for;
	/* --frame-times is always on in this mode, and naming it is an unknown
	 * option there. */
	if (draw) {
		static char draw_w[] = "--draw";
		argv[an++] = draw_w;
	}
	if (no_vsync) {
		static char vsync_w[] = "--no-vsync";
		argv[an++] = vsync_w;
	}
	argv[an] = NULL;
	(void)extra;

	char log_path[512];
	snprintf(log_path, sizeof log_path, "build/drive/pace-%s-%s-%s.log",
			 backend, variant, rate);
	char dir[512];
	snprintf(dir, sizeof dir, "%s", log_path);
	char *d = strrchr(dir, '/');
	if (d) {
		*d = '\0';
		mkdir(dir, S_IRWXU);
	}

	Prog pr;
	memset(&pr, 0, sizeof pr);
	prog_start(&pr, argv, envp, log_path);
	prog_sockets(&pr);
	const double wait_end = now_s() + 60.0;
	bool         up       = false;
	while (now_s() < wait_end && !up) {
		up = prog_wait_socket(&pr, 1, 0.2);
		if (!up) {
			int status = 0;
			if (waitpid(pr.pid, &status, WNOHANG) == pr.pid) {
				pr.ended = true;
				break;
			}
		}
	}
	if (!up) {
		if (!pr.ended)
			kill(pr.pid, SIGTERM);
		waitpid(pr.pid, NULL, 0);
		printf("%-8s %s\n", rate, "no port: the program did not open one");
		return false;
	}

	Port p;
	bool connected = false;
	const double conn_end = now_s() + 10.0;
	while (now_s() < conn_end && !connected) {
		connected = connect_port(&p, pr.sock);
		if (!connected)
			sleep_s(0.05);
	}
	if (!connected) {
		kill(pr.pid, SIGTERM);
		waitpid(pr.pid, NULL, 0);
		printf("%-8s %s\n", rate, "no port: nothing answered");
		return false;
	}

	/* The settle, then the stretch: the CPU share is what the process took
	 * between two asks, so the first is taken where the stretch starts. */
	const double settle_s = seconds_of(settle, 4.0);
	sleep_s(settle_s + 1.0);
	mcp_call(&p, "process", "{}");          /* the clock starts here */
	mcp_call(&p, "state", "{\"live\":true}");
	const double t0 = now_s();
	sleep_s(seconds_of(seconds, 12.0));
	const double span = now_s() - t0;
	/* Each reply is read before the next call: both answer into the port's
	 * one buffer. */
	const char  *proc = mcp_call(&p, "process", "{}");
	const double cpu  = num(proc, "cpu_pm", -1) / 10.0;
	const char  *st   = mcp_call(&p, "state", "{\"live\":true}");
	const double fps  = num(st, "fps", -1);
	printf("%-8s %8.1f fps %6.1f %%\n", rate, fps, cpu);
	fflush(stdout);
	(void)span;

	port_close(&p);
	str_free(&p.text);
	str_free(&p.image);
	str_free(&p.buf);
	/* The program ends itself when its seconds are up; give it that. */
	const double end = now_s() + 10.0;
	while (now_s() < end && !pr.ended) {
		int status = 0;
		if (waitpid(pr.pid, &status, WNOHANG) == pr.pid)
			pr.ended = true;
		else
			sleep_s(0.05);
	}
	if (!pr.ended) {
		kill(pr.pid, SIGTERM);
		waitpid(pr.pid, NULL, 0);
	}
	return true;
}

int main(int argc, char **argv)
{
	const char *variant  = "release";
	const char *backend  = "x11";
	const char *rates    = "0,60,250";
	const char *seconds  = "12";
	const char *settle   = "4";
	bool        draw     = false;
	bool        no_vsync = false;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--variant") && i + 1 < argc)
			variant = argv[++i];
		else if (!strcmp(a, "--backend") && i + 1 < argc)
			backend = argv[++i];
		else if (!strcmp(a, "--rates") && i + 1 < argc)
			rates = argv[++i];
		else if (!strcmp(a, "--seconds") && i + 1 < argc)
			seconds = argv[++i];
		else if (!strcmp(a, "--settle") && i + 1 < argc)
			settle = argv[++i];
		else if (!strcmp(a, "--draw"))
			draw = true;
		else if (!strcmp(a, "--no-vsync"))
			no_vsync = true;
		else
			fprintf(stderr,
					"pace: %s is not an option (--variant, --backend, --rates, "
					"--seconds, --settle, --draw, --no-vsync)\n", a);
	}

	if (draw)
		rates = "draw";
	printf("%-8s %s\n", "rate", "measured");
	char work[256];
	snprintf(work, sizeof work, "%s", rates);
	char *rate_list[RATES_MAX + 1];
	const int n = words(work, rate_list, RATES_MAX);
	for (int i = 0; i < n; i++) {
		char rate[32];
		snprintf(rate, sizeof rate, "%s", rate_list[i]);
		/* A comma is the separator the command line uses. */
		char *comma = strchr(rate, ',');
		while (comma) {
			*comma = '\0';
			cell(variant, backend, rate, seconds, settle, "", draw,
				 no_vsync);
			memmove(rate, comma + 1, strlen(comma + 1) + 1);
			comma = strchr(rate, ',');
		}
		cell(variant, backend, rate, seconds, settle, "", draw, no_vsync);
	}
	return 0;
}
