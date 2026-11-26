/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The MCP port: a Model Context Protocol server inside the program, on a
 * local socket, so that an agent can drive the window from inside the
 * process -- the window's state, the events the program polled, keys, text
 * and the pointer, and the frame as the GPU drew it. DESIGN *Driven from
 * inside* has what it is for and what it cannot stand in for.
 *
 *   FNL_MCP=1        $XDG_RUNTIME_DIR/fenestal-<pid>.sock (else $TMPDIR, /tmp;
 *                    %TEMP% on Windows)
 *   FNL_MCP=<path>   that socket
 *
 * The framing is MCP's stdio transport, one JSON-RPC message a line, so a
 * client that speaks stdio reaches it through a bridge:
 *
 *   socat - UNIX-CONNECT:/run/user/1000/fenestal-1234.sock
 *
 * One thread of its own, one client at a time. It never touches what the
 * window's two threads own: the input goes through a mailbox that the pump
 * drains into the backends' own entry points (mcp_drain(), from
 * app_drain_requests()), the capture through fnl_post() and the readback a
 * move already takes (App::snap_owed), and the events are the echo's lines
 * (echo.c), handed over as the program polls them. Everything else it reads
 * through the public [any] getters.
 *
 * The socket is AF_UNIX everywhere: Winsock has had it since Windows 10
 * 1803, the oldest this library runs on. The few calls that differ are the
 * shim below.
 */

/* Before app.h, whose <windows.h> would otherwise bring in winsock.h, the
 * first version, which winsock2.h cannot follow. */
#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include "core/app.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

#if defined(_WIN32)

#include <winsock2.h>
#include <afunix.h>

typedef SOCKET Sock;
#define SOCK_NONE INVALID_SOCKET
#define SEP "\\"

static int sock_err(void) { return WSAGetLastError(); }
static bool sock_again(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
static void sock_close(Sock s) { closesocket(s); }
static int sock_poll(struct pollfd *p, int n, int ms)
{
	return WSAPoll(p, (ULONG)n, ms);
}
static long sock_recv(Sock s, char *p, size_t n)
{
	return recv(s, p, (int)n, 0);
}
static long sock_send(Sock s, const char *p, size_t n)
{
	return send(s, p, n > 1u << 30 ? 1 << 30 : (int)n, 0);
}
static void path_remove(const char *path) { DeleteFileA(path); }
static long own_pid(void) { return (long)GetCurrentProcessId(); }

/* shortcut: Windows has no cheap file identity here, so a socket at the path
 * at exit is taken for ours; upgrade with GetFileInformationByHandle. */
typedef struct { unsigned long long a, b; } PathId;
static bool path_id(const char *path, PathId *id)
{
	id->a = id->b = 0;
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}
static const char *err_text(int e)
{
	(void)e;
	return "";
}

/* A socket at the path, which a program that died leaves behind: a reparse
 * point of AF_UNIX's own tag, which no other file is. */
static bool path_is_socket(const char *path)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(path, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	FindClose(h);
	return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
		   fd.dwReserved0 == IO_REPARSE_TAG_AF_UNIX;
}

/* Windows has no mode on the file: the directory's ACL decides who may
 * connect, and %TEMP% is the user's. */
static bool path_private(const char *path)
{
	(void)path;
	return true;
}

static void sock_flags(Sock s)
{
	u_long one = 1;
	(void)ioctlsocket(s, FIONBIO, &one);
	(void)SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
}

static bool sock_up(void)
{
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
static void sock_down(void) { WSACleanup(); }

#else

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef int Sock;
#define SOCK_NONE (-1)
#define SEP "/"

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0 /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

static int sock_err(void) { return errno; }
static bool sock_again(int e)
{
	return e == EINTR || e == EAGAIN || e == EWOULDBLOCK;
}
static void sock_close(Sock s) { close(s); }
static int sock_poll(struct pollfd *p, int n, int ms)
{
	return poll(p, (nfds_t)n, ms);
}
static long sock_recv(Sock s, char *p, size_t n)
{
	return (long)recv(s, p, n, 0);
}
static long sock_send(Sock s, const char *p, size_t n)
{
	return (long)send(s, p, n, SEND_FLAGS);
}
static void path_remove(const char *path) { unlink(path); }
static long own_pid(void) { return (long)getpid(); }

/* Which file is at the path, so that the exit removes the one this program
 * made and not one that a later program put there. */
typedef struct { unsigned long long a, b; } PathId;
static bool path_id(const char *path, PathId *id)
{
	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
		return false;
	id->a = (unsigned long long)st.st_dev;
	id->b = (unsigned long long)st.st_ino;
	return true;
}
static const char *err_text(int e) { return strerror(e); }

static bool path_is_socket(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

/* The user's alone. Set before listen(): nothing can connect until then,
 * so there is no moment the socket is anybody else's. */
static bool path_private(const char *path)
{
	return chmod(path, S_IRUSR | S_IWUSR) == 0;
}

static void sock_flags(Sock s)
{
	(void)fcntl(s, F_SETFD, FD_CLOEXEC);
	(void)fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
	const int one = 1;
	(void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

static bool sock_up(void) { return true; }
static void sock_down(void) {}

#endif

/* Whether the peer has hung up: a byte peeked at, or none, on the socket
 * that is not blocking. For a request whose work is wasted on a client that
 * is gone. */
static bool sock_gone(Sock s)
{
	char c;
	const long r = (long)recv(s, &c, 1, MSG_PEEK);
	return r == 0 || (r < 0 && !sock_again(sock_err()));
}

/* Where FNL_MCP=1 puts the socket: a directory of the user's alone where
 * there is one, since the socket is a remote control of the window. */
static const char *socket_dir(void)
{
#if defined(_WIN32)
	static const char *const VARS[] = { "TEMP", "TMP" };
	const char *dir = ".";
#else
	static const char *const VARS[] = { "XDG_RUNTIME_DIR", "TMPDIR" };
	const char *dir = "/tmp";
#endif
	for (size_t i = 0; i < sizeof VARS / sizeof VARS[0]; i++) {
		const char *v = getenv(VARS[i]);
		if (v && *v)
			return v;
	}
	return dir;
}

/* A request is a few hundred bytes; one longer than this is answered with
 * a parse error and dropped to its newline. */
#define MCP_LINE_MAX (64 * 1024)
/* Lines kept between two `events`: the newest MCP_EVENTS, the older ones
 * dropped and counted, as a reader that connects late wants them. Each is
 * echo.c's line, which is 640 bytes at most. */
#define MCP_EVENTS  256
#define MCP_EV_LINE 640
/* One batch for the pump: a triple click with its modifiers is fifteen. */
#define MCP_IN_MAX  24
/* What the mailbox carries of a `text` at a time, and what one call takes,
 * cut into those. */
#define MCP_TEXT_MAX 512
#define MCP_TYPE_MAX 4096
/* Code points a chunk of `text` posts: the queue holds EVENT_QUEUE_MAX, and
 * the chunk goes in only when half of that is free. */
#define MCP_CHUNK_CP 100
/* How long a call waits for the pump's turn or for a frame: a window that
 * is pumping and shown answers within a frame or two. */
#define MCP_WAIT_S 2.0
/* A capture asked for while the window moves to another GPU or renderer
 * waits the move out, as long as a rebuild takes. */
#define MCP_MOVE_S 8.0
/* How often a blocked read or write looks at the stop flag. */
#define MCP_POLL_MS 200
/* A client that has asked nothing this long gives way to one waiting to be
 * served: an agent that died without closing its socket locks nobody out. */
#define MCP_IDLE_S 30.0
#define MCP_BACKLOG 4

typedef enum {
	IN_KEY, IN_REPEAT, IN_MOTION, IN_RELATIVE, IN_BUTTON, IN_SCROLL, IN_TEXT
} InKind;

/* One input for the pump, in the units the backends hand in: window points
 * and notches. */
typedef struct {
	InKind kind;
	int    code;    /* FnlKey or FnlButton */
	bool   pressed;
	double x, y, dx, dy;
} In;

typedef enum {
	SHOT_IDLE,
	SHOT_WANT,      /* posted; the next frame drawn reads itself back */
	SHOT_DONE,
	SHOT_NONE,      /* the renderer declined the readback */
	SHOT_MOVING,    /* a move's veil is up: its frames are not the window's */
} ShotState;

struct Mcp {
	App *app;
	FnlThread thread;
	FnlAtomicBool stop;
	Sock listen_fd;
	bool sock_up;
	struct sockaddr_un addr;
	/* The file this program made at the path: the exit removes that one,
	 * and not what a later program put in its place. */
	PathId path_made;
	bool   path_made_ok;

	/* The port's thread alone, from here to the lock. */
	Sock   cur;             /* the client being served */
	bool   initialized;     /* it has sent `initialize` */
	bool   live;            /* it asked for the figures to be kept measured */
	uint32_t key_held[KEY_HELD_WORDS];
	unsigned button_held;   /* bit per FnlButton */
	double   ptr_x, ptr_y;  /* where the port last put the pointer */
	bool     ptr_known;
	double   press_s;       /* when it last pressed, for a click's spacing */

	/* Everything below is under `lock`. `cond` has one waiter, the port's
	 * thread, which is all FnlCond promises on Windows. */
	FnlMutex lock;
	FnlCond  cond;

	In       in[MCP_IN_MAX];
	int      in_n;
	uint32_t in_done;       /* batches the pump has taken */
	char     text[MCP_TEXT_MAX];

	char   (*ev)[MCP_EV_LINE]; /* a ring: ev_head the oldest */
	int      ev_head, ev_n;
	uint32_t ev_lost;       /* older lines pushed out of it */
	double   ev_last_s;     /* when the newest arrived */

	ShotState      shot;
	unsigned char *shot_px; /* rows of shot_w * 4 bytes */
	int            shot_w, shot_h;
	bool           shot_bgra;
};

typedef struct Mcp Mcp;

/* ---- what the window's threads call -------------------------------------- */

void mcp_event_line(const App *a, const char *line, size_t n)
{
	Mcp *m = a->mcp;
	/* Without its newline, and cut to a slot. */
	if (n && line[n - 1] == '\n')
		n--;
	if (n >= MCP_EV_LINE)
		n = MCP_EV_LINE - 1;
	fnl_mutex_lock(&m->lock);
	if (m->ev_n == MCP_EVENTS) {
		m->ev_head = (m->ev_head + 1) % MCP_EVENTS;
		m->ev_n--;
		m->ev_lost++;
	}
	char *slot = m->ev[(m->ev_head + m->ev_n) % MCP_EVENTS];
	memcpy(slot, line, n);
	slot[n] = '\0';
	m->ev_n++;
	m->ev_last_s = now_seconds();
	fnl_cond_signal(&m->cond);
	fnl_mutex_unlock(&m->lock);
}

void mcp_frame(const App *a, const void *px, int w, int h, size_t pitch,
			   bool bgra)
{
	Mcp *m = a->mcp;
	fnl_mutex_lock(&m->lock);
	if (m->shot == SHOT_WANT) {
		/* Copied tight here, and turned into a picture on the port's
		 * thread: the render thread pays a copy and no more. */
		unsigned char *copy = NULL;
		const size_t row    = w > 0 ? (size_t)w * 4u : 0;
		if (px && row && h > 0 && pitch >= row &&
			(size_t)h <= SIZE_MAX / row)
			copy = malloc(row * (size_t)h);
		if (copy) {
			for (int y = 0; y < h; y++)
				memcpy(copy + (size_t)y * row,
					   (const unsigned char *)px + (size_t)y * pitch, row);
			free(m->shot_px);
			m->shot_px   = copy;
			m->shot_w    = w;
			m->shot_h    = h;
			m->shot_bgra = bgra;
			m->shot      = SHOT_DONE;
		} else {
			m->shot = SHOT_NONE;
		}
		fnl_cond_signal(&m->cond);
	}
	fnl_mutex_unlock(&m->lock);
}

void mcp_frame_declined(const App *a)
{
	Mcp *m = a->mcp;
	fnl_mutex_lock(&m->lock);
	if (m->shot == SHOT_WANT) {
		m->shot = SHOT_NONE;
		fnl_cond_signal(&m->cond);
	}
	fnl_mutex_unlock(&m->lock);
}

/* Posted (fnl_post()) by `capture`: the render thread's own flag, set on
 * the render thread. Outside a move, which owns the readback while its veil
 * is up. Through the App and not the port, which a post outliving it could
 * not reach. */
static void shot_arm(void *ud)
{
	App *a = ud;
	Mcp *m = a->mcp;
	if (!m)
		return;
	if (a->veil_phase != VEIL_NONE && a->veil_phase != VEIL_SNAP) {
		fnl_mutex_lock(&m->lock);
		if (m->shot == SHOT_WANT) {
			m->shot = SHOT_MOVING;
			fnl_cond_signal(&m->cond);
		}
		fnl_mutex_unlock(&m->lock);
		return;
	}
	a->snap_owed = true;
	a->record_gen++;
}

/* The pointer put at a point, as a device's motion is: where the backend
 * keeps a pointer of its own it follows (KMS's cursor), the program is told,
 * and the titlebar's drag sees it. */
static void pointer_to(App *a, double x, double y)
{
	if (a->pointer_captured)
		return;
	a->ptr_x = x;
	a->ptr_y = y;
	BACKEND(a)->pointer_warp(a, x, y);
	app_pointer_motion(a, x, y);
	deco_drag(a, a->ptr_x, a->ptr_y);
}

void mcp_drain(App *a)
{
	Mcp *m = a->mcp;
	In run[MCP_IN_MAX];
	char text[MCP_TEXT_MAX];
	fnl_mutex_lock(&m->lock);
	const int n = m->in_n;
	memcpy(run, m->in, sizeof run);
	memcpy(text, m->text, sizeof text);
	m->in_n = 0;
	fnl_mutex_unlock(&m->lock);
	if (!n)
		return;

	/* The click counter's clock: a double-click is two presses within
	 * DECO_DBLCLICK_MS of each other on it. */
	const uint32_t ms = (uint32_t)((now_seconds() - a->started_at) * 1e3);
	for (int i = 0; i < n; i++) {
		const In *in = &run[i];
		switch (in->kind) {
		case IN_KEY: app_key(a, (FnlKey)in->code, in->pressed); break;
		case IN_REPEAT: app_key_repeat(a, (FnlKey)in->code); break;
		case IN_MOTION: pointer_to(a, in->x, in->y); break;
		case IN_RELATIVE:
			if (a->pointer_captured) {
				/* What a device's delta is: reported, and the reticle's
				 * to move, in pixels. */
				const double s = fnl_scale120(a) / 120.0;
				app_pointer_relative(a, in->dx, in->dy);
				app_reticle_move(a, in->dx * s, in->dy * s);
			} else {
				pointer_to(a, a->ptr_x + in->dx, a->ptr_y + in->dy);
			}
			break;
		case IN_BUTTON:
			app_pointer_button(a, (FnlButton)in->code, in->pressed, in->x,
							   in->y, ms);
			break;
		case IN_SCROLL:
			app_pointer_scroll(a, in->x, in->y, in->dx, in->dy);
			break;
		case IN_TEXT: app_text_utf8(a, text); break;
		}
	}

	fnl_mutex_lock(&m->lock);
	m->in_done++;
	fnl_cond_signal(&m->cond);
	fnl_mutex_unlock(&m->lock);
}

/* ---- a growing string ---------------------------------------------------- */

typedef struct {
	char  *p;
	size_t n, cap;
	bool   bad; /* an allocation failed; everything after is dropped */
} Sb;

/* Room for `extra` bytes and a terminator after them. */
static char *sb_room(Sb *b, size_t extra)
{
	if (b->bad)
		return NULL;
	if (extra >= b->cap - b->n) {
		size_t cap = b->cap ? b->cap : 4096;
		while (extra >= cap - b->n) {
			if (cap > SIZE_MAX / 2) {
				b->bad = true;
				return NULL;
			}
			cap *= 2;
		}
		char *q = realloc(b->p, cap);
		if (!q) {
			b->bad = true;
			return NULL;
		}
		b->p   = q;
		b->cap = cap;
	}
	return b->p + b->n;
}

static void sb_put(Sb *b, const char *s, size_t n)
{
	char *o = sb_room(b, n);
	if (!o)
		return;
	memcpy(o, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}

static void sb_s(Sb *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

static void sb_f(Sb *b, const char *fmt, ...) FNL_PRINTF(2, 3);
static void sb_f(Sb *b, const char *fmt, ...)
{
	char t[512];
	va_list ap;
	va_start(ap, fmt);
	const int w = vsnprintf(t, sizeof t, fmt, ap);
	va_end(ap);
	if (w > 0)
		sb_put(b, t, (size_t)w < sizeof t ? (size_t)w : sizeof t - 1);
}

/* A JSON string. Another program's text goes through here -- an output's
 * name, a paste -- so a malformed sequence is U+FFFD and not bytes that
 * would make the whole message invalid. */
static void sb_jstr(Sb *b, const char *s)
{
	const char *p = s ? s : "";
	sb_put(b, "\"", 1);
	while (*p) {
		const unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			const char e[2] = { '\\', (char)c };
			sb_put(b, e, 2);
			p++;
		} else if (c < 0x20 || c == 0x7f) {
			char e[8];
			snprintf(e, sizeof e, "\\u%04x", c);
			sb_put(b, e, 6);
			p++;
		} else if (c < 0x80) {
			const char *q = p;
			while ((unsigned char)*q >= 0x20 && (unsigned char)*q < 0x7f &&
				   *q != '"' && *q != '\\')
				q++;
			sb_put(b, p, (size_t)(q - p));
			p = q;
		} else {
			uint32_t cp;
			bool ok;
			const char *q = utf8_decode(p, &cp, &ok);
			if (ok)
				sb_put(b, p, (size_t)(q - p));
			else
				sb_put(b, "\\ufffd", 6);
			p = q;
		}
	}
	sb_put(b, "\"", 1);
}

/* A number, with no help from printf's %f, whose decimal point is the
 * locale's: three places, which is finer than any figure here means. */
static void sb_num(Sb *b, double v)
{
	if (!isfinite(v)) {
		sb_s(b, "null");
		return;
	}
	const bool neg = v < 0.0;
	const double av = neg ? -v : v;
	if (av >= 1e15) {
		sb_f(b, "%.0f", v);
		return;
	}
	long long ip  = (long long)av;
	int       frc = (int)((av - (double)ip) * 1000.0 + 0.5);
	if (frc >= 1000) {
		ip++;
		frc -= 1000;
	}
	if (frc)
		sb_f(b, "%s%lld.%03d", neg ? "-" : "", ip, frc);
	else
		sb_f(b, "%s%lld", neg && ip ? "-" : "", ip);
}

/* A member's name, after a comma unless it is the object's first. */
static void sb_key(Sb *b, const char *name)
{
	if (b->n && b->p[b->n - 1] != '{')
		sb_s(b, ",");
	sb_jstr(b, name);
	sb_s(b, ":");
}

static void sb_base64(Sb *b, const unsigned char *d, size_t n)
{
	static const char T[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const size_t need = (n + 2) / 3 * 4;
	char *o = sb_room(b, need);
	if (!o)
		return;
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		const uint32_t v = ((uint32_t)d[i] << 16) |
						   ((uint32_t)d[i + 1] << 8) | d[i + 2];
		*o++ = T[v >> 18];
		*o++ = T[(v >> 12) & 63];
		*o++ = T[(v >> 6) & 63];
		*o++ = T[v & 63];
	}
	if (n - i == 1) {
		const uint32_t v = (uint32_t)d[i] << 16;
		*o++ = T[v >> 18];
		*o++ = T[(v >> 12) & 63];
		*o++ = '=';
		*o++ = '=';
	} else if (n - i == 2) {
		const uint32_t v = ((uint32_t)d[i] << 16) |
						   ((uint32_t)d[i + 1] << 8);
		*o++ = T[v >> 18];
		*o++ = T[(v >> 12) & 63];
		*o++ = T[(v >> 6) & 63];
		*o++ = '=';
	}
	b->n += need;
	b->p[b->n] = '\0';
}

/* ---- the picture ---------------------------------------------------------
 *
 * A PNG, Sub-filtered and deflated with fixed Huffman codes and a hash
 * chain over the last 32 KiB: no zlib, which is not on the link line
 * (DESIGN *One build per operating system*). A screen is flat runs and the
 * same rows again, so this is a tenth of the stored form's size for a few
 * lines of code; a better entropy coder would gain little on it. */

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
	}
	return ~c;
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

typedef struct {
	unsigned char *p;
	size_t   n;
	uint32_t acc;
	int      nb;
} Bits;

/* `n` bits of `v`, low first, as deflate packs them. */
static void bits_put(Bits *b, uint32_t v, int n)
{
	b->acc |= v << b->nb;
	b->nb += n;
	while (b->nb >= 8) {
		b->p[b->n++] = (unsigned char)b->acc;
		b->acc >>= 8;
		b->nb -= 8;
	}
}

/* A Huffman code goes out high bit first. */
static uint32_t bits_rev(uint32_t v, int n)
{
	uint32_t r = 0;
	for (int i = 0; i < n; i++, v >>= 1)
		r = (r << 1) | (v & 1u);
	return r;
}

static void put_sym(Bits *b, unsigned s)
{
	if (s < 144)
		bits_put(b, bits_rev(0x30u + s, 8), 8);
	else if (s < 256)
		bits_put(b, bits_rev(0x190u + s - 144u, 9), 9);
	else if (s < 280)
		bits_put(b, bits_rev(s - 256u, 7), 7);
	else
		bits_put(b, bits_rev(0xc0u + s - 280u, 8), 8);
}

static const uint16_t LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15,
	17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227,
	258 };
static const uint8_t LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
	2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33,
	49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
	6145, 8193, 12289, 16385, 24577 };
static const uint8_t DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,
	5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

#define LZ_HASH_BITS 15
#define LZ_WINDOW    32768
#define LZ_CHAIN     16

static uint32_t lz_hash(const unsigned char *p)
{
	return (((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) *
			2654435761u) >> (32 - LZ_HASH_BITS);
}

/* One final block of fixed codes, in `out`, which holds n + n / 8 + 16 bytes
 * (a literal is nine bits at most, and a match is never longer than what it
 * replaces). Returns its length, 0 where there was no memory for the hash. */
static size_t deflate_fixed(const unsigned char *in, size_t n,
							unsigned char *out)
{
	int32_t *head = malloc(((size_t)1 << LZ_HASH_BITS) * sizeof *head);
	int32_t *prev = malloc(LZ_WINDOW * sizeof *prev);
	if (!head || !prev) {
		free(head);
		free(prev);
		return 0;
	}
	for (size_t i = 0; i < (size_t)1 << LZ_HASH_BITS; i++)
		head[i] = -1;
	Bits b = { .p = out };
	bits_put(&b, 1, 1); /* the last block */
	bits_put(&b, 1, 2); /* fixed codes */

	for (size_t i = 0; i < n;) {
		size_t best = 0, dist = 0;
		if (i + 3 <= n) {
			int32_t cand = head[lz_hash(in + i)];
			for (int chain = 0; cand >= 0 && i - (size_t)cand <= LZ_WINDOW &&
								chain < LZ_CHAIN;
				 chain++, cand = prev[cand & (LZ_WINDOW - 1)]) {
				size_t l = 0;
				while (l < 258 && i + l < n && in[(size_t)cand + l] == in[i + l])
					l++;
				if (l > best) {
					best = l;
					dist = i - (size_t)cand;
					if (l == 258)
						break;
				}
			}
		}
		size_t step = 1;
		if (best >= 3) {
			int lc = 28;
			while (LEN_BASE[lc] > best)
				lc--;
			put_sym(&b, 257u + (unsigned)lc);
			bits_put(&b, (uint32_t)(best - LEN_BASE[lc]), LEN_EXTRA[lc]);
			int dc = 29;
			while (DIST_BASE[dc] > dist)
				dc--;
			bits_put(&b, bits_rev((uint32_t)dc, 5), 5);
			bits_put(&b, (uint32_t)(dist - DIST_BASE[dc]), DIST_EXTRA[dc]);
			step = best;
		} else {
			put_sym(&b, in[i]);
		}
		for (size_t k = 0; k < step; k++, i++)
			if (i + 3 <= n) {
				const uint32_t h = lz_hash(in + i);
				prev[i & (LZ_WINDOW - 1)] = head[h];
				head[h] = (int32_t)i;
			}
	}
	put_sym(&b, 256);
	if (b.nb)
		bits_put(&b, 0, 8 - b.nb);
	free(head);
	free(prev);
	return b.n;
}

/* `raw` is the image data PNG filters, `h` rows of a filter byte and w * 3
 * bytes of RGB. */
static unsigned char *png_encode(const unsigned char *raw, size_t raw_n, int w,
								 int h, size_t *out_n)
{
	if (raw_n == 0 || raw_n > UINT32_MAX / 4)
		return NULL;
	const size_t def_max = raw_n + raw_n / 8 + 16;
	const size_t total   = 8u + 25u + 8u + 2u + def_max + 4u + 4u + 12u;
	unsigned char *o = malloc(total);
	if (!o)
		return NULL;
	unsigned char *p = o;
	memcpy(p, "\x89PNG\r\n\x1a\n", 8);
	p += 8;

	put32(p, 13);
	memcpy(p + 4, "IHDR", 4);
	put32(p + 8, (uint32_t)w);
	put32(p + 12, (uint32_t)h);
	p[16] = 8; /* bits a channel */
	p[17] = 2; /* RGB */
	p[18] = p[19] = p[20] = 0;
	put32(p + 21, crc32_of(p + 4, 17));
	p += 25;

	unsigned char *d = p + 8;
	memcpy(p + 4, "IDAT", 4);
	*d++ = 0x78;
	*d++ = 0x01;
	const size_t dn = deflate_fixed(raw, raw_n, d);
	if (!dn) {
		free(o);
		return NULL;
	}
	d += dn;
	uint32_t s1 = 1, s2 = 0;
	for (size_t i = 0; i < raw_n; i++) {
		s1 = (s1 + raw[i]) % 65521u;
		s2 = (s2 + s1) % 65521u;
	}
	put32(d, (s2 << 16) | s1);
	d += 4;
	const size_t idat = (size_t)(d - (p + 8));
	put32(p, (uint32_t)idat);
	put32(d, crc32_of(p + 4, 4 + idat));
	d += 4;

	put32(d, 0);
	memcpy(d + 4, "IEND", 4);
	put32(d + 8, crc32_of(d + 4, 4));
	d += 12;
	FNL_ASSERT((size_t)(d - o) <= total);
	*out_n = (size_t)(d - o);
	return o;
}

/* The rectangle (x0, y0, cw, ch) of the frame, `f` pixels to one each way
 * by their mean, as PNG's rows with the Sub filter; the edge's blocks are
 * what is left of them. */
static unsigned char *shrink(const unsigned char *px, int w, bool bgra, int x0,
							 int y0, int cw, int ch, int f, int *ow, int *oh,
							 size_t *raw_n)
{
	const int W = (cw + f - 1) / f, H = (ch + f - 1) / f;
	const size_t row = (size_t)W * 3u + 1u;
	if ((size_t)H > SIZE_MAX / row)
		return NULL;
	unsigned char *raw = malloc(row * (size_t)H);
	if (!raw)
		return NULL;
	const int r = bgra ? 2 : 0, bl = bgra ? 0 : 2;
	for (int oy = 0; oy < H; oy++) {
		unsigned char *out = raw + (size_t)oy * row;
		*out++ = 1; /* Sub: each byte less the one a pixel before */
		unsigned char last[3] = { 0, 0, 0 };
		const int ya = y0 + oy * f, yb = ya + f < y0 + ch ? ya + f : y0 + ch;
		for (int ox = 0; ox < W; ox++) {
			const int xa = x0 + ox * f,
					  xb = xa + f < x0 + cw ? xa + f : x0 + cw;
			uint32_t sum[3] = { 0, 0, 0 }, cnt = 0;
			for (int y = ya; y < yb; y++)
				for (int x = xa; x < xb; x++) {
					const unsigned char *s = px + ((size_t)y * w + x) * 4u;
					sum[0] += s[r];
					sum[1] += s[1];
					sum[2] += s[bl];
					cnt++;
				}
			for (int c = 0; c < 3; c++) {
				const unsigned char v =
					(unsigned char)((sum[c] + cnt / 2) / cnt);
				*out++  = (unsigned char)(v - last[c]);
				last[c] = v;
			}
		}
	}
	*ow    = W;
	*oh    = H;
	*raw_n = row * (size_t)H;
	return raw;
}

/* ---- the tools ----------------------------------------------------------- */

/* A text item in a tool's content; say() is a failure's, note() a
 * success's, each returning what the tool returns. */
static void text_item(Sb *c, const char *s)
{
	if (c->n)
		sb_s(c, ",");
	sb_s(c, "{\"type\":\"text\",\"text\":");
	sb_jstr(c, s);
	sb_s(c, "}");
}

static bool say(Sb *c, const char *fmt, ...) FNL_PRINTF(2, 3);
static bool say(Sb *c, const char *fmt, ...)
{
	char t[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(t, sizeof t, fmt, ap);
	va_end(ap);
	text_item(c, t);
	return true;
}

static bool note(Sb *c, const char *fmt, ...) FNL_PRINTF(2, 3);
static bool note(Sb *c, const char *fmt, ...)
{
	char t[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(t, sizeof t, fmt, ap);
	va_end(ap);
	text_item(c, t);
	return false;
}

static bool stopping(Mcp *m)
{
	return fnl_atomic_load_bool(&m->stop);
}

/* Waits `s` seconds, or until the port stops. */
static void pause_s(Mcp *m, double s)
{
	const double end = now_seconds() + s;
	fnl_mutex_lock(&m->lock);
	while (!stopping(m)) {
		const double left = end - now_seconds();
		if (left <= 0.0)
			break;
		(void)fnl_cond_wait_for(&m->cond, &m->lock, left);
	}
	fnl_mutex_unlock(&m->lock);
}

/* An optional number: absent leaves `v` alone, present and not a number is
 * false. */
static bool opt_num(const char *args, const char *key, double *v)
{
	const char *p = json_get(args, key);
	return !p || json_num(p, v);
}

/* An optional whole number in [lo, hi]: absent leaves `v`, present and
 * anything else is false -- 2.7 is not a count, and nor is 9 of 1 to 3. */
static bool opt_int(const char *args, const char *key, int lo, int hi, int *v)
{
	const char *p = json_get(args, key);
	double d;
	if (!p)
		return true;
	if (!json_num(p, &d) || d != floor(d) || d < lo || d > hi)
		return false;
	*v = (int)d;
	return true;
}

static bool ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

/* The input to the pump, and the wait for its turn to take it. */
static const char *submit(Mcp *m, const In *in, int n, const char *text)
{
	fnl_mutex_lock(&m->lock);
	if (m->in_n) {
		fnl_mutex_unlock(&m->lock);
		return "the last input is still waiting for the window's thread: "
			   "is the program pumping (fnl_run(), fnl_pump())?";
	}
	memcpy(m->in, in, (size_t)n * sizeof *in);
	m->in_n = n;
	if (text)
		snprintf(m->text, sizeof m->text, "%s", text);
	const uint32_t gen = m->in_done;
	fnl_mutex_unlock(&m->lock);

	app_wakeup(m->app);

	const double end = now_seconds() + MCP_WAIT_S;
	fnl_mutex_lock(&m->lock);
	while (m->in_done == gen && !stopping(m)) {
		const double left = end - now_seconds();
		if (left <= 0.0)
			break;
		(void)fnl_cond_wait_for(&m->cond, &m->lock, left);
	}
	const bool taken = m->in_done != gen;
	fnl_mutex_unlock(&m->lock);
	return taken ? NULL
				 : "the window's thread did not take the input within 2 s: "
				   "is the program pumping (fnl_run(), fnl_pump())?";
}

/* A client that goes away leaves nothing held: every key and button the
 * port pressed and did not release is released, where it was pressed. */
static void release_all(Mcp *m)
{
	In in[MCP_IN_MAX];
	int n = 0;
	for (int k = 1; k < 32 * KEY_HELD_WORDS; k++) {
		if (!((m->key_held[k / 32] >> (k % 32)) & 1u))
			continue;
		in[n++] = (In){ .kind = IN_KEY, .code = k };
		if (n == MCP_IN_MAX) {
			(void)submit(m, in, n, NULL);
			n = 0;
		}
	}
	for (int b = 0; b < 3; b++)
		if ((m->button_held >> b) & 1u) {
			if (n == MCP_IN_MAX) {
				(void)submit(m, in, n, NULL);
				n = 0;
			}
			in[n++] = (In){ .kind = IN_BUTTON, .code = b, .x = m->ptr_x,
							.y = m->ptr_y };
		}
	if (n)
		(void)submit(m, in, n, NULL);
	for (int i = 0; i < KEY_HELD_WORDS; i++)
		m->key_held[i] = 0;
	m->button_held = 0;
}

static void held_set(Mcp *m, FnlKey k, bool down)
{
	const unsigned bit = 1u << ((unsigned)k % 32);
	if (down)
		m->key_held[(unsigned)k / 32] |= bit;
	else
		m->key_held[(unsigned)k / 32] &= ~bit;
}

/* ---- state --------------------------------------------------------------- */

static const char *const AA_NAMES[]   = { "none", "fsaa", "mlaa", "taau" };
static const char *const GPU_KINDS[]  = { "unknown", "integrated", "discrete",
										  "virtual", "software" };

static void sb_bool(Sb *b, bool v)
{
	sb_s(b, v ? "true" : "false");
}

/* The lists the titlebar's drop-downs show, and what the window holds that
 * a caller can read: the GPUs, the outputs, the modes, the title, the
 * clipboard and the composition. */
static void state_lists(Sb *s, App *a)
{
	sb_key(s, "gpus");
	sb_s(s, "[");
	for (int i = 0, n = fnl_gpu_count(a); i < n; i++) {
		const unsigned kind = (unsigned)fnl_gpu_kind_at(a, i);
		sb_s(s, i ? ",{" : "{");
		sb_key(s, "name");
		sb_jstr(s, fnl_gpu_name_at(a, i));
		sb_key(s, "kind");
		sb_jstr(s, kind < 5 ? GPU_KINDS[kind] : "?");
		sb_key(s, "current");
		sb_bool(s, i == fnl_gpu_current(a));
		sb_s(s, "}");
	}
	sb_s(s, "]");

	sb_key(s, "outputs");
	sb_s(s, "[");
	for (int i = 0, n = fnl_output_count(a); i < n; i++) {
		if (i)
			sb_s(s, ",");
		sb_jstr(s, fnl_output_name_at(a, i));
	}
	sb_s(s, "]");

	FnlMode cur;
	const bool have_cur = fnl_mode_current(a, &cur);
	sb_key(s, "modes");
	sb_s(s, "[");
	for (int i = 0, n = fnl_mode_count(a); i < n; i++) {
		FnlMode md;
		if (!fnl_mode_get(a, i, &md))
			continue;
		sb_f(s, "%s{\"w\":%d,\"h\":%d,\"hz\":", s->p[s->n - 1] == '[' ? "" : ",",
			 md.w_px, md.h_px);
		sb_num(s, md.refresh_mhz / 1000.0);
		sb_key(s, "current");
		sb_bool(s, have_cur && md.w_px == cur.w_px && md.h_px == cur.h_px &&
					   md.refresh_mhz == cur.refresh_mhz);
		sb_s(s, "}");
	}
	sb_s(s, "]");

	char title[TITLE_MAX];
	fnl_mutex_lock(&a->lock);
	memcpy(title, a->title, sizeof title);
	fnl_mutex_unlock(&a->lock);
	title[TITLE_MAX - 1] = '\0';
	sb_key(s, "title");
	sb_jstr(s, title);

	char buf[1024];
	(void)fnl_clipboard_text(a, buf, sizeof buf);
	sb_key(s, "clipboard");
	sb_jstr(s, buf);
	(void)fnl_compose_text(a, buf, sizeof buf);
	sb_key(s, "compose");
	sb_jstr(s, buf);
}

static bool tool_state(Mcp *m, const char *args, Sb *c)
{
	App *a = m->app;
	bool live = m->live;
	const char *lv = json_get(args, "live");
	if (lv && !json_bool(lv, &live))
		return say(c, "live: true or false");
	/* The figures below are the last drawn frame's, kept while nothing is
	 * drawn (fnl_fps()); sampling draws a frame five times a second at rest
	 * to keep them current, and is the port's to turn off with the client
	 * that asked. */
	if (live != m->live) {
		fnl_set_cost_sampling(a, live);
		m->live = live;
	}

	int ww = 0, wh = 0, cw = 0, ch = 0, sw = 0, sh = 0, ow = 0, oh = 0;
	int rx = 0, ry = 0, rw = 0, rh = 0;
	fnl_window_size_pt(a, &ww, &wh);
	fnl_content_rect_pt(a, &rx, &ry, &rw, &rh);
	fnl_content_size_px(a, &cw, &ch);
	fnl_scene_size_px(a, &sw, &sh);
	fnl_output_size_pt(a, &ow, &oh);
	const FnlAntialias aa = fnl_antialias(a);

	Sb s = { 0 };
	sb_s(&s, "{");
	sb_key(&s, "backend");
	sb_jstr(&s, fnl_backend_name(a));
	sb_key(&s, "renderer");
	sb_jstr(&s, fnl_renderer_name(a));
	sb_key(&s, "gpu");
	sb_jstr(&s, fnl_gpu_name(a));
	sb_key(&s, "present_mode");
	sb_jstr(&s, fnl_present_mode(a));
	sb_key(&s, "vsync");
	sb_bool(&s, fnl_vsync(a));
	sb_key(&s, "window_pt");
	sb_f(&s, "[%d,%d]", ww, wh);
	sb_key(&s, "content_rect_pt");
	sb_f(&s, "[%d,%d,%d,%d]", rx, ry, rw, rh);
	sb_key(&s, "titlebar_pt");
	sb_f(&s, "%d", fnl_titlebar_height(a));
	sb_key(&s, "content_px");
	sb_f(&s, "[%d,%d]", cw, ch);
	sb_key(&s, "scene_px");
	sb_f(&s, "[%d,%d]", sw, sh);
	sb_key(&s, "scale120");
	sb_f(&s, "%d", fnl_scale120(a));
	sb_key(&s, "output");
	sb_jstr(&s, fnl_output_name(a));
	sb_key(&s, "output_pt");
	sb_f(&s, "[%d,%d]", ow, oh);
	sb_key(&s, "output_hz");
	sb_num(&s, fnl_output_refresh_mhz(a) / 1000.0);
	sb_key(&s, "fullscreen");
	sb_bool(&s, fnl_is_fullscreen(a));
	sb_key(&s, "maximized");
	sb_bool(&s, fnl_is_maximized(a));
	sb_key(&s, "active");
	sb_bool(&s, fnl_is_active(a));
	sb_key(&s, "shown");
	sb_bool(&s, fnl_is_shown(a));
	sb_key(&s, "pointer_captured");
	sb_bool(&s, fnl_is_pointer_captured(a));
	/* Where the port last put the pointer, in window points: null before
	 * it has. A device's own motion is not seen from here. */
	sb_key(&s, "pointer_pt");
	if (m->ptr_known)
		sb_f(&s, "[%d,%d]", (int)lround(m->ptr_x), (int)lround(m->ptr_y));
	else
		sb_s(&s, "null");
	sb_key(&s, "text_input");
	sb_bool(&s, fnl_is_text_input(a));
	sb_key(&s, "antialias");
	sb_jstr(&s, (unsigned)aa < 4 ? AA_NAMES[aa] : "?");
	sb_key(&s, "windows");
	sb_f(&s, "%u", (unsigned)fnl_atomic_load_u32(&a->host->window_count));
	sb_key(&s, "live");
	sb_bool(&s, m->live);
	/* The three below are the last drawn frame's unless `live`. */
	sb_key(&s, "fps");
	sb_f(&s, "%d", fnl_fps(a));
	sb_key(&s, "frame_cpu_ms");
	sb_num(&s, fnl_frame_cpu_ms(a));
	sb_key(&s, "frame_gpu_ms");
	sb_num(&s, fnl_frame_gpu_ms(a));
	sb_key(&s, "input_latency_ms");
	sb_num(&s, fnl_input_latency_ms(a));
	sb_key(&s, "keys_down");
	sb_s(&s, "[");
	bool first = true;
	for (int k = 1; k < 32 * KEY_HELD_WORDS; k++)
		if (echo_key_known((FnlKey)k) && fnl_key_down(a, (FnlKey)k)) {
			if (!first)
				sb_s(&s, ",");
			sb_jstr(&s, fnl_key_name((FnlKey)k));
			first = false;
		}
	sb_s(&s, "]");
	state_lists(&s, a);
	sb_s(&s, "}");

	const bool bad = s.bad;
	if (!bad)
		text_item(c, s.p);
	free(s.p);
	return bad ? say(c, "out of memory") : false;
}

/* ---- events -------------------------------------------------------------- */

static bool ev_has(const Mcp *m, const char *until)
{
	for (int i = 0; i < m->ev_n; i++)
		if (strstr(m->ev[(m->ev_head + i) % MCP_EVENTS], until))
			return true;
	return false;
}

static bool tool_events(Mcp *m, const char *args, Sb *c)
{
	double wait_ms = 0.0;
	char until[64] = "";
	if (!opt_num(args, "wait_ms", &wait_ms) || wait_ms < 0.0 ||
		wait_ms > 10000.0)
		return say(c, "wait_ms: milliseconds, 0 to 10000");
	const char *uv = json_get(args, "until");
	if (uv && (!json_str(uv, until, sizeof until) || !*until))
		return say(c, "until: a non-empty string of fewer than %d bytes",
				   (int)sizeof until);

	Sb t = { 0 };
	const double end = now_seconds() + wait_ms / 1e3;
	fnl_mutex_lock(&m->lock);
	/* Without `until`, the first line ends the wait; with it, the first
	 * line that holds the text. */
	while (!stopping(m) && (*until ? !ev_has(m, until) : !m->ev_n && !m->ev_lost)) {
		const double left = end - now_seconds();
		if (left <= 0.0)
			break;
		(void)fnl_cond_wait_for(&m->cond, &m->lock, left);
	}
	/* The program polls a frame's events together, a frame apart: the first
	 * line is the start of a batch, and the rest of it is a few
	 * milliseconds behind. */
	while (!*until && wait_ms > 0.0 && m->ev_n && !stopping(m)) {
		const double now = now_seconds();
		double left = m->ev_last_s + 0.04 - now;
		if (end - now < left)
			left = end - now;
		if (left <= 0.0)
			break;
		(void)fnl_cond_wait_for(&m->cond, &m->lock, left);
	}
	const uint32_t lost = m->ev_lost;
	if (lost)
		sb_f(&t, "(%u older events dropped: over %d between two calls)\n",
			 (unsigned)lost, MCP_EVENTS);
	for (int i = 0; i < m->ev_n; i++) {
		sb_s(&t, m->ev[(m->ev_head + i) % MCP_EVENTS]);
		sb_s(&t, "\n");
	}
	const bool matched = *until && ev_has(m, until);
	m->ev_head = m->ev_n = 0;
	m->ev_lost = 0;
	fnl_mutex_unlock(&m->lock);

	if (*until && !matched)
		sb_f(&t, "(no event with '%s' within %d ms)\n", until, (int)wait_ms);
	const bool bad = t.bad;
	if (bad)
		say(c, "out of memory");
	else
		text_item(c, t.n ? t.p : "no events");
	free(t.p);
	return bad;
}

/* ---- keys ---------------------------------------------------------------- */

/* Names a person types and fnl_key_name() does not spell. */
static const struct {
	const char *alias, *key;
} KEY_ALIASES[] = {
	{ "esc", "Escape" },     { "return", "Enter" },    { "ctrl", "LCtrl" },
	{ "control", "LCtrl" },  { "shift", "LShift" },    { "alt", "LAlt" },
	{ "super", "LSuper" },   { "win", "LSuper" },      { "meta", "LSuper" },
	{ "cmd", "LSuper" },     { "pgup", "PageUp" },     { "pgdn", "PageDown" },
	{ "pagedn", "PageDown" }, { "del", "Delete" },     { "ins", "Insert" },
	{ "bksp", "Backspace" }, { "kp_enter", "KPEnter" }, { "arrowup", "Up" },
	{ "arrowdown", "Down" }, { "arrowleft", "Left" },  { "arrowright", "Right" },
	{ "printscr", "PrintScreen" },
};

static FnlKey key_by_name(const char *name)
{
	for (size_t i = 0; i < sizeof KEY_ALIASES / sizeof KEY_ALIASES[0]; i++)
		if (ieq(name, KEY_ALIASES[i].alias))
			return fnl_key_from_name(KEY_ALIASES[i].key);
	return fnl_key_from_name(name);
}

/* "modifiers":["Shift","Ctrl"], held round the input: up to four keys.
 * NULL, or what is wrong. */
static const char *mods_parse(const char *args, FnlKey *mods, int *n)
{
	*n = 0;
	const char *v = json_get(args, "modifiers");
	if (!v)
		return NULL;
	if (*v != '[')
		return "modifiers: an array of key names, Shift Ctrl Alt Super";
	const char *p = v + 1;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
			   *p == ',')
			p++;
		if (*p == ']')
			return NULL;
		char nm[16];
		if (!json_str(p, nm, sizeof nm) || !(p = json_skip(p)))
			return "modifiers: an array of key names, Shift Ctrl Alt Super";
		const FnlKey k = key_by_name(nm);
		if (k == FNL_KEY_UNKNOWN || *n == 4)
			return "modifiers: at most four of Shift Ctrl Alt Super";
		mods[(*n)++] = k;
	}
}

static bool tool_key(Mcp *m, const char *args, Sb *c)
{
	char name[32], act[16] = "tap";
	if (!json_str(json_get(args, "key"), name, sizeof name))
		return say(c, "key: a name from the schema's list (Esc, Return, "
					  "Ctrl, Shift, PgUp, Del and the like are taken too)");
	const FnlKey k = key_by_name(name);
	if (k == FNL_KEY_UNKNOWN)
		return say(c, "no key named '%s': tools/list has the names", name);
	const char *av = json_get(args, "action");
	if (av && !json_str(av, act, sizeof act))
		return say(c, "action: tap, press, release or repeat");
	const bool tap = !strcmp(act, "tap"), down = !strcmp(act, "press"),
			   up = !strcmp(act, "release"), rep = !strcmp(act, "repeat");
	if (!tap && !down && !up && !rep)
		return say(c, "action '%s': tap, press, release or repeat", act);
	FnlKey mods[4];
	int nm;
	const char *bad = mods_parse(args, mods, &nm);
	if (bad)
		return say(c, "%s", bad);

	In in[MCP_IN_MAX];
	int n = 0;
	for (int i = 0; i < nm; i++)
		in[n++] = (In){ .kind = IN_KEY, .code = (int)mods[i], .pressed = true };
	if (rep) {
		in[n++] = (In){ .kind = IN_REPEAT, .code = (int)k };
	} else {
		if (tap || down)
			in[n++] = (In){ .kind = IN_KEY, .code = (int)k, .pressed = true };
		if (tap || up)
			in[n++] = (In){ .kind = IN_KEY, .code = (int)k, .pressed = false };
	}
	for (int i = nm - 1; i >= 0; i--)
		in[n++] = (In){ .kind = IN_KEY, .code = (int)mods[i] };
	const char *err = submit(m, in, n, NULL);
	if (err)
		return say(c, "%s", err);
	if (down)
		held_set(m, k, true);
	if (up)
		held_set(m, k, false);
	return note(c, "%s: %s", fnl_key_name(k), act);
}

/* The schema of `key`, with the names the library spells keys by. */
static void key_schema(Sb *o)
{
	sb_s(o, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\","
			"\"description\":\"A key. Esc, Return, Ctrl, Shift, Alt, Super, "
			"PgUp, PgDn, Del, Ins and Bksp are taken as well, and the case "
			"is no matter.\",\"enum\":[");
	bool first = true;
	for (int k = 1; k < 32 * KEY_HELD_WORDS; k++)
		if (echo_key_known((FnlKey)k)) {
			if (!first)
				sb_s(o, ",");
			sb_jstr(o, fnl_key_name((FnlKey)k));
			first = false;
		}
	sb_s(o, "]},\"action\":{\"type\":\"string\",\"enum\":[\"tap\",\"press\","
			"\"release\",\"repeat\"]},\"modifiers\":{\"type\":\"array\","
			"\"maxItems\":4,\"items\":{\"type\":\"string\",\"enum\":[\"Shift\","
			"\"Ctrl\",\"Alt\",\"Super\"]}}},\"required\":[\"key\"]}");
}

/* ---- text ---------------------------------------------------------------- */

static bool tool_text(Mcp *m, const char *args, Sb *c)
{
	char text[MCP_TYPE_MAX];
	if (!json_str(json_get(args, "text"), text, sizeof text) || !*text)
		return say(c, "text: a string of 1 to %d bytes", MCP_TYPE_MAX - 1);
	if (!fnl_is_text_input(m->app))
		return say(c, "text entry is off: the program has not called "
					  "fnl_set_text_input()");
	/* Characters, not keys: Enter, Tab and Backspace are key's. */
	for (const char *p = text; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f)
			return say(c, "text: no control characters (Enter, Tab and "
						  "Backspace are keys)");

	/* Cut into chunks that the queue has room for, each waiting for it: the
	 * queue drops what it has no room for (events.c), and a call that
	 * said "typed" over characters that never arrived would be a lie. */
	size_t typed = 0, total = 0;
	for (const char *p = text; *p;) {
		uint32_t cp;
		bool ok;
		p = utf8_decode(p, &cp, &ok);
		total++;
	}
	for (const char *p = text; *p;) {
		char chunk[MCP_TEXT_MAX];
		size_t bytes = 0, cps = 0;
		while (*p && cps < MCP_CHUNK_CP) {
			uint32_t cp;
			bool ok;
			const char *q = utf8_decode(p, &cp, &ok);
			if (bytes + (size_t)(q - p) >= sizeof chunk)
				break;
			memcpy(chunk + bytes, p, (size_t)(q - p));
			bytes += (size_t)(q - p);
			cps++;
			p = q;
		}
		chunk[bytes] = '\0';

		const double end = now_seconds() + MCP_WAIT_S;
		while (app_events_pending(m->app) > EVENT_QUEUE_MAX / 2 &&
			   now_seconds() < end && !stopping(m))
			pause_s(m, 0.005);
		if (app_events_pending(m->app) > EVENT_QUEUE_MAX / 2)
			return say(c, "typed %zu of %zu characters: the program is not "
						  "draining its events", typed, total);
		const In in = { .kind = IN_TEXT };
		const char *err = submit(m, &in, 1, chunk);
		if (err)
			return say(c, "typed %zu of %zu characters: %s", typed, total,
					   err);
		typed += cps;
	}
	return note(c, "typed %zu characters", typed);
}

/* ---- the pointer --------------------------------------------------------- */

static bool tool_pointer(Mcp *m, const char *args, Sb *c)
{
	char act[16], btn[16] = "left";
	double x = 0.0, y = 0.0, dx = 0.0, dy = 0.0, tx = 0.0, ty = 0.0;
	int count = 1, steps = 10;
	if (!json_str(json_get(args, "action"), act, sizeof act))
		return say(c, "action: move, press, release, click, scroll, drag or "
					  "relative");
	const bool relative = !strcmp(act, "relative"), drag = !strcmp(act, "drag");
	if (!relative && (!json_num(json_get(args, "x"), &x) ||
					  !json_num(json_get(args, "y"), &y) || fabs(x) > 1e6 ||
					  fabs(y) > 1e6))
		return say(c, "x and y: window points");
	if (!opt_num(args, "dx", &dx) || !opt_num(args, "dy", &dy) ||
		fabs(dx) > 100.0 || fabs(dy) > 100.0)
		return say(c, "dx and dy: at most 100 (notches for scroll, points "
					  "for relative)");
	if (!opt_int(args, "count", 1, 3, &count))
		return say(c, "count: 1, 2 or 3");
	if (!opt_int(args, "steps", 1, 120, &steps))
		return say(c, "steps: a whole number, 1 to 120");
	if (drag && (!json_num(json_get(args, "to_x"), &tx) ||
				 !json_num(json_get(args, "to_y"), &ty) || fabs(tx) > 1e6 ||
				 fabs(ty) > 1e6))
		return say(c, "to_x and to_y: where the drag ends, window points");
	const char *bv = json_get(args, "button");
	if (bv && !json_str(bv, btn, sizeof btn))
		return say(c, "button: left, right or middle");
	FnlButton b;
	if (!strcmp(btn, "left"))
		b = FNL_BUTTON_LEFT;
	else if (!strcmp(btn, "right"))
		b = FNL_BUTTON_RIGHT;
	else if (!strcmp(btn, "middle"))
		b = FNL_BUTTON_MIDDLE;
	else
		return say(c, "button '%s': left, right or middle", btn);
	FnlKey mods[4];
	int nm;
	const char *bad = mods_parse(args, mods, &nm);
	if (bad)
		return say(c, "%s", bad);

	In in[MCP_IN_MAX];
	int n = 0;
	for (int i = 0; i < nm; i++)
		in[n++] = (In){ .kind = IN_KEY, .code = (int)mods[i], .pressed = true };
	const In press   = { .kind = IN_BUTTON, .code = (int)b, .pressed = true,
						 .x = x, .y = y };
	const In release = { .kind = IN_BUTTON, .code = (int)b, .x = x, .y = y };
	bool pressing = false, releasing = false;
	/* Moved there first, as a hand is, so that the hover and the press's
	 * place agree. */
	if (!relative)
		in[n++] = (In){ .kind = IN_MOTION, .x = x, .y = y };
	if (!strcmp(act, "press")) {
		in[n++]  = press;
		pressing = true;
	} else if (!strcmp(act, "release")) {
		in[n++]   = release;
		releasing = true;
	} else if (!strcmp(act, "click")) {
		for (int i = 0; i < count; i++) {
			in[n++] = press;
			in[n++] = release;
		}
		pressing = true;
	} else if (!strcmp(act, "scroll")) {
		if (dx == 0.0 && dy == 0.0)
			return say(c, "scroll: dx or dy, in notches, not both zero");
		in[n++] = (In){ .kind = IN_SCROLL, .x = x, .y = y, .dx = dx, .dy = dy };
	} else if (relative) {
		if (dx == 0.0 && dy == 0.0)
			return say(c, "relative: dx or dy, in points, not both zero");
		in[n++] = (In){ .kind = IN_RELATIVE, .dx = dx, .dy = dy };
	} else if (!drag && strcmp(act, "move") != 0) {
		return say(c, "action '%s': move, press, release, click, scroll, drag "
					  "or relative", act);
	}

	/* A press close behind another is the next of its run (the library's
	 * click count), which two separate calls do not mean: a click that is
	 * not a double-click waits out the interval, and `count` is the double. */
	if ((pressing || drag) && count == 1) {
		const double gap = (DECO_DBLCLICK_MS + 60) / 1e3 - (now_seconds() - m->press_s);
		if (gap > 0.0)
			pause_s(m, gap);
	}

	const char *err;
	if (drag) {
		/* A press, the motion in steps a frame apart, a release: what a
		 * titlebar drag or a resize edge wants to see. */
		in[n++] = press;
		if ((err = submit(m, in, n, NULL)))
			return say(c, "%s", err);
		m->button_held |= 1u << (unsigned)b;
		pause_s(m, 0.05);
		for (int i = 1; i <= steps && !stopping(m); i++) {
			const In mv = { .kind = IN_MOTION,
							.x = x + (tx - x) * i / steps,
							.y = y + (ty - y) * i / steps };
			if ((err = submit(m, &mv, 1, NULL)))
				return say(c, "%s", err);
			pause_s(m, 0.017);
		}
		n = 0;
		in[n++] = (In){ .kind = IN_BUTTON, .code = (int)b, .x = tx, .y = ty };
		for (int i = nm - 1; i >= 0; i--)
			in[n++] = (In){ .kind = IN_KEY, .code = (int)mods[i] };
		if ((err = submit(m, in, n, NULL)))
			return say(c, "%s", err);
		m->button_held &= ~(1u << (unsigned)b);
		m->press_s = now_seconds();
		m->ptr_x = tx;
		m->ptr_y = ty;
		m->ptr_known = true;
		return note(c, "drag: done");
	}

	for (int i = nm - 1; i >= 0; i--)
		in[n++] = (In){ .kind = IN_KEY, .code = (int)mods[i] };
	if ((err = submit(m, in, n, NULL)))
		return say(c, "%s", err);
	if (!relative) {
		m->ptr_x     = x;
		m->ptr_y     = y;
		m->ptr_known = true;
	}
	if (pressing)
		m->press_s = now_seconds();
	if (act[0] == 'p' && pressing) /* press */
		m->button_held |= 1u << (unsigned)b;
	if (releasing)
		m->button_held &= ~(1u << (unsigned)b);
	return note(c, "%s: done", act);
}

/* ---- capture ------------------------------------------------------------- */

/* The next frame presented, read back. A move under way is waited out. */
static ShotState shot_take(Mcp *m, unsigned char **px, int *w, int *h,
						   bool *bgra)
{
	const double give_up = now_seconds() + MCP_MOVE_S;
	for (;;) {
		fnl_mutex_lock(&m->lock);
		free(m->shot_px);
		m->shot_px = NULL;
		m->shot    = SHOT_WANT;
		fnl_mutex_unlock(&m->lock);

		/* The post is a reason to draw, so a window at rest draws the
		 * frame that is read back. */
		const bool posted = app_post(m->app, shot_arm, m->app);

		const double end = now_seconds() + MCP_WAIT_S;
		fnl_mutex_lock(&m->lock);
		while (posted && m->shot == SHOT_WANT && !stopping(m)) {
			const double left = end - now_seconds();
			if (left <= 0.0)
				break;
			(void)fnl_cond_wait_for(&m->cond, &m->lock, left);
		}
		ShotState st = m->shot;
		*px   = m->shot_px;
		*w    = m->shot_w;
		*h    = m->shot_h;
		*bgra = m->shot_bgra;
		m->shot_px = NULL;
		m->shot    = SHOT_IDLE;
		fnl_mutex_unlock(&m->lock);

		if (!posted)
			return SHOT_IDLE;
		if (st == SHOT_MOVING && now_seconds() < give_up && !stopping(m)) {
			pause_s(m, 0.1);
			continue;
		}
		return st;
	}
}

static bool tool_capture(Mcp *m, const char *args, Sb *c)
{
	double max_w = 1024.0;
	if (!opt_num(args, "max_width", &max_w) || max_w < 0.0 ||
		(max_w != 0.0 && max_w != floor(max_w)))
		return say(c, "max_width: a whole number of pixels, 0 for all of "
					  "them");
	int rg[4] = { 0, 0, 0, 0 };
	const char *rv = json_get(args, "region");
	if (rv && (!opt_int(rv, "x", 0, 1 << 20, &rg[0]) ||
			   !opt_int(rv, "y", 0, 1 << 20, &rg[1]) ||
			   !opt_int(rv, "w", 1, 1 << 20, &rg[2]) ||
			   !opt_int(rv, "h", 1, 1 << 20, &rg[3]) || !rg[2] || !rg[3]))
		return say(c, "region: {x, y, w, h} in frame pixels, w and h at "
					  "least 1");

	/* A client that has hung up asked for nothing: the read-back and the
	 * encoding are a few hundred milliseconds for no one. */
	if (m->cur != SOCK_NONE && sock_gone(m->cur))
		return say(c, "the client has gone");

	unsigned char *px = NULL;
	int w = 0, h = 0;
	bool bgra = false;
	const ShotState st = shot_take(m, &px, &w, &h, &bgra);
	if (st == SHOT_IDLE)
		return say(c, "the frame's post queue is full: ask again");
	if (st == SHOT_MOVING)
		return say(c, "the window is still moving to another GPU or "
					  "renderer after %d s: ask again", (int)MCP_MOVE_S);
	if (st == SHOT_NONE)
		return say(c, "this frame cannot be read back here (an HDR "
					  "swapchain, or one whose images cannot be copied)");
	if (st != SHOT_DONE || !px)
		return say(c, "no frame drawn within 2 s: is the window shown, and "
					  "the program running?");

	int x0 = 0, y0 = 0, cw = w, ch = h;
	if (rv) {
		if (rg[0] + rg[2] > w || rg[1] + rg[3] > h) {
			free(px);
			return say(c, "region %d,%d %dx%d is outside the frame, which is "
						  "%dx%d px", rg[0], rg[1], rg[2], rg[3], w, h);
		}
		x0 = rg[0];
		y0 = rg[1];
		cw = rg[2];
		ch = rg[3];
	}
	const int f = max_w >= 1.0 && cw > max_w ? (int)ceil(cw / max_w) : 1;
	int ow = 0, oh = 0;
	size_t raw_n = 0, png_n = 0;
	unsigned char *raw = shrink(px, w, bgra, x0, y0, cw, ch, f, &ow, &oh, &raw_n);
	free(px);
	unsigned char *png = raw ? png_encode(raw, raw_n, ow, oh, &png_n) : NULL;
	free(raw);
	if (!png)
		return say(c, "out of memory");

	if (c->n)
		sb_s(c, ",");
	sb_s(c, "{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
	sb_base64(c, png, png_n);
	sb_s(c, "\"}");
	free(png);
	return note(c, "frame %dx%d px, region %d,%d %dx%d, image %dx%d (a pixel "
				   "of it is %d of the frame's each way); scale120 %d: a "
				   "window point is scale120/120 frame pixels",
				w, h, x0, y0, cw, ch, ow, oh, f, fnl_scale120(m->app));
}

/* ---- the library's own controls ------------------------------------------ */

static bool tool_controls(Mcp *m, const char *args, Sb *c)
{
	(void)args;
	const App *a = m->app;
	int ww = 0, wh = 0;
	fnl_window_size_pt(a, &ww, &wh);
	CtlInfo ci[64];
	const int n = ctl_describe(a, ww, ci, 64);

	Sb s = { 0 };
	sb_s(&s, "{");
	sb_key(&s, "titlebar_pt");
	sb_f(&s, "%d", fnl_titlebar_height(a));
	sb_key(&s, "controls");
	sb_s(&s, "[");
	for (int i = 0; i < n; i++) {
		sb_s(&s, i ? ",{" : "{");
		sb_key(&s, "name");
		sb_jstr(&s, ci[i].name);
		sb_key(&s, "rect_pt");
		sb_f(&s, "[%d,%d,%d,%d]", ci[i].x, ci[i].y, ci[i].w, ci[i].h);
		sb_key(&s, "enabled");
		sb_bool(&s, ci[i].enabled);
		if (ci[i].on >= 0) {
			sb_key(&s, "on");
			sb_bool(&s, ci[i].on != 0);
		}
		if (ci[i].label[0]) {
			sb_key(&s, "label");
			sb_jstr(&s, ci[i].label);
		}
		sb_s(&s, "}");
	}
	sb_s(&s, "]}");
	const bool bad = s.bad;
	if (!bad)
		text_item(c, s.p);
	free(s.p);
	return bad ? say(c, "out of memory") : false;
}

/* The knobs a caller has, acted on as the titlebar acts on them: asked for,
 * and what came of it is in `state` a frame or two on. */
static bool tool_window(Mcp *m, const char *args, Sb *c)
{
	App *a = m->app;
	Sb done = { 0 };
	bool bv;
	const char *v;
	char buf[TITLE_MAX];

	if ((v = json_get(args, "fullscreen"))) {
		if (!json_bool(v, &bv))
			return say(c, "fullscreen: true or false");
		fnl_set_fullscreen(a, bv);
		sb_s(&done, " fullscreen");
	}
	if ((v = json_get(args, "maximized"))) {
		if (!json_bool(v, &bv))
			return say(c, "maximized: true or false");
		fnl_set_maximized(a, bv);
		sb_s(&done, " maximized");
	}
	if ((v = json_get(args, "minimize"))) {
		if (!json_bool(v, &bv))
			return say(c, "minimize: true");
		if (bv)
			fnl_minimize(a);
		sb_s(&done, " minimize");
	}
	if ((v = json_get(args, "antialias"))) {
		int k = -1;
		if (!json_str(v, buf, sizeof buf))
			return say(c, "antialias: none, fsaa, mlaa or taau");
		for (int i = 0; i < 4; i++)
			if (ieq(buf, AA_NAMES[i]))
				k = i;
		if (k < 0)
			return say(c, "antialias '%s': none, fsaa, mlaa or taau", buf);
		fnl_set_antialias(a, (FnlAntialias)k);
		sb_s(&done, " antialias");
	}
	if ((v = json_get(args, "title"))) {
		if (!json_str(v, buf, sizeof buf))
			return say(c, "title: a string of fewer than %d bytes",
					   (int)sizeof buf);
		fnl_set_title(a, buf);
		sb_s(&done, " title");
	}
	if ((v = json_get(args, "vsync"))) {
		if (!json_bool(v, &bv))
			return say(c, "vsync: true or false");
		fnl_set_vsync(a, bv);
		sb_s(&done, " vsync");
	}
	if ((v = json_get(args, "metrics_panel"))) {
		if (!json_bool(v, &bv))
			return say(c, "metrics_panel: true or false");
		fnl_set_metrics_panel(a, bv);
		sb_s(&done, " metrics_panel");
	}
	if ((v = json_get(args, "text_input"))) {
		if (!json_bool(v, &bv))
			return say(c, "text_input: true or false");
		fnl_set_text_input(a, bv);
		sb_s(&done, " text_input");
	}
	if ((v = json_get(args, "gpu"))) {
		double d;
		int k = -1;
		if (json_num(v, &d) && d == floor(d) && d >= 0 && d < fnl_gpu_count(a))
			k = (int)d;
		else if (json_str(v, buf, sizeof buf) && *buf)
			for (int i = 0, n = fnl_gpu_count(a); i < n && k < 0; i++) {
				char lo[128], hay[128];
				snprintf(hay, sizeof hay, "%s", fnl_gpu_name_at(a, i));
				snprintf(lo, sizeof lo, "%s", buf);
				for (char *p = hay; *p; p++)
					*p = (char)tolower((unsigned char)*p);
				for (char *p = lo; *p; p++)
					*p = (char)tolower((unsigned char)*p);
				if (strstr(hay, lo))
					k = i;
			}
		if (k < 0)
			return say(c, "gpu: an index into state's gpus, or part of a "
						  "name");
		fnl_request_gpu(a, k);
		sb_s(&done, " gpu");
	}
	if ((v = json_get(args, "output"))) {
		if (!json_str(v, buf, sizeof buf))
			return say(c, "output: a name from state's outputs");
		fnl_request_output(a, buf);
		sb_s(&done, " output");
	}
	if ((v = json_get(args, "mode"))) {
		int w = 0, h = 0;
		double hz = 0.0;
		if (!opt_int(v, "w", 1, 1 << 16, &w) || !opt_int(v, "h", 1, 1 << 16, &h) ||
			!w || !h || !opt_num(v, "hz", &hz))
			return say(c, "mode: {w, h} and optionally hz, from state's "
						  "modes");
		FnlMode pick = { 0, 0, 0 };
		for (int i = 0, n = fnl_mode_count(a); i < n; i++) {
			FnlMode md;
			if (fnl_mode_get(a, i, &md) && md.w_px == w && md.h_px == h &&
				(hz == 0.0 || fabs(md.refresh_mhz - hz * 1000.0) < 10.0) &&
				md.refresh_mhz > pick.refresh_mhz)
				pick = md;
		}
		if (!pick.refresh_mhz)
			return say(c, "mode: no %dx%d in state's modes", w, h);
		fnl_request_mode(a, &pick);
		sb_s(&done, " mode");
	}
	if ((v = json_get(args, "size"))) {
		int w = 0, h = 0;
		if (!opt_int(v, "w", 1, 1 << 16, &w) || !opt_int(v, "h", 1, 1 << 16, &h) ||
			!w || !h)
			return say(c, "size: {w, h} in window points");
		fnl_request_size_pt(a, w, h);
		sb_s(&done, " size");
	}
	if ((v = json_get(args, "release_pointer"))) {
		if (!json_bool(v, &bv))
			return say(c, "release_pointer: true");
		if (bv)
			fnl_release_pointer(a);
		sb_s(&done, " release_pointer");
	}
	if ((v = json_get(args, "close"))) {
		if (!json_bool(v, &bv))
			return say(c, "close: true");
		if (bv)
			fnl_quit(a);
		sb_s(&done, " close");
	}
	const bool none = !done.n;
	const bool ok   = !done.bad;
	bool bad = true;
	if (none)
		say(c, "window: none of fullscreen, maximized, minimize, antialias, "
			   "title, vsync, metrics_panel, text_input, gpu, output, mode, "
			   "size, release_pointer, close");
	else if (!ok)
		say(c, "out of memory");
	else
		bad = note(c, "requested:%s; state has what came of it", done.p);
	free(done.p);
	return bad;
}

#ifndef NDEBUG
static bool tool_fault(Mcp *m, const char *args, Sb *c)
{
	(void)m;
	char spec[256];
	if (!json_str(json_get(args, "spec"), spec, sizeof spec) || !*spec)
		return say(c, "spec: as FNL_FAULT takes it");
	if (!fault_arm(spec))
		return say(c, "nothing changed: a name in '%s' is no fault site "
					  "(src/core/fault.c), or a count is not 1 to 1000000",
				   spec);
	return note(c, "applied: %s (a name with '-' before it is disarmed)", spec);
}
#endif

typedef bool (*ToolFn)(Mcp *m, const char *args, Sb *content);

/* Each tool's name, what it does, and its arguments' JSON Schema. `key`'s is
 * made in tools_list(), from the names the library has. */
static const struct {
	const char *name;
	ToolFn      fn;
	const char *desc;
	const char *schema;
} TOOLS[] = {
	{ "state", tool_state,
	  "The window as the library sees it: backend, renderer, GPU, sizes in "
	  "points and pixels, where the content sits in the window, scale, "
	  "output, fullscreen and maximized, pointer capture and where this port "
	  "last put the pointer, text entry, the keys held, the GPUs, outputs "
	  "and modes on offer, the title, the clipboard and the composition. "
	  "fps and the frame costs are the last drawn frame's and stay so while "
	  "the window rests: live=true keeps them measured, until the client "
	  "leaves or says live=false.",
	  "{\"type\":\"object\",\"properties\":{\"live\":{\"type\":\"boolean\"}}}" },
	{ "events", tool_events,
	  "The events the program has polled since the last call, oldest "
	  "first, one line each in the event echo's format (`key A press | "
	  "area ...`). Only what the program drains is seen, and only the last "
	  "256 between two calls (older ones are counted); a client that "
	  "connects starts from none. wait_ms waits that long at most for the "
	  "first, or with until for the first line that holds that text "
	  "(`gpu 1`, `antialias mlaa`).",
	  "{\"type\":\"object\",\"properties\":{\"wait_ms\":{\"type\":"
	  "\"integer\",\"minimum\":0,\"maximum\":10000},\"until\":{\"type\":"
	  "\"string\"}}}" },
	{ "key", tool_key,
	  "A key, as the backends hand one in. tap is a press and a release, "
	  "repeat is what a held key's auto-repeat sends (only while text entry "
	  "is on). modifiers are held round it. No character is typed from a key "
	  "here; text does that.",
	  NULL },
	{ "text", tool_text,
	  "Committed text, one FNL_EVENT_TEXT per code point, as an input "
	  "method hands it over: up to 4095 bytes, no control characters. Sent "
	  "as fast as the program drains its events, and the reply says how "
	  "many arrived. Only while the program has text entry on.",
	  "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
	  "\"required\":[\"text\"]}" },
	{ "pointer", tool_pointer,
	  "The pointer, in window points, the titlebar included (state has the "
	  "content's rectangle, controls the bar's controls): move, press, "
	  "release, click (count 2 is a double-click), scroll (dx, dy in "
	  "notches), drag (to_x, to_y in steps, the way a titlebar or a resize "
	  "edge is dragged) or relative (dx, dy in points: the only motion a "
	  "captured pointer takes). Where the backend draws its own cursor (KMS) "
	  "it follows. A click that is not a double-click waits out the "
	  "double-click interval after the last one. modifiers are held round it.",
	  "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\","
	  "\"enum\":[\"move\",\"press\",\"release\",\"click\",\"scroll\",\"drag\","
	  "\"relative\"]},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
	  "\"to_x\":{\"type\":\"number\"},\"to_y\":{\"type\":\"number\"},"
	  "\"steps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120},"
	  "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\","
	  "\"middle\"]},\"count\":{\"type\":\"integer\",\"minimum\":1,"
	  "\"maximum\":3},\"dx\":{\"type\":\"number\"},\"dy\":{\"type\":"
	  "\"number\"},\"modifiers\":{\"type\":\"array\",\"maxItems\":4,"
	  "\"items\":{\"type\":\"string\",\"enum\":[\"Shift\",\"Ctrl\",\"Alt\","
	  "\"Super\"]}}},\"required\":[\"action\"]}" },
	{ "capture", tool_capture,
	  "The next frame the window presents, read back from the GPU: a PNG of "
	  "what the program and the library drew, titlebar included, before "
	  "any compositor or cursor plane. max_width shrinks it by a whole "
	  "factor (1024 unless asked; 0 keeps every pixel); region crops first, "
	  "in frame pixels. A frame asked for while the window moves to "
	  "another GPU waits the move out.",
	  "{\"type\":\"object\",\"properties\":{\"max_width\":{\"type\":"
	  "\"integer\",\"minimum\":0},\"region\":{\"type\":\"object\","
	  "\"properties\":{\"x\":{\"type\":\"integer\",\"minimum\":0},\"y\":{"
	  "\"type\":\"integer\",\"minimum\":0},\"w\":{\"type\":\"integer\","
	  "\"minimum\":1},\"h\":{\"type\":\"integer\",\"minimum\":1}},"
	  "\"required\":[\"x\",\"y\",\"w\",\"h\"]}}}" },
	{ "controls", tool_controls,
	  "The library's own titlebar: each window button and control (the "
	  "antialiasing boxes, the output, GPU and size drop-downs) with its "
	  "rectangle in window points, whether it acts and a box's tick, and "
	  "the rows of a drop-down while one is open. Click the middle of a "
	  "rectangle.",
	  "{\"type\":\"object\",\"properties\":{}}" },
	{ "window", tool_window,
	  "The window's own knobs, as the titlebar and a caller drive them: "
	  "fullscreen, maximized, minimize, antialias, title, vsync, "
	  "metrics_panel, text_input, gpu (an index into state's gpus or part "
	  "of a name), output, mode ({w,h,hz}: a display mode, where the window "
	  "is the output), size ({w,h} in points), release_pointer, close. "
	  "Asked for, as a caller asks; state has what came of it.",
	  "{\"type\":\"object\",\"properties\":{\"fullscreen\":{\"type\":"
	  "\"boolean\"},\"maximized\":{\"type\":\"boolean\"},\"minimize\":{"
	  "\"type\":\"boolean\"},\"antialias\":{\"type\":\"string\",\"enum\":["
	  "\"none\",\"fsaa\",\"mlaa\",\"taau\"]},\"title\":{\"type\":\"string\"},"
	  "\"vsync\":{\"type\":\"boolean\"},\"metrics_panel\":{\"type\":"
	  "\"boolean\"},\"text_input\":{\"type\":\"boolean\"},\"gpu\":{\"type\":["
	  "\"integer\",\"string\"]},\"output\":{\"type\":\"string\"},\"mode\":{"
	  "\"type\":\"object\",\"properties\":{\"w\":{\"type\":\"integer\"},"
	  "\"h\":{\"type\":\"integer\"},\"hz\":{\"type\":\"number\"}},"
	  "\"required\":[\"w\",\"h\"]},\"size\":{\"type\":\"object\","
	  "\"properties\":{\"w\":{\"type\":\"integer\"},\"h\":{\"type\":"
	  "\"integer\"}},\"required\":[\"w\",\"h\"]},\"release_pointer\":{"
	  "\"type\":\"boolean\"},\"close\":{\"type\":\"boolean\"}}}" },
#ifndef NDEBUG
	{ "fault", tool_fault,
	  "Arm a fault site as FNL_FAULT does at start (device-lost@1, "
	  "create-surface*3); -name disarms. The whole list or none of it.",
	  "{\"type\":\"object\",\"properties\":{\"spec\":{\"type\":\"string\"}},"
	  "\"required\":[\"spec\"]}" },
#endif
};
#define TOOL_COUNT (sizeof TOOLS / sizeof TOOLS[0])

/* ---- JSON-RPC ------------------------------------------------------------ */

static bool send_all(Mcp *m, Sock fd, const char *p, size_t n)
{
	while (n) {
		if (stopping(m))
			return false;
		struct pollfd pf = { .fd = fd, .events = POLLOUT };
		const int r = sock_poll(&pf, 1, MCP_POLL_MS);
		if (r < 0 && !sock_again(sock_err()))
			return false;
		if (r <= 0)
			continue;
		const long w = sock_send(fd, p, n);
		if (w < 0) {
			if (sock_again(sock_err()))
				continue;
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}

/* The envelope's head: `id` is the request's own spelling, `id_n` bytes,
 * or null where there was none to give back. */
static void head(Sb *o, const char *id, size_t id_n)
{
	sb_s(o, "{\"jsonrpc\":\"2.0\",\"id\":");
	if (id_n)
		sb_put(o, id, id_n);
	else
		sb_s(o, "null");
}

static void rpc_error(Sb *o, int code, const char *msg)
{
	sb_f(o, ",\"error\":{\"code\":%d,\"message\":", code);
	sb_jstr(o, msg);
	sb_s(o, "}}");
}

static void initialize(Sb *o, const char *params)
{
	/* The client's version where it is one this speaks, as the protocol
	 * asks; the newest otherwise. Nothing here differs between them. */
	static const char *const KNOWN[] = { "2025-06-18", "2025-03-26",
										 "2024-11-05" };
	char want[32] = "";
	const char *ver = KNOWN[0];
	if (json_str(json_get(params, "protocolVersion"), want, sizeof want))
		for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
			if (!strcmp(want, KNOWN[i]))
				ver = KNOWN[i];
	sb_s(o, ",\"result\":{\"protocolVersion\":");
	sb_jstr(o, ver);
	sb_f(o, ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":"
			"\"fenestal\",\"version\":\"%d.%d.%d\"},\"instructions\":",
		 FNL_VERSION_MAJOR, FNL_VERSION_MINOR, FNL_VERSION_PATCH);
	sb_jstr(o, "A Fenestal window, driven from inside its process. state "
			   "first: pointer coordinates are window points, and "
			   "content_rect_pt says where the program's own area is; "
			   "controls says where the library's titlebar controls are. "
			   "Input goes in where the window server's would, so the "
			   "program and the library's titlebar act on it; events then "
			   "has what the program polled, and capture the frame as "
			   "drawn. window asks for what the titlebar's controls do. "
			   "One client at a time: a client that is idle for 30 s gives "
			   "way to one that is waiting, and keys and buttons it left "
			   "pressed are released.");
	sb_s(o, "}}");
}

static void tools_list(Sb *o)
{
	sb_s(o, ",\"result\":{\"tools\":[");
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		sb_s(o, i ? ",{\"name\":" : "{\"name\":");
		sb_jstr(o, TOOLS[i].name);
		sb_s(o, ",\"description\":");
		sb_jstr(o, TOOLS[i].desc);
		sb_s(o, ",\"inputSchema\":");
		if (TOOLS[i].schema)
			sb_s(o, TOOLS[i].schema);
		else
			key_schema(o);
		sb_s(o, "}");
	}
	sb_s(o, "]}}");
}

static void tools_call(Mcp *m, Sb *o, const char *params)
{
	char name[32];
	if (!json_str(json_get(params, "name"), name, sizeof name)) {
		/* A name too long for the buffer is a name no tool has. */
		rpc_error(o, -32602, json_get(params, "name") ? "no tool by that name"
													  : "params.name: a tool's name");
		return;
	}
	const char *args = json_get(params, "arguments");
	if (args && *args != '{' && strncmp(args, "null", 4) != 0) {
		rpc_error(o, -32602, "params.arguments: an object");
		return;
	}
	if (!args || *args != '{')
		args = "{}";
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		if (strcmp(name, TOOLS[i].name) != 0)
			continue;
		Sb c = { 0 };
		const bool bad = TOOLS[i].fn(m, args, &c);
		sb_s(o, ",\"result\":{\"content\":[");
		if (c.p)
			sb_put(o, c.p, c.n);
		sb_s(o, bad || c.bad ? "],\"isError\":true}}"
							 : "],\"isError\":false}}");
		free(c.p);
		return;
	}
	rpc_error(o, -32602, "no tool by that name");
}

/* One request, notification or response, in `req` -- NUL-terminated, an
 * element of a batch or the whole line -- answered into `o`. False where
 * nothing is owed. */
static bool respond(Mcp *m, const char *req, Sb *o)
{
	const char *p = req;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p != '{') {
		head(o, NULL, 0);
		rpc_error(o, -32600, "a request is a JSON object");
		return true;
	}

	/* The id is given back as it was spelt, and only a string or a number
	 * is: anything else is no id to echo into a message. */
	const char *id = json_get(req, "id");
	const char *id_end = NULL;
	if (id && (*id == '"' || *id == '-' || (*id >= '0' && *id <= '9')))
		id_end = json_skip(id);
	if (id_end)
		while (id_end > id && (id_end[-1] == ' ' || id_end[-1] == '\t' ||
							   id_end[-1] == '\r' || id_end[-1] == '\n'))
			id_end--;
	const size_t id_n = id_end ? (size_t)(id_end - id) : 0;

	/* What a client sends back to a request: the port sends none. */
	if (!json_get(req, "method") &&
		(json_get(req, "result") || json_get(req, "error")))
		return false;

	char method[64], ver[8];
	const bool has_method =
		json_str(json_get(req, "method"), method, sizeof method);
	if (!has_method || (id && !id_n) ||
		!json_str(json_get(req, "jsonrpc"), ver, sizeof ver) ||
		strcmp(ver, "2.0") != 0) {
		/* A notification that is none: nothing owed it. */
		if (!id && has_method)
			return true;
		head(o, id_n ? id : NULL, id_n);
		rpc_error(o, -32600, "a request is {\"jsonrpc\":\"2.0\",\"method\":"
							 "...} with a string or number id");
		return true;
	}
	if (!id)
		return false; /* notifications/initialized and the like */

	const char *params = json_get(req, "params");
	head(o, id, id_n);
	if (!strcmp(method, "initialize")) {
		m->initialized = true;
		initialize(o, params);
	} else if (!strcmp(method, "ping")) {
		sb_s(o, ",\"result\":{}}");
	} else if (!m->initialized) {
		rpc_error(o, -32002, "send initialize first");
	} else if (!strcmp(method, "tools/list")) {
		tools_list(o);
	} else if (!strcmp(method, "tools/call")) {
		tools_call(m, o, params);
	} else {
		rpc_error(o, -32601, "no such method");
	}
	return true;
}

/* One line: a message or a batch of them. False where the client cannot be
 * written to any more. */
static bool handle(Mcp *m, Sock fd, char *line)
{
	const char *p = line;
	while (*p == ' ' || *p == '\t' || *p == '\r')
		p++;
	if (!*p)
		return true;

	Sb o = { 0 };
	const char *end = json_skip(p);
	bool owed;
	if (!end || *end) {
		/* Not one JSON text: the whole line is the fault. */
		head(&o, NULL, 0);
		rpc_error(&o, -32700, "not a JSON text");
		owed = true;
	} else if (*p == '[') {
		/* A batch: each element answered, the answers one array. Each
		 * element is cut at its end for the length of its answer. */
		sb_s(&o, "[");
		const char *q = p + 1;
		int answers = 0, elements = 0;
		for (;;) {
			while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' ||
				   *q == ',')
				q++;
			if (*q == ']' || !*q)
				break;
			char *e = (char *)json_skip(q);
			if (!e)
				break;
			const char sv = *e;
			*e            = '\0';
			Sb one        = { 0 };
			if (respond(m, q, &one)) {
				if (answers++)
					sb_s(&o, ",");
				sb_put(&o, one.p, one.n);
			}
			free(one.p);
			*e = sv;
			q  = e;
			elements++;
		}
		if (!elements) {
			free(o.p);
			o = (Sb){ 0 };
			head(&o, NULL, 0);
			rpc_error(&o, -32600, "an empty batch");
			owed = true;
		} else {
			sb_s(&o, "]");
			owed = answers > 0;
		}
	} else {
		owed = respond(m, p, &o);
	}
	if (!owed) {
		free(o.p);
		return true;
	}
	sb_s(&o, "\n");

	bool ok;
	if (o.bad) {
		static const char OOM[] = "{\"jsonrpc\":\"2.0\",\"id\":null,"
								  "\"error\":{\"code\":-32603,\"message\":"
								  "\"out of memory\"}}\n";
		ok = send_all(m, fd, OOM, sizeof OOM - 1);
	} else {
		ok = send_all(m, fd, o.p, o.n);
	}
	free(o.p);
	return ok;
}

/* ---- the socket ---------------------------------------------------------- */

/* One client, to its end or the port's. Lines are cut where they end;
 * `line` is the buffer they gather in. A client idle for MCP_IDLE_S gives
 * way as soon as another is waiting. */
static void session(Mcp *m, Sock fd, char *line)
{
	static const char TOO_LONG[] =
		"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
		"\"message\":\"a request longer than 64 KiB\"}}\n";
	size_t n = 0;
	bool skipping = false;
	double last = now_seconds();
	m->cur         = fd;
	m->initialized = false;
	while (!stopping(m)) {
		struct pollfd pf[2] = { { .fd = fd, .events = POLLIN },
								{ .fd = m->listen_fd, .events = POLLIN } };
		const int r = sock_poll(pf, 2, MCP_POLL_MS);
		if (r < 0 && !sock_again(sock_err()))
			break;
		if (r <= 0)
			continue;
		if (!pf[0].revents) {
			if ((pf[1].revents & POLLIN) && now_seconds() - last > MCP_IDLE_S)
				break;
			/* Somebody waiting is no reason to spin on it. */
			pause_s(m, 0.05);
			continue;
		}
		const long got = sock_recv(fd, line + n, MCP_LINE_MAX - n);
		if (got == 0)
			break;
		if (got < 0) {
			if (sock_again(sock_err()))
				continue;
			break;
		}
		last = now_seconds();
		const size_t was = n;
		n += (size_t)got;
		size_t start = 0;
		bool alive   = true;
		for (size_t i = was; i < n && alive; i++) {
			if (line[i] != '\n')
				continue;
			line[i] = '\0';
			if (!skipping)
				alive = handle(m, fd, line + start);
			skipping = false;
			start    = i + 1;
		}
		if (!alive)
			break;
		memmove(line, line + start, n - start);
		n -= start;
		if (n == MCP_LINE_MAX) {
			if (!skipping && !send_all(m, fd, TOO_LONG, sizeof TOO_LONG - 1))
				break;
			skipping = true;
			n = 0;
		}
	}
	/* What the client left pressed, and the measuring it asked for, go with
	 * it -- unless the window is going, which no pump is left to tell. */
	if (!stopping(m)) {
		release_all(m);
		if (m->live) {
			fnl_set_cost_sampling(m->app, false);
			m->live = false;
		}
	}
	m->cur = SOCK_NONE;
}

static void *serve(void *arg)
{
	Mcp *m = arg;
	char *line = malloc(MCP_LINE_MAX);
	if (!line) {
		fnl_log(FNL_LOG_WARN, "mcp: no memory for a request; port shut");
		return NULL;
	}
	while (!stopping(m)) {
		struct pollfd pf = { .fd = m->listen_fd, .events = POLLIN };
		if (sock_poll(&pf, 1, MCP_POLL_MS) <= 0)
			continue;
		const Sock fd = accept(m->listen_fd, NULL, NULL);
		if (fd == SOCK_NONE)
			continue;
		/* Not blocking: a client that stops reading would otherwise hold
		 * a write, and the window's teardown waits on this thread. */
		sock_flags(fd);
		/* A client starts from now: the lines of the program's start, or of
		 * the last client's time, are not its. */
		fnl_mutex_lock(&m->lock);
		m->ev_head = m->ev_n = 0;
		m->ev_lost = 0;
		fnl_mutex_unlock(&m->lock);
		session(m, fd, line);
		sock_close(fd);
	}
	free(line);
	return NULL;
}

/* The socket's own file goes with the port -- the one this program made,
 * and not what another has put at the path since. */
static void mcp_free(Mcp *m)
{
	if (m->listen_fd != SOCK_NONE) {
		sock_close(m->listen_fd);
		PathId now;
		if (m->path_made_ok && path_id(m->addr.sun_path, &now) &&
			now.a == m->path_made.a && now.b == m->path_made.b)
			path_remove(m->addr.sun_path);
	}
	if (m->sock_up)
		sock_down();
	free(m->shot_px);
	free(m->ev);
	free(m);
}

/* Said once, with the socket API's own error, and the port shut. */
static void refuse(Mcp *m, const char *what, int err)
{
	fnl_log(FNL_LOG_WARN, "FNL_MCP: %s %s (error %d: %s); port shut", what,
			m->addr.sun_path, err, err_text(err));
	mcp_free(m);
}

/* Whether a program is listening at the path: a connect that is not refused.
 * A full backlog is a program too busy to accept, and listening. */
static bool path_live(Mcp *m)
{
	const Sock s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == SOCK_NONE)
		return false;
	sock_flags(s);
	const bool live =
		connect(s, (const struct sockaddr *)&m->addr, sizeof m->addr) == 0 ||
		sock_again(sock_err());
	sock_close(s);
	return live;
}

/* FNL_MCP's words: off for 0, false, off, no and nothing; the default socket
 * for 1, true, on and yes; anything else is a path, which must say so with a
 * separator -- a word that is neither would make a file of that name in
 * whatever directory the program was started in. */
typedef enum { MCP_OFF, MCP_DEFAULT, MCP_PATH, MCP_BAD } McpWant;

static McpWant want_of(const char *w)
{
	static const char *const OFF[] = { "0", "false", "off", "no" };
	static const char *const ON[]  = { "1", "true", "on", "yes" };
	if (!*w)
		return MCP_OFF;
	for (size_t i = 0; i < 4; i++) {
		if (ieq(w, OFF[i]))
			return MCP_OFF;
		if (ieq(w, ON[i]))
			return MCP_DEFAULT;
	}
	return strchr(w, '/') || strchr(w, '\\') ? MCP_PATH : MCP_BAD;
}

void mcp_open(App *a)
{
	const char *want = getenv("FNL_MCP");
	if (!want)
		return;
	const McpWant how = want_of(want);
	if (how == MCP_OFF)
		return;
	if (how == MCP_BAD) {
		fnl_log(FNL_LOG_WARN, "FNL_MCP=%s: neither 0 or 1 nor a socket path "
							  "(one with a '/' in it); port shut", want);
		return;
	}

	Mcp *m = calloc(1, sizeof *m);
	if (!m || !(m->ev = calloc(MCP_EVENTS, sizeof *m->ev))) {
		fnl_log(FNL_LOG_WARN, "FNL_MCP: no memory; port shut");
		free(m);
		return;
	}
	m->app       = a;
	m->listen_fd = SOCK_NONE;
	m->cur       = SOCK_NONE;
	m->addr.sun_family = AF_UNIX;
	char *path = m->addr.sun_path;
	const size_t cap = sizeof m->addr.sun_path;
	const int w = how == MCP_DEFAULT
				? snprintf(path, cap, "%s" SEP "fenestal-%ld.sock",
						   socket_dir(), own_pid())
				: snprintf(path, cap, "%s", want);
	if (w < 0 || (size_t)w >= cap) {
		fnl_log(FNL_LOG_WARN, "FNL_MCP: a socket path is %u bytes at most; "
							  "port shut", (unsigned)(cap - 1));
		mcp_free(m);
		return;
	}

	if (!(m->sock_up = sock_up())) {
		refuse(m, "no sockets for", 0);
		return;
	}
	const Sock fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == SOCK_NONE) {
		refuse(m, "no socket for", sock_err());
		return;
	}
	m->listen_fd = fd;
	sock_flags(fd);
	/* A socket left by a program that died is taken over; one a program is
	 * listening on is another's, and so is anything else at the path. */
	if (path_is_socket(path)) {
		if (path_live(m)) {
			sock_close(fd);
			m->listen_fd = SOCK_NONE;
			fnl_log(FNL_LOG_WARN, "FNL_MCP: %s has a program listening on "
								  "it; port shut", path);
			mcp_free(m);
			return;
		}
		path_remove(path);
	}
	if (bind(fd, (const struct sockaddr *)&m->addr, sizeof m->addr) != 0) {
		/* Not ours: mcp_free() would remove what is at the path. */
		const int err = sock_err();
		sock_close(fd);
		m->listen_fd = SOCK_NONE;
		refuse(m, "cannot bind", err);
		return;
	}
	m->path_made_ok = path_id(path, &m->path_made);
	if (!path_private(path) || listen(fd, MCP_BACKLOG) != 0) {
		refuse(m, "cannot listen on", sock_err());
		return;
	}

	fnl_mutex_init(&m->lock);
	fnl_cond_init(&m->cond);
	a->mcp = m;
	if (!fnl_thread_start(&m->thread, serve, m)) {
		a->mcp = NULL;
		fnl_cond_destroy(&m->cond);
		fnl_mutex_destroy(&m->lock);
		refuse(m, "no thread for", 0);
		return;
	}
	fnl_log(FNL_LOG_INFO, "mcp: serving on %s", path);
}

void mcp_close(App *a)
{
	Mcp *m = a->mcp;
	if (!m)
		return;
	fnl_mutex_lock(&m->lock);
	fnl_atomic_store_bool(&m->stop, true);
	fnl_cond_signal(&m->cond);
	fnl_mutex_unlock(&m->lock);
	fnl_thread_join(m->thread);
	a->mcp = NULL;
	fnl_cond_destroy(&m->cond);
	fnl_mutex_destroy(&m->lock);
	mcp_free(m);
}
