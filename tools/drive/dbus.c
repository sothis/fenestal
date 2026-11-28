/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The session bus, as much of it as a window server's portal needs: the
 * connection, the authentication a peer connection makes, and one method
 * call at a time.
 *
 * The wire format is D-Bus's own and documented: a message is a header of
 * typed fields and a body of typed arguments, little-endian here as every
 * bus on a machine of this tree's is. What is here is only what the calls
 * this tree makes need -- `s`, `o`, `g`, `u`, `b`, `a{sv}` and `h` -- and a
 * reply read back far enough to say whether the call was answered.
 *
 * It is a tool's client, not the library's: the library opens no bus and
 * links nothing for one.
 */
#include "dbus.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define HEADER_LITTLE_ENDIAN 'l'
#define MSG_METHOD_CALL 1
#define MSG_METHOD_RETURN 2
#define MSG_ERROR 3

/* The header's field codes: what a method call names. */
#define FIELD_PATH 1
#define FIELD_INTERFACE 2
#define FIELD_MEMBER 3
#define FIELD_ERROR_NAME 4
#define FIELD_REPLY_SERIAL 5
#define FIELD_DESTINATION 6
#define FIELD_SIGNATURE 8

/* ---- a buffer ------------------------------------------------------------ */

void dbuf_put(DBuf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		while (cap < b->n + n)
			cap *= 2;
		char *q = realloc(b->p, cap);
		if (!q)
			return;
		b->p   = q;
		b->cap = cap;
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
}

void dbuf_free(DBuf *b)
{
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

static void put_u32(DBuf *b, uint32_t v)
{
	const unsigned char c[4] = { (unsigned char)(v & 0xFF),
								 (unsigned char)((v >> 8) & 0xFF),
								 (unsigned char)((v >> 16) & 0xFF),
								 (unsigned char)((v >> 24) & 0xFF) };
	dbuf_put(b, c, 4);
}

static void put_u8(DBuf *b, unsigned char v)
{
	dbuf_put(b, &v, 1);
}

/* D-Bus pads every field to its own alignment, counted from the message's
 * first byte: the header is what the bus reads first. */
static void align_to(DBuf *b, size_t n)
{
	while (b->n % n)
		put_u8(b, 0);
}

static void put_string(DBuf *b, const char *s)
{
	const uint32_t n = (uint32_t)strlen(s);
	put_u32(b, n);
	dbuf_put(b, s, n);
	put_u8(b, 0);
}

static void put_signature(DBuf *b, const char *s)
{
	const unsigned char n = (unsigned char)strlen(s);
	put_u8(b, n);
	dbuf_put(b, s, n);
	put_u8(b, 0);
}

/* ---- a body's arguments --------------------------------------------------- */

void dbuf_align(DBuf *b, size_t n)
{
	align_to(b, n);
}

void dbuf_str(DBuf *b, const char *s)
{
	align_to(b, 4);
	put_string(b, s);
}

void dbuf_obj(DBuf *b, const char *s)
{
	align_to(b, 4);
	put_string(b, s);
}

void dbuf_i32(DBuf *b, int32_t v)
{
	align_to(b, 4);
	put_u32(b, (uint32_t)v);
}

void dbuf_u32_arg(DBuf *b, uint32_t v)
{
	align_to(b, 4);
	put_u32(b, v);
}

void dbuf_double(DBuf *b, double v)
{
	align_to(b, 8);
	unsigned char c[8];
	uint64_t      bits;
	memcpy(&bits, &v, sizeof bits);
	for (int i = 0; i < 8; i++)
		c[i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
	dbuf_put(b, c, 8);
}

/* ---- the connection ------------------------------------------------------ */

static uint32_t g_serial;

/* One open message: the body is written after the header's fields are known,
 * so the header is built last, as it carries the body's length. */
typedef struct {
	DBuf body;
} Msg;

static bool write_all(int fd, const char *p, size_t n)
{
	while (n) {
		const ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}

/* The bus at $DBUS_SESSION_BUS_ADDRESS, or NULL: `unix:path=<p>` is what a
 * session of this tree's is, and the only transport here. */
static int bus_connect(char *path_out, size_t n)
{
	const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (!addr || !*addr)
		return -1;
	const char *at = strstr(addr, "unix:path=");
	if (!at)
		return -1;
	at += strlen("unix:path=");
	const char *end = strchr(at, ',');
	const size_t len = end ? (size_t)(end - at) : strlen(at);
	if (len >= n)
		return -1;
	memcpy(path_out, at, len);
	path_out[len] = '\0';

	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (len >= sizeof sa.sun_path) {
		close(fd);
		return -1;
	}
	memcpy(sa.sun_path, path_out, len + 1);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* The peer authentication a session bus takes: EXTERNAL with the uid in hex,
 * then BEGIN. One line each way, NUL-terminated, ending in a newline. */
static bool bus_auth(int fd)
{
	/* EXTERNAL carries the identity hex-encoded: the uid's own digits, not
	 * the number -- `1000` goes as `31303030`. */
	char uid[32];
	snprintf(uid, sizeof uid, "%u", (unsigned)geteuid());
	char hex[80];
	size_t h = 0;
	for (const char *p = uid; *p && h + 2 < sizeof hex; p++)
		h += (size_t)snprintf(hex + h, sizeof hex - h, "%02x", (unsigned)*p);

	/* The protocol opens with a nul byte, then the command: a client that
	 * writes the line alone has its connection reset. */
	const char nul = 0;
	char       line[128];
	const int  n = snprintf(line, sizeof line, "AUTH EXTERNAL %s\r\n", hex);
	if (n <= 0 || (size_t)n >= sizeof line || !write_all(fd, &nul, 1) ||
		!write_all(fd, line, (size_t)n))
		return false;
	char reply[256];
	size_t got = 0;
	while (got < sizeof reply - 1) {
		const ssize_t r = read(fd, reply + got, 1);
		if (r <= 0)
			return false;
		got += (size_t)r;
		if (got >= 2 && reply[got - 2] == '\r' && reply[got - 1] == '\n')
			break;
	}
	reply[got] = '\0';
	if (strncmp(reply, "OK ", 3) != 0)
		return false;
	const char begin[] = "BEGIN\r\n";
	return write_all(fd, begin, sizeof begin - 1);
}

Dbus *dbus_open(void)
{
	char path[256];
	const int fd = bus_connect(path, sizeof path);
	if (fd < 0)
		return NULL;
	if (!bus_auth(fd)) {
		close(fd);
		return NULL;
	}
	Dbus *b = calloc(1, sizeof *b);
	if (!b) {
		close(fd);
		return NULL;
	}
	/* A bus that stops talking is a failed call, not a hung tool. */
	struct timeval tv = { 10, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	b->fd = fd;
	/* Every connection says Hello first: the bus refuses everything else
	 * with `:not.active.yet`. */
	if (!dbus_call(b, "org.freedesktop.DBus", "/org/freedesktop/DBus",
				   "org.freedesktop.DBus", "Hello", "")) {
		close(fd);
		b->fd = -1;
		free(b);
		return NULL;
	}
	return b;
}

void dbus_close(Dbus *b)
{
	if (!b)
		return;
	if (b->fd >= 0)
		close(b->fd);
	dbuf_free(&b->reply);
	free(b);
}

/* ---- a method call ------------------------------------------------------- */

/* The header of one call, written into `out` from its first byte: every
 * alignment in D-Bus counts from the message's start, so the fields are
 * padded against `out->n` and not against a buffer of their own. */
static void header_write(DBuf *out, unsigned char type, uint32_t serial,
						 const char *path, const char *iface,
						 const char *member, const char *dest,
						 const char *sig)
{
	put_u8(out, HEADER_LITTLE_ENDIAN);
	put_u8(out, type);
	put_u8(out, 0); /* flags */
	put_u8(out, 1); /* protocol version */
	put_u32(out, 0); /* body length: the caller fills it in */
	put_u32(out, serial);

	/* The field array: its length, padding to the first element's eight,
	 * then the elements. */
	const size_t len_at = out->n;
	put_u32(out, 0);
	const size_t elems_at = out->n;

	const unsigned char code_path = FIELD_PATH;
	align_to(out, 8);
	put_u8(out, code_path);
	put_signature(out, "o");
	align_to(out, 4);
	put_string(out, path);

	const unsigned char code_iface = FIELD_INTERFACE;
	align_to(out, 8);
	put_u8(out, code_iface);
	put_signature(out, "s");
	align_to(out, 4);
	put_string(out, iface);

	const unsigned char code_member = FIELD_MEMBER;
	align_to(out, 8);
	put_u8(out, code_member);
	put_signature(out, "s");
	align_to(out, 4);
	put_string(out, member);

	if (dest) {
		const unsigned char code_dest = FIELD_DESTINATION;
		align_to(out, 8);
		put_u8(out, code_dest);
		put_signature(out, "s");
		align_to(out, 4);
		put_string(out, dest);
	}
	if (sig && *sig) {
		const unsigned char code_sig = FIELD_SIGNATURE;
		align_to(out, 8);
		put_u8(out, code_sig);
		put_signature(out, sig);
	}
	const uint32_t fields = (uint32_t)(out->n - elems_at);
	if (getenv("FNL_DBUS_DEBUG"))
		fprintf(stderr, "dbus: fields len_at %zu elems_at %zu n %zu -> %u\n",
				len_at, elems_at, out->n, (unsigned)fields);
	memcpy(out->p + len_at, &fields, sizeof fields);
}

/* A call with no arguments, which is all this file's caller needs so far:
 * the portal's own are written where they are made. */
bool dbus_call(Dbus *b, const char *dest, const char *path, const char *iface,
			   const char *member, const char *sig)
{
	if (!b || b->fd < 0)
		return false;
	const uint32_t serial = ++g_serial;
	DBuf           h      = { 0 };
	header_write(&h, MSG_METHOD_CALL, serial, path, iface, member, dest, sig);
	/* The body begins on an eight, and is empty: the signature named above
	 * is the whole of what a call of this one passes. */
	align_to(&h, 8);
	/* The body is empty: the signature above says so. */
	if (getenv("FNL_DBUS_DEBUG")) {
		fprintf(stderr, "dbus: send %zu bytes:", h.n);
		for (size_t i = 0; i < h.n; i++)
			fprintf(stderr, " %02x", (unsigned char)h.p[i]);
		fputc('\n', stderr);
	}
	const bool sent = write_all(b->fd, h.p, h.n);
	dbuf_free(&h);
	if (!sent)
		return false;

	/* The reply: its header first, then as much body as this reads. A call
	 * that was answered is the bus's word that it was delivered -- what it
	 * answered is the caller's to read, and none of these calls has an
	 * answer this file uses. */
	/* Signals arrive between a call and its answer -- NameAcquired, a
	 * NameOwnerChanged -- so what is read is taken until one is a reply or
	 * an error. A message the bus sends a client unsolicited carries no
	 * answer to this call and is dropped here. */
	for (;;) {
		b->reply.n = 0;
		if (!dbus_read_message(b, &b->reply))
			break;
		const unsigned char kind = (unsigned char)b->reply.p[1];
		if (kind == MSG_METHOD_RETURN || kind == MSG_ERROR)
			break;
	}
	if (!b->reply.n) {
		if (getenv("FNL_DBUS_DEBUG"))
			fprintf(stderr, "dbus: %s.%s -> nothing readable\n", iface,
					member);
		return false;
	}
	if (b->reply.n < 16)
		return false;
	const unsigned char type = (unsigned char)b->reply.p[1];
	if (getenv("FNL_DBUS_DEBUG")) {
		fprintf(stderr, "dbus: %s.%s -> type %u, %zu bytes:",
				iface, member, (unsigned)type, b->reply.n);
		for (size_t i = 0; i < b->reply.n && i < 48; i++)
			fprintf(stderr, " %02x", (unsigned char)b->reply.p[i]);
		fputc('\n', stderr);
	}
	if (type == MSG_ERROR) {
		/* Said: an error's first argument is its message, and a caller
		 * that asked for something the bus refuses wants to know. */
		fprintf(stderr, "dbus: %s.%s refused\n", iface, member);
		return false;
	}
	return type == MSG_METHOD_RETURN;
}

/* One whole message off the bus, its header and body as they came. */
/* A string's bytes at `p`, which D-Bus writes as a length, the bytes and a
 * nul. The signature's own alignment is the caller's: an `s` or `o` starts on
 * a four. */
static bool string_at(const char *p, size_t left, char *out, size_t n)
{
	if (left < 4)
		return false;
	const uint32_t len = (uint32_t)(unsigned char)p[0] |
						 (uint32_t)(unsigned char)p[1] << 8 |
						 (uint32_t)(unsigned char)p[2] << 16 |
						 (uint32_t)(unsigned char)p[3] << 24;
	if ((size_t)len + 5 > left || (size_t)len >= n)
		return false;
	memcpy(out, p + 4, len);
	out[len] = '\0';
	return true;
}

bool dbus_call_with(Dbus *b, const char *dest, const char *path,
					const char *iface, const char *member, const char *sig,
					const DBuf *body)
{
	if (!b || b->fd < 0)
		return false;
	const uint32_t serial = ++g_serial;
	DBuf           h      = { 0 };
	header_write(&h, MSG_METHOD_CALL, serial, path, iface, member, dest, sig);
	align_to(&h, 8);
	if (body && body->n) {
		const uint32_t len = (uint32_t)body->n;
		memcpy(h.p + 4, &len, sizeof len);
		dbuf_put(&h, body->p, body->n);
	}
	const bool sent = write_all(b->fd, h.p, h.n);
	dbuf_free(&h);
	if (!sent)
		return false;
	for (;;) {
		b->reply.n = 0;
		if (!dbus_read_message(b, &b->reply))
			break;
		const unsigned char kind = (unsigned char)b->reply.p[1];
		if (kind == MSG_METHOD_RETURN || kind == MSG_ERROR)
			break;
	}
	if (!b->reply.n || b->reply.n < 16)
		return false;
	const unsigned char type = (unsigned char)b->reply.p[1];
	if (type == MSG_ERROR) {
		char name[128] = "";
		dbus_error_name(b, name, sizeof name);
		fprintf(stderr, "dbus: %s.%s refused (%s)\n", iface, member, name);
		return false;
	}
	return type == MSG_METHOD_RETURN;
}

/* The header's fields walked for one code: a variant's value, which for the
 * codes this file reads is a string or an object path. */
static bool field_value(const char *msg, size_t n, unsigned char code,
						char *out, size_t cap)
{
	if (n < 16)
		return false;
	const uint32_t fields = (uint32_t)(unsigned char)msg[12] |
							(uint32_t)(unsigned char)msg[13] << 8 |
							(uint32_t)(unsigned char)msg[14] << 16 |
							(uint32_t)(unsigned char)msg[15] << 24;
	size_t at   = 16;
	size_t left = fields;
	while (left > 0 && at < n) {
		/* Every field is a struct: eight-aligned from the message's start. */
		while (at % 8)
			at++;
		if (at >= n || at - 16 >= fields)
			break;
		const unsigned char what = (unsigned char)msg[at++];
		const unsigned char slen = (unsigned char)msg[at++];
		at += (size_t)slen + 1; /* the signature and its nul */
		if (what == code)
			return string_at(msg + at, n - at, out, cap);
		/* Past it: a string's length and bytes, or an unsigned int. */
		if (slen == 1 && (msg[at - 2] == 's' || msg[at - 2] == 'o')) {
			const uint32_t len = (uint32_t)(unsigned char)msg[at] |
								 (uint32_t)(unsigned char)msg[at + 1] << 8 |
								 (uint32_t)(unsigned char)msg[at + 2] << 16 |
								 (uint32_t)(unsigned char)msg[at + 3] << 24;
			at += 4 + len + 1;
		} else if (slen == 1 && msg[at - 2] == 'u') {
			at += 4;
		} else {
			break;
		}
		left = fields;
		if (at > 16)
			left = fields - (uint32_t)(at - 16);
	}
	return false;
}

bool dbus_error_name(Dbus *b, char *out, size_t n)
{
	if (!b || !b->reply.p || b->reply.n < 16)
		return false;
	return field_value(b->reply.p, b->reply.n, FIELD_ERROR_NAME, out, n);
}

bool dbus_reply_string(Dbus *b, char *out, size_t n)
{
	if (!b || !b->reply.p || b->body_at >= b->reply.n)
		return false;
	return string_at(b->reply.p + b->body_at, b->reply.n - b->body_at, out,
					 n);
}

bool dbus_read_message(Dbus *b, DBuf *out)
{
	unsigned char head[16];
	size_t        got = 0;
	while (got < sizeof head) {
		const ssize_t r = read(b->fd, head + got, sizeof head - got);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return false;
		}
		got += (size_t)r;
	}
	const uint32_t body_len = (uint32_t)head[4] | (uint32_t)head[5] << 8 |
							  (uint32_t)head[6] << 16 |
							  (uint32_t)head[7] << 24;
	const uint32_t fields_len = (uint32_t)head[12] | (uint32_t)head[13] << 8 |
								(uint32_t)head[14] << 16 |
								(uint32_t)head[15] << 24;
	/* What is left is the field array itself (its length is in the sixteen
	 * bytes above), the padding to the body's eight, and the body. */
	/* The first sixteen bytes are the fixed header and the field array's own
	 * length; the body then starts eight-aligned after the fields. */
	const size_t pad  = (8 - (16 + fields_len) % 8) % 8;
	const size_t rest = fields_len + pad + body_len;
	char        *buf  = malloc(sizeof head + rest);
	if (!buf)
		return false;
	memcpy(buf, head, sizeof head);
	size_t have = 0;
	while (have < rest) {
		const ssize_t r = read(b->fd, buf + sizeof head + have, rest - have);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			free(buf);
			return false;
		}
		have += (size_t)r;
	}
	b->body_at = sizeof head + fields_len + pad;
	if (getenv("FNL_DBUS_DEBUG"))
		fprintf(stderr, "dbus: read body %u fields %u pad %zu total %zu\n",
				(unsigned)body_len, (unsigned)fields_len, pad,
				sizeof head + rest);
	out->n = 0;
	dbuf_put(out, buf, sizeof head + rest);
	free(buf);
	return true;
}
