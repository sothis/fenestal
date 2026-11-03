/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The X11 backend's extensions, encoded by hand, and what they are for: the
 * monitor list and the room a window has on it (RandR), the keyboard
 * (XKB), the counter a manager waits on before it shows a new size (SYNC),
 * and when a frame reached the screen (Present, with the Generic Event
 * Extension its events need).
 *
 * None of them has a binding in core libxcb, so their requests are encoded
 * against xcbext.h, the interface those bindings are generated against.
 * Every one is *asked* and not assumed: a server without the extension, or
 * one that dislikes the encoding, answers a NULL that switches the feature
 * off and leaves the fallback in place.
 *
 * Requests are sent in one function and their replies taken in another, so a
 * caller can put several on the wire before it waits for any. Whatever is
 * sent must be taken, on every path: an uncollected reply is never freed.
 */

#include "core/app.h"

#include "backend/x11/x11_state.h"

#include <limits.h>
#include <string.h>

/* RandR's opcodes, and the RRSelectInput mask bits that mean a monitor
 * moved. */
#define RANDR_QUERY_VERSION   0
#define RANDR_SELECT_INPUT    4
#define RANDR_GET_MONITORS   42
#define RANDR_GET_CRTC_INFO  20
#define RANDR_GET_SCREEN_RESOURCES_CURRENT 25

#define RANDR_MODE_INTERLACE  0x0010u
#define RANDR_MODE_DOUBLESCAN 0x0020u
/* CRTCs asked about per read: more than a machine drives. */
#define CRTC_MAX 16

#define RANDR_NOTIFY_SCREEN_CHANGE 0x0001u
#define RANDR_NOTIFY_CRTC_CHANGE   0x0002u
#define RANDR_NOTIFY_OUTPUT_CHANGE 0x0004u

/* XKB's opcodes. */
#define XKB_USE_EXTENSION     0
#define XKB_SELECT_EVENTS     1
#define XKB_GET_NAMES        17
#define XKB_PER_CLIENT_FLAGS 21

/* The two events that mean the keymap is not what it was: a different
 * keyboard description altogether, and a change within the one in force. */
#define XKB_NEW_KEYBOARD_NOTIFY 0
#define XKB_MAP_NOTIFY          1

/* SYNC's opcodes: just what _NET_WM_SYNC_REQUEST needs, a counter of our
 * own and setting it. */
#define SYNC_INITIALIZE      0
#define SYNC_CREATE_COUNTER  2
#define SYNC_SET_COUNTER     3
#define SYNC_DESTROY_COUNTER 6

/* The Generic Event Extension's and Present's: their versions, and the
 * completion of a present, selected on the window. A GenericEvent's core
 * type, which core xcb does not name. */
#define GE_QUERY_VERSION        0
#define PRESENT_QUERY_VERSION   0
#define PRESENT_SELECT_INPUT    3
#define PRESENT_COMPLETE_MASK   0x0002u
#define PRESENT_COMPLETE_NOTIFY 1
#define PRESENT_KIND_PIXMAP     0
#define PRESENT_MODE_SKIP       2
#define X11_GENERIC_EVENT       35

#define XKB_USE_CORE_KBD              0x0100
#define XKB_PCF_DETECTABLE_AUTOREPEAT 0x0001
#define XKB_KEY_NAMES_MASK            (1u << 9)

/* xcb_extension_t is mutable by design: libxcb caches the negotiated opcode in
 * global_id, so these cannot be const. */
static xcb_extension_t randr_ext = { "RANDR", 0 };
static xcb_extension_t xkb_ext   = { "XKEYBOARD", 0 };
static xcb_extension_t sync_ext  = { "SYNC", 0 };
static xcb_extension_t ge_ext    = { "Generic Event Extension", 0 };
static xcb_extension_t present_ext = { "Present", 0 };

/* Asks whether the server has each without waiting for the answer, so it
 * arrives with whatever the caller collects next. */
void x11_ext_prefetch(struct BackendState *p)
{
	xcb_prefetch_extension_data(p->conn, &xkb_ext);
	xcb_prefetch_extension_data(p->conn, &randr_ext);
	xcb_prefetch_extension_data(p->conn, &sync_ext);
	xcb_prefetch_extension_data(p->conn, &ge_ext);
	xcb_prefetch_extension_data(p->conn, &present_ext);
}

static bool ext_present(struct BackendState *p, xcb_extension_t *ext)
{
	const struct xcb_query_extension_reply_t *q =
		xcb_get_extension_data(p->conn, ext);
	return q && q->present;
}

/* Send one extension request; the sequence number to take its reply by, or 0
 * for a request that has none or was not sent.
 *
 * `payload` is everything after the four-byte header libxcb fills in, and its
 * length must be a multiple of four -- the assert at each call site is on the
 * payload struct.
 *
 * The two iovecs before the one handed over are libxcb's: xcbext.h says in as
 * many words that indices -1 and -2 of the array must be valid, which is why
 * this is a three-element array with the payload at the end. */
static unsigned int ext_send(struct BackendState *p, xcb_extension_t *ext,
							 uint8_t opcode, const void *payload,
							 size_t bytes, bool wants_reply)
{
	uint8_t buf[32];
	if (bytes + 4 > sizeof buf)
		return 0;
	memset(buf, 0, 4);              /* libxcb writes the opcodes and length */
	memcpy(buf + 4, payload, bytes);

	struct iovec vec[3];
	vec[2].iov_base = buf;
	vec[2].iov_len  = bytes + 4;

	const xcb_protocol_request_t req = {
		.count  = 1,
		.ext    = ext,
		.opcode = opcode,
		.isvoid = wants_reply ? 0u : 1u,
	};
	const unsigned int seq = xcb_send_request(p->conn, 0, vec + 2, &req);
	return wants_reply ? seq : 0;
}

/* The reply to ext_send(), which the caller frees. NULL on any error, which
 * is the answer. */
static void *ext_take(struct BackendState *p, unsigned int seq)
{
	if (!seq)
		return NULL;
	xcb_generic_error_t *err = NULL;
	void *reply = xcb_wait_for_reply(p->conn, seq, &err);
	free(err);
	return reply;
}

/* ---- SYNC -----------------------------------------------------------------
 *
 * _NET_WM_SYNC_REQUEST: a manager that resizes the window sends a value,
 * and waits to show the new size until the window's counter reaches it --
 * which this backend sets once the frame at that size has gone to the
 * server (x11.c, x11_flush()). An INT64 travels as its high word, signed,
 * then its low one. */
typedef struct {
	uint32_t counter;
	int32_t  hi;
	uint32_t lo;
} SyncCounterValue;
_Static_assert(sizeof(SyncCounterValue) == 12,
			   "CreateCounter and SetCounter: counter, INT64 value");

/* The counter, at open, or 0 without the extension. SyncInitialize first,
 * as the extension asks of a client; its reply says only the version and is
 * let go rather than waited for, so opening gains no round trip. The
 * extension's data was prefetched with the first batch, which has been
 * waited for by now. */
void x11_sync_open(struct BackendState *p)
{
	p->sync_counter = 0;
	if (!ext_present(p, &sync_ext))
		return;
	const struct { uint8_t major, minor, pad[2]; } init = { 3, 1, { 0, 0 } };
	_Static_assert(sizeof init == 4, "SyncInitialize: two versions, pad");
	const unsigned int seq = ext_send(p, &sync_ext, SYNC_INITIALIZE, &init,
									  sizeof init, true);
	if (seq)
		xcb_discard_reply(p->conn, seq);
	const uint32_t id = xcb_generate_id(p->conn);
	const SyncCounterValue cc = { id, 0, 0 };
	(void)ext_send(p, &sync_ext, SYNC_CREATE_COUNTER, &cc, sizeof cc, false);
	p->sync_counter = id;
}

/* From the render thread, after a present: a write on the window's
 * connection, the crossing BackendState::conn allows. */
void x11_sync_set(struct BackendState *p, uint64_t value)
{
	if (!p->sync_counter)
		return;
	const SyncCounterValue sc = { p->sync_counter,
								  (int32_t)(uint32_t)(value >> 32),
								  (uint32_t)value };
	(void)ext_send(p, &sync_ext, SYNC_SET_COUNTER, &sc, sizeof sc, false);
}

/* ---- Present --------------------------------------------------------------
 *
 * The graphics API presents into the window on a connection of its own --
 * Mesa's X11 WSI and EGL by PresentPixmap -- and the server tells every
 * client that selected the window's completions about each, at the
 * display's microsecond (`ust`, CLOCK_MONOTONIC's on Linux, the frame
 * clock's) or as skipped for a later one. The serial is that connection's,
 * not this one's, so a completion is the oldest frame not yet answered
 * (BackendState::present_q): one completion a present, in order, every
 * present queued from the window's first. Both
 * versions are asked first, as the two extensions ask of a client, and
 * their replies let go unwaited. */
void x11_present_open(struct BackendState *p)
{
	p->present_opcode = 0;
	if (!ext_present(p, &ge_ext) || !ext_present(p, &present_ext))
		return;
	const struct { uint16_t major, minor; } gev = { 1, 0 };
	_Static_assert(sizeof gev == 4, "GEQueryVersion: two CARD16");
	unsigned int seq = ext_send(p, &ge_ext, GE_QUERY_VERSION, &gev,
								sizeof gev, true);
	if (seq)
		xcb_discard_reply(p->conn, seq);
	const struct { uint32_t major, minor; } pv = { 1, 2 };
	_Static_assert(sizeof pv == 8, "PresentQueryVersion: two CARD32");
	seq = ext_send(p, &present_ext, PRESENT_QUERY_VERSION, &pv, sizeof pv,
				   true);
	if (seq)
		xcb_discard_reply(p->conn, seq);
	const struct { uint32_t eid, window, mask; } sel = {
		xcb_generate_id(p->conn), p->window, PRESENT_COMPLETE_MASK };
	_Static_assert(sizeof sel == 12, "PresentSelectInput: eid, window, mask");
	(void)ext_send(p, &present_ext, PRESENT_SELECT_INPUT, &sel, sizeof sel,
				   false);
	p->present_opcode =
		xcb_get_extension_data(p->conn, &present_ext)->major_opcode;
}

/* A completion of Present's, and the frame it answers stamped; true for any
 * event of Present's, which nothing else wants. libxcb puts full_sequence
 * at byte 32 of an event longer than 32, so `ust` is at 24, `msc` at 36. */
bool x11_present_event(App *a, const xcb_generic_event_t *ev)
{
	struct BackendState *p = a->backend_state;
	const uint8_t *b = (const uint8_t *)ev;
	if (!p->present_opcode ||
		(ev->response_type & (uint8_t)X11_EVENT_TYPE_MASK) !=
			X11_GENERIC_EVENT ||
		b[1] != p->present_opcode)
		return false;
	uint16_t evtype;
	uint32_t window, serial;
	uint64_t ust, msc;
	memcpy(&evtype, b + 8, sizeof evtype);
	memcpy(&window, b + 16, sizeof window);
	memcpy(&serial, b + 20, sizeof serial);
	memcpy(&ust, b + 24, sizeof ust);
	memcpy(&msc, b + 36, sizeof msc);
	if (evtype != PRESENT_COMPLETE_NOTIFY || b[10] != PRESENT_KIND_PIXMAP ||
		window != p->window)
		return true;

	bool have = false;
	uint64_t index = 0;
	double pushed = 0.0;
	int dropped = 0;
	fnl_mutex_lock(&p->present_lock);
	/* The oldest queued frame of this serial's number, the frames ahead of
	 * it never sent (BackendState::present_ord); none of that number -- a
	 * presenter that counts otherwise -- is the oldest, in order. */
	bool found = false;
	for (int k = 0; k < p->present_n; k++)
		if (p->present_ord[(p->present_head + k) % X11_PRESENT_Q] ==
			serial) {
			dropped = k;
			found = true;
			break;
		}
	if (found)
		p->present_by_serial = true;
	p->present_head = (p->present_head + dropped) % X11_PRESENT_Q;
	p->present_n -= dropped;
	if (p->present_n > 0 && (found || !p->present_by_serial)) {
		index = p->present_q[p->present_head];
		pushed = p->present_at[p->present_head];
		p->present_head = (p->present_head + 1) % X11_PRESENT_Q;
		p->present_n--;
		p->present_answered = index + 1;
		have = true;
	}
	const int left = p->present_n;
	fnl_mutex_unlock(&p->present_lock);
	/* FNL_X11_PRESENT_LOG: which frame the serial was taken for, and how
	 * many queued ahead of it went unsent. */
	if (p->present_log)
		fnl_log(FNL_LOG_INFO, "X11 present: completion serial %u mode %u, "
				"taken for frame %llu, %.1f ms after its push, %d never "
				"sent, %d left",
				(unsigned)serial, (unsigned)b[11],
				have ? (unsigned long long)index : 0ULL,
				have ? (now_seconds() - pushed) * 1e3 : -1.0, dropped, left);
	if (!have)
		return true;
	fnl_atomic_store_bool(&p->present_live, true);
	x11_sync_answered(p, index);
	app_frame_answered(a, index);
	/* The period, as the server counts its refreshes: the stamps' step
	 * over the counter's, between two completions. Held to what a display
	 * runs at, 10 to 500 Hz; a server that fakes the counter for a window
	 * it does not show answers outside it. */
	if (p->present_msc && msc > p->present_msc && ust > p->present_ust) {
		const double per = (double)(ust - p->present_ust) * 1e-6 /
						   (double)(msc - p->present_msc);
		if (per >= 0.002 && per <= 0.1)
			p->present_period = per;
	}
	p->present_ust = ust;
	p->present_msc = msc;
	/* A stamp from before the frame was presented is not when it was
	 * shown: the server completed the present at once with the vertical
	 * blank before it -- XWayland's flip for a window resting between
	 * frames, measured 1 to 2 ms after the present. With vsync on the frame
	 * goes up at the first refresh after its present, so that is its stamp;
	 * with it off -- or Mesa's vblank_mode=0 -- it tears on at once, and is
	 * stamped nothing, as a skip is. */
	double shown = (double)ust * 1e-6;
	if (b[11] == PRESENT_MODE_SKIP)
		return true;
	if (shown < pushed) {
		double per = p->present_period;
		const uint32_t mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
		if (per <= 0.0 && mhz > 0)
			per = 1e3 / (double)mhz;
		if (!fnl_atomic_load_bool(&a->vsync) || per <= 0.0)
			return true;
		shown += ceil((pushed - shown) / per) * per;
	}
	app_frame_shown(a, index, shown, true);
	return true;
}

void x11_sync_close(struct BackendState *p)
{
	if (!p->sync_counter)
		return;
	const uint32_t id = p->sync_counter;
	(void)ext_send(p, &sync_ext, SYNC_DESTROY_COUNTER, &id, sizeof id, false);
	p->sync_counter = 0;
}

/* ---- XKB ------------------------------------------------------------------
 *
 * Detectable auto-repeat: the server stops synthesising a KeyRelease in front
 * of every repeated KeyPress, so a held key is one press and one release,
 * which is what fenestal.h promises. Where the server refuses, the filter in
 * x11_on_key() stands in.
 *
 * XkbUseExtension must precede the other two on the wire, not in time: the
 * server runs requests in order, so all three are sent together and a refused
 * UseExtension shows as its own answer. */
/* The keys' names, and nothing else of XkbGetNames: a key name is where a key
 * sits (<AC01> is the home row's first), whatever keycode the server gave it,
 * which is what FnlKey is. Sent after XkbUseExtension, as everything of XKB
 * must be. */
static unsigned int names_send(struct BackendState *p)
{
	struct { uint16_t device_spec, pad; uint32_t which; } req = {
		.device_spec = XKB_USE_CORE_KBD, .pad = 0,
		.which = XKB_KEY_NAMES_MASK,
	};
	_Static_assert(sizeof req == 8, "XkbGetNames payload is eight bytes");
	return ext_send(p, &xkb_ext, XKB_GET_NAMES, &req, sizeof req, true);
}

/* The reply: a 32-byte header, then four bytes a key from firstKey (byte
 * 18) for nKeys (byte 19). Only the key names were asked for, and a reply
 * that says it holds anything more is not read, since the lists come in a
 * fixed order and this knows the place of one. Bounded as division on the
 * reply's own length. Anything short of a whole answer leaves the keycodes
 * read as evdev's. */
static void names_take(struct BackendState *p, uint8_t *r)
{
	p->key_names_stale = false;
	if (!r)
		return;
	uint32_t units = 0, which = 0;
	memcpy(&units, r + 4, sizeof units);
	memcpy(&which, r + 8, sizeof which);
	const int first = r[18], count = r[19];
	if (which == XKB_KEY_NAMES_MASK && (uint32_t)count <= units)
		x11_keys_from_names(p, r + 32, first, count);
	free(r);
}

unsigned int x11_key_names_ask(struct BackendState *p)
{
	return p->have_xkb_events ? names_send(p) : 0;
}

void x11_key_names_take(struct BackendState *p, unsigned int seq)
{
	names_take(p, ext_take(p, seq));
}

static void xkb_ask(struct BackendState *p, X11ExtAsk *ask)
{
	if (!ext_present(p, &xkb_ext))
		return;

	struct { uint16_t major, minor; } want = { 1, 0 };
	_Static_assert(sizeof want == 4, "XkbUseExtension payload is four bytes");
	ask->xkb_use = ext_send(p, &xkb_ext, XKB_USE_EXTENSION, &want,
							sizeof want, true);

	/* Not optional once XkbUseExtension has been sent: the server no longer
	 * sends this client the core MappingNotify. selectAll takes every detail
	 * of NewKeyboardNotify. MapNotify ignores selectAll and reports only the
	 * parts named by affectMap and map; with those at zero the event is
	 * "selected" and never sent. All eight components, since any of them can
	 * move what a key types. */
	struct {
		uint16_t device_spec, affect_which, clear, select_all;
		uint16_t affect_map, map;
	} sel = {
		.device_spec  = XKB_USE_CORE_KBD,
		.affect_which = (1u << XKB_NEW_KEYBOARD_NOTIFY) |
						(1u << XKB_MAP_NOTIFY),
		.clear        = 0,
		.select_all   = (1u << XKB_NEW_KEYBOARD_NOTIFY) |
						(1u << XKB_MAP_NOTIFY),
		.affect_map   = 0xff,
		.map          = 0xff,
	};
	_Static_assert(sizeof sel == 12, "XkbSelectEvents payload is 12 bytes");
	(void)ext_send(p, &xkb_ext, XKB_SELECT_EVENTS, &sel, sizeof sel, false);

	struct {
		uint16_t device_spec, pad;
		uint32_t change, value, ctrls_to_change, auto_ctrls, auto_ctrls_values;
	} pcf = {
		.device_spec = XKB_USE_CORE_KBD,
		.pad = 0,
		.change = XKB_PCF_DETECTABLE_AUTOREPEAT,
		.value  = XKB_PCF_DETECTABLE_AUTOREPEAT,
		.ctrls_to_change = 0, .auto_ctrls = 0, .auto_ctrls_values = 0,
	};
	_Static_assert(sizeof pcf == 24, "XkbPerClientFlags payload is 24 bytes");
	ask->xkb_flags = ext_send(p, &xkb_ext, XKB_PER_CLIENT_FLAGS, &pcf,
							  sizeof pcf, true);
	ask->xkb_names = names_send(p);
}

static void xkb_take(struct BackendState *p, const X11ExtAsk *ask)
{
	xcb_generic_reply_t *ur = ext_take(p, ask->xkb_use);
	uint8_t *r = ext_take(p, ask->xkb_flags);
	uint8_t *names = ext_take(p, ask->xkb_names);

	/* The reply's `supported` byte is where a generic reply's first pad byte
	 * is -- XKB uses it rather than adding a field. */
	const bool usable = ur && ur->pad0 != 0;
	free(ur);
	if (!usable) {
		free(r);
		free(names);
		return;
	}
	names_take(p, names);
	p->xkb_first_event = xcb_get_extension_data(p->conn, &xkb_ext)->first_event;
	p->have_xkb_events = true;

	if (!r)
		return;
	/* supported, value, autoCtrls, autoCtrlsValues follow the eight-byte
	 * generic reply header; the first says whether the server took the
	 * flag. */
	uint32_t supported = 0;
	memcpy(&supported, r + 8, sizeof supported);
	free(r);
	p->detectable_repeat = (supported & XKB_PCF_DETECTABLE_AUTOREPEAT) != 0;
	if (!p->detectable_repeat)
		fnl_log(FNL_LOG_WARN, "the server refused detectable auto-repeat; "
							  "held keys are filtered by keycode instead");
}

/* ---- RandR ----------------------------------------------------------------
 *
 * 1.5's GetMonitors is the whole reason the extension is here: one request
 * that answers where every monitor is and what it is called. An older server
 * reports a version this refuses, and the screen stands in.
 *
 * The version has to be negotiated before any other RandR request reaches
 * the server, which answers BadRequest otherwise. On the root, which every
 * RandR event for the screen is delivered through and which exists before
 * this client's window does. */
static unsigned int monitors_send(struct BackendState *p)
{
	struct { xcb_window_t window; uint8_t get_active, pad[3]; } req = {
		.window = p->root, .get_active = 1, .pad = { 0, 0, 0 },
	};
	_Static_assert(sizeof req == 8, "RRGetMonitors payload is eight bytes");
	return ext_send(p, &randr_ext, RANDR_GET_MONITORS, &req, sizeof req,
					true);
}

static void randr_ask(struct BackendState *p, X11ExtAsk *ask)
{
	if (!ext_present(p, &randr_ext))
		return;

	struct { uint32_t major, minor; } ver = { 1, 5 };
	_Static_assert(sizeof ver == 8, "RRQueryVersion payload is two CARD32s");
	ask->randr_version = ext_send(p, &randr_ext, RANDR_QUERY_VERSION, &ver,
								  sizeof ver, true);

	/* Screen, CRTC and output changes, so a monitor plugged in or a resolution
	 * changed re-reads the list. Selected ahead of the first read, so no
	 * change falls between the two. */
	struct { xcb_window_t window; uint16_t enable, pad; } sel = {
		.window = p->root,
		.enable = RANDR_NOTIFY_SCREEN_CHANGE | RANDR_NOTIFY_CRTC_CHANGE |
				  RANDR_NOTIFY_OUTPUT_CHANGE,
		.pad = 0,
	};
	_Static_assert(sizeof sel == 8, "RRSelectInput payload is eight bytes");
	(void)ext_send(p, &randr_ext, RANDR_SELECT_INPUT, &sel, sizeof sel,
				   false);

	ask->monitors = monitors_send(p);
}

static void randr_take(struct BackendState *p, const X11ExtAsk *ask)
{
	struct RandrVersionReply {
		uint8_t  response_type, pad0;
		uint16_t sequence;
		uint32_t length;
		uint32_t major, minor;
		uint8_t  pad1[16];
	};
	_Static_assert(sizeof(struct RandrVersionReply) == 32,
				   "RRQueryVersion reply is 32 bytes");

	struct RandrVersionReply *vr = ext_take(p, ask->randr_version);
	if (!vr)
		return;

	const bool ok = vr->major > 1 || (vr->major == 1 && vr->minor >= 5);
	free(vr);
	if (!ok) {
		fnl_log(FNL_LOG_INFO, "RandR is older than 1.5; the output is "
							  "reported as the whole screen");
		return;
	}
	p->have_randr = true;
	p->randr_first_event =
		xcb_get_extension_data(p->conn, &randr_ext)->first_event;
}

/* Everything of the two extensions that opening a window wants, sent. Call
 * once the answer to x11_ext_prefetch() has arrived -- an extension's opcode
 * has to be known before a request to it can be encoded -- and take with
 * x11_ext_take(). */
X11ExtAsk x11_ext_ask(struct BackendState *p)
{
	X11ExtAsk ask = { 0, 0, 0, 0, 0 };
	xkb_ask(p, &ask);
	randr_ask(p, &ask);
	return ask;
}

/* ---- outputs --------------------------------------------------------------
 *
 * What the core is told of an output is its room and its name. The room is
 * _NET_WORKAREA intersected with the monitor the window is on; the name is
 * the RandR monitor's, which is the connector. */

/* The monitor a window rectangle belongs to: the one its centre is in, and
 * otherwise the one it overlaps most, since the centre can sit in the gap
 * between two screens of different heights. -1 only when the list is empty. */
int x11_monitor_index_for(const struct BackendState *p,
						  int x, int y, int w, int h)
{
	const int cx = x + w / 2, cy = y + h / 2;
	int best = -1;
	long best_area = 0;

	for (int i = 0; i < p->monitor_count; i++) {
		const X11Monitor *m = &p->monitors[i];
		if (cx >= m->x && cx < m->x + m->w && cy >= m->y && cy < m->y + m->h)
			return i;

		int ox = (x + w < m->x + m->w ? x + w : m->x + m->w) -
				 (x > m->x ? x : m->x);
		int oy = (y + h < m->y + m->h ? y + h : m->y + m->h) -
				 (y > m->y ? y : m->y);
		if (ox <= 0 || oy <= 0)
			continue;
		long area = (long)ox * oy;
		if (area > best_area) {
			best_area = area;
			best = i;
		}
	}
	if (best < 0 && p->monitor_count > 0)
		best = 0;
	return best;
}

/* The whole screen as the one monitor there is: the fallback without RandR
 * 1.5 or for a reply that would not parse. With several monitors it is their
 * *bounding box*, which overstates the room by whatever lies between them. */
static void monitors_from_screen(struct BackendState *p)
{
	p->monitor_count = 1;
	p->monitors[0] = (X11Monitor){
		.x = 0, .y = 0,
		.w = p->screen->width_in_pixels,
		.h = p->screen->height_in_pixels,
	};
	snprintf(p->monitors[0].name, sizeof p->monitors[0].name, "Screen %d",
			 p->screen_num);
}

/* The monitor list out of an RRGetMonitors reply, which this frees; NULL is
 * no reply. A wrong reply is bounded: the walk stops at the reply's own
 * length and not at the server's n_monitors, both range checks are a division
 * on a remaining length, and anything that will not parse falls back to a
 * screen-sized monitor. */
static void monitors_parse(struct BackendState *p, uint8_t *reply)
{
	struct RandrMonitorsReply {
		uint8_t  response_type, pad0;
		uint16_t sequence;
		uint32_t length;
		uint32_t timestamp, n_monitors, n_outputs;
		uint8_t  pad1[12];
	};
	struct RandrMonitorInfo {
		uint32_t name;
		uint8_t  primary, automatic;
		uint16_t n_output;
		int16_t  x, y;
		uint16_t width, height;
		uint32_t mm_width, mm_height;
	};
	_Static_assert(sizeof(struct RandrMonitorsReply) == 32,
				   "RRGetMonitors reply header is 32 bytes");
	_Static_assert(sizeof(struct RandrMonitorInfo) == 24,
				   "RandR MONITORINFO is 24 bytes plus its output list");

	if (!reply) {
		monitors_from_screen(p);
		return;
	}

	struct RandrMonitorsReply head;
	memcpy(&head, reply, sizeof head);

	/* The reply's length counts four-byte units *after* the 32-byte header. */
	const uint8_t *cur = reply + sizeof head;
	const uint8_t *end = cur + (size_t)head.length * 4;

	/* Names are atoms, so each costs a GetAtomName: all sent, then all
	 * collected. A monitor may have none, and asking for None's name is a
	 * BadAtom in the event queue and a line in the log. */
	xcb_get_atom_name_cookie_t name_cookie[MAX_OUTPUTS] = { { 0 } };
	bool named[MAX_OUTPUTS];
	int n = 0;

	while (n < MAX_OUTPUTS &&
		   (size_t)(end - cur) >= sizeof(struct RandrMonitorInfo)) {
		struct RandrMonitorInfo mi;
		memcpy(&mi, cur, sizeof mi);

		const size_t left = (size_t)(end - cur) - sizeof mi;
		if ((size_t)mi.n_output > left / 4)
			break;                       /* a list longer than the reply */

		p->monitors[n] = (X11Monitor){
			.x = mi.x, .y = mi.y,
			.w = mi.width, .h = mi.height,
			.primary = mi.primary != 0,
		};
		named[n] = mi.name != XCB_ATOM_NONE;
		if (named[n])
			name_cookie[n] = xcb_get_atom_name(p->conn, mi.name);
		n++;

		cur += sizeof mi + (size_t)mi.n_output * 4;
	}
	free(reply);

	for (int i = 0; i < n; i++) {
		xcb_get_atom_name_reply_t *nr =
			named[i] ? xcb_get_atom_name_reply(p->conn, name_cookie[i], NULL)
					 : NULL;
		if (nr) {
			int len = xcb_get_atom_name_name_length(nr);
			if (len > (int)sizeof p->monitors[i].name - 1)
				len = (int)sizeof p->monitors[i].name - 1;
			memcpy(p->monitors[i].name, xcb_get_atom_name_name(nr),
				   (size_t)len);
			p->monitors[i].name[len] = '\0';
			free(nr);
		} else {
			snprintf(p->monitors[i].name, sizeof p->monitors[i].name,
					 "Monitor %d", i);
		}
	}

	p->monitor_count = n;
	if (n == 0)
		monitors_from_screen(p);       /* a screen with no active monitor */
	/* Read on the pump's next turn and not here: this list is also the one
	 * opening waits for, and the refresh rates are two round trips more. */
	p->refresh_stale = p->have_randr;
}

/* Each monitor's refresh rate, from the mode its CRTC is showing.
 *
 * RRGetMonitors names no mode, so the screen's resources are asked for --
 * the CRTCs and every mode's timings -- and then each CRTC, all sent before
 * any is taken. A CRTC belongs to the monitor whose origin is its own,
 * which holds for every monitor RandR makes of one CRTC. The rate is the
 * mode's dot clock over its total size, doubled for an interlaced mode and
 * halved for a double-scanned one. Anything that will not parse leaves the
 * rate at zero, which the core reads as unknown; both reply walks are
 * bounded by the reply's own length, as division on what is left. */
void x11_refresh_read(struct BackendState *p)
{
	struct ResourcesReply {
		uint8_t  response_type, pad0;
		uint16_t sequence;
		uint32_t length;
		uint32_t timestamp, config_timestamp;
		uint16_t n_crtcs, n_outputs, n_modes, names_len;
		uint8_t  pad1[8];
	};
	struct ModeInfo {
		uint32_t id;
		uint16_t width, height;
		uint32_t dot_clock;
		uint16_t hsync_start, hsync_end, htotal, hskew;
		uint16_t vsync_start, vsync_end, vtotal, name_len;
		uint32_t flags;
	};
	struct CrtcInfoReply {
		uint8_t  response_type, status;
		uint16_t sequence;
		uint32_t length;
		uint32_t timestamp;
		int16_t  x, y;
		uint16_t width, height;
		uint32_t mode;
		uint16_t rotation, rotations, n_outputs, n_possible;
	};
	_Static_assert(sizeof(struct ResourcesReply) == 32,
				   "RRGetScreenResourcesCurrent reply header is 32 bytes");
	_Static_assert(sizeof(struct ModeInfo) == 32, "RandR MODEINFO is 32");
	_Static_assert(sizeof(struct CrtcInfoReply) == 32,
				   "RRGetCrtcInfo reply header is 32 bytes");

	p->refresh_stale = false;
	for (int i = 0; i < p->monitor_count; i++)
		p->monitors[i].refresh_mhz = 0;
	if (!p->have_randr || FNL_FAULT("x11-refresh"))
		return;

	struct { xcb_window_t window; } req = { p->root };
	_Static_assert(sizeof req == 4, "RRGetScreenResourcesCurrent: a window");
	uint8_t *res = ext_take(p, ext_send(p, &randr_ext,
										RANDR_GET_SCREEN_RESOURCES_CURRENT,
										&req, sizeof req, true));
	if (!res)
		return;

	struct ResourcesReply head;
	memcpy(&head, res, sizeof head);
	const size_t body = (size_t)head.length * 4;
	const uint8_t *crtcs = res + sizeof head;
	size_t ids = (size_t)head.n_crtcs + head.n_outputs;
	if (ids > body / 4 ||
		(size_t)head.n_modes > (body - ids * 4) / sizeof(struct ModeInfo)) {
		free(res);
		return;
	}
	const uint8_t *modes = crtcs + ids * 4;

	unsigned int seq[CRTC_MAX];
	const int n = head.n_crtcs < CRTC_MAX ? head.n_crtcs : CRTC_MAX;
	for (int i = 0; i < n; i++) {
		struct { uint32_t crtc, config_timestamp; } ci;
		memcpy(&ci.crtc, crtcs + (size_t)i * 4, 4);
		ci.config_timestamp = head.config_timestamp;
		_Static_assert(sizeof ci == 8, "RRGetCrtcInfo payload is eight");
		seq[i] = ext_send(p, &randr_ext, RANDR_GET_CRTC_INFO, &ci,
						  sizeof ci, true);
	}

	for (int i = 0; i < n; i++) {
		uint8_t *reply = ext_take(p, seq[i]);
		if (!reply)
			continue;
		struct CrtcInfoReply crtc;
		memcpy(&crtc, reply, sizeof crtc);
		free(reply);
		if (crtc.mode == 0)
			continue;                       /* switched off */

		for (uint16_t m = 0; m < head.n_modes; m++) {
			struct ModeInfo mi;
			memcpy(&mi, modes + (size_t)m * sizeof mi, sizeof mi);
			if (mi.id != crtc.mode || mi.htotal == 0 || mi.vtotal == 0)
				continue;
			uint64_t mhz = (uint64_t)mi.dot_clock * 1000u /
						   ((uint64_t)mi.htotal * mi.vtotal);
			if (mi.flags & RANDR_MODE_INTERLACE)
				mhz *= 2;
			if (mi.flags & RANDR_MODE_DOUBLESCAN)
				mhz /= 2;
			/* A rate past what the field holds is no mode a monitor shows:
			 * unknown, as for one that will not parse, and not a wrapped
			 * number or a ceiling the pacing would take for real. */
			if (mhz > UINT_MAX)
				break;
			for (int k = 0; k < p->monitor_count; k++)
				if (p->monitors[k].x == crtc.x &&
					p->monitors[k].y == crtc.y &&
					p->monitors[k].refresh_mhz == 0)
					p->monitors[k].refresh_mhz = (unsigned int)mhz;
			break;
		}
	}
	free(res);
}

/* The monitor list, asked for and taken apart, for a caller with something
 * else to send before it waits. 0 where there is no RandR to ask. */
unsigned int x11_monitors_ask(struct BackendState *p)
{
	return p->have_randr ? monitors_send(p) : 0;
}

void x11_monitors_take(struct BackendState *p, unsigned int seq)
{
	monitors_parse(p, ext_take(p, seq));
}

/* The answers to x11_ext_ask(): the keyboard's flag, whether RandR is new
 * enough, and the first monitor list. That list was asked for before the
 * version was known, so its reply is taken either way and used only where
 * the version allows. */
void x11_ext_take(struct BackendState *p, const X11ExtAsk *ask)
{
	xkb_take(p, ask);
	randr_take(p, ask);

	uint8_t *monitors = ext_take(p, ask->monitors);
	if (!p->have_randr) {
		free(monitors);
		monitors = NULL;
	}
	monitors_parse(p, monitors);
}

/* The most desktops' worth of _NET_WORKAREA that is read. The current desktop
 * is a root property any client may write, and it indexes the reply. */
#define DESKTOPS_MAX 64

/* The shell's free area, asked for: which desktop is current, and the areas
 * of them all. The first is needed only to pick a row out of the second, so
 * neither waits on the other. */
X11WorkAreaAsk x11_work_area_ask(struct BackendState *p)
{
	return (X11WorkAreaAsk){
		.desktop = xcb_get_property(p->conn, 0, p->root,
									p->atom[FNL_ATOM_NET_CURRENT_DESKTOP],
									XCB_ATOM_CARDINAL, 0, 1),
		.area    = xcb_get_property(p->conn, 0, p->root,
									p->atom[FNL_ATOM_NET_WORKAREA],
									XCB_ATOM_CARDINAL, 0,
									(DESKTOPS_MAX + 1) * 4),
	};
}

/* _NET_WORKAREA for the desktop that is current. Zeroed when the shell
 * publishes none, which is what a bare X server does and what leaves the
 * monitor's own rectangle standing as the room a window has. */
void x11_work_area_take(struct BackendState *p, X11WorkAreaAsk ask)
{
	p->work_w = p->work_h = 0;

	uint32_t desktop = 0;
	xcb_get_property_reply_t *d =
		xcb_get_property_reply(p->conn, ask.desktop, NULL);
	xcb_get_property_reply_t *r =
		xcb_get_property_reply(p->conn, ask.area, NULL);
	if (x11_prop_words(d) >= 1)
		memcpy(&desktop, xcb_get_property_value(d), sizeof desktop);
	free(d);
	/* 0xffffffff is EWMH's own "on every desktop", and anything past the
	 * desktops that exist is a property somebody else wrote. Either way the
	 * first rectangle is the honest guess. */
	if (desktop > DESKTOPS_MAX)
		desktop = 0;

	/* Four CARD32s per desktop. */
	if (x11_prop_words(r) >= (desktop + 1) * 4) {
		uint32_t v[4];
		memcpy(v, (const uint32_t *)xcb_get_property_value(r) + desktop * 4,
			   sizeof v);
		/* Bounded where it arrives. These are bare CARD32s on the *root*,
		 * which any client on the display may write, and they reach an
		 * addition in x11_monitor_room() and a multiplication in px_to_pt():
		 * a signed overflow in both for a value near the top of the type.
		 *
		 * Dropped rather than clamped: the zero left above is the no-shell
		 * case, which is handled, and a clamped work area would be a
		 * rectangle no shell reserved. */
		if (v[0] <= OVERLAY_COORD_MAX && v[1] <= OVERLAY_COORD_MAX &&
			v[2] <= OVERLAY_COORD_MAX && v[3] <= OVERLAY_COORD_MAX) {
			p->work_x = (int)v[0];
			p->work_y = (int)v[1];
			p->work_w = (int)v[2];
			p->work_h = (int)v[3];
		}
	}
	free(r);
}

/* One monitor's rectangle in pixels on the root -- the whole screen for a
 * negative index -- or, with `work`, the part of it the shell leaves free.
 * Intersected rather than substituted: _NET_WORKAREA describes the whole
 * desktop, so on a second monitor it is a rectangle this window is nowhere
 * near. A monitor it does not touch keeps its own. */
void x11_monitor_room(const struct BackendState *p, int idx, bool work,
					  int *x, int *y, int *w, int *h)
{
	if (idx >= 0) {
		*x = p->monitors[idx].x;
		*y = p->monitors[idx].y;
		*w = p->monitors[idx].w;
		*h = p->monitors[idx].h;
	} else {
		*x = *y = 0;
		*w = p->screen->width_in_pixels;
		*h = p->screen->height_in_pixels;
	}
	if (!work || p->work_w <= 0 || p->work_h <= 0)
		return;

	const int wx1 = p->work_x + p->work_w, wy1 = p->work_y + p->work_h;
	const int x0 = *x > p->work_x ? *x : p->work_x;
	const int y0 = *y > p->work_y ? *y : p->work_y;
	const int x1 = *x + *w < wx1 ? *x + *w : wx1;
	const int y1 = *y + *h < wy1 ? *y + *h : wy1;
	if (x1 > x0 && y1 > y0) {
		*x = x0;
		*y = y0;
		*w = x1 - x0;
		*h = y1 - y0;
	}
}

/* The two extensions' events, numbered from whatever the server assigned;
 * true when `ev` was one. Both only mark what they made stale: each arrives
 * as a burst, and the re-read is a blocking round trip.
 *
 * Either RandR event means the screen layout moved. XKB has one number for
 * all of its events, and which it is rides in the second byte. */
bool x11_ext_event(struct BackendState *p, const xcb_generic_event_t *ev)
{
	const uint8_t type = ev->response_type & (uint8_t)X11_EVENT_TYPE_MASK;

	if (p->have_randr && type >= p->randr_first_event &&
		type <= p->randr_first_event + 1) {
		p->outputs_stale = true;
		return true;
	}
	if (p->have_xkb_events && type == p->xkb_first_event) {
		if (ev->pad0 == XKB_NEW_KEYBOARD_NOTIFY ||
			ev->pad0 == XKB_MAP_NOTIFY)
			p->keymap_stale = p->key_names_stale = true;
		return true;
	}
	return false;
}
