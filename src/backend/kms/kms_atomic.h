/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * KMS's atomic commits: the mode set, the page flip, an output unbound and a
 * CRTC switched off -- each one DRM_IOCTL_MODE_ATOMIC over the CRTC, the
 * connector and the primary plane together, in place of the legacy SETCRTC
 * and PAGE_FLIP (kms_atomic.c). A commit that names several objects is one
 * transaction, so a mode set and the connector that shows it land together
 * and a refusal leaves neither half applied. Opaque here: kms.c sees that it
 * worked, and no DRM type. DESIGN *KMS: no window server*.
 *
 * One call stays out of it deliberately. The display path darkens an output
 * it leaves by setting the connector's DPMS off, which keeps the connector
 * bound to its CRTC, since Mesa takes a bound connector's CRTC and the
 * primary plane it chose first (kms_output_off()). Atomic cannot leave a
 * connector bound to an inactive CRTC, so that one remains a property set.
 */

#ifndef FNL_KMS_ATOMIC_H
#define FNL_KMS_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

struct drm_mode_modeinfo;

/* The card's atomic state: its CRTCs, each with the primary plane that drives
 * it and the properties a commit names, read once at open. NULL where the
 * driver takes no atomic, and the backend then makes the legacy calls it made
 * before. */
struct KmsAtomic;

/* Enable the atomic client capability on `fd` and read the card's CRTCs and
 * primary planes. NULL where the driver offers no atomic, or no CRTC has a
 * primary plane to set a mode through. */
struct KmsAtomic *kms_atomic_enable(int fd);
void              kms_atomic_disable(struct KmsAtomic *at);

/* `mode` on `crtc` and its primary plane, showing `fb`, `connector` bound to
 * the CRTC: one commit. The mode travels as a property blob of its own,
 * freed once the commit has issued. False where the kernel refused it, as a
 * legacy set would have. */
bool kms_atomic_set(struct KmsAtomic *at, uint32_t connector, uint32_t crtc,
					const struct drm_mode_modeinfo *mode, uint32_t fb);

/* `fb` onto the CRTC's active primary plane by a page flip, `stamp` the
 * event's user_data. `async` asks it to tear rather than wait the vblank; a
 * driver that refuses it answers EINVAL and the caller flips at the vblank
 * instead. The CRTC must already be lit (kms_atomic_set()); this moves the
 * plane alone. */
bool kms_atomic_flip(struct KmsAtomic *at, uint32_t crtc, uint32_t fb,
					 bool async, uint64_t stamp);

/* An output the window left, the copy path's: its CRTC switched off and the
 * connector unbound, one commit -- what SETCRTC with no framebuffer did, and
 * what frees the CRTC for the next output. */
bool kms_atomic_unbind(struct KmsAtomic *at, uint32_t crtc, uint32_t connector);

/* A CRTC switched off with its primary plane emptied and its connectors left
 * as they are: the console's where it will not come back, and a CRTC the open
 * found lit but the window never reached. */
bool kms_atomic_crtc_off(struct KmsAtomic *at, uint32_t crtc);

/* The cursor plane over `crtc`: whether it has one, the glyph `fb` put on it
 * with its top-left at (x, y) in pixels, and it emptied. False where the CRTC
 * has no cursor plane, and the legacy DRM_IOCTL_MODE_CURSOR is then kept.
 *
 * `wait` makes the commit synchronous: it is programmed before the call
 * returns, so none is left pending on the CRTC. The display path needs that:
 * Mesa's display WSI flips on the same CRTC through a dup of this descriptor,
 * and a NONBLOCK commit of ours still in flight when Mesa's page flip is
 * issued stops the kernel taking that flip (a commit is already pending on
 * the CRTC), which Mesa reports as a lost surface and rebuilds. Our own cursor
 * commits were seen refused with EBUSY in those rounds, the same collision
 * from the other side. The copy path flips through this table under
 * at_commit's lock, so a pending commit there is nobody else's: a blocking
 * cursor commit would sit out a vblank behind the render thread's flip, which
 * is the lag f035cb9 took out. */
bool kms_atomic_cursor_at(struct KmsAtomic *at, uint32_t crtc);
bool kms_atomic_cursor_set(struct KmsAtomic *at, uint32_t crtc, uint32_t fb,
						   int x, int y, uint32_t w, uint32_t h, bool wait);
bool kms_atomic_cursor_off(struct KmsAtomic *at, uint32_t crtc, bool wait);

#endif /* FNL_KMS_ATOMIC_H */
