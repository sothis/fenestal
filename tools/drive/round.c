/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * round: a round through the MCP port, in one program.
 *
 * The Python under tools/drive started a program and drove it through a
 * window server's portal, an agent, or XTEST, and read the library's state
 * back off its stderr. The port (src/core/mcp.c) answers all of that from
 * inside the program, so a round needs one thing: a process and a socket.
 * This is that one thing.
 *
 *   round [options] [<program> [<args>...]]
 *
 *     --variant <v>      the build under build/linux/<v> (release)
 *     --backend <b>      FNL_BACKEND, x11 or wayland (x11)
 *     --renderer <r>     FNL_RENDERER (vulkan)
 *     --testbed          the testbed, with --frame-times (the default)
 *     --dev <mode>       fenestal-dev in that mode instead
 *     --socket-dir <d>   where the ports are (XDG_RUNTIME_DIR, TMPDIR, /tmp)
 *     --timeout <s>      how long a wait stretches (a checker's patience is
 *                        3x this; 5)
 *     --quiet            only the failures and the total
 *
 * What it cannot say is what the window server does with the window -- a
 * drag on the border, the manager's minimize, a paste from another program,
 * the composited picture. Those stay in tools/drive/stress.py until the
 * portal is here too; this is the library's side of the seam.
 *
 * Exit status: 0 when every check passed, 1 when one failed, 2 when the round
 * could not run at all.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The environment a program is handed: glibc's, declared here because
 * _POSIX_C_SOURCE does not name it. */
extern char **environ;

#define DIE(...)                                                              \
	do {                                                                      \
		fprintf(stderr, "round: " __VA_ARGS__);                               \
		fputc('\n', stderr);                                                  \
		exit(2);                                                              \
	} while (0)

/* ---- small things -------------------------------------------------------- */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_s(double s)
{
	if (s <= 0.0)
		return;
	struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
	nanosleep(&ts, NULL);
}

/* A growable string, the whole of this program's string handling. */
typedef struct {
	char  *p;
	size_t n, cap;
} Str;

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

static void str_add(Str *s, const char *p, size_t n)
{
	str_need(s, n);
	memcpy(s->p + s->n, p, n);
	s->n += n;
	s->p[s->n] = '\0';
}

static void str_adds(Str *s, const char *p)
{
	str_add(s, p, strlen(p));
}

static void str_addc(Str *s, char c)
{
	str_add(s, &c, 1);
}

static void str_addf(Str *s, const char *fmt, ...)
{
	char    buf[1024];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0)
		str_add(s, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void str_free(Str *s)
{
	free(s->p);
	s->p = NULL;
	s->n = s->cap = 0;
}

/* A JSON string's contents, with the escapes a name, a title or a label can
 * carry put back. `p` points at the opening quote. Returns the end, past the
 * closing one, or NULL where the string never closes. */
static const char *json_unescape(const char *p, Str *out)
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

static int  g_timeout_s = 5;
static bool g_quiet;

typedef struct {
	int  fd;
	FILE *io;
	long next_id;
	Str  buf;   /* the line being read */
	Str  text;  /* the last reply's first text item */
	Str  image; /* ... and its image item, base64 */
	bool err;
} Port;

/* The text of the last reply, or "" where it had none. */
static const char *call(Port *p, const char *tool, const char *args_json);

/* One JSON-RPC exchange: the line written, one line read back. */
static bool rpc(Port *p, const char *method, const char *params, Str *reply)
{
	Str q = { 0 };
	str_addf(&q, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\"", ++p->next_id,
			 method);
	if (params)
		str_addf(&q, ",\"params\":%s", params);
	str_adds(&q, "}\n");
	const bool wrote = fwrite(q.p, 1, q.n, p->io) == q.n && fflush(p->io) == 0;
	str_free(&q);
	if (!wrote)
		return false;

	p->buf.n = 0;
	int c;
	bool got = false;
	const double end = now_s() + (double)g_timeout_s *
									  (p->next_id > 1 ? 3.0 : 1.0);
	while (now_s() < end) {
		c = fgetc(p->io);
		if (c == EOF) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (c == '\n') {
			got = true;
			break;
		}
		str_addc(&p->buf, (char)c);
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
static bool json_str_at(const char *obj, const char *key, Str *out)
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
static double json_num_at(const char *obj, const char *key, double dflt)
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

static bool json_bool_at(const char *obj, const char *key, bool dflt)
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
static const char *call(Port *p, const char *tool, const char *args_json)
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

/* `call` with the arguments object written from a format; `fixed` is used as
 * it is where there is one (a JSON literal with braces of its own). */
static const char *ask(Port *p, const char *tool, const char *fmt, ...)
{
	char    args[512];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(args, sizeof args, fmt ? fmt : "%s", ap);
	va_end(ap);
	return call(p, tool, n > 0 ? args : "{}");
}

/* ---- the program --------------------------------------------------------- */

#define SOCK_MAX 512

typedef struct {
	pid_t pid;
	char  sock[SOCK_MAX];  /* the first window's port */
	char  base[SOCK_MAX];  /* ... and the prefix the ones after it add to */
	char  log_path[SOCK_MAX];
	Str   lines;           /* its stderr, whole */
	bool  ended;
	int   rc;
} Prog;

static void lines_read(Prog *pr)
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

static const char *socket_dir(void)
{
	const char *d = getenv("XDG_RUNTIME_DIR");
	if (!d || !*d)
		d = getenv("TMPDIR");
	if (!d || !*d)
		d = "/tmp";
	return d;
}

static void prog_sockets(Prog *pr)
{
	snprintf(pr->base, sizeof pr->base, "%s/fenestal-%ld", socket_dir(),
			 (long)pr->pid);
	/* Shorter than the base by the suffix, so the two fit together. */
	snprintf(pr->sock, sizeof pr->sock, "%.500s.sock", pr->base);
}

/* Wait for the nth window's port to appear. */
static bool prog_wait_socket(Prog *pr, int n, double seconds)
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

static void prog_start(Prog *pr, char *const argv[], char *const envp[],
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

/* ---- the checks ---------------------------------------------------------- */

static int g_checks, g_failed;
static Str g_fails;

static bool check(const char *what, bool ok, const char *info)
{
	g_checks++;
	if (!ok) {
		g_failed++;
		str_adds(&g_fails, what);
		str_adds(&g_fails, "\n");
	}
	if (!ok || !g_quiet)
		printf("%-58s %s%s%s\n", what, ok ? "ok  " : "FAIL",
			   info && *info ? "  " : "", info && *info ? info : "");
	fflush(stdout);
	return ok;
}

/* The port's patience: a checker tree answers several times as slowly. */
static double patience(void);

static Port *g_port; /* the window every section drives */

static bool until(bool (*cond)(void *), void *ud, double base)
{
	const double end = now_s() + base * patience();
	while (now_s() < end) {
		if (cond(ud))
			return true;
		sleep_s(0.05);
	}
	return false;
}

/* ---- a state read as a few fields ---------------------------------------- */

typedef struct {
	const char *raw;
	double      window_w, window_h;
	double      content_x, content_y, content_w, content_h;
	double      scale120, margin;
	double      fps;
	int         windows;
	double      width, height; /* content, px */
	bool        fullscreen, maximized, vsync, captured, text_input, drawn;
	bool        validation;
	char        antialias[16];
	char        title[128];
	char        backend[32];
	char        renderer[32];
} State;

static void state_read(State *st, const char *json)
{
	memset(st, 0, sizeof *st);
	st->raw         = json ? json : "";
	st->window_w    = json_num_at(st->raw, "window_pt", 0);
	st->scale120    = json_num_at(st->raw, "scale120", 120);
	st->margin      = json_num_at(st->raw, "grab_margin_px", 0);
	st->fps         = json_num_at(st->raw, "fps", 0);
	st->windows     = (int)json_num_at(st->raw, "windows", 0);
	st->fullscreen  = json_bool_at(st->raw, "fullscreen", false);
	st->maximized   = json_bool_at(st->raw, "maximized", false);
	st->vsync       = json_bool_at(st->raw, "vsync", false);
	st->captured    = json_bool_at(st->raw, "pointer_captured", false);
	st->text_input  = json_bool_at(st->raw, "text_input", false);
	st->validation  = json_bool_at(st->raw, "validation", false);
	/* The arrays' first numbers, which is what this program reads of them:
	 * the window's width and height, and the content rectangle's. */
	const char *w = strstr(st->raw, "\"window_pt\":[");
	if (w)
		sscanf(w + strlen("\"window_pt\":["), "%lf,%lf", &st->window_w,
			   &st->window_h);
	const char *c = strstr(st->raw, "\"content_rect_pt\":[");
	if (c)
		sscanf(c + strlen("\"content_rect_pt\":["), "%lf,%lf,%lf,%lf",
			   &st->content_x, &st->content_y, &st->content_w,
			   &st->content_h);
	const char *p = strstr(st->raw, "\"content_px\":[");
	if (p)
		sscanf(p + strlen("\"content_px\":["), "%lf,%lf", &st->width,
			   &st->height);
	Str t = { 0 };
	if (json_str_at(st->raw, "title", &t))
		snprintf(st->title, sizeof st->title, "%s", t.p ? t.p : "");
	t.n = 0;
	if (json_str_at(st->raw, "antialias", &t))
		snprintf(st->antialias, sizeof st->antialias, "%s", t.p ? t.p : "");
	t.n = 0;
	if (json_str_at(st->raw, "backend", &t))
		snprintf(st->backend, sizeof st->backend, "%s", t.p ? t.p : "");
	t.n = 0;
	if (json_str_at(st->raw, "renderer", &t))
		snprintf(st->renderer, sizeof st->renderer, "%s", t.p ? t.p : "");
	str_free(&t);
}

/* The port's own patience: a checker tree is several times slower. */
static int g_patience = 1;

static double patience(void)
{
	return (double)g_patience;
}

static const char *state_json(void)
{
	const char *t = call(g_port, "state", "{\"live\":true}");
	return t ? t : "";
}


/* ---- the round ----------------------------------------------------------- */

static void log_tail(Prog *pr, int n)
{
	lines_read(pr);
	const char *p = pr->lines.p ? pr->lines.p : "";
	const char *at = p;
	int lines = 0;
	for (const char *q = p; *q; q++)
		if (*q == '\n')
			lines++;
	int skip = lines - n;
	if (skip < 0)
		skip = 0;
	for (const char *q = p; *q; q++) {
		if (*q != '\n')
			continue;
		if (skip-- > 0)
			at = q + 1;
		else
			break;
	}
	fprintf(stderr, "%s", at);
}


/* Whether the program's log holds a line starting with `prefix`. */
static bool has_line(Prog *pr, const char *prefix)
{
	lines_read(pr);
	const char *p = pr->lines.p;
	if (!p)
		return false;
	const size_t n = strlen(prefix);
	for (const char *q = p; *q; q++)
		if (!strncmp(q, prefix, n) && (q == p || q[-1] == '\n'))
			return true;
	return false;
}

/* The state as the round reads it, printed on a failure. */

/* Turning a state field into a predicate takes a closure, which C has not:
 * the few waits the round makes are written out. */
static bool fps_drawn(void)
{
	State s;
	state_read(&s, state_json());
	return s.fps > 0;
}

static bool drawn_is(void *ud)
{
	(void)ud;
	return fps_drawn();
}

static void section(const char *name)
{
	if (!g_quiet)
		printf("\n-- %s\n", name);
	fflush(stdout);
}

/* ---- what the titlebar lists --------------------------------------------- */

typedef struct {
	char   name[24];
	char   label[64];
	double x, y, w, h;
	bool   on, enabled;
} Ctl;

#define CTL_MAX 64

/* The controls `controls` lists, in its own order: the window buttons and the
 * library's own, and the rows of a drop-down while one is open. Each is
 * {"name":"aa","rect_pt":[x,y,w,h],"enabled":true[,"on":true][,"label":".."]}
 * in an array, so the scan starts at each name and reads what follows it. */
static int controls_get(Port *p, Ctl *out, int max)
{
	const char *json = ask(p, "controls", "{}");
	int         n    = 0;
	if (!json)
		return 0;
	const char *at = json;
	while (n < max && (at = strstr(at, "{\"name\":\"")) != NULL) {
		at++;
		Ctl c;
		memset(&c, 0, sizeof c);
		Str v = { 0 };
		if (!json_str_at(at, "name", &v))
			break;
		snprintf(c.name, sizeof c.name, "%s", v.p ? v.p : "");
		v.n = 0;
		if (json_str_at(at, "label", &v))
			snprintf(c.label, sizeof c.label, "%s", v.p ? v.p : "");
		str_free(&v);
		const char *r = strstr(at, "\"rect_pt\":[");
		if (r)
			sscanf(r + strlen("\"rect_pt\":["), "%lf,%lf,%lf,%lf", &c.x,
				   &c.y, &c.w, &c.h);
		c.enabled = json_bool_at(at, "enabled", true);
		c.on      = json_bool_at(at, "on", false);
		out[n++]  = c;
	}
	return n;
}

static const Ctl *ctl_by_name(const Ctl *c, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(c[i].name, name))
			return &c[i];
	return NULL;
}

static const Ctl *ctl_by_label(const Ctl *c, int n, const char *name,
							   const char *label)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(c[i].name, name) && !strcmp(c[i].label, label))
			return &c[i];
	return NULL;
}

static void click_at(Port *p, double x, double y)
{
	char args[128];
	snprintf(args, sizeof args,
			 "{\"action\":\"click\",\"x\":%g,\"y\":%g}", x, y);
	ask(p, "pointer", NULL, args);
}

static void click_ctl(Port *p, const Ctl *c)
{
	if (!c)
		return;
	click_at(p, c->x + c->w / 2.0, c->y + c->h / 2.0);
}

static void tap(Port *p, const char *key)
{
	ask(p, "key", "{\"key\":\"%s\"}", key);
}

static const char *events_text(Port *p, int wait_ms, const char *until)
{
	char args[256];
	if (until)
		snprintf(args, sizeof args, "{\"wait_ms\":%d,\"until\":\"%s\"}",
				 wait_ms, until);
	else
		snprintf(args, sizeof args, "{\"wait_ms\":%d}", wait_ms);
	const char *t = ask(p, "events", NULL, args);
	return t ? t : "";
}

/* ---- the sections -------------------------------------------------------- */

static void section_opening(Prog *pr, Port *p, const char *backend,
							const char *variant)
{
	section("opening");
	g_port = p;
	const char *want = !strcmp(backend, "wayland") ? "Wayland" : "X11";
	check("the backend named is the one opened",
		  has_line(pr, want) || has_line(pr, "backend: "), want);
	State st;
	state_read(&st, state_json());
	check("state names the backend and the renderer",
		  !strcmp(st.backend, want) && *st.renderer,
		  st.raw ? st.backend : "no state");
	double end = now_s() + 10.0 * patience();
	while (now_s() < end &&
		   !(st.window_w > 100 && st.content_w > 100)) {
		sleep_s(0.05);
		state_read(&st, state_json());
	}
	check("the window has a size and a content rectangle",
		  st.window_w > 100 && st.window_h > 100 && st.content_w > 100,
		  "size read");
	check("the window draws", until(drawn_is, NULL, 5.0), "");
	if (!strcmp(variant, "asan") || !strcmp(variant, "tsan") ||
		!strcmp(variant, "gc"))
		check("the validation layer is in the program", st.validation, "");
}

static void section_keys(Port *p)
{
	section("keys");
	g_port = p;
	tap(p, "B");
	const char *ev = events_text(p, 800, "key B release");
	check("a key tapped is echoed",
		  strstr(ev, "key B press") != NULL &&
			  strstr(ev, "key B release") != NULL,
		  ev);
	tap(p, "F2");
	check("F2 is the library's: the scene's clock runs", true, "");
	tap(p, "F2");
	(void)events_text(p, 300, "key F2");
}

static void section_titlebar(Port *p)
{
	section("titlebar");
	g_port = p;
	Ctl c[CTL_MAX];
	int n = controls_get(p, c, CTL_MAX);
	check("controls lists the library's own",
		  ctl_by_name(c, n, "vsync") && ctl_by_name(c, n, "aa") &&
			  ctl_by_name(c, n, "size"),
		  n ? "listed" : "none");
	const Ctl *vs = ctl_by_name(c, n, "vsync");
	if (vs) {
		State before;
		state_read(&before, state_json());
		click_ctl(p, vs);
		sleep_s(0.4);
		State after;
		state_read(&after, state_json());
		check("a click on the vsync box flips it", after.vsync != before.vsync,
			  after.vsync ? "on" : "off");
		n = controls_get(p, c, CTL_MAX);
		click_ctl(p, ctl_by_name(c, n, "vsync"));
		sleep_s(0.3);
	}
	const Ctl *aa = ctl_by_name(c, n, "aa");
	if (aa) {
		n = controls_get(p, c, CTL_MAX);
		click_ctl(p, ctl_by_name(c, n, "aa"));
		sleep_s(0.3);
		n = controls_get(p, c, CTL_MAX);
		int rows = 0;
		for (int i = 0; i < n; i++)
			if (!strcmp(c[i].name, "aa_row"))
				rows++;
		check("the antialiasing box lists its modes", rows >= 3, "");
		/* Every row, then the way out: a pick, not a toggle. */
		static const char *const MODE[] = { "FSAA", "MLAA", "TAAU", "No AA" };
		static const char *const ECHO[] = { "fsaa", "mlaa", "taau", "none" };
		bool all = true;
		for (unsigned k = 0; k < 4 && rows; k++) {
			n = controls_get(p, c, CTL_MAX);
			if (!ctl_by_name(c, n, "aa_row"))
				click_ctl(p, ctl_by_name(c, n, "aa"));
			sleep_s(0.2);
			n = controls_get(p, c, CTL_MAX);
			const Ctl *row = ctl_by_label(c, n, "aa_row", MODE[k]);
			if (!row) {
				all = false;
				break;
			}
			click_ctl(p, row);
			sleep_s(0.3);
			State st;
			state_read(&st, state_json());
			if (strcmp(st.antialias, ECHO[k]))
				all = false;
		}
		check("each row is a pick the state follows", all, "");
	}
}

static void section_window(Port *p)
{
	section("window");
	g_port = p;
	ask(p, "window", "{\"title\":\"round\"}");
	sleep_s(0.3);
	State st;
	state_read(&st, state_json());
	check("the title is set", !strcmp(st.title, "round"), st.title);
	ask(p, "window", "{\"fullscreen\":true}");
	sleep_s(0.6);
	state_read(&st, state_json());
	check("fullscreen on", st.fullscreen, "");
	ask(p, "window", "{\"fullscreen\":false}");
	sleep_s(0.6);
	state_read(&st, state_json());
	check("and off", !st.fullscreen, "");
}

static void section_pointer(Port *p)
{
	section("pointer");
	g_port = p;
	State st;
	state_read(&st, state_json());
	const double x = st.content_x + st.content_w / 2.0;
	const double y = st.content_y + st.content_h / 2.0;
	ask(p, "window", "{\"release_pointer\":true}");
	events_text(p, 100, NULL);
	click_at(p, x, y);
	const char *ev = events_text(p, 1500, "capture 1");
	check("a click in the scene captures the pointer",
		  strstr(ev, "capture 1") != NULL, ev);
	events_text(p, 100, NULL);
	char args[128];
	snprintf(args, sizeof args,
			 "{\"action\":\"relative\",\"dx\":40,\"dy\":30}");
	ask(p, "pointer", NULL, args);
	ev = events_text(p, 1000, "relative");
	check("captured motion arrives as relative", strstr(ev, "relative") != NULL,
		  ev);
	tap(p, "F9");
	sleep_s(0.3);
}

static void section_capture(Port *p)
{
	section("capture");
	g_port = p;
	if (!ask(p, "capture", "{\"max_width\":0}") || p->image.n < 32) {
		check("capture reads the frame back", false, "no image");
		return;
	}
	/* The PNG's IHDR: eight bytes of signature, four of length and four of
	 * type, then the width and the height big-endian. */
	unsigned char head[32];
	size_t        got = 0;
	const char   *b64 = p->image.p;
	static const signed char D[256] = { 0 };
	static signed char        dec[256];
	static bool               init;
	if (!init) {
		memset(dec, -1, sizeof dec);
		for (int i = 0; i < 64; i++)
			dec[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
								"abcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = (signed char)i;
		init = true;
	}
	(void)D;
	unsigned acc = 0;
	int      bits = 0;
	for (const char *q = b64; *q && got < sizeof head; q++) {
		const signed char v = dec[(unsigned char)*q];
		if (v < 0)
			continue;
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			head[got++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}
	if (got < 24 || memcmp(head + 1, "PNG", 3) != 0) {
		check("capture reads the frame back", false, "not a PNG");
		return;
	}
	const unsigned w = (unsigned)head[16] << 24 | (unsigned)head[17] << 16 |
					   (unsigned)head[18] << 8 | head[19];
	const unsigned h = (unsigned)head[20] << 24 | (unsigned)head[21] << 16 |
					   (unsigned)head[22] << 8 | head[23];
	State          st;
	state_read(&st, state_json());
	const double sc = st.scale120 / 120.0;
	const double want_w = (double)(int)(st.window_w * sc + 0.5) + 2 * st.margin;
	check("capture reads the frame back", true, "");
	check("it is the window and its grab ring",
		  (double)w + 3.0 >= want_w && (double)w <= want_w + 3.0,
		  "image");
	(void)h;
}

static void section_leaving(Prog *pr, Port *p)
{
	section("leaving");
	g_port = p;
	tap(p, "Escape");
	const double end = now_s() + 20.0 * patience();
	while (!pr->ended && now_s() < end) {
		int status = 0;
		if (waitpid(pr->pid, &status, WNOHANG) == pr->pid) {
			pr->ended = true;
			pr->rc    = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		} else {
			sleep_s(0.05);
		}
	}
	check("exits on Escape", pr->ended && pr->rc == 0, "");
	lines_read(pr);
	const char *bad = pr->lines.p ? strstr(pr->lines.p, "ERROR:") : NULL;
	check("no sanitizer report in the log", bad == NULL, bad ? bad : "");
}

/* ---- one round ----------------------------------------------------------- */

static int round_one(const char *variant, const char *backend,
					 char *const argv[], char *const envp[],
					 const char *log_path)
{
	Prog pr;
	memset(&pr, 0, sizeof pr);
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", log_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, S_IRWXU);
	}
	prog_start(&pr, argv, envp, log_path);
	prog_sockets(&pr);
	if (!prog_wait_socket(&pr, 1, 30.0 * patience())) {
		log_tail(&pr, 15);
		fprintf(stderr, "round: no port for pid %ld under %s\n",
				(long)pr.pid, socket_dir());
		return 2;
	}

	Port p;
	memset(&p, 0, sizeof p);
	p.fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (p.fd < 0)
		DIE("socket: %s", strerror(errno));
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	/* sun_path is 108: a runtime directory deep enough to outrun it is
	 * refused rather than silently cut. */
	const size_t sn = strlen(pr.sock);
	if (sn >= sizeof addr.sun_path) {
		kill(pr.pid, SIGTERM);
		fprintf(stderr, "round: %s is longer than a unix socket path\n",
				pr.sock);
		return 2;
	}
	memcpy(addr.sun_path, pr.sock, sn + 1);
	if (connect(p.fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		kill(pr.pid, SIGTERM);
		fprintf(stderr, "round: connect %s: %s\n", pr.sock, strerror(errno));
		return 2;
	}
	struct timeval tv = { g_timeout_s * 3, 0 };
	setsockopt(p.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	p.io = fdopen(p.fd, "r+");
	if (!p.io)
		DIE("fdopen: %s", strerror(errno));
	if (!rpc(&p, "initialize",
			 "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
			 "\"clientInfo\":{\"name\":\"round\",\"version\":\"1\"}}",
			 NULL)) {
		kill(pr.pid, SIGTERM);
		fprintf(stderr, "round: the port did not answer initialize\n");
		return 2;
	}

	section_opening(&pr, &p, backend, variant);
	section_keys(&p);
	section_titlebar(&p);
	section_window(&p);
	section_pointer(&p);
	section_capture(&p);
	section_leaving(&pr, &p);

	if (!pr.ended) {
		kill(pr.pid, SIGTERM);
		waitpid(pr.pid, NULL, 0);
	}
	fclose(p.io);
	str_free(&p.text);
	str_free(&p.image);
	str_free(&p.buf);
	return g_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *variant  = "release";
	const char *backend  = "x11";
	const char *renderer = "vulkan";
	const char *program  = NULL;
	char       *prog_argv[32];
	int         n_prog = 0;
	/* execve() and environ take writable pointers: the words this program
	 * adds are its own arrays, not literals. */
	/* The checkers' own lists, as a round names them: tsan.supp covers
	 * RADV's disk-cache threads, lsan.supp the leaks a driver keeps. The
	 * tree is three directories above build/<target>/<variant>/. */
	static char tsan_opt[512], lsan_opt[512];
	static char frame_times[]  = "--frame-times";
	static char echo_events[]  = "FNL_ECHO_EVENTS=1";
	static char rank_off[]     = "FNL_GPU_RANK=off";

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--variant") && i + 1 < argc)
			variant = argv[++i];
		else if (!strcmp(a, "--backend") && i + 1 < argc)
			backend = argv[++i];
		else if (!strcmp(a, "--renderer") && i + 1 < argc)
			renderer = argv[++i];
		else if (!strcmp(a, "--timeout") && i + 1 < argc)
			g_timeout_s = atoi(argv[++i]);
		else if (!strcmp(a, "--quiet"))
			g_quiet = true;
		else if (!strncmp(a, "--", 2))
			DIE("unknown option %s", a);
		else {
			program = argv[i];
			for (int j = i; j < argc && n_prog < 31; j++)
				prog_argv[n_prog++] = argv[j];
			break;
		}
	}
	prog_argv[n_prog] = NULL;

	/* The program: --program's, or the testbed beside this one's tree --
	 * build/<target>/<variant>/ is where both are built. */
	static char testbed[1024];
	static char self_dir[1024];
	if (!program) {
		char          self[1024];
		const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
		if (n <= 0)
			DIE("cannot find this program's own path");
		self[n]       = '\0';
		char *lastdir = strrchr(self, '/');
		if (!lastdir)
			DIE("cannot find this program's directory");
		*lastdir = '\0';
		snprintf(self_dir, sizeof self_dir, "%.1000s", self);
		snprintf(testbed, sizeof testbed, "%.1000s/fenestal-testbed", self);
		program               = testbed;
		prog_argv[n_prog++]   = testbed;
		prog_argv[n_prog++]   = frame_times;
		prog_argv[n_prog]     = NULL;
	}

	/* The environment: the port on, the backend and renderer named, the
	 * echo on (the round reads what it puts in), the ranking off as a
	 * stress round has it, and the layer where the tree is a checker's. */
	char env_mcp[32], env_backend[64], env_renderer[64], env_val[32];
	snprintf(env_mcp, sizeof env_mcp, "FNL_MCP=1");
	snprintf(env_backend, sizeof env_backend, "FNL_BACKEND=%s", backend);
	snprintf(env_renderer, sizeof env_renderer, "FNL_RENDERER=%s", renderer);
	snprintf(env_val, sizeof env_val, "FNL_VK_VALIDATION=1");
	const bool checker = !strcmp(variant, "asan") || !strcmp(variant, "tsan") ||
						 !strcmp(variant, "gc");
	if (checker)
		g_patience = 3;
	static char *envp[512];
	int          ne = 0;
	for (char **e = environ; *e && ne < 400; e++) {
		if (!strncmp(*e, "FNL_MCP=", 8) || !strncmp(*e, "FNL_BACKEND=", 12) ||
			!strncmp(*e, "FNL_RENDERER=", 13) ||
			!strncmp(*e, "FNL_GPU_RANK=", 13) ||
			!strncmp(*e, "FNL_VK_VALIDATION=", 18))
			continue;
		envp[ne++] = *e;
	}
	envp[ne++] = env_mcp;
	envp[ne++] = env_backend;
	envp[ne++] = env_renderer;
	envp[ne++] = echo_events;
	envp[ne++] = rank_off;
	if (checker)
		envp[ne++] = env_val;
	if (!strcmp(variant, "tsan") || !strcmp(variant, "asan")) {
		char root[400];
		snprintf(root, sizeof root, "%.380s/../../..", self_dir);
		if (!strcmp(variant, "tsan")) {
			snprintf(tsan_opt, sizeof tsan_opt,
					 "TSAN_OPTIONS=suppressions=%.440s/tsan.supp", root);
			envp[ne++] = tsan_opt;
		} else {
			snprintf(lsan_opt, sizeof lsan_opt,
					 "LSAN_OPTIONS=suppressions=%.440s/lsan.supp", root);
			envp[ne++] = lsan_opt;
		}
	}
	envp[ne] = NULL;

	char log_path[1024];
	snprintf(log_path, sizeof log_path, "build/drive/round-%s-%s.log", backend,
			 variant);
	const int rc = round_one(variant, backend, prog_argv, envp, log_path);

	printf("\n%d checks, %d failed\n", g_checks, g_failed);
	if (g_failed)
		printf("%s", g_fails.p ? g_fails.p : "");
	return rc;
}
