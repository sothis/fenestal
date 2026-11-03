/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A window moved to another GPU of its list and back, with no hand at the
 * machine, for a look at the dissolve across the move (DESIGN *The veil*):
 * entry `to` asked for SETTLE seconds in, the way back once the move's
 * FNL_EVENT_GPU has come and SETTLE more have passed, then the program
 * quits. The presentable area is one colour on the GPU the window opened
 * on and another on any other -- red there, blue elsewhere -- with a bar
 * that walks across it, so a capture of the window (tools/probe/dissolve.py)
 * shows the one colour give way to the other over the transition's length,
 * the bar live on both sides, and no frame black or missing in between.
 * Each event's time from the request is printed on stderr.
 *
 *   cc -std=gnu11 -O2 -Iinclude tools/probe/dissolve.c \
 *      build/linux/release/libfenestal.a -lm -lpthread -ldl -o dissolve
 *   FNL_BACKEND=x11 ./dissolve [--still] [to] [transition-ms]
 *                              [lift-fraction]
 *
 * On Windows the screen is read from inside instead (below), since no
 * capture from here is quick enough for the gap the child window's erase
 * fills (win32.c, present_erase()):
 *
 *   x86_64-w64-mingw32-gcc -std=gnu11 -O2 -D_WIN32_WINNT=0x0A00 -DFNL_STATIC \
 *      -Iinclude tools/probe/dissolve.c build/win64/release/libfenestal.a \
 *      -ld3d12 -ldxgi -ldxguid -luser32 -lgdi32 -static -o dissolve.exe
 *   tools/probe/native_run.py dissolve.exe 40 --env FNL_GPU=intel NVIDIA
 *   tools/probe/guest_run.py dissolve.exe 60 WARP
 *
 * `to` is the entry's number in the list, from 1, or any part of its name
 * ("llvmpipe", "on nouveau"), looked up when the move is asked for, by
 * which time the other renderers' GPUs and the kernel driver entries are
 * listed too; the list is printed then.
 *
 * `lift-fraction`, between 0 and 1, asks for the way back during the first
 * move's lift instead of after its event: that far into the dissolve,
 * timed from the first frame on the new GPU, where the lift starts (DESIGN
 * *The veil*). A capture across it should show no jump at the readback and
 * the blend held where it stood until the way back's own lift. The first
 * move's FNL_EVENT_GPU follows its lift, which never ends, so it is never
 * posted; and back on the GPU it opened on nothing has changed for the
 * caller, so the way back may post none either -- the probe says which.
 *
 * `--still` leaves the scene at rest: no bar walking, nothing invalidated,
 * so a move lands on a window that draws nothing of its own -- which should
 * still dissolve, and not fade through black. A thread of the probe's wakes
 * the window (fnl_invalidate()) only when the probe has something to ask.
 *
 * Exit status 0 when both moves came back as FNL_EVENT_GPU (or, asked
 * during the lift, the window came back to its GPU), 1 when one did not
 * within MOVE_LIMIT or `to` names nothing.
 */
#include <fenestal/fenestal.h>

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#define SETTLE     2.0      /* seconds drawn before each move */
#define MOVE_LIMIT 30.0     /* a move not done in this is a failure */
#define LIST_WAIT  15.0     /* how long a name is waited for in the list */
#define BACK_QUIET 2.0      /* past the lift, back home with no event: done */

#ifdef _WIN32
/* The screen across the move, read from inside: the presentable area as
 * the desktop composes it, taken off the screen DC with BitBlt by a thread
 * of the probe's as fast as that goes -- some hundred times a second --
 * and classed by colour, red, blue, the bar's white, black and the blends
 * between, with the bar's column, which stands still while a frozen frame
 * shows and walks on a live one. What it is for is the child window a move
 * makes (win32.c, present_erase()): its erase has to show the frame read
 * back from the moment the old child is gone to the new one's first
 * present, and never after, and only a look at the screen between the two
 * can say so. A line is printed when the class changes, whenever a pixel
 * is black, and otherwise every tenth of a second, from the request to a
 * second past the move's event; a summary closes each step: the longest
 * gap between samples, the black samples, red seen after blue, and the
 * longest stretch the bar stood still. */
typedef struct {
	volatile LONG gen;              /* counts requests; 0 before the first */
	volatile LONG done;             /* the step's event has come */
	volatile LONG cx, cy, cw, ch;   /* the presentable area in client px */
	double asked_ms, event_ms;
} Screen;

static Screen screen;
static HWND   screen_win;

static double qpc_ms(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
}

static BOOL CALLBACK each_window(HWND h, LPARAM unused)
{
	DWORD pid = 0;
	(void)unused;
	GetWindowThreadProcessId(h, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetParent(h))
		return TRUE;
	screen_win = h;
	return FALSE;
}

enum { C_RED, C_BLUE, C_WHITE, C_BLACK, C_OTHER, C_COUNT };
static const char *const CLASS_NAMES[C_COUNT] = {
	"red", "blue", "white", "black", "blend"
};

typedef struct {
	double t;                   /* seconds from the request */
	double frac[C_COUNT];
	int    r, g, b;             /* the mean */
	double bar;                 /* the white's mean column, 0..1, or -1 */
	int    dominant;
} Sample;

/* The fills are (0.95, 0.15, 0.10) and (0.10, 0.20, 0.95), the bar and the
 * text white; a blend of the two reads as neither. */
static int pixel_class(uint32_t p)
{
	const int b = p & 0xff, g = (p >> 8) & 0xff, r = (p >> 16) & 0xff;
	if (r < 40 && g < 40 && b < 40)
		return C_BLACK;
	if (r > 200 && g > 200 && b > 200)
		return C_WHITE;
	if (r > 140 && g < 110 && b < 110)
		return C_RED;
	if (b > 140 && r < 110 && g < 130)
		return C_BLUE;
	return C_OTHER;
}

static bool screen_sample(HDC sdc, HDC mdc, HBITMAP *bmp, uint32_t **bits,
						  int *bw, int *bh, Sample *s)
{
	POINT o = { 0, 0 };
	const int cx = screen.cx, cy = screen.cy, w = screen.cw, h = screen.ch;
	if (w < 8 || h < 8 || !ClientToScreen(screen_win, &o))
		return false;
	if (w != *bw || h != *bh) {
		if (*bmp)
			DeleteObject(*bmp);
		BITMAPINFO bi;
		memset(&bi, 0, sizeof bi);
		bi.bmiHeader.biSize = sizeof bi.bmiHeader;
		bi.bmiHeader.biWidth = w;
		bi.bmiHeader.biHeight = -h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void *p = NULL;
		*bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &p, NULL, 0);
		if (!*bmp) {
			*bw = *bh = 0;
			return false;
		}
		SelectObject(mdc, *bmp);
		*bits = p;
		*bw = w;
		*bh = h;
	}
	if (!BitBlt(mdc, 0, 0, w, h, sdc, o.x + cx, o.y + cy, SRCCOPY))
		return false;
	GdiFlush();
	long n = 0, counts[C_COUNT] = { 0 }, barn = 0;
	double sr = 0, sg = 0, sb = 0, barx = 0;
	for (int y = 0; y < h; y += 2) {
		const uint32_t *row = *bits + (size_t)y * (size_t)w;
		for (int x = 0; x < w; x += 2) {
			const uint32_t p = row[x];
			const int c = pixel_class(p);
			counts[c]++;
			n++;
			sb += p & 0xff;
			sg += (p >> 8) & 0xff;
			sr += (p >> 16) & 0xff;
			/* The bar alone walks the middle half; the text is at the top. */
			if (c == C_WHITE && y >= h / 4 && y < h - h / 4) {
				barx += x;
				barn++;
			}
		}
	}
	s->dominant = 0;
	for (int c = 0; c < C_COUNT; c++) {
		s->frac[c] = (double)counts[c] / (double)n;
		if (counts[c] > counts[s->dominant])
			s->dominant = c;
	}
	s->r = (int)(sr / n + 0.5);
	s->g = (int)(sg / n + 0.5);
	s->b = (int)(sb / n + 0.5);
	s->bar = barn ? barx / barn / w : -1.0;
	return true;
}

static void screen_print(int step, const Sample *s)
{
	fprintf(stderr, "step %d %+7.3f s: %-5s rgb (%3d,%3d,%3d) red %.2f "
			"blue %.2f black %.2f blend %.2f bar %.3f\n", step,
			s->t, CLASS_NAMES[s->dominant], s->r, s->g, s->b,
			s->frac[C_RED], s->frac[C_BLUE], s->frac[C_BLACK],
			s->frac[C_OTHER], s->bar);
}

typedef struct {
	long   samples, blacks, back;     /* back: the old colour after the new */
	int    from, to;                  /* the step's colours, C_RED or C_BLUE */
	double t_prev, gap, period_sum, first_blend, first_to, last_from;
	double still_from, still_prev, best_from, best_to, bar_prev;
	bool   to_seen;
	Sample printed;
} Stats;

static void still_close(Stats *st)
{
	if (st->still_from >= 0 &&
		st->still_prev - st->still_from > st->best_to - st->best_from) {
		st->best_from = st->still_from;
		st->best_to = st->still_prev;
	}
	st->still_from = -1;
}

static DWORD WINAPI screen_thread(LPVOID unused)
{
	(void)unused;
	HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
	HBITMAP bmp = NULL;
	uint32_t *bits = NULL;
	int bw = 0, bh = 0;
	LONG gen = 0, gen_done = 0;
	Sample last = { 0 };
	bool have_last = false;
	Stats st = { 0 };
	for (;;) {
		if (!screen_win) {
			EnumWindows(each_window, 0);
			if (!screen_win) {
				Sleep(100);
				continue;
			}
		}
		Sample s = { 0 };
		const double now = qpc_ms();
		if (!screen_sample(sdc, mdc, &bmp, &bits, &bw, &bh, &s)) {
			Sleep(10);
			continue;
		}
		const LONG g = screen.gen;
		if (g == 0 || g == gen_done) {
			/* Between moves: the last sample is kept for the step's first
			 * line, so a step opens with what showed before the request. */
			last = s;
			last.t = now;
			have_last = true;
			Sleep(5);
			continue;
		}
		if (g != gen) {
			gen = g;
			memset(&st, 0, sizeof st);
			st.first_blend = st.first_to = st.last_from = -1.0;
			st.still_from = st.best_from = st.best_to = -1.0;
			st.still_prev = st.bar_prev = -1.0;
			/* Red is the GPU the window opened on: the first move leaves
			 * it, the second comes back. */
			st.from = (g - 1) % 2 ? C_BLUE : C_RED;
			st.to = st.from == C_RED ? C_BLUE : C_RED;
			fprintf(stderr, "step %d screen: %dx%d px sampled\n",
					(int)g - 1, bw, bh);
			if (have_last) {
				last.t = (last.t - screen.asked_ms) / 1e3;
				screen_print((int)g - 1, &last);
				st.printed = last;
				st.t_prev = st.still_prev = last.t;
				st.bar_prev = last.bar;
			}
		}
		s.t = (now - screen.asked_ms) / 1e3;
		st.samples++;
		if (st.samples > 1) {
			const double d = s.t - st.t_prev;
			st.period_sum += d;
			if (d > st.gap)
				st.gap = d;
		}
		if (s.frac[C_BLACK] > 0.01)
			st.blacks++;
		if (s.dominant == st.to)
			st.to_seen = true;
		if (s.dominant == st.from) {
			st.last_from = s.t;
			if (st.to_seen)
				st.back++;
		}
		if (s.dominant == C_OTHER && st.first_blend < 0.0)
			st.first_blend = s.t;
		if (s.dominant == st.to && st.first_to < 0.0)
			st.first_to = s.t;
		/* The bar at the column it had: a frozen frame. */
		if (s.bar >= 0.0 && st.bar_prev >= 0.0 &&
			fabs(s.bar - st.bar_prev) < 0.004) {
			if (st.still_from < 0.0)
				st.still_from = st.still_prev;
		} else {
			still_close(&st);
		}
		st.still_prev = s.t;
		st.bar_prev = s.bar;
		/* A ramp prints every step of it: the mean moved. */
		const bool moved = abs(s.r - st.printed.r) > 12 ||
						   abs(s.g - st.printed.g) > 12 ||
						   abs(s.b - st.printed.b) > 12;
		const bool print = st.samples == 1 || moved ||
						   s.dominant != st.printed.dominant ||
						   s.frac[C_BLACK] > 0.01 ||
						   s.t - st.printed.t >= 0.1;
		if (print) {
			screen_print((int)g - 1, &s);
			st.printed = s;
		}
		st.t_prev = s.t;
		if (screen.done && now - screen.event_ms > 1000.0) {
			still_close(&st);
			fprintf(stderr, "step %d screen: %ld samples to %+.3f s, "
					"%.1f ms apart, the longest gap %.0f ms; black %ld; "
					"first blend %+.3f, first %s %+.3f, last %s %+.3f; "
					"%s after %s %ld; bar still %+.3f to %+.3f s "
					"(%.0f ms)\n", (int)g - 1, st.samples, s.t,
					st.samples > 1 ? st.period_sum / (st.samples - 1) * 1e3
								   : 0.0,
					st.gap * 1e3, st.blacks, st.first_blend,
					CLASS_NAMES[st.to], st.first_to, CLASS_NAMES[st.from],
					st.last_from, CLASS_NAMES[st.from], CLASS_NAMES[st.to],
					st.back, st.best_from, st.best_to,
					(st.best_to - st.best_from) * 1e3);
			gen_done = g;
		}
	}
	return 0;
}

static void screen_area(FnlApp *app, int x, int y, int w, int h)
{
	/* Points to pixels as win32.c does, and four pixels in from each
	 * edge, so a rounding at the border reads as nothing. */
	const int s = fnl_scale120(app);
	screen.cx = (x * s + 60) / 120 + 4;
	screen.cy = (y * s + 60) / 120 + 4;
	screen.cw = (w * s + 60) / 120 - 8;
	screen.ch = (h * s + 60) / 120 - 8;
}

static void screen_asked(void)
{
	screen.asked_ms = qpc_ms();
	screen.done = 0;
	InterlockedIncrement(&screen.gen);
}

static void screen_event(void)
{
	screen.event_ms = qpc_ms();
	screen.done = 1;
}

static void screen_start(void)
{
	CreateThread(NULL, 0, screen_thread, NULL, 0, NULL);
}
#else
static void screen_area(FnlApp *app, int x, int y, int w, int h)
{
	(void)app; (void)x; (void)y; (void)w; (void)h;
}
static void screen_asked(void) {}
static void screen_event(void) {}
static void screen_start(void) {}
#endif

typedef struct {
	int    to, home, step, last_gpu, fresh;
	const char *to_name;
	bool   moving, failed;
	double asked_at, settled_at, t0;
	unsigned frames;
	double lift_frac;       /* > 0: the way back asked during the lift */
	double lift_s;          /* the transition's length, in seconds */
	double landed_at;       /* the first frame on the GPU last moved to */
	bool   still;           /* --still: nothing drawn but what is asked */
	FnlApp *app;
	_Atomic long long poke_at_ms;   /* the waker's next fnl_invalidate() */
	_Atomic bool waker_stop;        /* set, and the waker joined, before
									 * fnl_shutdown() */
} Probe;

/* ---- the waker, for --still ----------------------------------------------
 *
 * A window at rest runs no callback, so a probe that draws nothing of its
 * own would never get to ask for the next move. This thread invalidates the
 * window once at each time on_overlay() leaves it, on a clock of its own:
 * one frame, identical to the last, and the window at rest again. Joined,
 * not detached: a poke it had already read when the app was freed would be
 * an fnl_invalidate() on freed memory, which no flag cleared beside the
 * shutdown can stop. */

#ifdef _WIN32
static HANDLE    g_waker;
#else
static pthread_t g_waker;
static bool      g_waker_on;
#endif

static long long mono_ms(void)
{
#ifdef _WIN32
	return (long long)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void poke_in(Probe *p, double seconds)
{
	if (p->still)
		atomic_store(&p->poke_at_ms, mono_ms() + (long long)(seconds * 1e3));
}

#ifdef _WIN32
static DWORD WINAPI waker(LPVOID ud)
#else
static void *waker(void *ud)
#endif
{
	Probe *p = ud;
	while (!atomic_load(&p->waker_stop)) {
		const long long at = atomic_load(&p->poke_at_ms);
		if (at > 0 && mono_ms() >= at) {
			atomic_store(&p->poke_at_ms, 0);
			fnl_invalidate(p->app);
		}
#ifdef _WIN32
		Sleep(10);
#else
		usleep(10000);
#endif
	}
#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

static void waker_start(Probe *p)
{
#ifdef _WIN32
	g_waker = CreateThread(NULL, 0, waker, p, 0, NULL);
#else
	g_waker_on = pthread_create(&g_waker, NULL, waker, p) == 0;
#endif
}

/* Before fnl_shutdown(): the app is still there for the last poke. */
static void waker_stop(Probe *p)
{
	atomic_store(&p->waker_stop, true);
#ifdef _WIN32
	if (g_waker) {
		WaitForSingleObject(g_waker, INFINITE);
		CloseHandle(g_waker);
		g_waker = NULL;
	}
#else
	if (g_waker_on) {
		pthread_join(g_waker, NULL);
		g_waker_on = false;
	}
#endif
}

/* The entry `to` or `to_name` picks, or -1; the list printed when `say`. */
static int resolve_target(FnlApp *app, Probe *p, bool say)
{
	const int n = fnl_gpu_count(app);
	int found = p->to_name ? -1 : p->to;
	for (int i = 0; i < n; i++) {
		const char *name = fnl_gpu_name_at(app, i);
		if (say)
			fprintf(stderr, "  GPU %d: %s%s\n", i + 1, name,
					i == fnl_gpu_current(app) ? " (in use)" : "");
		if (p->to_name && found < 0 && strstr(name, p->to_name))
			found = i;
	}
	return found >= 0 && found < n ? found : -1;
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *f)
{
	Probe *p = ud;
	if (p->t0 == 0.0)
		p->t0 = f->time;
	p->frames++;
	/* The first frames on a device the window moved to, timed: the first
	 * is the one the cover is held over, and the gap to the second is
	 * what its present cost, the wait until it is shown included. */
	if (fnl_gpu_current(app) != p->last_gpu) {
		p->last_gpu = fnl_gpu_current(app);
		p->fresh = p->moving ? 2 : 0;
	}
	if (p->fresh > 0) {
		fprintf(stderr, "step %d: frame %u, the %s on GPU %d, %.3f s after "
				"the request\n", p->step, p->frames,
				p->fresh == 2 ? "first" : "second", p->last_gpu + 1,
				f->time - p->asked_at);
		if (p->fresh == 2)
			p->landed_at = f->time;
		p->fresh--;
	}

	int x, y, w, h;
	fnl_content_rect_pt(app, &x, &y, &w, &h);
	screen_area(app, x, y, w, h);
	const bool away = fnl_gpu_current(app) != p->home;
	const FnlColor fill = away ? (FnlColor){ 0.10f, 0.20f, 0.95f, 1.0f }
							   : (FnlColor){ 0.95f, 0.15f, 0.10f, 1.0f };
	const FnlColor bar  = { 0.98f, 0.98f, 0.98f, 1.0f };
	fnl_overlay_rect(app, x, y, w, h, fill);
	/* A bar walking the area once every two seconds: live frames. Still,
	 * it stands at the left. */
	const double phase = p->still ? 0.0 : (f->time - p->t0) / 2.0;
	const int bx = x + (int)((phase - (double)(long)phase) * (w - 20));
	fnl_overlay_rect(app, bx, y + h / 4, 20, h / 2, bar);
	char line[128];
	snprintf(line, sizeof line, "GPU %d: %s", fnl_gpu_current(app) + 1,
			 fnl_gpu_short_name(app));
	fnl_overlay_text(app, x + 10, y + 10, FNL_TEXT_LARGE, bar, line);
	/* Something moves every frame, so the window never rests -- unless
	 * --still, where only the waker draws a frame, and only when asked. */
	if (!p->still)
		fnl_invalidate(app);

	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type == FNL_EVENT_GPU && p->moving) {
			fprintf(stderr, "step %d: EV gpu %d %d, %.3f s after the request, "
					"%u frames\n", p->step, ev.gpu.index,
					(int)ev.gpu.renderer, f->time - p->asked_at, p->frames);
			screen_event();
			p->moving = false;
			p->settled_at = f->time;
			p->step++;
		}
	}
	/* The way back, asked that far into the first move's lift. */
	if (p->moving && p->lift_frac > 0.0 && p->step == 0 &&
		p->landed_at > p->asked_at &&
		f->time - p->landed_at >= p->lift_frac * p->lift_s) {
		fprintf(stderr, "step 1: asking for GPU %d (%s) at %.3f s, %.3f s "
				"into the lift\n", p->home + 1,
				fnl_gpu_name_at(app, p->home), f->time,
				f->time - p->landed_at);
		p->step = 1;
		p->asked_at = f->time;
		p->frames = 0;
		screen_asked();
		fnl_request_gpu(app, p->home);
		return;
	}
	/* Back where it opened, the lift long done and no event: nothing
	 * changed for the caller, so none is owed. */
	if (p->moving && p->lift_frac > 0.0 && p->step == 1 &&
		fnl_gpu_current(app) == p->home && p->landed_at > p->asked_at &&
		f->time - p->landed_at > p->lift_s + BACK_QUIET) {
		fprintf(stderr, "step 1: back on GPU %d with no EV gpu, %.3f s "
				"after the request, %u frames\n", p->home + 1,
				f->time - p->asked_at, p->frames);
		screen_event();
		p->moving = false;
		p->settled_at = f->time;
		p->step = 2;
		return;
	}
	if (p->moving) {
		if (f->time - p->asked_at > MOVE_LIMIT) {
			fprintf(stderr, "step %d: no EV gpu in %.0f s\n", p->step,
					MOVE_LIMIT);
			p->failed = true;
			fnl_quit(app);
			/* No poke past the quit: nothing is left to ask. */
			return;
		}
		/* Still, nothing is drawn through the move but what the library
		 * draws: woken only to say it never ended. */
		poke_in(p, MOVE_LIMIT - (f->time - p->asked_at) + 0.1);
		return;
	}
	if (p->step >= 2) {
		if (f->time - p->settled_at > SETTLE)
			fnl_quit(app);
		else
			poke_in(p, SETTLE - (f->time - p->settled_at) + 0.05);
		return;
	}
	if (f->time - p->settled_at < SETTLE) {
		poke_in(p, SETTLE - (f->time - p->settled_at) + 0.05);
		return;
	}
	/* The other renderers' GPUs and the kernel driver entries are listed
	 * on a thread after the first frame, late on a slow machine: a name
	 * not there yet is looked for again next frame, for LIST_WAIT. */
	int target = p->home;
	if (p->step == 0) {
		const bool last = f->time - p->settled_at > SETTLE + LIST_WAIT;
		target = resolve_target(app, p, false);
		if (target < 0 && !last) {
			poke_in(p, 0.2);
			return;
		}
		(void)resolve_target(app, p, true);
		if (target < 0) {
			fprintf(stderr, "no GPU %s in the list\n", p->to_name);
			p->failed = true;
			fnl_quit(app);
			return;
		}
	}
	fprintf(stderr, "step %d: asking for GPU %d (%s) at %.3f s\n", p->step,
			target + 1, fnl_gpu_name_at(app, target), f->time);
	p->asked_at = f->time;
	p->frames = 0;
	p->moving = true;
	screen_asked();
	fnl_request_gpu(app, target);
}

static void on_gpu_created(void *ud, FnlApp *app)
{
	Probe *p = ud;
	if (p->home < 0)
		p->home = fnl_gpu_current(app);
	fprintf(stderr, "on GPU %d: %s\n", fnl_gpu_current(app) + 1,
			fnl_gpu_name(app));
}

int main(int argc, char **argv)
{
	Probe p = { .to = 1, .home = -1, .last_gpu = -1 };
	/* Relayed through a pipe on Windows, where stderr is buffered. */
	setvbuf(stderr, NULL, _IONBF, 0);
	if (argc > 1 && !strcmp(argv[1], "--still")) {
		p.still = true;
		argv++;
		argc--;
	}
	if (argc > 1) {
		char *end;
		const long n = strtol(argv[1], &end, 10);
		if (*argv[1] && !*end)
			p.to = (int)n - 1;
		else
			p.to_name = argv[1];
	}
	const int ms = argc > 2 ? atoi(argv[2]) : -1;
	p.lift_frac = argc > 3 ? atof(argv[3]) : 0.0;
	if (p.lift_frac < 0.0 || p.lift_frac >= 1.0) {
		fprintf(stderr, "lift-fraction is between 0 and 1\n");
		return 1;
	}
	/* fnl_set_transition_ms()'s 250 until set. */
	p.lift_s = (ms >= 0 ? ms : 250) / 1000.0;

	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "dissolve";
	desc.width_pt       = 640;
	desc.height_pt      = 360;
	desc.renderers      = FNL_RENDERERS_ANY;

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		fprintf(stderr, "%s: %s\n", fnl_result_str(rc), fnl_last_error());
		return 1;
	}
	if (ms >= 0)
		fnl_set_transition_ms(app, ms);
	p.settled_at = 0.0;
	p.app = app;
	screen_start();
	if (p.still)
		waker_start(&p);

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size           = sizeof cb;
	cb.ud             = &p;
	cb.on_gpu_created = on_gpu_created;
	cb.on_overlay     = on_overlay;
	rc = fnl_run(app, &cb);
	waker_stop(&p);
	if (rc < 0)
		fprintf(stderr, "%s: %s\n", fnl_result_str(rc), fnl_last_error());
	fnl_shutdown(app);
	return rc < 0 || p.failed ? 1 : 0;
}
