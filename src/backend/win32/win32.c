/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Win32 backend: one implementation of core/backend.h.
 *
 * Cross-compiled with mingw-w64 and driven three ways: under wine, on Windows
 * in a virtual machine with no GPU, and on Windows laptops with GPUs of their
 * own. Wine is not Windows, and where the two are known to differ the code
 * says so -- see WM_NCCALCSIZE, max_client_rect() and publish_size(). DESIGN
 * *Win32* and *Measured on Windows itself* have the rest.
 *
 * The window rectangle is the client rectangle and is measured in pixels, and
 * nothing is queued or told a scale -- so set_geometry, frame_request,
 * frame_cancel and apply_scale are no-ops. A size is acknowledged, though:
 * ack_configure and flush are what size_await() waits on. Nor is there an
 * invitation to draw: FIFO present is the only throttle, which
 * FnlBackendVtbl::invites says to the frame loop.
 */

#include "core/app.h"

#include "backend/win32/win32_state.h"
#include "backend/win32/win32_rawin.h"
#include "core/dynlib.h"

#include <windowsx.h>
/* The input method's types and constants; its functions are
 * opened at run time (imm_load()). */
#include <imm.h>
#include <limits.h>
#include <wchar.h>

/* ---- units ----------------------------------------------------------------
 *
 * Windows hands out physical pixels and a DPI where 96 is 100%; the library
 * works in logical points and 120ths, and app.h has the conversions. */
#define DPI_BASE 96

static int dpi_to_scale120(UINT dpi)
{
	return (int)((dpi * 120 + DPI_BASE / 2) / DPI_BASE);
}

/* The scale this backend resolves: the caller's override, or else the
 * monitor's density held to app.h's range like every other source of the
 * figure. Takes the DPI rather than reading BackendState::dpi because
 * win32_open() asks before that field is filled in. */
static int resolve_scale120(const App *a, UINT dpi)
{
	if (a->scale_override120 > 0)
		return a->scale_override120;
	return dpi ? clamp_scale120(dpi_to_scale120(dpi)) : 120;
}

/* ---- keys -----------------------------------------------------------------
 *
 * Scancode to FnlKey: the *scancode*, bits 16-23 of lParam, and never the
 * virtual-key code beside it. FnlKey names a key by where it is, and a
 * virtual-key code is what the key prints under the layout in force.
 *
 * Set-1 scancodes, indexed directly, in two tables because lParam's extended
 * bit (24) is half the code: it is what keeps the keypad's 8 from arriving as
 * the Up arrow. A code with no row reads 0, which is FNL_KEY_UNKNOWN.
 *
 * Three rows are Windows' own doing:
 *
 *   Pause, Num Lock   swapped, as a message spells them: Pause arrives plain
 *                     and Num Lock carries the extended bit, the reverse of
 *                     the wire.
 *   Print Screen      a release with no press before it, because the press is
 *                     the shell's. Wine delivers neither.
 *   right Alt         where it is AltGr, Windows sends a left Ctrl press ahead
 *                     of it. Both reach the caller.
 *
 * Wine takes a key's scancode from matching the X keymap against layouts it
 * knows, so a key it cannot place arrives with a code no keyboard sends and
 * is FNL_KEY_UNKNOWN. It also sends a modifier's release twice. */
_Static_assert(FNL_KEY_UNKNOWN == 0 && FNL_KEY_KP_ENTER <= UINT8_MAX,
			   "the scancode tables hold a FnlKey in a byte, 0 for none");

static const uint8_t SC_PLAIN[] = {
	[0x01] = FNL_KEY_ESCAPE,
	[0x02] = FNL_KEY_1,
	[0x03] = FNL_KEY_2,
	[0x04] = FNL_KEY_3,
	[0x05] = FNL_KEY_4,
	[0x06] = FNL_KEY_5,
	[0x07] = FNL_KEY_6,
	[0x08] = FNL_KEY_7,
	[0x09] = FNL_KEY_8,
	[0x0a] = FNL_KEY_9,
	[0x0b] = FNL_KEY_0,
	[0x0c] = FNL_KEY_MINUS,
	[0x0d] = FNL_KEY_EQUAL,
	[0x0e] = FNL_KEY_BACKSPACE,
	[0x0f] = FNL_KEY_TAB,
	[0x10] = FNL_KEY_Q,
	[0x11] = FNL_KEY_W,
	[0x12] = FNL_KEY_E,
	[0x13] = FNL_KEY_R,
	[0x14] = FNL_KEY_T,
	[0x15] = FNL_KEY_Y,
	[0x16] = FNL_KEY_U,
	[0x17] = FNL_KEY_I,
	[0x18] = FNL_KEY_O,
	[0x19] = FNL_KEY_P,
	[0x1a] = FNL_KEY_LBRACKET,
	[0x1b] = FNL_KEY_RBRACKET,
	[0x1c] = FNL_KEY_ENTER,
	[0x1d] = FNL_KEY_LCTRL,
	[0x1e] = FNL_KEY_A,
	[0x1f] = FNL_KEY_S,
	[0x20] = FNL_KEY_D,
	[0x21] = FNL_KEY_F,
	[0x22] = FNL_KEY_G,
	[0x23] = FNL_KEY_H,
	[0x24] = FNL_KEY_J,
	[0x25] = FNL_KEY_K,
	[0x26] = FNL_KEY_L,
	[0x27] = FNL_KEY_SEMICOLON,
	[0x28] = FNL_KEY_APOSTROPHE,
	[0x29] = FNL_KEY_GRAVE,
	[0x2a] = FNL_KEY_LSHIFT,
	[0x2b] = FNL_KEY_BACKSLASH,
	[0x2c] = FNL_KEY_Z,
	[0x2d] = FNL_KEY_X,
	[0x2e] = FNL_KEY_C,
	[0x2f] = FNL_KEY_V,
	[0x30] = FNL_KEY_B,
	[0x31] = FNL_KEY_N,
	[0x32] = FNL_KEY_M,
	[0x33] = FNL_KEY_COMMA,
	[0x34] = FNL_KEY_PERIOD,
	[0x35] = FNL_KEY_SLASH,
	[0x36] = FNL_KEY_RSHIFT,
	[0x37] = FNL_KEY_KP_MULTIPLY,
	[0x38] = FNL_KEY_LALT,
	[0x39] = FNL_KEY_SPACE,
	[0x3a] = FNL_KEY_CAPS_LOCK,
	[0x3b] = FNL_KEY_F1,
	[0x3c] = FNL_KEY_F2,
	[0x3d] = FNL_KEY_F3,
	[0x3e] = FNL_KEY_F4,
	[0x3f] = FNL_KEY_F5,
	[0x40] = FNL_KEY_F6,
	[0x41] = FNL_KEY_F7,
	[0x42] = FNL_KEY_F8,
	[0x43] = FNL_KEY_F9,
	[0x44] = FNL_KEY_F10,
	[0x45] = FNL_KEY_PAUSE,           /* sic: see above */
	[0x46] = FNL_KEY_SCROLL_LOCK,
	[0x47] = FNL_KEY_KP_7,
	[0x48] = FNL_KEY_KP_8,
	[0x49] = FNL_KEY_KP_9,
	[0x4a] = FNL_KEY_KP_SUBTRACT,
	[0x4b] = FNL_KEY_KP_4,
	[0x4c] = FNL_KEY_KP_5,
	[0x4d] = FNL_KEY_KP_6,
	[0x4e] = FNL_KEY_KP_ADD,
	[0x4f] = FNL_KEY_KP_1,
	[0x50] = FNL_KEY_KP_2,
	[0x51] = FNL_KEY_KP_3,
	[0x52] = FNL_KEY_KP_0,
	[0x53] = FNL_KEY_KP_DECIMAL,
	[0x56] = FNL_KEY_INTL_BACKSLASH,
	[0x57] = FNL_KEY_F11,
	[0x58] = FNL_KEY_F12,
};

static const uint8_t SC_EXTENDED[] = {
	[0x1c] = FNL_KEY_KP_ENTER,
	[0x1d] = FNL_KEY_RCTRL,
	[0x35] = FNL_KEY_KP_DIVIDE,
	[0x37] = FNL_KEY_PRINT_SCREEN,    /* a release only: see above */
	[0x38] = FNL_KEY_RALT,            /* after a left Ctrl where it is AltGr */
	[0x45] = FNL_KEY_NUM_LOCK,        /* sic: see above */
	[0x47] = FNL_KEY_HOME,
	[0x48] = FNL_KEY_UP,
	[0x49] = FNL_KEY_PAGE_UP,
	[0x4b] = FNL_KEY_LEFT,
	[0x4d] = FNL_KEY_RIGHT,
	[0x4f] = FNL_KEY_END,
	[0x50] = FNL_KEY_DOWN,
	[0x51] = FNL_KEY_PAGE_DOWN,
	[0x52] = FNL_KEY_INSERT,
	[0x53] = FNL_KEY_DELETE,
	[0x5b] = FNL_KEY_LSUPER,
	[0x5c] = FNL_KEY_RSUPER,
	[0x5d] = FNL_KEY_MENU,
};

/* The codes seedable() asks about by name. */
#define SC_ENTER       0x1c     /* the keypad's when extended */
#define SC_PAUSE       0x45     /* Num Lock when extended */
#define SC_KP_FIRST    0x47     /* keypad 7 ... */
#define SC_KP_SUBTRACT 0x4a
#define SC_KP_ADD      0x4e
#define SC_KP_LAST     0x53     /* ... to the keypad's point */

/* MapVirtualKey's spelling of the extended bit. */
#define SC_EXT_PREFIX 0xe000u
/* lParam bit 30 of a key-down: the key was already down, so it is a repeat. */
#define LPARAM_KEY_REPEAT ((LPARAM)1 << 30)
/* GetAsyncKeyState's high bit: the key is down now. */
#define ASYNC_KEY_DOWN 0x8000

static FnlKey key_from_scancode(LPARAM lparam)
{
	const size_t   sc  = (size_t)((lparam >> 16) & 0xff);
	const bool     ext = ((lparam >> 24) & 1) != 0;
	const uint8_t *t   = ext ? SC_EXTENDED : SC_PLAIN;
	const size_t   n   = ext ? sizeof SC_EXTENDED : sizeof SC_PLAIN;

	if (sc >= n)
		return FNL_KEY_UNKNOWN;
	return (FnlKey)t[sc];
}

/* Can this key be asked about through a virtual-key code? Three groups have
 * none of their own:
 *
 *   Pause, Num Lock    MapVirtualKey speaks the wire's scancodes and the
 *                      tables the message's; asking would seed each from the
 *                      other.
 *   the keypad's       they map to VK_HOME, VK_UP and the rest, so a held Up
 *   digits and point   arrow would read as a held keypad 8 that never clears:
 *                      the release that arrives is the arrow's. Minus and
 *                      plus, inside that range, have codes of their own.
 *   keypad Enter       VK_RETURN, the same as the other one.
 *
 * A key missed costs one press made again; one seeded wrongly is down until
 * focus next leaves. The reverse case cannot be told apart: with Num Lock
 * off, a held keypad 8 *is* VK_UP and seeds the arrow. */
static bool seedable(size_t sc, bool ext)
{
	if (sc == SC_PAUSE)
		return false;
	if (!ext && sc >= SC_KP_FIRST && sc <= SC_KP_LAST &&
		sc != SC_KP_SUBTRACT && sc != SC_KP_ADD)
		return false;
	return !(ext && sc == SC_ENTER);
}

/* The keys already down as focus arrives, which fenestal.h promises and
 * Windows tells nobody: asked per physical key, through the scancode.
 * MapVirtualKey turns each into the layout's virtual-key code, which is the
 * layout being used to look a key up and not to decide what it means. */
static void seed_held_keys(App *a)
{
	for (int ext = 0; ext < 2; ext++) {
		const uint8_t *t = ext ? SC_EXTENDED : SC_PLAIN;
		const size_t   n = ext ? sizeof SC_EXTENDED : sizeof SC_PLAIN;

		for (size_t sc = 0; sc < n; sc++) {
			if (!t[sc] || !seedable(sc, ext != 0))
				continue;
			const UINT code = ext ? (SC_EXT_PREFIX | (UINT)sc) : (UINT)sc;
			const UINT vk   = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
			if (vk && (GetAsyncKeyState((int)vk) & ASYNC_KEY_DOWN))
				app_key_seed(a, (FnlKey)t[sc]);
		}
	}
}

/* ---- publishing to the render thread ------------------------------------- */

/* The monitor's work area, where the system's answer can be one.
 *
 * A work area lies inside its monitor. Wine's Wayland driver reports one that
 * does not -- every monitor at the origin, each work rectangle at the desk's
 * real offset -- and a maximized position worked out from the two puts the
 * window half off the virtual screen the pointer is held to. There is nothing
 * to correct such a rectangle from, so the monitor stands in for it, and the
 * compositor's own answer sets the size as it does for every maximize under
 * wine. Every reader of rcWork goes through here. */
static RECT monitor_work(const MONITORINFO *mi)
{
	const RECT *w = &mi->rcWork, *m = &mi->rcMonitor;
	if (w->left >= m->left && w->top >= m->top && w->right <= m->right &&
		w->bottom <= m->bottom && w->left < w->right && w->top < w->bottom)
		return *w;
	return *m;
}

/* The refresh rate of the GDI device `dev` (`\\.\DISPLAY1`), in millihertz,
 * from the display configuration: the signal's own vertical frequency, a
 * fraction such as 59940/1000 or 143856/1000, which GetMonitorInfo and
 * EnumDisplaySettings' whole hertz cannot say. 0 where it is not found --
 * a remote session's display has no path. Read where the output is, on a
 * monitor change or a mode's, and not per frame. */
static unsigned int refresh_of(const WCHAR *dev)
{
	UINT32 np = 0, nm = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) !=
			ERROR_SUCCESS || np == 0 || nm == 0)
		return 0;
	DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
	DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
	unsigned int mhz = 0;
	if (paths && modes &&
		QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes,
						   NULL) == ERROR_SUCCESS) {
		for (UINT32 i = 0; i < np && !mhz; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {
				.header = {
					.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
					.size = sizeof src,
					.adapterId = paths[i].sourceInfo.adapterId,
					.id = paths[i].sourceInfo.id,
				},
			};
			if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
				wcscmp(src.viewGdiDeviceName, dev) != 0)
				continue;
			/* The mode's signal where the path names one, else the
			 * path's own rate, which is the same fraction rounded less. */
			DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
			const UINT32 k = paths[i].targetInfo.modeInfoIdx;
			if (k < nm &&
				modes[k].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
				r = modes[k].targetMode.targetVideoSignalInfo.vSyncFreq;
			if (r.Denominator)
				mhz = (unsigned int)((UINT64)r.Numerator * 1000u /
									 r.Denominator);
		}
	}
	free(paths);
	free(modes);
	return mhz;
}

/* ---- the monitors --------------------------------------------------------
 *
 * Named as a status line wants them: szDevice is `\\.\DISPLAY1`, and the
 * device-namespace prefix is not part of the name. The list is
 * EnumDisplayMonitors' order, read where the window changes monitor or a
 * monitor changes, and again for a move to one by name. */
typedef struct {
	HMONITOR handle[OUTPUTS_MAX];
	char     name[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int      n;
} MonitorList;

static void monitor_name(const WCHAR *dev, char *out, size_t n)
{
	char name[OUTPUT_NAME_MAX] = { 0 };
	WideCharToMultiByte(CP_UTF8, 0, dev, -1, name, sizeof name - 1, NULL,
						NULL);
	snprintf(out, n, "%s",
			 strncmp(name, "\\\\.\\", 4) == 0 ? name + 4 : name);
}

static BOOL CALLBACK monitor_listed(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp)
{
	(void)dc; (void)r;
	MonitorList *l = (MonitorList *)lp;
	MONITORINFOEXW mi = { .cbSize = sizeof mi };
	if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
		return TRUE;
	l->handle[l->n] = mon;
	monitor_name(mi.szDevice, l->name[l->n], sizeof l->name[0]);
	return ++l->n < OUTPUTS_MAX;
}

static void monitors_list(MonitorList *l)
{
	l->n = 0;
	EnumDisplayMonitors(NULL, NULL, monitor_listed, (LPARAM)l);
}

/* Which monitor the window is on and how much room it has: the monitor's
 * *work area*, the screen minus the taskbar. A caller offering window sizes
 * filters them against fnl_output_size_pt(), where a zero reads as "no idea,
 * offer everything".
 *
 * `force` re-reads when the monitor handle has not changed, which a scale,
 * mode or work-area change needs: same monitor, different numbers. Without
 * it an unchanged monitor costs one MonitorFromWindow, which is what a step
 * of a drag can afford. */
static void publish_output(App *a, bool force)
{
	struct BackendState *p = a->backend_state;
	if (!p || !p->hwnd)
		return;

	HMONITOR mon = MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST);
	if (mon == p->monitor && !force)
		return;
	p->monitor = mon;

	MONITORINFOEXW mi = { .cbSize = sizeof mi };
	if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
		return;

	const int s120 = want_s120(a);
	const RECT work = monitor_work((const MONITORINFO *)&mi);
	const int w_pt = px_to_pt(work.right - work.left, s120);
	const int h_pt = px_to_pt(work.bottom - work.top, s120);

	char name[OUTPUT_NAME_MAX];
	monitor_name(mi.szDevice, name, sizeof name);
	app_publish_output(a, name, w_pt, h_pt);
	app_publish_refresh(a, refresh_of(mi.szDevice));

	/* The titlebar's output box follows the window from here. */
	MonitorList l;
	monitors_list(&l);
	int cur = -1;
	for (int i = 0; i < l.n; i++)
		if (l.handle[i] == mon)
			cur = i;
	app_publish_outputs(a, l.name, l.n, cur);
}

/* The window's size and state, published through app_publish_size(). Called
 * from the message handlers, all of which run on the thread that made the
 * window.
 *
 * The size is asked for, never taken from WM_SIZE's lParam: in a fullscreen
 * transition a WM_SIZE arrives describing the size the window has just
 * stopped having, and GetClientRect is right inside that same message. Vulkan
 * hides a mis-published size -- currentExtent pins the swapchain to the client
 * area -- but the titlebar, the hit test and the scene's resolution are still
 * laid out from it. */
static void publish_size(App *a)
{
	RECT c;
	if (!GetClientRect(a->backend_state->hwnd, &c))
		return;
	const int w_px = (int)(c.right - c.left);
	const int h_px = (int)(c.bottom - c.top);

	/* The override wins over the DPI, as it does in win32_update_scale().
	 * Once the system has chosen the window's pixel size those pixels are not
	 * the client's to add to, so here a larger scale leaves fewer points,
	 * where on Wayland the buffer grows. Only the size the window is *created*
	 * at is scaled up; see win32_open(). */
	const int  s120     = resolve_scale120(a, a->backend_state->dpi);
	const bool rescaled = s120 != a->want_scale120;
	const int  w_pt     = px_to_pt(w_px, s120);
	const int  h_pt     = px_to_pt(h_px, s120);

	/* A minimized window has no size to publish, and zero is how the frame
	 * loop is told to idle: a clamp to 1 would have Vulkan asked for an
	 * extent the surface reports as illegal, on every retry. cur_* and win_*
	 * keep the last good size, so restoring costs one configure.
	 *
	 * Asked of IsIconic() and not read off the rectangle. Under wine the
	 * client area of a minimized window is nothing; on Windows it is the
	 * stub of a title Windows would have drawn, 160 pixels wide and all of
	 * it client area here, and published it is a resize to that and back. */
	if (IsIconic(a->backend_state->hwnd) || w_px <= 0 || h_px <= 0) {
		app_publish_hidden(a);
		return;
	}

	/* A size is a configure with a serial, as on Wayland, and for the use
	 * Wayland makes of it: the frame that answers this one says so
	 * (win32_ack_configure), which is what size_await() waits for. */
	app_publish_size_serial(a, w_pt, h_pt, s120,
							++a->backend_state->size_serial);

	/* The output's size is reported in points, so a scale that moved restates
	 * it. Otherwise the handle comparison is all a step of a resize pays;
	 * the monitor's own changes arrive as messages of their own. */
	publish_output(a, rescaled);
}

/* ---- pointer capture ----------------------------------------------------- */

/* Confine and hide the pointer, or give it back, without deciding whether it
 * is captured: the capture is the library's and lasts until F9, while the
 * confinement is the desktop's and is handed back whenever this window is not
 * the one in front. See WM_KILLFOCUS.
 *
 * ShowCursor is a process-wide *counter*, not a flag; cursor_hidden keeps the
 * calls balanced across a capture that is suspended and resumed. ClipCursor
 * keeps a snapshot in screen coordinates and does not track the window, which
 * is what capture_follow() is for. */
static void capture_apply(struct BackendState *p, bool on)
{
	if (on) {
		RECT r;
		GetClientRect(p->hwnd, &r);
		POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
		ClientToScreen(p->hwnd, &tl);
		ClientToScreen(p->hwnd, &br);
		RECT screen = { tl.x, tl.y, br.x, br.y };
		ClipCursor(&screen);
		if (!p->cursor_hidden) {
			ShowCursor(FALSE);
			p->cursor_hidden = true;
		}
	} else {
		ClipCursor(NULL);
		if (p->cursor_hidden) {
			ShowCursor(TRUE);
			p->cursor_hidden = false;
		}
	}
}

/* Move the confinement with the window; called from every message that moves
 * the client area. Gated on cursor_hidden -- "the confinement is applied
 * right now" -- rather than on the capture, so a capture suspended by a focus
 * loss does not take the pointer back because the window was resized. Not
 * while minimized: the client area is nothing, and WM_SETFOCUS re-applies on
 * the way out. */
static void capture_follow(App *a, HWND hwnd)
{
	struct BackendState *p = a->backend_state;
	if (p && a->pointer_captured && p->cursor_hidden && !IsIconic(hwnd))
		capture_apply(p, true);
}

/* The process's registration for the generic-desktop mouse, into `out`;
 * false where there is none or the list cannot be read. */
static bool raw_mouse_registration(RAWINPUTDEVICE *out)
{
	RAWINPUTDEVICE got[32];
	UINT n = sizeof got / sizeof got[0];
	const UINT c = GetRegisteredRawInputDevices(got, &n, sizeof got[0]);
	if (c == (UINT)-1)
		return false;
	for (UINT i = 0; i < c; i++)
		if (got[i].usUsagePage == 0x01 && got[i].usUsage == 0x02) {
			*out = got[i];
			return true;
		}
	return false;
}

/* The generic-desktop mouse (usage page 1, usage 2) as raw input, once per
 * window. No INPUTSINK: raw input is wanted only while this window has the
 * focus, for a captured pointer's motion and under wine for the wheel. A
 * process has one registration a device, so a program's own is taken from
 * it meanwhile: kept here, and put back by raw_unregister(). */
static bool raw_register(struct BackendState *p)
{
	if (p->raw_registered)
		return true;
	p->raw_saved_valid = raw_mouse_registration(&p->raw_saved) &&
						 p->raw_saved.hwndTarget != p->hwnd;
	RAWINPUTDEVICE rid = {
		.usUsagePage = 0x01,
		.usUsage     = 0x02,
		.dwFlags     = 0,
		.hwndTarget  = p->hwnd,
	};
	p->raw_registered = RegisterRawInputDevices(&rid, 1, sizeof rid) != 0;
	return p->raw_registered;
}

/* The mouse given back: the registration raw_register() found, or none
 * where there was none. Only while the registration is still this
 * window's -- one the program made since is the program's to keep. */
static void raw_unregister(struct BackendState *p)
{
	if (!p->raw_registered)
		return;
	p->raw_registered = false;
	RAWINPUTDEVICE now;
	if (!raw_mouse_registration(&now) || now.hwndTarget != p->hwnd)
		return;
	RAWINPUTDEVICE rid = p->raw_saved_valid ? p->raw_saved
		: (RAWINPUTDEVICE){ .usUsagePage = 0x01, .usUsage = 0x02,
							.dwFlags = RIDEV_REMOVE };
	if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
		fnl_log(FNL_LOG_WARN, "the raw mouse could not be given back to "
							  "the program (%lu)", GetLastError());
	p->raw_saved_valid = false;
}

/* Raw input with the cursor clip. Only a device that reports *relative*
 * motion turns the view. A tablet, or a virtual machine's pointer
 * integration, reports positions, which on_raw_input() passes over: in a
 * VirtualBox guest, absolute reports moved a captured pointer not at all and
 * the emulated mouse's relative ones arrived count for count. */
static void win32_capture(App *a)
{
	struct BackendState *p = a->backend_state;
	if (a->pointer_captured)
		return;

	/* Where the raw input thread has the process's raw mouse, the motion is
	 * handed on from there; otherwise registered here. */
	if (p->rawin && rawin_has_mouse(p->rawin)) {
		rawin_moves_spend(p->rawin);
		rawin_forward(p->rawin, true);
	} else if (!raw_register(p)) {
		fnl_log(FNL_LOG_WARN, "raw mouse input unavailable; the "
							  "pointer cannot be captured");
		return;
	}

	/* The reticle starts at the click, and is published once the capture has
	 * been taken. */
	app_reticle_start(a);
	a->pointer_captured = true;
	capture_apply(p, true);
	app_reticle_publish(a);
}

static void win32_release(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!a->pointer_captured)
		return;

	/* The desktop cursor comes back where the reticle is, as on the other
	 * backends. The hidden cursor moves by Windows' acceleration and stops
	 * where the clip stops it, so left there it can stand on the bar or an
	 * edge, and the next press is a move or Windows' own sizing loop. Not
	 * while the capture is suspended: the cursor is then over another
	 * window, and the user's. */
	if (p->cursor_hidden) {
		int cx, cy;
		app_content_px_rect(a, &cx, &cy, NULL, NULL);
		POINT at = { cx + (int)(a->cur_px + 0.5),
					 cy + (int)(a->cur_py + 0.5) };
		const int s120 = want_s120(a);
		a->ptr_x = px_to_ptf(at.x, s120);
		a->ptr_y = px_to_ptf(at.y, s120);
		ClientToScreen(p->hwnd, &at);
		SetCursorPos(at.x, at.y);
	}

	capture_apply(p, false);
	if (p->rawin)
		rawin_forward(p->rawin, false);
	/* Registered for the capture alone, the mouse goes back now; under
	 * wine the wheel keeps it to the close. */
	if (!p->wheel_raw)
		raw_unregister(p);
	a->pointer_captured = false;
	app_reticle_publish(a);
}

/* ---- the client titlebar --------------------------------------------------
 *
 * The library draws the whole window, which takes two things here. Windows'
 * own caption and sizing border are taken to nothing by WM_NCCALCSIZE, so the
 * window rectangle and the client rectangle are the same thing; and
 * WM_NCHITTEST answers for the bar's buttons, its drag strip, the resize edges
 * and a caller's run, from decor.c's deco_hit().
 *
 * Removing the caption unconditionally is safe because fnl_init() refuses to
 * open on a renderer that cannot draw a bar (FnlRendererVtbl::draws_overlay).
 *
 * The resize edges are an *inner* margin of DECO_GRAB points, hit-tested by
 * deco_hit() and drawn over by the scene: the window rectangle is the window,
 * so there is no room outside it for the transparent ring Wayland has.
 */

/* fnl_set_cursor()'s shapes as the system's own, by resource number: the
 * IDC_* names are pointers made from these, which is not something a static
 * initialiser may hold under every compiler this builds with. */
static const WORD CALLER_CURSORS[] = {
	[FNL_CURSOR_DEFAULT]     = 32512,   /* IDC_ARROW   */
	[FNL_CURSOR_TEXT]        = 32513,   /* IDC_IBEAM   */
	[FNL_CURSOR_POINTER]     = 32649,   /* IDC_HAND    */
	[FNL_CURSOR_CROSSHAIR]   = 32515,   /* IDC_CROSS   */
	[FNL_CURSOR_MOVE]        = 32646,   /* IDC_SIZEALL */
	[FNL_CURSOR_EW_RESIZE]   = 32644,   /* IDC_SIZEWE  */
	[FNL_CURSOR_NS_RESIZE]   = 32645,   /* IDC_SIZENS  */
	[FNL_CURSOR_NOT_ALLOWED] = 32648,   /* IDC_NO      */
	[FNL_CURSOR_WAIT]        = 32514,   /* IDC_WAIT    */
};
_Static_assert(sizeof CALLER_CURSORS / sizeof CALLER_CURSORS[0] ==
			   FNL_CURSOR_WAIT + 1,
			   "a FnlCursor was appended without a system cursor for it here");

/* A system cursor's handle, loaded once: WM_SETCURSOR asks per pointer move.
 * They are shared cursors, the system's, and are never destroyed. */
static HCURSOR cursor_handle(struct BackendState *p, FnlCursor c)
{
	if (!p->cursors[c])
		p->cursors[c] = LoadCursorW(NULL,
									MAKEINTRESOURCEW(CALLER_CURSORS[c]));
	return p->cursors[c];
}

/* Put the right cursor up for where the pointer is in the client area: the
 * caller's over what the caller draws, the arrow over what the library does.
 *
 * Only the client area's half of the decision, so only deco_cursor()'s shape
 * is read. The resize edges and the library's drag strip are answered from
 * WM_NCHITTEST in Windows' own vocabulary, so Windows picks their cursors
 * itself; and a capture hides the cursor through ShowCursor's count, which no
 * SetCursor here undoes.
 *
 * Set every time, and never skipped for being the shape set last: Windows
 * puts its own arrows up over the edges in between, so the cursor on screen
 * is not necessarily the one this function left there.
 *
 * The position is asked for rather than taken from App::ptr_x: WM_SETCURSOR is
 * sent *ahead* of the WM_MOUSEMOVE that would update it, so the stored one is
 * a message behind exactly when the pointer crosses from one zone to the
 * next. */
static void cursor_apply(App *a, HWND hwnd)
{
	POINT pt;
	if (!GetCursorPos(&pt))
		return;
	ScreenToClient(hwnd, &pt);
	const int s120 = want_s120(a);
	Edge edge;
	const Zone z = deco_hit(a, px_to_ptf(pt.x, s120), px_to_ptf(pt.y, s120),
							&edge);
	SetCursor(cursor_handle(a->backend_state, deco_cursor(a, z, edge).shape));
}

/* The rectangle this window wants when maximized: the monitor's work area.
 *
 * Windows offers a maximized WS_THICKFRAME window a rectangle *larger* than
 * the work area for its frame to cover, and this window has none. Wine takes
 * the answer given here, and the window is the work area. Windows does not: a
 * size that fills the work area is read as "maximized" and the frame's overhang
 * goes back on, eight pixels a side at 100%, measured. That half is
 * max_client_rect()'s.
 *
 * ptMaxPosition is relative to the monitor's own origin, not the desktop's.
 * ptMaxTrackSize is left alone: it bounds SetWindowPos as well as maximize,
 * and borderless fullscreen wants the whole monitor. ptMinTrackSize is left
 * alone too, for the reason the size floor below gives. */
static void window_max_rect(HWND hwnd, MINMAXINFO *mmi)
{
	MONITORINFO mi = { .cbSize = sizeof mi };
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
						 &mi))
		return;      /* the system's own defaults stand; they were prefilled */
	const RECT work = monitor_work(&mi);
	mmi->ptMaxPosition.x = work.left - mi.rcMonitor.left;
	mmi->ptMaxPosition.y = work.top  - mi.rcMonitor.top;
	mmi->ptMaxSize.x     = work.right  - work.left;
	mmi->ptMaxSize.y     = work.bottom - work.top;
}

/* WM_NCCALCSIZE for a maximized window: the client area is the part of the
 * proposed rectangle that is on the work area, and the overhang Windows put
 * back is left outside it, off the monitor, where the compositor clips it.
 *
 * An intersection and not an inset by the frame's metrics: it is the same
 * answer on Windows and no change at all where the rectangle already is the
 * work area, which is wine's. IsZoomed() and not App::is_maximized, because
 * this message precedes the WM_SIZE that sets the latter. The monitor is the
 * proposed rectangle's: the window itself is still where it was. */
static void max_client_rect(HWND hwnd, RECT *r)
{
	if (!IsZoomed(hwnd))
		return;
	MONITORINFO mi = { .cbSize = sizeof mi };
	if (!GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi))
		return;
	const RECT work = monitor_work(&mi);
	if (r->left < work.left)
		r->left = work.left;
	if (r->top < work.top)
		r->top = work.top;
	if (r->right > work.right)
		r->right = work.right;
	if (r->bottom > work.bottom)
		r->bottom = work.bottom;
}

/* ---- the size floor, applied after the fact --------------------------------
 *
 * The window may not stay smaller than its own titlebar; deco_min_window() has
 * the figure. ptMinTrackSize is the idiomatic bound and is not used: wine
 * honours it for its own client rectangle and never publishes it to the X
 * server, so mutter goes on shrinking the X window while the client is
 * pinned, and a 34-point bar is painted into a one-pixel window. So nothing
 * bounds the drag, and a timer grows the window back once no mouse button is
 * held. Not WM_EXITSIZEMOVE, which under wine goes missing about as often as
 * it arrives. DESIGN *The size floor* has the rest. */
#define FLOOR_TIMER_ID 1
#define FLOOR_TIMER_MS 120

static bool below_size_floor(App *a, HWND hwnd, int *want_w, int *want_h)
{
	if (!deco_client(a->is_fullscreen) || a->is_maximized || IsIconic(hwnd))
		return false;

	RECT c;
	if (!GetClientRect(hwnd, &c))
		return false;

	const int s120 = want_s120(a);
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	const int px = scale_pt(min_w, s120), py = scale_pt(min_h, s120);
	const int w = (int)(c.right - c.left), h = (int)(c.bottom - c.top);
	if (w >= px && h >= py)
		return false;

	*want_w = w < px ? px : w;
	*want_h = h < py ? py : h;
	return true;
}

/* Arm the check. It does nothing unless the window is under the floor, and
 * nothing twice while one is pending, so every WM_SIZE can call it. */
static void arm_size_floor(App *a, HWND hwnd)
{
	int w, h;
	if (!below_size_floor(a, hwnd, &w, &h)) {
		/* At or above the floor: the next time the window goes under it is
		 * worth another try. */
		a->backend_state->floor_refused = false;
		return;
	}
	if (!a->backend_state->floor_timer && !a->backend_state->floor_refused)
		a->backend_state->floor_timer =
			SetTimer(hwnd, FLOOR_TIMER_ID, FLOOR_TIMER_MS, NULL);
}

static void enforce_size_floor(App *a, HWND hwnd)
{
	/* Still holding a button: a drag in progress, or one paused mid-way. Grow
	 * the window under a pointer that is still resizing it and the two fight;
	 * the timer keeps running and asks again. */
	if (GetAsyncKeyState(VK_LBUTTON) & ASYNC_KEY_DOWN)
		return;

	KillTimer(hwnd, FLOOR_TIMER_ID);
	a->backend_state->floor_timer = 0;

	int w, h;
	if (!below_size_floor(a, hwnd, &w, &h))
		return;

	SetWindowPos(hwnd, NULL, 0, 0, w, h,
				 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	/* Asked, and not necessarily granted. The WM_SIZE this SetWindowPos
	 * provokes arms the timer again, so a manager that keeps the window where
	 * it wants it would be argued with eight times a second. One refusal is
	 * taken as the answer until the window is next seen at or above the
	 * floor. */
	if (below_size_floor(a, hwnd, &w, &h))
		a->backend_state->floor_refused = true;
}

/* Put a fullscreen window back where it belongs. WM_WINDOWPOSCHANGING answers
 * everything that *asks*, and not everything does: wine applies a window
 * manager's own configure directly and synthesises the WM_SIZE from it --
 * seen leaving the maximized state, where the manager's rectangle arrives
 * after the fullscreen move and overwrites it.
 *
 * Re-asserting converges rather than fights: this SetWindowPos goes back
 * through WM_WINDOWPOSCHANGING, the rectangle matches afterwards, and the
 * nested WM_SIZE it provokes returns at the EqualRect test. */
static void hold_fullscreen(App *a, HWND hwnd)
{
	const RECT *f = &a->backend_state->fs_rect;
	RECT r;

	if (!a->is_fullscreen || IsIconic(hwnd) || IsRectEmpty(f))
		return;
	if (!GetWindowRect(hwnd, &r) || EqualRect(&r, f))
		return;

	SetWindowPos(hwnd, HWND_TOP, f->left, f->top, f->right - f->left,
				 f->bottom - f->top, SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

/* WM_SIZE's handler, which a fullscreen change runs where Windows does not. */
static void on_size(App *a, HWND hwnd, WPARAM wp);

/* Borderless fullscreen: the whole monitor, no frame, back where it was after.
 *
 * The window rectangle is saved, and not a WINDOWPLACEMENT to hand back to
 * SetWindowPlacement: a placement records where the window would be with its
 * ordinary frame, so restoring one through a style that has just regained
 * WS_OVERLAPPEDWINDOW adds a frame this window does not draw -- 1280x754 came
 * back 1288x788, and grew again on every round trip. The maximized state is
 * the one thing a bare rectangle cannot carry, so it is remembered beside it
 * and re-applied. */
static void win32_set_fullscreen(App *a, bool on)
{
	struct BackendState *p = a->backend_state;
	if (on == a->is_fullscreen)
		return;
	const uint32_t serial = p->size_serial;

	if (on) {
		/* WS_MAXIMIZE is masked out of the saved style, so the one style
		 * write below clears it along with WS_OVERLAPPEDWINDOW. A maximized
		 * window's rectangle belongs to the manager: where it is already the
		 * monitor's, the SetWindowPos below asks for what the window has, is
		 * skipped, and the style change alone drops the window to its restored
		 * rectangle while it believes it is fullscreen.
		 *
		 * Not ShowWindow(SW_RESTORE) first: that is a round trip through the
		 * manager, whose answer under wine arrives *after* the fullscreen
		 * move and overwrites it. The way out re-enters the maximized state
		 * with ShowWindow(SW_MAXIMIZE), from saved_maximized, and never
		 * through the style. */
		p->saved_style = (DWORD)GetWindowLongPtrW(p->hwnd, GWL_STYLE)
					   & ~(DWORD)WS_MAXIMIZE;
		p->saved_maximized = a->is_maximized;
		/* A maximized window's rectangle is the screen's, which is not what
		 * it should come back to, so what is saved is the restored one. */
		WINDOWPLACEMENT wp = { .length = sizeof wp };
		if (GetWindowPlacement(p->hwnd, &wp))
			p->saved_rect = wp.rcNormalPosition;
		else
			GetWindowRect(p->hwnd, &p->saved_rect);

		MONITORINFO mi = { .cbSize = sizeof mi };
		if (!GetMonitorInfoW(MonitorFromWindow(p->hwnd,
											   MONITOR_DEFAULTTONEAREST), &mi))
			return;

		/* The style loses everything that would draw or reserve a frame. The
		 * bar is not drawn in fullscreen either -- deco_client() is false
		 * there -- so WM_NCCALCSIZE defers to the default, which for this
		 * style is no non-client area at all. */
		SetWindowLongPtrW(
			p->hwnd, GWL_STYLE,
			(LONG_PTR)(p->saved_style & ~(DWORD)WS_OVERLAPPEDWINDOW));
		/* What the window is held at for as long as it is fullscreen; see
		 * WM_WINDOWPOSCHANGING. Recorded before the move, because the move is
		 * the first thing that has to be held. */
		p->fs_rect = mi.rcMonitor;
		a->is_fullscreen = true;   /* before the move, so the WM_SIZE it
									* provokes publishes the right state */
		SetWindowPos(p->hwnd, HWND_TOP,
					 mi.rcMonitor.left, mi.rcMonitor.top,
					 mi.rcMonitor.right - mi.rcMonitor.left,
					 mi.rcMonitor.bottom - mi.rcMonitor.top,
					 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	} else {
		SetWindowLongPtrW(p->hwnd, GWL_STYLE, (LONG_PTR)p->saved_style);
		a->is_fullscreen = false;
		/* FRAMECHANGED with the restored style, so the frame this window does
		 * not want is recalculated away before the size lands on it. */
		SetWindowPos(p->hwnd, NULL,
					 p->saved_rect.left, p->saved_rect.top,
					 p->saved_rect.right - p->saved_rect.left,
					 p->saved_rect.bottom - p->saved_rect.top,
					 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		if (p->saved_maximized)
			ShowWindow(p->hwnd, SW_MAXIMIZE);
	}

	/* The state is published with a size, and a move that changes no size
	 * sends no WM_SIZE. A maximized window on a monitor whose work area is
	 * the whole of it -- no taskbar there, or wine on an X server with no
	 * panel -- already has the monitor's rectangle, and its fullscreen
	 * would never be told: so the handler runs here when nothing ran it. */
	if (p->size_serial == serial)
		on_size(a, p->hwnd, IsZoomed(p->hwnd) ? SIZE_MAXIMIZED
											  : SIZE_RESTORED);

	app_mark_urgent(a);
}

/* ---- the window procedure's handlers --------------------------------------
 *
 * One per message with a body worth a name. Each returns to a case in
 * wnd_proc(), which is where the choice between answering a message and
 * leaving it to DefWindowProc is made. */

/* Edge to Windows' hit code: what WM_NCHITTEST answers with, and what a
 * resize begun through the seam is entered by. EDGE_NONE is the move. */
static const uint8_t WIN32_EDGE_HT[EDGE_COUNT] = {
	[EDGE_NONE] = HTCAPTION,
	[EDGE_T]    = HTTOP,        [EDGE_B]  = HTBOTTOM,
	[EDGE_L]    = HTLEFT,       [EDGE_R]  = HTRIGHT,
	[EDGE_TL]   = HTTOPLEFT,    [EDGE_TR] = HTTOPRIGHT,
	[EDGE_BL]   = HTBOTTOMLEFT, [EDGE_BR] = HTBOTTOMRIGHT,
};

/* WM_NCHITTEST: what the pointer is over, in Windows' own vocabulary.
 *
 * HTCAPTION for the strip buys the whole of the manager's caption behaviour
 * -- drag, double-click to maximize, snap to an edge, the system menu on a
 * right click -- which no sequence of calls reproduces.
 *
 * The edges are answered here because, with no non-client area left,
 * DefWindowProc has nothing to report but HTCLIENT.
 *
 * The buttons, the controls and a caller's run stay HTCLIENT: a point the
 * manager owns gets WM_NC* messages and never a WM_LBUTTONDOWN. */
static LRESULT hit_test(App *a, HWND hwnd, LPARAM lp)
{
	POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   /* screen */
	ScreenToClient(hwnd, &pt);
	const int s120 = want_s120(a);
	Edge edge;
	const Zone z = deco_hit(a, px_to_ptf(pt.x, s120), px_to_ptf(pt.y, s120),
							&edge);
	if (z == ZONE_RESIZE)
		return WIN32_EDGE_HT[edge];
	return z == ZONE_TITLEBAR ? HTCAPTION : HTCLIENT;
}

/* WM_WINDOWPOSCHANGING: a fullscreen window is the monitor, and stays it.
 * Borderless fullscreen is a rectangle this library chose, not a state the
 * window manager is keeping for it, so anything that proposes another is
 * answered with fs_rect -- which under wine the manager does when the window
 * leaves the maximized state. */
static void on_pos_changing(const App *a, WINDOWPOS *pos)
{
	const RECT *f = &a->backend_state->fs_rect;
	pos->x  = f->left;
	pos->y  = f->top;
	pos->cx = f->right - f->left;
	pos->cy = f->bottom - f->top;
	pos->flags &= ~(UINT)(SWP_NOMOVE | SWP_NOSIZE);
}

/* ---- one size at a time ---------------------------------------------------
 *
 * The window procedure does not return from a size until the frame drawn at
 * that size has been presented, or SIZE_AWAIT_MS has passed.
 *
 * A swapchain is built for the client area as it is at that moment, and one
 * that no longer matches by the present is refused by the driver that checks
 * (NVIDIA's: VK_ERROR_OUT_OF_DATE_KHR, and the frame is gone). A new one is
 * 17 to 30 ms on the Windows drivers measured, so sizes arriving faster than
 * that -- a border dragged with any mouse, a caller asking 250 times a
 * second -- left a loop turning at the display's rate and presenting three
 * frames a second. Held here, the next size is taken when the last has been
 * shown, the ones between are never the window's, and every rebuild ends in
 * a frame.
 *
 * Bounded, because a frame is not owed: a covered window presents nothing
 * and a lost device is busy. One wait that timed out is the last until a
 * frame has answered a size again (size_live), so a window that is not
 * drawing costs its thread a quarter of a second once and not per message;
 * and it says so, because a frame that never comes back from the driver is
 * otherwise a window that stopped for no reason anybody can see.
 *
 * Sent messages are dispatched meanwhile and nothing else is: the frame's
 * thread may be inside the driver sending this window one. Not the posted
 * ones -- taking input off the queue inside a size would take the button's
 * release from the loop that is dragging the border -- and pumping those too
 * was measured to change nothing. A size that arrives while this waits
 * publishes and does not wait a second time. */
#define SIZE_AWAIT_MS 250

static void size_await(App *a)
{
	struct BackendState *p = a->backend_state;

	if (p->size_awaiting || !p->size_event ||
		!fnl_atomic_load_bool(&p->size_live) ||
		IsIconic(p->hwnd) || !IsWindowVisible(p->hwnd))
		return;

	const uint32_t serial = p->size_serial;
	const DWORD    t0     = GetTickCount();

	p->size_awaiting = true;
	/* Signed, so a serial that wrapped still orders. */
	while ((int32_t)(fnl_atomic_load_u32(&p->size_shown) - serial) < 0) {
		const DWORD gone = GetTickCount() - t0;
		if (gone >= SIZE_AWAIT_MS) {
			fnl_atomic_store_bool(&p->size_live, false);
			fnl_log(FNL_LOG_WARN, "no frame at the new size in %d ms; sizes "
								  "are not waited for until one is shown",
					SIZE_AWAIT_MS);
			break;
		}
		if (MsgWaitForMultipleObjectsEx(1, &p->size_event,
										SIZE_AWAIT_MS - gone, QS_SENDMESSAGE,
										0) == WAIT_OBJECT_0 + 1) {
			/* Asking is what dispatches them; nothing is taken off the
			 * queue. */
			MSG m;
			PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
	}
	p->size_awaiting = false;
}

/* The frame's thread, just ahead of the present that answers `serial`. The
 * word is passed on in win32_flush(), which is after it. */
static void win32_ack_configure(App *a, uint32_t serial)
{
	fnl_mutex_lock(&a->lock);
	if (a->cfg_serial == serial)
		a->cfg_pending = false;
	fnl_mutex_unlock(&a->lock);
	struct BackendState *p = a->backend_state;
	fnl_mutex_lock(&p->ack_lock);
	p->size_acked = serial;
	p->size_ack_owed = true;
	p->size_ack_calls = a->present_calls;
	fnl_mutex_unlock(&p->ack_lock);
}

/* The size owed, given back to the window's thread, if one is. */
static void size_release_owed(struct BackendState *p)
	FNL_REQUIRES(p->ack_lock)
{
	if (!p->size_ack_owed)
		return;
	p->size_ack_owed = false;
	fnl_atomic_store_u32(&p->size_shown, p->size_acked);
	fnl_atomic_store_bool(&p->size_live, true);
	if (p->size_event)
		SetEvent(p->size_event);
}

/* The frame's thread, once the present has returned -- whatever it returned:
 * a present that failed is a frame the loop draws again, and the size after
 * it must not wait on this one. */
static void win32_flush(App *a)
{
	struct BackendState *p = a->backend_state;

	/* The child a move made has presented: from here on nothing erases it
	 * (present_proc). After the present, as the ack below is. */
	if (p->present && !fnl_atomic_load_bool(&p->present_presented) &&
		a->present_calls != p->present_calls_at_renew)
		fnl_atomic_store_bool(&p->present_presented, true);

	/* Not the flush app_surface_sync() makes ahead of the present: the
	 * size is held until the present has returned, and the frame it shows
	 * waited for where it can be (ack_when_shown) -- by the renderer's
	 * waiter while App::ack_held, which then releases it itself. */
	fnl_mutex_lock(&p->ack_lock);
	if (a->present_calls != p->size_ack_calls &&
		!fnl_atomic_load_bool(&a->ack_held))
		size_release_owed(p);
	fnl_mutex_unlock(&p->ack_lock);
}

/* [renderer's waiter] The frame the size was held for is on the screen. */
static void win32_ack_release(App *a)
{
	struct BackendState *p = a->backend_state;
	fnl_mutex_lock(&p->ack_lock);
	size_release_owed(p);
	fnl_mutex_unlock(&p->ack_lock);
}

/* The child frames are presented into, where a move made one: the whole
 * client area, ahead of the size being published, so the surface the
 * renderer asks after has the size the frame is drawn at. */
static void present_follow(HWND hwnd, HWND present)
{
	RECT c;
	if (present && GetClientRect(hwnd, &c))
		SetWindowPos(present, NULL, 0, 0, c.right - c.left, c.bottom - c.top,
					 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

static void on_size(App *a, HWND hwnd, WPARAM wp)
{
	a->is_maximized = (wp == SIZE_MAXIMIZED);
	present_follow(hwnd, a->backend_state->present);
	publish_size(a);
	/* Nothing bounded the resize, so this is what puts the floor back once
	 * the pointer lets go. */
	arm_size_floor(a, hwnd);
	hold_fullscreen(a, hwnd);
	capture_follow(a, hwnd);
	/* Last: everything above is this size's, and the wait is for its
	 * frame. */
	size_await(a);
}

static const char *win32_host(const App *a);

/* A move to another monitor at the same DPI changes no size. Cheap: the
 * monitor is only asked about when its handle changed.
 *
 * Under wine on X11, with Vulkan, a move also asks for a frame: wine's
 * Vulkan surface there is black after the window moves until the next
 * present, and a window at rest presents nothing -- measured, a moved
 * testbed's capture went from its frame to all black and stayed there.
 * Wine's OpenGL keeps its picture, and Windows' compositor keeps a
 * window's, so nothing is asked there; nor on wine's Wayland driver, which
 * was not seen to lose it. One frame per move, as fnl_invalidate() asks
 * for any. */
static void on_move(App *a, HWND hwnd)
{
	publish_output(a, false);
	hold_fullscreen(a, hwnd);
	capture_follow(a, hwnd);
	/* The window moves while it opens, before a renderer is chosen. */
	if (renderer_any(a) && renderer_any(a)->api == FNL_RENDERER_VULKAN &&
		strcmp(win32_host(a), "wine on X11") == 0)
		fnl_invalidate(a);
}

static void on_dpi_changed(App *a, HWND hwnd, WPARAM wp, LPARAM lp)
{
	/* The suggested rectangle is not advice: refusing it leaves the window
	 * at a physical size that no longer matches its logical one, and the
	 * next drag snaps. */
	a->backend_state->dpi = HIWORD(wp);
	const RECT *sug = (const RECT *)lp;
	SetWindowPos(hwnd, NULL, sug->left, sug->top,
				 sug->right - sug->left, sug->bottom - sug->top,
				 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* WM_SETFOCUS and WM_KILLFOCUS. */
static void on_focus(App *a, bool in)
{
	/* Focus loss never delivers the matching key releases, so every held key
	 * is dropped; and the keys already down are seeded before the caller
	 * hears about the focus, so fnl_key_down() already answers by then. */
	app_keys_clear(a);
	if (in)
		seed_held_keys(a);
	/* The confinement goes with the focus and the capture does not.
	 * ClipCursor is global and ShowCursor is a process-wide counter, so a
	 * window holding both after losing focus has confined and hidden the
	 * cursor over somebody else's window. */
	if (a->pointer_captured)
		capture_apply(a->backend_state, in);
	app_publish_focus(a, in);
	app_post_focus(a, in);
}

/* WM_KEYDOWN and WM_SYSKEYDOWN. A repeat is never a press: it reaches the
 * caller only as the repeat a text field asks for, which app_key_repeat()
 * drops otherwise. */
static void on_key_down(App *a, LPARAM lp)
{
	if (!(lp & LPARAM_KEY_REPEAT))
		app_key(a, key_from_scancode(lp), true);
	else
		app_key_repeat(a, key_from_scancode(lp));
}

/* WM_CHAR: what the key typed. The system has already applied the layout, the
 * dead keys, AltGr and an input method's committed string, which
 * DefWindowProc turns into these. TranslateMessage posts it from the
 * WM_KEYDOWN the loop is about to dispatch, so it lands after that key's
 * event -- the order fenestal.h gives -- and once more per repeat.
 *
 * UTF-16, one unit a message: a high surrogate is held for the low one, and a
 * low one with nothing held types nothing. Control characters come through
 * here too and are app_text()'s to drop. */
static void on_char(App *a, WPARAM wp)
{
	const uint32_t u = (uint32_t)wp & 0xffffu;
	if (u >= 0xd800 && u <= 0xdbff) {
		a->backend_state->char_high = u;
		return;
	}
	uint32_t cp = u;
	if (u >= 0xdc00 && u <= 0xdfff) {
		if (!a->backend_state->char_high)
			return;
		cp = 0x10000u + ((a->backend_state->char_high - 0xd800u) << 10) +
			 (u - 0xdc00u);
	}
	a->backend_state->char_high = 0;
	app_text(a, cp);
}

/* ---- the input method -------------------------------------------------------
 *
 * What an input method is composing is the caller's to draw, as on Wayland
 * and macOS: FNL_EVENT_COMPOSE carries it, and the system's composition
 * window is never shown. Left to DefWindowProc, wine on X11 showed a dead
 * key's accent in a white box of its own at the window's bottom-left, an
 * override-redirect X window mapped for as long as the accent was held. The
 * committed text is typed from WM_IME_COMPOSITION itself, which is then not
 * passed on, so it is never typed a second time as WM_IME_CHAR.
 *
 * imm32 is opened at run time and not linked (DESIGN *One build per
 * operating system*, *The link line*), once a process, and only ever used
 * on the window's thread. Where it will not open, every message goes to
 * DefWindowProc as it did before. */

typedef HIMC (WINAPI *ImmGetContextFn)(HWND);
typedef BOOL (WINAPI *ImmReleaseContextFn)(HWND, HIMC);
typedef LONG (WINAPI *ImmGetCompositionStringWFn)(HIMC, DWORD, LPVOID,
												  DWORD);
typedef BOOL (WINAPI *ImmNotifyIMEFn)(HIMC, DWORD, DWORD, DWORD);
typedef BOOL (WINAPI *ImmSetCompositionWindowFn)(HIMC, LPCOMPOSITIONFORM);
typedef BOOL (WINAPI *ImmSetCandidateWindowFn)(HIMC, LPCANDIDATEFORM);

static struct {
	bool                       tried;
	ImmGetContextFn            get;
	ImmReleaseContextFn        release;
	ImmGetCompositionStringWFn string;
	ImmNotifyIMEFn             notify;
	ImmSetCompositionWindowFn  place_composition;
	ImmSetCandidateWindowFn    place_candidates;
} imm;

static bool imm_load(void)
{
	if (imm.tried)
		return imm.get != NULL;
	imm.tried = true;
	static const char *const NAMES[] = { "imm32.dll", NULL };
	char why[160] = "FNL_FAULT";
	void *lib = FNL_FAULT("win32-no-imm32")
					? NULL : fnl_lib_open(NAMES, why, sizeof why);
	if (lib) {
		imm.get = (ImmGetContextFn)fnl_lib_sym(lib, "ImmGetContext");
		imm.release = (ImmReleaseContextFn)fnl_lib_sym(lib,
													   "ImmReleaseContext");
		imm.string = (ImmGetCompositionStringWFn)fnl_lib_sym(
			lib, "ImmGetCompositionStringW");
		imm.notify = (ImmNotifyIMEFn)fnl_lib_sym(lib, "ImmNotifyIME");
		imm.place_composition = (ImmSetCompositionWindowFn)fnl_lib_sym(
			lib, "ImmSetCompositionWindow");
		imm.place_candidates = (ImmSetCandidateWindowFn)fnl_lib_sym(
			lib, "ImmSetCandidateWindow");
		if (!imm.get || !imm.release || !imm.string || !imm.notify ||
			!imm.place_composition || !imm.place_candidates) {
			snprintf(why, sizeof why, "imm32.dll lacks a function");
			imm.get = NULL;
		}
	}
	if (!imm.get) {
		fnl_log(FNL_LOG_WARN, "no input method composition (%s): the "
				"system shows it in a window of its own", why);
		return false;
	}
	return true;
}

/* One of the context's strings as UTF-8, empty when there is none. Cut to
 * WCHARs the buffer holds; the composition is then cut again to what
 * App::compose holds (app_compose_set()). */
static void imm_string(HIMC c, DWORD which, char *out, int out_n)
{
	WCHAR w[256];
	const LONG got = imm.string(c, which, w, (DWORD)(sizeof w - sizeof *w));
	const size_t n = got > 0 ? (size_t)got / sizeof *w : 0;
	w[n] = 0;
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_n, NULL, NULL))
		out[0] = '\0';
}

/* WM_IME_COMPOSITION: what was committed is typed, then the composition
 * becomes what is left of it, in Wayland's order (ti_done()). False where
 * there is no imm32, and DefWindowProc is to have the message. */
static bool on_composition(App *a, HWND hwnd, LPARAM lp)
{
	if (!imm_load())
		return false;
	const HIMC c = imm.get(hwnd);
	if (!c)
		return false;
	char u[sizeof a->compose * 4];
	if (lp & GCS_RESULTSTR) {
		imm_string(c, GCS_RESULTSTR, u, (int)sizeof u);
		app_text_utf8(a, u);
	}
	u[0] = '\0';
	if ((lp & GCS_COMPSTR) && app_text_input(a))
		imm_string(c, GCS_COMPSTR, u, (int)sizeof u);
	imm.release(hwnd, c);
	app_compose_set(a, u);
	return true;
}

/* The caret (fnl_set_text_rect_pt()) to the input method, in client pixels:
 * the window's points with no ring around them, as on_motion() reads them.
 * A field that never said gets the window's corner, as on Wayland. The
 * candidate list is told to keep off the caret's rectangle; the composition
 * window is never shown, but placed as well, because an IME that puts its
 * candidates beside its composition reads that one and not the other. */
static void imm_place(App *a, HWND hwnd)
{
	if (!hwnd || !imm_load())
		return;
	int x, y, w, h;
	if (!app_text_rect(a, &x, &y, &w, &h)) {
		x = y = 0;
		w = h = 1;
	}
	const int s120 = want_s120(a);
	const LONG l = scale_pt(x, s120), t = scale_pt(y, s120);
	const LONG r = l + scale_pt(w, s120), b = t + scale_pt(h, s120);
	const HIMC c = imm.get(hwnd);
	if (!c)
		return;
	COMPOSITIONFORM cf = { .dwStyle = CFS_POINT, .ptCurrentPos = { l, t } };
	CANDIDATEFORM cand = { .dwIndex = 0, .dwStyle = CFS_EXCLUDE,
						   .ptCurrentPos = { l, t },
						   .rcArea = { l, t, r, b } };
	imm.place_composition(c, &cf);
	imm.place_candidates(c, &cand);
	imm.release(hwnd, c);
}

/* Text entry went on, or its caret moved: the input method is told where.
 * It went off: a composition still up is cancelled, so it neither lingers on
 * the caller's screen nor commits into the next one. */
static void win32_text_input_changed(App *a)
{
	const HWND hwnd = a->backend_state->hwnd;
	if (app_text_input(a)) {
		imm_place(a, hwnd);
		return;
	}
	if (imm.get && hwnd) {
		const HIMC c = imm.get(hwnd);
		if (c) {
			imm.notify(c, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
			imm.release(hwnd, c);
		}
	}
	app_compose_set(a, NULL);
}

/* A wheel turn, in WHEEL_DELTA units, as the caller's scroll event. WHEEL_DELTA
 * is one detent, so this is already in notches. Win32 counts a vertical turn
 * up and fenestal.h counts it down. */
static void wheel_publish(App *a, bool horizontal, int delta)
{
	const double d = (double)delta / WHEEL_DELTA;
	if (horizontal)
		app_pointer_scroll(a, a->ptr_x, a->ptr_y, d, 0.0);
	else
		app_pointer_scroll(a, a->ptr_x, a->ptr_y, 0.0, -d);
}

/* Whether a wheel step now is this window's by raw input's rule as well as
 * the wheel message's: wine sends raw input to the foreground process only,
 * and the message to the window under the pointer. Where both hold, the raw
 * event is the step and the message its echo; anywhere else the message is
 * all there is. Wine queues a step's raw event ahead of its message, so the
 * raw one is always read first. */
static bool wheel_raw_here(const struct BackendState *p)
{
	POINT pt;
	return GetForegroundWindow() == p->hwnd && GetCursorPos(&pt) &&
		   GetAncestor(WindowFromPoint(pt), GA_ROOT) == p->hwnd;
}

/* A captured pointer's raw report: its movement, as the reticle's and the
 * caller's. */
static void raw_motion(App *a, const RAWMOUSE *m)
{
	/* A report of a button alone moves nothing, and is no motion to the
	 * caller nor to fnl_input_latency_ms(). */
	if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE) && (m->lLastX || m->lLastY)) {
		/* Raw input counts device movement, which lands on this window as
		 * pixels: the reticle's own unit. */
		const double dx = (double)m->lLastX;
		const double dy = (double)m->lLastY;
		app_reticle_move(a, dx, dy);

		/* The caller's copy is in *points*, like every position this backend
		 * reports: a caller integrates these to move a reticle of its own,
		 * and in pixels it would run fast above 100%. */
		const int s120 = want_s120(a);
		app_pointer_relative(a, px_to_ptf(dx, s120), px_to_ptf(dy, s120));
	}
}

/* WM_INPUT: a wheel step under wine, whether or not the pointer is
 * captured, and a captured pointer's motion. The message goes on to
 * DefWindowProc whatever was read of it: RIM_INPUT asks for that, and it is
 * what frees the report (rawin_proc() does the same). */
static void on_raw_input(App *a, LPARAM lp)
{
	struct BackendState *p = a->backend_state;
	if (!a->pointer_captured && !p->wheel_raw)
		return;
	RAWINPUT ri;
	UINT size = sizeof ri;
	if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
						sizeof(RAWINPUTHEADER)) == (UINT)-1)
		return;
	if (p->wheel_raw && ri.header.dwType == RIM_TYPEMOUSE &&
		(ri.data.mouse.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) &&
		wheel_raw_here(p)) {
		p->wheel_raw_seen = true;
		wheel_publish(a, (ri.data.mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0,
					  (SHORT)ri.data.mouse.usButtonData);
	}
	if (a->pointer_captured && ri.header.dwType == RIM_TYPEMOUSE)
		raw_motion(a, &ri.data.mouse);
}

/* FNL_WM_RAW, and ahead of every input message while captured, so that a
 * click lands after the motion before it: the captured pointer's reports
 * the raw input thread handed on, each stamped with when it arrived there.
 * Only while this window is in front, as raw input registered on it
 * without INPUTSINK was. */
static void raw_forwarded(App *a)
{
	struct BackendState *p = a->backend_state;
	RawForward got[32];
	int n;
	while ((n = rawin_forwarded(p->rawin, got, 32)) > 0) {
		if (!a->pointer_captured || GetForegroundWindow() != p->hwnd)
			continue;
		const double now = now_seconds();
		for (int i = 0; i < n && a->pointer_captured; i++) {
			app_input_age(a, now - got[i].t);
			raw_motion(a, &got[i].m);
		}
	}
}

/* WM_MOUSEMOVE: the pointer's position in logical points, the space the
 * overlay draws in; Win32 reports client-area pixels. Suppressed while
 * captured, when WM_INPUT's deltas carry the movement. */
static void on_motion(App *a, HWND hwnd, LPARAM lp)
{
	if (a->pointer_captured)
		return;

	const int s120 = want_s120(a);
	a->ptr_x = px_to_ptf(GET_X_LPARAM(lp), s120);
	a->ptr_y = px_to_ptf(GET_Y_LPARAM(lp), s120);

	/* Asked for once per entry, and re-armed by the leave itself. Win32 sends
	 * nothing to say the pointer has gone unless asked, and a titlebar button
	 * would stay lit. */
	if (!a->backend_state->tracking_leave) {
		TRACKMOUSEEVENT tme = { .cbSize = sizeof tme,
								.dwFlags = TME_LEAVE,
								.hwndTrack = hwnd };
		a->backend_state->tracking_leave = TrackMouseEvent(&tme) != 0;
	}
	app_pointer_motion(a, a->ptr_x, a->ptr_y);
	Edge edge;
	deco_pointer_at(a, a->ptr_x, a->ptr_y, &edge);
}

/* WM_MOUSEWHEEL and WM_MOUSEHWHEEL: the step, or under wine the echo of
 * one on_raw_input() has already published. */
static void on_wheel(App *a, UINT msg, WPARAM wp)
{
	if (a->backend_state->wheel_raw_seen && wheel_raw_here(a->backend_state))
		return;
	wheel_publish(a, msg == WM_MOUSEHWHEEL, GET_WHEEL_DELTA_WPARAM(wp));
}

/* Raw input from the start under wine, for the wheel: see
 * BackendState::wheel_raw. Not on Windows, whose WM_MOUSEWHEEL loses
 * nothing and reaches an inactive window under the pointer, where raw input
 * without INPUTSINK does not. */
static void raw_wheel_open(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!win32_host(a)[0])
		return;
	if (FNL_FAULT("win32-no-raw-wheel") || !raw_register(p)) {
		fnl_log(FNL_LOG_WARN, "raw mouse input unavailable; under wine a "
							  "wheel step can be lost");
		return;
	}
	p->wheel_raw = true;
}

/* The six button messages. The window holds the mouse capture while any
 * button is down, which is what brings the release here when the pointer
 * has left the window before it; without it the caller would be left
 * holding the button. */
static void on_button(App *a, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
					  msg == WM_MBUTTONDOWN;
	const int s120 = want_s120(a);
	FnlButton b;
	switch (msg) {
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		b = FNL_BUTTON_RIGHT;
		break;
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		b = FNL_BUTTON_MIDDLE;
		break;
	default:
		b = FNL_BUTTON_LEFT;
		break;
	}
	/* Taken before the press is acted on, as win32_drag() releases it. */
	if (down)
		SetCapture(hwnd);
	/* A press on the bar's drag strip or on an edge never arrives here:
	 * WM_NCHITTEST gave those to Windows, which runs the drag itself. */
	app_pointer_button(a, b, down, px_to_ptf(GET_X_LPARAM(lp), s120),
					   px_to_ptf(GET_Y_LPARAM(lp), s120),
					   (uint32_t)GetMessageTime());
	/* An up message's key state is the one after this release. */
	if (!down && !(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) &&
		GetCapture() == hwnd)
		ReleaseCapture();
}

/* WM_CAPTURECHANGED with a button still down: the capture went to
 * win32_drag()'s move-size loop, which eats the release, or to another
 * window, and no up message will come. Each press posted is released here,
 * where the pointer was last seen, or the caller holds it until that button
 * is next pressed and the titlebar stays armed. */
static void on_capture_lost(App *a)
{
	for (int b = FNL_BUTTON_LEFT; b <= FNL_BUTTON_RIGHT; b++)
		if (a->buttons_down & (1u << b))
			app_pointer_button(a, (FnlButton)b, false, a->ptr_x, a->ptr_y,
							   (uint32_t)GetMessageTime());
}

/* ---- the window procedure ------------------------------------------------ */

/* Wine: ntdll exports wine_get_version under wine and nowhere else. */
static bool under_wine(void)
{
	const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	return nt && GetProcAddress(nt, "wine_get_version");
}

/* ---- the window frames are presented into, after a move -----------------
 *
 * The window itself until a move to another GPU, and from the first move a
 * child filling its client area, made again at every move: a driver that
 * set the window's pixel format has it for the window's life, and the next
 * GPU's swapchain is then refused (FnlBackendVtbl::present_window_renew).
 * Everything measured on Windows before was measured on the window itself,
 * so a window nobody moves keeps presenting there. Under wine on Wayland
 * the child is made at open (win32_open() says why).
 *
 * The child is the frames' alone: every hit test is passed to the window
 * under it (HTTRANSPARENT), so the pointer, the cursor, the zones and the
 * focus are the window's as they were, and it is never painted. */
static const wchar_t FNL_PRESENT_CLASS[] = L"FenestalPresent";

/* The child's erase, until its first present: the last frame the move read
 * back (App::snap), stretched over the client area as a top-down 32-bit
 * DIB -- BGRA, which the snapshot always is, so it is drawn from in place
 * -- and black where there is none. Under App::lock, which the render
 * thread takes to replace the block (app_snapshot_set()). */
static void present_erase(App *a, HWND hwnd, HDC dc)
{
	RECT r;
	if (!GetClientRect(hwnd, &r))
		return;
	bool drawn = false;
	fnl_mutex_lock(&a->lock);
	if (a->snap_valid && a->snap_px) {
		const int w = a->snap_w, h = a->snap_h;
		BITMAPINFO bmi;
		memset(&bmi, 0, sizeof bmi);
		bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
		bmi.bmiHeader.biWidth = w;
		bmi.bmiHeader.biHeight = -h;            /* top row first */
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		SetStretchBltMode(dc, COLORONCOLOR);
		const int lines = StretchDIBits(dc, 0, 0, r.right - r.left,
										r.bottom - r.top, 0, 0, w, h,
										a->snap_px, &bmi, DIB_RGB_COLORS,
										SRCCOPY);
		drawn = lines > 0 && lines != (int)GDI_ERROR;
		/* For a loss during the lift, which lifts again from what the
		 * gap showed (device_recover()). */
		a->snap_shown = a->snap_shown || drawn;
	}
	fnl_mutex_unlock(&a->lock);
	if (!drawn)
		FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

static LRESULT CALLBACK present_proc(HWND hwnd, UINT msg, WPARAM wp,
									 LPARAM lp)
{
	switch (msg) {
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_ERASEBKGND: {
		/* Until the child's first present (BackendState::present_presented,
		 * win32_flush()): the last frame before the move, or black, so
		 * what shows between the old GPU's last frame and the new one's
		 * first is that frame -- or the colour the veil fades to (DESIGN
		 * *The veil*) -- and not whatever the redirection surface held.
		 * Never after: an erase under a frame would flash. The App is the
		 * parent's, as wnd_proc keeps it. */
		App *a = (App *)GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA);
		if (a && a->backend_state &&
			!fnl_atomic_load_bool(&a->backend_state->present_presented))
			present_erase(a, hwnd, (HDC)wp);
		return 1;
	}
	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

/* FNL_WM_PRESENT_RENEW, on the window's thread: the old child gone, its
 * surface already torn down by the renderer, and a new one in its place.
 * A child that will not be made leaves the frames going where they went,
 * with one line. */
static void present_renew(App *a, HWND hwnd)
{
	struct BackendState *p = a->backend_state;
	if (!p->present_class) {
		const WNDCLASSEXW pc = {
			.cbSize        = sizeof pc,
			.lpfnWndProc   = present_proc,
			.hInstance     = p->instance,
			.lpszClassName = FNL_PRESENT_CLASS,
		};
		p->present_class = RegisterClassExW(&pc);
		if (!p->present_class) {
			fnl_log(FNL_LOG_WARN, "RegisterClassExW for the frames' window "
					"failed (%lu); they go into the window itself",
					GetLastError());
			return;
		}
		/* The window paints nothing over its child -- nor once a
		 * fullscreen made now ends, and puts back the style it saved. */
		SetWindowLongPtrW(hwnd, GWL_STYLE,
						  GetWindowLongPtrW(hwnd, GWL_STYLE) |
							  WS_CLIPCHILDREN);
		p->saved_style |= WS_CLIPCHILDREN;
	}
	RECT c = { 0, 0, 0, 0 };
	GetClientRect(hwnd, &c);
	/* Erased until its first present (present_proc): marked here, on this
	 * thread, before the child exists, and painted below before the render
	 * thread goes on. Marked on the render thread once the message had
	 * returned, the mark could follow the child's first paint, and under
	 * Direct3D 12 on both laptops the child did not erase at all: the
	 * window showed the erase of the move before until the first frame. */
	fnl_atomic_store_bool(&p->present_presented, false);
	p->present_calls_at_renew = a->present_calls;
	HWND made = CreateWindowExW(WS_EX_NOPARENTNOTIFY, FNL_PRESENT_CLASS, L"",
								WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
								c.right - c.left, c.bottom - c.top, hwnd,
								NULL, p->instance, NULL);
	if (!made) {
		fnl_log(FNL_LOG_WARN, "the frames' window was not made (%lu); they "
				"go where they went", GetLastError());
		return;
	}
	/* Painted now, before the old child goes. A child's GDI paint lands in
	 * the window's redirection surface, and the frames never do, so the
	 * surface under the old child holds the erase of the move before; the
	 * desktop shows it from the old swapchain's going to the new child's
	 * first paint, which waits for an idle queue -- one to four composed
	 * frames of a frame a move old, measured on both laptops under Vulkan.
	 * Erased synchronously, the fresh frame is there first. */
	RedrawWindow(made, NULL, NULL,
				 RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW);
	if (p->present)
		DestroyWindow(p->present);
	p->present = made;
}

/* FNL_WM_PRESENT_HOLD, on the window's thread: the gap's picture into the
 * window the frames go into, its swapchain still up -- the child's, or the
 * window's own before the first move. GDI lands in the redirection
 * surface, which the desktop shows once the swapchain has gone and until
 * the next frames' window is painted (present_renew()); left alone, that
 * is the erase of the move before, or of none: an old frame, or whatever
 * GDI last put there. What is painted is the frame read back, which is the
 * frame on screen, or black, which is the veil's: either way nothing is
 * seen to change while the swapchain is up. */
static void present_hold(App *a, HWND hwnd)
{
	struct BackendState *p = a->backend_state;
	HWND w = p->present ? p->present : hwnd;
	HDC dc = GetDC(w);
	if (!dc)
		return;
	present_erase(a, w, dc);
	ReleaseDC(w, dc);
}

/* ---- asked of the window's thread, with a bound ---------------------------
 *
 * Sent and not posted, so that each returns with the work done, and with a
 * bound, so that a window thread joining the render thread at shutdown
 * cannot hold it for ever -- a wait on the window's thread dispatches sent
 * messages (size_await()), a join does not. A send that times out leaves
 * its message queued, to be done when the window's thread comes to it,
 * after the render thread has gone on: a renew then would destroy the
 * child the swapchain built since is on, and a hold would paint under it.
 * So each ask is a ticket, taken under App::lock by whichever thread comes
 * to it first: the window's thread acts on the ticket outstanding alone,
 * and the render thread, timed out, gives the ticket up. Where the
 * window's thread has it in hand the render thread waits for it to finish,
 * without a bound: the window's thread is then running it, so it is not
 * the thread joining, and nothing it does waits on the render thread. An
 * ask given up and not yet come to says the window's thread is not
 * answering, and the next is not sent to wait out the bound again. */
#define PRESENT_ASK_MS 2000

enum { ASK_NONE, ASK_SENT, ASK_TAKEN, ASK_DONE, ASK_DROPPED };

/* [render] True where the window's thread did what was asked. */
static bool ask_send(App *a, UINT msg)
{
	struct BackendState *p = a->backend_state;
	fnl_mutex_lock(&a->lock);
	if (p->ask_state == ASK_DROPPED) {
		fnl_mutex_unlock(&a->lock);
		return false;
	}
	const uint32_t ticket = ++p->ask_ticket;
	p->ask_state = ASK_SENT;
	fnl_mutex_unlock(&a->lock);
	DWORD_PTR r;
	if (SendMessageTimeoutW(p->hwnd, msg, (WPARAM)ticket, 0, SMTO_NORMAL,
							PRESENT_ASK_MS, &r)) {
		fnl_mutex_lock(&a->lock);
		const bool done = p->ask_state == ASK_DONE;
		fnl_mutex_unlock(&a->lock);
		return done;
	}
	bool said = false;
	for (;;) {
		fnl_mutex_lock(&a->lock);
		const int state = p->ask_state;
		if (state == ASK_SENT)
			p->ask_state = ASK_DROPPED;
		fnl_mutex_unlock(&a->lock);
		if (state != ASK_TAKEN)
			return state == ASK_DONE;
		if (!said)
			fnl_log(FNL_LOG_INFO, "the window's thread is slow with the "
					"frames' window; waited for");
		said = true;
		fnl_sleep_seconds(0.001);
	}
}

/* [window] Whether the ask `wp` names is the one outstanding, and taken if
 * so; ask_done() once it is done. One given up on, come to now, says the
 * window's thread answers again. */
static bool ask_take(App *a, WPARAM wp)
{
	struct BackendState *p = a->backend_state;
	fnl_mutex_lock(&a->lock);
	const bool now = (uint32_t)wp == p->ask_ticket;
	const bool mine = now && p->ask_state == ASK_SENT;
	if (mine)
		p->ask_state = ASK_TAKEN;
	else if (now && p->ask_state == ASK_DROPPED)
		p->ask_state = ASK_NONE;
	fnl_mutex_unlock(&a->lock);
	return mine;
}

/* [window] The ask `wp` names, done: no later ask is marked by it, though
 * none can be outstanding while one is taken (ask_send()). */
static void ask_done(App *a, WPARAM wp)
{
	struct BackendState *p = a->backend_state;
	fnl_mutex_lock(&a->lock);
	if ((uint32_t)wp == p->ask_ticket && p->ask_state == ASK_TAKEN)
		p->ask_state = ASK_DONE;
	fnl_mutex_unlock(&a->lock);
}

/* The seam's entries, on the render thread. */
static void win32_present_window_renew(App *a)
{
	if (!ask_send(a, FNL_WM_PRESENT_RENEW))
		fnl_log(FNL_LOG_WARN, "the window's thread did not make the frames' "
				"window in %d ms; they go where they went", PRESENT_ASK_MS);
}

static void win32_present_window_hold(App *a)
{
	if (!ask_send(a, FNL_WM_PRESENT_HOLD))
		fnl_log(FNL_LOG_WARN, "the window's thread did not paint the gap in "
				"%d ms; it shows what was there", PRESENT_ASK_MS);
}

/* An input message's age, for fnl_input_latency_ms(). GetMessageTime() is
 * on GetTickCount()'s clock, so the two's difference in 32 bits is the age,
 * in that clock's steps -- about 16 ms, which a mean over many inputs
 * reads through, since the two readings fall anywhere in their steps.
 * Where the raw input thread runs, the report the message stands for gives
 * it to a fraction of a millisecond instead (win32_rawin.c), and the tick's
 * age is what picks that report out. */
static bool is_input_msg(UINT msg)
{
	return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
		   (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
}

/* The raw transition a button or wheel message is, by the button the
 * device reported: Windows swaps the messages, and not the reports, for a
 * left-handed mouse. 0 for any other message. */
static unsigned raw_flag(UINT msg)
{
	const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
	switch (msg) {
	case WM_LBUTTONDOWN: return swap ? RI_MOUSE_RIGHT_BUTTON_DOWN
									 : RI_MOUSE_LEFT_BUTTON_DOWN;
	case WM_LBUTTONUP:   return swap ? RI_MOUSE_RIGHT_BUTTON_UP
									 : RI_MOUSE_LEFT_BUTTON_UP;
	case WM_RBUTTONDOWN: return swap ? RI_MOUSE_LEFT_BUTTON_DOWN
									 : RI_MOUSE_RIGHT_BUTTON_DOWN;
	case WM_RBUTTONUP:   return swap ? RI_MOUSE_LEFT_BUTTON_UP
									 : RI_MOUSE_RIGHT_BUTTON_UP;
	case WM_MBUTTONDOWN: return RI_MOUSE_MIDDLE_BUTTON_DOWN;
	case WM_MBUTTONUP:   return RI_MOUSE_MIDDLE_BUTTON_UP;
	case WM_MOUSEWHEEL:  return RI_MOUSE_WHEEL;
	case WM_MOUSEHWHEEL: return RI_MOUSE_HWHEEL;
	default:             return 0;
	}
}

static void input_stamp(App *a, UINT msg, LPARAM lp)
{
	struct BackendState *p = a->backend_state;
	const double tick_age =
		(double)(DWORD)(GetTickCount() - (DWORD)GetMessageTime()) * 1e-3;
	if (p->rawin) {
		if (a->pointer_captured)
			raw_forwarded(a);
		double age = -1.0;
		switch (msg) {
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
			age = rawin_key(p->rawin, (unsigned)(lp >> 16) & 0xffu,
							(lp >> 24) & 1,
							msg == WM_KEYUP || msg == WM_SYSKEYUP, tick_age);
			break;
		case WM_MOUSEMOVE:
			if (a->pointer_captured) {
				rawin_moves_spend(p->rawin);
			} else {
				double sum, oldest;
				uint32_t n;
				if (rawin_moves(p->rawin, tick_age, &sum, &oldest, &n)) {
					app_input_ages(a, sum, oldest, n);
					return;
				}
			}
			break;
		default:
			if (raw_flag(msg))
				age = rawin_button(p->rawin, raw_flag(msg), tick_age);
			break;
		}
		if (age >= 0.0) {
			app_input_age(a, age);
			return;
		}
	}
	app_input_age(a, tick_age);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	/* WM_NCCREATE is the first message that carries the App, as
	 * lpCreateParams. Installed here rather than from CreateWindowExW's
	 * return value because WM_NCCALCSIZE, which decides whether this window
	 * has a frame at all, is delivered during creation. */
	if (msg == WM_NCCREATE)
		SetWindowLongPtrW(hwnd, GWLP_USERDATA,
						  (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);

	App *a = (App *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (!a)
		return DefWindowProcW(hwnd, msg, wp, lp);
	if (is_input_msg(msg))
		input_stamp(a, msg, lp);

	switch (msg) {
	/* What app_wakeup() posts, answered by doing the *loop's* work.
	 * DefWindowProc runs an inner message loop for a titlebar drag or a
	 * border resize, so fnl_run() does not come round to spend App::req until
	 * the button comes up -- and a *posted* message is dispatched by that
	 * inner loop. Re-entrant and safe for it: app_drain_requests() clears the
	 * block under the lock before spending any of it. fnl_quit() is not
	 * reached this way and lands on release; see DESIGN *Win32*. */
	case FNL_WM_WAKEUP:
		app_drain_requests(a);
		return 0;

	case FNL_WM_PRESENT_RENEW:
		if (ask_take(a, wp)) {
			present_renew(a, hwnd);
			ask_done(a, wp);
		}
		return 0;

	case FNL_WM_PRESENT_HOLD:
		if (ask_take(a, wp)) {
			present_hold(a, hwnd);
			ask_done(a, wp);
		}
		return 0;

	case FNL_WM_RAW:
		if (a->backend_state->rawin)
			raw_forwarded(a);
		return 0;

	/* Not while minimized, or the taskbar could not put the window down. */
	case WM_WINDOWPOSCHANGING:
		if (!a->is_fullscreen || IsIconic(hwnd))
			break;
		on_pos_changing(a, (WINDOWPOS *)lp);
		return 0;

	/* The system frame, removed: returning the proposed rectangle unchanged
	 * says all of the window is client area. The style keeps WS_CAPTION and
	 * WS_THICKFRAME (see win32_open), so it is the frame's drawing that goes
	 * and not its behaviour.
	 *
	 * The one exception is a maximized window on Windows, whose rectangle
	 * overhangs the work area: see max_client_rect().
	 *
	 * Both spellings of the message are answered here -- wParam FALSE carries
	 * a RECT and TRUE an NCCALCSIZE_PARAMS whose first member is that RECT --
	 * because under wine the only one this window ever receives is the FALSE
	 * one at creation. */
	case WM_NCCALCSIZE:
		if (!deco_client(a->is_fullscreen))
			break;              /* fullscreen: the default holds */
		max_client_rect(hwnd, (RECT *)lp);
		return 0;               /* what is left is all client area */

	/* How large to be maximized, and only that: the floor is applied after a
	 * resize and is not this message's. See window_max_rect(). */
	case WM_GETMINMAXINFO:
		if (!deco_client(a->is_fullscreen))
			break;              /* no bar to protect; the defaults hold */
		window_max_rect(hwnd, (MINMAXINFO *)lp);
		return 0;

	case WM_NCHITTEST:
		if (!deco_client(a->is_fullscreen))
			break;              /* no zones of ours; HTCLIENT throughout */
		return hit_test(a, hwnd, lp);

	/* Sent on every pointer move with the hit-test answer in the low word.
	 * Anything but HTCLIENT is left to DefWindowProc, which draws the sizing
	 * arrows over the edges WM_NCHITTEST named -- except a caller's own
	 * bar, whose drag strip wears the caller's shape on every backend
	 * (deco_cursor_is_callers()). */
	case WM_SETCURSOR:
		if (LOWORD(lp) != HTCLIENT &&
			!(LOWORD(lp) == HTCAPTION && a->deco_custom))
			break;
		cursor_apply(a, hwnd);
		return TRUE;

	case WM_MOUSELEAVE:
		a->backend_state->tracking_leave = false;
		deco_publish_hover(a, ZONE_CONTENT);
		return 0;

	case WM_TIMER:
		if (wp == FLOOR_TIMER_ID) {
			enforce_size_floor(a, hwnd);
			return 0;
		}
		break;

	case WM_SIZE:
		on_size(a, hwnd, wp);
		return 0;

	case WM_MOVE:
		on_move(a, hwnd);
		return 0;

	/* The monitor changed mode, or one was plugged in: same handle, different
	 * numbers, so the re-read is forced. */
	case WM_DISPLAYCHANGE:
		publish_output(a, true);
		return 0;

	/* Back from a sleep. AUTOMATIC and not RESUMESUSPEND: the second comes
	 * only once a hand has been on the machine, and a timer's wake has
	 * none. */
	case WM_POWERBROADCAST:
		if (wp == PBT_APMRESUMEAUTOMATIC)
			app_resumed(a);
		return TRUE;

	/* And so is a work area that moved under the same handle -- a taskbar
	 * resized or sent to another edge. The message is still DefWindowProc's. */
	case WM_SETTINGCHANGE:
		if (wp == SPI_SETWORKAREA)
			publish_output(a, true);
		break;

	case WM_DPICHANGED:
		on_dpi_changed(a, hwnd, wp, lp);
		return 0;

	case WM_SETFOCUS:
		on_focus(a, true);
		return 0;

	case WM_KILLFOCUS:
		on_focus(a, false);
		return 0;

	/* Answered, so WM_SYSKEYDOWN is swallowed as well. F10 is a system key on
	 * Windows -- it activates the menu bar -- and left to DefWindowProc the
	 * window loses focus to a menu that is not there. Alt+F4 alone goes on
	 * to DefWindowProc, which turns it into SC_CLOSE and so WM_CLOSE. */
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		on_key_down(a, lp);
		if (msg == WM_SYSKEYDOWN && wp == VK_F4)
			break;
		return 0;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		app_key(a, key_from_scancode(lp), false);
		return 0;

	case WM_CHAR:
		on_char(a, wp);
		return 0;

	/* Alt and a letter is a menu's mnemonic, and with no menu to take it
	 * DefWindowProc beeps. Alt+Space still opens the system menu. */
	case WM_SYSCOMMAND:
		if ((wp & 0xfff0) == SC_KEYMENU && lp != ' ')
			return 0;
		break;

	/* The input method's composition is the caller's to draw (above
	 * on_composition()), so the system is told to show none of it. */
	case WM_IME_SETCONTEXT:
		if (imm_load())
			lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
		break;

	/* Placed again as each composition starts, as well as when the caret
	 * moves: what the context held from before is not relied on. */
	case WM_IME_STARTCOMPOSITION:
		if (!imm_load())
			break;
		imm_place(a, hwnd);
		return 0;

	case WM_IME_COMPOSITION:
		if (on_composition(a, hwnd, lp))
			return 0;
		break;

	case WM_IME_ENDCOMPOSITION:
		app_compose_set(a, NULL);
		if (imm_load())
			return 0;
		break;

	case WM_INPUT:
		on_raw_input(a, lp);
		break;

	case WM_MOUSEMOVE:
		on_motion(a, hwnd, lp);
		return 0;

	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		on_wheel(a, msg, wp);
		return 0;

	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		on_button(a, hwnd, msg, wp, lp);
		return 0;

	/* lParam is the window taking the capture: this one again is no loss.
	 * on_button()'s own release, to none, finds nothing owed. */
	case WM_CAPTURECHANGED:
		if ((HWND)lp != hwnd)
			on_capture_lost(a);
		return 0;

	case WM_CLOSE:
		app_close_requested(a);
		return 0;

	case WM_DESTROY:
		if (!a->backend_state->closing)
			fnl_atomic_store_bool(&a->should_close, true);
		return 0;

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the backend seam -----------------------------------------------------
 *
 * FnlBackendVtbl says what an entry may assume: all but close() and wakeup()
 * run with a->backend_state and its window in place, and do not test for
 * them. */

static const wchar_t FNL_WNDCLASS[] = L"FenestalWindow";

static FnlResult win32_open(App *a)
{
	a->backend_state = calloc(1, sizeof *a->backend_state);
	if (!a->backend_state)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the Win32 backend state");
	struct BackendState *p = a->backend_state;

	fnl_mutex_init(&p->ack_lock);
	p->instance = GetModuleHandleW(NULL);

	/* Per-monitor v2: the client area is in physical pixels and WM_DPICHANGED
	 * arrives with a rectangle to adopt. Failure is not fatal -- the process
	 * may already have had awareness set by a manifest. */
	SetProcessDpiAwarenessContext(
		DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	WNDCLASSEXW wc = {
		.cbSize        = sizeof wc,
		.lpfnWndProc   = wnd_proc,
		.hInstance     = p->instance,
		.hCursor       = cursor_handle(p, FNL_CURSOR_DEFAULT),
		.lpszClassName = FNL_WNDCLASS,
	};
	/* Before the window, whose creation is already a size. Without one a
	 * size is simply not waited for. */
	p->size_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	p->wndclass = RegisterClassExW(&wc);
	if (!p->wndclass)
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"RegisterClassExW failed (%lu)", GetLastError());

	/* WS_OVERLAPPEDWINDOW, and not WS_POPUP with WS_THICKFRAME: a popup is
	 * not an ordinary top-level window and loses the taskbar button, the
	 * minimize and maximize animations, alt-tab and snap. The caption this
	 * style brings is removed by WM_NCCALCSIZE rather than by the style, so
	 * the frame's *drawing* goes and its behaviour stays. Fullscreen takes
	 * the style off for as long as it lasts. */
	const DWORD style = WS_OVERLAPPEDWINDOW;

	/* The size asked for is the *presentable area*, so the titlebar this
	 * library draws is added on top of it; deco_default_window_pt() has both
	 * the caller's size and the default.
	 *
	 * No AdjustWindowRectEx anywhere in this file. No style this window wears
	 * carries a frame outside the client area, so the window rectangle and
	 * the client rectangle are one and adjusting would open the window a
	 * frame too large. A style that regains a real frame has to bring the
	 * adjustment back with it.
	 *
	 * Created at 100%, where a point is a pixel: the density is not knowable
	 * until there is a window to ask. Corrected below, before it is shown. */
	int w_pt, h_pt;
	deco_default_window_pt(a, 120, &w_pt, &h_pt);

	/* UTF-8 in, UTF-16 out. A title that will not convert leaves the buffer
	 * empty, a window with no caption rather than a mojibake one; the bar
	 * this library draws shows the UTF-8 original regardless. */
	wchar_t wtitle[TITLE_MAX] = { 0 };
	MultiByteToWideChar(CP_UTF8, 0, a->title, -1, wtitle,
						(int)(sizeof wtitle / sizeof wtitle[0]));

	/* The App goes in as lpCreateParams, so WM_NCCREATE can install it before
	 * the frame is decided -- see the top of wnd_proc. */
	p->hwnd = CreateWindowExW(0, FNL_WNDCLASS, wtitle, style,
							  CW_USEDEFAULT, CW_USEDEFAULT, w_pt, h_pt,
							  NULL, NULL, p->instance, a);
	if (!p->hwnd)
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"CreateWindowExW failed (%lu)", GetLastError());

	p->dpi = GetDpiForWindow(p->hwnd);
	if (!p->dpi)
		p->dpi = DPI_BASE;
	raw_wheel_open(a);
	/* On Windows itself: wine's tick is a millisecond already, and its
	 * wheel wants the raw mouse on the window. */
	if (!win32_host(a)[0]) {
		if (FNL_FAULT("win32-no-raw-thread"))
			fnl_log(FNL_LOG_WARN, "no raw input thread (FNL_FAULT); input "
								  "latency is read in the tick's steps");
		else
			p->rawin = rawin_start(p->instance, p->hwnd);
	}

	/* The size again, at the density the window landed on: WM_DPICHANGED is
	 * sent when the DPI *changes* and never at creation. At the *resolved*
	 * scale, so the caller's override reaches the size the window opens at --
	 * which is why the test is the scale and not the DPI. Before ShowWindow,
	 * so it is one window at one size. */
	const int open_s120 = resolve_scale120(a, p->dpi);
	int w_px = scale_pt(w_pt, open_s120), h_px = scale_pt(h_pt, open_s120);

	/* And fitted to the room there is. Nothing on Windows declines a size:
	 * a window asked for larger than the screen opens larger than the
	 * screen, its far edges where no pointer reaches -- the default at 150%
	 * on a 1920x1080 panel, which is 1280x720 points before the taskbar.
	 * So the answer a window manager would have given is given here: the
	 * work area's size at most, the floor's at least, and moved back inside
	 * if where it was put now overhangs. Only the opening size; a size
	 * asked for afterwards is the caller's to have filtered. */
	RECT at = { 0, 0, 0, 0 };
	GetWindowRect(p->hwnd, &at);
	int x = at.left, y = at.top;
	MONITORINFO mi = { .cbSize = sizeof mi };
	if (GetMonitorInfoW(MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST),
						&mi)) {
		const RECT work = monitor_work(&mi);
		int min_w, min_h;
		deco_min_window(a, false, &min_w, &min_h);
		min_w = scale_pt(min_w, open_s120);
		min_h = scale_pt(min_h, open_s120);
		if (w_px > work.right - work.left)
			w_px = work.right - work.left;
		if (h_px > work.bottom - work.top)
			h_px = work.bottom - work.top;
		if (w_px < min_w)
			w_px = min_w;
		if (h_px < min_h)
			h_px = min_h;
		if (x + w_px > work.right)
			x = work.right - w_px;
		if (y + h_px > work.bottom)
			y = work.bottom - h_px;
		if (x < work.left)
			x = work.left;
		if (y < work.top)
			y = work.top;
	}
	if (x != at.left || y != at.top || w_px != at.right - at.left ||
		h_px != at.bottom - at.top)
		SetWindowPos(p->hwnd, NULL, x, y, w_px, h_px,
					 SWP_NOZORDER | SWP_NOACTIVATE);

	/* Under wine on Wayland the frames go into a child from the first: the
	 * window itself is never presented into. Wine 11 keeps the surface a
	 * destroyed swapchain leaves on a window, still showing its last frame,
	 * for that window's next one, until the window is destroyed; and its
	 * Wayland driver stacks each new subsurface directly above the window's
	 * own, so a child made at a move is drawn under that frame -- frozen
	 * there, and on a compositor that sends no frame callback to a covered
	 * surface (cage), every present after the move blocks. A child is
	 * destroyed at each move, which is what lets its surface go. Not on
	 * X11: wine draws a child there offscreen and copies it into the
	 * window at each present, a copy the window itself does not need
	 * until a move (and one that shows nothing where wine finds no
	 * libXcomposite). The driver is loaded by now, with the window. */
	if (GetModuleHandleW(L"winewayland.drv") && under_wine())
		present_renew(a, p->hwnd);

	ShowWindow(p->hwnd, SW_SHOWNORMAL);

	/* Prime the size: nothing has asked for a frame yet, and the render
	 * thread must not start against a zero. */
	publish_size(a);

	return FNL_OK;
}

static void win32_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p)
		return;

	win32_release(a);
	raw_unregister(p);
	if (p->rawin && a->frame_times) {
		uint32_t matched, missed;
		rawin_tally(p->rawin, &matched, &missed);
		fnl_log(FNL_LOG_INFO, "raw input: %u inputs matched to their "
							  "reports, %u messages left to the tick",
				matched, missed);
	}
	rawin_stop(p->rawin);
	p->rawin = NULL;
	if (p->hwnd) {
		p->closing = true;
		DestroyWindow(p->hwnd);
	}
	if (p->wndclass)
		UnregisterClassW(FNL_WNDCLASS, p->instance);
	/* The child went with the window. */
	if (p->present_class)
		UnregisterClassW(FNL_PRESENT_CLASS, p->instance);
	if (p->size_event)
		CloseHandle(p->size_event);
	fnl_mutex_destroy(&p->ack_lock);

	free(a->backend_state);
	a->backend_state = NULL;
}

/* One turn: block for a message, then drain whatever else is queued rather
 * than going back to sleep between each -- a burst of raw input would
 * otherwise cost one blocking call apiece. The wakeup is a posted message, so
 * there is nothing else to wait on.
 *
 * GetMessageW answers 0 for WM_QUIT and -1 for an error, and PeekMessageW
 * hands WM_QUIT over as a message; all three end the loop. */
static void win32_pump(App *a)
{
	MSG  msg;
	BOOL got = GetMessageW(&msg, NULL, 0, 0);

	while (got > 0 && msg.message != WM_QUIT) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
		if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
			return;
	}
	fnl_atomic_store_bool(&a->should_close, true);
}

static void win32_wakeup(App *a)
{
	if (a->backend_state && a->backend_state->hwnd)
		PostMessageW(a->backend_state->hwnd, FNL_WM_WAKEUP, 0, 0);
}

/* Minimized, hidden, or never shown. Not "covered", which no Win32 call
 * reports and FnlRendererVtbl::occluded exists for. Cheap enough to ask twice a
 * frame: both are style-bit reads. */
static bool win32_window_on_screen(const App *a)
{
	const HWND hwnd = a->backend_state->hwnd;
	return !IsIconic(hwnd) && IsWindowVisible(hwnd) != 0;
}

static void win32_update_scale(App *a)
{
	UINT dpi = GetDpiForWindow(a->backend_state->hwnd);
	if (!dpi)
		dpi = DPI_BASE;
	a->backend_state->dpi = dpi;

	(void)app_publish_scale(a, resolve_scale120(a, dpi));

	/* The output is reported in points, so a scale change restates it. */
	publish_output(a, true);
}

/* The window asked for, in points: app_request_size() has gated it and added
 * the titlebar. scale_pt(), which publish_size() converts straight back
 * from: the two must round the same way or a size picked from a list reads
 * back a point short. */
static void win32_request_size(App *a, int w_pt, int h_pt)
{
	const int s120 = want_s120(a);

	SetWindowPos(a->backend_state->hwnd, NULL, 0, 0,
				 scale_pt(w_pt, s120), scale_pt(h_pt, s120),
				 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void win32_set_maximized(App *a, bool on)
{
	ShowWindow(a->backend_state->hwnd, on ? SW_MAXIMIZE : SW_RESTORE);
}

/* Where workspace coordinates -- a WINDOWPLACEMENT's -- start on the
 * screen: the primary monitor's work area, which differs from the screen's
 * origin where the taskbar is on its top or left edge. rcWork itself, not
 * monitor_work(): this is Windows' own definition. */
static POINT workspace_origin(void)
{
	const POINT zero = { 0, 0 };
	MONITORINFO mi = { .cbSize = sizeof mi };
	if (!GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY),
						 &mi))
		return zero;
	const POINT o = { mi.rcWork.left - mi.rcMonitor.left,
					  mi.rcWork.top - mi.rcMonitor.top };
	return o;
}

static FnlRect rect_of(RECT r)
{
	const FnlRect f = { r.left, r.top, r.right - r.left, r.bottom - r.top };
	return f;
}

/* To the monitor of this name, keeping the window's size and state, where
 * app_place_on_output() puts it in that monitor's work area. Fullscreen is
 * a monitor's own rectangle, so the window leaves it, moves and takes it
 * again, all three at once here. A maximized window moves its restored
 * rectangle, by its placement, and is maximized again on the monitor that
 * rectangle is then on: a SW_RESTORE first would be a round trip through
 * the manager, whose answer under wine arrives after the move (see
 * win32_set_fullscreen()). The monitor change itself arrives as WM_MOVE,
 * which publishes it. */
static void win32_set_output(App *a, const char *name)
{
	struct BackendState *p = a->backend_state;
	MonitorList l;
	monitors_list(&l);
	HMONITOR to = NULL;
	for (int i = 0; i < l.n && !to; i++)
		if (strcmp(l.name[i], name) == 0)
			to = l.handle[i];
	HMONITOR from = MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST);
	if (!to || to == from)
		return;
	MONITORINFO fi = { .cbSize = sizeof fi }, ti = { .cbSize = sizeof ti };
	if (!GetMonitorInfoW(from, &fi) || !GetMonitorInfoW(to, &ti))
		return;
	const FnlRect fr = rect_of(monitor_work(&fi));
	const FnlRect tr = rect_of(monitor_work(&ti));

	const bool fs = a->is_fullscreen;
	if (fs)
		win32_set_fullscreen(a, false);

	int nx, ny;
	if (IsZoomed(p->hwnd)) {
		WINDOWPLACEMENT wp = { .length = sizeof wp };
		if (GetWindowPlacement(p->hwnd, &wp)) {
			const POINT o = workspace_origin();
			RECT *r = &wp.rcNormalPosition;
			const int w = r->right - r->left, h = r->bottom - r->top;
			app_place_on_output(r->left + o.x, r->top + o.y, w, h, &fr, &tr,
								&nx, &ny);
			r->left   = nx - o.x;
			r->top    = ny - o.y;
			r->right  = r->left + w;
			r->bottom = r->top + h;
			wp.showCmd = SW_SHOWMAXIMIZED;
			wp.flags   = 0;
			SetWindowPlacement(p->hwnd, &wp);
		}
	} else {
		RECT wr;
		if (GetWindowRect(p->hwnd, &wr)) {
			const FnlRect w = rect_of(wr);
			app_place_on_output(w.x, w.y, w.w, w.h, &fr, &tr, &nx, &ny);
			SetWindowPos(p->hwnd, NULL, nx, ny, 0, 0,
						 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	if (fs)
		win32_set_fullscreen(a, true);
	app_mark_urgent(a);
}

static void win32_minimize(App *a)
{
	ShowWindow(a->backend_state->hwnd, SW_MINIMIZE);
}

/* ---- the seam's move and resize -------------------------------------------
 *
 * For a titlebar the library never hit-tested. The hit test answers HTCLIENT
 * for a claimed rectangle by design, so a caller's control receives its
 * click; these hand Windows the press it would otherwise have delivered
 * through the non-client area. WM_NCLBUTTONDOWN with the right hit code
 * enters the same modal loop, so the drag is still the system's own.
 *
 * ReleaseCapture first, and it is not a precaution: the press that got here
 * set the mouse capture on this window, and the modal loop needs the mouse
 * itself. Without it the window follows nothing. */
static void win32_drag(App *a, WPARAM ht)
{
	ReleaseCapture();
	/* lParam is the screen position, which the loop uses as the drag's
	 * origin. GetMessagePos packs the pointer's, which is where the press it
	 * is standing in for landed. */
	const DWORD pos = (DWORD)GetMessagePos();
	SendMessageW(a->backend_state->hwnd, WM_NCLBUTTONDOWN, ht, (LPARAM)pos);
}

static void win32_begin_move(App *a)
{
	win32_drag(a, HTCAPTION);
}

static void win32_begin_resize(App *a, Edge edge)
{
	if (edge <= EDGE_NONE || edge >= EDGE_COUNT)
		return;
	win32_drag(a, (WPARAM)WIN32_EDGE_HT[edge]);
}

/* The floor moved. Nothing here holds it between resizes -- it is applied
 * after one, from WM_SIZE, and never through WM_GETMINMAXINFO -- so a floor
 * raised past the size the window already has is armed from here. */
static void win32_apply_size_bounds(App *a)
{
	arm_size_floor(a, a->backend_state->hwnd);
}

/* ---- the clipboard --------------------------------------------------------
 *
 * CF_UNICODETEXT both ways, converted at the edge. Synchronous: the clipboard
 * holds the bytes rather than a promise of them, so there is no other program
 * to wait for.
 *
 * Lines end in CRLF there, which Windows' edit controls want and its programs
 * put, and in LF to the caller (fenestal.h): each LF without its CR gains one
 * on the way out, and each CRLF loses the CR on the way in. A lone CR is
 * passed through either way, as the other backends pass the owner's text.
 *
 * The handle given to SetClipboardData is the system's from then on. It is
 * freed here only where that call was never reached or refused it. */

/* The clipboard is one lock for the whole session, and a clipboard manager
 * takes it the instant anything is copied, so it is asked a few times,
 * briefly. */
#define CLIP_OPEN_TRIES   5
#define CLIP_OPEN_WAIT_MS 10

static bool clipboard_open(HWND hwnd)
{
	for (int i = 0; i < CLIP_OPEN_TRIES; i++) {
		if (OpenClipboard(hwnd))
			return true;
		Sleep(CLIP_OPEN_WAIT_MS);
	}
	fnl_log(FNL_LOG_WARN, "the clipboard is held by another program");
	return false;
}

static void win32_clipboard_set(App *a)
{
	const int wn = MultiByteToWideChar(CP_UTF8, 0, a->clip_out, -1, NULL, 0);
	WCHAR *wide = wn > 0 ? malloc((size_t)wn * sizeof *wide) : NULL;
	if (!wide)
		return;
	MultiByteToWideChar(CP_UTF8, 0, a->clip_out, -1, wide, wn);
	/* Counted on what was converted, so the block is sized from the units
	 * the copy below actually reads. wn includes the terminator. */
	size_t bare = 0;
	for (int i = 0; i < wn; i++)
		if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
			bare++;
	if (!clipboard_open(a->backend_state->hwnd)) {
		free(wide);
		return;
	}

	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE,
							((size_t)wn + bare) * sizeof(WCHAR));
	WCHAR *w = h ? GlobalLock(h) : NULL;
	if (w) {
		size_t o = 0;
		for (int i = 0; i < wn; i++) {
			if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
				w[o++] = L'\r';
			w[o++] = wide[i];
		}
		GlobalUnlock(h);
		EmptyClipboard();
		if (SetClipboardData(CF_UNICODETEXT, h))
			h = NULL;
	}
	if (h)
		GlobalFree(h);
	CloseClipboard();
	free(wide);
}

static void win32_clipboard_request(App *a)
{
	char *out = NULL;
	int   n = 0;

	if (clipboard_open(a->backend_state->hwnd)) {
		HANDLE h = GetClipboardData(CF_UNICODETEXT);
		const WCHAR *w = h ? GlobalLock(h) : NULL;
		if (w) {
			/* Counted, not trusted to be terminated: the block is whatever
			 * another program put there, and its size is the only bound on
			 * it that is the system's. */
			const SIZE_T cap = GlobalSize(h) / sizeof(WCHAR);
			SIZE_T wn = 0;
			while (wn < cap && wn < (SIZE_T)INT_MAX && w[wn])
				wn++;
			if (wn > 0)
				n = WideCharToMultiByte(CP_UTF8, 0, w, (int)wn, NULL, 0, NULL,
										NULL);
			out = n > 0 ? malloc((size_t)n + 1) : NULL;
			if (out)
				WideCharToMultiByte(CP_UTF8, 0, w, (int)wn, out, n, NULL,
									NULL);
			GlobalUnlock(h);
			/* CRLF to LF, in place: it only ever shortens. */
			int k = 0;
			for (int i = 0; out && i < n; i++)
				if (out[i] != '\r' || i + 1 >= n || out[i + 1] != '\n')
					out[k++] = out[i];
			n = k;
		}
		CloseClipboard();
	}
	/* Delivered even when there was nothing, because a request always gets
	 * its event. The buffer has the byte the core terminates it in. */
	app_clipboard_take(a, out, out ? (size_t)n : 0);
}

/* [main] From the drain, on the thread that owns the window, so the
 * WM_SETTEXT it sends is answered in place. Converted as at creation: a
 * title that will not convert is an empty caption. A UTF-16 unit per byte
 * at most, so TITLE_MAX units hold any title with its terminator -- the
 * whole buffer is the count, which includes the terminator. */
static void win32_set_title(App *a, const char *utf8)
{
	wchar_t w[TITLE_MAX] = { 0 };
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w,
						(int)(sizeof w / sizeof w[0]));
	SetWindowTextW(a->backend_state->hwnd, w);
}

/* fnl_set_cursor() moved the shape: put it up now if the pointer is already
 * somewhere it applies, rather than at the next WM_SETCURSOR, which is the
 * next time the pointer moves.
 *
 * Only over this window's client area, or a caller's bar (WM_SETCURSOR's
 * rule). SetCursor is global, and a window that called it whenever its
 * caller changed its mind would be repainting the pointer over somebody
 * else's window. */
static void win32_cursor_changed(App *a)
{
	const HWND hwnd = a->backend_state->hwnd;
	POINT pt;
	if (!GetCursorPos(&pt) || WindowFromPoint(pt) != hwnd)
		return;
	const LRESULT ht = SendMessageW(hwnd, WM_NCHITTEST, 0,
									MAKELPARAM(pt.x, pt.y));
	if (ht != HTCLIENT && !(ht == HTCAPTION && a->deco_custom))
		return;
	cursor_apply(a, hwnd);
}

/* Wine, and which of its display drivers this process has loaded.
 *
 * Asked of the process and not of the registry: the driver is chosen per
 * launch -- no DISPLAY in the environment is enough for the Wayland one --
 * and the key that would pin it is usually absent; a display driver is a
 * module like any other once the first window exists. */
static const char *win32_host(const App *a)
{
	(void)a;
	if (!under_wine())
		return "";
	if (GetModuleHandleW(L"winewayland.drv"))
		return "wine on Wayland";
	if (GetModuleHandleW(L"winex11.drv"))
		return "wine on X11";
	return "wine";
}

/* Under wine whichever display driver: its Direct3D 12 is vkd3d-proton,
 * on the Vulkan driver its Vulkan reaches. */
static bool win32_shared_driver(const App *a)
{
	(void)a;
	return under_wine();
}

const FnlBackendVtbl fnl_backend_win32 = {
	.name               = "Win32",
	.id                 = BACKEND_WIN32,
	.key                = "win32",
	.absent             = backend_always_present,
	.host               = win32_host,
	.shared_driver      = win32_shared_driver,
	.gpu_drivers        = backend_noop_gpu_drivers,
	.gpu_driver_set     = backend_noop_gpu_driver_set,
	.gpu_driver_held    = backend_noop_gpu_driver_held,
	/* No ring: the window rectangle is the window, so the resize edges are an
	 * inner margin that WM_NCHITTEST answers for. */
	.grab_ring          = 0,
	/* No invitation to draw: the present is the throttle. See
	 * backend_frame_wait_shown(), and late_latch_slack() in
	 * src/core/frame.c for the signal that replaces it. */
	.invites            = false,
	/* size_await() holds a size until its frame is shown, and the flush
	 * that releases it runs after the present returns -- which under wine,
	 * and on any driver that presents from a thread of its own, is before
	 * the frame is on the screen. */
	.ack_when_shown     = true,
	.ack_release        = win32_ack_release,
	.open               = win32_open,
	.close              = win32_close,
	.pump               = win32_pump,
	.wakeup             = win32_wakeup,
	.check_error        = backend_noop_check_error,
	.flush              = win32_flush,
	.present_pixels     = backend_noop_present_pixels,
	.set_geometry       = backend_noop_set_geometry,
	.ack_configure      = win32_ack_configure,
	.frame_request      = backend_noop,
	.frame_cancel       = backend_noop,
	.present_window_renew = win32_present_window_renew,
	.present_window_hold  = win32_present_window_hold,
	.frame_wait         = backend_frame_wait_shown,
	.window_on_screen   = win32_window_on_screen,
	.apply_scale        = backend_noop_apply_scale,
	.request_size       = win32_request_size,
	.nudge_size         = backend_noop,
	.set_fullscreen     = win32_set_fullscreen,
	.set_maximized      = win32_set_maximized,
	.minimize           = win32_minimize,
	.modes              = backend_noop_modes,
	.outputs            = backend_published_outputs,
	.set_mode           = backend_noop_set_mode,
	.set_output         = win32_set_output,
	.begin_move         = win32_begin_move,
	.begin_resize       = win32_begin_resize,
	.apply_size_bounds  = win32_apply_size_bounds,
	.buffer_granularity = backend_noop_buffer_granularity,
	.update_scale       = win32_update_scale,
	.capture_pointer    = win32_capture,
	.release_pointer    = win32_release,
	.cursor_changed     = win32_cursor_changed,
	.text_input_changed = win32_text_input_changed,
	.clipboard_set      = win32_clipboard_set,
	.clipboard_request  = win32_clipboard_request,
	.set_title          = win32_set_title,
};
