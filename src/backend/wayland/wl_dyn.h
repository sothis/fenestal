/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * libwayland-client, opened at run time rather than linked, so that a
 * machine without it still loads the program and fnl_init() steps past
 * Wayland (DESIGN *One build per operating system*, *The link line*).
 *
 * Every function the backend reaches is a pointer here, and its name a
 * macro onto that pointer, so the static inline requests of
 * <wayland-client-protocol.h> and of the generated *-client-protocol.h
 * headers call through it too. **This header comes before any other Wayland
 * header in the TU**: one included sooner compiles those inline functions
 * against the library's own symbols, which are then a link to nothing. The
 * #error below says so where that happens.
 *
 * The interface objects (wl_surface_interface and the rest) are not
 * redirected: the build generates the core protocol's code into the
 * library, as it does every other protocol's, and libwayland compares
 * interfaces by name, never by address.
 */

#ifndef FNL_WL_DYN_H
#define FNL_WL_DYN_H

#ifdef WAYLAND_CLIENT_PROTOCOL_H
#error "backend/wayland/wl_dyn.h must come before every other Wayland header"
#endif

/* With its prototypes, which the pointers take their types from: before
 * the macros below exist. */
#include <wayland-client-core.h>

/* Every libwayland-client function called, by the backend and by the
 * inline requests of the protocol headers it includes; a call to one not
 * listed fails to link. Inside an X body the name is used only as #f or
 * fnl_##f: bare, it would be expanded through the #defines below. */
#define WL_DYN_FUNCS(X)                  \
	X(wl_proxy_add_listener)             \
	X(wl_proxy_destroy)                  \
	X(wl_proxy_get_user_data)            \
	X(wl_proxy_get_version)              \
	X(wl_proxy_marshal_flags)            \
	X(wl_proxy_set_user_data)            \
	X(wl_display_cancel_read)            \
	X(wl_display_connect)                \
	X(wl_display_disconnect)             \
	X(wl_display_dispatch)               \
	X(wl_display_dispatch_pending)       \
	X(wl_display_flush)                  \
	X(wl_display_get_error)              \
	X(wl_display_get_fd)                 \
	X(wl_display_get_protocol_error)     \
	X(wl_display_prepare_read)           \
	X(wl_display_read_events)            \
	X(wl_display_roundtrip)

/* The one body that names the function bare, and safely: it is expanded
 * here, before the #defines. */
#define WL_DYN_DECLARE(f) extern __typeof__(f) *fnl_##f;
WL_DYN_FUNCS(WL_DYN_DECLARE)
#undef WL_DYN_DECLARE

/* In the list's order. */
#define wl_proxy_add_listener         (*fnl_wl_proxy_add_listener)
#define wl_proxy_destroy              (*fnl_wl_proxy_destroy)
#define wl_proxy_get_user_data        (*fnl_wl_proxy_get_user_data)
#define wl_proxy_get_version          (*fnl_wl_proxy_get_version)
#define wl_proxy_marshal_flags        (*fnl_wl_proxy_marshal_flags)
#define wl_proxy_set_user_data        (*fnl_wl_proxy_set_user_data)
#define wl_display_cancel_read        (*fnl_wl_display_cancel_read)
#define wl_display_connect            (*fnl_wl_display_connect)
#define wl_display_disconnect         (*fnl_wl_display_disconnect)
#define wl_display_dispatch           (*fnl_wl_display_dispatch)
#define wl_display_dispatch_pending   (*fnl_wl_display_dispatch_pending)
#define wl_display_flush              (*fnl_wl_display_flush)
#define wl_display_get_error          (*fnl_wl_display_get_error)
#define wl_display_get_fd             (*fnl_wl_display_get_fd)
#define wl_display_get_protocol_error (*fnl_wl_display_get_protocol_error)
#define wl_display_prepare_read       (*fnl_wl_display_prepare_read)
#define wl_display_read_events        (*fnl_wl_display_read_events)
#define wl_display_roundtrip          (*fnl_wl_display_roundtrip)

/* Only now: its inline requests then call through the pointers. Every
 * later <wayland-client.h>, the generated headers' included, is a guarded
 * no-op. */
#include <wayland-client-protocol.h>
#include <wayland-client.h>

/* [main] NULL once every entry above is resolved; otherwise why not, one
 * line, the same on every call. The library is opened by the first call
 * and never closed. Called by the backend's open() before anything else,
 * so that its refusal comes before the window exists. */
const char *wl_dyn_load(void);

#endif /* FNL_WL_DYN_H */
