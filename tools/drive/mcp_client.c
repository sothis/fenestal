/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The client side of the MCP port, shared by the tools that drive a
 * program: mcp_client.h says what it is for.
 */

#include "mcp_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIE(...)                                                              \
	do {                                                                      \
		fprintf(stderr, "round: " __VA_ARGS__);                               \
		fputc('\n', stderr);                                                  \
		exit(2);                                                              \
	} while (0)

/* ---- small things -------------------------------------------------------- */

double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void sleep_s(double s)
{
	if (s <= 0.0)
		return;
	struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
	nanosleep(&ts, NULL);
}


static void str_need(Str *s, size_t more)
{
	if (s->n + more + 1 <= s->cap)
		return;
	size_t cap = s->cap ? s->cap : 256;
	while (cap < s->n + more + 1)
		cap *= 2;
	char *p = realloc(s->p, cap);
	if (!p)
		DIE("out of memory");
	s->p   = p;
	s->cap = cap;
}

void str_add(Str *s, const char *p, size_t n)
{
	str_need(s, n);
	memcpy(s->p + s->n, p, n);
	s->n += n;
	s->p[s->n] = '\0';
}

void str_adds(Str *s, const char *p)
{
	str_add(s, p, strlen(p));
}

void str_addc(Str *s, char c)
{
	str_add(s, &c, 1);
}

void str_addf(Str *s, const char *fmt, ...)
{
	char    buf[1024];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0)
		str_add(s, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

void str_free(Str *s)
{
	free(s->p);
	s->p = NULL;
	s->n = s->cap = 0;
}

/* A JSON string's contents, with the escapes a name, a title or a label can
 * carry put back. `p` points at the opening quote. Returns the end, past the
 * closing one, or NULL where the string never closes. */
const char *json_unescape(const char *p, Str *out)
{
	if (*p != '"')
		return NULL;
	p++;
	while (*p && *p != '"') {
		if (*p != '\\') {
			str_addc(out, *p++);
			continue;
		}
		p++;
		switch (*p) {
		case 'n': str_addc(out, '\n'); p++; break;
		case 't': str_addc(out, '\t'); p++; break;
		case 'r': str_addc(out, '\r'); p++; break;
		case 'b': str_addc(out, '\b'); p++; break;
		case 'f': str_addc(out, '\f'); p++; break;
		case '"': str_addc(out, '"'); p++; break;
		case '\\': str_addc(out, '\\'); p++; break;
		case '/': str_addc(out, '/'); p++; break;
		case 'u': {
			/* Only what a name or a label can hold is put back as UTF-8:
			 * the port escapes control characters and quotes, and sends
			 * everything else as it is. */
			if (!p[1] || !p[2] || !p[3] || !p[4])
				return NULL;
			char hex[5] = { p[1], p[2], p[3], p[4], 0 };
			const unsigned v = (unsigned)strtoul(hex, NULL, 16);
			p += 5;
			if (v < 0x80) {
				str_addc(out, (char)v);
			} else if (v < 0x800) {
				str_addc(out, (char)(0xC0 | (v >> 6)));
				str_addc(out, (char)(0x80 | (v & 0x3F)));
			} else {
				str_addc(out, (char)(0xE0 | (v >> 12)));
				str_addc(out, (char)(0x80 | ((v >> 6) & 0x3F)));
				str_addc(out, (char)(0x80 | (v & 0x3F)));
			}
			break;
		}
		default: return NULL;
		}
	}
	if (*p != '"')
		return NULL;
	return p + 1;
}

/* ---- the port ------------------------------------------------------------ */

int  g_timeout_s = 5;
bool g_quiet;
/* A command that speaks the port's framing on its standard input and output,
 * for a window this program cannot connect to directly: `ssh host
 * fenestal-mcp`, or socat for a socket. */
const char *g_command;
/* Where the program runs: here, or on a machine `ssh` reaches. The port of a
 * remote window is `ssh <host> <dir>/fenestal-mcp`, whose stdin and stdout
 * carry the framing, and its log is fetched when the round is over. */
const char *g_ssh;
const char *g_remote_dir;


/* The text of the last reply, or "" where it had none. */
/* One JSON-RPC exchange: the line written, one line read back. */
bool rpc(Port *p, const char *method, const char *params, Str *reply)
{
	Str q = { 0 };
	str_addf(&q, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\"", ++p->next_id,
			 method);
	if (params)
		str_addf(&q, ",\"params\":%s", params);
	str_adds(&q, "}\n");
	size_t off = 0;
	while (off < q.n) {
		const ssize_t n = write(p->wfd, q.p + off, q.n - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			str_free(&q);
			return false;
		}
		off += (size_t)n;
	}
	str_free(&q);

	p->buf.n = 0;
	bool got = false;
	const double end = now_s() + (double)g_timeout_s *
									  (p->next_id > 1 ? 3.0 : 1.0);
	while (now_s() < end && !got) {
		struct pollfd pfd = { p->rfd, POLLIN, 0 };
		const int     left = (int)((end - now_s()) * 1000.0) + 1;
		const int     pr   = poll(&pfd, 1, left);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (pr == 0)
			return false;
		char    chunk[4096];
		ssize_t n = read(p->rfd, chunk, sizeof chunk);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return false;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (chunk[i] == '\n') {
				got = true;
				break;
			}
			str_addc(&p->buf, chunk[i]);
		}
	}
	if (!got)
		return false;
	if (reply)
		str_add(reply, p->buf.p ? p->buf.p : "", p->buf.n);
	return true;
}

/* `"key":"value"` in a JSON object, unescaped into `out`. The scan is the
 * whole of this program's JSON reading: what the port answers is one object
 * of known shape, and a parser for all of it would be a second one in the
 * tree. Returns false where the key is not there. */
bool json_str_at(const char *obj, const char *key, Str *out)
{
	Str pat = { 0 };
	str_addf(&pat, "\"%s\":\"", key);
	const char *at = strstr(obj, pat.p);
	str_free(&pat);
	if (!at)
		return false;
	at += strlen("\"") + strlen(key) + strlen("\":");
	out->n = 0;
	return json_unescape(at, out) != NULL;
}

/* A number's text for `"key":` -- an integer, a float or -1. */
double json_num_at(const char *obj, const char *key, double dflt)
{
	Str pat = { 0 };
	str_addf(&pat, "\"%s\":", key);
	const char *at = strstr(obj, pat.p);
	str_free(&pat);
	if (!at)
		return dflt;
	at += strlen("\"") + strlen(key) + strlen("\":");
	return strtod(at, NULL);
}

bool json_bool_at(const char *obj, const char *key, bool dflt)
{
	Str pat = { 0 };
	str_addf(&pat, "\"%s\":", key);
	const char *at = strstr(obj, pat.p);
	str_free(&pat);
	if (!at)
		return dflt;
	at += strlen("\"") + strlen(key) + strlen("\":");
	if (!strncmp(at, "true", 4))
		return true;
	if (!strncmp(at, "false", 5))
		return false;
	return dflt;
}

/* The `tools/call` exchange: the first text item's contents, unescaped, into
 * p->text and returned; the image item, if any, into p->image (still
 * base64). NULL where the call failed or the port said so. */
const char *mcp_call(Port *p, const char *tool, const char *args_json)
{
	Str params = { 0 };
	str_addf(&params, "{\"name\":\"%s\",\"arguments\":%s}", tool,
			 args_json ? args_json : "{}");
	Str reply = { 0 };
	const bool ok = rpc(p, "tools/call", params.p, &reply);
	str_free(&params);
	p->text.n = 0;
	p->image.n = 0;
	p->err = true;
	if (!ok) {
		str_free(&reply);
		return NULL;
	}
	/* "result":{"content":[{"type":"text","text":"..."},{"type":"image",
	 * "data":"..."}],"isError":false} -- the strings unescaped as they are
	 * found, in the order the port writes them. */
	const char *at = strstr(reply.p, "\"content\":[");
	if (at && json_str_at(at, "text", &p->text)) {
		/* Nothing: the text is unescaped in place, which the base64 that
		 * may follow does not disturb. */
	}
	json_str_at(reply.p, "data", &p->image);
	p->err = json_bool_at(reply.p, "isError", false);
	str_free(&reply);
	return p->err ? NULL : p->text.p;
}

/* `call` with the arguments object written from a format. */
const char *ask(Port *p, const char *tool, const char *fmt, ...)
{
	char    args[512];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(args, sizeof args, fmt, ap);
	va_end(ap);
	return mcp_call(p, tool, n > 0 ? args : "{}");
}

/* The words of `text`, split on spaces, into `out`: what execvp() wants from
 * one string. `buf` is the caller's copy, which is what the pointers point
 * into -- the words are not copied twice. */
int words(char *buf, char **out, int max)
{
	int n = 0;
	char *p = buf;
	while (*p && n < max) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		out[n++] = p;
		while (*p && *p != ' ')
			p++;
		if (*p)
			*p++ = '\0';
	}
	out[n] = NULL;
	return n;
}

/* A whole number from a word, or `dflt` where it is not one. */
int whole(const char *word, int dflt)
{
	char *end = NULL;
	const long v = strtol(word, &end, 10);
	if (!word || *word == '\0' || (end && *end != '\0') || v < 0 ||
		v > 3600)
		return dflt;
	return (int)v;
}

/* A command run for what it says: its standard output, whole, into `out`.
 * False where it could not be run; `*status` is the exit status or -1. */
void dbg(const char *fmt, ...)
{
	if (!getenv("FNL_ROUND_DEBUG"))
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("round: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

bool sh_run(const char *cmd, Str *out, int *status)
{
	dbg("run: %s", cmd);
	FILE *f = popen(cmd, "r");
	if (!f)
		return false;
	char buf[1024];
	size_t n;
	out->n = 0;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		str_add(out, buf, n);
	if (out->p)
		out->p[out->n] = '\0';
	const int rc = pclose(f);
	*status      = rc < 0 ? -1 : WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
	return true;
}

bool remote_gone(const char *host, long pid)
{
	Str  out = { 0 };
	char cmd[512];
	int  status = -1;
	snprintf(cmd, sizeof cmd, "ssh -o BatchMode=yes %.200s 'kill -0 %ld'", host,
			 pid);
	const bool ok = sh_run(cmd, &out, &status);
	str_free(&out);
	return ok && status != 0;
}

/* A port over a command's standard input and output: `ssh host
 * fenestal-mcp` is how another machine's window is reached, and `socat -`
 * how a socket is, where a socket is not connectable directly. The command
 * is this program's child, and its two pipes are the port's ends. */
void port_spawn(Port *p, char *const argv[])
{
	int in[2], out[2];
	if (pipe(in) != 0 || pipe(out) != 0)
		DIE("pipe: %s", strerror(errno));
	const pid_t pid = fork();
	if (pid < 0)
		DIE("fork: %s", strerror(errno));
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	p->wfd = in[1];
	p->rfd = out[0];
	p->cmd = pid;
}

void port_close(Port *p)
{
	if (p->cmd > 0) {
		kill(p->cmd, SIGTERM);
		waitpid(p->cmd, NULL, 0);
	} else {
		if (p->rfd > 0 && p->rfd == p->wfd)
			close(p->rfd);
	}
	if (p->rfd > 0 && p->rfd != p->wfd)
		close(p->rfd);
	if (p->wfd > 0 && p->rfd != p->wfd)
		close(p->wfd);
	p->rfd = p->wfd = -1;
}

/* ---- the program --------------------------------------------------------- */

#define SOCK_MAX 512


void lines_read(Prog *pr)
{
	FILE *f = fopen(pr->log_path, "r");
	if (!f)
		return;
	pr->lines.n = 0;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		str_add(&pr->lines, buf, n);
	if (pr->lines.p)
		pr->lines.p[pr->lines.n] = '\0';
	fclose(f);
}

const char *socket_dir(void)
{
	const char *d = getenv("XDG_RUNTIME_DIR");
	if (!d || !*d)
		d = getenv("TMPDIR");
	if (!d || !*d)
		d = "/tmp";
	return d;
}

void prog_sockets(Prog *pr)
{
	snprintf(pr->base, sizeof pr->base, "%s/fenestal-%ld", socket_dir(),
			 (long)pr->pid);
	/* Shorter than the base by the suffix, so the two fit together. */
	snprintf(pr->sock, sizeof pr->sock, "%.500s.sock", pr->base);
}

/* Wait for the nth window's port to appear. */
bool prog_wait_socket(Prog *pr, int n, double seconds)
{
	char path[SOCK_MAX];
	if (n <= 1)
		snprintf(path, sizeof path, "%s", pr->sock);
	else
		snprintf(path, sizeof path, "%.490s-%d.sock", pr->base, n);
	const double end = now_s() + seconds;
	while (now_s() < end) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
			return true;
		int status = 0;
		if (waitpid(pr->pid, &status, WNOHANG) == pr->pid) {
			pr->ended = true;
			pr->rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			return false;
		}
		sleep_s(0.02);
	}
	return false;
}

void prog_start(Prog *pr, char *const argv[], char *const envp[],
					   const char *log_path)
{
	snprintf(pr->log_path, sizeof pr->log_path, "%.500s", log_path);
	pr->pid = fork();
	if (pr->pid < 0)
		DIE("fork: %s", strerror(errno));
	if (pr->pid == 0) {
		/* Its own log file, not a pipe this program has to drain: a
		 * program that fills a pipe nobody reads stops drawing, and the
		 * round's own thread is the one that would have to read it. */
		const int out = open(log_path, O_WRONLY | O_CREAT | O_TRUNC,
							 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if (out >= 0) {
			dup2(out, 1);
			dup2(out, 2);
			close(out);
		}
		const int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, 0);
			close(devnull);
		}
		execve(argv[0], argv, envp);
		_exit(127);
	}
}

/* The program started on the machine `ssh` reaches: the same tree, at the
 * path it has here, run with its output into a file there -- a file, not a
 * pipe, for prog_start()'s reason -- and its pid echoed back. */
long remote_start(const char *host, const char *dir,
						 const char *backend, const char *renderer,
						 const char *variant, char *log_out, size_t log_n)
{
	snprintf(log_out, log_n, "/tmp/fenestal-round-%ld-%s.log", (long)getpid(),
			 variant);
	/* The window server's own variables are the session's, and a session
	 * over ssh has none of this one's: driving the machine this program runs
	 * on (`localhost`) is the one case where they must be passed on, since
	 * the window has to open on the display it was told about. A bare
	 * machine needs none of them. */
	const bool  here = !strcmp(host, "localhost") || !strcmp(host, "127.0.0.1");
	const char *disp = here ? getenv("DISPLAY") : NULL;
	const char *wl   = here ? getenv("WAYLAND_DISPLAY") : NULL;
	const char *rt   = here ? getenv("XDG_RUNTIME_DIR") : NULL;
	/* X's own authority file is part of "which display may this client
	 * draw on": without it the server answers and then refuses. */
	const char *xa   = here ? getenv("XAUTHORITY") : NULL;

	Str cmd = { 0 };
	/* A `;`, not a `&&`: `cd x && prog &` puts the whole list in the
	 * background, and the shell then waits for it -- the ssh session stays
	 * open for as long as the window does. */
	str_addf(&cmd, "cd %.300s; ", dir);
	if (disp)
		str_addf(&cmd, "DISPLAY=%.160s ", disp);
	if (wl)
		str_addf(&cmd, "WAYLAND_DISPLAY=%.160s ", wl);
	if (rt)
		str_addf(&cmd, "XDG_RUNTIME_DIR=%.300s ", rt);
	if (xa)
		str_addf(&cmd, "XAUTHORITY=%.300s ", xa);
	str_addf(&cmd, "FNL_MCP=1 FNL_BACKEND=%s FNL_RENDERER=%s "
				   "FNL_ECHO_EVENTS=1 FNL_GPU_RANK=off ",
			 backend, renderer);
	if (strcmp(variant, "release") != 0)
		str_adds(&cmd, "FNL_VK_VALIDATION=1 ");
	/* Standard input too, or the program holds the ssh channel's open and
	 * the channel outlives the command that started it. */
	str_addf(&cmd, "nohup ./fenestal-testbed --frame-times < /dev/null "
				   "> %.200s 2>&1 & echo $!",
			 log_out);

	/* ssh takes the whole remote command as one argument, so nothing of it
	 * is a local shell's to mangle. */
	dbg("start: %s", cmd.p);
	int fds[2];
	if (pipe(fds) != 0)
		DIE("pipe: %s", strerror(errno));
	const pid_t child = fork();
	if (child < 0)
		DIE("fork: %s", strerror(errno));
	if (child == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		/* execvp() takes writable pointers; the words are this file's
		 * own arrays, not literals. */
		static char ssh_w[] = "ssh", opt_w[] = "-o";
		static char batch_w[] = "BatchMode=yes";
		char *args[] = { ssh_w, opt_w, batch_w, (char *)host, cmd.p, NULL };
		execvp("ssh", args);
		_exit(127);
	}
	close(fds[1]);
	char   buf[128];
	size_t n = 0;
	ssize_t r;
	while (n < sizeof buf - 1 && (r = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0)
		n += (size_t)r;
	buf[n] = '\0';
	close(fds[0]);
	waitpid(child, NULL, 0);
	str_free(&cmd);
	return strtol(buf, NULL, 10);
}

/* The log fetched from the machine, whole, into the local one the sections
 * read: what the library said there is what this program would have read
 * here. */
void remote_log(const char *host, const char *remote, const char *local)
{
	char cmd[1024];
	snprintf(cmd, sizeof cmd, "ssh -o BatchMode=yes %.120s 'cat %.300s'",
			 host, remote);
	FILE *f = popen(cmd, "r");
	if (!f)
		return;
	FILE *out = fopen(local, "w");
	if (!out) {
		pclose(f);
		return;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		fwrite(buf, 1, n, out);
	fclose(out);
	pclose(f);
}

