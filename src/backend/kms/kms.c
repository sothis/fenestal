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
#include "backend/kms/kms_atomic.h"
#include "backend/kms/kms_driver.h"
#include "backend/evdev_input.h"
#include "backend/loop.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#endif

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

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
#define CONNECTOR_NAME_COUNT                                                   \
	(sizeof CONNECTOR_NAMES / sizeof CONNECTOR_NAMES[0])

/* How often the outputs and the input devices are looked at again where
 * there is no uevent socket or inotify to say they changed (FreeBSD): an
 * EDID probe for each connector is tens of milliseconds, on the pump's
 * thread, which is why it is not a tighter loop. */
#define KMS_RESCAN_S 2.0

/* How long a refused selection is left alone before it is tried again: a
 * link still training or a CRTC busy a moment ago comes right, while a
 * refusal that is real costs one attempt a second rather than a blank
 * window for the window's life. */
#define KMS_SCAN_RETRY_S 1.0

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
						? CONNECTOR_NAMES[type]
						: "Unknown";
	snprintf(out, size, "%s-%u", t, type_id);
}

/* One connector, with its modes. `count_modes` zero makes the kernel probe
 * the output, which only a master may do; a non-master is given what was
 * last probed. Asked twice, the second time with room, and again if a
 * hotplug between the two changed the count. */
typedef struct {
	struct drm_mode_get_connector info;
	struct drm_mode_modeinfo *modes;
} Connector;

static bool connector_read(int fd, uint32_t id, Connector *c)
{
	c->modes = NULL;
	for (int tries = 0; tries < 3; tries++) {
		memset(&c->info, 0, sizeof c->info);
		c->info.connector_id = id;
		/* A probe that fails on a retry breaks to the free below: a
		 * return here would leave the last allocation to nobody. */
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c->info) < 0)
			break;
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
		const unsigned int was_mhz          = kms_refresh_mhz(was);
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
		int j                            = i;
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
		o->mode_pref                = o->mode_count - 1;
	}
	free(list);
	return o->mode_count > 0;
}

/* The pump's own figures for a selection. */
static void mode_adopt(struct BackendState *p, int sel)
{
	const struct drm_mode_modeinfo *m = kms_sel_mode(p, sel);
	p->out_cur                        = KMS_SEL_OUT(sel);
	p->mode_cur                       = KMS_SEL_MODE(sel);
	p->mode_w                         = m->hdisplay;
	p->mode_h                         = m->vdisplay;
	p->refresh_mhz                    = kms_refresh_mhz(m);
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
#define KMS_MAX_CRTCS 16

/* The connector's property ids this backend reads by name, resolved once when
 * the slot is read and never again: an id belongs to the connector object for
 * its life, so a hotplug on it does not invalidate one, and a slot the card
 * stops listing is never read. One GETPROPERTY per property here; every later
 * read by name -- the cursor's CRTC probe, the HDR statement, an output gone
 * dark -- is then one OBJ_GETPROPERTIES. */
static void output_props_read(int fd, KmsOutput *o)
{
	uint32_t ids[64];
	uint64_t vals[64];
	struct drm_mode_obj_get_properties gp = {
		.props_ptr       = (uintptr_t)ids,
		.prop_values_ptr = (uintptr_t)vals,
		.count_props     = 64,
		.obj_id          = o->connector_id,
		.obj_type        = DRM_MODE_OBJECT_CONNECTOR,
	};
	if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
		return;
	const uint32_t n = gp.count_props < 64 ? gp.count_props : 64;
	for (uint32_t i = 0; i < n; i++) {
		struct drm_mode_get_property prop = { .prop_id = ids[i] };
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
			continue;
		if (strcmp(prop.name, "CRTC_ID") == 0)
			o->prop[KMS_PROP_CRTC] = ids[i];
		else if (strcmp(prop.name, "DPMS") == 0)
			o->prop[KMS_PROP_DPMS] = ids[i];
		else if (strcmp(prop.name, "HDR_OUTPUT_METADATA") == 0)
			o->prop[KMS_PROP_HDR] = ids[i];
		else if (strcmp(prop.name, "EDID") == 0)
			o->prop[KMS_PROP_EDID] = ids[i];
		else if (strcmp(prop.name, "Colorspace") == 0)
			o->prop[KMS_PROP_COLORSPACE] = ids[i];
		else if (strcmp(prop.name, "max bpc") == 0)
			o->prop[KMS_PROP_MAX_BPC] = ids[i];
	}
}

/* The CRTC driving a connector now, or 0: what lit it, the console or a mode
 * set since, Vulkan's included -- which picks its own CRTC for a display, so
 * the one read at open is a guess for any output not lit then. The cached
 * CRTC_ID is asked in one query, which an atomic commit sets and the encoder
 * path may leave stale or zero; a driver that keeps no property, or names
 * none, answers through the encoder it names. */
static uint32_t connector_live_crtc(int fd, const KmsOutput *o)
{
	const uint32_t want = o->prop[KMS_PROP_CRTC];
	uint32_t ids[64];
	uint64_t vals[64];
	struct drm_mode_obj_get_properties gp = {
		.props_ptr       = (uintptr_t)ids,
		.prop_values_ptr = (uintptr_t)vals,
		.count_props     = 64,
		.obj_id          = o->connector_id,
		.obj_type        = DRM_MODE_OBJECT_CONNECTOR,
	};
	if (want && drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) == 0) {
		const uint32_t n = gp.count_props < 64 ? gp.count_props : 64;
		for (uint32_t i = 0; i < n; i++)
			if (ids[i] == want) {
				if (vals[i])
					return (uint32_t)vals[i];
				break; /* bound to no CRTC: the encoder answers */
			}
	}
	struct drm_mode_modeinfo one;
	struct drm_mode_get_connector c = {
		.connector_id = o->connector_id,
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
static uint32_t connector_crtc(int fd, KmsOutput *o)
{
	const uint32_t live = connector_live_crtc(fd, o);
	if (live)
		return live;
	struct drm_mode_modeinfo one;
	uint32_t enc[KMS_MAX_ENCODERS];
	struct drm_mode_get_connector c = {
		.connector_id   = o->connector_id,
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
	const uint32_t ncrtc =
		r.count_crtcs < KMS_MAX_CRTCS ? r.count_crtcs : KMS_MAX_CRTCS;
	const uint32_t nenc = c.count_encoders < KMS_MAX_ENCODERS
							  ? c.count_encoders
							  : KMS_MAX_ENCODERS;
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

/* The CRTCs, by index into GETRESOURCES' list, that one of the connector's
 * encoders can be driven by; 0 where the kernel would not say. */
static uint32_t connector_possible_crtcs(int fd, const KmsOutput *o)
{
	struct drm_mode_modeinfo one;
	uint32_t enc[KMS_MAX_ENCODERS];
	struct drm_mode_get_connector c = {
		.connector_id   = o->connector_id,
		.count_modes    = 1,
		.modes_ptr      = (uintptr_t)&one,
		.count_encoders = KMS_MAX_ENCODERS,
		.encoders_ptr   = (uintptr_t)enc,
	};
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
		return 0;
	const uint32_t nenc = c.count_encoders < KMS_MAX_ENCODERS
							  ? c.count_encoders
							  : KMS_MAX_ENCODERS;
	uint32_t mask = 0;
	for (uint32_t i = 0; i < nenc; i++) {
		struct drm_mode_get_encoder e = { .encoder_id = enc[i] };
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0)
			mask |= e.possible_crtcs;
	}
	return mask;
}

/* Whether this output can scan out PQ, into KmsOutput::hdr_ok and max_lum
 * (defined with the other connector reads below). */
static void output_hdr_probe(int fd, KmsOutput *o);

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
	errno          = 0; /* a read given up after its tries sets none */
	const bool got = connector_read(fd, id, &c);
	*read          = got || errno == ENOENT;
	if (!got)
		return false;
	bool ok = false;
	if (c.info.connection == 1 && c.info.count_modes > 0) {
		memset(o, 0, sizeof *o);
		connector_name(c.info.connector_type, c.info.connector_type_id, o->name,
					   sizeof o->name);
		o->connector_id = c.info.connector_id;
		*type           = c.info.connector_type;
		ok              = modes_read(o, c.modes, c.info.count_modes);
	}
	free(c.modes);
	if (ok) {
		output_props_read(fd, o);
		o->crtc_id = connector_crtc(fd, o);
		output_hdr_probe(fd, o);
	}
	return ok;
}

/* The connected outputs of one card, into the state, and which the window
 * opens on: the connector FNL_KMS_OUTPUT names, or the first connected
 * panel, or the first connected anything. -1 when the card has none, or
 * none of that name. */
static int card_read_outputs(struct BackendHostState *hs, int fd,
							 const char *want)
{
	uint32_t ids[KMS_MAX_CONNECTORS];
	const int n = card_connectors(fd, ids);
	if (n <= 0)
		return -1;

	int count = 0, best = -1, best_rank = 0;
	for (int i = 0; i < n && count < KMS_MAX_OUTPUTS; i++) {
		KmsOutput *o  = &hs->outputs[count];
		uint32_t type = 0;
		bool read;
		if (!output_read(fd, ids[i], o, &type, &read))
			continue;
		const int rank = want ? (strcmp(o->name, want) == 0 ? 3 : 0)
						 : connector_internal(type) ? 2
													: 1;
		if (rank > best_rank) {
			best      = count;
			best_rank = rank;
		}
		count++;
	}
	fnl_atomic_store_u32(&hs->output_count, (uint32_t)count);
	return best;
}

/* What each output's CRTC shows as the open finds it -- the console, as a
 * rule -- and at which mode, kept to be given back at close
 * (crtcs_restore()). A framebuffer's id names it to every client of the
 * device, the kernel's own included, so the console's can be set from
 * here. One entry a CRTC: a second connector cloned onto one would be
 * detached by a set naming the first alone. */
static void crtcs_save(struct BackendHostState *hs, int fd)
{
	hs->saved_count  = 0;
	const int count = (int)fnl_atomic_load_u32(&hs->output_count);
	for (int i = 0; i < count; i++) {
		struct drm_mode_crtc c = {
			.crtc_id = connector_live_crtc(fd, &hs->outputs[i]),
		};
		if (!c.crtc_id || drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0)
			continue;
		bool seen = false;
		for (int j = 0; j < hs->saved_count && !seen; j++)
			seen = hs->saved[j].crtc_id == c.crtc_id;
		if (seen)
			continue;
		KmsCrtcSaved *s = &hs->saved[hs->saved_count++];
		s->connector_id = hs->outputs[i].connector_id;
		s->crtc_id      = c.crtc_id;
		s->fb_id        = c.fb_id;
		s->mode_valid   = c.mode_valid != 0;
		s->mode         = c.mode;
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
	struct BackendHostState *hs = p->host;
	uint32_t lit =
		p->scan_sel >= 0
			? connector_live_crtc(hs->card_fd, kms_out(p, p->scan_sel))
			: 0;
	for (int i = 0; i < hs->saved_count; i++) {
		const KmsCrtcSaved *s    = &hs->saved[i];
		const bool shown         = s->fb_id && s->mode_valid;
		if (hs->atomic) {
			/* Atomic where the card has it: the console's mode and frame on
			 * its CRTC, or the CRTC off. A failure falls through to the
			 * legacy set, which is what this did before. */
			const bool ok = shown
				? kms_atomic_set(hs->atomic, s->connector_id, s->crtc_id,
								 &s->mode, s->fb_id)
				: kms_atomic_crtc_off(hs->atomic, s->crtc_id);
			if (ok) {
				if (s->crtc_id == lit)
					lit = 0;
				continue;
			}
		}
		struct drm_mode_crtc set = {
			.set_connectors_ptr = (uintptr_t)&s->connector_id,
			.count_connectors   = shown ? 1 : 0,
			.crtc_id            = s->crtc_id,
			.fb_id              = shown ? s->fb_id : 0,
			.mode_valid         = shown ? 1 : 0,
			.mode               = s->mode,
		};
		if (drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
			fnl_log(FNL_LOG_WARN,
					"KMS: the console would not come back on "
					"connector %u (%s); off instead",
					s->connector_id, strerror(errno));
			struct drm_mode_crtc off = { .crtc_id = s->crtc_id };
			drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
		}
		if (s->crtc_id == lit)
			lit = 0;
	}
	if (lit) {
		if (hs->atomic && kms_atomic_crtc_off(hs->atomic, lit))
			return;
		struct drm_mode_crtc off = { .crtc_id = lit };
		drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
	}
}

/* The primary node of the first card with an output to give, held open and
 * master. Master is taken by being the first to open a card nobody holds;
 * SET_MASTER is then a no-op that says so, and fails while an X server or a
 * compositor has it. */
static FnlResult open_output(struct BackendHostState *hs)
{
	const char *want = getenv("FNL_KMS_OUTPUT");
	if (want && !want[0])
		want = NULL;

	for (int card = 0; card < KMS_MAX_CARDS; card++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", card);
		const int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		const int first = card_read_outputs(hs, fd, want);
		if (first < 0) {
			close(fd);
			continue;
		}
		if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0) {
			close(fd);
			return fnl_fail(FNL_ERR_NO_DISPLAY,
							"%s on %s is in another program's hands -- an X "
							"server or a compositor; this backend needs the "
							"display to itself",
							hs->outputs[first].name, path);
		}
		hs->card_fd    = fd;
		hs->card_index = card;
		/* The output the window opens on, kept where kms_window_open() reads
		 * it: the host's open runs before the window exists, so it cannot
		 * set the window's selection itself. */
		hs->first_output = first;
		crtcs_save(hs, fd);
		char others[KMS_MAX_OUTPUTS * 34] = "";
		const int count = (int)fnl_atomic_load_u32(&hs->output_count);
		const KmsOutput *fo = &hs->outputs[first];
		const struct drm_mode_modeinfo *fm = &fo->modes[fo->mode_pref];
		for (int i = 0; i < count; i++)
			if (i != first) {
				const size_t at = strlen(others);
				snprintf(others + at, sizeof others - at, "%s%s",
						 at ? ", " : "", hs->outputs[i].name);
			}
		fnl_log(FNL_LOG_INFO,
				"KMS: %s on %s, %dx%d at %u.%03u Hz, %d "
				"mode%s on offer; other outputs: %s",
				fo->name, path, fm->hdisplay, fm->vdisplay,
				kms_refresh_mhz(fm) / 1000, kms_refresh_mhz(fm) % 1000,
				fo->mode_count, fo->mode_count == 1 ? "" : "s",
				others[0] ? others : "none");
		return FNL_OK;
	}
	if (want)
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"no connected output named %s "
						"(FNL_KMS_OUTPUT) on any card",
						want);
	return fnl_fail(FNL_ERR_NO_DISPLAY,
					"no connected output on any card "
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

/* What a key does to the modifier state, by the keysym on its first level:
 * this backend keeps the state itself (on a bare output nobody reports it),
 * so it reads the roles from the layout and not from the key's place. */
enum {
	ROLE_SHIFT  = 1 << 0,
	ROLE_CTRL   = 1 << 1,
	ROLE_ALT    = 1 << 2,
	ROLE_SUPER  = 1 << 3,
	ROLE_CAPS   = 1 << 4,
	ROLE_NUM    = 1 << 5,
	ROLE_LEVEL3 = 1 << 6,
};

#define KEYSYM_ISO_LEVEL3_SHIFT 0xfe03u

static uint8_t role_of_keysym(uint32_t k)
{
	switch (k) {
	case 0xffe1: case 0xffe2:            return ROLE_SHIFT;  /* Shift_L/R */
	case 0xffe3: case 0xffe4:            return ROLE_CTRL;   /* Control_L/R */
	case 0xffe5:                         return ROLE_CAPS;   /* Caps_Lock */
	case 0xffe6:                         return ROLE_CAPS;   /* Shift_Lock */
	case 0xffe7: case 0xffe8:            /* Meta_L/R */
	case 0xffe9: case 0xffea:            return ROLE_ALT;    /* Alt_L/R */
	case 0xffeb: case 0xffec:            return ROLE_SUPER;  /* Super_L/R */
	case 0xff7f:                         return ROLE_NUM;    /* Num_Lock */
	case KEYSYM_ISO_LEVEL3_SHIFT:        return ROLE_LEVEL3;
	default:                             return 0;
	}
}

/* The roles of every key in every group, indexed by evdev code (the keymap
 * is by X code, which is that plus 8). Built where the keymap is loaded and
 * not per key: the defaults are the keys' usual codes, for a keymap that
 * names none; a code the keymap gives a plain symbol keeps only what that
 * keysym says, so Caps Lock under ctrl:nocaps is Control, where one whose
 * plain level is NoSymbol keeps its usual code's role (grp:alt_shift_toggle's
 * <LALT> is [NoSymbol, ISO_Next_Group]). */
static void keymap_note_roles(struct BackendHostState *hs)
{
	static const struct { unsigned code; uint8_t role; } DEFAULT[] = {
		{ KEY_LEFTSHIFT, ROLE_SHIFT },  { KEY_RIGHTSHIFT, ROLE_SHIFT },
		{ KEY_LEFTCTRL, ROLE_CTRL },    { KEY_RIGHTCTRL, ROLE_CTRL },
		{ KEY_LEFTALT, ROLE_ALT },      { KEY_RIGHTALT, ROLE_ALT },
		{ KEY_LEFTMETA, ROLE_SUPER },   { KEY_RIGHTMETA, ROLE_SUPER },
		{ KEY_CAPSLOCK, ROLE_CAPS },    { KEY_NUMLOCK, ROLE_NUM },
	};
	for (unsigned g = 0; g < KEYMAP_GROUPS; g++)
		for (unsigned c = 0; c < KEYMAP_CODES; c++)
			hs->role[g][c] = 0;
	for (size_t i = 0; i < sizeof DEFAULT / sizeof DEFAULT[0]; i++)
		for (unsigned g = 0; g < KEYMAP_GROUPS; g++)
			hs->role[g][DEFAULT[i].code] = DEFAULT[i].role;
	for (unsigned code = 0; code + 8u < KEYMAP_CODES; code++) {
		const unsigned x = code + 8u;
		bool any = false;
		for (unsigned g = 0; g < KEYMAP_GROUPS; g++)
			for (unsigned l = 0; l < KEYMAP_LEVELS; l++)
				any = any || hs->keymap.sym[x][g][l];
		if (!any)
			continue;
		for (unsigned g = 0; g < KEYMAP_GROUPS; g++) {
			/* A group with nothing on the key takes the first group's, as
			 * typing does (keymap_lookup()). A level 0 the layout leaves
			 * NoSymbol says nothing about the key -- grp:alt_shift_toggle's
			 * <LALT> is [NoSymbol, ISO_Next_Group] -- so the usual role
			 * stands rather than being replaced by none. */
			bool empty = true;
			for (unsigned l = 0; l < KEYMAP_LEVELS; l++)
				empty = empty && !hs->keymap.sym[x][g][l];
			const uint32_t plain = hs->keymap.sym[x][empty ? 0 : g][0];
			if (plain)
				hs->role[g][code] = role_of_keysym(plain);
		}
	}
}

/* The role of an evdev code in the group typed in; 0 out of the table. */
static uint8_t key_role(const struct BackendHostState *hs, unsigned code)
{
	return code < KEYMAP_CODES ? hs->role[hs->group % KEYMAP_GROUPS][code]
							   : 0;
}

/* "de(nodeadkeys),us caps:none": the names as FNL_KMS_LAYOUT spells them,
 * for the log. The layouts and the variants are comma lists side by side. */
static void keymap_names_show(const KeymapNames *n, char *out, size_t size)
{
	const char *l = n->layout, *v = n->variant;
	size_t at = 0;
	out[0]    = '\0';
	while (*l && at + 1 < size) {
		const char *le = strchr(l, ','), *ve = *v ? strchr(v, ',') : NULL;
		const size_t ln = le ? (size_t)(le - l) : strlen(l);
		const size_t vn = ve ? (size_t)(ve - v) : strlen(v);
		snprintf(out + at, size - at, "%s%.*s%s%.*s%s", at ? "," : "", (int)ln,
				 l, vn ? "(" : "", (int)vn, v, vn ? ")" : "");
		at = strlen(out);
		l += le ? ln + 1 : ln;
		v += ve ? vn + 1 : vn;
	}
	if (n->options[0] && at + 1 < size)
		snprintf(out + at, size - at, " %s", n->options);
}

/* Each group's layout, "de" and "us(intl)", for the line a switch logs:
 * the comma lists side by side, as keymap_names_show() reads them. */
static void keymap_group_names(struct BackendHostState *hs,
							   const KeymapNames *n)
{
	const char *l = n->layout, *v = n->variant;
	memset(hs->group_name, 0, sizeof hs->group_name);
	for (unsigned g = 0; g < KEYMAP_GROUPS && *l; g++) {
		const char *le = strchr(l, ','), *ve = *v ? strchr(v, ',') : NULL;
		const size_t ln = le ? (size_t)(le - l) : strlen(l);
		const size_t vn = ve ? (size_t)(ve - v) : strlen(v);
		snprintf(hs->group_name[g], sizeof hs->group_name[g], "%.*s%s%.*s%s",
				 (int)ln, l, vn ? "(" : "", (int)vn, v, vn ? ")" : "");
		l += le ? ln + 1 : ln;
		v += ve ? vn + 1 : vn;
	}
}

static void keymap_load(struct BackendHostState *hs)
{
	KeymapNames names;
	const char *from = "the console's";
	const char *spec = getenv("FNL_KMS_LAYOUT");
	bool named       = spec && spec[0];
	if (named && !keymap_names_parse(&names, spec)) {
		fnl_log(FNL_LOG_WARN,
				"FNL_KMS_LAYOUT=%s is not <layout>[(<variant>)]"
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
	else if (!keymap_load_names(&hs->keymap, &names, NULL, detail,
								sizeof detail))
		why = detail;
	if (why) {
		memset(&hs->keymap, 0, sizeof hs->keymap);
		fnl_log(FNL_LOG_WARN,
				"no keymap for layout %s: %s, so text entry "
				"will type nothing",
				shown, why);
	} else {
		fnl_log(FNL_LOG_INFO, "KMS: layout %s, %s%s%s", shown, from,
				detail[0] ? "; left out: " : "", detail);
	}
	keymap_note_roles(hs);
	keymap_group_names(hs, &names);
	hs->group  = 0;
	hs->groups = keymap_groups(&hs->keymap);
}

/* ---- the cursor ------------------------------------------------------------
 *
 * Nothing draws a pointer on a bare output, so this does, on the CRTC's
 * cursor plane: a buffer the kernel moves without a frame being drawn. Drawn
 * here from text, three shapes, because there is no theme to load one from;
 * the rest of FnlCursor is the arrow, which says less and nothing wrong. A
 * '#' is black, a '+' white, anything else clear. */

static const char *const GLYPH_ARROW[] = {
	"#           ", "##          ", "#+#         ", "#++#        ",
	"#+++#       ", "#++++#      ", "#+++++#     ", "#++++++#    ",
	"#+++++++#   ", "#++++++++#  ", "#+++++++++# ", "#++++++#####",
	"#+++#++#    ", "#++# #++#   ", "#+#  #++#   ", "##    #++#  ",
	"#     #++#  ", "       #++# ", "       ###  ",
};

static const char *const GLYPH_IBEAM[] = {
	"#### ####", "#++#+#++#", "####+####", "   #+#   ",
	"   #+#   ", "   #+#   ", "   #+#   ", "   #+#   ",
	"   #+#   ", "   #+#   ", "   #+#   ", "   #+#   ",
	"   #+#   ", "####+####", "#++#+#++#", "#### ####",
};

static const char *const GLYPH_CROSS[] = {
	"      #+#      ", "      #+#      ", "      #+#      ", "      #+#      ",
	"      #+#      ", "      #+#      ", "#######+#######", "+++++++++++++++",
	"#######+#######", "      #+#      ", "      #+#      ", "      #+#      ",
	"      #+#      ", "      #+#      ", "      #+#      ",
};

typedef struct {
	const char *const *rows;
	int count;
	int hot_x, hot_y;
} GlyphArt;

#define ART(g) (g), (int)(sizeof(g) / sizeof(g)[0])
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

/* Said once per output: a move to another CRTC asks afresh, since one
 * refusal at a move is the render thread's mode set having just switched
 * this one off. */
static void cursor_warn(struct BackendState *p, uint32_t crtc, const char *what)
{
	if (p->cursor_warned && p->cursor_warned_crtc == crtc)
		return;
	p->cursor_warned      = true;
	p->cursor_warned_crtc = crtc;
	fnl_log(FNL_LOG_WARN, "KMS: %s (%s), so the pointer is not shown", what,
			strerror(errno));
}

static bool glyph_make(struct BackendState *p, KmsGlyph g)
{
	const GlyphArt *art            = &GLYPHS[g];
	struct drm_mode_create_dumb cd = {
		.width  = p->cursor_w,
		.height = p->cursor_h,
		.bpp    = 32,
	};
	if (FNL_FAULT("kms-no-cursor") ||
		drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return false;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	unsigned char *px           = MAP_FAILED;
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
		px = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
				  p->host->card_fd, (off_t)md.offset);
	if (px == MAP_FAILED) {
		struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
		drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		return false;
	}
	memset(px, 0, (size_t)cd.size);
	for (int y = 0; y < art->count && (uint32_t)y < cd.height; y++) {
		const char *row = art->rows[y];
		for (int x = 0; row[x] && (uint32_t)x < cd.width; x++) {
			const uint32_t v = row[x] == '#'   ? 0xff000000u
							   : row[x] == '+' ? 0xffffffffu
											   : 0u;
			memcpy(px + (size_t)y * cd.pitch + (size_t)x * 4u, &v, 4);
		}
	}
	munmap(px, (size_t)cd.size);
	p->glyph_handle[g] = cd.handle;
	/* The atomic commit names a framebuffer, not a raw buffer: one wrapped
	 * around it, ARGB8888 as a cursor plane takes. A driver without atomic
	 * needs none -- the legacy cursor ioctl wrapped the handle itself -- and
	 * a refusal here leaves the glyph to that ioctl. */
	if (p->host->atomic) {
		struct drm_mode_fb_cmd2 fb = {
			.width        = p->cursor_w,
			.height       = p->cursor_h,
			.pixel_format = DRM_FORMAT_ARGB8888,
			.handles      = { cd.handle },
			.pitches      = { cd.pitch },
		};
		if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_ADDFB2, &fb) == 0)
			p->glyph_fb[g] = fb.fb_id;
	}
	return true;
}

/* Whether this window's cursor goes by atomic commits on `crtc`: the driver
 * took the capability, the CRTC has a cursor plane, and none of this window's
 * cursor commits has been refused. A cursor plane is a CRTC's, so a window
 * that moves to another output asks afresh. */
static bool cursor_atomic(const struct BackendState *p, uint32_t crtc)
{
	return p->host->atomic && !p->cursor_legacy &&
		   kms_atomic_cursor_at(p->host->atomic, crtc);
}

/* `g` (or nothing) on the CRTC's cursor plane through an atomic commit, its
 * hot spot at (x, y). False where the kernel refused it, and the legacy ioctl
 * then answers this one; three refused with nothing taken make the legacy
 * ioctl the window's for its life, one line saying so. A refusal can be a
 * race -- the render thread's mode set switching the CRTC off between the
 * lit check and the commit -- so one is not the plane's last word. */
static bool cursor_atomic_send(struct BackendState *p, uint32_t crtc,
							   KmsGlyph g, int x, int y, bool wait)
{
	bool ok = g == KMS_GLYPH_NONE
				  ? kms_atomic_cursor_off(p->host->atomic, crtc, wait)
				  : kms_atomic_cursor_set(p->host->atomic, crtc,
										  p->glyph_fb[g], x, y, p->cursor_w,
										  p->cursor_h, wait);
	/* The plane refusing, forced (DESIGN *Forcing the paths that never
	 * run*): three in a row and the legacy ioctl is the window's. */
	if (FNL_FAULT("kms-cursor-legacy"))
		ok = false;
	if (ok)
		p->cursor_refused = 0;
	else if (++p->cursor_refused >= 3) {
		p->cursor_legacy = true;
		fnl_log(FNL_LOG_INFO,
				"KMS: the cursor plane took no atomic commit (%s); the "
				"legacy cursor ioctl is used",
				strerror(errno));
	}
	return ok;
}

/* After a switch the pump looks for the new output's CRTC this often and
 * this many times: the render thread's mode set is a frame or two away,
 * and a device rebuild on the display path some hundred milliseconds. */
#define CURSOR_RECHECK_MS 50
#define CURSOR_RECHECKS 100

/* Take the cursor off the CRTC it was last shown on, if it shows one. */
static void cursor_hide(struct BackendState *p, bool wait)
{
	const uint32_t crtc = p->cursor_on;
	if (crtc && p->glyph_shown != KMS_GLYPH_NONE &&
		p->glyph_shown != KMS_GLYPH_UNSET) {
		if (cursor_atomic(p, crtc) &&
			kms_atomic_cursor_off(p->host->atomic, crtc, wait))
			return;
		struct drm_mode_cursor c = { .flags   = DRM_MODE_CURSOR_BO,
									 .crtc_id = crtc };
		drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_CURSOR, &c);
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
 * A shape and its place go in one commit, and only a move when it stands. An
 * output that refused a cursor once is not asked again: the plane is either
 * there or not, and the ioctl would otherwise fail on every motion. */
static void cursor_show(App *a, KmsGlyph g)
{
	struct BackendState *p = a->backend_state;
	/* One seat, one pointer: only the window the pointer is over shows one,
	 * and the pump now rechecks every window (its output may come back) rather
	 * than the one fnl_pump() was handed -- so a recheck for a window that is
	 * not the focus must show nothing, not an arrow on an output no pointer is
	 * on. focus_set() moves this from one window to the next. */
	if (p->host->focus != a)
		g = KMS_GLYPH_NONE;
	/* On the display path -- the window holds the display -- the commit is made
	 * synchronous, so none of ours is left pending for Mesa's own NONBLOCK
	 * page-flip commit to be refused on (kms_atomic_cursor_set()'s header). The
	 * copy path flips under this backend's lock, where the lag f035cb9 took
	 * out would come back, so there it stays NONBLOCK. */
	const bool wait = fnl_atomic_load_ptr(&p->host->display_owner) == a;
	const int px = (int)p->ptr_x, py = (int)p->ptr_y;
	/* The CRTC driving the output now, asked of the kernel until there is
	 * one: after a switch the render thread sets the mode, and the display
	 * path's driver chooses the CRTC itself. Until then the open's guess,
	 * and a shape set afresh once the answer is another CRTC. The walk is a
	 * property query per connector on the thread that reads input, so a
	 * motion report makes it no more often than the recheck does. */
	if (!p->cursor_crtc) {
		const double now = now_seconds();
		if (now - p->cursor_probe_at >= (double)CURSOR_RECHECK_MS * 1e-3) {
			p->cursor_probe_at = now;
			p->cursor_crtc     = connector_live_crtc(
				p->host->card_fd, &p->host->outputs[p->out_cur]);
		}
	}
	const uint32_t crtc =
		p->cursor_crtc ? p->cursor_crtc : p->host->outputs[p->out_cur].crtc_id;
	if (!crtc)
		return; /* nothing can drive the output */
	/* Refused on this output: not asked again per motion; another CRTC is
	 * another plane's to answer. */
	if (p->cursor_warned && p->cursor_warned_crtc == crtc)
		return;
	/* The CRTC's lit state, read once per CRTC and not per motion: a GETCRTC
	 * on every mouse move is a kernel ioctl that would block the pump behind
	 * the render thread's mode set. Re-checked when the CRTC is first known
	 * or changes (cursor_lit_for), and again by cursor_recheck() after a
	 * switch, when the render thread's mode set may have just lit it. */
	if (crtc != p->cursor_lit_for) {
		p->cursor_lit_for = crtc;
		p->cursor_lit     = crtc_lit(p->host->card_fd, crtc);
	}
	/* Nothing to send: answered before the ioctl below, which a pad's
	 * sub-pixel motion -- four reports in five move no whole pixel -- would
	 * otherwise pay for every time. */
	if (crtc != p->cursor_on) {
		cursor_hide(p, wait);
		p->glyph_shown = KMS_GLYPH_UNSET;
	}
	if (g == p->glyph_shown &&
		(g == KMS_GLYPH_NONE || (px == p->cursor_at_x && py == p->cursor_at_y)))
		return;
	/* Not yet lit: the recheck sets the shape once a CRTC drives the output,
	 * and a refusal from one that is off is not the output refusing a
	 * cursor, which would be taken as the last word (cursor_warn()). The
	 * lit state is cached per CRTC change (cursor_lit), so this is a flag
	 * test on the hot path, not a GETCRTC per motion. */
	if (!p->cursor_lit) {
		if (!p->cursor_recheck) {
			p->cursor_recheck    = CURSOR_RECHECKS;
			p->cursor_recheck_at = now_seconds();
		}
		return;
	}

	/* The glyph's buffer, made once per shape and shared by both paths. */
	if (g != KMS_GLYPH_NONE && !p->glyph_handle[g] && !glyph_make(p, g)) {
		cursor_warn(p, crtc, "no buffer for the cursor plane");
		p->glyph_shown = g;
		return;
	}
	const int cx = g != KMS_GLYPH_NONE ? px - GLYPHS[g].hot_x : 0;
	const int cy = g != KMS_GLYPH_NONE ? py - GLYPHS[g].hot_y : 0;
	/* An atomic commit names the framebuffer around that buffer; a glyph the
	 * kernel would not wrap, or a plane that refused a commit, is the legacy
	 * ioctl's, which wraps the buffer itself. */
	if (cursor_atomic(p, crtc) && (g == KMS_GLYPH_NONE || p->glyph_fb[g]) &&
		cursor_atomic_send(p, crtc, g, cx, cy, wait)) {
		if (!p->cursor_atomic_said) {
			p->cursor_atomic_said = true;
			fnl_log(FNL_LOG_INFO, "KMS: the cursor plane on CRTC %u by "
								  "atomic commits", crtc);
		}
		p->glyph_shown = g;
		p->cursor_on   = crtc;
		p->cursor_at_x = px;
		p->cursor_at_y = py;
		return;
	}

	struct drm_mode_cursor c = { .crtc_id = crtc };
	if (g != p->glyph_shown) {
		c.flags |= DRM_MODE_CURSOR_BO;
		if (g != KMS_GLYPH_NONE) {
			c.handle = p->glyph_handle[g];
			c.width  = p->cursor_w;
			c.height = p->cursor_h;
		}
	}
	if (g != KMS_GLYPH_NONE) {
		c.flags |= DRM_MODE_CURSOR_MOVE;
		c.x = cx;
		c.y = cy;
	}
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_CURSOR, &c) < 0) {
		cursor_warn(p, crtc, "the output took no cursor");
		p->glyph_shown = g;
		return;
	}
	p->glyph_shown = g;
	p->cursor_on   = crtc;
	p->cursor_at_x = px;
	p->cursor_at_y = py;
}

static void cursor_update(App *a, Zone z, Edge edge)
{
	const DecoCursor c = deco_cursor(a, z, edge);
	cursor_show(a, c.kind == DECO_CURSOR_HIDDEN ? KMS_GLYPH_NONE
				   : c.kind == DECO_CURSOR_EDGE ? KMS_GLYPH_ARROW
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
	struct BackendHostState *hs = p->host;
	struct drm_get_cap w = { .capability = DRM_CAP_CURSOR_WIDTH };
	struct drm_get_cap h = { .capability = DRM_CAP_CURSOR_HEIGHT };
	p->cursor_w = drm_ioctl(hs->card_fd, DRM_IOCTL_GET_CAP, &w) == 0 && w.value
					  ? (uint32_t)w.value
					  : 64u;
	p->cursor_h = drm_ioctl(hs->card_fd, DRM_IOCTL_GET_CAP, &h) == 0 && h.value
					  ? (uint32_t)h.value
					  : 64u;
	p->glyph_shown = KMS_GLYPH_UNSET;
}

/* The window left an output for the current one: the cursor comes off the
 * CRTC it was on and is set afresh, on whichever drives the new output, at
 * the next show -- a CRTC has its own cursor plane. */
static void cursor_moved(App *a)
{
	struct BackendState *p = a->backend_state;
	cursor_hide(p, fnl_atomic_load_ptr(&p->host->display_owner) == a);
	p->cursor_crtc      = 0;
	p->cursor_on        = 0;
	p->glyph_shown      = KMS_GLYPH_UNSET;
	p->cursor_recheck    = CURSOR_RECHECKS;
	p->cursor_recheck_at = now_seconds();
	p->cursor_probe_at  = 0.0; /* the next motion asks at once */
	p->cursor_lit_for   = 0;
	/* Another output's plane answers for itself: a refusal here is asked
	 * afresh. */
	p->cursor_warned = false;
}

/* After a switch, a pump turn that nothing woke: until the kernel names
 * the CRTC driving the new output, the cursor waits on the open's guess --
 * and a pointer nobody moves would leave it there. Asked on the recheck's
 * 50 ms clock, not on pump turns: a caller pumping faster than that would
 * spend the budget before the render thread's mode set. */
static void cursor_recheck(App *a)
{
	struct BackendState *p = a->backend_state;
	struct BackendHostState *hs = p->host;
	if (p->cursor_recheck <= 0)
		return;
	const double now = now_seconds();
	if (now - p->cursor_recheck_at < (double)CURSOR_RECHECK_MS * 1e-3)
		return;
	p->cursor_recheck_at = now;
	p->cursor_recheck--;
	const uint32_t live =
		connector_live_crtc(hs->card_fd, &hs->outputs[p->out_cur]);
	if (!live)
		return;
	p->cursor_recheck = 0;
	/* The render thread may have lit the CRTC since the last check: make the
	 * next cursor_show() read its lit state again. */
	p->cursor_lit_for = 0;
	if (live != p->cursor_on) {
		p->cursor_crtc = live;
		kms_cursor_changed(a);
	}
}

static void cursor_free(struct BackendState *p)
{
	if (p->host->card_fd < 0)
		return;
	cursor_hide(p, false);
	for (int g = 0; g < KMS_GLYPH_COUNT; g++) {
		if (p->glyph_fb[g]) {
			uint32_t fb = p->glyph_fb[g];
			drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_RMFB, &fb);
			p->glyph_fb[g] = 0;
		}
		if (p->glyph_handle[g]) {
			struct drm_mode_destroy_dumb dd = { .handle = p->glyph_handle[g] };
			drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
			p->glyph_handle[g] = 0;
		}
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
	const int s120         = kms_scale120(a);
	const int w = px_to_pt(p->mode_w, s120), h = px_to_pt(p->mode_h, s120);
	const KmsOutput *o = &p->host->outputs[p->out_cur];
	app_publish_size(a, w, h, s120);
	app_publish_refresh(a, p->refresh_mhz);
	app_publish_output(a, o->name, w, h);
	/* The headroom is the output's, and moves with it: the EDID's peak in
	 * SDR-white units where it can show PQ, 1.0 where it cannot. Said here,
	 * where every adoption of a selection passes, rather than once at the
	 * window's open -- a move to an HDR output would otherwise keep the old
	 * output's figure, or the renderer's 1000-nit default. */
	float peak = 1.0f;
	if (o->hdr_ok)
		peak = (float)(o->max_lum ? o->max_lum : 1000u) / 203.0f;
	app_publish_peak(a, peak);
}

/* ---- the keyboard -------------------------------------------------------- */

static void held_set(struct BackendState *p, unsigned code, bool on)
{
	if (code >= 8u * sizeof p->host->down)
		return;
	if (on)
		p->host->down[code / 8] |= (uint8_t)(1u << (code % 8));
	else
		p->host->down[code / 8] &= (uint8_t)~(1u << (code % 8));
}

/* The window that has the keyboard and the pointer, or `a` before one does:
 * one seat, one pointer, and the window under the pointer is both the
 * pointer's and the keyboard's (DESIGN *A second window*). */
static App *kms_focus(App *a)
{
	App *f = app_backend_host(a)->focus;
	return f ? f : a;
}

/* The modifiers, worked out from the keys held and their roles in the group
 * typed in: on a bare output nobody reports them, and the locks are this
 * backend's own toggles. */
static KeyMods kb_mods(const struct BackendState *p)
{
	struct BackendHostState *hs = p->host;
	bool shift = false, ctrl = false, alt = false, super = false, l3 = false;
	for (unsigned i = 0; i < sizeof hs->down; i++) {
		if (!hs->down[i])
			continue;
		for (unsigned b = 0; b < 8; b++) {
			if (!((hs->down[i] >> b) & 1u))
				continue;
			const uint8_t r = key_role(hs, i * 8u + b);
			shift = shift || (r & ROLE_SHIFT);
			ctrl  = ctrl || (r & ROLE_CTRL);
			alt   = alt || (r & ROLE_ALT);
			super = super || (r & ROLE_SUPER);
			l3    = l3 || (r & ROLE_LEVEL3);
		}
	}
	return (KeyMods){
		.shift   = shift,
		.lock    = hs->caps_on,
		.num     = hs->num_on,
		.level3  = l3,
		.command = alt || ctrl || super,
		.group   = hs->group,
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
	struct BackendHostState *hs = p->host;
	const KeyMods mods = kb_mods(p);
	unsigned to;
	if (!keymap_group_key(&hs->keymap, code + 8u, &mods, hs->groups, &to))
		return false;
	if (to != hs->group) {
		hs->group = to;
		fnl_log(FNL_LOG_INFO, "KMS: typing in group %u of %u, %s", to + 1,
				hs->groups, hs->group_name[to][0] ? hs->group_name[to] : "?");
	}
	return true;
}

static void key_type(App *a, unsigned code, bool repeat)
{
	struct BackendState *p = a->backend_state;
	struct BackendHostState *hs = p->host;
	const KeyMods mods     = kb_mods(p);
	const uint32_t was     = hs->compose.dead;
	const uint32_t c =
		keymap_type(&hs->keymap, &hs->compose, code + 8u, &mods, repeat);
	app_text(a, c);
	if (hs->compose.dead != was) {
		char shown[8];
		keymap_composing(&hs->compose, shown);
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
	app_backend_host(a)->compose.dead = 0;
	app_compose_set(a, NULL);
}

/* The lock lights, on every keyboard: the toggles are kept here, and a
 * keyboard shows none of its own without being told. */
static void leds_write(struct BackendState *p)
{
	struct BackendHostState *hs = p->host;
	struct input_event ev[3];
	memset(ev, 0, sizeof ev);
	ev[0].type  = EV_LED;
	ev[0].code  = LED_CAPSL;
	ev[0].value = hs->caps_on;
	ev[1].type  = EV_LED;
	ev[1].code  = LED_NUML;
	ev[1].value = hs->num_on;
	ev[2].type  = EV_SYN;
	ev[2].code  = SYN_REPORT;
	for (int i = 0; i < hs->input_count; i++)
		if (hs->inputs[i].kind & KMS_KEYBOARD) {
			ssize_t n = write(hs->inputs[i].fd, ev, sizeof ev);
			(void)n; /* a read-only open has no lights */
		}
}

/* The kernel repeats every held key itself, as value 2, modifiers too; a
 * repeat goes through app_key_repeat() and only while text entry is on, and
 * types again where the layout says the key repeats. */
static void on_key(App *a, unsigned code, int value)
{
	a = kms_focus(a);
	struct BackendState *p = a->backend_state;
	struct BackendHostState *hs = p->host;
	const FnlKey k         = key_from_evdev(code);

	if (value == 2) {
		if (app_text_input(a) &&
			keymap_held_repeats(&hs->keymap, code + 8u, k)) {
			app_key_repeat(a, k);
			key_type(a, code, true);
		}
		return;
	}
	const bool pressed = value != 0;
	/* The group key's level from the modifiers held before it, as XKB
	 * takes it: counted with its own, Left Alt under grp:alts_toggle (or
	 * Shift under shifts_toggle) would switch the group at every press. */
	const bool grouped = pressed && group_switch(p, code);
	held_set(p, code, pressed);
	/* By the layout's keysym, as the modifiers are: under ctrl:nocaps the
	 * Caps Lock key is Control and toggles nothing. */
	const uint8_t role = key_role(hs, code);
	if (pressed && !grouped && (role & (ROLE_CAPS | ROLE_NUM))) {
		if (role & ROLE_CAPS)
			hs->caps_on = !hs->caps_on;
		else
			hs->num_on = !hs->num_on;
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
	struct BackendHostState *hs = p->host;
	uint8_t bits[sizeof hs->down];
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
	const int s120         = want_s120(a);
	a->ptr_x               = px_to_ptf(p->ptr_x, s120);
	a->ptr_y               = px_to_ptf(p->ptr_y, s120);
	app_pointer_motion(a, a->ptr_x, a->ptr_y);
	deco_drag(a, a->ptr_x, a->ptr_y);
	Edge edge;
	const Zone z = deco_pointer_at(a, a->ptr_x, a->ptr_y, &edge);
	cursor_update(a, z, edge);
}

/* A window whose output is gone is on no desktop the pointer moves over;
 * kms_window_on_screen() is below, the pump's answer. */
static bool kms_window_on_screen(const App *a);

/* The desktop the windows make, laid left to right each at its output's
 * mode; a window whose output is gone is in none of it. */
static void desktop_size(App *a, double *w, double *h)
{
	double tw = 0.0, th = 0.0;
	for (App *o = a->host->windows; o; o = o->next) {
		const struct BackendState *q = o->backend_state;
		if (!q || !kms_window_on_screen(o))
			continue;
		tw += q->mode_w;
		if (q->mode_h > th)
			th = q->mode_h;
	}
	*w = tw > 0.0 ? tw : 1.0;
	*h = th > 0.0 ? th : 1.0;
}

/* The window whose output holds the desktop point, and the point in that
 * output; the last window past the end. NULL where none is on screen. */
static App *desktop_win(App *a, double gx, double gy, double *lx, double *ly)
{
	App *last = NULL;
	double ox = 0.0;
	for (App *o = a->host->windows; o; o = o->next) {
		const struct BackendState *q = o->backend_state;
		if (!q || !kms_window_on_screen(o))
			continue;
		if (gx < ox + q->mode_w) {
			*lx = gx - ox;
			*ly = gy;
			return o;
		}
		ox += q->mode_w;
		last = o;
	}
	if (!last)
		return NULL;
	*lx = last->backend_state->mode_w - 1.0;
	*ly = gy;
	return last;
}

/* The keyboard and pointer move to `win`: the old window's cursor comes off
 * and it is told it lost the focus, the new one the other way. */
static void focus_set(App *a, App *win)
{
	struct BackendHostState *hs = app_backend_host(a);
	if (hs->focus == win)
		return;
	if (hs->focus) {
		cursor_show(hs->focus, KMS_GLYPH_NONE);
		/* What it holds is the host's, not either window's: the window
		 * that takes the focus is seeded below, so a key held across the
		 * move is released there and not left held here for good. */
		app_keys_clear(hs->focus);
		app_publish_focus(hs->focus, false);
		app_post_focus(hs->focus, false);
	}
	hs->focus = win;
	if (win) {
		for (unsigned code = 0; code < 8u * sizeof hs->down; code++)
			if ((hs->down[code / 8] >> (code % 8)) & 1u)
				app_key_seed(win, key_from_evdev(code));
		app_publish_focus(win, true);
		app_post_focus(win, true);
	}
}

/* The pointer put where the desktop says, the window under it told, and the
 * focus followed; `a` is any window, to reach the host's list. */
static void pointer_route(App *a)
{
	struct BackendHostState *hs = app_backend_host(a);
	double tw, th;
	desktop_size(a, &tw, &th);
	hs->gx = clampd(hs->gx, 0.0, tw - 1.0);
	hs->gy = clampd(hs->gy, 0.0, th - 1.0);
	double lx, ly;
	App *win = desktop_win(a, hs->gx, hs->gy, &lx, &ly);
	if (!win)
		return;
	struct BackendState *p = win->backend_state;
	p->ptr_x = clampd(lx, 0.0, p->mode_w - 1.0);
	p->ptr_y = clampd(ly, 0.0, p->mode_h - 1.0);
	focus_set(a, win);
	pointer_moved(win);
}

/* A delta in pixels: the pointer moves, or a captured one reports it. */
static void pointer_by(App *a, double dx, double dy)
{
	struct BackendHostState *hs = app_backend_host(a);
	App *win                   = hs->focus ? hs->focus : a;
	struct BackendState *p     = win->backend_state;
	if (dx == 0.0 && dy == 0.0)
		return;
	if (p && p->captured) {
		const int s120 = want_s120(win);
		app_pointer_relative(win, px_to_ptf(dx, s120), px_to_ptf(dy, s120));
		if (p->captured)
			app_reticle_move(win, dx, dy);
		return;
	}
	hs->gx += dx;
	hs->gy += dy;
	pointer_route(a);
}

/* An absolute device's report, onto the desktop. The axes are the device's
 * to state, so every difference of them is taken as a double, where no pair
 * of ints can overflow it. */
static void pointer_to(App *a, KmsInput *d)
{
	struct BackendHostState *hs = app_backend_host(a);
	App *win                   = hs->focus ? hs->focus : a;
	struct BackendState *p     = win->backend_state;
	const double sx = d->x_max > d->x_min ? (double)d->x_max - d->x_min : 1;
	const double sy = d->y_max > d->y_min ? (double)d->y_max - d->y_min : 1;
	if (p && p->captured) {
		/* Taken in the device's units, which last_x holds exactly: against
		 * a pixel position kept as an int, every report would lose up to a
		 * pixel on each axis, always the same way. */
		const double kx = (p->mode_w - 1) / sx, ky = (p->mode_h - 1) / sy;
		if (d->have_last)
			pointer_by(a, ((double)d->ax - d->last_x) * kx,
					   ((double)d->ay - d->last_y) * ky);
		d->last_x    = d->ax;
		d->last_y    = d->ay;
		d->have_last = true;
		return;
	}
	double tw, th;
	desktop_size(a, &tw, &th);
	hs->gx = ((double)d->ax - d->x_min) * (tw - 1.0) / sx;
	hs->gy = ((double)d->ay - d->y_min) * (th - 1.0) / sy;
	pointer_route(a);
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
	return hi > lo ? PAD_PX_PER_MM * PAD_ASSUMED_MM / ((double)hi - lo) : 1.0;
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
		d->last_x    = d->ax;
		d->last_y    = d->ay;
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
	case BTN_LEFT: b = FNL_BUTTON_LEFT; break;
	case BTN_RIGHT: b = FNL_BUTTON_RIGHT; break;
	case BTN_MIDDLE: b = FNL_BUTTON_MIDDLE; break;
	case BTN_TOUCH:
		/* A pad's finger, or a touchscreen's, which is a left press. */
		if (d->kind & KMS_PAD) {
			d->touching  = value != 0;
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
	default: return;
	}
	report_motion(a, d);
	App *win = kms_focus(a);
	app_pointer_button(win, b, value != 0, win->ptr_x, win->ptr_y, ms);
}

/* The wheel's detents: positive is towards the user, which is down. */
static void on_wheel(App *a, KmsInput *d, double dx, double dy)
{
	report_motion(a, d);
	App *win = kms_focus(a);
	app_pointer_scroll(win, win->ptr_x, win->ptr_y, dx, dy);
}

/* ---- the devices --------------------------------------------------------- */

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

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

static KmsInput *input_by_fd(struct BackendHostState *hs, int fd)
{
	for (int i = 0; i < hs->input_count; i++)
		if (hs->inputs[i].fd == fd)
			return &hs->inputs[i];
	return NULL;
}

static bool input_open_already(const struct BackendState *p, const char *node)
{
	for (int i = 0; i < p->host->input_count; i++)
		if (!strcmp(p->host->inputs[i].node, node))
			return true;
	return false;
}

/* A node of /dev/input, read if it is something this backend reads. Quiet on
 * failure: most nodes are not, and a new one is often not yet ours to open
 * until udev has set its group, which inotify reports as a second event. */
static void input_add(App *a, const char *node)
{
	struct BackendState *p = a->backend_state;
	struct BackendHostState *hs = p->host;
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
	if (!kind || hs->input_count >= KMS_MAX_INPUTS) {
		if (kind)
			fnl_log(FNL_LOG_WARN,
					"KMS: more than %d input devices; %s is "
					"not read",
					KMS_MAX_INPUTS, node);
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

	KmsInput *d = &hs->inputs[hs->input_count];
	memset(d, 0, sizeof *d);
	d->fd   = fd;
	d->kind = kind;
	snprintf(d->node, sizeof d->node, "%s", node);
	if (kind & (KMS_PAD | KMS_TABLET)) {
		struct input_absinfo x, y;
		memset(&x, 0, sizeof x);
		memset(&y, 0, sizeof y);
		ioctl(fd, EVIOCGABS(ABS_X), &x);
		ioctl(fd, EVIOCGABS(ABS_Y), &y);
		d->x_min = x.minimum;
		d->x_max = x.maximum;
		d->x_res = x.resolution;
		d->y_min = y.minimum;
		d->y_max = y.maximum;
		d->y_res = y.resolution;
		d->ax    = x.value;
		d->ay    = y.value;
	}
	if (!loop_add(&hs->loop, fd)) {
		close(fd);
		return;
	}
	hs->input_count++;

	if (kind & KMS_KEYBOARD) {
		/* Longs, not bytes: test_bit() reads them a word at a time, and
		 * the array holds LED_CNT bits whatever the word size. */
		unsigned long leds[LONGS(LED_CNT)];
		memset(leds, 0, sizeof leds);
		/* The locks as the first keyboard shows them, which is whatever
		 * the console left them at. */
		if (!hs->leds_known && ioctl(fd, EVIOCGLED(sizeof leds), leds) >= 0) {
			hs->caps_on    = test_bit(leds, LED_CAPSL);
			hs->num_on     = test_bit(leds, LED_NUML);
			hs->leds_known = true;
		}
		/* The focused window's held set, where typing goes -- not whatever
		 * window the pump was handed (the SYN_DROPPED path below does the
		 * same). */
		keys_seed(kms_focus(a), d);
	}
}

/* A device that has gone. Its keys go with it, all of them: the held set is
 * one for every keyboard, and a key held on another is rare enough to lose. */
static void input_remove(App *a, KmsInput *d)
{
	struct BackendState *p = a->backend_state;
	const bool keyboard    = (d->kind & KMS_KEYBOARD) != 0;
	/* Out of the set before the close: epoll drops a closed descriptor by
	 * itself, poll() would report it as invalid for ever. */
	loop_del(&p->host->loop, d->fd);
	close(d->fd);
	*d = p->host->inputs[--p->host->input_count];
	if (keyboard) {
		memset(p->host->down, 0, sizeof p->host->down);
		app_keys_clear(kms_focus(a));
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

#if defined(__linux__)
static void inputs_hotplug(App *a)
{
	struct BackendState *p = a->backend_state;
	_Alignas(struct inotify_event) char buf[4096];
	for (;;) {
		const ssize_t n = read(p->host->inotify_fd, buf, sizeof buf);
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
#endif

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
				d->have_last  = false;
				if (d->kind & KMS_KEYBOARD) {
					memset(app_backend_host(a)->down, 0,
						   sizeof app_backend_host(a)->down);
					App *t = kms_focus(a);
					app_keys_clear(t);
					keys_seed(t, d);
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
				d->ax     = e->value;
				d->ax_new = true;
			} else if (e->code == ABS_Y) {
				d->ay     = e->value;
				d->ay_new = true;
			}
			break;
		default: break;
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
#define FLIP_WAIT_MS 100
#define FLIP_DRAIN_MS 500

/* The kernel's colourimetry enum (drm_connector.h) for BT.2020 RGB, which a
 * PQ frame is in: DRM_MODE_COLORIMETRY_BT2020_CYCC is 8, BT2020_RGB 9, and
 * DEFAULT 0. RGB it must be: the frame is RGB, and 8 would tell the sink to
 * read it as constant-luminance YCbCr. */
#define KMS_COLORIMETRY_BT2020_RGB 9
/* CTA-861's codes for the metadata infoframe: the EOTF and the descriptor
 * type. The kernel's drm_mode.h names the structures only. */
#define KMS_EOTF_SMPTE_ST2084 0x02
#define KMS_STATIC_METADATA_TYPE1 0x01

/* BT.2020's primaries and D65 in the infoframe's units of 0.00002: what a
 * frame scanned out in PQ is mastered in. */
static const uint16_t HDR_PRIMARIES[4][2] = {
	{ 35400, 14600 }, /* red:   0.708, 0.292  */
	{ 8500, 39850 },  /* green: 0.170, 0.797  */
	{ 6550, 2300 },   /* blue:  0.131, 0.046  */
	{ 15635, 16450 }, /* D65:   0.3127, 0.3290 */
};

/* What the pair is made of: XRGB2101010 where the colour the renderer
 * published is PQ -- which is also the format its readback arrives in -- and
 * XRGB8888 otherwise. */
static uint32_t scan_fourcc(App *a)
{
	FnlOutputColor c;
	return fnl_output_color(a, &c) && c.space == FNL_COLOR_PQ
			   ? DRM_FORMAT_XRGB2101010
			   : DRM_FORMAT_XRGB8888;
}

/* A buffer of mode `m`'s size into slot `i` of `s`, in `fourcc`. The 8-bit
 * one goes by the legacy add, which every driver takes; the 10-bit one needs
 * the fourcc, so it goes by ADDFB2. */
static bool scan_make(struct BackendState *p, KmsScan *s,
					  const struct drm_mode_modeinfo *m, int i,
					  uint32_t fourcc)
{
	struct drm_mode_create_dumb cd = {
		.width  = m->hdisplay,
		.height = m->vdisplay,
		.bpp    = 32,
	};
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return false;
	s->handle[i]                = cd.handle;
	s->gen[i]                   = ++p->scan_gen;
	s->pitch                    = cd.pitch;
	s->size                     = (size_t)cd.size;
	s->w                        = m->hdisplay;
	s->h                        = m->vdisplay;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return false;
	void *map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED,
					 p->host->card_fd, (off_t)md.offset);
	if (map == MAP_FAILED)
		return false;
	s->map[i] = map;
	memset(map, 0, s->size);
	s->fourcc = fourcc;
	if (fourcc == DRM_FORMAT_XRGB2101010) {
		struct drm_mode_fb_cmd2 fb = {
			.width        = cd.width,
			.height       = cd.height,
			.pixel_format = fourcc,
			.handles      = { cd.handle },
			.pitches      = { cd.pitch },
		};
		if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
			return false;
		s->fb[i] = fb.fb_id;
		return true;
	}
	struct drm_mode_fb_cmd fb = {
		.width  = cd.width,
		.height = cd.height,
		.pitch  = cd.pitch,
		.bpp    = 32,
		.depth  = 24,
		.handle = cd.handle,
	};
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0)
		return false;
	s->fb[i] = fb.fb_id;
	return true;
}

/* Only of a pair no plane shows. The kernel removes a framebuffer that is
 * shown by disabling its plane, and where the driver refuses a CRTC with
 * no primary plane -- amdgpu -- by disabling the CRTC, which there fails
 * too: a WARNING with a stack in the journal each time (DESIGN *KMS: no
 * window server*). So a pair goes once another is set in its place
 * (scan_retire()), or its CRTC is given back or turned off
 * (kms_window_close()). */
static void scan_free(struct BackendState *p, KmsScan *s)
{
	for (int i = 0; i < 2; i++) {
		if (s->fb[i]) {
			uint32_t fb = s->fb[i];
			drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_RMFB, &fb);
		}
		if (s->map[i])
			munmap(s->map[i], s->size);
		if (s->handle[i]) {
			struct drm_mode_destroy_dumb dd = { .handle = s->handle[i] };
			drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		}
	}
	memset(s, 0, sizeof *s);
}

/* The CRTC whose plane scans out one of `s`'s buffers, or 0. An inactive
 * CRTC counts: DPMS off leaves the framebuffer on the plane, and the
 * kernel removing it there disables the CRTC, which amdgpu refuses. */
static uint32_t scan_shown_on(const struct BackendState *p, const KmsScan *s)
{
	struct BackendHostState *hs = p->host;
	if (!s->fb[0] && !s->fb[1])
		return 0;
	uint32_t crtcs[KMS_MAX_CRTCS];
	struct drm_mode_card_res r;
	memset(&r, 0, sizeof r);
	r.count_crtcs = KMS_MAX_CRTCS;
	r.crtc_id_ptr = (uintptr_t)crtcs;
	if (drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0)
		return 0;
	const uint32_t n =
		r.count_crtcs < KMS_MAX_CRTCS ? r.count_crtcs : KMS_MAX_CRTCS;
	for (uint32_t i = 0; i < n; i++) {
		struct drm_mode_crtc c = { .crtc_id = crtcs[i] };
		if (drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_GETCRTC, &c) == 0 &&
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
			bool darkened = false;
			if (p->host->atomic)
				darkened = kms_atomic_crtc_off(p->host->atomic, crtc);
			if (!darkened) {
				struct drm_mode_crtc dark = { .crtc_id = crtc };
				drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_SETCRTC, &dark);
			}
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
		fnl_log(FNL_LOG_WARN,
				"KMS: %d outputs dark with a frame of ours "
				"each; one is removed while shown",
				KMS_PARKED);
	}
	scan_free(p, s);
}

/* Before a frame of mode `m`: the pair stays while the mode keeps its
 * size, and a frame of that size sets the mode again from it. Another
 * size's pair is made afresh and this one kept as scan_old until the new
 * one is on the plane -- unless an older pair waits there already, which
 * means nothing was set since and this one was never shown, so it goes. */
static void scan_retire(struct BackendState *p,
						const struct drm_mode_modeinfo *m, bool force)
{
	if (!p->scan.map[0] ||
		(!force && p->scan.w == m->hdisplay && p->scan.h == m->vdisplay))
		return;
	if (p->scan_old.map[0]) {
		scan_release(p, &p->scan);
	} else {
		p->scan_old = p->scan;
		memset(&p->scan, 0, sizeof p->scan);
	}
	p->scan_front = -1;
}

/* A page-flip completion, given to the window whose flip it is: the event's
 * user_data was that window's address when the flip was made
 * (kms_scan_show()), so a wait reading another window's event clears the
 * right pending flip and counts the right frame. The list walked is the
 * host's, under the lock, and not Host::windows: this runs on whichever
 * window's render thread read the event, and the registry is the main
 * thread's alone -- a window closing there takes its state, and its App,
 * away under the walk. A stamp naming no window of the list is nobody's
 * now -- the window has closed -- and is dropped. */
static void flip_complete(App *a, const struct drm_event_vblank *v)
{
	const double mono           = (double)v->tv_sec + (double)v->tv_usec * 1e-6;
	struct BackendHostState *hs = a->backend_state->host;
	fnl_mutex_lock(&hs->flip_lock);
	for (struct BackendState *q = hs->flip_windows; q; q = q->flip_next) {
		if ((uintptr_t)q->flip_app != (uintptr_t)v->user_data)
			continue;
		if (q->flip_stale > 0) {
			/* The completion that was given up on (flip_give_up()):
			 * consumed, not reported -- it is older than the frame the
			 * window waits for now. */
			q->flip_stale--;
			break;
		}
		if (!q->flip_pending)
			break; /* nobody's now */
		q->flip_pending = false;
		app_frame_shown(q->flip_app, q->flip_index, mono, !q->flip_async);
		break;
	}
	fnl_mutex_unlock(&hs->flip_lock);
}

/* The pending flip is not waited for after all: given up on, and the
 * completion still to come for it is stale -- consumed by flip_complete()
 * and not reported, where it would otherwise be taken for the next flip's.
 * Under the lock, so a completion another window's thread reads at this
 * moment is one decision with this one. */
static void flip_give_up(App *a)
{
	struct BackendState *p      = a->backend_state;
	struct BackendHostState *hs = p->host;
	fnl_mutex_lock(&hs->flip_lock);
	if (p->flip_pending) {
		p->flip_pending = false;
		p->flip_stale++;
	}
	fnl_mutex_unlock(&hs->flip_lock);
}

/* Wait up to `ms` for the pending flip's completion event; whether it came.
 * The node's events are this thread's alone only while no driver holds a
 * copy of the descriptor: one open file is one event queue, and Mesa's
 * display WSI reads it on a thread of its own for as long as it is lent
 * one (surface_kms.c). Nothing here flips while it is, and events_drain()
 * keeps the two from reading each other's. Between this library's windows
 * the queue is shared too, so a poll and the read it licenses are one
 * thread's (BackendHostState::ev_lock): a completion one window's read
 * takes away is routed to the other by flip_complete(), where a read begun
 * after that drain would block for an event no one still owes. */
static bool flip_wait(App *a, int ms)
{
	struct BackendState *p      = a->backend_state;
	struct BackendHostState *hs = p->host;
	/* One deadline for the whole wait: another window's completions wake the
	 * poll, and a timeout restarted at each would never run out while that
	 * window flips faster than `ms`. */
	const double deadline = now_seconds() + ms / 1000.0;
	for (;;) {
		fnl_mutex_lock(&hs->flip_lock);
		const bool pending = p->flip_pending;
		fnl_mutex_unlock(&hs->flip_lock);
		if (!pending)
			return true;
		struct pollfd pf = { .fd = hs->card_fd, .events = POLLIN };
		fnl_mutex_lock(&hs->ev_lock);
		/* A completion another thread read while this one waited for the
		 * queue cleared this window's flag with it, and there is nothing
		 * left to poll for. */
		fnl_mutex_lock(&hs->flip_lock);
		const bool still = p->flip_pending;
		fnl_mutex_unlock(&hs->flip_lock);
		if (!still) {
			fnl_mutex_unlock(&hs->ev_lock);
			return true;
		}
		const double left = deadline - now_seconds();
		/* Past it, still one look that does not wait: a completion already
		 * queued is read, which is all a wait of 0 ms asks. */
		const int r = poll(&pf, 1, left > 0.0 ? (int)(left * 1000.0) + 1 : 0);
		if (r < 0 && errno == EINTR) {
			fnl_mutex_unlock(&hs->ev_lock);
			continue;
		}
		if (r == 0) {
			/* Timed out: another window's thread may have read this
			 * window's completion while we polled -- the flag is the
			 * answer, not the poll. */
			fnl_mutex_lock(&hs->flip_lock);
			const bool owed = p->flip_pending;
			fnl_mutex_unlock(&hs->flip_lock);
			fnl_mutex_unlock(&hs->ev_lock);
			return !owed;
		}
		if (r < 0) {
			fnl_mutex_unlock(&hs->ev_lock);
			return false;
		}
		_Alignas(struct drm_event) char buf[1024];
		const ssize_t n = read(hs->card_fd, buf, sizeof buf);
		fnl_mutex_unlock(&hs->ev_lock);
		if (n <= 0)
			return false;
		for (ssize_t off = 0; off + (ssize_t)sizeof(struct drm_event) <= n;) {
			const struct drm_event *e =
				(const struct drm_event *)(const void *)(buf + off);
			if (e->type == DRM_EVENT_FLIP_COMPLETE &&
				e->length >= sizeof(struct drm_event_vblank)) {
				/* When it reached the screen, by the kernel's stamp on
				 * CLOCK_MONOTONIC -- the frame clock's -- and which frame
				 * it was (flip_complete()). A mode set's first frame has
				 * no flip and no stamp. */
				const struct drm_event_vblank *v =
					(const struct drm_event_vblank *)(const void *)e;
				flip_complete(a, v);
			}
			if (e->length == 0)
				break;
			off += (ssize_t)e->length;
		}
	}
}

/* Read off every event the node holds now, waiting for none. What the
 * queue holds is nobody's at two points. Before the descriptor is lent to
 * a driver, once the pending flip is waited out: a flip of ours still
 * queued is read by Mesa's wait thread as one of its own images and its
 * user_data -- the window's address, where the driver's is an image's --
 * dereferenced: a segfault inside the driver at the move from OpenGL back
 * to Vulkan. And after a mode set, which the
 * kernel completes only once every flip before it has: the driver's last,
 * in flight when the display was released and it stopped reading, would
 * otherwise count as the first of ours, and the next flip find the CRTC
 * busy and stay refused for good. */
static void events_drain(App *a)
{
	struct BackendHostState *hs = a->backend_state->host;
	const int fd                = hs->card_fd;
	for (;;) {
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		fnl_mutex_lock(&hs->ev_lock);
		const int r = poll(&pf, 1, 0);
		if (r <= 0) {
			fnl_mutex_unlock(&hs->ev_lock);
			if (r < 0 && errno == EINTR)
				continue;
			return;
		}
		_Alignas(struct drm_event) char buf[1024];
		const ssize_t n = read(fd, buf, sizeof buf);
		fnl_mutex_unlock(&hs->ev_lock);
		if (n <= 0)
			return;
		/* A completion is still a window's, and is routed rather than
		 * dropped: another window's mode set would otherwise leave its
		 * pending flip waiting out its bound (flip_complete()). */
		for (ssize_t off = 0; off + (ssize_t)sizeof(struct drm_event) <= n;) {
			const struct drm_event *e =
				(const struct drm_event *)(const void *)(buf + off);
			if (e->type == DRM_EVENT_FLIP_COMPLETE &&
				e->length >= sizeof(struct drm_event_vblank))
				flip_complete(
					a, (const struct drm_event_vblank *)(const void *)e);
			if (e->length == 0)
				break;
			off += (ssize_t)e->length;
		}
	}
}

static void scan_refuse(struct BackendState *p, const char *what)
{
	if (!p->scan_said) {
		fnl_log(FNL_LOG_WARN, "KMS: %s (%s), so no frame reaches %s", what,
				strerror(errno), kms_name(p, p->scan_sel));
		p->scan_said = true;
	}
	p->scan_failed    = true;
	p->scan_failed_at = now_seconds();
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
	if (p->scan_failed) {
		/* A refusal -- the pair's, or a flip that found the CRTC busy --
		 * is not the window's for good. A mode asked for anew is tried,
		 * and so is one refused a while back: a link not yet trained or a
		 * CRTC busy a moment ago comes right, where latching the refusal
		 * leaves a black window no titlebar can be reached from. The
		 * reset that comes with another selection or a returned output
		 * tries it at once. */
		const int show        = kms_sel_to_show(p);
		const uint32_t resets = fnl_atomic_load_u32(&p->scan_resets);
		const bool anew       = show != p->scan_sel ||
						  resets != p->scan_resets_seen ||
						  now_seconds() - p->scan_failed_at >= KMS_SCAN_RETRY_S;
		if (!anew) {
			/* The flip is this path's only throttle: without it a refused
			 * output has the renderer drawing flat out for nothing. About a
			 * refresh, so a new selection is still taken at once. */
			fnl_sleep_seconds(0.016);
			return -1;
		}
		p->scan_failed = false;
		/* A flip refused on a CRTC that has since been unbound would be
		 * refused again: the retry sets the mode. */
		p->scan_set_owed = true;
	}
	/* The pair is of the colour the renderer last published: a device that
	 * came back onto or off HDR leaves the other format's pair, retired as
	 * another size's is. The same frame then sets the mode again. */
	const uint32_t fourcc = scan_fourcc(a);
	if (p->scan.map[0] && p->scan.fourcc != fourcc) {
		flip_wait(a, FLIP_WAIT_MS);
		flip_give_up(a);
		scan_retire(p, kms_sel_mode(p, p->scan_sel), true);
		p->scan_set_owed = true;
	}
	/* Another mode, or another output: the last flip waited in, an output
	 * left goes dark, the buffers kept or retired by the new mode's size
	 * (scan_retire()), and the first frame sets the mode. */
	const int show        = kms_sel_to_show(p);
	const uint32_t resets = fnl_atomic_load_u32(&p->scan_resets);
	if (show != p->scan_sel || resets != p->scan_resets_seen) {
		p->scan_resets_seen = resets;
		flip_wait(a, FLIP_WAIT_MS);
		flip_give_up(a);
		const int was   = p->scan_sel;
		p->scan_sel     = show;
		if (was >= 0 && KMS_SEL_OUT(was) != KMS_SEL_OUT(show))
			kms_output_off(a, KMS_SEL_OUT(was), show, true);
		scan_retire(p, kms_sel_mode(p, show), false);
		p->scan_set_owed = true;
	}
	const struct drm_mode_modeinfo *mode = kms_sel_mode(p, p->scan_sel);
	if (!p->scan.map[0]) {
		p->scan_front = -1;
		if (!scan_make(p, &p->scan, mode, 0, fourcc) ||
			!scan_make(p, &p->scan, mode, 1, fourcc)) {
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
	if (!flip_wait(a, vsync ? FLIP_WAIT_MS : 0)) {
		if (vsync) {
			flip_give_up(a); /* lost: flip again regardless */
		} else {
			/* A frame that still finds the last flip pending is
			 * dropped -- but a completion that never comes would drop
			 * every frame for the window's life, so one older than a
			 * vsync wait is given up on, as that path's is. */
			fnl_mutex_lock(&p->host->flip_lock);
			const bool lost = p->flip_pending &&
							  now_seconds() - p->flip_at >=
								  (double)FLIP_WAIT_MS / 1000.0;
			fnl_mutex_unlock(&p->host->flip_lock);
			if (!lost)
				return -1;
			flip_give_up(a);
		}
	}
	return p->scan_front == 0 ? 1 : 0;
}

/* The output told what is being shown in it: 10 bits a channel on the link,
 * BT.2020 RGB, and the static metadata a PQ frame carries. Set with the
 * mode that first shows a 10-bit pair; `hdr_blob` is ours to destroy.
 *
 * The peak in the metadata is the panel's own where its EDID names one, so
 * a compositor or a TV need not assume the 1000-nit master the renderer's
 * PQ encode is written against. */
static void scan_hdr_apply(App *a, int sel)
{
	struct BackendState *p      = a->backend_state;
	struct BackendHostState *hs = p->host;
	const int fd                = hs->card_fd;
	const KmsOutput *o          = kms_out(p, sel);
	const uint32_t connector    = o->connector_id;
	struct drm_mode_obj_set_property prop = {
		.obj_id   = connector,
		.obj_type = DRM_MODE_OBJECT_CONNECTOR,
	};
	prop.prop_id = o->prop[KMS_PROP_MAX_BPC];
	prop.value   = 10;
	if (prop.prop_id)
		drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &prop);
	prop.prop_id = o->prop[KMS_PROP_COLORSPACE];
	prop.value   = KMS_COLORIMETRY_BT2020_RGB;
	if (prop.prop_id &&
		drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &prop) < 0)
		fnl_log(FNL_LOG_DEBUG, "KMS: Colorspace was refused (%s)",
				strerror(errno));

	unsigned max_lum = o->max_lum ? o->max_lum : 1000;

	struct hdr_output_metadata md;
	memset(&md, 0, sizeof md);
	md.metadata_type = KMS_STATIC_METADATA_TYPE1;
	struct hdr_metadata_infoframe *h = &md.hdmi_metadata_type1;
	h->eotf          = KMS_EOTF_SMPTE_ST2084;
	h->metadata_type = KMS_STATIC_METADATA_TYPE1;
	for (int i = 0; i < 3; i++) {
		h->display_primaries[i].x = HDR_PRIMARIES[i][0];
		h->display_primaries[i].y = HDR_PRIMARIES[i][1];
	}
	h->white_point.x = HDR_PRIMARIES[3][0];
	h->white_point.y = HDR_PRIMARIES[3][1];
	h->max_display_mastering_luminance =
		(uint16_t)(max_lum > 65535u ? 65535u : max_lum);
	h->min_display_mastering_luminance = 1; /* 0.0001 cd/m² */
	h->max_cll = h->max_fall = 0;         /* not known from the frame */

	struct drm_mode_create_blob cb = {
		.data   = (uintptr_t)&md,
		.length = sizeof md,
	};
	if (p->hdr_blob) {
		struct drm_mode_destroy_blob db = { .blob_id = p->hdr_blob };
		drm_ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
		p->hdr_blob = 0;
	}
	if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb) == 0) {
		p->hdr_blob  = cb.blob_id;
		prop.prop_id = o->prop[KMS_PROP_HDR];
		prop.value   = p->hdr_blob;
		if (prop.prop_id &&
			drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &prop) < 0)
			fnl_log(FNL_LOG_WARN,
					"KMS: the HDR metadata was refused (%s)",
					strerror(errno));
	}
	p->hdr_set = true;
	p->hdr_out = KMS_SEL_OUT(sel);
	fnl_log(FNL_LOG_INFO, "KMS: %s scans out 10-bit PQ, %u cd/m²",
			kms_name(p, p->scan_sel), max_lum);
}

/* The output back to no colour statement, which an 8-bit pair wants: the
 * metadata pulled, the colourimetry default (max bpc is left for the link's
 * own good). */
static void scan_hdr_clear(App *a)
{
	struct BackendState *p      = a->backend_state;
	struct BackendHostState *hs = p->host;
	const int fd                = hs->card_fd;
	const KmsOutput *o          = p->hdr_out >= 0 ? &hs->outputs[p->hdr_out]
												  : NULL;
	const uint32_t connector    = o ? o->connector_id : 0;
	struct drm_mode_obj_set_property prop = {
		.obj_id   = connector,
		.obj_type = DRM_MODE_OBJECT_CONNECTOR,
	};
	prop.prop_id = o ? o->prop[KMS_PROP_COLORSPACE] : 0;
	prop.value   = 0; /* DRM_MODE_COLORIMETRY_DEFAULT */
	if (prop.prop_id)
		drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &prop);
	prop.prop_id = o ? o->prop[KMS_PROP_HDR] : 0;
	prop.value   = 0;
	if (prop.prop_id)
		drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &prop);
	if (p->hdr_blob) {
		struct drm_mode_destroy_blob db = { .blob_id = p->hdr_blob };
		drm_ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
		p->hdr_blob = 0;
	}
	p->hdr_set = false;
	p->hdr_out = -1;
}

/* [render] The CRTC the mode is set on, chosen when the set is made: what
 * drives the connector now -- the console's, the display path driver's, or
 * ours from before -- else a CRTC GETCRTC shows with no mode and no
 * framebuffer and that no other output of the card is driven by -- else the
 * open's guess where no other output is driven by it, a set on one that is
 * cloning the two onto a plane rather than refusing; and 0, a refused set,
 * where even that is busy. The caller holds BackendHostState::crtc_lock. */
static uint32_t scan_crtc_pick(const struct BackendState *p,
							   const KmsOutput *out)
{
	struct BackendHostState *hs = p->host;
	const uint32_t live         = connector_live_crtc(hs->card_fd, out);
	if (live)
		return live;
	uint32_t crtcs[KMS_MAX_CRTCS];
	struct drm_mode_card_res r = { 0 };
	r.count_crtcs = KMS_MAX_CRTCS;
	r.crtc_id_ptr = (uintptr_t)crtcs;
	if (drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0)
		return out->crtc_id;
	const uint32_t n = r.count_crtcs < KMS_MAX_CRTCS ? r.count_crtcs
													 : KMS_MAX_CRTCS;
	uint32_t busy    = 0;
	const uint32_t count = fnl_atomic_load_u32(&hs->output_count);
	for (uint32_t i = 0; i < count && i < KMS_MAX_OUTPUTS; i++) {
		if (hs->outputs[i].connector_id == out->connector_id)
			continue;
		const uint32_t c = connector_live_crtc(hs->card_fd, &hs->outputs[i]);
		for (uint32_t j = 0; c && j < n; j++)
			if (crtcs[j] == c)
				busy |= 1u << j;
	}
	/* Only a CRTC one of the connector's encoders reaches: where the routing
	 * is fixed (vc4, older i915) any other is a set refused for good. */
	const uint32_t reach = connector_possible_crtcs(hs->card_fd, out);
	for (uint32_t j = 0; j < n; j++) {
		if ((busy & (1u << j)) || (reach && !(reach & (1u << j))))
			continue;
		struct drm_mode_crtc c = { .crtc_id = crtcs[j] };
		if (drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_GETCRTC, &c) == 0 &&
			!c.mode_valid && !c.fb_id)
			return crtcs[j];
	}
	/* The open's guess, but only where no other output is driven by it: a
	 * set on a CRTC another window's output is on would clone the two onto
	 * that one plane, or take it from the other window. */
	for (uint32_t j = 0; j < n; j++)
		if (crtcs[j] == out->crtc_id && !(busy & (1u << j)))
			return out->crtc_id;
	return 0;
}

/* [render] No CRTC but `keep` scans out one of `s`'s framebuffers: a mode set
 * on another CRTC leaves the one it used to be on lit with the same frame
 * (the display driver's, after kms_scanout_take()). The framebuffer test is
 * the guard: a CRTC this window set and another window has since taken shows
 * that window's framebuffer, not ours. Atomic where the table has the CRTC,
 * the legacy off otherwise. The caller holds crtc_lock. */
static void scan_dark_elsewhere(struct BackendState *p, const KmsScan *s,
								uint32_t keep)
{
	if (!s->fb[0] && !s->fb[1])
		return;
	uint32_t crtcs[KMS_MAX_CRTCS];
	struct drm_mode_card_res r = { 0 };
	r.count_crtcs = KMS_MAX_CRTCS;
	r.crtc_id_ptr = (uintptr_t)crtcs;
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0)
		return;
	const uint32_t n = r.count_crtcs < KMS_MAX_CRTCS ? r.count_crtcs
													 : KMS_MAX_CRTCS;
	for (uint32_t i = 0; i < n; i++) {
		if (crtcs[i] == keep)
			continue;
		struct drm_mode_crtc c = { .crtc_id = crtcs[i] };
		if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0 ||
			!c.fb_id || (c.fb_id != s->fb[0] && c.fb_id != s->fb[1]))
			continue;
		bool darkened = p->host->atomic &&
						kms_atomic_crtc_off(p->host->atomic, crtcs[i]);
		if (!darkened) {
			struct drm_mode_crtc off = { .crtc_id = crtcs[i] };
			drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
		}
	}
}

void kms_scan_show(App *a, int back)
{
	struct BackendState *p            = a->backend_state;
	struct BackendHostState *hs       = p->host;
	if (back < 0 || back > 1 || !p->scan.fb[back])
		return;
	const bool vsync                     = fnl_atomic_load_bool(&a->vsync);
	const struct drm_mode_modeinfo *mode = kms_sel_mode(p, p->scan_sel);
	const KmsOutput *out                 = kms_out(p, p->scan_sel);
	if (p->scan_front < 0 || p->scan_set_owed) {
		/* What the frame is, said to the output before it is set: 10-bit
		 * PQ where the pair is, and back to nothing where it is not. The
		 * connector told is the one set, not one set once: a move to another
		 * output clears the old statement and tells the new, and one that
		 * goes 8-bit clears whichever connector was told. */
		const bool pq = p->scan.fourcc == DRM_FORMAT_XRGB2101010;
		if (pq && (!p->hdr_set || p->hdr_out != KMS_SEL_OUT(p->scan_sel))) {
			if (p->hdr_set)
				scan_hdr_clear(a);
			scan_hdr_apply(a, p->scan_sel);
		} else if (!pq && p->hdr_set) {
			scan_hdr_clear(a);
		}
		const bool good = (int)fnl_atomic_load_u32(&p->sel_good) == p->scan_sel;
		const bool forced = !good && FNL_FAULT("kms-mode-refused");
		/* The CRTC is chosen here and not at the open: the open's guess can
		 * be one another output is lit on, or one the display driver has
		 * since left showing this pair (kms_scanout_take()). Under the lock
		 * with the set, so a second window cannot choose the CRTC this one
		 * is about to bind. */
		fnl_mutex_lock(&hs->crtc_lock);
		const uint32_t crtc = scan_crtc_pick(p, out);
		bool set_ok      = false;
		int atomic_err   = 0;
		if (hs->atomic && !forced && crtc) {
			set_ok = kms_atomic_set(hs->atomic, out->connector_id, crtc, mode,
									p->scan.fb[back]);
			/* Kept: the legacy fallback below overwrites errno, and a
			 * CRTC busy a moment ago is not a refusal. */
			if (!set_ok)
				atomic_err = errno;
		}
		/* The legacy set where the card has no atomic, and where one it
		 * offered refused this commit: the same call the backend made before
		 * atomic, so a mode it will not take is refused by both. */
		if (!set_ok) {
			struct drm_mode_crtc set = {
				.set_connectors_ptr = (uintptr_t)&out->connector_id,
				.count_connectors   = 1,
				.crtc_id            = crtc,
				.fb_id              = p->scan.fb[back],
				.mode_valid         = 1,
				.mode               = *mode,
			};
			set_ok = crtc && !forced &&
					 drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
		}
		if (!set_ok) {
			const bool busy = crtc && !forced &&
							  (atomic_err == EBUSY || errno == EBUSY);
			fnl_mutex_unlock(&hs->crtc_lock);
			/* A CRTC busy with a commit in flight is not a refusal, as at a
			 * flip's EBUSY: the frame is dropped and the next tries again,
			 * where a refusal here darkens the window for KMS_SCAN_RETRY_S.
			 */
			if (busy)
				return;
			if (!good) {
				/* A mode the output was switched to and will not take: the
				 * last that did comes back at the next frame. */
				kms_sel_refused(a, p->scan_sel, false);
				return;
			}
			scan_refuse(p, "the mode would not set");
			return;
		}
		p->scan_crtc = crtc;
		events_drain(a);
		/* Off the CRTC the pair was on if this set left it there, before
		 * the pair is released: a buffer no plane shows may go. */
		scan_dark_elsewhere(p, &p->scan, crtc);
		scan_dark_elsewhere(p, &p->scan_old, crtc);
		/* Off this plane now that this pair is on, and on no other's
		 * unless an output gone dark still shows it (scan_release()). */
		scan_release(p, &p->scan_old);
		fnl_mutex_unlock(&hs->crtc_lock);
		kms_sel_shown(a, p->scan_sel);
		p->scan_front     = back;
		p->scan_set_owed  = false;
		p->flip_async_off = false;
		return;
	}
	const uint32_t crtc = p->scan_crtc ? p->scan_crtc : out->crtc_id;
	const bool async    = !vsync && !p->flip_async_off;
	bool async_now      = async;
	bool flipped      = false;
	int atomic_err    = 0;
	/* Published before the flip is submitted: the kernel may deliver the
	 * completion at once, and another window's thread read it before this
	 * one returns from the ioctl -- one that found no pending flip would be
	 * dropped as nobody's, leaving this window's flag set with no event
	 * owed, which no later flip could clear. */
	fnl_mutex_lock(&hs->flip_lock);
	p->flip_async   = async_now;
	p->flip_index   = a->presents;
	p->flip_pending = true;
	p->flip_at      = now_seconds();
	fnl_mutex_unlock(&hs->flip_lock);
	if (hs->atomic) {
		flipped = kms_atomic_flip(hs->atomic, crtc, p->scan.fb[back],
								  async_now, (uint64_t)(uintptr_t)a);
		if (!flipped && async_now && errno == EINVAL) {
			p->flip_async_off = true;
			fnl_log(FNL_LOG_INFO,
					"KMS: %s takes no asynchronous flip; without "
					"vsync a frame that finds one pending is dropped",
					kms_name(p, p->scan_sel));
			async_now = false;
			fnl_mutex_lock(&hs->flip_lock);
			p->flip_async = false;
			fnl_mutex_unlock(&hs->flip_lock);
			flipped = kms_atomic_flip(hs->atomic, crtc,
									  p->scan.fb[back], false,
									  (uint64_t)(uintptr_t)a);
		}
		/* Kept: the legacy fallback below overwrites errno, and a CRTC
		 * busy a moment ago is the one answer this frame must not turn
		 * into a refusal. */
		if (!flipped)
			atomic_err = errno;
	}
	if (!flipped) {
		/* The legacy flip: a card with no atomic, and a CRTC the atomic table
		 * does not name (a driver whose CRTC has no primary plane to set a
		 * mode through), which the mode set above already fell back for. The
		 * window rides on it and comes back in its event (flip_complete());
		 * the frame index is the window's own, kept until then. */
		struct drm_mode_crtc_page_flip flip = {
			.crtc_id = crtc,
			.fb_id   = p->scan.fb[back],
			.flags   = DRM_MODE_PAGE_FLIP_EVENT |
					 (async_now ? DRM_MODE_PAGE_FLIP_ASYNC : 0u),
			.user_data = (uint64_t)(uintptr_t)a,
		};
		flipped =
			drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0;
		if (!flipped && async_now && errno == EINVAL) {
			p->flip_async_off = true;
			fnl_log(FNL_LOG_INFO,
					"KMS: %s takes no asynchronous flip; without "
					"vsync a frame that finds one pending is dropped",
					kms_name(p, p->scan_sel));
			flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
			async_now  = false;
			fnl_mutex_lock(&hs->flip_lock);
			p->flip_async = false;
			fnl_mutex_unlock(&hs->flip_lock);
			flipped =
				drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0;
		}
	}
	if (!flipped) {
		/* Refused: the pending flip published above was never made. */
		fnl_mutex_lock(&hs->flip_lock);
		p->flip_pending = false;
		fnl_mutex_unlock(&hs->flip_lock);
		if (atomic_err)
			errno = atomic_err;
		/* A CRTC still busy -- the flip before it not yet complete, or a
		 * mode set in flight -- is not a refusal: this frame is dropped and
		 * the next one tries again. Told apart from a real refusal, a
		 * transient EBUSY would end every frame for the window's life. */
		if (errno == EBUSY)
			return;
		/* A CRTC switched off under the window, or an output that has just
		 * left and whose window has not moved off yet, is not a refusal
		 * either: EINVAL with no mode on the CRTC, with a connector the
		 * kernel says is no longer connected (a forced unplug counts as
		 * gone only after its away time, and flips to it are refused
		 * meanwhile), with no connector bound, or with the scan's output
		 * away or in outputs_gone, is that, and the next frame moves the
		 * window or sets the mode again. */
		const int e = errno;
		const int sout = KMS_SEL_OUT(p->scan_sel);
		bool away = false;
		if (e == EINVAL) {
			struct drm_mode_get_connector gc = {
				.connector_id = hs->outputs[sout].connector_id,
			};
			away =
				drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0 &&
				gc.connection != 1; /* DRM_MODE_CONNECTED */
		}
		if (e == EINVAL &&
			(away ||
			 ((fnl_atomic_load_u32(&hs->outputs_away) >> (unsigned)sout) &
			  1u) ||
			 !crtc_lit(hs->card_fd, crtc) ||
			 !connector_live_crtc(hs->card_fd, &hs->outputs[sout]) ||
			 ((fnl_atomic_load_u32(&hs->outputs_gone) >> (unsigned)sout) &
			  1u)))
			return;
		scan_refuse(p, "the flip was refused");
		return;
	}
	/* The pending flip was published with its index and whether it tears,
	 * before the submit; a retry that turned async off updated the same
	 * field under the same lock. */
	p->scan_front = back;
	/* A frame reached the output: a refusal from here on is a new one. */
	p->scan_said = false;
	if (vsync)
		flip_wait(a, FLIP_WAIT_MS);
}

/* One row of a frame into a scanout buffer, `scan10`'s format. The frame's
 * 10-bit word is Vulkan's A2B10G10R10: R in bits 9:0, G 19:10 and B 29:20.
 * An 8-bit one is X in 31:24, R 23:16, G 15:8 and B 7:0 (XRGB8888). The
 * 10-bit scanout's own word is XRGB2101010, x 31:30, R 29:20, G 19:10 and B
 * 9:0 -- the opposite order -- so the two named channels must swap here.
 *
 * A loop a format rather than three tests a pixel: where the driver cannot
 * take a display (lavapipe on the Pi and the thinkpad) this copy is the
 * frame's whole CPU cost. The arithmetic is the general form's with the
 * format known: the 8-bit sources are a byte a channel, so their shifts by
 * two and back cancel. */
static void scan_row(unsigned char *dst, const unsigned char *src, int n,
					 FnlPixelFormat fmt, bool scan10)
{
	if (fmt == PIXELS_RGB10 && scan10) {
		for (int x = 0; x < n; x++) {
			uint32_t w;
			memcpy(&w, src + (size_t)x * 4u, sizeof w);
			const uint32_t o = ((w & 0x3ffu) << 20) | (w & 0xffc00u) |
							   ((w >> 20) & 0x3ffu);
			memcpy(dst + (size_t)x * 4u, &o, sizeof o);
		}
	} else if (fmt == PIXELS_RGB10) {
		for (int x = 0; x < n; x++) {
			uint32_t w;
			memcpy(&w, src + (size_t)x * 4u, sizeof w);
			const uint32_t o = (((w >> 20) & 0x3ffu) >> 2) |
							   ((((w >> 10) & 0x3ffu) >> 2) << 8) |
							   (((w & 0x3ffu) >> 2) << 16);
			memcpy(dst + (size_t)x * 4u, &o, sizeof o);
		}
	} else if (fmt == PIXELS_BGRA8 && scan10) {
		for (int x = 0; x < n; x++) {
			const unsigned char *px = src + (size_t)x * 4u;
			const uint32_t o        = ((uint32_t)px[2] << 22) |
						   ((uint32_t)px[1] << 12) | ((uint32_t)px[0] << 2);
			memcpy(dst + (size_t)x * 4u, &o, sizeof o);
		}
	} else if (fmt == PIXELS_BGRA8) {
		for (int x = 0; x < n; x++) {
			const unsigned char *px = src + (size_t)x * 4u;
			const uint32_t o        = (uint32_t)px[0] |
						   ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16);
			memcpy(dst + (size_t)x * 4u, &o, sizeof o);
		}
	} else if (scan10) {
		for (int x = 0; x < n; x++) {
			const unsigned char *px = src + (size_t)x * 4u;
			const uint32_t o        = ((uint32_t)px[0] << 22) |
						   ((uint32_t)px[1] << 12) | ((uint32_t)px[2] << 2);
			memcpy(dst + (size_t)x * 4u, &o, sizeof o);
		}
	} else {
		/* RGBA8, the default. */
		for (int x = 0; x < n; x++) {
			const unsigned char *px = src + (size_t)x * 4u;
			const uint32_t o        = (uint32_t)px[2] |
						   ((uint32_t)px[1] << 8) | ((uint32_t)px[0] << 16);
			memcpy(dst + (size_t)x * 4u, &o, sizeof o);
		}
	}
}

static void kms_present_pixels(App *a, const void *px, int w, int h,
							   size_t pitch, FnlPixelFormat fmt)
{
	struct BackendState *p = a->backend_state;
	const int back         = kms_scan_begin(a);
	if (back < 0)
		return;
	const struct drm_mode_modeinfo *mode = kms_sel_mode(p, p->scan_sel);
	/* The pair's own size as well as the mode's: a pair taken over from
	 * the CRTC's mode (kms_scanout_take()) need not be the selection's. */
	int cw = w < mode->hdisplay ? w : mode->hdisplay;
	int ch = h < mode->vdisplay ? h : mode->vdisplay;
	if (cw > (int)p->scan.w)
		cw = (int)p->scan.w;
	if (ch > (int)p->scan.h)
		ch = (int)p->scan.h;
	const unsigned char *src = px;
	const bool scan10        = p->scan.fourcc == DRM_FORMAT_XRGB2101010;
	for (int y = 0; y < ch; y++) {
		unsigned char *dst =
			p->scan.map[back] + (size_t)y * p->scan.pitch;
		scan_row(dst, src + (size_t)y * pitch, cw, fmt, scan10);
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
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0)
		return -1;
	*out = (KmsScanBuf){
		.w      = p->scan.w,
		.h      = p->scan.h,
		.pitch  = p->scan.pitch,
		.fourcc = p->scan.fourcc,
		.key    = p->scan.gen[i],
	};
	return ph.fd;
}

/* [render] Through the mapping every buffer of the pair has: a GPU that
 * imports a buffer of another device's may draw into a copy of its own,
 * and nothing else says so (kms_scan_peek()'s caller, egl_kms.c). */
bool kms_scan_peek(App *a, int i, uint32_t *first, uint32_t *last)
{
	const struct BackendState *p = a->backend_state;
	if (i < 0 || i > 1 || !p->scan.map[i] || p->scan.w <= 0 || p->scan.h <= 0)
		return false;
	const unsigned char *m = p->scan.map[i];
	const size_t end =
		(size_t)(p->scan.h - 1) * p->scan.pitch + (size_t)(p->scan.w - 1) * 4u;
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

bool kms_display_hdr(App *a, unsigned *max_lum)
{
	const struct BackendState *p = a->backend_state;
	const KmsOutput *o           = kms_out(p, kms_sel_to_show(p));
	if (max_lum)
		*max_lum = o->max_lum;
	return o->hdr_ok;
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
		fnl_atomic_store_u32(m,
							 fnl_atomic_load_u32(m) | 1u << KMS_SEL_MODE(sel));
	}
	loop_wake(&p->host->wake);
}

void kms_sel_shown(App *a, int sel)
{
	fnl_atomic_store_u32(&a->backend_state->sel_good, (uint32_t)sel);
}

/* The connector's EDID as its property blob holds it, into `edid`: the
 * bytes read, or 0. */
static size_t edid_read(int fd, const KmsOutput *o, unsigned char *edid,
						size_t cap)
{
	const uint32_t want = o->prop[KMS_PROP_EDID];
	if (!want)
		return 0;
	uint32_t ids[64];
	/* 64-bit, as the kernel writes them: `values` here was 32-bit once, and
	 * a connector with more than 32 properties had the kernel write past
	 * it. Only index 0 would also have read back right. */
	uint64_t values[64];
	struct drm_mode_obj_get_properties gp = {
		.props_ptr       = (uintptr_t)ids,
		.prop_values_ptr = (uintptr_t)values,
		.count_props     = 64,
		.obj_id          = o->connector_id,
		.obj_type        = DRM_MODE_OBJECT_CONNECTOR,
	};
	if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
		return 0;
	const uint32_t n = gp.count_props < 64 ? gp.count_props : 64;
	uint32_t blob    = 0;
	for (uint32_t i = 0; i < n && !blob; i++)
		if (ids[i] == want)
			blob = (uint32_t)values[i];
	if (!blob)
		return 0;
	/* Asked twice: the kernel copies the blob only when the length given is
	 * exactly the blob's, and writes the real length back either way, so a
	 * buffer's capacity in its place copies nothing and leaves `edid` as it
	 * was. One longer than the buffer is not read at all. */
	struct drm_mode_get_blob gb = { .blob_id = blob };
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) < 0 || gb.length == 0 ||
		gb.length > cap)
		return 0;
	const uint32_t len = gb.length;
	gb.data            = (uintptr_t)edid;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) < 0 || gb.length != len)
		return 0;
	return len;
}

/* The HDR static metadata block of the EDID's CTA-861 extension, where it
 * has one: whether ST 2084 is among its EOTFs, and its desired-content peak
 * in cd/m² (0 where it names none). */
static bool edid_hdr_ok(const unsigned char *e, size_t n, unsigned *max_lum)
{
	if (n < 128 || e[0] != 0x00 || e[1] != 0xff || e[2] != 0xff ||
		e[3] != 0xff || e[4] != 0xff || e[5] != 0xff || e[6] != 0xff ||
		e[7] != 0x00)
		return false;
	const int ext = e[126];
	for (int b = 1; b <= ext && (size_t)(b + 1) * 128u <= n; b++) {
		const unsigned char *x = e + (size_t)b * 128u;
		if (x[0] != 0x02) /* a CTA-861 extension block */
			continue;
		const int dtd = x[2] >= 4 ? x[2] : 128;
		int i         = 4;
		while (i + 1 < dtd) {
			const int tag = x[i] >> 5, len = x[i] & 0x1f;
			const unsigned char *p = x + i + 1;
			if (i + 1 + len > 127)
				break;
			if (tag == 7 && len >= 2 && p[0] == 0x06) {
				/* Extended tag 6, HDR static metadata: p[1]'s EOTF
				 * flags carry ST 2084 at bit 2, and p[3], where it is
				 * given, the desired content's peak in units of
				 * 50 cd/m². */
				if (!(p[1] & 0x04))
					return false;
				if (len >= 4 && p[3])
					*max_lum = p[3] * 50u;
				return true;
			}
			i += 1 + len;
		}
	}
	return false;
}

/* Whether this output can scan out PQ: its EDID says so, and the connector
 * has the HDR_OUTPUT_METADATA property to say what is shown in it. */
static void output_hdr_probe(int fd, KmsOutput *o)
{
	unsigned char edid[1024];
	const size_t n = edid_read(fd, o, edid, sizeof edid);
	o->hdr_ok = edid_hdr_ok(edid, n, &o->max_lum) &&
				o->prop[KMS_PROP_HDR] != 0;
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
/* Whether `crtc` scans out nothing or one of this window's pairs. The pump
 * gives an output up as soon as the window leaves it, and the render thread
 * darkens it a frame later; a CRTC another window has set its mode on in
 * between shows that window's framebuffer, and is not ours to unbind. The
 * caller holds crtc_lock. */
static bool scan_shows_ours(const struct BackendState *p, uint32_t crtc)
{
	struct drm_mode_crtc c = { .crtc_id = crtc };
	if (drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0)
		return false;
	if (!c.fb_id)
		return true;
	const KmsScan *pairs[2 + KMS_PARKED] = { &p->scan, &p->scan_old };
	for (int i = 0; i < KMS_PARKED; i++)
		pairs[2 + i] = &p->scan_parked[i];
	for (int i = 0; i < 2 + KMS_PARKED; i++)
		if (c.fb_id == pairs[i]->fb[0] || c.fb_id == pairs[i]->fb[1])
			return true;
	return false;
}

void kms_output_off(App *a, int out, int shown, bool unbind)
{
	struct BackendState *p   = a->backend_state;
	struct BackendHostState *hs = p->host;
	fnl_mutex_lock(&hs->crtc_lock);
	const uint32_t crtc = connector_live_crtc(hs->card_fd, &hs->outputs[out]);
	const uint32_t keep =
		connector_live_crtc(hs->card_fd, kms_out(p, shown));
	if (!crtc || crtc == keep || (unbind && !scan_shows_ours(p, crtc))) {
		fnl_mutex_unlock(&hs->crtc_lock);
		return;
	}
	/* Every connector the kernel still has on that CRTC, not only the
	 * output's: a commit that switches a CRTC off while any connector is
	 * still on it is refused (EINVAL, drm_atomic_helper_check_modeset), and
	 * Mesa's WSI leaves a connector bound as the window takes another's
	 * mode. */
	uint32_t conns[KMS_MAX_OUTPUTS];
	int nconn = 0;
	const uint32_t nout = fnl_atomic_load_u32(&hs->output_count);
	for (uint32_t i = 0; i < nout && i < KMS_MAX_OUTPUTS; i++)
		if (hs->outputs[i].connector_id &&
			connector_live_crtc(hs->card_fd, &hs->outputs[i]) == crtc)
			conns[nconn++] = hs->outputs[i].connector_id;
	int r;
	if (unbind) {
		if (hs->atomic &&
			kms_atomic_unbind(hs->atomic, crtc, conns, nconn))
			r = 0;
		else {
			struct drm_mode_crtc off = { .crtc_id = crtc };
			r = drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_SETCRTC, &off);
		}
	} else {
		struct drm_mode_obj_set_property dpms = {
			.value    = DRM_MODE_DPMS_OFF,
			.prop_id  = hs->outputs[out].prop[KMS_PROP_DPMS],
			.obj_id   = hs->outputs[out].connector_id,
			.obj_type = DRM_MODE_OBJECT_CONNECTOR,
		};
		r = dpms.prop_id
				? drm_ioctl(hs->card_fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &dpms)
				: -1;
	}
	fnl_mutex_unlock(&hs->crtc_lock);
	if (r < 0)
		fnl_log(FNL_LOG_WARN, "KMS: %s would not go dark (%s)",
				hs->outputs[out].name, strerror(errno));
	else
		fnl_log(FNL_LOG_INFO, "KMS: %s dark", hs->outputs[out].name);
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
	const int cw       = a->snap_w < m->hdisplay ? a->snap_w : m->hdisplay;
	const int ch       = a->snap_h < m->vdisplay ? a->snap_h : m->vdisplay;
	const size_t pitch = (size_t)a->snap_w * 4u;
	const bool scan10  = p->scan.fourcc == DRM_FORMAT_XRGB2101010;
	for (int y = 0; y < ch; y++) {
		unsigned char *dst = p->scan.map[back] + (size_t)y * p->scan.pitch;
		const unsigned char *row = a->snap_px + (size_t)y * pitch;
		if (scan10)
			scan_row(dst, row, cw, PIXELS_BGRA8, true);
		else
			memcpy(dst, row, (size_t)cw * 4u);
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
	fnl_mutex_lock(&p->host->crtc_lock);
	struct drm_mode_crtc on  = {
		 .crtc_id = connector_live_crtc(p->host->card_fd, kms_out(p, sel)),
	};
	if (!on.crtc_id ||
		drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_GETCRTC, &on) < 0 ||
		!on.mode_valid || !on.fb_id) {
		fnl_mutex_unlock(&p->host->crtc_lock);
		return;
	}
	flip_wait(a, FLIP_WAIT_MS);
	flip_give_up(a);
	scan_retire(p, &on.mode, false);
	const uint32_t fourcc = scan_fourcc(a);
	if (!p->scan.map[0] && (!scan_make(p, &p->scan, &on.mode, 0, fourcc) ||
							!scan_make(p, &p->scan, &on.mode, 1, fourcc))) {
		scan_free(p, &p->scan);
		fnl_log(FNL_LOG_WARN, "KMS: no buffer to take %s back with (%s)",
				kms_name(p, sel), strerror(errno));
		fnl_mutex_unlock(&p->host->crtc_lock);
		return;
	}
	const int back = p->scan_front == 0 ? 1 : 0;
	scan_fill(p, back, &on.mode, a);
	bool took = false;
	if (p->host->atomic)
		took = kms_atomic_set(p->host->atomic, connector, on.crtc_id,
							  &on.mode, p->scan.fb[back]);
	if (!took) {
		struct drm_mode_crtc set = {
			.set_connectors_ptr = (uintptr_t)&connector,
			.count_connectors   = 1,
			.crtc_id            = on.crtc_id,
			.fb_id              = p->scan.fb[back],
			.mode_valid         = 1,
			.mode               = on.mode,
		};
		took = drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
	}
	if (!took) {
		fnl_log(FNL_LOG_WARN,
				"KMS: %s could not be taken back from the "
				"driver (%s)",
				kms_name(p, sel), strerror(errno));
		fnl_mutex_unlock(&p->host->crtc_lock);
		return;
	}
	events_drain(a);
	scan_release(p, &p->scan_old);
	fnl_mutex_unlock(&p->host->crtc_lock);
	/* The CRTC the driver left the pair on is this window's from now. */
	p->scan_crtc     = on.crtc_id;
	p->scan_front    = back;
	p->scan_sel      = sel;
	p->scan_set_owed = true;
}

/* Adopted and handed over, the surface asked for again, the size
 * published; the pointer kept where it was on an output it stays on, and
 * put in the middle of one it moves to. */
/* [main] Output `now` is this window's from here and `was` is no longer: the
 * host's held set follows the selection, after the selection itself, so a
 * reader that sees the new one does not briefly hide the output moved to. */
static void outputs_hold(struct BackendHostState *hs, int was, int now)
{
	uint32_t m = fnl_atomic_load_u32(&hs->outputs_held);
	if (was >= 0 && was < KMS_MAX_OUTPUTS)
		m &= ~(1u << (unsigned)was);
	if (now >= 0 && now < KMS_MAX_OUTPUTS)
		m |= 1u << (unsigned)now;
	fnl_atomic_store_u32(&hs->outputs_held, m);
}

static void sel_set(App *a, int sel)
{
	struct BackendState *p = a->backend_state;
	const int was          = p->out_cur;
	mode_adopt(p, sel);
	fnl_atomic_store_u32(&p->sel_want, (uint32_t)sel);
	outputs_hold(p->host, was, p->out_cur);
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
			p->host->outputs[p->out_cur].name, p->mode_w, p->mode_h,
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
	struct BackendHostState *hs = p->host;
	const int now          = KMS_SEL(p->out_cur, p->mode_cur);
	if (!kms_sel_is_refused(p, now))
		return;
	const int was = p->out_cur, was_w = p->mode_w, was_h = p->mode_h;
	mode_adopt(p, (int)fnl_atomic_load_u32(&p->sel_good));
	fnl_atomic_store_u32(&p->sel_want, fnl_atomic_load_u32(&p->sel_good));
	outputs_hold(hs, was, p->out_cur);
	if (p->out_cur != was)
		cursor_moved(a);
	p->ptr_x = clampd(p->ptr_x, 0.0, p->mode_w - 1.0);
	p->ptr_y = clampd(p->ptr_y, 0.0, p->mode_h - 1.0);
	fnl_log(FNL_LOG_WARN,
			"KMS: %s would not show %dx%d; back to %s at "
			"%dx%d",
			hs->outputs[was].name, was_w, was_h, hs->outputs[p->out_cur].name,
			p->mode_w, p->mode_h);
	publish_mode(a);
	pointer_moved(a);
}

/* The mode of this size on the output the window is on, the refresh
 * nearest, of those not refused; or -1. */
static int mode_find(const struct BackendState *p, const FnlMode *want)
{
	const KmsOutput *o    = &p->host->outputs[p->out_cur];
	int best              = -1;
	unsigned int best_off = 0;
	for (int i = 0; i < o->mode_count; i++) {
		if (o->modes[i].hdisplay != want->w_px ||
			o->modes[i].vdisplay != want->h_px ||
			kms_sel_is_refused(p, KMS_SEL(p->out_cur, i)))
			continue;
		const unsigned int off = refresh_off(&o->modes[i], want->refresh_mhz);
		if (best < 0 || off < best_off) {
			best     = i;
			best_off = off;
		}
	}
	return best;
}

static void kms_set_mode(App *a, const FnlMode *mode)
{
	struct BackendState *p = a->backend_state;
	const int i            = mode_find(p, mode);
	if (i >= 0 && i != p->mode_cur)
		sel_set(a, KMS_SEL(p->out_cur, i));
}

/* Output `o` at its preferred mode, or where that would not show its first
 * that would. */
static void output_pick(App *a, int o)
{
	struct BackendState *p = a->backend_state;
	struct BackendHostState *hs = p->host;
	int m                  = hs->outputs[o].mode_pref;
	for (int i = 0;
		 kms_sel_is_refused(p, KMS_SEL(o, m)) && i < hs->outputs[o].mode_count;
		 i++)
		m = i;
	if (!kms_sel_is_refused(p, KMS_SEL(o, m)))
		sel_set(a, KMS_SEL(o, m));
}

/* The outputs a window other than `a` holds, a bit each: a second window is
 * a second output, never a second card, and no two windows share one
 * (DESIGN *A second window*). */
/* [any] The outputs other windows hold: the host's held set less `own`, the
 * asking window's output (-1 for a window that holds none yet). The set is
 * the host's, not the registry's: Host::windows is the main thread's to walk,
 * and the titlebar's lists are drawn on the render thread while
 * fnl_output_count() is [any]. */
static uint32_t held_by_others(const App *a, int own)
{
	const struct BackendHostState *hs = app_backend_host((App *)a);
	uint32_t held = fnl_atomic_load_u32(&hs->outputs_held);
	if (own >= 0 && own < KMS_MAX_OUTPUTS)
		held &= ~(1u << (unsigned)own);
	return held;
}

/* An output that is away has no monitor for the kernel to light, so the
 * move is owed until it is back (outputs_rescan()); a later request
 * replaces the one owed. */
static void kms_set_output(App *a, const char *name)
{
	struct BackendState *p = a->backend_state;
	const int count        = (int)fnl_atomic_load_u32(&p->host->output_count);
	const uint32_t gone    = fnl_atomic_load_u32(&p->host->outputs_gone);
	const uint32_t held    = held_by_others(a, p->out_cur);
	for (int o = 0; o < count; o++) {
		if (strcmp(p->host->outputs[o].name, name) != 0 || o == p->out_cur ||
			((gone >> o) & 1u))
			continue;
		if ((held >> o) & 1u) {
			fnl_log(FNL_LOG_INFO,
					"KMS: %s is another window's; the window stays on %s",
					p->host->outputs[o].name,
					p->host->outputs[p->out_cur].name);
			return;
		}
		/* A request that names an output this window can act on replaces the
		 * one owed; a refused one -- another window's output -- is not the
		 * owed window's to lose, whichever window asked. */
		p->host->away_owed     = -1;
		p->host->away_owed_app = NULL;
		if ((fnl_atomic_load_u32(&p->host->outputs_away) >> o) & 1u) {
			p->host->away_owed     = o;
			p->host->away_owed_app = a;
			fnl_log(FNL_LOG_INFO,
					"KMS: %s is away; the window goes there "
					"when it is back",
					p->host->outputs[o].name);
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
	const int sel                = (int)fnl_atomic_load_u32(&p->sel_want);
	const KmsOutput *o           = kms_out(p, sel);
	int n                        = 0;
	*current                     = -1;
	for (int i = 0; i < o->mode_count && n < max; i++) {
		if (kms_sel_is_refused(p, KMS_SEL(KMS_SEL_OUT(sel), i)))
			continue;
		if (i == KMS_SEL_MODE(sel))
			*current = n;
		out[n].w_px        = o->modes[i].hdisplay;
		out[n].h_px        = o->modes[i].vdisplay;
		out[n].refresh_mhz = kms_refresh_mhz(&o->modes[i]);
		n++;
	}
	return n;
}

static int kms_outputs(const App *a, char (*names)[OUTPUT_NAME_MAX], int max,
					   int *current)
{
	const struct BackendState *p = a->backend_state;
	const int now          = KMS_SEL_OUT(fnl_atomic_load_u32(&p->sel_want));
	const uint32_t refused = fnl_atomic_load_u32(&p->outputs_refused) |
							 fnl_atomic_load_u32(&p->host->outputs_gone);
	/* The outputs no other window holds, and this window's own. */
	const uint32_t held = held_by_others(a, now);
	const int count = (int)fnl_atomic_load_u32(&p->host->output_count);
	int n           = 0;
	*current        = -1;
	for (int o = 0; o < count && n < max; o++) {
		if ((refused >> o) & 1u)
			continue;
		if (o != now && ((held >> o) & 1u))
			continue;
		if (o == now)
			*current = n;
		snprintf(names[n], OUTPUT_NAME_MAX, "%s", p->host->outputs[o].name);
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

static void uevent_open(struct BackendHostState *hs)
{
#if defined(__linux__)
	hs->uevent_fd = socket(AF_NETLINK,
							SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
							NETLINK_KOBJECT_UEVENT);
	if (hs->uevent_fd < 0) {
		fnl_log(FNL_LOG_WARN,
				"KMS: no uevents (%s); a monitor plugged in "
				"later is not seen",
				strerror(errno));
		return;
	}
	struct sockaddr_nl at = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	if (bind(hs->uevent_fd, (struct sockaddr *)&at, sizeof at) < 0 ||
		!loop_add(&hs->loop, hs->uevent_fd)) {
		fnl_log(FNL_LOG_WARN,
				"KMS: no uevents (%s); a monitor plugged in "
				"later is not seen",
				strerror(errno));
		close(hs->uevent_fd);
		hs->uevent_fd = -1;
	}
#else
	/* No kobject uevent group here: outputs and inputs are looked at again
	 * on rescan_due instead. */
	(void)hs;
	fnl_log(FNL_LOG_INFO,
			"KMS: no uevents here; monitors are rescanned");
#endif
}

#if defined(__linux__)
/* Whether a uevent is a hotplug of this card: its strings, NUL-separated
 * after the "action@path" header, carry SUBSYSTEM=drm, HOTPLUG=1 and the
 * card's DEVNAME. The host's, like the socket the pump reads it from. */
static bool uevent_is_ours(const struct BackendHostState *hs, const char *buf,
						   size_t n)
{
	char dev[24];
	snprintf(dev, sizeof dev, "DEVNAME=dri/card%d", hs->card_index);
	bool drm = false, hotplug = false, card = false;
	for (size_t at = 0; at < n;) {
		const char *s    = buf + at;
		const size_t len = strnlen(s, n - at);
		drm |= strcmp(s, "SUBSYSTEM=drm") == 0;
		hotplug |= strcmp(s, "HOTPLUG=1") == 0;
		card |= strcmp(s, dev) == 0;
		at += len + 1;
	}
	return drm && hotplug && card;
}
#endif

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
	const int count = (int)fnl_atomic_load_u32(&p->host->output_count);
	for (int s = count - 1; s >= 0; s--) {
		const KmsOutput *h = &p->host->outputs[s];
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
	struct BackendHostState *hs = p->host;
	const int count = (int)fnl_atomic_load_u32(&hs->output_count);
	for (int s = 0; s < count; s++)
		if (!((gone >> s) & 1u) && hs->outputs[s].connector_id == connector_id)
			return s;
	return -1;
}

/* Where the window goes when its output is gone: the first output not in
 * `skip` -- those gone, and those away -- nor refused, at its preferred
 * mode or else its first that is not; -1 when there is none. */
static int sel_elsewhere(const struct BackendState *p, uint32_t skip)
{
	const int count = (int)fnl_atomic_load_u32(&p->host->output_count);
	for (int o = 0; o < count; o++) {
		if ((skip >> o) & 1u)
			continue;
		int m = p->host->outputs[o].mode_pref;
		for (int i = 0; kms_sel_is_refused(p, KMS_SEL(o, m)) &&
						i < p->host->outputs[o].mode_count;
			 i++)
			m = i;
		if (!kms_sel_is_refused(p, KMS_SEL(o, m)))
			return KMS_SEL(o, m);
	}
	return -1;
}

/* The earliest time an output now away counts as gone; 0 with none away. The
 * host's, not one window's: the away set changes on the pump's thread alone,
 * for any output of the card. */
static double away_next(const struct BackendHostState *hs)
{
	double next = 0.0;
	for (int s = 0; s < KMS_MAX_OUTPUTS; s++)
		if (((fnl_atomic_load_u32(&hs->outputs_away) >> s) & 1u) &&
			(next == 0.0 || hs->away_until[s] < next))
			next = hs->away_until[s];
	return next;
}

/* Whether any window shows on output `out`: the shorter away time is a lit
 * output's, and a lit output is one a window is on -- whichever window, so the
 * pump asks the registry rather than the one it was handed. */
static bool output_lit(const App *a, int out)
{
	for (const App *w = a->host->windows; w; w = w->next) {
		const struct BackendState *q = w->backend_state;
		if (q && KMS_SEL_OUT(fnl_atomic_load_u32(&q->sel_want)) == out)
			return true;
	}
	return false;
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
	struct BackendHostState *hs = p->host;
	uint32_t ids[KMS_MAX_CONNECTORS];
	/* A list the kernel would not give is no connector read, but the away
	 * still run out below, or the pump would wake for them for ever. */
	int n                 = card_connectors(hs->card_fd, ids);
	const bool listed_all = n >= 0 && n < KMS_MAX_CONNECTORS;
	if (n < 0)
		n = 0;
	uint32_t gone           = fnl_atomic_load_u32(&hs->outputs_gone);
	const uint32_t was_gone = gone;
	uint32_t away           = fnl_atomic_load_u32(&hs->outputs_away);
	/* Slots that left and came back with the modes they had: the CRTC may
	 * have gone dark with the monitor, as a gone output's has, so the
	 * window on one sets its mode again below. */
	uint32_t revived = 0;
	bool came = false, back = false;
	/* A connector the card no longer lists -- a DisplayPort MST sink's,
	 * which goes with its monitor -- is unplugged too, away or not; unless
	 * the list was cut at KMS_MAX_CONNECTORS, when not being on it says
	 * nothing. */
	const int count =
		listed_all ? (int)fnl_atomic_load_u32(&hs->output_count) : 0;
	for (int s = 0; s < count; s++) {
		bool listed = false;
		for (int i = 0; i < n && !listed; i++)
			listed = ids[i] == hs->outputs[s].connector_id;
		if (!listed && !((gone >> s) & 1u)) {
			gone |= 1u << s;
			fnl_log(FNL_LOG_INFO, "KMS: %s unplugged", hs->outputs[s].name);
		}
	}
	for (int i = 0; i < n; i++) {
		KmsOutput o;
		uint32_t type = 0;
		bool read;
		const bool on = output_read(hs->card_fd, ids[i], &o, &type, &read);
		if (!read)
			continue;
		int live = slot_live(p, ids[i], gone);
		if (live >= 0 && on && ((away >> live) & 1u)) {
			away &= ~(1u << live);
			if (modes_alike(&hs->outputs[live], &o)) {
				revived |= 1u << live;
				back = true;
				continue;
			}
			gone |= 1u << live;
			fnl_log(FNL_LOG_INFO, "KMS: %s unplugged", hs->outputs[live].name);
			live = -1;
		}
		if (live >= 0 && !on) {
			if (!((away >> live) & 1u)) {
				away |= 1u << live;
				hs->away_until[live] =
					now_seconds() +
					(output_lit(a, live) ? KMS_AWAY_LIT_S : KMS_AWAY_S);
				if (!hs->away_said)
					fnl_log(FNL_LOG_INFO,
							"KMS: %s left; an output counts "
							"as unplugged once it has been away %g s, %g s "
							"if the window is on it, since a monitor leaves "
							"the bus as it sleeps and as it wakes",
							hs->outputs[live].name, KMS_AWAY_S, KMS_AWAY_LIT_S);
				hs->away_said = true;
			}
			continue;
		}
		if (live >= 0 || !on)
			continue;
		int s            = slot_held(p, &o, gone);
		const bool again = s >= 0;
		if (again) {
			gone &= ~(1u << s);
		} else {
			s = (int)fnl_atomic_load_u32(&hs->output_count);
			if (s >= KMS_MAX_OUTPUTS) {
				if (!hs->slots_warned)
					fnl_log(FNL_LOG_WARN,
							"KMS: %s plugged in, and no "
							"slot is left for it: %d monitors have been "
							"seen",
							o.name, KMS_MAX_OUTPUTS);
				hs->slots_warned = true;
				continue;
			}
			hs->outputs[s] = o;
			fnl_atomic_store_u32(&hs->output_count, (uint32_t)s + 1);
		}
		came = true;
		fnl_log(FNL_LOG_INFO, "KMS: %s plugged in%s, %d mode%s",
				hs->outputs[s].name, again ? " again" : "",
				hs->outputs[s].mode_count,
				hs->outputs[s].mode_count == 1 ? "" : "s");
	}
	/* Away too long, or gone some other way meanwhile: unplugged. */
	const double now = now_seconds();
	for (int s = 0; s < KMS_MAX_OUTPUTS; s++) {
		if (!((away >> s) & 1u))
			continue;
		if ((gone >> s) & 1u) {
			away &= ~(1u << s);
		} else if (now >= hs->away_until[s]) {
			away &= ~(1u << s);
			gone |= 1u << s;
			fnl_log(FNL_LOG_INFO, "KMS: %s unplugged", hs->outputs[s].name);
		}
	}
	fnl_atomic_store_u32(&hs->outputs_away, away);
	/* A move owed to an output away: made now it is back, dropped if it is
	 * gone instead. The window that asked is the one it was owed to. */
	int owed = -1;
	if (hs->away_owed >= 0 && !((away >> hs->away_owed) & 1u)) {
		if ((gone >> hs->away_owed) & 1u)
			fnl_log(FNL_LOG_INFO,
					"KMS: %s did not come back; the window "
					"stays where it is",
					hs->outputs[hs->away_owed].name);
		else
			owed = hs->away_owed;
		hs->away_owed = -1;
	}
	if (gone == was_gone && !came && owed < 0 && !back)
		return;
	fnl_atomic_store_u32(&hs->outputs_gone, gone);

	if (owed >= 0) {
		/* The window that asked is the one it was owed to; a window gone
		 * since takes the move with it rather than giving it to whichever
		 * window this pump was handed. */
		App *tw = hs->away_owed_app;
		bool live = false;
		for (App *w = a->host->windows; w && !live; w = w->next)
			live = w == tw && w->backend_state;
		if (live)
			output_pick(tw, owed);
	}
	hs->away_owed_app = NULL;

	/* Every window whose output went moves to one still on -- the first no
	 * other window holds -- and one whose output came back sets its mode
	 * again (its CRTC may have gone dark with the monitor). A window with
	 * nowhere to go draws nothing until an output comes
	 * (kms_window_on_screen()). */
	for (App *w = a->host->windows; w; w = w->next) {
		struct BackendState *q = w->backend_state;
		if (!q)
			continue;
		const int out = KMS_SEL_OUT(fnl_atomic_load_u32(&q->sel_want));
		if ((gone >> out) & 1u) {
			const int to = sel_elsewhere(q, gone | away | held_by_others(w, out));
			if (to >= 0)
				sel_set(w, to);
			else
				fnl_log(FNL_LOG_INFO,
						"KMS: %s has no output to show on; it waits for "
						"one",
						hs->outputs[out].name);
			continue;
		}
		if ((((was_gone | revived) >> out) & 1u) &&
			!((gone >> out) & 1u)) {
			fnl_atomic_store_u32(&q->scan_resets,
								 fnl_atomic_load_u32(&q->scan_resets) + 1);
			app_surface_remake(w);
			cursor_moved(w);
			publish_mode(w);
		}
	}
	/* The lists changed under the titlebar -- every window's, since the
	 * outputs are the host's card's and each bar draws the same list. */
	for (App *w = a->host->windows; w; w = w->next)
		fnl_invalidate(w);
}

/* ---- the seam ------------------------------------------------------------ */

/* The host's half: the card and its outputs, the event loop, the inputs and
 * the layout. One card a process -- SET_MASTER succeeds for one holder --
 * and one keyboard and pointer, so these are the host's; a window is a
 * selection of one of the card's outputs and its own scanout. A machine
 * this backend refuses (no output, a card in another program's hands) is a
 * display the caller steps past, so every failure here is FNL_ERR_NO_DISPLAY
 * up to a real error. */
static void kms_host_close(App *a);

static FnlResult kms_host_open(App *a)
{
	struct BackendHostState *hs = calloc(1, sizeof *hs);
	if (!hs)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the KMS host state");
	a->host->backend_host = hs;
	/* The modifier roles of the usual keys, before any layout is read: a
	 * keymap that fails to load keeps Shift, Ctrl, Alt and the locks. */
	keymap_note_roles(hs);
	loop_init(&hs->loop);
	hs->wake.fd = hs->wake.wfd = -1;
	hs->card_fd = hs->inotify_fd = hs->uevent_fd = -1;
	hs->away_owed                               = -1;
	fnl_mutex_init(&hs->flip_lock);
	fnl_mutex_init(&hs->crtc_lock);
	fnl_mutex_init(&hs->ev_lock);

	/* Every step conditional, and every failure unwinds through
	 * kms_host_close() here: a NO_DISPLAY refusal (no output, a card held by
	 * another program) makes fnl_init() step to the next backend without
	 * calling host_close() on this one. */
	FnlResult rc = loop_wake_create(&hs->wake);
	/* The card, and the outputs the open found on it: the window picks
	 * one of them next (kms_window_open). */
	if (rc == FNL_OK)
		rc = open_output(hs);
	if (rc < 0) {
		kms_host_close(a);
		return rc;
	}
	/* The card's atomic state, where the driver offers it. NULL leaves every
	 * mode set, flip and darkening to the legacy ioctls, which is nouveau
	 * without nouveau.atomic=1 and any driver that never took atomic. */
	hs->atomic = kms_atomic_enable(hs->card_fd);
	if (hs->atomic)
		fnl_log(FNL_LOG_INFO, "KMS: atomic commits on card%d", hs->card_index);

#if defined(__linux__)
	hs->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (hs->inotify_fd < 0) {
		kms_host_close(a);
		return fnl_fail(FNL_ERR_SYSTEM, "inotify_init1() failed: %s",
						strerror(errno));
	}
	/* Watched before the scan, so a device that appears between the two is
	 * reported rather than missed; inputs_scan() takes each node once. */
	if (inotify_add_watch(hs->inotify_fd, "/dev/input",
						  IN_CREATE | IN_ATTRIB) < 0)
		fnl_log(FNL_LOG_WARN,
				"KMS: /dev/input is not watched (%s); a "
				"device plugged in later is not read",
				strerror(errno));
	rc = loop_create(&hs->loop, hs->inotify_fd, hs->wake.fd);
#else
	/* No inotify here: the set watches the wakeup, and /dev/input and the
	 * card's outputs are looked at again on a deadline. */
	hs->inotify_fd = -1;
	hs->rescan_due = now_seconds() + KMS_RESCAN_S;
	rc = loop_create(&hs->loop, -1, hs->wake.fd);
#endif
	if (rc < 0) {
		kms_host_close(a);
		return rc;
	}
	uevent_open(hs);

	keymap_load(hs);
	return FNL_OK;
}

/* The window's half: the selection it is, its cursor plane and its initial
 * figures. The host's card, inputs and event loop are already up. */
static FnlResult kms_window_open(App *a)
{
	struct BackendHostState *hs = app_backend_host(a);
	/* A window is an output, and one card is a process's: a second window is
	 * a second output, never a second card, so it takes an output no other
	 * window holds; two windows on one output are refused for want of a way
	 * to show both (DESIGN *A second window*). */
	int out;
	if (a->host->windows == a) {
		out = hs->first_output;
	} else {
		const uint32_t held = held_by_others(a, -1);
		/* Away as well as gone: an output whose monitor is asleep is still
		 * listed, and opening a window on it shows nothing for as long as
		 * the away lasts. */
		const uint32_t gone =
			fnl_atomic_load_u32(&hs->outputs_gone) |
			fnl_atomic_load_u32(&hs->outputs_away);
		const int n         = (int)fnl_atomic_load_u32(&hs->output_count);
		out                 = -1;
		for (int o = 0; o < n && out < 0; o++)
			if (!((gone >> o) & 1u) && !((held >> o) & 1u))
				out = o;
		if (out < 0)
			return fnl_fail(FNL_ERR_UNSUPPORTED,
							"KMS: every output is another window's; a second "
							"window needs one free (DESIGN *A second window*)");
	}
	struct BackendState *p = calloc(1, sizeof *p);
	if (!p)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the KMS backend state");
	a->backend_state = p;
	p->host          = hs;
	/* In the host's flip list from before its first frame: a completion is
	 * routed by the list, whichever window's thread reads it. */
	p->flip_app = a;
	fnl_mutex_lock(&hs->flip_lock);
	p->flip_next     = hs->flip_windows;
	hs->flip_windows = p;
	fnl_mutex_unlock(&hs->flip_lock);
	/* A second window means the direct-scanout display cannot be shared on
	 * the one device: the whole host draws by the copy path from here, and a
	 * window already showing lets its display go on its next build
	 * (surface_kms.c). Already on: the remakes are owed from the window that
	 * turned it on, and this one builds on the copy path anyway. */
	if (a->host->windows != a &&
		!fnl_atomic_load_bool(&hs->force_copy)) {
		fnl_atomic_store_bool(&hs->force_copy, true);
		p->force_copy_set = true;
		for (App *w = a->host->windows; w; w = w->next)
			app_surface_remake(w);
	}
	/* This window's selection: its output at the preferred mode, until a move
	 * says otherwise. */
	const int sel = KMS_SEL(out, hs->outputs[out].mode_pref);
	fnl_atomic_store_u32(&p->sel_want, (uint32_t)sel);
	fnl_atomic_store_u32(&p->sel_good, (uint32_t)sel);
	/* The host's held set: this window's output from here. */
	outputs_hold(hs, -1, out);
	p->disp_out = p->surf_sel = p->scan_sel = -1;
	mode_adopt(p, sel);
	cursor_init(p);
	/* What the output can show, for a scene's tone map: its own peak where
	 * the EDID names one, in SDR-white units at PQ's 203-nit white. */
	if (hs->outputs[out].hdr_ok)
		app_publish_peak(a, (float)(hs->outputs[out].max_lum
										? hs->outputs[out].max_lum
										: 1000u) /
								203.0f);

	/* The window is the output, and nothing will ever say otherwise. */
	a->is_maximized = true;
	publish_mode(a);

	/* The pointer in the middle of this window's output, and shown there if
	 * this window is the one the keyboard and pointer are with: the first
	 * window, or one the pointer has since come to (focus_set()). */
	p->ptr_x = p->mode_w / 2.0;
	p->ptr_y = p->mode_h / 2.0;
	a->ptr_x = px_to_ptf(p->ptr_x, want_s120(a));
	a->ptr_y = px_to_ptf(p->ptr_y, want_s120(a));
	if (!hs->focus) {
		hs->focus = a;
		hs->gx    = p->mode_w / 2.0;
		hs->gy    = p->mode_h / 2.0;
		kms_cursor_changed(a);
	}

	/* The devices, now that there is a window whose held set they seed: a
	 * key already down is seeded without acting on it. A second window finds
	 * them already open and grabbed. */
	inputs_scan(a);
	if (!hs->input_count)
		fnl_log(FNL_LOG_WARN, "KMS: no keyboard or pointer this user may "
							  "read (/dev/input/event*, group input)");

	/* The keyboard is the focused window's: the first has it, a second waits
	 * for the pointer to come to its output. */
	app_publish_focus(a, hs->focus == a);
	app_post_focus(a, hs->focus == a);
	return FNL_OK;
}

/* The window's close: its scanout and its part of the display's state. The
 * host's card, inputs and event loop stay for the windows beside it and go
 * in kms_host_close(). */
static void kms_window_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p)
		return;
	/* Whether this window ever joined the host's registry. A creation that
	 * fails after its output was taken has not, and must not give the
	 * console back over the windows that are still open. */
	bool joined = false;
	for (const App *w = a->host->windows; w && !joined; w = w->next)
		joined = w == a;
	/* The output it held is the host's no longer, joined or not. */
	outputs_hold(p->host, p->out_cur, -1);
	/* The keyboard and pointer move to a window still open, before this
	 * one's cursor and state go. */
	if (p->host->focus == a) {
		App *next = NULL;
		for (App *w = a->host->windows; w && !next; w = w->next)
			if (w != a && w->backend_state)
				next = w;
		focus_set(a, next);
	}
	if (fnl_atomic_load_ptr(&p->host->display_owner) == a)
		fnl_atomic_store_ptr(&p->host->display_owner, NULL);
	/* A move to another output asked for by this window goes with it: the
	 * rescan would otherwise give its address to a later window. */
	if (p->host->away_owed_app == a) {
		p->host->away_owed     = -1;
		p->host->away_owed_app = NULL;
	}
	cursor_free(p);
	if (p->host->card_fd >= 0) {
		/* The CRTC and scan ioctls below are the render thread's under
		 * crtc_lock (kms_scan_show(), kms_output_off()): a close on the
		 * main thread that runs them without it races another window's
		 * flip -- an RMFB or SETCRTC on a buffer that thread is mid-flip
		 * on. The lock is the outermost, as the order has it. */
		fnl_mutex_lock(&p->host->crtc_lock);
		flip_wait(a, FLIP_WAIT_MS);
		flip_give_up(a);
		/* Only the last window gives the console back, and only one that
		 * joined the host: a CRTC restored while another window still shows
		 * on the card would take that window's output with it. */
		if (joined && app_backend_host(a) &&
			fnl_atomic_load_u32(&a->host->window_count) == 1)
			crtcs_restore(p);
		scan_release(p, &p->scan_old);
		scan_release(p, &p->scan);
		scan_parked_tidy(p, true);
		/* The output's colour statement goes with the window: left, it
		 * outlives this program on a connector the console or the next
		 * program now shows in. clear() destroys the blob with it. */
		if (p->hdr_set)
			scan_hdr_clear(a);
		if (p->hdr_blob) {
			struct drm_mode_destroy_blob db = { .blob_id = p->hdr_blob };
			drm_ioctl(p->host->card_fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
			p->hdr_blob = 0;
		}
		fnl_mutex_unlock(&p->host->crtc_lock);
	}
	/* A window that never joined the host took the copy path with it: the
	 * windows that are open get their display path back (surface_kms.c). */
	if (!joined && p->force_copy_set &&
		fnl_atomic_load_u32(&a->host->window_count) == 1) {
		fnl_atomic_store_bool(&p->host->force_copy, false);
		for (App *w = a->host->windows; w; w = w->next)
			app_surface_remake(w);
	}
	/* Out of the host's flip list under its lock, before the state is
	 * freed: a render thread routing a completion holds that lock while it
	 * reads the list. */
	fnl_mutex_lock(&p->host->flip_lock);
	for (struct BackendState **q = &p->host->flip_windows; *q;
		 q = &(*q)->flip_next)
		if (*q == p) {
			*q = p->flip_next;
			break;
		}
	fnl_mutex_unlock(&p->host->flip_lock);
	free(p);
	a->backend_state = NULL;
}

/* Innermost first and every step conditional: the failure path out of
 * kms_host_open() as well. */
static void kms_host_close(App *a)
{
	struct BackendHostState *hs = app_backend_host(a);
	if (!hs)
		return;
	/* The console is given back in kms_window_close(), the window's last
	 * look at the card while its selection still stands; here the card and
	 * the devices go. */
	for (int i = 0; i < hs->input_count; i++)
		close(hs->inputs[i].fd);
	hs->input_count = 0;
	kms_atomic_disable(hs->atomic);
	hs->atomic = NULL;
	if (hs->inotify_fd >= 0)
		close(hs->inotify_fd);
	if (hs->uevent_fd >= 0)
		close(hs->uevent_fd);
	if (hs->card_fd >= 0)
		close(hs->card_fd);
	/* The two a signal handler may still be writing to: see wl_host_close(). */
	if (!app_signals_installed(a)) {
		loop_destroy(&hs->loop);
		loop_wake_destroy(&hs->wake);
	}
	fnl_mutex_destroy(&hs->flip_lock);
	fnl_mutex_destroy(&hs->crtc_lock);
	fnl_mutex_destroy(&hs->ev_lock);
	free(hs);
	a->host->backend_host = NULL;
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
	if (!flip_wait(a, FLIP_DRAIN_MS))
		fnl_log(FNL_LOG_WARN,
				"KMS: a flip's event did not come in %d ms; "
				"a driver lent the node may read it",
				FLIP_DRAIN_MS);
	events_drain(a);
	/* The node is lent to a driver next, which may read a completion still
	 * owing: one given up on would never be reconciled -- the first flip
	 * after the lend would be taken for it as stale, and the bookkeeping
	 * would stay a flip behind for the window's life. So the pending flip
	 * and any stale count go with the drain; the renew owes a mode set
	 * (scan_resets), whose path drains again, and a completion arriving
	 * after this names no pending flip and is dropped. */
	fnl_mutex_lock(&p->host->flip_lock);
	p->flip_pending = false;
	p->flip_stale   = 0;
	fnl_mutex_unlock(&p->host->flip_lock);
}

static void kms_pump(App *a, int timeout_ms)
{
	struct BackendHostState *hs = app_backend_host(a);
	LoopEvent evs[LOOP_EVENTS];
	/* The pump is the host's and serves every window on it, but `a` is only
	 * whichever window fnl_pump() was handed -- under fnl_run() always the
	 * first. Its per-window work (the cursor recheck after a switch, a refused
	 * selection adopted back) must therefore run for every window on the
	 * registry, not `a` alone, or a second window's cursor would never be
	 * re-placed on its CRTC and its refused mode or output never gone back
	 * from. The host is reached through app_backend_host(a): `a` may be a
	 * window already closed while another is still going (fnl_pump()'s own
	 * comment), and the host is shared. An output away is gone once its time is
	 * up, with or without another uevent to say so, and that clock is the
	 * host's. */
	/* A second window taken away leaves the one that remains as a single
	 * window was before it: the copy path was the pair's, and the display is
	 * free again (kms_window_close() clears the owner). The count falls in
	 * core, after the window's close, so the pump is what sees it; a window
	 * still opening keeps the path until it joins. */
	if (fnl_atomic_load_bool(&hs->force_copy) &&
		fnl_atomic_load_u32(&a->host->opening) == 0 &&
		fnl_atomic_load_u32(&a->host->window_count) == 1) {
		fnl_atomic_store_bool(&hs->force_copy, false);
		for (App *w = a->host->windows; w; w = w->next)
			if (w->backend_state)
				app_surface_remake(w);
	}

	const double away_at = away_next(hs);
	int wait_ms          = -1;
	for (App *w = a->host->windows; w; w = w->next) {
		const struct BackendState *q = w->backend_state;
		if (q && q->cursor_recheck > 0 &&
			(wait_ms < 0 || CURSOR_RECHECK_MS < wait_ms))
			wait_ms = CURSOR_RECHECK_MS;
	}
#if defined(__linux__)
	const double rescan_at = -1.0;
#else
	const double rescan_at = hs->rescan_due;
#endif
	const int ms =
		loop_wait_ms(fnl_atomic_load_u32(&hs->outputs_away) ? away_at : -1.0,
								rescan_at);
	if (ms >= 0 && (wait_ms < 0 || ms < wait_ms))
		wait_ms = ms;
	/* And by the caller's own bound, for a loop that is not fnl_run()'s
	 * (fnl_pump()). */
	if (timeout_ms >= 0 && (wait_ms < 0 || timeout_ms < wait_ms))
		wait_ms = timeout_ms;
	const int n = loop_wait(&hs->loop, evs, LOOP_EVENTS, wait_ms);
	for (App *w = a->host->windows; w; w = w->next) {
		struct BackendState *q = w->backend_state;
		if (q && q->cursor_recheck > 0)
			cursor_recheck(w);
	}
	if (fnl_atomic_load_u32(&hs->outputs_away) && now_seconds() >= away_at)
		outputs_rescan(a);
#if !defined(__linux__)
	/* No uevent socket and no inotify: a monitor or a keyboard that came or
	 * went is found by looking again, on the pump's thread, at this rate. */
	if (now_seconds() >= hs->rescan_due) {
		outputs_rescan(a);
		for (App *w = a->host->windows; w; w = w->next)
			if (w->backend_state) {
				inputs_scan(w);
				break;
			}
		hs->rescan_due = now_seconds() + KMS_RESCAN_S;
	}
#endif
	for (int i = 0; i < n; i++) {
		const int fd = evs[i].fd;
		if (fd == hs->wake.fd) {
			loop_wake_drain(&hs->wake);
			/* The wake names no window: any window the render thread marked
			 * refused is the one to adopt back. */
			for (App *w = a->host->windows; w; w = w->next)
				if (w->backend_state)
					sel_refused_seen(w);
		}
#if defined(__linux__)
		else if (fd == hs->inotify_fd) {
			inputs_hotplug(a);
		} else if (fd == hs->uevent_fd) {
			/* Every uevent there is, drained; one rescan for any number of
			 * this card's. */
			char buf[4096];
			bool ours = false, gpu = false;
			ssize_t got;
			while ((got = recv(hs->uevent_fd, buf, sizeof buf - 1, 0)) > 0) {
				buf[got] = '\0';
				ours |= uevent_is_ours(hs, buf, (size_t)got);
				gpu |= loop_uevent_is_gpu(a, buf, (size_t)got);
			}
			/* Every window's, as the Wayland backend's: the GPU list
			 * is a window's, and each draws with the GPU that came
			 * or went. */
			if (gpu)
				for (App *w = a->host->windows; w; w = w->next)
					app_gpu_hotplug(w);
			/* An overrun lost uevents nobody can read now; one of them may
			 * have been ours. */
			ours |= got < 0 && errno == ENOBUFS;
			if (ours)
				outputs_rescan(a);
		}
#endif
		else {
			KmsInput *d = input_by_fd(hs, fd);
			if (d && !input_read(a, d))
				input_remove(a, d);
		}
	}
}

static void kms_wakeup(App *a)
{
	struct BackendHostState *hs = app_backend_host(a);
	if (hs && hs->wake.fd >= 0)
		loop_wake(&hs->wake);
}

/* The output is ours for as long as the program runs -- the keyboard is
 * grabbed, so nobody switches the console away from under it -- and for as
 * long as its monitor is plugged in: with it gone and none to move to, the
 * window draws nothing until one comes (outputs_rescan()). */
static bool kms_window_on_screen(const App *a)
{
	const struct BackendState *p = a->backend_state;
	const int out = KMS_SEL_OUT(fnl_atomic_load_u32(&p->sel_want));
	return !((fnl_atomic_load_u32(&p->host->outputs_gone) >> out) & 1u);
}

/* Nothing can change the size but a mode (set_mode), so the answer is the
 * size there is. */
static void kms_request_size(App *a, int w_pt, int h_pt)
{
	(void)w_pt;
	(void)h_pt;
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
	(void)a;
	(void)edge;
}

static void kms_capture(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->captured)
		return;
	p->captured         = true;
	a->pointer_captured = true;
	for (int i = 0; i < p->host->input_count; i++)
		p->host->inputs[i].have_last = false;
	kms_cursor_changed(a);
	app_reticle_start(a);
	app_reticle_publish(a);
}

/* The pointer comes back where the reticle is. */
static void kms_release(App *a)
{
	struct BackendState *p      = a->backend_state;
	struct BackendHostState *hs = p->host;
	if (!p->captured)
		return;
	int cx, cy;
	app_content_px_rect(a, &cx, &cy, NULL, NULL);
	p->ptr_x            = clampd(cx + a->cur_px, 0.0, p->mode_w - 1.0);
	p->ptr_y            = clampd(cy + a->cur_py, 0.0, p->mode_h - 1.0);
	/* The desktop point follows the released reticle. Captured motion kept
	 * only the window's position, so the host's still names where the
	 * capture began, and the first relative report after this would put the
	 * pointer back there. */
	double ox = 0.0, tw, th;
	for (App *o = a->host->windows; o && o != a; o = o->next) {
		const struct BackendState *q = o->backend_state;
		if (q && kms_window_on_screen(o))
			ox += q->mode_w;
	}
	desktop_size(a, &tw, &th);
	hs->gx              = clampd(ox + p->ptr_x, 0.0, tw - 1.0);
	hs->gy              = clampd(p->ptr_y, 0.0, th - 1.0);
	p->captured         = false;
	a->pointer_captured = false;
	for (int i = 0; i < p->host->input_count; i++)
		p->host->inputs[i].have_last = false;
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
		"WAYLAND_DISPLAY",
		"WAYLAND_SOCKET",
		"DISPLAY",
	};
	for (size_t i = 0; i < sizeof SERVERS / sizeof SERVERS[0]; i++) {
		const char *v = getenv(SERVERS[i]);
		if (v && v[0])
			return "a window server's session";
	}
	return NULL;
}

const FnlBackendVtbl fnl_backend_kms = {
	.name            = "KMS",
	.id              = BACKEND_KMS,
	.key             = "kms",
	.absent          = kms_absent,
	.host            = backend_noop_host,
	.shared_driver   = backend_noop_shared_driver,
#if defined(__linux__)
	.gpu_drivers     = gpu_driver_list,
	.gpu_driver_set  = gpu_driver_bind,
	.gpu_driver_held = gpu_driver_held,
#else
	/* The NVIDIA/nouveau move is Linux's: sysfs, systemd's socket and the
	 * module loader's tree. There is no move here, so no GPU is offered. */
	.gpu_drivers     = backend_noop_gpu_drivers,
	.gpu_driver_set  = backend_noop_gpu_driver_set,
	.gpu_driver_held = backend_noop_gpu_driver_held,
#endif
	/* No resize edges: there is nothing to resize. */
	.grab_ring = 0,
	/* The present is the throttle; a display swapchain flips on vblank. */
	.invites = false,
	/* The window is the mode: no minimize or maximize, and its size is the
	 * output's mode, chosen with set_mode and set_output. */
	.is_output            = true,
	/* Not split yet: one open() takes the card and its first output. */
	/* The process's: the card and its outputs, the event loop, the inputs
	 * and the layout, made once; a window is a selection of one output and
	 * its own scanout on them. */
	.host_open            = kms_host_open,
	.host_close           = kms_host_close,
	.window_open          = kms_window_open,
	.window_close         = kms_window_close,
	.pump                 = kms_pump,
	.wakeup               = kms_wakeup,
	.check_error          = backend_noop_check_error,
	.flush                = backend_noop,
	.present_pixels       = kms_present_pixels,
	.set_geometry         = backend_noop_set_geometry,
	.ack_configure        = backend_noop_ack_configure,
	.frame_request        = backend_noop,
	.frame_cancel         = backend_noop,
	.present_window_renew = kms_present_window_renew,
	.present_window_hold  = backend_noop,
	.frame_wait           = backend_frame_wait_shown,
	.window_on_screen     = kms_window_on_screen,
	.apply_scale          = backend_noop_apply_scale,
	.request_size         = kms_request_size,
	.nudge_size           = backend_noop,
	.set_fullscreen       = kms_set_fullscreen,
	.set_maximized        = kms_set_maximized,
	.minimize             = backend_noop,
	.modes                = kms_modes,
	.outputs              = kms_outputs,
	.set_mode             = kms_set_mode,
	.set_output           = kms_set_output,
	.begin_move           = backend_noop,
	.begin_resize         = kms_begin_resize,
	.apply_size_bounds    = kms_apply_size_bounds,
	.buffer_granularity   = backend_noop_buffer_granularity,
	.update_scale         = kms_update_scale,
	.capture_pointer      = kms_capture,
	.release_pointer      = kms_release,
	.cursor_changed       = kms_cursor_changed,
	.text_input_changed   = kms_text_input_changed,
	.clipboard_set        = kms_clipboard_set,
	.clipboard_request    = kms_clipboard_request,
	/* No window server to tell: the bar is the library's own. */
	.set_title = backend_noop_set_title,
};
