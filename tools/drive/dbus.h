/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The session bus and a method call on it: dbus.c says what this is and how
 * much of the protocol it is.
 */
#ifndef FNL_DRIVE_DBUS_H
#define FNL_DRIVE_DBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A growable byte buffer, for a message's header and body. */
typedef struct {
	char  *p;
	size_t n, cap;
} DBuf;

typedef struct {
	int    fd;
	DBuf   reply;   /* the last message read, header and body */
	size_t body_at; /* where that message's body starts in it */
} Dbus;

void dbuf_put(DBuf *b, const void *p, size_t n);
void dbuf_free(DBuf *b);

/* The bus at $DBUS_SESSION_BUS_ADDRESS, authenticated, or NULL. */
Dbus *dbus_open(void);
void  dbus_close(Dbus *b);

/* One method call with no arguments, answered or refused. `sig` is the
 * body's signature and is empty for a call that has no arguments. */
bool dbus_call(Dbus *b, const char *dest, const char *path, const char *iface,
			   const char *member, const char *sig);

/* The same, with a body the caller has built: the arguments' alignment is
 * D-Bus's, and these writers keep it. */
bool dbus_call_with(Dbus *b, const char *dest, const char *path,
					const char *iface, const char *member,
					const char *sig, const DBuf *body);

/* The body's arguments, as far as this tree's calls go: a string, an object
 * path, a 32-bit integer, a double, and the alignment any of them needs. */
void dbuf_align(DBuf *b, size_t n);
void dbuf_str(DBuf *b, const char *s);
void dbuf_obj(DBuf *b, const char *s);
void dbuf_i32(DBuf *b, int32_t v);
void dbuf_u32_arg(DBuf *b, uint32_t v);
void dbuf_double(DBuf *b, double v);

/* The first string (or object path) argument of the last reply, into `out`.
 * False where the reply had none. */
bool dbus_reply_string(Dbus *b, char *out, size_t n);

/* The error name of the last reply, where it was one: what the bus or the
 * service said it would not do. */
bool dbus_error_name(Dbus *b, char *out, size_t n);

/* One whole message off the bus into `out`. */
bool dbus_read_message(Dbus *b, DBuf *out);

#endif /* FNL_DRIVE_DBUS_H */
