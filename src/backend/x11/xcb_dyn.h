/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * libxcb, opened at run time rather than linked, so that a machine without
 * it still loads the program and fnl_init() steps past X11 (DESIGN *One
 * build per operating system*, *The link line*).
 *
 * Every libxcb function the backend calls is a pointer, filled by
 * x11_xcb_load(), behind a macro of the function's own name, so the
 * backend's code reads as plain xcb. Each of its files reaches this
 * through x11_state.h.
 *
 * **No xcb header may be included for the first time after this one**: its
 * prototypes would expand through the macros below into a definition of
 * each pointer in every file. So what the backend needs of libxcb is
 * included here, first. A function newly called is a row in both lists, in
 * the same place: one missing from the first fails the link, libxcb being
 * off the link line, and one missing from the second is an undeclared
 * fnl_ pointer.
 */

#ifndef FNL_XCB_DYN_H
#define FNL_XCB_DYN_H

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

/* Every function of libxcb's the backend calls: the connection's own,
 * xcbext.h's for the hand-encoded extensions (x11_ext.c), then the core
 * protocol's requests, replies, iterators and accessors. */
#define X11_XCB_FUNCS(X)                        \
	X(xcb_connect)                              \
	X(xcb_connection_has_error)                 \
	X(xcb_disconnect)                           \
	X(xcb_discard_reply)                        \
	X(xcb_flush)                                \
	X(xcb_generate_id)                          \
	X(xcb_get_extension_data)                   \
	X(xcb_get_file_descriptor)                  \
	X(xcb_get_maximum_request_length)           \
	X(xcb_get_setup)                            \
	X(xcb_poll_for_event)                       \
	X(xcb_poll_for_queued_event)                \
	X(xcb_prefetch_extension_data)              \
	X(xcb_prefetch_maximum_request_length)      \
	X(xcb_send_request)                         \
	X(xcb_wait_for_reply)                       \
	X(xcb_change_gc)                            \
	X(xcb_change_property)                      \
	X(xcb_change_window_attributes)             \
	X(xcb_close_font)                           \
	X(xcb_configure_window)                     \
	X(xcb_convert_selection)                    \
	X(xcb_create_cursor)                        \
	X(xcb_create_gc)                            \
	X(xcb_create_glyph_cursor)                  \
	X(xcb_create_pixmap)                        \
	X(xcb_create_window)                        \
	X(xcb_depth_next)                           \
	X(xcb_depth_visuals_iterator)               \
	X(xcb_destroy_window)                       \
	X(xcb_free_cursor)                          \
	X(xcb_free_gc)                              \
	X(xcb_free_pixmap)                          \
	X(xcb_get_atom_name)                        \
	X(xcb_get_atom_name_name)                   \
	X(xcb_get_atom_name_name_length)            \
	X(xcb_get_atom_name_reply)                  \
	X(xcb_get_geometry)                         \
	X(xcb_get_geometry_reply)                   \
	X(xcb_get_keyboard_mapping)                 \
	X(xcb_get_keyboard_mapping_keysyms)         \
	X(xcb_get_keyboard_mapping_keysyms_length)  \
	X(xcb_get_keyboard_mapping_reply)           \
	X(xcb_get_modifier_mapping)                 \
	X(xcb_get_modifier_mapping_keycodes)        \
	X(xcb_get_modifier_mapping_keycodes_length) \
	X(xcb_get_modifier_mapping_reply)           \
	X(xcb_get_property)                         \
	X(xcb_get_property_reply)                   \
	X(xcb_get_property_value)                   \
	X(xcb_get_property_value_length)            \
	X(xcb_get_selection_owner)                  \
	X(xcb_get_selection_owner_reply)            \
	X(xcb_grab_pointer)                         \
	X(xcb_grab_pointer_reply)                   \
	X(xcb_intern_atom)                          \
	X(xcb_intern_atom_reply)                    \
	X(xcb_map_window)                           \
	X(xcb_open_font)                            \
	X(xcb_poly_fill_rectangle)                  \
	X(xcb_query_keymap)                         \
	X(xcb_query_keymap_reply)                   \
	X(xcb_query_pointer)                        \
	X(xcb_query_pointer_reply)                  \
	X(xcb_screen_allowed_depths_iterator)       \
	X(xcb_screen_next)                          \
	X(xcb_send_event)                           \
	X(xcb_set_selection_owner)                  \
	X(xcb_setup_roots_iterator)                 \
	X(xcb_ungrab_pointer)                       \
	X(xcb_visualtype_next)                      \
	X(xcb_warp_pointer)

/* The pointers. The one expansion that names the function bare, so it
 * comes before the macros that would turn that name into its pointer. */
#define X11_XCB_DECLARE(f) extern __typeof__(f) *fnl_##f;
X11_XCB_FUNCS(X11_XCB_DECLARE)
#undef X11_XCB_DECLARE

#define xcb_connect                              (*fnl_xcb_connect)
#define xcb_connection_has_error                 (*fnl_xcb_connection_has_error)
#define xcb_disconnect                           (*fnl_xcb_disconnect)
#define xcb_discard_reply                        (*fnl_xcb_discard_reply)
#define xcb_flush                                (*fnl_xcb_flush)
#define xcb_generate_id                          (*fnl_xcb_generate_id)
#define xcb_get_extension_data                   (*fnl_xcb_get_extension_data)
#define xcb_get_file_descriptor                  (*fnl_xcb_get_file_descriptor)
#define xcb_get_maximum_request_length \
	(*fnl_xcb_get_maximum_request_length)
#define xcb_get_setup                            (*fnl_xcb_get_setup)
#define xcb_poll_for_event                       (*fnl_xcb_poll_for_event)
#define xcb_poll_for_queued_event \
	(*fnl_xcb_poll_for_queued_event)
#define xcb_prefetch_extension_data \
	(*fnl_xcb_prefetch_extension_data)
#define xcb_prefetch_maximum_request_length \
	(*fnl_xcb_prefetch_maximum_request_length)
#define xcb_send_request                         (*fnl_xcb_send_request)
#define xcb_wait_for_reply                       (*fnl_xcb_wait_for_reply)
#define xcb_change_gc                            (*fnl_xcb_change_gc)
#define xcb_change_property                      (*fnl_xcb_change_property)
#define xcb_change_window_attributes \
	(*fnl_xcb_change_window_attributes)
#define xcb_close_font                           (*fnl_xcb_close_font)
#define xcb_configure_window                     (*fnl_xcb_configure_window)
#define xcb_convert_selection                    (*fnl_xcb_convert_selection)
#define xcb_create_cursor                        (*fnl_xcb_create_cursor)
#define xcb_create_gc                            (*fnl_xcb_create_gc)
#define xcb_create_glyph_cursor                  (*fnl_xcb_create_glyph_cursor)
#define xcb_create_pixmap                        (*fnl_xcb_create_pixmap)
#define xcb_create_window                        (*fnl_xcb_create_window)
#define xcb_depth_next                           (*fnl_xcb_depth_next)
#define xcb_depth_visuals_iterator \
	(*fnl_xcb_depth_visuals_iterator)
#define xcb_destroy_window                       (*fnl_xcb_destroy_window)
#define xcb_free_cursor                          (*fnl_xcb_free_cursor)
#define xcb_free_gc                              (*fnl_xcb_free_gc)
#define xcb_free_pixmap                          (*fnl_xcb_free_pixmap)
#define xcb_get_atom_name                        (*fnl_xcb_get_atom_name)
#define xcb_get_atom_name_name                   (*fnl_xcb_get_atom_name_name)
#define xcb_get_atom_name_name_length \
	(*fnl_xcb_get_atom_name_name_length)
#define xcb_get_atom_name_reply                  (*fnl_xcb_get_atom_name_reply)
#define xcb_get_geometry                         (*fnl_xcb_get_geometry)
#define xcb_get_geometry_reply                   (*fnl_xcb_get_geometry_reply)
#define xcb_get_keyboard_mapping \
	(*fnl_xcb_get_keyboard_mapping)
#define xcb_get_keyboard_mapping_keysyms \
	(*fnl_xcb_get_keyboard_mapping_keysyms)
#define xcb_get_keyboard_mapping_keysyms_length \
	(*fnl_xcb_get_keyboard_mapping_keysyms_length)
#define xcb_get_keyboard_mapping_reply \
	(*fnl_xcb_get_keyboard_mapping_reply)
#define xcb_get_modifier_mapping \
	(*fnl_xcb_get_modifier_mapping)
#define xcb_get_modifier_mapping_keycodes \
	(*fnl_xcb_get_modifier_mapping_keycodes)
#define xcb_get_modifier_mapping_keycodes_length \
	(*fnl_xcb_get_modifier_mapping_keycodes_length)
#define xcb_get_modifier_mapping_reply \
	(*fnl_xcb_get_modifier_mapping_reply)
#define xcb_get_property                         (*fnl_xcb_get_property)
#define xcb_get_property_reply                   (*fnl_xcb_get_property_reply)
#define xcb_get_property_value                   (*fnl_xcb_get_property_value)
#define xcb_get_property_value_length \
	(*fnl_xcb_get_property_value_length)
#define xcb_get_selection_owner                  (*fnl_xcb_get_selection_owner)
#define xcb_get_selection_owner_reply \
	(*fnl_xcb_get_selection_owner_reply)
#define xcb_grab_pointer                         (*fnl_xcb_grab_pointer)
#define xcb_grab_pointer_reply                   (*fnl_xcb_grab_pointer_reply)
#define xcb_intern_atom                          (*fnl_xcb_intern_atom)
#define xcb_intern_atom_reply                    (*fnl_xcb_intern_atom_reply)
#define xcb_map_window                           (*fnl_xcb_map_window)
#define xcb_open_font                            (*fnl_xcb_open_font)
#define xcb_poly_fill_rectangle                  (*fnl_xcb_poly_fill_rectangle)
#define xcb_query_keymap                         (*fnl_xcb_query_keymap)
#define xcb_query_keymap_reply                   (*fnl_xcb_query_keymap_reply)
#define xcb_query_pointer                        (*fnl_xcb_query_pointer)
#define xcb_query_pointer_reply                  (*fnl_xcb_query_pointer_reply)
#define xcb_screen_allowed_depths_iterator \
	(*fnl_xcb_screen_allowed_depths_iterator)
#define xcb_screen_next                          (*fnl_xcb_screen_next)
#define xcb_send_event                           (*fnl_xcb_send_event)
#define xcb_set_selection_owner                  (*fnl_xcb_set_selection_owner)
#define xcb_setup_roots_iterator                 (*fnl_xcb_setup_roots_iterator)
#define xcb_ungrab_pointer                       (*fnl_xcb_ungrab_pointer)
#define xcb_visualtype_next                      (*fnl_xcb_visualtype_next)
#define xcb_warp_pointer                         (*fnl_xcb_warp_pointer)

/* libxcb.so.1 opened and every row above resolved: NULL when it was, else
 * the one-line reason it was not -- the library missing, or a function it
 * lacks, by name. Tried once per process and the answer kept, failure
 * included (core/dynlib.h). Before anything above is called; [main]. */
const char *x11_xcb_load(void);

#endif /* FNL_XCB_DYN_H */
