/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The KMS backend's state, behind App::backend_state: no window server, so
 * this is the display's DRM node, the one output the program has whole, and
 * the evdev devices it reads itself. For kms.c, kms_driver.c and the two
 * bridges onto KMS, Vulkan's and OpenGL's, alone.
 */

#ifndef FNL_KMS_STATE_H
#define FNL_KMS_STATE_H

#include "backend/keymap.h"

#include <drm/drm_mode.h>

/* Modes an output is offered at: a monitor lists twenty or thirty, many of
 * them one size at several rates. Past this many the smallest go. 32 is
 * also the width of an output's refused set. */
#define KMS_MAX_MODES 32
/* Slots for the card's outputs: one each connected at open, and one more
 * for each monitor plugged in after it that is not the one a slot already
 * held -- a slot is never written again once published, so both threads
 * may read it without a lock (outputs_rescan()). The width of the refused
 * and gone sets. */
#define KMS_MAX_OUTPUTS 32
/* Pairs held aside while a dark output's plane shows one: one an output
 * the window has left, so as many as outputs at most, and four is more
 * than a card has been seen to light at once. */
#define KMS_PARKED 4
/* How long an output may be gone before it counts as unplugged, since a
 * monitor leaves the bus for a moment as it falls asleep and as it wakes,
 * which the kernel reports as an unplug and a plug. One left dark: black's
 * Samsung on HDMI-A-1, from 18.6 s after its last frame, is gone 2.4 s in
 * every 16.7 s for as long as it stays dark; twice that, so a slower
 * monitor's gap is covered too, and a cable pulled from a dark output
 * leaves the list that late, which is all it costs. The one the window is
 * on is lit and does not sleep, but wakes: black's iiyama on DP-3, lit
 * again after a minute dark, is gone 0.8 s from 3.7 s after and then
 * connected with no modes for 0.26 s more; over twice that, and a window
 * whose monitor is unplugged waits that long to move. */
#define KMS_AWAY_S     5.0
#define KMS_AWAY_LIT_S 2.5

_Static_assert(KMS_MAX_MODES <= OUTPUT_MODES_MAX,
			   "the seam's mode lists are read through a buffer that size");

/* One connected output of the card, read at open: the connector, the CRTC
 * the copy path sets its modes on -- the one that lit the console, else the
 * first an encoder of it can use -- and its modes, largest first and
 * fastest first within a size (modes_read()). Only a guess for an output
 * not lit at open, where Vulkan's driver chooses for itself, so what drives
 * an output now is asked of the kernel (connector_live_crtc()). */
typedef struct {
	uint32_t     connector_id, crtc_id;
	char         name[32];          /* the kernel's name, "HDMI-A-1" */
	struct drm_mode_modeinfo modes[KMS_MAX_MODES];
	int          mode_count, mode_pref;
} KmsOutput;

/* A pair of dumb buffers of one mode's size: the copy path's scanout, the
 * OpenGL bridge's, which the GPU draws into (kms_scan_export()), and the
 * backend's own frame on an output a driver let go (kms_scanout_take()).
 * Shown one while the other is written. `gen` names each buffer apart
 * from every other made (BackendState::scan_gen). */
typedef struct {
	uint32_t      handle[2], fb[2], gen[2];
	unsigned char *map[2];
	size_t        size;
	uint32_t      pitch;
	int           w, h;
} KmsScan;

/* A CRTC as the open found it -- what it showed, the console as a rule,
 * and at which mode -- set back at close before this file's framebuffers
 * go (crtcs_restore()). */
typedef struct {
	uint32_t connector_id, crtc_id, fb_id;
	bool     mode_valid;
	struct drm_mode_modeinfo mode;
} KmsCrtcSaved;

/* A selection: an output and one of its modes, as one word, so the two
 * threads never read the one without the other. */
#define KMS_SEL(o, m)   ((int)((unsigned)(o) << 8 | (unsigned)(m)))
#define KMS_SEL_OUT(s)  ((int)((unsigned)(s) >> 8))
#define KMS_SEL_MODE(s) ((int)((unsigned)(s) & 0xffu))

/* Devices read at once. A laptop has five or six -- the keyboard, the pad,
 * the lid and power buttons, which are dropped at open -- and the harness
 * adds two. */
#define KMS_MAX_INPUTS 32

/* What a device is to this backend; one device can be more than one. */
enum {
	KMS_KEYBOARD = 1u << 0,
	KMS_MOUSE    = 1u << 1,     /* relative motion */
	KMS_PAD      = 1u << 2,     /* a touchpad: absolute, read as relative */
	KMS_TABLET   = 1u << 3,     /* absolute, mapped onto the output */
};

typedef struct {
	int      fd;
	unsigned kind;
	char     node[16];          /* "event7", which is what inotify names */
	/* The axes of an absolute device, from EVIOCGABS. */
	int      x_min, x_max, y_min, y_max;
	int      x_res, y_res;      /* units per millimetre, 0 for unknown */
	/* Between two SYN_REPORTs: the motion that report carries, and for an
	 * absolute device the axes it moved. */
	double   dx, dy;
	int      ax, ay;
	bool     ax_new, ay_new;
	/* A pad's finger, or a captured tablet's report: where it was at the
	 * last, in the device's units, while there is one. */
	bool     touching, have_last;
	int      last_x, last_y;
	/* SYN_DROPPED: everything to the next SYN_REPORT is a partial report. */
	bool     dropped;
} KmsInput;

/* The cursor's shapes; see kms.c. */
typedef enum {
	KMS_GLYPH_ARROW, KMS_GLYPH_IBEAM, KMS_GLYPH_CROSS, KMS_GLYPH_COUNT,
	KMS_GLYPH_NONE = KMS_GLYPH_COUNT,
	KMS_GLYPH_UNSET
} KmsGlyph;

/* [main], the pump's thread, unless tagged otherwise. */
struct BackendState {
	/* The card and its outputs, read once at open. [any] after that: the
	 * bridge reads them on the render thread while building a surface, and
	 * nothing writes them again. card_fd is master, and what the bridge
	 * lends Vulkan is a dup of it -- one open file, so one master. */
	int          card_fd, card_index;
	KmsOutput    outputs[KMS_MAX_OUTPUTS];
	/* How many slots are published: [any], written by the pump alone and
	 * only after the slot it counts is filled. outputs_gone is the slots
	 * whose monitor is unplugged, a bit each, likewise. */
	FnlAtomicU32 output_count;
	FnlAtomicU32 outputs_gone;
	/* Every CRTC the open found lit, read once there and given back at
	 * close. */
	KmsCrtcSaved saved[KMS_MAX_OUTPUTS];
	int          saved_count;
	/* The kernel's uevents, for a monitor plugged or unplugged; -1 where the
	 * socket would not open, and outputs are then those of the open. */
	int          uevent_fd;
	bool         slots_warned;
	/* Outputs whose monitor left within KMS_AWAY_S, or KMS_AWAY_LIT_S for
	 * the one the window was on, a bit each: still listed and not gone,
	 * each with the time on now_seconds()'s clock at which it is
	 * (outputs_rescan()). An output asked for while away is `away_owed`,
	 * -1 for none, and the window moves there when it is back. */
	uint32_t     outputs_away;
	double       away_until[KMS_MAX_OUTPUTS];
	int          away_owed;
	bool         away_said;
	/* Bumped by the pump when the output the window is on comes back, and by
	 * the render thread at a move to another GPU: the copy path's next frame
	 * sets the mode afresh (scan_resets_seen). A load and a store, not an
	 * atomic add: two bumps that meet still leave it moved. */
	FnlAtomicU32 scan_resets;

	/* The selection the window is, and its mode's figures: what is
	 * published, what the pointer is bounded by. */
	int          out_cur, mode_cur;
	int          mode_w, mode_h;    /* pixels */
	unsigned int refresh_mhz;

	/* Between the two threads, a selection handed over and its answer. The
	 * pump writes sel_want, what the render thread is to put on screen; the
	 * render thread writes the rest -- the last selection that showed, the
	 * modes that would not, a bit each, and the outputs that would not --
	 * and wakes the pump, which then goes back to the one that did
	 * (sel_refused_seen()). One writer each. */
	FnlAtomicU32 sel_want;
	FnlAtomicU32 sel_good;
	FnlAtomicU32 modes_refused[KMS_MAX_OUTPUTS];
	FnlAtomicU32 outputs_refused;
	/* [render] The output whose display the bridge holds (-1 for none yet),
	 * the selection the display surface was last made for (-1), and the
	 * presents there had been then: no present since, on two surfaces
	 * running, is how a mode that would not show is told when the surface
	 * is made again. */
	int          disp_out;
	int          surf_sel;
	uint64_t     surf_presents;
	int          surf_unshown;      /* surfaces running for it, none shown */

	int          wake_fd, epoll_fd, inotify_fd;

	KmsInput     inputs[KMS_MAX_INPUTS];
	int          input_count;

	/* The layout, and the keys down by evdev code: the modifiers are worked
	 * out from these, since nobody sends them. KEY_MAX is 0x2ff. */
	Keymap       keymap;
	KeyCompose   compose;
	uint8_t      down[0x300 / 8];
	bool         caps_on, num_on, leds_known;
	/* Whether the key right of the space bar is AltGr in this layout, or Alt;
	 * read from the keymap. */
	bool         ralt_is_level3;
	/* The group typed in, of the groups the layout fills, kept here as the
	 * locks are: a layout of "de,us" is two, switched by its group keys
	 * (keymap_group_key()). And each group's layout as the log names it. */
	unsigned     group, groups;
	char         group_name[KEYMAP_GROUPS][48];

	/* The pointer, in pixels of the output. */
	double       ptr_x, ptr_y;
	bool         captured;

	/* The cursor plane: one dumb buffer per glyph, made when first shown. */
	uint32_t     cursor_w, cursor_h;
	uint32_t     glyph_handle[KMS_GLYPH_COUNT];
	KmsGlyph     glyph_shown;
	/* The CRTC driving the output now, as the kernel says, 0 until it says
	 * one; the CRTC the cursor was last set on; and how many more pump turns
	 * look for the first, after a switch (cursor_recheck()). */
	uint32_t     cursor_crtc, cursor_on;
	int          cursor_recheck;
	int          cursor_at_x, cursor_at_y;
	bool         cursor_warned;

	/* [render] The copy path's two scanout buffers
	 * (RendererState::present_copy): made at the first frame, shown one
	 * while the other is written, and swapped by a page flip. `scan_front`
	 * is -1 while neither is on the plane, `scan_set_owed` says the next
	 * frame sets the mode whichever is, and `scan_sel` is the selection they
	 * are the size of, -1 before the first. `scan_old` is the pair of the
	 * size before, held until the new one is on the plane: no framebuffer is
	 * removed while a plane shows it (scan_free()). */
	int          scan_sel;
	uint32_t     scan_resets_seen;

	KmsScan      scan, scan_old;
	uint32_t     scan_gen;
	/* Pairs let go while a CRTC still showed one: an output gone dark keeps
	 * the backend's frame on its plane (kms_output_off(), DPMS). Freed once
	 * nothing shows them, and at close (scan_release()). */
	KmsScan      scan_parked[KMS_PARKED];
	int          scan_front;
	bool         scan_set_owed, flip_pending, scan_failed;
	bool         flip_async;    /* the pending flip tears: no vblank */
	/* The driver refused an asynchronous flip (EINVAL): vblank flips until
	 * the next mode set, which asks again. */
	bool         flip_async_off;
};

/* The refresh in millihertz, from the timings rather than the rounded
 * vrefresh: 60.01 Hz and 59.94 Hz are both "60" there. */
static inline unsigned int kms_refresh_mhz(const struct drm_mode_modeinfo *m)
{
	const uint64_t total = (uint64_t)m->htotal * m->vtotal;
	if (!total) {
		const uint64_t v = (uint64_t)m->vrefresh * 1000u;
		return v > 0xffffffffu ? 0u : (unsigned int)v;
	}
	uint64_t mhz = (uint64_t)m->clock * 1000000u / total;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		mhz *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		mhz /= 2;
	if (m->vscan > 1)
		mhz /= m->vscan;
	/* A mode past any rate is no rate: 0, unknown, as X11's read leaves
	 * one it cannot use, rather than a wrapped figure the pacing believes. */
	return mhz > 0xffffffffu ? 0u : (unsigned int)mhz;
}

/* The output and the mode a selection names. */
static inline const KmsOutput *kms_out(const struct BackendState *p,
									   int sel)
{
	return &p->outputs[KMS_SEL_OUT(sel)];
}

static inline const struct drm_mode_modeinfo *
kms_sel_mode(const struct BackendState *p, int sel)
{
	return &p->outputs[KMS_SEL_OUT(sel)].modes[KMS_SEL_MODE(sel)];
}

/* A selection's size in pixels and refresh in millihertz, its connector and
 * its output's name: what the bridge needs of one, so that it names none of
 * DRM's types itself. */
static inline void kms_mode_size(const struct BackendState *p, int sel,
								 int *w, int *h, unsigned int *mhz)
{
	const struct drm_mode_modeinfo *m = kms_sel_mode(p, sel);
	*w = m->hdisplay;
	*h = m->vdisplay;
	*mhz = kms_refresh_mhz(m);
}

static inline uint32_t kms_connector(const struct BackendState *p, int sel)
{
	return kms_out(p, sel)->connector_id;
}

static inline const char *kms_name(const struct BackendState *p, int sel)
{
	return kms_out(p, sel)->name;
}

/* Whether a selection would not show: its output, or its mode on it. */
static inline bool kms_sel_is_refused(const struct BackendState *p, int sel)
{
	const int o = KMS_SEL_OUT(sel), m = KMS_SEL_MODE(sel);
	return ((fnl_atomic_load_u32(&p->outputs_refused) >> o) & 1u) ||
		   ((fnl_atomic_load_u32(&p->modes_refused[o]) >> m) & 1u);
}

/* [render] What to put on screen: the selection handed over, unless it
 * would not show, and then the last that did, until the pump hands that
 * over. */
static inline int kms_sel_to_show(const struct BackendState *p)
{
	const int want = (int)fnl_atomic_load_u32(&p->sel_want);
	return kms_sel_is_refused(p, want)
			   ? (int)fnl_atomic_load_u32(&p->sel_good) : want;
}

/* [render] In kms.c. A selection would not show -- its mode, or with
 * `whole` its output -- or did; and an output left behind, whose CRTC goes
 * dark unless the selection now shown has the same one. */
void kms_sel_refused(App *a, int sel, bool whole);
void kms_sel_shown(App *a, int sel);
void kms_output_off(App *a, int out, int shown, bool unbind);
void kms_scanout_take(App *a);

/* [render] In kms.c. The scanout pair for a bridge that draws into it on
 * the GPU (egl_kms.c): the buffer the frame goes into, -1 where this frame
 * shows nothing (dropped, or refused, as the copy path's would be); one of
 * the pair as a dma-buf, -1 where the kernel will not export it, and the
 * key naming it without the export; and the buffer put on the screen,
 * waited out under vsync. The format is a DRM fourcc and the layout
 * linear, so the bridge names nothing of DRM's. */
typedef struct {
	int      w, h;
	uint32_t pitch, fourcc, key;
} KmsScanBuf;
int kms_scan_begin(App *a);
int kms_scan_export(App *a, int i, KmsScanBuf *out);
uint32_t kms_scan_key(App *a, int i);
/* And what buffer `i` holds at its first and its last pixel, read through
 * the backend's own mapping: whether a GPU's writes reach it. */
bool kms_scan_peek(App *a, int i, uint32_t *first, uint32_t *last);
void kms_scan_show(App *a, int back);

#endif /* FNL_KMS_STATE_H */
