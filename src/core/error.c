/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Error recording and logging -- the two tiers of app.h, implemented.
 *
 * Nothing here exits. Every unrecoverable path records what broke and returns a
 * negative FnlResult, and the caller unwinds to somewhere that can decide what
 * to do about it. The message is kept here rather than returned: threading a
 * `char **why` through sixty functions for something that approximately never
 * happens is a poor trade against one thread-local slot.
 *
 * Thread-local and not global, because the render thread and the main thread
 * fail independently and concurrently -- a device is lost while a configure is
 * being refused -- and one slot would have each overwriting the other's
 * message before either was read.
 */

#include "core/app.h"

#include <stdarg.h>

/* Long enough for a Vulkan call site plus its result name and the file and
 * line it came from, which is the longest thing written here. Truncation is
 * silent and acceptable: this is a diagnostic, not a protocol. */
#define ERR_MAX 512

static FNL_THREAD_LOCAL char err_buf[ERR_MAX];

/* The log sink, the one global the caller sets. Set once before any thread
 * exists (fnl_init) and read from every thread afterwards, so a plain
 * pointer pair would be a data race by the letter of C11.
 *
 * Two atomics rather than a struct behind a lock: a sink swapped while another
 * thread logs could pair a new function with an old user pointer, which the
 * documentation forbids, and the alternative is a mutex on the logging
 * path. */
static FnlAtomicPtr log_fn;
static FnlAtomicPtr log_ud;

/* The sink is a function pointer kept in an object-pointer slot, which C does
 * not guarantee round-trips -- and every system this targets does, because
 * dlsym and GetProcAddress both require it. Wrapped in one pair of casts here
 * rather than at each use, so there is a single place to change if a system
 * that means it ever appears. */
static inline FnlLogFn log_fn_load(void)
{
	union { void *o; FnlLogFn f; } u;
	u.o = fnl_atomic_load_ptr(&log_fn);
	return u.f;
}

static inline void log_fn_store(FnlLogFn f)
{
	union { void *o; FnlLogFn f; } u;
	u.f = f;
	fnl_atomic_store_ptr(&log_fn, u.o);
}

const char *fnl_result_str(FnlResult r)
{
	switch (r) {
	case FNL_OK:                   return "ok";
	case FNL_SKIPPED:              return "skipped";
	case FNL_ERR_UNSUPPORTED:      return "unsupported";
	case FNL_ERR_NO_DISPLAY:       return "no display";
	case FNL_ERR_NO_DEVICE:        return "no device";
	case FNL_ERR_OUT_OF_MEMORY:    return "out of memory";
	case FNL_ERR_DEVICE_LOST:      return "device lost";
	case FNL_ERR_INVALID_ARGUMENT: return "invalid argument";
	case FNL_ERR_VERSION_MISMATCH: return "version mismatch";
	case FNL_ERR_SYSTEM:           return "system error";
	}
	return "unknown error";
}

const char *fnl_last_error(void)
{
	return err_buf[0] ? err_buf : "no error";
}

void fnl_set_log_fn(FnlLogFn fn, void *ud)
{
	fnl_atomic_store_ptr(&log_ud, ud);
	log_fn_store(fn);
}

/* Both tiers end up here. `lvl` is what the sink is told; a NULL sink means
 * stderr. */
static void emit(FnlLogLevel lvl, const char *msg)
{
	FnlLogFn fn = log_fn_load();
	if (fn) {
		fn(fnl_atomic_load_ptr(&log_ud), lvl, msg);
		return;
	}
	/* Built whole and written once, as echo.c writes its lines: on Windows
	 * fprintf is mingw's own (_POSIX_C_SOURCE turns its ANSI stdio on) and
	 * may write a line in pieces, and under wine an echoed event landed
	 * inside one ("GPU 5: l" + "EV motion ..." + "lvmpipe ..."). */
	char   line[ERR_MAX + 1];
	size_t n = strlen(msg);
	if (n > ERR_MAX - 1)
		n = ERR_MAX - 1;
	memcpy(line, msg, n);
	line[n++] = '\n';
	fwrite(line, 1, n, stderr);
	/* Said now, and not when four kilobytes of it have gathered. C promises
	 * that stderr is not fully buffered and Windows' runtime keeps the
	 * promise for a console alone: into a pipe, which is what a harness or
	 * a service reads, a line waits for a block to fill, and a process that
	 * is killed takes the last block with it -- the lines a log is read
	 * for. */
	fflush(stderr);
}

void fnl_log(FnlLogLevel lvl, const char *fmt, ...)
{
	char    buf[ERR_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	emit(lvl, buf);
}

void fnl_assert_fail(const char *expr, const char *file, int line,
					 const char *func)
{
	fnl_log(FNL_LOG_ERROR, "assertion failed: %s at %s:%d in %s()",
			expr, file, line, func);
	abort();
}

FnlResult fnl_fail(FnlResult code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);

	/* Logged as well as recorded. A failure that unwinds all the way out is
	 * reported by whoever catches it, but one that is caught and handled --
	 * device loss, a surface that went away -- would otherwise leave no trace
	 * at all, and those are exactly the ones worth seeing in a log. */
	emit(FNL_LOG_ERROR, err_buf);
	return code;
}

FnlResult fnl_fail_quiet(FnlResult code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
	return code;
}
