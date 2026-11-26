/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * fenestal-mcp: stdin and stdout to the MCP port's socket (src/core/mcp.c),
 * for a client that starts a command -- an MCP client's `command` -- on a
 * machine with no socat, which is Windows. It is socat - UNIX-CONNECT:<path>
 * and nothing more, except for the path:
 *
 *   fenestal-mcp [<path>]
 *
 * With none, the newest fenestal-*.sock in the directory the port makes them
 * in ($XDG_RUNTIME_DIR, $TMPDIR, /tmp; %TEMP% on Windows) that something
 * answers on, which is the one program running with FNL_MCP=1 there. A line
 * is a message in both directions, so bytes are passed as they come.
 */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
typedef SOCKET Sock;
#define SOCK_NONE INVALID_SOCKET
#else
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_NONE (-1)
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Sock dial(const char *path)
{
	struct sockaddr_un a;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof a.sun_path)
		return SOCK_NONE;
	strcpy(a.sun_path, path);
	const Sock s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == SOCK_NONE)
		return SOCK_NONE;
	if (connect(s, (const struct sockaddr *)&a, sizeof a) != 0) {
#if defined(_WIN32)
		closesocket(s);
#else
		close(s);
#endif
		return SOCK_NONE;
	}
	return s;
}

/* The newest fenestal-*.sock that answers, connected; the path in `out`. */
static Sock find(char *out, size_t cap)
{
#if defined(_WIN32)
	static const char *const VARS[] = { "TEMP", "TMP" };
	static const char SEP = '\\';
#else
	static const char *const VARS[] = { "XDG_RUNTIME_DIR", "TMPDIR" };
	static const char SEP = '/';
#endif
	const char *dir = NULL;
	for (size_t i = 0; i < sizeof VARS / sizeof VARS[0] && !dir; i++) {
		const char *v = getenv(VARS[i]);
		if (v && *v)
			dir = v;
	}
	if (!dir)
#if defined(_WIN32)
		dir = ".";
#else
		dir = "/tmp";
#endif

	Sock best = SOCK_NONE;
	char path[512];
#if defined(_WIN32)
	FILETIME best_t = { 0, 0 };
	WIN32_FIND_DATAA fd;
	snprintf(path, sizeof path, "%s%cfenestal-*.sock", dir, SEP);
	HANDLE h = FindFirstFileA(path, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return SOCK_NONE;
	do {
		snprintf(path, sizeof path, "%s%c%s", dir, SEP, fd.cFileName);
		if (best != SOCK_NONE &&
			CompareFileTime(&fd.ftLastWriteTime, &best_t) <= 0)
			continue;
		const Sock s = dial(path);
		if (s == SOCK_NONE)
			continue;
		if (best != SOCK_NONE)
			closesocket(best);
		best   = s;
		best_t = fd.ftLastWriteTime;
		snprintf(out, cap, "%s", path);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	time_t best_t = 0;
	DIR *d = opendir(dir);
	if (!d)
		return SOCK_NONE;
	for (struct dirent *e; (e = readdir(d));) {
		struct stat st;
		if (strncmp(e->d_name, "fenestal-", 9) != 0 ||
			!strstr(e->d_name, ".sock"))
			continue;
		snprintf(path, sizeof path, "%s%c%s", dir, SEP, e->d_name);
		if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode) ||
			(best != SOCK_NONE && st.st_mtime <= best_t))
			continue;
		const Sock s = dial(path);
		if (s == SOCK_NONE)
			continue;
		if (best != SOCK_NONE)
			close(best);
		best   = s;
		best_t = st.st_mtime;
		snprintf(out, cap, "%s", path);
	}
	closedir(d);
#endif
	return best;
}

#if defined(_WIN32)

/* stdin to the socket on a thread of its own, the socket to stdout here:
 * Windows has no poll for a pipe. */
static DWORD WINAPI up(void *arg)
{
	const Sock s = *(const Sock *)arg;
	char buf[4096];
	DWORD n;
	while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof buf, &n, NULL) &&
		   n) {
		for (DWORD off = 0; off < n;) {
			const int w = send(s, buf + off, (int)(n - off), 0);
			if (w <= 0)
				return 0;
			off += (DWORD)w;
		}
	}
	shutdown(s, SD_SEND);
	return 0;
}

static int pump(Sock s)
{
	HANDLE t = CreateThread(NULL, 0, up, &s, 0, NULL);
	char buf[4096];
	int n;
	while ((n = recv(s, buf, sizeof buf, 0)) > 0) {
		DWORD done = 0;
		for (DWORD off = 0; off < (DWORD)n; off += done)
			if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf + off,
						   (DWORD)n - off, &done, NULL))
				return 1;
	}
	(void)t;
	return 0;
}

#else

static int pump(Sock s)
{
	struct pollfd pf[2] = { { .fd = 0, .events = POLLIN },
							{ .fd = s, .events = POLLIN } };
	char buf[4096];
	bool in_open = true;
	for (;;) {
		pf[0].fd = in_open ? 0 : -1;
		if (poll(pf, 2, -1) < 0)
			return 1;
		if (pf[0].revents) {
			const ssize_t n = read(0, buf, sizeof buf);
			if (n <= 0) {
				/* The client is done: what it asked is still answered. */
				shutdown(s, SHUT_WR);
				in_open = false;
			}
			for (ssize_t off = 0; n > 0 && off < n;) {
				const ssize_t w = write(s, buf + off, (size_t)(n - off));
				if (w <= 0)
					return 1;
				off += w;
			}
		}
		if (pf[1].revents) {
			const ssize_t n = read(s, buf, sizeof buf);
			if (n <= 0)
				return 0;
			for (ssize_t off = 0; off < n;) {
				const ssize_t w = write(1, buf + off, (size_t)(n - off));
				if (w <= 0)
					return 1;
				off += w;
			}
		}
	}
}

#endif

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
		fprintf(stderr, "usage: fenestal-mcp [<socket path>]\n");
		return 2;
	}
#if defined(_WIN32)
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "fenestal-mcp: no sockets\n");
		return 1;
	}
#endif
	char found[512] = "";
	const Sock s = argc == 2 ? dial(argv[1]) : find(found, sizeof found);
	if (s == SOCK_NONE) {
		fprintf(stderr,
				argc == 2 ? "fenestal-mcp: nothing answers on %s\n"
						  : "fenestal-mcp: no program with FNL_MCP=1 is "
							"listening%s\n",
				argc == 2 ? argv[1] : "");
		return 1;
	}
	return pump(s);
}
