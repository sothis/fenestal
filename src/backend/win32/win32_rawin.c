/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Raw input on a thread of its own, for fnl_input_latency_ms() on Windows.
 *
 * A window's input messages carry GetMessageTime(), which is GetTickCount()'s
 * clock and moves in the system tick's steps of about 16 ms, whatever
 * timeBeginPeriod() asks. Raw input carries no time at all, but a thread
 * that does nothing else is woken by each report as the system hands it
 * over, so now_seconds() there is when the report reached the process --
 * the window's thread, which pumps and dispatches everything else, may read
 * the message that report becomes much later, which is part of what is
 * measured. Each input message is then matched to its report: a key by its
 * scancode, extended bit and direction, a button or the wheel by its
 * transition, the one alike nearest GetMessageTime()'s age; a WM_MOUSEMOVE
 * takes every movement since the last. DESIGN *Input latency*.
 *
 * The process has one raw input registration a device, and the last made
 * wins. So the thread takes only a device the program has not registered
 * for itself, gives back at close only what is still its own, and while
 * it holds the mouse a captured pointer's motion, which the window read as
 * WM_INPUT of its own, is handed to it from here (FNL_WM_RAW). Not under
 * wine, whose GetTickCount() counts milliseconds and whose wheel is read
 * from raw input on the window (raw_wheel_open()).
 *
 * Not INPUTSINK: the reports are this process's only while one of its
 * windows is in front, as the window's own were, so nothing typed into
 * another program reaches the library.
 */

#include "core/app.h"

#include "backend/win32/win32_rawin.h"

#include <stdlib.h>

/* Reports kept for matching, and captured motion waiting for the window's
 * thread. A report is matched within a tick or two, and those that never
 * are -- typed into another of the process's windows -- go once
 * INPUT_AGE_MAX has passed or the ring is full, oldest first. */
#define RAW_RECORDS 512
#define RAW_FORWARD 256

/* How far a report's age may be from GetMessageTime()'s and still be the
 * message's: two of the tick's steps, since either reading may fall
 * anywhere in its own, and the thread's wake beside them. Seconds. */
#define TICK_SLACK 0.035

/* A key in a record: the make code, and what else tells it apart. */
#define KEY_E0   0x100u
#define KEY_UP   0x200u
#define KEY_NONE 0xffffu

/* The mouse transitions a message can be matched to. */
#define RAW_MATCHED (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP |   \
					 RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP | \
					 RI_MOUSE_MIDDLE_BUTTON_DOWN |                          \
					 RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_WHEEL |           \
					 RI_MOUSE_HWHEEL)

typedef struct {
	double   t;         /* now_seconds() as it arrived */
	bool     is_key;
	uint16_t key;       /* KEY_NONE once matched */
	uint16_t flags;     /* RAW_MATCHED transitions not yet matched */
	bool     moved;     /* a movement not yet matched */
} RawRecord;

struct RawIn {
	HINSTANCE  instance;
	HWND       target;
	HWND       hwnd;
	FnlThread  thread;
	HANDLE     ready;
	bool       ok;
	DWORD      err;
	/* Which devices the thread registered: those the program had not. */
	bool       has_mouse, has_keys;

	/* Both rings, and the forwarding flag the thread reads, under lock:
	 * the thread writes them, the window's thread matches and drains. The
	 * record ring's oldest is at rec_head. `posted` is an FNL_WM_RAW on
	 * its way, so a burst posts one. */
	FnlMutex   lock;
	RawRecord  rec[RAW_RECORDS];
	int        rec_head, rec_n;
	RawForward fwd[RAW_FORWARD];
	int        fwd_head, fwd_n;
	bool       forwarding;
	bool       posted;

	/* rawin_tally()'s, the window's thread's alone. */
	uint32_t   matched, missed;
};

static const wchar_t RAWIN_CLASS[] = L"FenestalRawInput";

/* Generic desktop's mouse and keyboard. */
#define USAGE_MOUSE 0x02
#define USAGE_KEYS  0x06

/* Whose window the process's registration for a generic desktop `usage`
 * sends to: NULL for none, `taken` where the list cannot be read, which
 * is then taken for somebody else's. */
static HWND usage_target(USHORT usage, HWND taken)
{
	RAWINPUTDEVICE got[32];
	UINT n = sizeof got / sizeof got[0];
	const UINT c = GetRegisteredRawInputDevices(got, &n, sizeof got[0]);
	if (c == (UINT)-1)
		return taken;
	for (UINT i = 0; i < c; i++)
		if (got[i].usUsagePage == 0x01 && got[i].usUsage == usage)
			return got[i].hwndTarget ? got[i].hwndTarget : taken;
	return NULL;
}

/* ---- the thread ------------------------------------------------------ */

static RawRecord *rec_at(struct RawIn *r, int i)
{
	return &r->rec[(r->rec_head + i) % RAW_RECORDS];
}

static bool rec_dead(const RawRecord *x)
{
	return x->is_key ? x->key == KEY_NONE : !x->flags && !x->moved;
}

static void rawin_take(struct RawIn *r, HRAWINPUT h)
{
	const double t = now_seconds();
	RAWINPUT ri;
	UINT size = sizeof ri;
	if (GetRawInputData(h, RID_INPUT, &ri, &size,
						sizeof(RAWINPUTHEADER)) == (UINT)-1)
		return;

	RawRecord rec = { .t = t, .key = KEY_NONE };
	if (ri.header.dwType == RIM_TYPEKEYBOARD) {
		const RAWKEYBOARD *k = &ri.data.keyboard;
		/* No code to match, an overrun, or Pause's E1 sequence, which no
		 * message spells: those keep the tick's time. */
		if (k->MakeCode == 0 || k->MakeCode >= 0xff ||
			(k->Flags & RI_KEY_E1))
			return;
		rec.is_key = true;
		rec.key = (uint16_t)((k->MakeCode & 0xffu) |
							 ((k->Flags & RI_KEY_E0) ? KEY_E0 : 0u) |
							 ((k->Flags & RI_KEY_BREAK) ? KEY_UP : 0u));
	} else if (ri.header.dwType == RIM_TYPEMOUSE) {
		const RAWMOUSE *m = &ri.data.mouse;
		rec.flags = (uint16_t)(m->usButtonFlags & RAW_MATCHED);
		/* An absolute device -- a tablet, a guest's pointer, SendInput's
		 * MOUSEEVENTF_ABSOLUTE -- moves with every report that says so. */
		rec.moved = (m->usFlags & MOUSE_MOVE_ABSOLUTE) || m->lLastX ||
					m->lLastY;
	} else {
		return;
	}

	bool post = false;
	fnl_mutex_lock(&r->lock);
	/* A captured pointer's movement is handed on, and not kept for a
	 * WM_MOUSEMOVE, which a capture has none of. Only movement: the
	 * window reads nothing else of a forwarded report, and its buttons
	 * come as messages. A report the ring has no room for is added to the
	 * newest, so that what the window is handed is late and never short;
	 * that one then counts as the input it was first. */
	const RAWMOUSE *m = &ri.data.mouse;
	if (!rec.is_key && r->forwarding && !(m->usFlags & MOUSE_MOVE_ABSOLUTE) &&
		(m->lLastX || m->lLastY)) {
		rec.moved = false;
		if (r->fwd_n < RAW_FORWARD) {
			r->fwd[(r->fwd_head + r->fwd_n) % RAW_FORWARD] =
				(RawForward){ .m = *m, .t = t };
			r->fwd_n++;
		} else {
			RAWMOUSE *last = &r->fwd[(r->fwd_head + r->fwd_n - 1) %
									 RAW_FORWARD].m;
			last->lLastX += m->lLastX;
			last->lLastY += m->lLastY;
		}
		post = !r->posted;
		r->posted = true;
	}
	if (!rec_dead(&rec)) {
		if (r->rec_n == RAW_RECORDS) {
			r->rec_head = (r->rec_head + 1) % RAW_RECORDS;
			r->rec_n--;
		}
		*rec_at(r, r->rec_n) = rec;
		r->rec_n++;
	}
	fnl_mutex_unlock(&r->lock);
	if (post)
		PostMessageW(r->target, FNL_WM_RAW, 0, 0);
}

static LRESULT CALLBACK rawin_proc(HWND hwnd, UINT msg, WPARAM wp,
								   LPARAM lp)
{
	if (msg == WM_NCCREATE)
		SetWindowLongPtrW(hwnd, GWLP_USERDATA,
						  (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
	struct RawIn *r = (struct RawIn *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

	switch (msg) {
	case WM_INPUT:
		if (r)
			rawin_take(r, (HRAWINPUT)lp);
		/* Which is what frees the report. */
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

static void *rawin_main(void *arg)
{
	struct RawIn *r = arg;
	/* Its time is the report's only while it is woken at once: above the
	 * pump and the render thread, for the microseconds a report costs. */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	r->hwnd = CreateWindowExW(0, RAWIN_CLASS, L"", 0, 0, 0, 0, 0,
							  HWND_MESSAGE, NULL, r->instance, r);
	if (r->hwnd) {
		/* Never a device the program registered for itself before the
		 * window opened: taking it would leave the program without. */
		RAWINPUTDEVICE rid[2];
		UINT n = 0;
		r->has_mouse = !usage_target(USAGE_MOUSE, r->hwnd);
		r->has_keys = !usage_target(USAGE_KEYS, r->hwnd);
		if (r->has_mouse)
			rid[n++] = (RAWINPUTDEVICE){ .usUsagePage = 0x01,
										 .usUsage = USAGE_MOUSE,
										 .hwndTarget = r->hwnd };
		if (r->has_keys)
			rid[n++] = (RAWINPUTDEVICE){ .usUsagePage = 0x01,
										 .usUsage = USAGE_KEYS,
										 .hwndTarget = r->hwnd };
		/* None free leaves err 0: the program's own, said as such. */
		if (n > 0 && RegisterRawInputDevices(rid, n, sizeof rid[0]))
			r->ok = true;
		else if (n > 0)
			r->err = GetLastError();
		if (!r->ok)
			r->has_mouse = r->has_keys = false;
	} else {
		r->err = GetLastError();
	}
	SetEvent(r->ready);
	if (!r->ok) {
		if (r->hwnd)
			DestroyWindow(r->hwnd);
		return NULL;
	}

	MSG m;
	while (GetMessageW(&m, NULL, 0, 0) > 0)
		DispatchMessageW(&m);
	return NULL;
}

/* ---- the window's thread --------------------------------------------- */

struct RawIn *rawin_start(HINSTANCE instance, HWND target)
{
	struct RawIn *r = calloc(1, sizeof *r);
	if (!r)
		return NULL;
	r->instance = instance;
	r->target = target;
	fnl_mutex_init(&r->lock);

	const WNDCLASSEXW wc = {
		.cbSize        = sizeof wc,
		.lpfnWndProc   = rawin_proc,
		.hInstance     = instance,
		.lpszClassName = RAWIN_CLASS,
	};
	r->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	/* Already there is a class an earlier window's thread left, and as
	 * good. */
	if (!r->ready || (!RegisterClassExW(&wc) &&
					  GetLastError() != ERROR_CLASS_ALREADY_EXISTS)) {
		fnl_log(FNL_LOG_WARN, "no raw input thread (%lu); input latency "
							  "is read in the tick's steps",
				GetLastError());
		if (r->ready)
			CloseHandle(r->ready);
		fnl_mutex_destroy(&r->lock);
		free(r);
		return NULL;
	}
	if (!fnl_thread_start(&r->thread, rawin_main, r)) {
		fnl_log(FNL_LOG_WARN, "no raw input thread; input latency is read "
							  "in the tick's steps");
		UnregisterClassW(RAWIN_CLASS, instance);
		CloseHandle(r->ready);
		fnl_mutex_destroy(&r->lock);
		free(r);
		return NULL;
	}
	/* Its window and its registration made, or refused, before the window
	 * needs either: a capture asks for forwarding from the first click. */
	WaitForSingleObject(r->ready, INFINITE);
	if (!r->ok) {
		if (r->err)
			fnl_log(FNL_LOG_WARN, "raw input refused to its thread (%lu); "
								  "input latency is read in the tick's "
								  "steps", r->err);
		else
			fnl_log(FNL_LOG_INFO, "raw input is the program's own; input "
								  "latency is read in the tick's steps");
		fnl_thread_join(r->thread);
		UnregisterClassW(RAWIN_CLASS, instance);
		CloseHandle(r->ready);
		fnl_mutex_destroy(&r->lock);
		free(r);
		return NULL;
	}
	return r;
}

void rawin_stop(struct RawIn *r)
{
	if (!r)
		return;
	/* What is still the thread's, and nothing the program registered
	 * since: a removal is the process's, whoever made the registration.
	 * From here, since a removal names no window. */
	RAWINPUTDEVICE rid[2];
	UINT n = 0;
	if (r->has_mouse && usage_target(USAGE_MOUSE, NULL) == r->hwnd)
		rid[n++] = (RAWINPUTDEVICE){ .usUsagePage = 0x01,
									 .usUsage = USAGE_MOUSE,
									 .dwFlags = RIDEV_REMOVE };
	if (r->has_keys && usage_target(USAGE_KEYS, NULL) == r->hwnd)
		rid[n++] = (RAWINPUTDEVICE){ .usUsagePage = 0x01,
									 .usUsage = USAGE_KEYS,
									 .dwFlags = RIDEV_REMOVE };
	if (n)
		RegisterRawInputDevices(rid, n, sizeof rid[0]);
	/* A queue too full for the close still takes a quit, which ends the
	 * loop and, with the thread, its window. */
	if (!PostMessageW(r->hwnd, WM_CLOSE, 0, 0))
		PostThreadMessageW(GetThreadId(r->thread), WM_QUIT, 0, 0);
	fnl_thread_join(r->thread);
	UnregisterClassW(RAWIN_CLASS, r->instance);
	CloseHandle(r->ready);
	fnl_mutex_destroy(&r->lock);
	free(r);
}

void rawin_forward(struct RawIn *r, bool on)
{
	fnl_mutex_lock(&r->lock);
	r->forwarding = on;
	if (!on)
		r->fwd_n = 0;
	fnl_mutex_unlock(&r->lock);
}

int rawin_forwarded(struct RawIn *r, RawForward *out, int max)
{
	fnl_mutex_lock(&r->lock);
	const int n = r->fwd_n < max ? r->fwd_n : max;
	for (int i = 0; i < n; i++)
		out[i] = r->fwd[(r->fwd_head + i) % RAW_FORWARD];
	r->fwd_head = (r->fwd_head + n) % RAW_FORWARD;
	r->fwd_n -= n;
	/* The next report posts again once this one is emptied; a caller that
	 * took a full `max` asks again. */
	if (r->fwd_n == 0)
		r->posted = false;
	fnl_mutex_unlock(&r->lock);
	return n;
}

/* What is spent or too old goes from the front; the ring is oldest
 * first, so a live record there keeps the dead behind it until it goes. */
static void prune(struct RawIn *r, double now) FNL_REQUIRES(r->lock)
{
	while (r->rec_n > 0) {
		const RawRecord *x = rec_at(r, 0);
		if (!rec_dead(x) && now - x->t <= INPUT_AGE_MAX)
			break;
		r->rec_head = (r->rec_head + 1) % RAW_RECORDS;
		r->rec_n--;
	}
}

static bool fits(double age, double tick_age)
{
	return age >= tick_age - TICK_SLACK && age <= tick_age + TICK_SLACK;
}

double rawin_key(struct RawIn *r, unsigned scancode, bool extended, bool up,
				 double tick_age)
{
	const uint16_t want = (uint16_t)((scancode & 0xffu) |
									 (extended ? KEY_E0 : 0u) |
									 (up ? KEY_UP : 0u));
	const double now = now_seconds();
	double age = -1.0;
	fnl_mutex_lock(&r->lock);
	prune(r, now);
	for (int i = 0; i < r->rec_n; i++) {
		const RawRecord *x = rec_at(r, i);
		if (x->is_key && x->key == want && fits(now - x->t, tick_age)) {
			age = now - x->t;
			/* And the same key's before it, which no message took: typed
			 * into another of the process's windows, or a message lost. */
			for (int j = 0; j <= i; j++) {
				RawRecord *y = rec_at(r, j);
				if (y->is_key && y->key == want)
					y->key = KEY_NONE;
			}
			break;
		}
	}
	fnl_mutex_unlock(&r->lock);
	if (age >= 0.0)
		r->matched++;
	else
		r->missed++;
	return age;
}

double rawin_button(struct RawIn *r, unsigned flag, double tick_age)
{
	const double now = now_seconds();
	double age = -1.0;
	fnl_mutex_lock(&r->lock);
	prune(r, now);
	for (int i = 0; i < r->rec_n; i++) {
		const RawRecord *x = rec_at(r, i);
		if (!x->is_key && (x->flags & flag) && fits(now - x->t, tick_age)) {
			age = now - x->t;
			for (int j = 0; j <= i; j++) {
				RawRecord *y = rec_at(r, j);
				if (!y->is_key)
					y->flags = (uint16_t)(y->flags & ~flag);
			}
			break;
		}
	}
	fnl_mutex_unlock(&r->lock);
	if (age >= 0.0)
		r->matched++;
	else
		r->missed++;
	return age;
}

/* Every movement since the last WM_MOUSEMOVE, but none older than the
 * message's own by more than the slack: the pump waits in GetMessage and
 * is woken by each move, so what is older moved the pointer somewhere this
 * window was not -- over another window, or the bar's drag strip, which
 * Windows answers with WM_NCMOUSEMOVE. */
bool rawin_moves(struct RawIn *r, double tick_age, double *sum,
				 double *oldest, uint32_t *n)
{
	const double now = now_seconds();
	*sum = *oldest = 0.0;
	*n = 0;
	fnl_mutex_lock(&r->lock);
	for (int i = 0; i < r->rec_n; i++) {
		RawRecord *x = rec_at(r, i);
		if (x->is_key || !x->moved)
			continue;
		x->moved = false;
		const double age = now - x->t;
		if (age > tick_age + TICK_SLACK || age > INPUT_AGE_MAX)
			continue;
		*sum += age > 0.0 ? age : 0.0;
		if (age > *oldest)
			*oldest = age;
		(*n)++;
	}
	prune(r, now);
	fnl_mutex_unlock(&r->lock);
	if (*n > 0)
		r->matched += *n;
	else
		r->missed++;
	return *n > 0;
}

void rawin_moves_spend(struct RawIn *r)
{
	const double now = now_seconds();
	fnl_mutex_lock(&r->lock);
	for (int i = 0; i < r->rec_n; i++)
		rec_at(r, i)->moved = false;
	prune(r, now);
	fnl_mutex_unlock(&r->lock);
}

void rawin_tally(const struct RawIn *r, uint32_t *matched, uint32_t *missed)
{
	*matched = r->matched;
	*missed = r->missed;
}

/* Asked of the process and not only of what start took: a registration
 * the program has made since is the program's. */
bool rawin_has_mouse(const struct RawIn *r)
{
	return r->has_mouse && usage_target(USAGE_MOUSE, NULL) == r->hwnd;
}
