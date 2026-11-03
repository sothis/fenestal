/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The X11 backend's clipboard: ICCCM's CLIPBOARD selection on the core
 * protocol. DESIGN *The clipboard*.
 *
 * Offering: own the selection and answer SelectionRequest by writing the text
 * onto the property the asker names. A string too large for one request is
 * refused rather than sent by INCR, whose sending half is a state machine per
 * asker.
 *
 * Both the ownership and the request are taken at a server time, as ICCCM
 * wants, read off a stamp: a zero-length write to a property of the
 * window's own, whose PropertyNotify carries the time it was made.
 *
 * Asking: ConvertSelection, then SelectionNotify says where the answer is. If
 * its type is INCR, deleting the property is the signal to begin, each piece
 * arrives as a PropertyNotify, and an empty piece is the end.
 *
 * A request given up at its deadline may still be answered, late, by an
 * owner that was only slow. So each is stamped with a server time, which the
 * answer carries back, and the one after it is answered on the other of two
 * properties: a late SelectionNotify is told apart by its time, and a late
 * INCR piece lands where nothing reads it -- unread, so its owner waits
 * for a delete that never comes rather than sending the rest. Nor is the
 * given-up property deleted, which would be that delete.
 */

#include "core/app.h"

#include "backend/x11/x11_state.h"

/* Writes a stamp, and says which it is. A zero-length write changes
 * nothing and is answered with a PropertyNotify that carries the server's
 * time (x11_clip_property()). A write and not an append, which a property
 * of another format would refuse.
 *
 * 0, and nothing written, where FNL_STAMP was never interned: a write to
 * None is refused with an error and no PropertyNotify, and counted it
 * would leave every later stamp one behind -- every paste after it run to
 * the deadline. The caller then acts at once, at CurrentTime, which ICCCM
 * discourages and owners take. */
static uint32_t clip_stamp(struct BackendState *p)
{
	if (p->atom[FNL_ATOM_FNL_STAMP] == XCB_ATOM_NONE ||
		FNL_FAULT("x11-clip-unstamped")) {
		if (!p->clip_unstamped_said) {
			p->clip_unstamped_said = true;
			fnl_log(FNL_LOG_WARN, "the clipboard has no server time to "
								  "stamp with; copy and paste at "
								  "CurrentTime");
		}
		return 0;
	}
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						p->atom[FNL_ATOM_FNL_STAMP], XCB_ATOM_STRING, 8, 0,
						NULL);
	return ++p->clip_stamps_sent;
}

/* Where this request's answer is asked to be put. */
static xcb_atom_t clip_prop(const struct BackendState *p)
{
	return p->atom[p->clip_alt ? FNL_ATOM_FNL_PASTE2 : FNL_ATOM_FNL_PASTE];
}

/* The paste is over, with `data` as the answer. NULL is no text -- which is
 * also what a transfer cut short is reported as: half a clipboard is not
 * the clipboard, and may end inside a character. */
static void clip_finish(App *a, const char *data, size_t len)
{
	struct BackendState *p = a->backend_state;
	if (data && data == p->clip.buf) {
		/* The assembled pieces change hands as they are. */
		clip_in_finish(a, &p->clip, true);
	} else {
		app_clipboard_deliver(a, data, len);
		clip_in_free(&p->clip);
	}
	p->clip_waiting = p->clip_incr = p->clip_over = p->clip_asked = false;
}

/* Whatever is on the paste property now: the whole answer, or the next
 * piece of one arriving by INCR. */
static void clip_read(App *a)
{
	struct BackendState *p = a->backend_state;
	/* Deleted as it is read: under INCR that is the acknowledgement the owner
	 * waits on before it sends the next. */
	xcb_get_property_reply_t *r = xcb_get_property_reply(
		p->conn,
		xcb_get_property(p->conn, 1, p->window, clip_prop(p),
						 XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffffu),
		NULL);
	if (!r) {
		clip_finish(a, NULL, 0);
		return;
	}
	const char *data = xcb_get_property_value(r);
	const size_t len = (size_t)xcb_get_property_value_length(r);
	clip_in_renew(&p->clip);

	if (r->type == p->atom[FNL_ATOM_INCR]) {
		p->clip_incr = true;
		p->clip.len = 0;
	} else if (r->type == XCB_ATOM_NONE) {
		/* No property at all, which is not the empty one that ends an INCR
		 * transfer: an owner that wrote two pieces without waiting had both
		 * read by the first notify, and this is the second. */
		if (!p->clip_incr)
			clip_finish(a, NULL, 0);
	} else if (r->format != 8) {
		/* Atoms or words where text was asked for. */
		if (!p->clip_incr)
			clip_finish(a, NULL, 0);
		else
			p->clip_over = true;
	} else if (!p->clip_incr) {
		clip_finish(a, data, len);
	} else if (len == 0) {
		clip_finish(a, p->clip_over ? NULL : p->clip.buf, p->clip.len);
	} else if (!p->clip_over && len <= CLIP_MAX - p->clip.len &&
			   clip_in_room(&p->clip, len)) {
		/* The limit before the room: a piece past it is never kept, so the
		 * buffer is never grown for one. */
		memcpy(p->clip.buf + p->clip.len, data, len);
		(void)clip_in_grew(&p->clip, len);
	} else {
		/* Past what will be kept. The pieces are still read and deleted, so
		 * the owner is not left waiting, and the end reports no text. */
		p->clip_over = true;
	}
	free(r);
}

/* A property of this window changed: the stamp a copy or a request is
 * waiting on, or the next piece of an answer arriving by INCR. Only a new
 * value counts: our own delete of the last piece comes through here too. */
void x11_clip_property(App *a, const xcb_property_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;
	if (e->state != XCB_PROPERTY_NEW_VALUE)
		return;
	if (e->atom == p->atom[FNL_ATOM_FNL_STAMP]) {
		/* None outstanding: a write that was not ours. */
		if (p->clip_stamps_back == p->clip_stamps_sent)
			return;
		const uint32_t n = ++p->clip_stamps_back;
		if (p->clip_owning && n == p->clip_stamp_own) {
			/* Not read back by GetSelectionOwner: a server that ignores
			 * this has had the selection taken at a later time, by a later
			 * copy, which is the one that should win, and nothing here
			 * waits on knowing. */
			p->clip_owning = false;
			xcb_set_selection_owner(p->conn, p->window,
									p->atom[FNL_ATOM_CLIPBOARD], e->time);
		} else if (p->clip_waiting && !p->clip_asked &&
				   n == p->clip_stamp_paste) {
			/* With no owner the server answers at once with no property.
			 * An owner that never answers is what the deadline is for. */
			p->clip_asked = true;
			p->clip_time = e->time;
			xcb_convert_selection(p->conn, p->window,
								  p->atom[FNL_ATOM_CLIPBOARD],
								  p->atom[FNL_ATOM_UTF8_STRING],
								  clip_prop(p), e->time);
		}
	} else if (p->clip_waiting && e->atom == clip_prop(p) && p->clip_incr) {
		clip_read(a);
	}
}

/* The owner has had as long as it is given: the paste is answered with no
 * text. Called every pump turn, since nothing else arrives to say so. */
void x11_clip_expire(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->clip_waiting || !clip_in_expired(&p->clip))
		return;
	/* An owner asked may yet answer, onto the property it was given: the
	 * next request is asked for on the other. */
	if (p->clip_asked)
		p->clip_alt = !p->clip_alt;
	clip_finish(a, NULL, 0);
}

void x11_clipboard_request(App *a)
{
	struct BackendState *p = a->backend_state;
	/* One already on its way answers both -- asking again would put a
	 * second answer on the property the first is still arriving through --
	 * unless this window copied since it was asked: then the clipboard it
	 * asks for is the copy, and the one on its way is given up, its late
	 * answer left on the property no longer read. One answer still serves
	 * every waiting request, as on Wayland (wl_clipboard_request()). */
	if (p->clip_waiting && !p->clip_copied)
		return;
	if (p->clip_waiting && p->clip_asked)
		p->clip_alt = !p->clip_alt;
	p->clip_copied = false;
	p->clip_incr = p->clip_over = p->clip_asked = false;
	p->clip.len = 0;
	p->clip_waiting = true;
	clip_in_renew(&p->clip);
	/* ICCCM wants ConvertSelection stamped with a server time rather than
	 * CurrentTime, and an owner may refuse one from before it took the
	 * selection, so the time is the server's when this stamp comes back.
	 * Written after the stamp of any copy made before it, it comes back
	 * after that one, and so the ownership reaches the server first. */
	p->clip_stamp_paste = clip_stamp(p);
	if (!p->clip_stamp_paste) {
		p->clip_asked = true;
		p->clip_time = XCB_CURRENT_TIME;
		xcb_convert_selection(p->conn, p->window,
							  p->atom[FNL_ATOM_CLIPBOARD],
							  p->atom[FNL_ATOM_UTF8_STRING], clip_prop(p),
							  XCB_CURRENT_TIME);
	}
}

void x11_clipboard_set(App *a)
{
	struct BackendState *p = a->backend_state;
	/* ICCCM wants the selection taken at a server time and not at
	 * CurrentTime, and no event of the copy's reaches here to carry one, so
	 * it is taken when this stamp comes back. A copy before then supersedes
	 * it: only the latest is taken, at the latest time. Until then the
	 * previous owner keeps it, and a request answered here meanwhile
	 * already gets the new string. */
	p->clip_stamp_own = clip_stamp(p);
	p->clip_owning = p->clip_stamp_own != 0;
	if (!p->clip_owning)
		xcb_set_selection_owner(p->conn, p->window,
								p->atom[FNL_ATOM_CLIPBOARD], XCB_CURRENT_TIME);
	/* A paste still on its way asked for the clipboard before this copy and
	 * keeps the answer it asked for, unless another request follows the
	 * copy (x11_clipboard_request()): its stamp is then written after this
	 * one, so it is asked after the selection is taken. */
	p->clip_copied = p->clip_waiting;
}

void x11_selection_request(App *a, const xcb_selection_request_event_t *e)
{
	struct BackendState *p = a->backend_state;
	/* A property of None is an asker from before ICCCM 2, which means the
	 * target's own name. Refusal is this event sent with no property. */
	const xcb_atom_t prop = e->property != XCB_ATOM_NONE ? e->property
														 : e->target;
	xcb_selection_notify_event_t n;
	memset(&n, 0, sizeof n);
	n.response_type = XCB_SELECTION_NOTIFY;
	n.time      = e->time;
	n.requestor = e->requestor;
	n.selection = e->selection;
	n.target    = e->target;
	n.property  = XCB_ATOM_NONE;

	if (e->selection == p->atom[FNL_ATOM_CLIPBOARD] && a->clip_out) {
		if (e->target == p->atom[FNL_ATOM_TARGETS]) {
			const xcb_atom_t targets[3] = {
				p->atom[FNL_ATOM_TARGETS], p->atom[FNL_ATOM_UTF8_STRING],
				p->atom[FNL_ATOM_TEXT_PLAIN_UTF8] };
			xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, e->requestor,
								prop, XCB_ATOM_ATOM, 32, 3, targets);
			n.property = prop;
		} else if (e->target == p->atom[FNL_ATOM_UTF8_STRING] ||
				   e->target == p->atom[FNL_ATOM_TEXT_PLAIN_UTF8]) {
			/* In bytes; the protocol counts a request in four-byte units and
			 * the ChangeProperty header takes a few of them. */
			const size_t room =
				(size_t)xcb_get_maximum_request_length(p->conn) * 4 - 64;
			if (a->clip_out_len <= room) {
				xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE,
									e->requestor, prop, e->target, 8,
									(uint32_t)a->clip_out_len, a->clip_out);
				n.property = prop;
			}
		}
	}
	xcb_send_event(p->conn, 0, e->requestor, XCB_EVENT_MASK_NO_EVENT,
				   (const char *)&n);
}

void x11_selection_notify(App *a, const xcb_selection_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;
	if (!p->clip_asked || e->requestor != p->window ||
		e->selection != p->atom[FNL_ATOM_CLIPBOARD])
		return;
	/* ICCCM has the owner carry back the request's time, and the server
	 * does where there is no owner: any other is a request given up, its
	 * answer come late. CurrentTime is an owner that carries back nothing,
	 * which is still taken at its word rather than left to the deadline. */
	if (e->time != p->clip_time && e->time != XCB_CURRENT_TIME)
		return;
	/* And an answer on the other property is a request given up, however
	 * it is stamped: read, it would end this one with no text. */
	if (e->property != XCB_ATOM_NONE && e->property != clip_prop(p))
		return;
	if (e->property == XCB_ATOM_NONE)
		clip_finish(a, NULL, 0);      /* no owner, or none with text */
	else
		clip_read(a);
}
