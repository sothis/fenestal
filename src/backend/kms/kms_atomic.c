/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * KMS's atomic commits (kms_atomic.h). Raw DRM ioctls, as the rest of the
 * backend: libdrm would be an entry on a consumer's link line. The CRTCs and
 * their primary planes are read once at open, with each plane's FB_ID,
 * CRTC_ID and the rest of its placement by name -- the kernel gives no id for
 * them any other way -- and every commit then names the CRTC (ACTIVE,
 * MODE_ID), the connector (CRTC_ID) and the plane (CRTC_ID, FB_ID, SRC_* and
 * CRTC_*), one transaction for all of them.
 *
 * A mode travels as a property blob (DRM_IOCTL_MODE_CREATEPROPBLOB), freed
 * once its commit is issued: the kernel has copied it by then, and nothing
 * here keeps one.
 *
 * The primary plane is found by its type property -- drm_plane_type, which
 * the UAPI header names only in prose -- and one a CRTC may show, by its
 * possible_crtcs bit for that CRTC's place in the card's list. The
 * connector's properties are read at each set or unbind, not kept: those are
 * a mode change and a move, never a frame.
 */

#include "core/app.h"

#include "backend/kms/kms_atomic.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

/* drm_plane_type.OVERLAY is 0; a plane's `type` property is this enum. */
#define AT_PLANE_PRIMARY 1u
#define AT_PLANE_CURSOR  2u

/* The CRTCs and planes of one card read at once, and the properties of one
 * object. A card with more CRTCs than this has the rest unset here, and its
 * modes go by the legacy calls. */
#define AT_MAX_CRTCS  16
#define AT_MAX_PLANES 32
#define AT_MAX_PROPS  64

/* One CRTC, the primary plane that drives it and the cursor plane over it. */
typedef struct {
	uint32_t crtc_id, plane_id;
	uint32_t active;   /* the CRTC's ACTIVE */
	uint32_t mode_id;  /* the CRTC's MODE_ID */
	uint32_t p_crtc_id, p_fb_id;
	uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
	uint32_t p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
	/* The cursor plane and its placement properties; 0 where the CRTC has
	 * none, and the legacy cursor ioctl is then kept. */
	uint32_t cursor_plane;
	uint32_t c_crtc_id, c_fb_id;
	uint32_t c_src_x, c_src_y, c_src_w, c_src_h;
	uint32_t c_crtc_x, c_crtc_y, c_crtc_w, c_crtc_h;
} AtCrtc;

struct KmsAtomic {
	int      fd;
	AtCrtc   crtcs[AT_MAX_CRTCS];
	int      count;
	/* One commit at a time: two windows' render threads share the card, and
	 * a mode set on one output colliding with a flip on another is EBUSY on
	 * both (the kernel's modeset locks). The table above is read only after
	 * enable(), so this guards the commits alone. */
	FnlMutex lock;
};

static int at_ioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* A property of `obj`/`kind` by name, and its current value, 0 for none. */
static uint32_t at_prop(int fd, uint32_t obj, uint32_t kind, const char *name,
						uint64_t *value)
{
	uint32_t ids[AT_MAX_PROPS];
	uint64_t vals[AT_MAX_PROPS];
	struct drm_mode_obj_get_properties gp = {
		.props_ptr       = (uintptr_t)ids,
		.prop_values_ptr = (uintptr_t)vals,
		.count_props     = AT_MAX_PROPS,
		.obj_id          = obj,
		.obj_type        = kind,
	};
	if (at_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
		return 0;
	const uint32_t n =
		gp.count_props < AT_MAX_PROPS ? gp.count_props : AT_MAX_PROPS;
	for (uint32_t i = 0; i < n; i++) {
		struct drm_mode_get_property prop = { .prop_id = ids[i] };
		if (at_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) == 0 &&
			strcmp(prop.name, name) == 0) {
			if (value)
				*value = vals[i];
			return ids[i];
		}
	}
	return 0;
}

/* A property of a CRTC, or of a plane: only the object kind differs. */
static uint32_t at_cprop(int fd, uint32_t crtc, const char *name, uint64_t *v)
{
	return at_prop(fd, crtc, DRM_MODE_OBJECT_CRTC, name, v);
}

static uint32_t at_pprop(int fd, uint32_t plane, const char *name, uint64_t *v)
{
	return at_prop(fd, plane, DRM_MODE_OBJECT_PLANE, name, v);
}

/* A plane's type: 0 overlay, 1 primary, 2 cursor, -1 where it has none. */
static int at_plane_type(int fd, uint32_t plane)
{
	uint64_t t = 0;
	if (!at_pprop(fd, plane, "type", &t))
		return -1;
	return (int)t;
}

/* The plane of this id, if it is a primary plane of the card. */
static bool at_is_primary(int fd, uint32_t plane)
{
	return at_plane_type(fd, plane) == (int)AT_PLANE_PRIMARY;
}

/* The card's CRTC ids into `crtcs`, its plane ids into `planes`, each
 * returned count; -1 where the kernel would not give them. Asked twice, the
 * second time with room, and clamped to what one read holds. */
static int at_resources(int fd, uint32_t crtcs[AT_MAX_CRTCS],
						uint32_t planes[AT_MAX_PLANES], int *ncrtc,
						int *nplane)
{
	struct drm_mode_card_res res;
	struct drm_mode_get_plane_res pres;
	memset(&res, 0, sizeof res);
	memset(&pres, 0, sizeof pres);
	if (at_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
		at_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0)
		return -1;
	uint32_t nc = res.count_crtcs, np = pres.count_planes;
	if (nc > AT_MAX_CRTCS)
		nc = AT_MAX_CRTCS;
	if (np > AT_MAX_PLANES)
		np = AT_MAX_PLANES;
	memset(&res, 0, sizeof res);
	res.count_crtcs = nc;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	memset(&pres, 0, sizeof pres);
	pres.count_planes = np;
	pres.plane_id_ptr = (uintptr_t)planes;
	if (at_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
		at_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0)
		return -1;
	*ncrtc  = (int)(res.count_crtcs < nc ? res.count_crtcs : nc);
	*nplane = (int)(pres.count_planes < np ? pres.count_planes : np);
	return 0;
}

/* Fill one AtCrtc from its CRTC, or false where it has no primary plane or
 * no property a commit needs. A primary plane is one a CRTC may show
 * (possible_crtcs' bit for its index) and that no CRTC already read has
 * taken: a driver whose primaries are all possible on every pipe would
 * otherwise give two CRTCs one plane, and the two windows' flips would fight
 * over it (EBUSY). A plane already bound to the CRTC is the driver's own
 * choice and preferred. */
static bool at_crtc_read(int fd, AtCrtc *c, const uint32_t *planes, int nplane,
						 int crtc_index, const struct KmsAtomic *at)
{
	uint32_t bound = 0, any = 0;
	for (int i = 0; i < nplane && !bound; i++) {
		if (!at_is_primary(fd, planes[i]))
			continue;
		bool taken = false;
		for (int j = 0; j < at->count && !taken; j++)
			taken = at->crtcs[j].plane_id == planes[i];
		if (taken)
			continue;
		struct drm_mode_get_plane gp = { .plane_id = planes[i] };
		if (at_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
			continue;
		if (!(gp.possible_crtcs & (1u << crtc_index)))
			continue;
		if (gp.crtc_id == c->crtc_id)
			bound = planes[i];
		else if (!any)
			any = planes[i];
	}
	c->plane_id = bound ? bound : any;
	if (!c->plane_id)
		return false;
	uint64_t v;
	const uint32_t pl = c->plane_id;
	c->active  = at_cprop(fd, c->crtc_id, "ACTIVE", &v);
	c->mode_id = at_cprop(fd, c->crtc_id, "MODE_ID", &v);
	c->p_crtc_id = at_pprop(fd, pl, "CRTC_ID", &v);
	c->p_fb_id   = at_pprop(fd, pl, "FB_ID", &v);
	c->p_src_x   = at_pprop(fd, pl, "SRC_X", &v);
	c->p_src_y   = at_pprop(fd, pl, "SRC_Y", &v);
	c->p_src_w   = at_pprop(fd, pl, "SRC_W", &v);
	c->p_src_h   = at_pprop(fd, pl, "SRC_H", &v);
	c->p_crtc_x  = at_pprop(fd, pl, "CRTC_X", &v);
	c->p_crtc_y  = at_pprop(fd, pl, "CRTC_Y", &v);
	c->p_crtc_w  = at_pprop(fd, pl, "CRTC_W", &v);
	c->p_crtc_h  = at_pprop(fd, pl, "CRTC_H", &v);
	if (!(c->active && c->mode_id && c->p_crtc_id && c->p_fb_id &&
		  c->p_src_x && c->p_src_y && c->p_src_w && c->p_src_h &&
		  c->p_crtc_x && c->p_crtc_y && c->p_crtc_w && c->p_crtc_h))
		return false;

	/* And the cursor plane over it, where the driver has one: a cursor plane
	 * a CRTC may show and that no other has taken. Missing it leaves the
	 * legacy cursor ioctl to this window. */
	uint32_t cib = 0, cia = 0;
	for (int i = 0; i < nplane; i++) {
		if (at_plane_type(fd, planes[i]) != (int)AT_PLANE_CURSOR)
			continue;
		bool taken = planes[i] == c->plane_id;
		for (int j = 0; j < at->count && !taken; j++)
			taken = at->crtcs[j].plane_id == planes[i] ||
					at->crtcs[j].cursor_plane == planes[i];
		if (taken)
			continue;
		struct drm_mode_get_plane gp = { .plane_id = planes[i] };
		if (at_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
			continue;
		if (!(gp.possible_crtcs & (1u << crtc_index)))
			continue;
		if (gp.crtc_id == c->crtc_id) {
			cib = planes[i];
			break;
		}
		if (!cia)
			cia = planes[i];
	}
	c->cursor_plane = cib ? cib : cia;
	if (c->cursor_plane) {
		const uint32_t cp = c->cursor_plane;
		c->c_crtc_id = at_pprop(fd, cp, "CRTC_ID", &v);
		c->c_fb_id   = at_pprop(fd, cp, "FB_ID", &v);
		c->c_src_x   = at_pprop(fd, cp, "SRC_X", &v);
		c->c_src_y   = at_pprop(fd, cp, "SRC_Y", &v);
		c->c_src_w   = at_pprop(fd, cp, "SRC_W", &v);
		c->c_src_h   = at_pprop(fd, cp, "SRC_H", &v);
		c->c_crtc_x  = at_pprop(fd, cp, "CRTC_X", &v);
		c->c_crtc_y  = at_pprop(fd, cp, "CRTC_Y", &v);
		c->c_crtc_w  = at_pprop(fd, cp, "CRTC_W", &v);
		c->c_crtc_h  = at_pprop(fd, cp, "CRTC_H", &v);
		if (!(c->c_crtc_id && c->c_fb_id && c->c_src_w && c->c_src_h &&
			  c->c_crtc_w && c->c_crtc_h))
			c->cursor_plane = 0;
	}
	return true;
}

struct KmsAtomic *kms_atomic_enable(int fd)
{
	struct drm_set_client_cap cap = {
		.capability = DRM_CLIENT_CAP_ATOMIC,
		.value      = 1,
	};
	if (at_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0)
		return NULL;
	uint32_t crtcs[AT_MAX_CRTCS], planes[AT_MAX_PLANES];
	int ncrtc = 0, nplane = 0;
	if (at_resources(fd, crtcs, planes, &ncrtc, &nplane) < 0)
		return NULL;
	struct KmsAtomic *at = calloc(1, sizeof *at);
	if (!at)
		return NULL;
	at->fd = fd;
	fnl_mutex_init(&at->lock);
	for (int i = 0; i < ncrtc; i++) {
		AtCrtc c = { .crtc_id = crtcs[i] };
		if (at_crtc_read(fd, &c, planes, nplane, i, at))
			at->crtcs[at->count++] = c;
	}
	if (!at->count) {
		free(at);
		return NULL;
	}
	return at;
}

void kms_atomic_disable(struct KmsAtomic *at)
{
	if (!at)
		return;
	fnl_mutex_destroy(&at->lock);
	free(at);
}

static AtCrtc *at_crtc(struct KmsAtomic *at, uint32_t crtc)
{
	for (int i = 0; i < at->count; i++)
		if (at->crtcs[i].crtc_id == crtc)
			return &at->crtcs[i];
	return NULL;
}

/* One commit, `nobj` objects each with its properties, values in the same
 * order across the two arrays. */
static bool at_commit(struct KmsAtomic *at, uint32_t flags, uint64_t stamp,
					  const uint32_t *objs, const uint32_t *counts,
					  const uint32_t *props, const uint64_t *values,
					  int nobj)
{
	struct drm_mode_atomic a = {
		.flags           = flags,
		.count_objs      = (uint32_t)nobj,
		.objs_ptr        = (uintptr_t)objs,
		.count_props_ptr = (uintptr_t)counts,
		.props_ptr       = (uintptr_t)props,
		.prop_values_ptr = (uintptr_t)values,
		.user_data       = stamp,
	};
	fnl_mutex_lock(&at->lock);
	const bool ok = at_ioctl(at->fd, DRM_IOCTL_MODE_ATOMIC, &a) == 0;
	fnl_mutex_unlock(&at->lock);
	return ok;
}

/* The connector carries the CRTC it shows; the plane carries the frame. */
bool kms_atomic_set(struct KmsAtomic *at, uint32_t connector, uint32_t crtc,
					const struct drm_mode_modeinfo *mode, uint32_t fb)
{
	AtCrtc *c = at_crtc(at, crtc);
	uint64_t cv;
	const uint32_t conn_crtc =
		at_prop(at->fd, connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &cv);
	if (!c || !conn_crtc || !fb || !mode)
		return false;
	struct drm_mode_create_blob blob = {
		.data   = (uintptr_t)mode,
		.length = (uint32_t)sizeof *mode,
	};
	if (at_ioctl(at->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0)
		return false;
	/* The CRTC takes its place in the mode; the plane its source in 16.16
	 * fixed point and its destination in pixels, both at the origin. */
	const uint64_t w = (uint64_t)mode->hdisplay, h = (uint64_t)mode->vdisplay;
	const uint32_t objs[3]   = { crtc, connector, c->plane_id };
	const uint32_t counts[3] = { 2, 1, 10 };
	const uint32_t props[13] = {
		c->active, c->mode_id,
		conn_crtc,
		c->p_crtc_id, c->p_fb_id,
		c->p_src_x, c->p_src_y, c->p_src_w, c->p_src_h,
		c->p_crtc_x, c->p_crtc_y, c->p_crtc_w, c->p_crtc_h,
	};
	const uint64_t vals[13] = {
		1, blob.blob_id,
		crtc,
		crtc, fb,
		0, 0, w << 16, h << 16,
		0, 0, w, h,
	};
	/* Checked first with TEST_ONLY, which the kernel applies to nothing: a
	 * mode or plane the driver will not take is refused before the screen
	 * changes, rather than by a set that then has to be gone back from. */
	bool ok = at_commit(at, DRM_MODE_ATOMIC_ALLOW_MODESET |
								DRM_MODE_ATOMIC_TEST_ONLY,
						0, objs, counts, props, vals, 3);
	if (ok)
		ok = at_commit(at, DRM_MODE_ATOMIC_ALLOW_MODESET, 0, objs, counts,
					   props, vals, 3);
	struct drm_mode_destroy_blob db = { .blob_id = blob.blob_id };
	at_ioctl(at->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
	return ok;
}

bool kms_atomic_flip(struct KmsAtomic *at, uint32_t crtc, uint32_t fb,
					 bool async, uint64_t stamp)
{
	AtCrtc *c = at_crtc(at, crtc);
	if (!c || !fb)
		return false;
	const uint32_t objs[1]   = { c->plane_id };
	const uint32_t counts[1] = { 2 };
	const uint32_t props[2]  = { c->p_crtc_id, c->p_fb_id };
	const uint64_t vals[2]   = { crtc, fb };
	/* A vsynced flip blocks until the plane is programmed, which the kernel
	 * serialises with the other window's commit; a tearing flip must not
	 * wait the vblank, so it is non-blocking (which atomic requires the
	 * event flag for). */
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	if (async)
		flags |= DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_ATOMIC_NONBLOCK;
	return at_commit(at, flags, stamp, objs, counts, props, vals, 1);
}

bool kms_atomic_unbind(struct KmsAtomic *at, uint32_t crtc, uint32_t connector)
{
	AtCrtc *c = at_crtc(at, crtc);
	if (!c)
		return false;
	uint64_t cv;
	const uint32_t conn_crtc =
		at_prop(at->fd, connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &cv);
	/* The CRTC off, its plane emptied and unbound from it, and the connector
	 * unbound -- the last left out where the connector has no CRTC_ID
	 * property. */
	const uint32_t objs[3]   = { crtc, c->plane_id, connector };
	const uint32_t counts[3] = { 1, 2, 1 };
	const uint32_t props[4]  = { c->active, c->p_crtc_id, c->p_fb_id,
								 conn_crtc };
	const uint64_t vals[4]   = { 0, 0, 0, 0 };
	const int nobj           = conn_crtc ? 3 : 2;
	return at_commit(at, DRM_MODE_ATOMIC_ALLOW_MODESET, 0, objs, counts, props,
					 vals, nobj);
}

bool kms_atomic_crtc_off(struct KmsAtomic *at, uint32_t crtc)
{
	AtCrtc *c = at_crtc(at, crtc);
	if (!c)
		return false;
	const uint32_t objs[2]   = { crtc, c->plane_id };
	const uint32_t counts[2] = { 1, 2 };
	const uint32_t props[3]  = { c->active, c->p_crtc_id, c->p_fb_id };
	const uint64_t vals[3]   = { 0, 0, 0 };
	return at_commit(at, DRM_MODE_ATOMIC_ALLOW_MODESET, 0, objs, counts, props,
					 vals, 2);
}

bool kms_atomic_cursor_at(struct KmsAtomic *at, uint32_t crtc)
{
	AtCrtc *c = at_crtc(at, crtc);
	return c && c->cursor_plane;
}

bool kms_atomic_cursor_set(struct KmsAtomic *at, uint32_t crtc, uint32_t fb,
						   int x, int y, uint32_t w, uint32_t h)
{
	AtCrtc *c = at_crtc(at, crtc);
	if (!c || !c->cursor_plane || !fb)
		return false;
	const uint32_t objs[1]   = { c->cursor_plane };
	const uint32_t counts[1] = { 10 };
	const uint32_t props[10] = {
		c->c_crtc_id, c->c_fb_id, c->c_src_x, c->c_src_y, c->c_src_w,
		c->c_src_h, c->c_crtc_x, c->c_crtc_y, c->c_crtc_w, c->c_crtc_h,
	};
	const uint64_t vals[10] = {
		crtc, fb, 0, 0, (uint64_t)w << 16, (uint64_t)h << 16,
		(uint64_t)(int64_t)x, (uint64_t)(int64_t)y, w, h,
	};
	return at_commit(at, 0, 0, objs, counts, props, vals, 1);
}

bool kms_atomic_cursor_off(struct KmsAtomic *at, uint32_t crtc)
{
	AtCrtc *c = at_crtc(at, crtc);
	if (!c || !c->cursor_plane)
		return false;
	const uint32_t objs[1]   = { c->cursor_plane };
	const uint32_t counts[1] = { 2 };
	const uint32_t props[2]  = { c->c_crtc_id, c->c_fb_id };
	const uint64_t vals[2]   = { 0, 0 };
	return at_commit(at, 0, 0, objs, counts, props, vals, 1);
}
