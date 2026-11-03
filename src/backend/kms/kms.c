/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The KMS backend: one implementation of core/backend.h, for a
 * machine with no window server at all. The program takes one output of the
 * display whole -- DESIGN *KMS: no window server* -- and reads the kernel's
 * input devices itself.
 *
 * The window is the output's mode. Nothing can move it or minimize it, so it
 * is maximized from the first frame and stays so; its size is one of the
 * output's modes, chosen by set_mode, and set_output moves it to another
 * connected output of the card. Fullscreen is the one state left, and takes
 * the titlebar away. This file holds the DRM node, as master, and the
 * display is shown one of three ways: by Vulkan, lent a duplicate of the
 * node through VK_EXT_acquire_drm_display (surface_kms.c); by the copy
 * path, a frame drawn headless and its pixels put in a pair of dumb buffers
 * here (kms_present_pixels()), where no Vulkan device drives the output;
 * or by the OpenGL bridge, which draws into that pair on the GPU and has
 * this file flip it (egl_kms.c; kms_scan_begin(), kms_scan_export(),
 * kms_scan_show()).
 *
 * Raw ioctls against the kernel's own headers, for the DRM node and for
 * evdev both: libdrm, libinput and libudev would each be an entry on a
 * consumer's link line. The layout is the one library-shaped thing this
 * backend needs, and it comes from xkeyboard-config's own files: the
 * console's names resolved through their rules and their symbols read by
 * keymap_rules.c, with nothing run and nothing linked.
 *
 * The keyboards are grabbed (EVIOCGRAB). A key that also reached the
 * console's tty would be typed into whatever is logged in there, and a grab
 * is the one way to keep it out that needs no tty of our own.
 */

#include "core/app.h"

#include "backend/kms/kms_state.h"
#include "backend/evdev_keys.h"
#include "backend/kms/kms_driver.h"
#include "backend/loop_linux.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/input.h>
#include <linux/netlink.h>

/* ---- the DRM node -------------------------------------------------------- */

/* The kernel's names for a connector type, as /sys/class/drm spells them:
 * what FNL_KMS_OUTPUT is matched against and what fnl_output_name() says. */
static const char *const CONNECTOR_NAMES[] = {
	[DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
	[DRM_MODE_CONNECTOR_VGA]         = "VGA",
	[DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
	[DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
	[DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
	[DRM_MODE_CONNECTOR_Composite]   = "Composite",
	[DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
	[DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
	[DRM_MODE_CONNECTOR_Component]   = "Component",
	[DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
	[DRM_MODE_CONNECTOR_DisplayPort] = "DP",
	[DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
	[DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
	[DRM_MODE_CONNECTOR_TV]          = "TV",
	[DRM_MODE_CONNECTOR_eDP]         = "eDP",
	[DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
	[DRM_MODE_CONNECTOR_DSI]         = "DSI",
	[DRM_MODE_CONNECTOR_DPI]         = "DPI",
};
#define CONNECTOR_NAME_COUNT \
	(sizeof CONNECTOR_NAMES / sizeof CONNECTOR_NAMES[0])

/* Cards looked at, card0 upwards. */
#define KMS_MAX_CARDS 8
/* Connectors on one card; a card with more has the rest ignored. */
#define KMS_MAX_CONNECTORS 32

static int drm_ioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static bool connector_internal(uint32_t type)
{
	return type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP ||
		   type == DRM_MODE_CONNECTOR_DSI;
}

static void connector_name(uint32_t type, uint32_t type_id, char *out,
						   size_t size)
{
	const char *t = type < CONNECTOR_NAME_COUNT && CONNECTOR_NAMES[type]
						? CONNECTOR_NAMES[type] : "Unknown";
	snprintf(out, size, "%s-%u", t, type_id);
}

/* One connector, with its modes. `count_modes` zero makes the kernel probe
 * the output, which only a master may do; a non-master is given what was
 * last probed. Asked twice, the second time with room, and again if a
 * hotplug between the two changed the count. */
typedef struct {
	struct drm_mode_get_connector info;
	struct drm_mode_modeinfo     *modes;
} Connector;

static bool connector_read(int fd, uint32_t id, Connector *c)
{
	c->modes = NULL;
	for (int tries = 0; tries < 3; tries++) {
		memset(&c->info, 0, sizeof c->info);
		c->info.connector_id = id;
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c->info) < 0)
			return false;
		const uint32_t n = c->info.count_modes;
		free(c->modes);
		c->modes = n ? calloc(n, sizeof *c->modes) : NULL;
		if (n && !c->modes)
			return false;
		struct drm_mode_get_connector again = {
			.connector_id = id,
			.count_modes  = n,
			.modes_ptr    = (uintptr_t)c->modes,
		};
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &again) < 0)
			break;
		if (again.count_modes == n) {
			c->info = again;
			return true;
		}
	}
	free(c->modes);
	c->modes = NULL;
	return false;
}

/* Interlaced and doubled modes are left out of the list: a display surface
 * of one is a field or a half, and no window wants either. The preferred
 * mode is kept whatever it is where its size has no other. */
#define MODE_SKIPPED (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)

static unsigned int refresh_off(const struct drm_mode_modeinfo *m,
								unsigned int want)
{
	const unsigned int r = kms_refresh_mhz(m);
	return r > want ? r - want : want - r;
}

/* The order a list shows modes in: larger first, and of one size the faster
 * first. */
static bool mode_before(const struct drm_mode_modeinfo *x,
						const struct drm_mode_modeinfo *y)
{
	const uint32_t ax = (uint32_t)x->hdisplay * x->vdisplay;
	const uint32_t ay = (uint32_t)y->hdisplay * y->vdisplay;
	if (ax != ay)
		return ax > ay;
	if (x->hdisplay != y->hdisplay)
		return x->hdisplay > y->hdisplay;
	return kms_refresh_mhz(x) > kms_refresh_mhz(y);
}

/* Two modes a list would name alike: one size, one refresh to the
 * hundredth of a hertz, which is what it prints. */
static bool mode_same(const struct drm_mode_modeinfo *x,
					  const struct drm_mode_modeinfo *y)
{
	return x->hdisplay == y->hdisplay && x->vdisplay == y->vdisplay &&
		   (kms_refresh_mhz(x) + 5) / 10 == (kms_refresh_mhz(y) + 5) / 10;
}

/* The connector's modes as a list offers them: every one, larger first and
 * faster first within a size, but two it would name alike once and
 * interlaced and doubled ones not at all. The preferred mode is the one
 * the output opens at; an interlaced one -- a television's -- gives way to
 * a progressive one of its size, the refresh nearest, where there is one,
 * and is kept where there is not, since it is the one there is. The kernel
 * lists the preferred mode first for most outputs, but says so only by
 * flag. Past KMS_MAX_MODES the smallest go, never the preferred. */
static bool modes_read(KmsOutput *o, const struct drm_mode_modeinfo *in,
					   uint32_t n)
{
	const struct drm_mode_modeinfo *pref = &in[0];
	for (uint32_t i = 0; i < n; i++)
		if (in[i].type & DRM_MODE_TYPE_PREFERRED) {
			pref = &in[i];
			break;
		}
	if (pref->flags & MODE_SKIPPED) {
		const struct drm_mode_modeinfo *was = pref;
		const unsigned int was_mhz = kms_refresh_mhz(was);
		for (uint32_t i = 0; i < n; i++) {
			const struct drm_mode_modeinfo *m = &in[i];
			if ((m->flags & MODE_SKIPPED) || m->hdisplay != was->hdisplay ||
				m->vdisplay != was->vdisplay)
				continue;
			if (pref == was ||
				refresh_off(m, was_mhz) < refresh_off(pref, was_mhz))
				pref = m;
		}
	}

	struct drm_mode_modeinfo *list = calloc(n, sizeof *list);
	if (!list)
		return false;
	int count = 0;
	for (uint32_t i = 0; i < n; i++) {
		const struct drm_mode_modeinfo *m = &in[i];
		if (m != pref && (m->flags & MODE_SKIPPED))
			continue;
		if (!m->hdisplay || !m->vdisplay)
			continue;
		int at = -1;
		for (int j = 0; j < count; j++)
			if (mode_same(&list[j], m)) {
				at = j;
				break;
			}
		/* Of two named alike the first stays, unless the second is the
		 * preferred one. */
		if (at < 0)
			list[count++] = *m;
		else if (m == pref)
			list[at] = *m;
	}
	/* In the list's order, by insertion: a few dozen at most. */
	for (int i = 1; i < count; i++) {
		const struct drm_mode_modeinfo m = list[i];
		int j = i;
		for (; j > 0 && mode_before(&m, &list[j - 1]); j--)
			list[j] = list[j - 1];
		list[j] = m;
	}
	int pref_at = -1;
	for (int i = 0; i < count && pref_at < 0; i++)
		if (memcmp(&list[i], pref, sizeof *pref) == 0)
			pref_at = i;
	/* The first KMS_MAX_MODES, and the preferred one in place of the last of
	 * them where it would not be among them. */
	o->mode_count = count < KMS_MAX_MODES ? count : KMS_MAX_MODES;
	for (int i = 0; i < o->mode_count; i++)
		o->modes[i] = list[i];
	o->mode_pref = pref_at < 0 ? 0 : pref_at;
	if (pref_at >= o->mode_count) {
		o->modes[o->mode_count - 1] = list[pref_at];
		o->mode_pref = o->mode_count - 1;
	}
	free(list);
	return o->mode_count > 0;
}

/* The pump's own figures for a selection. */
static void mode_adopt(struct BackendState *p, int sel)
{
	const struct drm_mode_modeinfo *m = kms_sel_mode(p, sel);
	p->out_cur = KMS_SEL_OUT(sel);
	p->mode_cur = KMS_SEL_MODE(sel);
	p->mode_w = m->hdisplay;
	p->mode_h = m->vdisplay;
	p->refresh_mhz = kms_refresh_mhz(m);
}

/* The connectors of one card, at most KMS_MAX_CONNECTORS. */
static int card_connectors(int fd, uint32_t ids[KMS_MAX_CONNECTORS])
{
	struct drm_mode_card_res r;
	memset(&r, 0, sizeof r);
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0)
		return -1;
	uint32_t n = r.count_connectors;
	if (n > KMS_MAX_CONNECTORS)
		n = KMS_MAX_CONNECTORS;
	struct drm_mode_card_res again;
	memset(&again, 0, sizeof again);
	again.count_connectors = n;
	again.connector_id_ptr = (uintptr_t)ids;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &again) < 0)
		return -1;
	return (int)(again.count_connectors < n ? again.count_connectors : n);
}

#define KMS_MAX_ENCODERS 8
#define KMS_MAX_CRTCS    16

/* The CRTC driving a connector now, or 0: what lit it, the console or a
 * mode set since, Vulkan's included -- which picks its own CRTC for a
 * display, so the one read at open is a guess for any output not lit then. */
static uint32_t connector_live_crtc(int fd, uint32_t connector_id)
{
	struct drm_mode_modeinfo one;
	struct drm_mode_get_connector c = {
		.connector_id = connector_id,
		.count_modes  = 1,
		.modes_ptr    = (uintptr_t)&one,
	};
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || !c.encoder_id)
		return 0;
	struct drm_mode_get_encoder e = { .encoder_id = c.encoder_id };
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0)
		return 0;
	return e.crtc_id;
}

/* The CRTC driving a connector: whatever lit the console, else the first an
 * encoder of the connector can use. The same choice a driver makes, which
 * keeps the CRTC that already drives a connector, so the cursor is on the
 * CRTC Vulkan flips as well as on the one the copy path sets. A connector
 * read with room for one mode is not probed again. */
static uint32_t connector_crtc(int fd, uint32_t connector_id)
{
	const uint32_t live = connector_live_crtc(fd, connector_id);
	if (live)
		return live;
	struct drm_mode_modeinfo one;
	uint32_t enc[KMS_MAX_ENCODERS];
	struct drm_mode_get_connector c = {
		.connector_id   = connector_id,
		.count_modes    = 1,
		.modes_ptr      = (uintptr_t)&one,
		.count_encoders = KMS_MAX_ENCODERS,
		.encoders_ptr   = (uintptr_t)enc,
	};
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
		return 0;
	uint32_t crtcs[KMS_MAX_CRTCS];
	struct drm_mode_card_res r;
	memset(&r, 0, sizeof r);
	r.count_crtcs = KMS_MAX_CRTCS;
	r.crtc_id_ptr = (uintptr_t)crtcs;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0)
		return 0;
	const uint32_t ncrtc = r.count_crtcs < KMS_MAX_CRTCS ? r.count_crtcs
														 : KMS_MAX_CRTCS;
	const uint32_t nenc = c.count_encoders < KMS_MAX_ENCODERS
							  ? c.count_encoders : KMS_MAX_ENCODERS;
	for (uint32_t i = 0; i < nenc; i++) {
		struct drm_mode_get_encoder e = { .encoder_id = enc[i] };
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0)
			continue;
		for (uint32_t j = 0; j < ncrtc; j++)
			if (e.possible_crtcs & (1u << j))
				return crtcs[j];
	}
	return 0;
}

/* A connector as a slot: its name, its modes and its CRTC, probed afresh.
 * False when it is not connected or offers no mode it can be shown at;
 * `*type` is the connector's type for the open's ranking, and `*read`
 * whether the kernel answered at all: a failed read is not an unplug, but a
 * connector it no longer has (ENOENT, gone between the list and the read)
 * is. */
static bool output_read(int fd, uint32_t id, KmsOutput *o, uint32_t *type,
						bool *read)
{
	Connector c;
	errno = 0;  /* a read given up after its tries sets none */
	const bool got = connector_read(fd, id, &c);
	*read = got || errno == ENOENT;
	if (!got)
		return false;
	bool ok = false;
	if (c.info.connection == 1 && c.info.count_modes > 0) {
		memset(o, 0, sizeof *o);
		connector_name(c.info.connector_type, c.info.connector_type_id,
					   o->name, sizeof o->name);
		o->connector_id = c.info.connector_id;
		*type = c.info.connector_type;
		ok = modes_read(o, c.modes, c.info.count_modes);
	}
	free(c.modes);
	if (ok)
		o->crtc_id = connector_crtc(fd, o->connector_id);
	return ok;
}

/* The connected outputs of one card, into the state, and which the window
 * opens on: the connector FNL_KMS_OUTPUT names, or the first connected
 * panel, or the first connected anything. -1 when the card has none, or
 * none of that name. */
static int card_read_outputs(struct BackendState *p, int fd,
							 const char *want)
{
	uint32_t ids[KMS_MAX_CONNECTORS];
	const int n = card_connectors(fd, ids);
	if (n <= 0)
		return -1;

	int count = 0, best = -1, best_rank = 0;
	for (int i = 0; i < n && count < KMS_MAX_OUTPUTS; i++) {
		KmsOutput *o = &p->outputs[count];
		uint32_t type = 0;
		bool read;
		if (!output_read(fd, ids[i], o, &type, &read))
			continue;
		const int rank = want ? (strcmp(o->name, want) == 0 ? 3 : 0)
							  : connector_internal(type) ? 2 : 1;
		if (rank > best_rank) {
			best = count;
			best_rank = rank;
		}
		count++;
	}
	fnl_atomic_store_u32(&p->output_count, (uint32_t)count);
	return best;
}

/* What each output's CRTC shows as the open finds it -- the console, as a
 * rule -- and at which mode, kept to be given back at close
 * (crtcs_restore()). A framebuffer's id names it to every client of the
 * device, the kernel's own included, so the console's can be set from
 * here. One entry a CRTC: a second connector cloned onto one would be
 * detached by a set naming the first alone. */
static void crtcs_save(struct BackendState *p, int fd)
{
	p->saved_count = 0;
	const int count = (int)fnl_atomic_load_u32(&p->output_count);
	for (int i = 0; i < count; i++) {
		struct drm_mode_crtc c = {
			.crtc_id = connector_live_crtc(fd, p->outputs[i].connector_id),
		};
		if (!c.crtc_id || drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0)
			continue;
		bool seen = false;
		for (int j = 0; j < p->saved_count && !seen; j++)
			seen = p->saved[j].crtc_id == c.crtc_id;
		if (seen)
			continue;
		KmsCrtcSaved *s = &p->saved[p->saved_count++];
		s->connector_id = p->outputs[i].connector_id;
		s->crtc_id = c.crtc_id;
		s->fb_id = c.fb_id;
		s->mode_valid = c.mode_valid != 0;
		s->mode = c.mode;
	}
}

/* [main] At close, every CRTC the open found lit is set back to what it
 * showed, so the console is on screen at once and before this file's
 * framebuffers go, which are then on no plane (scan_free()); the kernel's
 * own restore at the last close then finds it done. The CRTC the window is
 * on is turned off instead where the open found it dark, or where the
 * console will not come back on it, and that restore lights it. */
static void crtcs_restore(struct BackendState *p)
{
	uint32_t lit = p->scan_sel >= 0
		? connector_live_crtc(p->card_fd, kms_connector(p, p->scan_sel)) : 0;
	for (int i = 0; i < p->saved_count; i++) {
		const KmsCrtcSaved *s = &p->saved[i];
		const bool shown = s->fb_id && s->mode_valid;
		struct drm_mode_crtc set = {
			.set_connectors_ptr = (uintptr_t)&s->connector_id,
			.count_connectors   = shown ? 1 : 0,
			.crtc_id            = s->crtc_id,
			.fb_id              = shown ? s->fb_id : 0,
			.mode_valid         = shown ? 1 : 0,
			.mode               = s->mode,
		};
		if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
			fnl_log(FNL_LOG_WARN, "KMS: the console would not come back on "
					"connector %u (%s); off instead", s->connector_id,
					strerror(errno));
			struct drm_mode_crtc off = { .crtc_id = s->crtc_id };
			drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
		}
		if (s->crtc_id == lit)
			lit = 0;
	}
	if (lit) {
		struct drm_mode_crtc off = { .crtc_id = lit };
		drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
	}
}

/* The primary node of the first card with an output to give, held open and
 * master. Master is taken by being the first to open a card nobody holds;
 * SET_MASTER is then a no-op that says so, and fails while an X server or a
 * compositor has it. */
static FnlResult open_output(App *a)
{
	struct BackendState *p = a->backend_state;
	const char *want = getenv("FNL_KMS_OUTPUT");
	if (want && !want[0])
		want = NULL;

	for (int card = 0; card < KMS_MAX_CARDS; card++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", card);
		const int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		const int first = card_read_outputs(p, fd, want);
		if (first < 0) {
			close(fd);
			continue;
		}
		const int sel = KMS_SEL(first, p->outputs[first].mode_pref);
		mode_adopt(p, sel);
		fnl_atomic_store_u32(&p->sel_want, (uint32_t)sel);
		fnl_atomic_store_u32(&p->sel_good, (uint32_t)sel);
		if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0) {
			close(fd);
			return fnl_fail(FNL_ERR_NO_DISPLAY,
							"%s on %s is in another program's hands -- an X "
							"server or a compositor; this backend needs the "
							"display to itself", p->outputs[first].name,
							path);
		}
		p->card_fd = fd;
		p->card_index = card;
		crtcs_save(p, fd);
		char others[KMS_MAX_OUTPUTS * 34] = "";
		const int count = (int)fnl_atomic_load_u32(&p->output_count);
		for (int i = 0; i < count; i++)
			if (i != first) {
				const size_t at = strlen(others);
				snprintf(others + at, sizeof others - at, "%s%s",
						 at ? ", " : "", p->outputs[i].name);
			}
		fnl_log(FNL_LOG_INFO, "KMS: %s on %s, %dx%d at %u.%03u Hz, %d "
				"mode%s on offer; other outputs: %s",
				p->outputs[first].name, path, p->mode_w, p->mode_h,
				p->refresh_mhz / 1000, p->refresh_mhz % 1000,
				p->outputs[first].mode_count,
				p->outputs[first].mode_count == 1 ? "" : "s",
				others[0] ? others : "none");
		return FNL_OK;
	}
	if (want)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "no connected output named %s "
						"(FNL_KMS_OUTPUT) on any card", want);
	return fnl_fail(FNL_ERR_NO_DISPLAY, "no connected output on any card "
					"this user may open (/dev/dri/card*, group video)");
}

/* ---- the layout ------------------------------------------------------------
 *
 * There is nobody to hand over a keymap, so the console's own is built: the
 * names its configuration gives, resolved through xkeyboard-config's rules
 * and read from its symbols files (keymap_rules.c), which every machine
 * with a configured console has, where the tool that would compile them
 * is not installed by default. FNL_KMS_LAYOUT names a layout in their
 * place, for this backend alone: under a window server the layout is the
 * session's, and core answers the variable with one line there. */

#define KEYSYM_ISO_LEVEL3_SHIFT 0xfe03u

/* Which keys are AltGr in this layout: the ones whose plain level is
 * ISO_Level3_Shift. Every other modifier is a key this backend knows by its
 * code. */
static void keymap_note_level3(struct BackendState *p)
{
	p->ralt_is_level3 =
		p->keymap.sym[KEY_RIGHTALT + 8][0][0] == KEYSYM_ISO_LEVEL3_SHIFT;
}

/* "de(nodeadkeys),us caps:none": the names as FNL_KMS_LAYOUT spells them,
 * for the log. The layouts and the variants are comma lists side by side. */
static void keymap_names_show(const KeymapNames *n, char *out, size_t size)
{
	const char *l = n->layout, *v = n->variant;
	size_t at = 0;
	out[0] = '\0';
	while (*l && at + 1 < size) {
		const char *le = strchr(l, ','), *ve = *v ? strchr(v, ',') : NULL;
		const size_t ln = le ? (size_t)(le - l) : strlen(l);
		const size_t vn = ve ? (size_t)(ve - v) : strlen(v);
		snprintf(out + at, size - at, "%s%.*s%s%.*s%s", at ? "," : "",
				 (int)ln, l, vn ? "(" : "", (int)vn, v, vn ? ")" : "");
		at = strlen(out);
		l += le ? ln + 1 : ln;
		v += ve ? vn + 1 : vn;
	}
	if (n->options[0] && at + 1 < size)
		snprintf(out + at, size - at, " %s", n->options);
}

/* Each group's layout, "de" and "us(intl)", for the line a switch logs:
 * the comma lists side by side, as keymap_names_show() reads them. */
static void keymap_group_names(struct BackendState *p, const KeymapNames *n)
{
	const char *l = n->layout, *v = n->variant;
	memset(p->group_name, 0, sizeof p->group_name);
	for (unsigned g = 0; g < KEYMAP_GROUPS && *l; g++) {
		const char *le = strchr(l, ','), *ve = *v ? strchr(v, ',') : NULL;
		const size_t ln = le ? (size_t)(le - l) : strlen(l);
		const size_t vn = ve ? (size_t)(ve - v) : strlen(v);
		snprintf(p->group_name[g], sizeof p->group_name[g], "%.*s%s%.*s%s",
				 (int)ln, l, vn ? "(" : "", (int)vn, v, vn ? ")" : "");
		l += le ? ln + 1 : ln;
		v += ve ? vn + 1 : vn;
	}
}

static void keymap_load(struct BackendState *p)
{
	KeymapNames names;
	const char *from = "the console's";
	const char *spec = getenv("FNL_KMS_LAYOUT");
	bool named = spec && spec[0];
	if (named && !keymap_names_parse(&names, spec)) {
		fnl_log(FNL_LOG_WARN, "FNL_KMS_LAYOUT=%s is not <layout>[(<variant>)]"
				"[,<more>][ <options>]; the console's layout is used instead",
				spec);
		named = false;
	}
	if (named)
		from = "FNL_KMS_LAYOUT's";
	else if (!keymap_names_console(&names))
		from = "the default, with no console keyboard configured";

	char shown[256], detail[192];
	const char *why = NULL;
	keymap_names_show(&names, shown, sizeof shown);
	if (FNL_FAULT("kms-no-keymap"))
		why = "FNL_FAULT";
	else if (!keymap_load_names(&p->keymap, &names, NULL, detail,
								sizeof detail))
		why = detail;
	if (why) {
		memset(&p->keymap, 0, sizeof p->keymap);
		fnl_log(FNL_LOG_WARN, "no keymap for layout %s: %s, so text entry "
				"will type nothing", shown, why);
	} else {
		fnl_log(FNL_LOG_INFO, "KMS: layout %s, %s%s%s", shown, from,
				detail[0] ? "; left out: " : "", detail);
	}
	keymap_note_level3(p);
	keymap_group_names(p, &names);
	p->group = 0;
	p->groups = keymap_groups(&p->keymap);
}

/* ---- the cursor ------------------------------------------------------------
 *
 * Nothing draws a pointer on a bare output, so this does, on the CRTC's
 * cursor plane: a buffer the kernel moves without a frame being drawn. Drawn
 * here from text, three shapes, because there is no theme to load one from;
 * the rest of FnlCursor is the arrow, which says less and nothing wrong. A
 * '#' is black, a '+' white, anything else clear. */

static const char *const GLYPH_ARROW[] = {
	"#           ",
	"##          ",
	"#+#         ",
	"#++#        ",
	"#+++#       ",
	"#++++#      ",
	"#+++++#     ",
	"#++++++#    ",
	"#+++++++#   ",
	"#++++++++#  ",
	"#+++++++++# ",
	"#++++++#####",
	"#+++#++#    ",
	"#++# #++#   ",
	"#+#  #++#   ",
	"##    #++#  ",
	"#     #++#  ",
	"       #++# ",
	"       ###  ",
};

static const char *const GLYPH_IBEAM[] = {
	"#### ####",
	"#++#+#++#",
	"####+####",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"   #+#   ",
	"####+####",
	"#++#+#++#",
	"#### ####",
};

static const char *const GLYPH_CROSS[] = {
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"#######+#######",
	"+++++++++++++++",
	"#######+#######",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
	"      #+#      ",
};

typedef struct {
	const char *const *rows;
	int                count;
	int                hot_x, hot_y;
} GlyphArt;

#define ART(g) (g), (int)(sizeof (g) / sizeof (g)[0])
static const GlyphArt GLYPHS[KMS_GLYPH_COUNT] = {
	[KMS_GLYPH_ARROW] = { ART(GLYPH_ARROW), 0, 0 },
	[KMS_GLYPH_IBEAM] = { ART(GLYPH_IBEAM), 4, 8 },
	[KMS_GLYPH_CROSS] = { ART(GLYPH_CROSS), 7, 7 },
};
#undef ART

/* fnl_set_cursor()'s shapes, as glyphs. */
static const KmsGlyph CALLER_GLYPHS[] = {
	[FNL_CURSOR_DEFAULT]     = KMS_GLYPH_ARROW,
	[FNL_CURSOR_TEXT]        = KMS_GLYPH_IBEAM,
	[FNL_CURSOR_POINTER]     = KMS_GLYPH_ARROW,
	[FNL_CURSOR_CROSSHAIR]   = KMS_GLYPH_CROSS,
	[FNL_CURSOR_MOVE]        = KMS_GLYPH_ARROW,
	[FNL_CURSOR_EW_RESIZE]   = KMS_GLYPH_ARROW,
	[FNL_CURSOR_NS_RESIZE]   = KMS_GLYPH_ARROW,
	[FNL_CURSOR_NOT_ALLOWED] = KMS_GLYPH_ARROW,
	[FNL_CURSOR_WAIT]        = KMS_GLYPH_ARROW,
};
_Static_assert(sizeof CALLER_GLYPHS / sizeof CALLER_GLYPHS[0] ==
			   FNL_CURSOR_WAIT + 1,
			   "a FnlCursor was appended without a glyph for it here");

static void cursor_warn(struct BackendState *p, const char *what)
{
	if (p->cursor_warned)
		return;
	p->cursor_warned = true;
	fnl_log(FNL_LOG_WARN, "KMS: %s (%s), so the pointer is not shown", what,
			strerror(errno));
}

static bool glyph_make(struct BackendState *p, KmsGlyph g)
{
	const GlyphArt *art = &GLYPHS[g];
	struct drm_mode_create_dumb cd = {
		.width = p->cursor_w, .height = p->cursor_h, .bpp = 32,
	};
	if (FNL_FAULT("kms-no-cursor") ||
		drm_ioctl(p->card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return false;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	unsigned char *px = MAP_FAILED;
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
		px = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
				  p->card_fd, (off_t)md.offset);
	if (px == MAP_FAILED) {
		struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
		drm_ioctl(p->card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		return false;
	}
	memset(px, 0, (size_t)cd.size);
	for (int y = 0; y < art->count && (uint32_t)y < cd.height; y++) {
		const char *row = art->rows[y];
		for (int x = 0; row[x] && (uint32_t)x < cd.width; x++) {
			const uint32_t v = row[x] == '#' ? 0xff000000u
							 : row[x] == '+' ? 0xffffffffu : 0u;
			memcpy(px + (size_t)y * cd.pitch + (size_t)x * 4u, &v, 4);
		}
	}
	munmap(px, (size_t)cd.size);
	p->glyph_handle[g] = cd.handle;
	return true;
}

/* After a switch the pump looks for the new output's CRTC this often and
 * this many times: the render thread's mode set is a frame or two away,
 * and a device rebuild on the display path some hundred milliseconds. */
#define CURSOR_RECHECK_MS 50
#define CURSOR_RECHECKS   100

/* Take the cursor off the CRTC it was last shown on, if it shows one. */
static void cursor_hide(struct BackendState *p)
{
	const uint32_t crtc = p->cursor_on;
	if (crtc && p->glyph_shown != KMS_GLYPH_NONE &&
		p->glyph_shown != KMS_GLYPH_UNSET) {
		struct drm_mode_cursor c = { .flags = DRM_MODE_CURSOR_BO,
									 .crtc_id = crtc };
		drm_ioctl(p->card_fd, DRM_IOCTL_MODE_CURSOR, &c);
	}
}

/* Whether `crtc` is lit: a mode on it and a framebuffer on its plane. A
 * cursor set on one that is off is refused (EINVAL on amdgpu), and the copy
 * path switches off the CRTC of an output it leaves (kms_output_off()), so
 * coming back the open's guess is off until the render thread's mode set. */
static bool crtc_lit(int fd, uint32_t crtc)
{
	struct drm_mode_crtc c = { .crtc_id = crtc };
	return drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) == 0 && c.mode_valid &&
		   c.fb_id;
}

/* Show `g` with its hot spot at the pointer, or nothing for KMS_GLYPH_NONE.
 * A shape is set and moved in one ioctl, and only a move when it stands. An
 * output that refused a cursor once is not asked again: the plane is either
 * there or not, and the ioctl would otherwise fail on every motion. */
static void cursor_show(App *a, KmsGlyph g)
{
	struct BackendState *p = a->backend_state;
	if (p->cursor_warned)
		return;                     /* refused once: not asked per motion */
	const int px = (int)p->ptr_x, py = (int)p->ptr_y;
	/* The CRTC driving the output now, asked of the kernel until there is
	 * one: after a switch the render thread sets the mode, and the display
	 * path's driver chooses the CRTC itself. Until then the open's guess,
	 * and a shape set afresh once the answer is another CRTC. */
	if (!p->cursor_crtc)
		p->cursor_crtc = connector_live_crtc(
			p->card_fd, p->outputs[p->out_cur].connector_id);
	const uint32_t crtc = p->cursor_crtc ? p->cursor_crtc
										 : p->outputs[p->out_cur].crtc_id;
	if (!crtc)
		return;                     /* nothing can drive the output */
	/* Not yet lit: the recheck sets the shape once a CRTC drives the output,
	 * and a refusal from one that is off is not the output refusing a
	 * cursor, which would be taken as the last word (cursor_warn()). */
	if (!crtc_lit(p->card_fd, crtc)) {
		if (!p->cursor_recheck)
			p->cursor_recheck = CURSOR_RECHECKS;
		return;
	}
	if (crtc != p->cursor_on) {
		cursor_hide(p);
		p->glyph_shown = KMS_GLYPH_UNSET;
	}
	if (g == p->glyph_shown &&
		(g == KMS_GLYPH_NONE || (px == p->cursor_at_x &&
								 py == p->cursor_at_y)))
		return;

	struct drm_mode_cursor c = { .crtc_id = crtc };
	if (g != p->glyph_shown) {
		c.flags |= DRM_MODE_CURSOR_BO;
		if (g != KMS_GLYPH_NONE) {
			if (!p->glyph_handle[g] && !glyph_make(p, g)) {
				cursor_warn(p, "no buffer for the cursor plane");
				p->glyph_shown = g;
				return;
			}
			c.handle = p->glyph_handle[g];
			c.width  = p->cursor_w;
			c.height = p->cursor_h;
		}
	}
	if (g != KMS_GLYPH_NONE) {
		c.flags |= DRM_MODE_CURSOR_MOVE;
		c.x = px - GLYPHS[g].hot_x;
		c.y = py - GLYPHS[g].hot_y;
	}
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_CURSOR, &c) < 0) {
		cursor_warn(p, "the output took no cursor");
		p->glyph_shown = g;
		return;
	}
	p->glyph_shown = g;
	p->cursor_on = crtc;
	p->cursor_at_x = px;
	p->cursor_at_y = py;
}

static void cursor_update(App *a, Zone z, Edge edge)
{
	const DecoCursor c = deco_cursor(a, z, edge);
	cursor_show(a, c.kind == DECO_CURSOR_HIDDEN ? KMS_GLYPH_NONE
				 : c.kind == DECO_CURSOR_EDGE   ? KMS_GLYPH_ARROW
												: CALLER_GLYPHS[c.shape]);
}

/* fnl_set_cursor() moved the shape, or the pointer is new: the shape is
 * decided again where the pointer stands. */
static void kms_cursor_changed(App *a)
{
	Edge edge;
	const Zone z = deco_hit(a, a->ptr_x, a->ptr_y, &edge);
	cursor_update(a, z, edge);
}

static void cursor_init(struct BackendState *p)
{
	struct drm_get_cap w = { .capability = DRM_CAP_CURSOR_WIDTH };
	struct drm_get_cap h = { .capability = DRM_CAP_CURSOR_HEIGHT };
	p->cursor_w = drm_ioctl(p->card_fd, DRM_IOCTL_GET_CAP, &w) == 0 && w.value
					  ? (uint32_t)w.value : 64u;
	p->cursor_h = drm_ioctl(p->card_fd, DRM_IOCTL_GET_CAP, &h) == 0 && h.value
					  ? (uint32_t)h.value : 64u;
	p->glyph_shown = KMS_GLYPH_UNSET;
}

/* The window left an output for the current one: the cursor comes off the
 * CRTC it was on and is set afresh, on whichever drives the new output, at
 * the next show -- a CRTC has its own cursor plane. */
static void cursor_moved(App *a)
{
	struct BackendState *p = a->backend_state;
	cursor_hide(p);
	p->cursor_crtc = 0;
	p->cursor_on = 0;
	p->glyph_shown = KMS_GLYPH_UNSET;
	p->cursor_recheck = CURSOR_RECHECKS;
}

/* After a switch, a pump turn that nothing woke: until the kernel names
 * the CRTC driving the new output, the cursor waits on the open's guess --
 * and a pointer nobody moves would leave it there. */
static void cursor_recheck(App *a)
{
	struct BackendState *p = a->backend_state;
	p->cursor_recheck--;
	const uint32_t live = connector_live_crtc(
		p->card_fd, p->outputs[p->out_cur].connector_id);
	if (!live)
		return;
	p->cursor_recheck = 0;
	if (live != p->cursor_on) {
		p->cursor_crtc = live;
		kms_cursor_changed(a);
	}
}

static void cursor_free(struct BackendState *p)
{
	if (p->card_fd < 0)
		return;
	cursor_hide(p);
	for (int g = 0; g < KMS_GLYPH_COUNT; g++)
		if (p->glyph_handle[g]) {
			struct drm_mode_destroy_dumb dd = { .handle = p->glyph_handle[g] };
			drm_ioctl(p->card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
			p->glyph_handle[g] = 0;
		}
}

/* ---- publishing ---------------------------------------------------------- */

/* 100%, or the caller's override: a bare output says how many millimetres it
 * is, and a television says it wrongly, so no density is guessed from it. */
static int kms_scale120(const App *a)
{
	return a->scale_override120 > 0 ? a->scale_override120 : 120;
}

/* The window is the mode, so its size is said whenever anything is. */
static void publish_mode(App *a)
{
	struct BackendState *p = a->backend_state;
	const int s120 = kms_scale120(a);
	const int w = px_to_pt(p->mode_w, s120), h = px_to_pt(p->mode_h, s120);
	app_publish_size(a, w, h, s120);
	app_publish_refresh(a, p->refresh_mhz);
	app_publish_output(a, p->outputs[p->out_cur].name, w, h);
}

/* ---- the keyboard -------------------------------------------------------- */

static bool held(const struct BackendState *p, unsigned code)
{
	return code < 8u * sizeof p->down &&
		   (p->down[code / 8] >> (code % 8)) & 1u;
}

static void held_set(struct BackendState *p, unsigned code, bool on)
{
	if (code >= 8u * sizeof p->down)
		return;
	if (on)
		p->down[code / 8] |= (uint8_t)(1u << (code % 8));
	else
		p->down[code / 8] &= (uint8_t)~(1u << (code % 8));
}

/* The modifiers, worked out from the keys held: on a bare output nobody
 * reports them, and the locks are this backend's own toggles. */
static KeyMods kb_mods(const struct BackendState *p)
{
	const bool ralt = held(p, KEY_RIGHTALT);
	const bool alt = held(p, KEY_LEFTALT) || (ralt && !p->ralt_is_level3);
	const bool ctrl = held(p, KEY_LEFTCTRL) || held(p, KEY_RIGHTCTRL);
	const bool super = held(p, KEY_LEFTMETA) || held(p, KEY_RIGHTMETA);
	return (KeyMods){
		.shift   = held(p, KEY_LEFTSHIFT) || held(p, KEY_RIGHTSHIFT),
		.lock    = p->caps_on,
		.num     = p->num_on,
		.level3  = ralt && p->ralt_is_level3,
		.command = alt || ctrl || super,
		.group   = p->group,
		.alt     = alt,
		.ctrl    = ctrl,
		.super   = super,
	};
}

/* A group key pressed: the group it moves to, and a line saying so. True
 * when the key was one, which then locks nothing else -- grp:caps_toggle
 * puts ISO_Next_Group where Caps Lock was. */
static bool group_switch(struct BackendState *p, unsigned code)
{
	const KeyMods mods = kb_mods(p);
	unsigned to;
	if (!keymap_group_key(&p->keymap, code + 8u, &mods, p->groups, &to))
		return false;
	if (to != p->group) {
		p->group = to;
		fnl_log(FNL_LOG_INFO, "KMS: typing in group %u of %u, %s", to + 1,
				p->groups, p->group_name[to][0] ? p->group_name[to] : "?");
	}
	return true;
}

static void key_type(App *a, unsigned code, bool repeat)
{
	struct BackendState *p = a->backend_state;
	const KeyMods mods = kb_mods(p);
	const uint32_t was = p->compose.dead;
	const uint32_t c = keymap_type(&p->keymap, &p->compose, code + 8u, &mods,
								   repeat);
	app_text(a, c);
	if (p->compose.dead != was) {
		char shown[8];
		keymap_composing(&p->compose, shown);
		app_compose_set(a, shown);
	}
}

/* No input method to enable, but a field that closed mid-accent must not
 * leave the accent for the next one: X11's and Wayland's rule, for the
 * same composer. */
static void kms_text_input_changed(App *a)
{
	if (app_text_input(a))
		return;
	a->backend_state->compose.dead = 0;
	app_compose_set(a, NULL);
}

/* The lock lights, on every keyboard: the toggles are kept here, and a
 * keyboard shows none of its own without being told. */
static void leds_write(struct BackendState *p)
{
	struct input_event ev[3];
	memset(ev, 0, sizeof ev);
	ev[0].type = EV_LED; ev[0].code = LED_CAPSL; ev[0].value = p->caps_on;
	ev[1].type = EV_LED; ev[1].code = LED_NUML;  ev[1].value = p->num_on;
	ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
	for (int i = 0; i < p->input_count; i++)
		if (p->inputs[i].kind & KMS_KEYBOARD) {
			ssize_t n = write(p->inputs[i].fd, ev, sizeof ev);
			(void)n;                /* a read-only open has no lights */
		}
}

/* The kernel repeats every held key itself, as value 2, modifiers too; a
 * repeat goes through app_key_repeat() and only while text entry is on, and
 * types again where the layout says the key repeats. */
static void on_key(App *a, unsigned code, int value)
{
	struct BackendState *p = a->backend_state;
	const FnlKey k = key_from_evdev(code);

	if (value == 2) {
		if (app_text_input(a) &&
			keymap_held_repeats(&p->keymap, code + 8u, k)) {
			app_key_repeat(a, k);
			key_type(a, code, true);
		}
		return;
	}
	const bool pressed = value != 0;
	held_set(p, code, pressed);
	const bool grouped = pressed && group_switch(p, code);
	if (pressed && !grouped &&
		(code == KEY_CAPSLOCK || code == KEY_NUMLOCK)) {
		if (code == KEY_CAPSLOCK)
			p->caps_on = !p->caps_on;
		else
			p->num_on = !p->num_on;
		leds_write(p);
	}

	/* Key first, text second: the order fenestal.h promises. */
	app_key(a, k, pressed);
	if (pressed && app_text_input(a))
		key_type(a, code, false);
}

/* The keys a keyboard says are down, into the held set without acting on
 * them: at open, and after the kernel dropped events and the state has to
 * be read rather than followed. */
static void keys_seed(App *a, const KmsInput *d)
{
	struct BackendState *p = a->backend_state;
	uint8_t bits[sizeof p->down];
	memset(bits, 0, sizeof bits);
	if (ioctl(d->fd, EVIOCGKEY(sizeof bits), bits) < 0)
		return;
	for (unsigned code = 0; code < 8u * sizeof bits; code++)
		if ((bits[code / 8] >> (code % 8)) & 1u) {
			held_set(p, code, true);
			app_key_seed(a, key_from_evdev(code));
		}
}

/* ---- the pointer --------------------------------------------------------- */

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Where the pointer is now, said to the caller and to the titlebar. */
static void pointer_moved(App *a)
{
	struct BackendState *p = a->backend_state;
	const int s120 = want_s120(a);
	a->ptr_x = px_to_ptf(p->ptr_x, s120);
	a->ptr_y = px_to_ptf(p->ptr_y, s120);
	app_pointer_motion(a, a->ptr_x, a->ptr_y);
	deco_drag(a, a->ptr_x, a->ptr_y);
	Edge edge;
	const Zone z = deco_pointer_at(a, a->ptr_x, a->ptr_y, &edge);
	cursor_update(a, z, edge);
}

/* A delta in pixels: the pointer moves, or a captured one reports it. */
static void pointer_by(App *a, double dx, double dy)
{
	struct BackendState *p = a->backend_state;
	if (dx == 0.0 && dy == 0.0)
		return;
	if (p->captured) {
		const int s120 = want_s120(a);
		app_pointer_relative(a, px_to_ptf(dx, s120), px_to_ptf(dy, s120));
		if (p->captured)
			app_reticle_move(a, dx, dy);
		return;
	}
	p->ptr_x = clampd(p->ptr_x + dx, 0.0, p->mode_w - 1.0);
	p->ptr_y = clampd(p->ptr_y + dy, 0.0, p->mode_h - 1.0);
	pointer_moved(a);
}

/* An absolute device's report, onto the output. The axes are the device's
 * to state, so every difference of them is taken as a double, where no
 * pair of ints can overflow it. */
static void pointer_to(App *a, KmsInput *d)
{
	struct BackendState *p = a->backend_state;
	const double sx = d->x_max > d->x_min ? (double)d->x_max - d->x_min : 1;
	const double sy = d->y_max > d->y_min ? (double)d->y_max - d->y_min : 1;
	const double kx = (p->mode_w - 1) / sx, ky = (p->mode_h - 1) / sy;
	const double x = ((double)d->ax - d->x_min) * kx;
	const double y = ((double)d->ay - d->y_min) * ky;
	if (p->captured) {
		/* Taken in the device's units, which last_x holds exactly: against
		 * a pixel position kept as an int, every report would lose up to a
		 * pixel on each axis, always the same way. */
		if (d->have_last)
			pointer_by(a, ((double)d->ax - d->last_x) * kx,
					   ((double)d->ay - d->last_y) * ky);
		d->last_x = d->ax;
		d->last_y = d->ay;
		d->have_last = true;
		return;
	}
	p->ptr_x = clampd(x, 0.0, p->mode_w - 1.0);
	p->ptr_y = clampd(y, 0.0, p->mode_h - 1.0);
	pointer_moved(a);
}

/* A millimetre of finger on a touchpad, in pixels, unaccelerated: about a
 * third of a laptop's panel for the width of its pad. */
#define PAD_PX_PER_MM 8.0
/* A pad that gives no resolution is taken to be this wide. */
#define PAD_ASSUMED_MM 100.0

static double pad_scale(int res, int lo, int hi)
{
	if (res > 0)
		return PAD_PX_PER_MM / res;
	return hi > lo ? PAD_PX_PER_MM * PAD_ASSUMED_MM / ((double)hi - lo)
				   : 1.0;
}

/* Everything the report so far has moved, applied: at its end, and ahead of
 * a button inside it, so a click lands where the motion before it went. */
static void report_motion(App *a, KmsInput *d)
{
	if ((d->kind & KMS_PAD) && (d->ax_new || d->ay_new)) {
		if (d->touching && d->have_last) {
			d->dx += ((double)d->ax - d->last_x) *
					 pad_scale(d->x_res, d->x_min, d->x_max);
			d->dy += ((double)d->ay - d->last_y) *
					 pad_scale(d->y_res, d->y_min, d->y_max);
		}
		d->last_x = d->ax;
		d->last_y = d->ay;
		d->have_last = d->touching;
	} else if ((d->kind & KMS_TABLET) && (d->ax_new || d->ay_new)) {
		pointer_to(a, d);
	}
	d->ax_new = d->ay_new = false;
	pointer_by(a, d->dx, d->dy);
	d->dx = d->dy = 0.0;
}

static void on_button(App *a, KmsInput *d, unsigned code, int value,
					  uint32_t ms)
{
	FnlButton b;
	switch (code) {
	case BTN_LEFT:   b = FNL_BUTTON_LEFT;   break;
	case BTN_RIGHT:  b = FNL_BUTTON_RIGHT;  break;
	case BTN_MIDDLE: b = FNL_BUTTON_MIDDLE; break;
	case BTN_TOUCH:
		/* A pad's finger, or a touchscreen's, which is a left press. */
		if (d->kind & KMS_PAD) {
			d->touching = value != 0;
			d->have_last = false;
			return;
		}
		/* A new contact lands where it lands: from the last one's point it
		 * is no motion, and captured it would move the view by the gap.
		 * Ahead of report_motion(), whichever of the touch and its place
		 * the kernel sends first. */
		if (d->kind & KMS_TABLET)
			d->have_last = false;
		b = FNL_BUTTON_LEFT;
		break;
	default:
		return;
	}
	report_motion(a, d);
	app_pointer_button(a, b, value != 0, a->ptr_x, a->ptr_y, ms);
}

/* The wheel's detents: positive is towards the user, which is down. */
static void on_wheel(App *a, KmsInput *d, double dx, double dy)
{
	report_motion(a, d);
	app_pointer_scroll(a, a->ptr_x, a->ptr_y, dx, dy);
}

/* ---- the devices --------------------------------------------------------- */

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define LONGS(n)      (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static bool test_bit(const unsigned long *bits, unsigned i)
{
	return (bits[i / BITS_PER_LONG] >> (i % BITS_PER_LONG)) & 1ul;
}

/* What a device is, from what it says it can report. A keyboard has the
 * letters, so a lid switch or a power button is not one. */
static unsigned device_kind(int fd)
{
	unsigned long ev[LONGS(EV_CNT)], key[LONGS(KEY_CNT)], rel[LONGS(REL_CNT)],
				  axes[LONGS(ABS_CNT)], prop[LONGS(INPUT_PROP_CNT)];
	memset(ev, 0, sizeof ev);
	memset(key, 0, sizeof key);
	memset(rel, 0, sizeof rel);
	memset(axes, 0, sizeof axes);
	memset(prop, 0, sizeof prop);
	if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0)
		return 0;
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof axes), axes);
	ioctl(fd, EVIOCGPROP(sizeof prop), prop);

	unsigned kind = 0;
	if (test_bit(ev, EV_KEY) && test_bit(key, KEY_A) && test_bit(key, KEY_Z) &&
		test_bit(key, KEY_SPACE))
		kind |= KMS_KEYBOARD;
	if (test_bit(ev, EV_REL) && test_bit(rel, REL_X) && test_bit(rel, REL_Y) &&
		test_bit(key, BTN_LEFT))
		kind |= KMS_MOUSE;
	if (test_bit(ev, EV_ABS) && test_bit(axes, ABS_X) &&
		test_bit(axes, ABS_Y)) {
		if (test_bit(key, BTN_TOOL_FINGER) &&
			!test_bit(prop, INPUT_PROP_DIRECT))
			kind |= KMS_PAD;
		else if (test_bit(key, BTN_LEFT) || test_bit(key, BTN_TOUCH))
			kind |= KMS_TABLET;
	}
	return kind;
}

static KmsInput *input_by_fd(struct BackendState *p, int fd)
{
	for (int i = 0; i < p->input_count; i++)
		if (p->inputs[i].fd == fd)
			return &p->inputs[i];
	return NULL;
}

static bool input_open_already(const struct BackendState *p,
							   const char *node)
{
	for (int i = 0; i < p->input_count; i++)
		if (!strcmp(p->inputs[i].node, node))
			return true;
	return false;
}

/* A node of /dev/input, read if it is something this backend reads. Quiet on
 * failure: most nodes are not, and a new one is often not yet ours to open
 * until udev has set its group, which inotify reports as a second event. */
static void input_add(App *a, const char *node)
{
	struct BackendState *p = a->backend_state;
	if (strncmp(node, "event", 5) != 0 || input_open_already(p, node))
		return;
	char path[64];
	snprintf(path, sizeof path, "/dev/input/%s", node);
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 && errno == EACCES)
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	const unsigned kind = device_kind(fd);
	if (!kind || p->input_count >= KMS_MAX_INPUTS) {
		if (kind)
			fnl_log(FNL_LOG_WARN, "KMS: more than %d input devices; %s is "
					"not read", KMS_MAX_INPUTS, node);
		close(fd);
		return;
	}
	if ((kind & KMS_KEYBOARD) && ioctl(fd, EVIOCGRAB, 1) < 0) {
		fnl_log(FNL_LOG_WARN, "KMS: %s is held by another program (%s)", node,
				strerror(errno));
		close(fd);
		return;
	}
	/* The clock a press's time is on, for the double-click: one that steps
	 * with the wall clock could make one out of two presses a day apart.
	 * And the one fnl_input_latency_ms() counts from (loop_input_us()). */
	int clk = CLOCK_MONOTONIC;
	ioctl(fd, EVIOCSCLOCKID, &clk);

	KmsInput *d = &p->inputs[p->input_count];
	memset(d, 0, sizeof *d);
	d->fd = fd;
	d->kind = kind;
	snprintf(d->node, sizeof d->node, "%s", node);
	if (kind & (KMS_PAD | KMS_TABLET)) {
		struct input_absinfo x, y;
		memset(&x, 0, sizeof x);
		memset(&y, 0, sizeof y);
		ioctl(fd, EVIOCGABS(ABS_X), &x);
		ioctl(fd, EVIOCGABS(ABS_Y), &y);
		d->x_min = x.minimum; d->x_max = x.maximum; d->x_res = x.resolution;
		d->y_min = y.minimum; d->y_max = y.maximum; d->y_res = y.resolution;
		d->ax = x.value;
		d->ay = y.value;
	}
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	if (epoll_ctl(p->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		close(fd);
		return;
	}
	p->input_count++;

	if (kind & KMS_KEYBOARD) {
		uint8_t leds[LONGS(LED_CNT) * sizeof(unsigned long)];
		memset(leds, 0, sizeof leds);
		/* The locks as the first keyboard shows them, which is whatever
		 * the console left them at. */
		if (!p->leds_known && ioctl(fd, EVIOCGLED(sizeof leds), leds) >= 0) {
			p->caps_on = (leds[LED_CAPSL / 8] >> (LED_CAPSL % 8)) & 1u;
			p->num_on  = (leds[LED_NUML / 8] >> (LED_NUML % 8)) & 1u;
			p->leds_known = true;
		}
		keys_seed(a, d);
	}
}

/* A device that has gone. Its keys go with it, all of them: the held set is
 * one for every keyboard, and a key held on another is rare enough to lose. */
static void input_remove(App *a, KmsInput *d)
{
	struct BackendState *p = a->backend_state;
	const bool keyboard = (d->kind & KMS_KEYBOARD) != 0;
	close(d->fd);
	*d = p->inputs[--p->input_count];
	if (keyboard) {
		memset(p->down, 0, sizeof p->down);
		app_keys_clear(a);
	}
}

static void inputs_scan(App *a)
{
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return;
	for (struct dirent *e; (e = readdir(dir)) != NULL;)
		input_add(a, e->d_name);
	closedir(dir);
}

static void inputs_hotplug(App *a)
{
	struct BackendState *p = a->backend_state;
	_Alignas(struct inotify_event) char buf[4096];
	for (;;) {
		const ssize_t n = read(p->inotify_fd, buf, sizeof buf);
		if (n <= 0)
			return;
		const ssize_t head = (ssize_t)sizeof(struct inotify_event);
		for (ssize_t off = 0; off + head <= n;) {
			const struct inotify_event *e =
				(const struct inotify_event *)(const void *)(buf + off);
			if (e->len)
				input_add(a, e->name);
			off += (ssize_t)(sizeof *e + e->len);
		}
	}
}

/* One batch from a device; false when it has gone. Level-triggered, so what
 * is left is read on the next turn. */
static bool input_read(App *a, KmsInput *d)
{
	struct input_event ev[64];
	const ssize_t n = read(d->fd, ev, sizeof ev);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR;
	if (n == 0)
		return false;

	for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
		const struct input_event *e = &ev[i];
		const uint32_t ms = (uint32_t)((uint64_t)e->input_event_sec * 1000u +
									   (uint64_t)e->input_event_usec / 1000u);
		/* For fnl_input_latency_ms(), whatever this event turns out to
		 * hand in: a report's motion goes at its SYN_REPORT, on that
		 * stamp. */
		loop_input_us(a, (uint64_t)e->input_event_sec * 1000000u +
						 (uint64_t)e->input_event_usec);
		if (e->type == EV_SYN) {
			if (e->code == SYN_DROPPED) {
				d->dropped = true;
			} else if (e->code == SYN_REPORT && d->dropped) {
				/* The motion is lost and the keys are read afresh. */
				d->dropped = false;
				d->dx = d->dy = 0.0;
				d->have_last = false;
				if (d->kind & KMS_KEYBOARD) {
					memset(a->backend_state->down, 0,
						   sizeof a->backend_state->down);
					app_keys_clear(a);
					keys_seed(a, d);
				}
			} else if (e->code == SYN_REPORT) {
				report_motion(a, d);
			}
			continue;
		}
		if (d->dropped)
			continue;
		switch (e->type) {
		case EV_KEY:
			if (e->code >= BTN_MOUSE && e->code < BTN_JOYSTICK)
				on_button(a, d, e->code, e->value, ms);
			else if (e->code == BTN_TOUCH)
				on_button(a, d, e->code, e->value, ms);
			else if ((d->kind & KMS_KEYBOARD) && e->code < BTN_MISC)
				on_key(a, e->code, e->value);
			break;
		case EV_REL:
			if (e->code == REL_X)
				d->dx += e->value;
			else if (e->code == REL_Y)
				d->dy += e->value;
			else if (e->code == REL_WHEEL)
				on_wheel(a, d, 0.0, -e->value);
			else if (e->code == REL_HWHEEL)
				on_wheel(a, d, e->value, 0.0);
			break;
		case EV_ABS:
			if (e->code == ABS_X) {
				d->ax = e->value;
				d->ax_new = true;
			} else if (e->code == ABS_Y) {
				d->ay = e->value;
				d->ay_new = true;
			}
			break;
		default:
			break;
		}
	}
	return true;
}

/* ---- the copy path --------------------------------------------------------
 *
 * Where no Vulkan device drives the output (lavapipe has no display), the
 * frame is drawn on a headless surface and its pixels come here: into the
 * one of two dumb buffers not on screen, and onto the screen by a page flip.
 * The mode is set by the first frame, so the console stays up until there
 * is something to replace it with, and set again by the first frame after
 * the pump hands over another selection (kms_sel_to_show()). Render
 * thread, all of it. */

/* How long a flip may take before it is taken as lost: several periods of
 * the slowest mode there is. And how long one is waited out before the
 * node is lent to a driver (events_drain()): longer, since a flip taken as
 * lost there is read by the driver and not by the next frame. */
#define FLIP_WAIT_MS  100
#define FLIP_DRAIN_MS 500

/* A buffer of mode `m`'s size into slot `i` of `s`. */
static bool scan_make(struct BackendState *p, KmsScan *s,
					  const struct drm_mode_modeinfo *m, int i)
{
	struct drm_mode_create_dumb cd = {
		.width = m->hdisplay, .height = m->vdisplay, .bpp = 32,
	};
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return false;
	s->handle[i] = cd.handle;
	s->gen[i] = ++p->scan_gen;
	s->pitch = cd.pitch;
	s->size = (size_t)cd.size;
	s->w = m->hdisplay;
	s->h = m->vdisplay;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return false;
	void *map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED,
					 p->card_fd, (off_t)md.offset);
	if (map == MAP_FAILED)
		return false;
	s->map[i] = map;
	memset(map, 0, s->size);
	/* XRGB8888 by the legacy call, which every driver takes. */
	struct drm_mode_fb_cmd fb = {
		.width = cd.width, .height = cd.height, .pitch = cd.pitch,
		.bpp = 32, .depth = 24, .handle = cd.handle,
	};
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0)
		return false;
	s->fb[i] = fb.fb_id;
	return true;
}

/* Only of a pair no plane shows. The kernel removes a framebuffer that is
 * shown by disabling its plane, and where the driver refuses a CRTC with
 * no primary plane -- amdgpu -- by disabling the CRTC, which there fails
 * too: a WARNING with a stack in the journal each time (DESIGN *KMS: no
 * window server*). So a pair goes once another is set in its place
 * (scan_retire()), or its CRTC is given back or turned off (kms_close()). */
static void scan_free(struct BackendState *p, KmsScan *s)
{
	for (int i = 0; i < 2; i++) {
		if (s->fb[i]) {
			uint32_t fb = s->fb[i];
			drm_ioctl(p->card_fd, DRM_IOCTL_MODE_RMFB, &fb);
		}
		if (s->map[i])
			munmap(s->map[i], s->size);
		if (s->handle[i]) {
			struct drm_mode_destroy_dumb dd = { .handle = s->handle[i] };
			drm_ioctl(p->card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		}
	}
	memset(s, 0, sizeof *s);
}

/* The CRTC whose plane scans out one of `s`'s buffers, or 0. An inactive
 * CRTC counts: DPMS off leaves the framebuffer on the plane, and the
 * kernel removing it there disables the CRTC, which amdgpu refuses. */
static uint32_t scan_shown_on(const struct BackendState *p, const KmsScan *s)
{
	if (!s->fb[0] && !s->fb[1])
		return 0;
	uint32_t crtcs[KMS_MAX_CRTCS];
	struct drm_mode_card_res r;
	memset(&r, 0, sizeof r);
	r.count_crtcs = KMS_MAX_CRTCS;
	r.crtc_id_ptr = (uintptr_t)crtcs;
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0)
		return 0;
	const uint32_t n = r.count_crtcs < KMS_MAX_CRTCS ? r.count_crtcs
													 : KMS_MAX_CRTCS;
	for (uint32_t i = 0; i < n; i++) {
		struct drm_mode_crtc c = { .crtc_id = crtcs[i] };
		if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_GETCRTC, &c) == 0 &&
			c.fb_id && (c.fb_id == s->fb[0] || c.fb_id == s->fb[1]))
			return crtcs[i];
	}
	return 0;
}

/* The parked pairs no CRTC shows any longer, freed; with `off`, at close,
 * every one, its CRTC turned off first where one still shows it -- a CRTC
 * crtcs_restore() had nothing to set back, lit since the open. */
static void scan_parked_tidy(struct BackendState *p, bool off)
{
	for (int i = 0; i < KMS_PARKED; i++) {
		KmsScan *s = &p->scan_parked[i];
		if (!s->fb[0] && !s->fb[1] && !s->map[0] && !s->map[1])
			continue;
		const uint32_t crtc = scan_shown_on(p, s);
		if (crtc && !off)
			continue;
		if (crtc) {
			struct drm_mode_crtc dark = { .crtc_id = crtc };
			drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &dark);
		}
		scan_free(p, s);
	}
}

/* A pair let go: freed where no CRTC shows it, and parked where one still
 * does -- the frame kms_scanout_take() left on an output that then went
 * dark, which a take-back on another output at another size would
 * otherwise retire and remove (scan_free() says what that costs). */
static void scan_release(struct BackendState *p, KmsScan *s)
{
	scan_parked_tidy(p, false);
	if (scan_shown_on(p, s)) {
		for (int i = 0; i < KMS_PARKED; i++) {
			KmsScan *slot = &p->scan_parked[i];
			if (!slot->fb[0] && !slot->fb[1] && !slot->map[0] &&
				!slot->map[1]) {
				*slot = *s;
				memset(s, 0, sizeof *s);
				return;
			}
		}
		fnl_log(FNL_LOG_WARN, "KMS: %d outputs dark with a frame of ours "
				"each; one is removed while shown", KMS_PARKED);
	}
	scan_free(p, s);
}

/* Before a frame of mode `m`: the pair stays while the mode keeps its
 * size, and a frame of that size sets the mode again from it. Another
 * size's pair is made afresh and this one kept as scan_old until the new
 * one is on the plane -- unless an older pair waits there already, which
 * means nothing was set since and this one was never shown, so it goes. */
static void scan_retire(struct BackendState *p,
						const struct drm_mode_modeinfo *m)
{
	if (!p->scan.map[0] ||
		(p->scan.w == m->hdisplay && p->scan.h == m->vdisplay))
		return;
	if (p->scan_old.map[0]) {
		scan_release(p, &p->scan);
	} else {
		p->scan_old = p->scan;
		memset(&p->scan, 0, sizeof p->scan);
	}
	p->scan_front = -1;
}

/* Wait up to `ms` for the pending flip's completion event; whether it came.
 * The node's events are this thread's alone only while no driver holds a
 * copy of the descriptor: one open file is one event queue, and Mesa's
 * display WSI reads it on a thread of its own for as long as it is lent
 * one (surface_kms.c). Nothing here flips while it is, and events_drain()
 * keeps the two from reading each other's. */
static bool flip_wait(App *a, int ms)
{
	struct BackendState *p = a->backend_state;
	while (p->flip_pending) {
		struct pollfd pf = { .fd = p->card_fd, .events = POLLIN };
		const int r = poll(&pf, 1, ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		_Alignas(struct drm_event) char buf[1024];
		const ssize_t n = read(p->card_fd, buf, sizeof buf);
		if (n <= 0)
			return false;
		for (ssize_t off = 0;
			 off + (ssize_t)sizeof(struct drm_event) <= n;) {
			const struct drm_event *e =
				(const struct drm_event *)(const void *)(buf + off);
			if (e->type == DRM_EVENT_FLIP_COMPLETE) {
				p->flip_pending = false;
				/* When it reached the screen, by the kernel's stamp on
				 * CLOCK_MONOTONIC -- the frame clock's -- and which frame
				 * it was, from the flip's user_data (kms_scan_show()). A
				 * mode set's first frame has no flip and no stamp. */
				if (e->length >= sizeof(struct drm_event_vblank)) {
					const struct drm_event_vblank *v =
						(const struct drm_event_vblank *)(const void *)e;
					app_frame_shown(a, v->user_data,
									(double)v->tv_sec +
										(double)v->tv_usec * 1e-6,
									!p->flip_async);
				}
			}
			if (e->length == 0)
				break;
			off += (ssize_t)e->length;
		}
	}
	return true;
}

/* Read off every event the node holds now, waiting for none. What the
 * queue holds is nobody's at two points. Before the descriptor is lent to
 * a driver, once the pending flip is waited out: a flip of ours still
 * queued is read by Mesa's wait thread as one of its own images and its
 * user_data, 0 for ours, dereferenced -- a segfault inside the driver at
 * the move from OpenGL back to Vulkan. And after a mode set, which the
 * kernel completes only once every flip before it has: the driver's last,
 * in flight when the display was released and it stopped reading, would
 * otherwise count as the first of ours, and the next flip find the CRTC
 * busy and stay refused for good. */
static void events_drain(struct BackendState *p)
{
	for (;;) {
		struct pollfd pf = { .fd = p->card_fd, .events = POLLIN };
		const int r = poll(&pf, 1, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return;
		char buf[1024];
		if (read(p->card_fd, buf, sizeof buf) <= 0)
			return;
	}
}

static void scan_refuse(struct BackendState *p, const char *what)
{
	if (!p->scan_failed)
		fnl_log(FNL_LOG_WARN, "KMS: %s (%s), so no frame reaches %s", what,
				strerror(errno), kms_name(p, p->scan_sel));
	p->scan_failed = true;
}

/* Under vsync, a flip this frame waits out, as a FIFO present would. Without
 * it the flip is asynchronous -- the frame goes out at once and tears, as
 * IMMEDIATE does on a window server -- and a frame that still finds the
 * last one pending is dropped, nothing waiting. A driver that refuses the
 * asynchronous flip (EINVAL: none of its own, or amdgpu's for an update
 * that is not "fast", which one between two dumb buffers of one size is)
 * gets the vblank flip until the next mode set, with one line.
 *
 * In two halves, for the copy path and for the OpenGL bridge, which draws
 * into the back buffer on the GPU in place of the copy (egl_kms.c):
 * kms_scan_begin() is the buffer the frame goes into, the selection
 * adopted, the pair made and the last flip waited out; kms_scan_show()
 * puts it on the screen. */
int kms_scan_begin(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->scan_failed)
		return -1;
	/* Another mode, or another output: the last flip waited in, an output
	 * left goes dark, the buffers kept or retired by the new mode's size
	 * (scan_retire()), and the first frame sets the mode. */
	const int show = kms_sel_to_show(p);
	const uint32_t resets = fnl_atomic_load_u32(&p->scan_resets);
	if (show != p->scan_sel || resets != p->scan_resets_seen) {
		p->scan_resets_seen = resets;
		flip_wait(a, FLIP_WAIT_MS);
		p->flip_pending = false;
		const int was = p->scan_sel;
		p->scan_sel = show;
		if (was >= 0 && KMS_SEL_OUT(was) != KMS_SEL_OUT(show))
			kms_output_off(a, KMS_SEL_OUT(was), show, true);
		scan_retire(p, kms_sel_mode(p, show));
		p->scan_set_owed = true;
	}
	const struct drm_mode_modeinfo *mode = kms_sel_mode(p, p->scan_sel);
	if (!p->scan.map[0]) {
		p->scan_front = -1;
		if (!scan_make(p, &p->scan, mode, 0) ||
			!scan_make(p, &p->scan, mode, 1)) {
			scan_free(p, &p->scan);
			/* A mode switched to whose buffers cannot be had is one that
			 * will not show; the last that did comes back next frame. */
			if ((int)fnl_atomic_load_u32(&p->sel_good) != p->scan_sel) {
				kms_sel_refused(a, p->scan_sel, false);
				return -1;
			}
			scan_refuse(p, "no scanout buffer");
			return -1;
		}
	}
	const bool vsync = fnl_atomic_load_bool(&a->vsync);
	if (p->flip_pending && !flip_wait(a, vsync ? FLIP_WAIT_MS : 0)) {
		if (vsync)
			p->flip_pending = false;        /* lost: flip again regardless */
		else
			return -1;
	}
	return p->scan_front == 0 ? 1 : 0;
}

void kms_scan_show(App *a, int back)
{
	struct BackendState *p = a->backend_state;
	if (back < 0 || back > 1 || !p->scan.fb[back])
		return;
	const bool vsync = fnl_atomic_load_bool(&a->vsync);
	const struct drm_mode_modeinfo *mode = kms_sel_mode(p, p->scan_sel);
	const KmsOutput *out = kms_out(p, p->scan_sel);
	if (p->scan_front < 0 || p->scan_set_owed) {
		struct drm_mode_crtc set = {
			.set_connectors_ptr = (uintptr_t)&out->connector_id,
			.count_connectors   = 1,
			.crtc_id            = out->crtc_id,
			.fb_id              = p->scan.fb[back],
			.mode_valid         = 1,
			.mode               = *mode,
		};
		const bool good =
			(int)fnl_atomic_load_u32(&p->sel_good) == p->scan_sel;
		const bool forced = !good && FNL_FAULT("kms-mode-refused");
		const bool set_ok = out->crtc_id && !forced &&
			drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
		if (!set_ok && !good) {
			/* A mode the output was switched to and will not take: the
			 * last that did comes back at the next frame. */
			kms_sel_refused(a, p->scan_sel, false);
			return;
		}
		if (!set_ok) {
			scan_refuse(p, "the mode would not set");
			return;
		}
		events_drain(p);
		/* Off this plane now that this pair is on, and on no other's
		 * unless an output gone dark still shows it (scan_release()). */
		scan_release(p, &p->scan_old);
		kms_sel_shown(a, p->scan_sel);
		p->scan_front = back;
		p->scan_set_owed = false;
		p->flip_async_off = false;
		return;
	}
	const bool async = !vsync && !p->flip_async_off;
	/* The frame's index rides on the flip and comes back in its event
	 * (flip_wait()): the renderer counts the present after this returns,
	 * so a->presents is still this frame's FnlFrame::index. */
	struct drm_mode_crtc_page_flip flip = {
		.crtc_id   = out->crtc_id,
		.fb_id     = p->scan.fb[back],
		.flags     = DRM_MODE_PAGE_FLIP_EVENT |
					 (async ? DRM_MODE_PAGE_FLIP_ASYNC : 0u),
		.user_data = a->presents,
	};
	int r = drm_ioctl(p->card_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
	if (r < 0 && async && errno == EINVAL) {
		p->flip_async_off = true;
		fnl_log(FNL_LOG_INFO, "KMS: %s takes no asynchronous flip; without "
				"vsync a frame that finds one pending is dropped",
				kms_name(p, p->scan_sel));
		flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
		r = drm_ioctl(p->card_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
	}
	p->flip_async = (flip.flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0;
	if (r < 0) {
		scan_refuse(p, "the flip was refused");
		return;
	}
	p->flip_pending = true;
	p->scan_front = back;
	if (vsync)
		flip_wait(a, FLIP_WAIT_MS);
}

static void kms_present_pixels(App *a, const void *px, int w, int h,
							   size_t pitch, bool bgra)
{
	struct BackendState *p = a->backend_state;
	const int back = kms_scan_begin(a);
	if (back < 0)
		return;
	const struct drm_mode_modeinfo *mode = kms_sel_mode(p, p->scan_sel);
	const int cw = w < mode->hdisplay ? w : mode->hdisplay;
	const int ch = h < mode->vdisplay ? h : mode->vdisplay;
	const unsigned char *src = px;
	for (int y = 0; y < ch; y++) {
		unsigned char *dst = p->scan.map[back] + (size_t)y * p->scan.pitch;
		const unsigned char *row = src + (size_t)y * pitch;
		if (bgra) {
			memcpy(dst, row, (size_t)cw * 4u);
			continue;
		}
		for (int x = 0; x < cw; x++) {
			dst[4 * x + 0] = row[4 * x + 2];
			dst[4 * x + 1] = row[4 * x + 1];
			dst[4 * x + 2] = row[4 * x + 0];
			dst[4 * x + 3] = 0xff;
		}
	}
	kms_scan_show(a, back);
}

/* [render] Buffer `i` of the pair as a dma-buf the GPU can draw into: its
 * size, pitch and format, and a key that names it and no buffer made
 * before or after, so the bridge imports each once. -1 where the kernel
 * will not export it -- a driver whose dumb buffers no other device may
 * share -- and the bridge then copies. The caller closes the descriptor. */
int kms_scan_export(App *a, int i, KmsScanBuf *out)
{
	struct BackendState *p = a->backend_state;
	if (i < 0 || i > 1 || !p->scan.handle[i])
		return -1;
	struct drm_prime_handle ph = {
		.handle = p->scan.handle[i],
		.flags  = DRM_CLOEXEC | DRM_RDWR,
		.fd     = -1,
	};
	if (drm_ioctl(p->card_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0)
		return -1;
	*out = (KmsScanBuf){
		.w = p->scan.w, .h = p->scan.h, .pitch = p->scan.pitch,
		.fourcc = DRM_FORMAT_XRGB8888,
		.key = p->scan.gen[i],
	};
	return ph.fd;
}

/* [render] Through the mapping every buffer of the pair has: a GPU that
 * imports a buffer of another device's may draw into a copy of its own,
 * and nothing else says so (kms_scan_peek()'s caller, egl_kms.c). */
bool kms_scan_peek(App *a, int i, uint32_t *first, uint32_t *last)
{
	const struct BackendState *p = a->backend_state;
	if (i < 0 || i > 1 || !p->scan.map[i] || p->scan.w <= 0 ||
		p->scan.h <= 0)
		return false;
	const unsigned char *m = p->scan.map[i];
	const size_t end = (size_t)(p->scan.h - 1) * p->scan.pitch +
					   (size_t)(p->scan.w - 1) * 4u;
	memcpy(first, m, 4);
	memcpy(last, m + end, 4);
	return true;
}

/* [render] The key kms_scan_export() would give buffer `i`, without the
 * export: 0 where there is none. */
uint32_t kms_scan_key(App *a, int i)
{
	const struct BackendState *p = a->backend_state;
	return i >= 0 && i <= 1 ? p->scan.gen[i] : 0u;
}

/* ---- the mode and the output -----------------------------------------------
 *
 * The window is a mode of an output, and the two are chosen together, as
 * one selection (KMS_SEL). The pump adopts it, hands it to the render
 * thread (sel_want), asks for the surface to be made again -- a display
 * surface names its mode and its display -- and only then publishes the
 * size; app_surface_remake() says why that order. A selection that would
 * not show is marked by the render thread, which goes back to the last that
 * did and wakes the pump to say so. DESIGN *KMS: no window server*. */

/* [render] One writer of each, so a load and a store are the update. */
void kms_sel_refused(App *a, int sel, bool whole)
{
	struct BackendState *p = a->backend_state;
	if (whole) {
		fnl_atomic_store_u32(&p->outputs_refused,
							 fnl_atomic_load_u32(&p->outputs_refused) |
								 1u << KMS_SEL_OUT(sel));
	} else {
		FnlAtomicU32 *m = &p->modes_refused[KMS_SEL_OUT(sel)];
		fnl_atomic_store_u32(m, fnl_atomic_load_u32(m) |
									1u << KMS_SEL_MODE(sel));
	}
	loop_wake(p->wake_fd);
}

void kms_sel_shown(App *a, int sel)
{
	fnl_atomic_store_u32(&a->backend_state->sel_good, (uint32_t)sel);
}

/* A connector's property of this name, or 0. */
static uint32_t connector_prop(int fd, uint32_t connector_id,
							   const char *name)
{
	uint32_t ids[64];
	uint64_t values[64];
	struct drm_mode_obj_get_properties gp = {
		.props_ptr = (uintptr_t)ids, .prop_values_ptr = (uintptr_t)values,
		.count_props = 64, .obj_id = connector_id,
		.obj_type = DRM_MODE_OBJECT_CONNECTOR,
	};
	if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
		return 0;
	const uint32_t n = gp.count_props < 64 ? gp.count_props : 64;
	for (uint32_t i = 0; i < n; i++) {
		struct drm_mode_get_property prop = { .prop_id = ids[i] };
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) == 0 &&
			strcmp(prop.name, name) == 0)
			return ids[i];
	}
	return 0;
}

/* [render] The CRTC driving an output the window left goes dark, so the
 * monitor says it has no picture rather than holding the last frame, and
 * the CRTC is free for the output now shown; unless that output is driven
 * by it already. Asked of the kernel, not read from the open's guess:
 * Vulkan's driver picks the CRTC for a display it sets.
 *
 * `unbind`: the CRTC switched off, which also lets the connector go from it
 * -- the copy path's, which chooses its CRTCs itself. Otherwise the
 * connector's DPMS off: the CRTC inactive, no signal, and the connector
 * still bound to it. The display path wants that, since Vulkan's driver
 * takes a connector's bound CRTC next time and keeps the primary plane it
 * chose the first time: Mesa's wsi_display_select_plane() returns the one
 * it cached whatever CRTC it now has (26.2.4, and main), and a connector
 * left unbound got the first free CRTC beside the old one's plane, which
 * the kernel refuses ("Invalid CRTC for plane"), every swapchain after. */
void kms_output_off(App *a, int out, int shown, bool unbind)
{
	struct BackendState *p = a->backend_state;
	const uint32_t connector = p->outputs[out].connector_id;
	const uint32_t crtc = connector_live_crtc(p->card_fd, connector);
	const uint32_t keep = connector_live_crtc(p->card_fd,
											  kms_connector(p, shown));
	if (!crtc || crtc == keep)
		return;
	int r;
	if (unbind) {
		struct drm_mode_crtc off = { .crtc_id = crtc };
		r = drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
	} else {
		struct drm_mode_obj_set_property dpms = {
			.value = DRM_MODE_DPMS_OFF,
			.prop_id = connector_prop(p->card_fd, connector, "DPMS"),
			.obj_id = connector, .obj_type = DRM_MODE_OBJECT_CONNECTOR,
		};
		r = dpms.prop_id
			? drm_ioctl(p->card_fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &dpms)
			: -1;
	}
	if (r < 0)
		fnl_log(FNL_LOG_WARN, "KMS: %s would not go dark (%s)",
				p->outputs[out].name, strerror(errno));
	else
		fnl_log(FNL_LOG_INFO, "KMS: %s dark", p->outputs[out].name);
}

/* The take-back buffer's picture: the last frame the move read back
 * (App::snap), where there is one, so the output freezes through the gap
 * instead of going dark -- rows to the mode's size, BGRA as the snapshot
 * always is, the rest black -- and black where there is none. Said to the
 * core (App::snap_shown), which lifts the cover again from it after a
 * loss during the lift. */
static void scan_fill(struct BackendState *p, int back,
					  const struct drm_mode_modeinfo *m, App *a)
{
	memset(p->scan.map[back], 0, p->scan.size);
	if (!a->snap_valid || !a->snap_px)
		return;
	const int cw = a->snap_w < m->hdisplay ? a->snap_w : m->hdisplay;
	const int ch = a->snap_h < m->vdisplay ? a->snap_h : m->vdisplay;
	const size_t pitch = (size_t)a->snap_w * 4u;
	for (int y = 0; y < ch; y++) {
		unsigned char *dst = p->scan.map[back] + (size_t)y * p->scan.pitch;
		memcpy(dst, a->snap_px + (size_t)y * pitch, (size_t)cw * 4u);
	}
	fnl_mutex_lock(&a->lock);
	a->snap_shown = true;
	fnl_mutex_unlock(&a->lock);
}

/* [render] The output taken back before the driver's swapchain on it goes
 * (VkBridge::scanout_take): a frame of the backend's own -- the last one
 * the move read back, else black (scan_fill()) -- on the CRTC the driver
 * set and at the mode it set, so that the swapchain's framebuffers are on
 * no plane when the driver removes them (scan_free() says what removing a
 * shown one costs). The mode is read off the CRTC rather than taken from
 * the selection: only a mode equal to the one set makes the set a plane
 * update and not a mode set, and the driver's pick of the selection's size
 * may differ from the backend's in its timings. The copy path's next frame
 * finds the pair at its size and sets its own mode from it
 * (scan_set_owed). */
void kms_scanout_take(App *a)
{
	struct BackendState *p = a->backend_state;
	const int sel = p->surf_sel >= 0 ? p->surf_sel : kms_sel_to_show(p);
	const uint32_t connector = kms_connector(p, sel);
	struct drm_mode_crtc on = {
		.crtc_id = connector_live_crtc(p->card_fd, connector),
	};
	if (!on.crtc_id ||
		drm_ioctl(p->card_fd, DRM_IOCTL_MODE_GETCRTC, &on) < 0 ||
		!on.mode_valid || !on.fb_id)
		return;
	flip_wait(a, FLIP_WAIT_MS);
	p->flip_pending = false;
	scan_retire(p, &on.mode);
	if (!p->scan.map[0] && (!scan_make(p, &p->scan, &on.mode, 0) ||
							!scan_make(p, &p->scan, &on.mode, 1))) {
		scan_free(p, &p->scan);
		fnl_log(FNL_LOG_WARN, "KMS: no buffer to take %s back with (%s)",
				kms_name(p, sel), strerror(errno));
		return;
	}
	const int back = p->scan_front == 0 ? 1 : 0;
	scan_fill(p, back, &on.mode, a);
	struct drm_mode_crtc set = {
		.set_connectors_ptr = (uintptr_t)&connector,
		.count_connectors   = 1,
		.crtc_id            = on.crtc_id,
		.fb_id              = p->scan.fb[back],
		.mode_valid         = 1,
		.mode               = on.mode,
	};
	if (drm_ioctl(p->card_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
		fnl_log(FNL_LOG_WARN, "KMS: %s could not be taken back from the "
				"driver (%s)", kms_name(p, sel), strerror(errno));
		return;
	}
	events_drain(p);
	scan_release(p, &p->scan_old);
	p->scan_front = back;
	p->scan_sel = sel;
	p->scan_set_owed = true;
}

/* Adopted and handed over, the surface asked for again, the size
 * published; the pointer kept where it was on an output it stays on, and
 * put in the middle of one it moves to. */
static void sel_set(App *a, int sel)
{
	struct BackendState *p = a->backend_state;
	const int was = p->out_cur;
	mode_adopt(p, sel);
	fnl_atomic_store_u32(&p->sel_want, (uint32_t)sel);
	app_surface_remake(a);
	if (p->out_cur != was) {
		p->ptr_x = p->mode_w / 2.0;
		p->ptr_y = p->mode_h / 2.0;
		cursor_moved(a);
	} else {
		p->ptr_x = clampd(p->ptr_x, 0.0, p->mode_w - 1.0);
		p->ptr_y = clampd(p->ptr_y, 0.0, p->mode_h - 1.0);
	}
	fnl_log(FNL_LOG_INFO, "KMS: %s at %dx%d, %u.%03u Hz",
			p->outputs[p->out_cur].name, p->mode_w, p->mode_h,
			p->refresh_mhz / 1000, p->refresh_mhz % 1000);
	publish_mode(a);
	pointer_moved(a);
}

/* After a wake: the selection handed over would not show, and the render
 * thread has gone back to the last that did. Nothing is asked of it again;
 * the size is published to match what it already shows. */
static void sel_refused_seen(App *a)
{
	struct BackendState *p = a->backend_state;
	const int now = KMS_SEL(p->out_cur, p->mode_cur);
	if (!kms_sel_is_refused(p, now))
		return;
	const int was = p->out_cur, was_w = p->mode_w, was_h = p->mode_h;
	mode_adopt(p, (int)fnl_atomic_load_u32(&p->sel_good));
	fnl_atomic_store_u32(&p->sel_want, fnl_atomic_load_u32(&p->sel_good));
	if (p->out_cur != was)
		cursor_moved(a);
	p->ptr_x = clampd(p->ptr_x, 0.0, p->mode_w - 1.0);
	p->ptr_y = clampd(p->ptr_y, 0.0, p->mode_h - 1.0);
	fnl_log(FNL_LOG_WARN, "KMS: %s would not show %dx%d; back to %s at "
			"%dx%d", p->outputs[was].name, was_w, was_h,
			p->outputs[p->out_cur].name, p->mode_w, p->mode_h);
	publish_mode(a);
	pointer_moved(a);
}

/* The mode of this size on the output the window is on, the refresh
 * nearest, of those not refused; or -1. */
static int mode_find(const struct BackendState *p, const FnlMode *want)
{
	const KmsOutput *o = &p->outputs[p->out_cur];
	int best = -1;
	unsigned int best_off = 0;
	for (int i = 0; i < o->mode_count; i++) {
		if (o->modes[i].hdisplay != want->w_px ||
			o->modes[i].vdisplay != want->h_px ||
			kms_sel_is_refused(p, KMS_SEL(p->out_cur, i)))
			continue;
		const unsigned int off = refresh_off(&o->modes[i],
											 want->refresh_mhz);
		if (best < 0 || off < best_off) {
			best = i;
			best_off = off;
		}
	}
	return best;
}

static void kms_set_mode(App *a, const FnlMode *mode)
{
	struct BackendState *p = a->backend_state;
	const int i = mode_find(p, mode);
	if (i >= 0 && i != p->mode_cur)
		sel_set(a, KMS_SEL(p->out_cur, i));
}

/* Output `o` at its preferred mode, or where that would not show its first
 * that would. */
static void output_pick(App *a, int o)
{
	struct BackendState *p = a->backend_state;
	int m = p->outputs[o].mode_pref;
	for (int i = 0; kms_sel_is_refused(p, KMS_SEL(o, m)) &&
					i < p->outputs[o].mode_count; i++)
		m = i;
	if (!kms_sel_is_refused(p, KMS_SEL(o, m)))
		sel_set(a, KMS_SEL(o, m));
}

/* An output that is away has no monitor for the kernel to light, so the
 * move is owed until it is back (outputs_rescan()); a later request
 * replaces the one owed. */
static void kms_set_output(App *a, const char *name)
{
	struct BackendState *p = a->backend_state;
	const int count = (int)fnl_atomic_load_u32(&p->output_count);
	const uint32_t gone = fnl_atomic_load_u32(&p->outputs_gone);
	p->away_owed = -1;
	for (int o = 0; o < count; o++) {
		if (strcmp(p->outputs[o].name, name) != 0 || o == p->out_cur ||
			((gone >> o) & 1u))
			continue;
		if ((p->outputs_away >> o) & 1u) {
			p->away_owed = o;
			fnl_log(FNL_LOG_INFO, "KMS: %s is away; the window goes there "
					"when it is back", p->outputs[o].name);
		} else {
			output_pick(a, o);
		}
		return;
	}
}

/* The lists, from what was read at open and the atomics: the output handed
 * over, which is the pump's own, and what would not show, which leaves the
 * lists. */
static int kms_modes(const App *a, FnlMode *out, int max, int *current)
{
	const struct BackendState *p = a->backend_state;
	const int sel = (int)fnl_atomic_load_u32(&p->sel_want);
	const KmsOutput *o = kms_out(p, sel);
	int n = 0;
	*current = -1;
	for (int i = 0; i < o->mode_count && n < max; i++) {
		if (kms_sel_is_refused(p, KMS_SEL(KMS_SEL_OUT(sel), i)))
			continue;
		if (i == KMS_SEL_MODE(sel))
			*current = n;
		out[n].w_px = o->modes[i].hdisplay;
		out[n].h_px = o->modes[i].vdisplay;
		out[n].refresh_mhz = kms_refresh_mhz(&o->modes[i]);
		n++;
	}
	return n;
}

static int kms_outputs(const App *a, char (*names)[OUTPUT_NAME_MAX], int max,
					   int *current)
{
	const struct BackendState *p = a->backend_state;
	const int now = KMS_SEL_OUT(fnl_atomic_load_u32(&p->sel_want));
	const uint32_t refused = fnl_atomic_load_u32(&p->outputs_refused) |
							 fnl_atomic_load_u32(&p->outputs_gone);
	const int count = (int)fnl_atomic_load_u32(&p->output_count);
	int n = 0;
	*current = -1;
	for (int o = 0; o < count && n < max; o++) {
		if ((refused >> o) & 1u)
			continue;
		if (o == now)
			*current = n;
		snprintf(names[n], OUTPUT_NAME_MAX, "%s", p->outputs[o].name);
		n++;
	}
	return n;
}

/* ---- monitors, and GPUs, plugged and unplugged ------------------------------
 *
 * The kernel says a connector changed with a uevent -- "HOTPLUG=1" from the
 * card's DRM device -- on the netlink group anyone may read, which is what
 * udev itself listens to; read here, it needs no library. A uevent says only
 * that something changed, so the card's connectors are read again, each
 * probed, which takes tens of milliseconds for an HDMI EDID; on the pump's
 * thread, which has nothing else to do while monitors come and go. */

static void uevent_open(struct BackendState *p)
{
	p->uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC |
						  SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (p->uevent_fd < 0) {
		fnl_log(FNL_LOG_WARN, "KMS: no uevents (%s); a monitor plugged in "
				"later is not seen", strerror(errno));
		return;
	}
	struct sockaddr_nl at = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = p->uevent_fd };
	if (bind(p->uevent_fd, (struct sockaddr *)&at, sizeof at) < 0 ||
		epoll_ctl(p->epoll_fd, EPOLL_CTL_ADD, p->uevent_fd, &ev) < 0) {
		fnl_log(FNL_LOG_WARN, "KMS: no uevents (%s); a monitor plugged in "
				"later is not seen", strerror(errno));
		close(p->uevent_fd);
		p->uevent_fd = -1;
	}
}

/* Whether a uevent is a hotplug of this card: its strings, NUL-separated
 * after the "action@path" header, carry SUBSYSTEM=drm, HOTPLUG=1 and the
 * card's DEVNAME. */
static bool uevent_is_ours(const struct BackendState *p, const char *buf,
						   size_t n)
{
	char dev[24];
	snprintf(dev, sizeof dev, "DEVNAME=dri/card%d", p->card_index);
	bool drm = false, hotplug = false, card = false;
	for (size_t at = 0; at < n;) {
		const char *s = buf + at;
		const size_t len = strnlen(s, n - at);
		drm     |= strcmp(s, "SUBSYSTEM=drm") == 0;
		hotplug |= strcmp(s, "HOTPLUG=1") == 0;
		card    |= strcmp(s, dev) == 0;
		at += len + 1;
	}
	return drm && hotplug && card;
}

/* Whether a uevent is a GPU coming or going: a DRM card node added or
 * removed -- a dock's GPU, an external one -- of any card, this one's
 * included, but for the GPU a move to another kernel driver takes away
 * and brings back (app_gpu_driver_owns()), whose card goes and comes with
 * each move. Untested: no GPU here can be plugged (TODO). */
static bool uevent_is_gpu(const App *a, const char *buf, size_t n)
{
	bool drm = false, card = false, change = false, moved = false;
	for (size_t at = 0; at < n;) {
		const char *s = buf + at;
		const size_t len = strnlen(s, n - at);
		drm    |= strcmp(s, "SUBSYSTEM=drm") == 0;
		card   |= strncmp(s, "DEVNAME=dri/card", 16) == 0;
		change |= strcmp(s, "ACTION=add") == 0 ||
				  strcmp(s, "ACTION=remove") == 0;
		if (strncmp(s, "DEVPATH=", 8) == 0)
			moved = app_gpu_driver_owns(a, s + 8);
		at += len + 1;
	}
	return drm && card && change && !moved;
}

/* A slot that was this connector and saw these same modes, as the lists
 * name them -- a size and a refresh, and the same preferred: a monitor
 * plugged back in, whose slot can be live again without being written. Not
 * byte for byte: a probe after a hotplug can carry other type bits. */
static bool modes_alike(const KmsOutput *x, const KmsOutput *y)
{
	if (x->mode_count != y->mode_count || x->mode_pref != y->mode_pref)
		return false;
	for (int i = 0; i < x->mode_count; i++)
		if (!mode_same(&x->modes[i], &y->modes[i]))
			return false;
	return true;
}

static int slot_held(const struct BackendState *p, const KmsOutput *o,
					 uint32_t gone)
{
	const int count = (int)fnl_atomic_load_u32(&p->output_count);
	for (int s = count - 1; s >= 0; s--) {
		const KmsOutput *h = &p->outputs[s];
		if (((gone >> s) & 1u) && h->connector_id == o->connector_id &&
			modes_alike(h, o))
			return s;
	}
	return -1;
}

/* The live slot that is this connector, or -1. */
static int slot_live(const struct BackendState *p, uint32_t connector_id,
					 uint32_t gone)
{
	const int count = (int)fnl_atomic_load_u32(&p->output_count);
	for (int s = 0; s < count; s++)
		if (!((gone >> s) & 1u) && p->outputs[s].connector_id == connector_id)
			return s;
	return -1;
}

/* Where the window goes when its output is gone: the first output not in
 * `skip` -- those gone, and those away -- nor refused, at its preferred
 * mode or else its first that is not; -1 when there is none. */
static int sel_elsewhere(const struct BackendState *p, uint32_t skip)
{
	const int count = (int)fnl_atomic_load_u32(&p->output_count);
	for (int o = 0; o < count; o++) {
		if ((skip >> o) & 1u)
			continue;
		int m = p->outputs[o].mode_pref;
		for (int i = 0; kms_sel_is_refused(p, KMS_SEL(o, m)) &&
						i < p->outputs[o].mode_count; i++)
			m = i;
		if (!kms_sel_is_refused(p, KMS_SEL(o, m)))
			return KMS_SEL(o, m);
	}
	return -1;
}

/* The earliest time an output now away counts as gone; 0 with none away. */
static double away_next(const struct BackendState *p)
{
	double next = 0.0;
	for (int s = 0; s < KMS_MAX_OUTPUTS; s++)
		if (((p->outputs_away >> s) & 1u) &&
			(next == 0.0 || p->away_until[s] < next))
			next = p->away_until[s];
	return next;
}

/* The card's connectors read again, on a uevent and when an output has
 * been away its time. A slot whose monitor went is away first -- still
 * listed, the window still drawing there if it was -- and gone only once
 * it has been away KMS_AWAY_LIT_S, if the window was on it, or KMS_AWAY_S:
 * a monitor leaves the bus for a moment as it falls asleep and as it wakes.
 * One back with the modes it had is the output it was and was never
 * unplugged, its mode set again if the window is on it; with others it is
 * another monitor on that connector. A monitor that came is a slot live
 * again where it is the one the slot held, and a new slot otherwise. The
 * window leaves an output that went for the first that is neither gone nor
 * away; with none it stays and draws nothing (kms_window_on_screen()), and
 * the first output to come back has it. */
static void outputs_rescan(App *a)
{
	struct BackendState *p = a->backend_state;
	uint32_t ids[KMS_MAX_CONNECTORS];
	/* A list the kernel would not give is no connector read, but the away
	 * still run out below, or the pump would wake for them for ever. */
	int n = card_connectors(p->card_fd, ids);
	const bool listed_all = n >= 0 && n < KMS_MAX_CONNECTORS;
	if (n < 0)
		n = 0;
	uint32_t gone = fnl_atomic_load_u32(&p->outputs_gone);
	const uint32_t was_gone = gone;
	uint32_t away = p->outputs_away;
	bool came = false, back = false;
	int revived_cur = 0;
	/* A connector the card no longer lists -- a DisplayPort MST sink's,
	 * which goes with its monitor -- is unplugged too, away or not; unless
	 * the list was cut at KMS_MAX_CONNECTORS, when not being on it says
	 * nothing. */
	const int count = listed_all
					? (int)fnl_atomic_load_u32(&p->output_count) : 0;
	for (int s = 0; s < count; s++) {
		bool listed = false;
		for (int i = 0; i < n && !listed; i++)
			listed = ids[i] == p->outputs[s].connector_id;
		if (!listed && !((gone >> s) & 1u)) {
			gone |= 1u << s;
			fnl_log(FNL_LOG_INFO, "KMS: %s unplugged", p->outputs[s].name);
		}
	}
	for (int i = 0; i < n; i++) {
		KmsOutput o;
		uint32_t type = 0;
		bool read;
		const bool on = output_read(p->card_fd, ids[i], &o, &type, &read);
		if (!read)
			continue;
		int live = slot_live(p, ids[i], gone);
		if (live >= 0 && on && ((away >> live) & 1u)) {
			away &= ~(1u << live);
			if (modes_alike(&p->outputs[live], &o)) {
				back = true;
				revived_cur |= live == p->out_cur;
				continue;
			}
			gone |= 1u << live;
			fnl_log(FNL_LOG_INFO, "KMS: %s unplugged", p->outputs[live].name);
			live = -1;
		}
		if (live >= 0 && !on) {
			if (!((away >> live) & 1u)) {
				away |= 1u << live;
				p->away_until[live] = now_seconds() +
					(live == p->out_cur ? KMS_AWAY_LIT_S : KMS_AWAY_S);
				if (!p->away_said)
					fnl_log(FNL_LOG_INFO, "KMS: %s left; an output counts "
							"as unplugged once it has been away %g s, %g s "
							"if the window is on it, since a monitor leaves "
							"the bus as it sleeps and as it wakes",
							p->outputs[live].name, KMS_AWAY_S,
							KMS_AWAY_LIT_S);
				p->away_said = true;
			}
			continue;
		}
		if (live >= 0 || !on)
			continue;
		int s = slot_held(p, &o, gone);
		const bool again = s >= 0;
		if (again) {
			gone &= ~(1u << s);
			revived_cur |= s == p->out_cur;
		} else {
			s = (int)fnl_atomic_load_u32(&p->output_count);
			if (s >= KMS_MAX_OUTPUTS) {
				if (!p->slots_warned)
					fnl_log(FNL_LOG_WARN, "KMS: %s plugged in, and no "
							"slot is left for it: %d monitors have been "
							"seen", o.name, KMS_MAX_OUTPUTS);
				p->slots_warned = true;
				continue;
			}
			p->outputs[s] = o;
			fnl_atomic_store_u32(&p->output_count, (uint32_t)s + 1);
		}
		came = true;
		fnl_log(FNL_LOG_INFO, "KMS: %s plugged in%s, %d mode%s",
				p->outputs[s].name, again ? " again" : "",
				p->outputs[s].mode_count,
				p->outputs[s].mode_count == 1 ? "" : "s");
	}
	/* Away too long, or gone some other way meanwhile: unplugged. */
	const double now = now_seconds();
	for (int s = 0; s < KMS_MAX_OUTPUTS; s++) {
		if (!((away >> s) & 1u))
			continue;
		if ((gone >> s) & 1u) {
			away &= ~(1u << s);
		} else if (now >= p->away_until[s]) {
			away &= ~(1u << s);
			gone |= 1u << s;
			fnl_log(FNL_LOG_INFO, "KMS: %s unplugged", p->outputs[s].name);
		}
	}
	p->outputs_away = away;
	/* A move owed to an output away: made now it is back, dropped if it is
	 * gone instead. */
	int owed = -1;
	if (p->away_owed >= 0 && !((away >> p->away_owed) & 1u)) {
		if ((gone >> p->away_owed) & 1u)
			fnl_log(FNL_LOG_INFO, "KMS: %s did not come back; the window "
					"stays where it is", p->outputs[p->away_owed].name);
		else
			owed = p->away_owed;
		p->away_owed = -1;
	}
	const bool waiting = (gone >> p->out_cur) & 1u;
	if (gone == was_gone && !came && owed < 0 && !(back && waiting) &&
		!revived_cur)
		return;
	fnl_atomic_store_u32(&p->outputs_gone, gone);

	if (owed >= 0)
		output_pick(a, owed);
	if ((gone >> p->out_cur) & 1u) {
		const int to = sel_elsewhere(p, gone | away);
		if (to >= 0)
			sel_set(a, to);
		else if (away)
			fnl_log(FNL_LOG_INFO, "KMS: every output left is away; the "
					"window waits for one to come back");
		else
			fnl_log(FNL_LOG_WARN, "KMS: no output left to show the window "
					"on; it waits for one");
	} else if (owed < 0 && (revived_cur || ((was_gone >> p->out_cur) & 1u))) {
		/* Back on the output it was on: its CRTC may have gone dark with
		 * the monitor, so the mode is set again -- the display path's
		 * surface made again, the copy path's next frame. */
		fnl_atomic_store_u32(&p->scan_resets,
							 fnl_atomic_load_u32(&p->scan_resets) + 1);
		app_surface_remake(a);
		cursor_moved(a);
		publish_mode(a);
	}
	/* The lists changed under the titlebar. */
	fnl_invalidate(a);
}

/* ---- the seam ------------------------------------------------------------ */

static FnlResult kms_open(App *a)
{
	struct BackendState *p = calloc(1, sizeof *p);
	if (!p)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the KMS backend state");
	a->backend_state = p;
	p->card_fd = p->wake_fd = p->epoll_fd = p->inotify_fd = -1;
	p->uevent_fd = -1;
	p->disp_out = p->surf_sel = p->scan_sel = -1;
	p->away_owed = -1;

	FNL_TRY(loop_wake_create(&p->wake_fd));
	FNL_TRY(open_output(a));
	cursor_init(p);

	p->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (p->inotify_fd < 0)
		return fnl_fail(FNL_ERR_SYSTEM, "inotify_init1() failed: %s",
						strerror(errno));
	/* Watched before the scan, so a device that appears between the two is
	 * reported rather than missed; input_add() takes a node once. */
	if (inotify_add_watch(p->inotify_fd, "/dev/input",
						  IN_CREATE | IN_ATTRIB) < 0)
		fnl_log(FNL_LOG_WARN, "KMS: /dev/input is not watched (%s); a "
				"device plugged in later is not read", strerror(errno));
	FNL_TRY(loop_epoll_create(&p->epoll_fd, p->inotify_fd, p->wake_fd));
	uevent_open(p);

	keymap_load(p);

	/* The window is the output, and nothing will ever say otherwise. */
	a->is_maximized = true;
	publish_mode(a);

	inputs_scan(a);
	if (!p->input_count)
		fnl_log(FNL_LOG_WARN, "KMS: no keyboard or pointer this user may "
				"read (/dev/input/event*, group input)");
	/* In the middle, and shown there before anything moves it. */
	p->ptr_x = p->mode_w / 2.0;
	p->ptr_y = p->mode_h / 2.0;
	a->ptr_x = px_to_ptf(p->ptr_x, want_s120(a));
	a->ptr_y = px_to_ptf(p->ptr_y, want_s120(a));
	kms_cursor_changed(a);

	/* The keyboard is ours from the start: there is nobody to give it to. */
	app_publish_focus(a, true);
	app_post_focus(a, true);
	return FNL_OK;
}

/* Innermost first and every step conditional: the failure path out of
 * kms_open() as well. */
static void kms_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p)
		return;
	cursor_free(p);
	if (p->card_fd >= 0) {
		flip_wait(a, FLIP_WAIT_MS);
		p->flip_pending = false;
		crtcs_restore(p);
		scan_release(p, &p->scan_old);
		scan_release(p, &p->scan);
		scan_parked_tidy(p, true);
	}
	for (int i = 0; i < p->input_count; i++)
		close(p->inputs[i].fd);
	p->input_count = 0;
	if (p->inotify_fd >= 0)
		close(p->inotify_fd);
	if (p->uevent_fd >= 0)
		close(p->uevent_fd);
	if (p->card_fd >= 0)
		close(p->card_fd);
	/* The two a signal handler may still be writing to: see wl_close(). */
	if (!a->installed_signals) {
		if (p->epoll_fd >= 0) close(p->epoll_fd);
		if (p->wake_fd  >= 0) close(p->wake_fd);
	}
	free(p);
	a->backend_state = NULL;
}

/* ---- a move to another GPU --------------------------------------------------
 *
 * The renderer lets the display go and takes it again, or goes from it to
 * the copy path or back (surface_kms.c), and the display path sets the
 * output's CRTC its own way in between: so the copy path's next frame sets
 * its mode afresh rather than flipping onto a CRTC it no longer set. The
 * pointer needs nothing: its plane was measured whole after a move each
 * way (aspire's HD 530, i915), with nothing done for it. Render thread.
 *
 * And the node's event queue is handed over empty: the next renderer may
 * lend the descriptor to a driver that reads it (events_drain()), so a
 * flip of ours still pending is waited out first, longer than a frame
 * would. */
static void kms_present_window_renew(App *a)
{
	struct BackendState *p = a->backend_state;
	fnl_atomic_store_u32(&p->scan_resets,
						 fnl_atomic_load_u32(&p->scan_resets) + 1);
	if (p->flip_pending && !flip_wait(a, FLIP_DRAIN_MS))
		fnl_log(FNL_LOG_WARN, "KMS: a flip's event did not come in %d ms; "
				"a driver lent the node may read it", FLIP_DRAIN_MS);
	p->flip_pending = false;
	events_drain(p);
}

static void kms_pump(App *a)
{
	struct BackendState *p = a->backend_state;
	struct epoll_event evs[16];
	/* An output away is gone once its time is up, with or without another
	 * uevent to say so. */
	const double away_at = away_next(p);
	int wait_ms = p->cursor_recheck > 0 ? CURSOR_RECHECK_MS : -1;
	if (p->outputs_away) {
		const int ms = loop_wait_ms(true, away_at);
		if (wait_ms < 0 || ms < wait_ms)
			wait_ms = ms;
	}
	const int n = epoll_wait(p->epoll_fd, evs, 16, wait_ms);
	if (p->cursor_recheck > 0)
		cursor_recheck(a);
	if (p->outputs_away && now_seconds() >= away_at)
		outputs_rescan(a);
	for (int i = 0; i < n; i++) {
		const int fd = evs[i].data.fd;
		if (fd == p->wake_fd) {
			loop_wake_drain(p->wake_fd);
			sel_refused_seen(a);
		} else if (fd == p->inotify_fd) {
			inputs_hotplug(a);
		} else if (fd == p->uevent_fd) {
			/* Every uevent there is, drained; one rescan for any number of
			 * this card's. */
			char buf[4096];
			bool ours = false, gpu = false;
			ssize_t got;
			while ((got = recv(p->uevent_fd, buf, sizeof buf - 1, 0)) > 0) {
				buf[got] = '\0';
				ours |= uevent_is_ours(p, buf, (size_t)got);
				gpu |= uevent_is_gpu(a, buf, (size_t)got);
			}
			if (gpu)
				app_gpu_hotplug(a);
			/* An overrun lost uevents nobody can read now; one of them may
			 * have been ours. */
			ours |= got < 0 && errno == ENOBUFS;
			if (ours)
				outputs_rescan(a);
		} else {
			KmsInput *d = input_by_fd(p, fd);
			if (d && !input_read(a, d))
				input_remove(a, d);
		}
	}
}

static void kms_wakeup(App *a)
{
	if (a->backend_state)
		loop_wake(a->backend_state->wake_fd);
}

/* The output is ours for as long as the program runs -- the keyboard is
 * grabbed, so nobody switches the console away from under it -- and for as
 * long as its monitor is plugged in: with it gone and none to move to, the
 * window draws nothing until one comes (outputs_rescan()). */
static bool kms_window_on_screen(const App *a)
{
	const struct BackendState *p = a->backend_state;
	const int out = KMS_SEL_OUT(fnl_atomic_load_u32(&p->sel_want));
	return !((fnl_atomic_load_u32(&p->outputs_gone) >> out) & 1u);
}

/* Nothing can change the size but a mode (set_mode), so the answer is the
 * size there is. */
static void kms_request_size(App *a, int w_pt, int h_pt)
{
	(void)w_pt; (void)h_pt;
	publish_mode(a);
}

/* Fullscreen is the titlebar going away; the output stays as it was. */
static void kms_set_fullscreen(App *a, bool on)
{
	if (on == a->is_fullscreen)
		return;
	a->is_fullscreen = on;
	publish_mode(a);
	app_mark_urgent(a);
}

/* Maximized is all there is, and the publish says so again. */
static void kms_set_maximized(App *a, bool on)
{
	(void)on;
	publish_mode(a);
}

static void kms_update_scale(App *a)
{
	publish_mode(a);
}

/* Nothing to move or size the window with, and nobody to hand a drag to. */
static void kms_begin_resize(App *a, Edge edge)
{
	(void)a; (void)edge;
}

static void kms_capture(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->captured)
		return;
	p->captured = true;
	a->pointer_captured = true;
	for (int i = 0; i < p->input_count; i++)
		p->inputs[i].have_last = false;
	kms_cursor_changed(a);
	app_reticle_start(a);
	app_reticle_publish(a);
}

/* The pointer comes back where the reticle is. */
static void kms_release(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->captured)
		return;
	int cx, cy;
	app_content_px_rect(a, &cx, &cy, NULL, NULL);
	p->ptr_x = clampd(cx + a->cur_px, 0.0, p->mode_w - 1.0);
	p->ptr_y = clampd(cy + a->cur_py, 0.0, p->mode_h - 1.0);
	p->captured = false;
	a->pointer_captured = false;
	for (int i = 0; i < p->input_count; i++)
		p->inputs[i].have_last = false;
	pointer_moved(a);
	app_reticle_publish(a);
}

/* There is no other program to share a clipboard with, so it is this
 * window's own: a paste is what was copied here last, or no text. */
static void kms_clipboard_set(App *a)
{
	(void)a;
}

static void kms_clipboard_request(App *a)
{
	app_clipboard_deliver(a, a->clip_out, a->clip_out ? a->clip_out_len : 0);
}

static void kms_apply_size_bounds(App *a)
{
	(void)a;
}

/* Not chosen inside a session that has a window server, even one that
 * refused: the display is that server's, and a program that took the
 * console from under it -- where a dead server had let go of the DRM node
 * -- would be doing what nobody asked. Named, it is still tried
 * (FNL_BACKEND=kms). */
static const char *kms_absent(void)
{
	static const char *const SERVERS[] = {
		"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
	};
	for (size_t i = 0; i < sizeof SERVERS / sizeof SERVERS[0]; i++) {
		const char *v = getenv(SERVERS[i]);
		if (v && v[0])
			return "a window server's session";
	}
	return NULL;
}

const FnlBackendVtbl fnl_backend_kms = {
	.name             = "KMS",
	.id               = BACKEND_KMS,
	.key              = "kms",
	.absent           = kms_absent,
	.host             = backend_noop_host,
	.shared_driver    = backend_noop_shared_driver,
	.gpu_drivers      = gpu_driver_list,
	.gpu_driver_set   = gpu_driver_bind,
	.gpu_driver_held  = gpu_driver_held,
	/* No resize edges: there is nothing to resize. */
	.grab_ring        = 0,
	/* The present is the throttle; a display swapchain flips on vblank. */
	.invites          = false,
	/* The window is the mode: no minimize or maximize, and its size is the
	 * output's mode, chosen with set_mode and set_output. */
	.is_output        = true,
	.open             = kms_open,
	.close            = kms_close,
	.pump             = kms_pump,
	.wakeup           = kms_wakeup,
	.check_error      = backend_noop_check_error,
	.flush            = backend_noop,
	.present_pixels   = kms_present_pixels,
	.set_geometry     = backend_noop_set_geometry,
	.ack_configure    = backend_noop_ack_configure,
	.frame_request    = backend_noop,
	.frame_cancel     = backend_noop,
	.present_window_renew = kms_present_window_renew,
	.present_window_hold  = backend_noop,
	.frame_wait       = backend_frame_wait_shown,
	.window_on_screen = kms_window_on_screen,
	.apply_scale      = backend_noop_apply_scale,
	.request_size     = kms_request_size,
	.nudge_size       = backend_noop,
	.set_fullscreen   = kms_set_fullscreen,
	.set_maximized    = kms_set_maximized,
	.minimize         = backend_noop,
	.modes            = kms_modes,
	.outputs          = kms_outputs,
	.set_mode         = kms_set_mode,
	.set_output       = kms_set_output,
	.begin_move       = backend_noop,
	.begin_resize     = kms_begin_resize,
	.apply_size_bounds = kms_apply_size_bounds,
	.buffer_granularity = backend_noop_buffer_granularity,
	.update_scale     = kms_update_scale,
	.capture_pointer  = kms_capture,
	.release_pointer  = kms_release,
	.cursor_changed   = kms_cursor_changed,
	.text_input_changed = kms_text_input_changed,
	.clipboard_set      = kms_clipboard_set,
	.clipboard_request  = kms_clipboard_request,
	/* No window server to tell: the bar is the library's own. */
	.set_title          = backend_noop_set_title,
};
