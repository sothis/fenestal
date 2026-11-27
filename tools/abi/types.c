/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The public headers' types, pinned: the exact type of every struct field
 * and the exact prototype of every FNL_API function, as a compile-time
 * assertion each. The .abi baselines in tools/abi record offsets and sizes
 * and the .syms names; neither sees a `float` that became an `int` or a
 * parameter that became a `double`, which keeps the symbol and the offset
 * and breaks every compiled caller.
 *
 * Compiled by the header check as C11 and never linked, and not as C++ or
 * Objective-C: _Generic is C's. A change to a type is a change to this file
 * in the same commit, and from 1.0 an ABI break (tools/abi/check.cmake).
 * A field or function added without a line here is not caught by
 * tools/check-abidump.py, which reads abidump.c: add the line with the
 * declaration.
 *
 * An enum is compatible with its integer type in C, so a field that changes
 * between the two is not seen here; the enum values are pinned by abidump.
 */

#include <fenestal/fenestal.h>
#include <fenestal/fnl_metal.h>
#ifdef FNL_CHECK_VULKAN
#include <fenestal/fnl_vulkan.h>
#endif
#ifdef FNL_CHECK_D3D12
#define COBJMACROS
#include <fenestal/fnl_d3d12.h>
#endif

#define FIELD(T, f, ty)                                                        \
	_Static_assert(_Generic(((T *)0)->f, ty : 1, default : 0),                 \
				   #T "::" #f " changed type")
#define FUNC(name, ret, params)                                                \
	_Static_assert(_Generic(&name, ret(*) params : 1, default : 0),            \
				   #name " changed its prototype")

/* fenestal.h */
FIELD(FnlOutputColor, space, FnlColorSpace);
FIELD(FnlOutputColor, sdr_scale, float);
FIELD(FnlOutputColor, peak, float);
FIELD(FnlInitDesc, size, size_t);
FIELD(FnlInitDesc, header_version, uint32_t);
FIELD(FnlInitDesc, log, FnlLogFn);
FIELD(FnlInitDesc, log_ud, void *);
FIELD(FnlInitDesc, title, const char *);
FIELD(FnlInitDesc, width_pt, int);
FIELD(FnlInitDesc, height_pt, int);
FIELD(FnlInitDesc, install_signal_handlers, bool);
FIELD(FnlInitDesc, renderer, FnlRenderer);
FIELD(FnlInitDesc, no_late_latch, bool);
FIELD(FnlInitDesc, gpu_name, const char *);
FIELD(FnlInitDesc, frame_times, bool);
FIELD(FnlInitDesc, echo_events, bool);
FIELD(FnlInitDesc, renderers, uint32_t);
FIELD(FnlInitDesc, content_is_ui, bool);
FIELD(FnlInitDesc, no_titlebar_controls, uint32_t);
FIELD(FnlInitDesc, no_gpu_ranking, bool);
FIELD(FnlInitDesc, want_color, FnlColorSpace);
FIELD(FnlInitDesc, app_id, const char *);
FIELD(FnlInitDesc, app_name, const char *);
FIELD(FnlInitDesc, x_pt, int);
FIELD(FnlInitDesc, y_pt, int);
FIELD(FnlInitDesc, positioned, bool);
FIELD(FnlInitDesc, vulkan_setup, const struct FnlVulkanSetup *);
FIELD(FnlWindowDesc, size, size_t);
FIELD(FnlWindowDesc, header_version, uint32_t);
FIELD(FnlWindowDesc, title, const char *);
FIELD(FnlWindowDesc, width_pt, int);
FIELD(FnlWindowDesc, height_pt, int);
FIELD(FnlWindowDesc, x_pt, int);
FIELD(FnlWindowDesc, y_pt, int);
FIELD(FnlWindowDesc, gpu_name, const char *);
FIELD(FnlWindowDesc, content_is_ui, bool);
FIELD(FnlWindowDesc, no_titlebar_controls, uint32_t);
FIELD(FnlWindowDesc, frame_times, bool);
FIELD(FnlWindowDesc, positioned, bool);
FIELD(FnlFrame, index, uint64_t);
FIELD(FnlFrame, time, double);
FIELD(FnlFrame, area_w_px, int);
FIELD(FnlFrame, area_h_px, int);
FIELD(FnlFrame, scene_w_px, int);
FIELD(FnlFrame, scene_h_px, int);
FIELD(FnlFrame, target, FnlSceneTarget);
FIELD(FnlFrame, samples, int);
FIELD(FnlFrame, scene_time, float);
FIELD(FnlFrame, jitter_x, float);
FIELD(FnlFrame, jitter_y, float);
FIELD(FnlFrame, color, FnlOutputColor);
FIELD(FnlKeyEvent, key, FnlKey);
FIELD(FnlKeyEvent, pressed, bool);
FIELD(FnlKeyEvent, repeat, bool);
FIELD(FnlButtonEvent, x, double);
FIELD(FnlButtonEvent, y, double);
FIELD(FnlButtonEvent, button, FnlButton);
FIELD(FnlButtonEvent, pressed, bool);
FIELD(FnlButtonEvent, clicks, int);
FIELD(FnlMotionEvent, x, double);
FIELD(FnlMotionEvent, y, double);
FIELD(FnlRelativeEvent, dx, double);
FIELD(FnlRelativeEvent, dy, double);
FIELD(FnlScrollEvent, x, double);
FIELD(FnlScrollEvent, y, double);
FIELD(FnlScrollEvent, dx, double);
FIELD(FnlScrollEvent, dy, double);
FIELD(FnlPresenceEvent, x, double);
FIELD(FnlPresenceEvent, y, double);
FIELD(FnlFocusEvent, focused, bool);
FIELD(FnlResizeEvent, w_pt, int);
FIELD(FnlResizeEvent, h_pt, int);
FIELD(FnlResizeEvent, w_px, int);
FIELD(FnlResizeEvent, h_px, int);
FIELD(FnlScaleEvent, scale120, int);
FIELD(FnlWindowEvent, fullscreen, bool);
FIELD(FnlWindowEvent, maximized, bool);
FIELD(FnlImageLostEvent, lost, bool);
FIELD(FnlClaimFocusEvent, claim, int);
FIELD(FnlAntialiasEvent, aa, FnlAntialias);
FIELD(FnlSceneEvent, w_px, int);
FIELD(FnlSceneEvent, h_px, int);
FIELD(FnlCaptureEvent, captured, bool);
FIELD(FnlColorEvent, color, FnlOutputColor);
FIELD(FnlLostEvent, count, unsigned);
FIELD(FnlTextEvent, codepoint, uint32_t);
FIELD(FnlTextEvent, utf8, char *);
FIELD(FnlLengthEvent, length, unsigned);
FIELD(FnlOutputEvent, w_pt, int);
FIELD(FnlOutputEvent, h_pt, int);
FIELD(FnlOutputListEvent, count, int);
FIELD(FnlGpuEvent, index, int);
FIELD(FnlGpuEvent, renderer, FnlRenderer);
FIELD(FnlEvent, type, FnlEventType);
FIELD(FnlEvent, input_age, float);
FIELD(FnlEvent, time, double);
FIELD(FnlEvent, key, FnlKeyEvent);
FIELD(FnlEvent, button, FnlButtonEvent);
FIELD(FnlEvent, motion, FnlMotionEvent);
FIELD(FnlEvent, relative, FnlRelativeEvent);
FIELD(FnlEvent, scroll, FnlScrollEvent);
FIELD(FnlEvent, focus, FnlFocusEvent);
FIELD(FnlEvent, resize, FnlResizeEvent);
FIELD(FnlEvent, scale, FnlScaleEvent);
FIELD(FnlEvent, window, FnlWindowEvent);
FIELD(FnlEvent, antialias, FnlAntialiasEvent);
FIELD(FnlEvent, scene, FnlSceneEvent);
FIELD(FnlEvent, capture, FnlCaptureEvent);
FIELD(FnlEvent, lost, FnlLostEvent);
FIELD(FnlEvent, text, FnlTextEvent);
FIELD(FnlEvent, compose, FnlLengthEvent);
FIELD(FnlEvent, clipboard, FnlLengthEvent);
FIELD(FnlEvent, output, FnlOutputEvent);
FIELD(FnlEvent, gpu, FnlGpuEvent);
FIELD(FnlEvent, presence, FnlPresenceEvent);
FIELD(FnlEvent, image_lost, FnlImageLostEvent);
FIELD(FnlEvent, claim_focus, FnlClaimFocusEvent);
FIELD(FnlEvent, color, FnlColorEvent);
FIELD(FnlEvent, output_list, FnlOutputListEvent);
FIELD(FnlCallbacks, size, size_t);
FIELD(FnlCallbacks, ud, void *);
FIELD(FnlCallbacks, on_gpu_created, void (*)(void *ud, FnlApp *app));
FIELD(FnlCallbacks, on_gpu_lost, void (*)(void *ud, FnlApp *app));
FIELD(FnlCallbacks, on_record, void (*)(void *ud, FnlApp *app, const FnlFrame *frame));
FIELD(FnlCallbacks, on_overlay, void (*)(void *ud, FnlApp *app, const FnlFrame *frame));
FIELD(FnlColor, r, float);
FIELD(FnlColor, g, float);
FIELD(FnlColor, b, float);
FIELD(FnlColor, a, float);
FIELD(FnlOverlayCounts, rects, int);
FIELD(FnlOverlayCounts, rects_max, int);
FIELD(FnlOverlayCounts, strokes, int);
FIELD(FnlOverlayCounts, strokes_max, int);
FIELD(FnlOverlayCounts, glyphs, int);
FIELD(FnlOverlayCounts, glyphs_max, int);
FIELD(FnlFrameStats, size, size_t);
FIELD(FnlFrameStats, frames, uint64_t);
FIELD(FnlFrameStats, presents, uint64_t);
FIELD(FnlFrameStats, pace_us, uint64_t);
FIELD(FnlFrameStats, fence_us, uint64_t);
FIELD(FnlFrameStats, acquire_us, uint64_t);
FIELD(FnlFrameStats, record_us, uint64_t);
FIELD(FnlFrameStats, submit_us, uint64_t);
FIELD(FnlFrameStats, present_us, uint64_t);
FIELD(FnlFrameStats, rebuild_us, uint64_t);
FIELD(FnlFrameStats, draw_us, uint64_t);
FIELD(FnlFrameStats, gpu_us, uint64_t);
FIELD(FnlFrameStats, gpu_samples, uint64_t);
FIELD(FnlFrameStats, gpu_idle, uint64_t);
FIELD(FnlFrameStats, cycles, uint64_t);
FIELD(FnlFrameStats, missed, uint64_t);
FIELD(FnlFrameStats, rebuilds, uint64_t);
FIELD(FnlFrameStats, period_ms, double);
FIELD(FnlFrameStats, hold_ms, double);
FIELD(FnlFrameSample, index, uint64_t);
FIELD(FnlFrameSample, time, double);
FIELD(FnlFrameSample, pace_ms, float);
FIELD(FnlFrameSample, fence_ms, float);
FIELD(FnlFrameSample, acquire_ms, float);
FIELD(FnlFrameSample, record_ms, float);
FIELD(FnlFrameSample, submit_ms, float);
FIELD(FnlFrameSample, present_ms, float);
FIELD(FnlFrameSample, rebuild_ms, float);
FIELD(FnlFrameSample, draw_ms, float);
FIELD(FnlFrameSample, presented, bool);
FIELD(FnlFrameSample, gpu_ms, float);
FIELD(FnlFrameShown, index, uint64_t);
FIELD(FnlFrameShown, time, double);
FIELD(FnlFrameShown, vblank, bool);
FIELD(FnlMode, w_px, int);
FIELD(FnlMode, h_px, int);
FIELD(FnlMode, refresh_mhz, unsigned int);
FUNC(fnl_version, uint32_t, (void));
FUNC(fnl_result_str, const char *, (FnlResult r));
FUNC(fnl_last_error, const char *, (void));
FUNC(fnl_error_copy, size_t, (char *dst, size_t cap));
FUNC(fnl_set_log_level, void, (FnlLogLevel lvl));
FUNC(fnl_init, FnlResult, (const FnlInitDesc *desc, FnlApp **out));
FUNC(fnl_window_create, FnlResult, (FnlApp *sibling, const FnlWindowDesc *desc, const FnlCallbacks *cb, FnlApp **out));
FUNC(fnl_shutdown, void, (FnlApp *app));
FUNC(fnl_run, FnlResult, (FnlApp *app, const FnlCallbacks *cb));
FUNC(fnl_start, FnlResult, (FnlApp *app, const FnlCallbacks *cb));
FUNC(fnl_pump, FnlResult, (FnlApp *app, int timeout_ms));
FUNC(fnl_stop, FnlResult, (FnlApp *app));
FUNC(fnl_quit, void, (FnlApp *app));
FUNC(fnl_is_closing, bool, (const FnlApp *app));
FUNC(fnl_poll_event, bool, (FnlApp *app, FnlEvent *out));
FUNC(fnl_gpu_name, const char *, (const FnlApp *app));
FUNC(fnl_gpu_short_name, const char *, (const FnlApp *app));
FUNC(fnl_renderer, FnlRenderer, (const FnlApp *app));
FUNC(fnl_gpu_count, int, (const FnlApp *app));
FUNC(fnl_gpu_current, int, (const FnlApp *app));
FUNC(fnl_gpu_name_at, const char *, (const FnlApp *app, int i));
FUNC(fnl_gpu_kind_at, FnlGpuKind, (const FnlApp *app, int i));
FUNC(fnl_gpu_renderer_at, FnlRenderer, (const FnlApp *app, int i));
FUNC(fnl_gpu_via_at, const char *, (const FnlApp *app, int i));
FUNC(fnl_request_gpu, void, (FnlApp *app, int i));
FUNC(fnl_font_notice, const char *, (void));
FUNC(fnl_font_licence, const char *, (void));
FUNC(fnl_text_width, int, (const char *utf8, FnlTextSize sz));
FUNC(fnl_key_name, const char *, (FnlKey key));
FUNC(fnl_key_from_name, FnlKey, (const char *name));
FUNC(fnl_text_line, int, (FnlTextSize sz));
FUNC(fnl_text_height, int, (FnlTextSize sz));
FUNC(fnl_text_advance, int, (FnlTextSize sz));
FUNC(fnl_text_elide, void, (char *dst, size_t cap, const char *src, int max_w, FnlTextSize sz));
FUNC(fnl_text_wrap, int, (char *dst, size_t cap, const char *src, int max_w, FnlTextSize sz));
FUNC(fnl_overlay_text_in, void, (FnlApp *app, int x, int y, int w, FnlTextAlign align, FnlTextSize sz, FnlColor c, const char *utf8));
FUNC(fnl_overlay_text, void, (FnlApp *app, int x, int y, FnlTextSize sz, FnlColor c, const char *utf8));
FUNC(fnl_overlay_rect, void, (FnlApp *app, int x, int y, int w, int h, FnlColor c));
FUNC(fnl_overlay_rect_blend, void, (FnlApp *app, int x, int y, int w, int h, FnlColor c));
FUNC(fnl_overlay_rect_round, void, (FnlApp *app, int x, int y, int w, int h, float radius, FnlColor c));
FUNC(fnl_overlay_circle, void, (FnlApp *app, float cx, float cy, float r, FnlColor c));
FUNC(fnl_overlay_gradient, void, (FnlApp *app, int x, int y, int w, int h, float angle, FnlColor c, FnlColor c2));
FUNC(fnl_image_supported, bool, (const FnlApp *app));
FUNC(fnl_image_valid, bool, (const FnlApp *app, const FnlImage *img));
FUNC(fnl_image_create, FnlImage *, (FnlApp *app, int w, int h, FnlImageFormat fmt, const void *pixels));
FUNC(fnl_image_destroy, void, (FnlImage *img));
FUNC(fnl_overlay_image, void, (FnlApp *app, int x, int y, int w, int h, const FnlImage *img));
FUNC(fnl_overlay_stroke, void, (FnlApp *app, float x0, float y0, float x1, float y1, float width, FnlColor c));
FUNC(fnl_overlay_elbow, void, (FnlApp *app, float x0, float y0, float x1, float y1, float x2, float y2, float width, FnlColor c));
FUNC(fnl_overlay_box, void, (FnlApp *app, float x0, float y0, float x1, float y1, float width, FnlColor c));
FUNC(fnl_overlay_flush, void, (FnlApp *app));
FUNC(fnl_overlay_clip, void, (FnlApp *app, int x, int y, int w, int h));
FUNC(fnl_overlay_counts, void, (const FnlApp *app, FnlOverlayCounts *out));
FUNC(fnl_window_size_pt, void, (const FnlApp *app, int *w_pt, int *h_pt));
FUNC(fnl_invalidate, void, (FnlApp *app));
FUNC(fnl_post, bool, (FnlApp *app, void (*fn)(void *ud), void *ud));
FUNC(fnl_invalidate_in, void, (FnlApp *app, int ms));
FUNC(fnl_titlebar_reserve, bool, (FnlApp *app, int want_w, int *x, int *y, int *w, int *h));
FUNC(fnl_titlebar_custom, bool, (FnlApp *app, int height_pt, int min_width_pt));
FUNC(fnl_titlebar_height, int, (const FnlApp *app));
FUNC(fnl_titlebar_owned, bool, (const FnlApp *app));
FUNC(fnl_titlebar_claim, bool, (FnlApp *app, int x, int y, int w, int h));
FUNC(fnl_claim, bool, (FnlApp *app, int x, int y, int w, int h));
FUNC(fnl_claim_focus, bool, (FnlApp *app, int x, int y, int w, int h));
FUNC(fnl_claim_at, int, (const FnlApp *app, double x_pt, double y_pt));
FUNC(fnl_begin_move, bool, (FnlApp *app));
FUNC(fnl_begin_resize, bool, (FnlApp *app, FnlEdge edge));
FUNC(fnl_is_fullscreen, bool, (const FnlApp *app));
FUNC(fnl_is_active, bool, (const FnlApp *app));
FUNC(fnl_is_shown, bool, (const FnlApp *app));
FUNC(fnl_is_maximized, bool, (const FnlApp *app));
FUNC(fnl_is_pointer_captured, bool, (const FnlApp *app));
FUNC(fnl_max_samples, int, (const FnlApp *app));
FUNC(fnl_offscreen_supported, bool, (const FnlApp *app));
FUNC(fnl_antialias, FnlAntialias, (const FnlApp *app));
FUNC(fnl_fps, int, (const FnlApp *app));
FUNC(fnl_frame_gpu_ms, double, (const FnlApp *app));
FUNC(fnl_frame_cpu_ms, double, (const FnlApp *app));
FUNC(fnl_input_latency_ms, double, (const FnlApp *app));
FUNC(fnl_set_cost_sampling, void, (FnlApp *app, bool on));
FUNC(fnl_set_scene_motion, void, (FnlApp *app, bool on));
FUNC(fnl_scene_motion, bool, (const FnlApp *app));
FUNC(fnl_set_scene_motion_px, void, (FnlApp *app, float dx_px, float dy_px));
FUNC(fnl_set_metrics_panel, void, (FnlApp *app, bool shown));
FUNC(fnl_metrics_panel, bool, (const FnlApp *app));
FUNC(fnl_set_metrics_key, void, (FnlApp *app, FnlKey key));
FUNC(fnl_metrics_key, FnlKey, (const FnlApp *app));
FUNC(fnl_set_frame_stats, void, (FnlApp *app, bool on));
FUNC(fnl_frame_stats, bool, (const FnlApp *app, FnlFrameStats *out));
FUNC(fnl_frame_samples, int, (FnlApp *app, FnlFrameSample *out, int max, size_t stride, uint64_t *lost));
FUNC(fnl_frame_shown, int, (FnlApp *app, FnlFrameShown *out, int max, size_t stride, uint64_t *lost));
FUNC(fnl_set_transition_ms, void, (FnlApp *app, int ms));
FUNC(fnl_scene_size_px, void, (const FnlApp *app, int *w_px, int *h_px));
FUNC(fnl_content_size_px, void, (const FnlApp *app, int *w_px, int *h_px));
FUNC(fnl_content_rect_pt, void, (const FnlApp *app, int *x, int *y, int *w, int *h));
FUNC(fnl_min_content_size_pt, void, (const FnlApp *app, int *w_pt, int *h_pt));
FUNC(fnl_set_fullscreen, void, (FnlApp *app, bool on));
FUNC(fnl_minimize, void, (FnlApp *app));
FUNC(fnl_set_maximized, void, (FnlApp *app, bool on));
FUNC(fnl_set_antialias, void, (FnlApp *app, FnlAntialias aa));
FUNC(fnl_set_cursor, void, (FnlApp *app, FnlCursor shape));
FUNC(fnl_set_title, void, (FnlApp *app, const char *utf8));
FUNC(fnl_release_pointer, void, (FnlApp *app));
FUNC(fnl_set_scene_size_px, void, (FnlApp *app, int w_px, int h_px));
FUNC(fnl_output_name, const char *, (const FnlApp *app));
FUNC(fnl_present_mode, const char *, (const FnlApp *app));
FUNC(fnl_set_vsync, void, (FnlApp *app, bool on));
FUNC(fnl_vsync, bool, (const FnlApp *app));
FUNC(fnl_backend_name, const char *, (const FnlApp *app));
FUNC(fnl_backend, FnlBackend, (const FnlApp *app));
FUNC(fnl_window_host, const char *, (const FnlApp *app));
FUNC(fnl_renderer_name, const char *, (const FnlApp *app));
FUNC(fnl_renderer_via, const char *, (const FnlApp *app));
FUNC(fnl_output_size_pt, void, (const FnlApp *app, int *w_pt, int *h_pt));
FUNC(fnl_output_refresh_mhz, unsigned, (const FnlApp *app));
FUNC(fnl_mode_count, int, (const FnlApp *app));
FUNC(fnl_mode_get, bool, (const FnlApp *app, int i, FnlMode *mode));
FUNC(fnl_mode_current, bool, (const FnlApp *app, FnlMode *mode));
FUNC(fnl_request_mode, void, (FnlApp *app, const FnlMode *mode));
FUNC(fnl_output_count, int, (const FnlApp *app));
FUNC(fnl_output_name_at, const char *, (const FnlApp *app, int i));
FUNC(fnl_request_output, void, (FnlApp *app, const char *name));
FUNC(fnl_scale120, int, (const FnlApp *app));
FUNC(fnl_output_color, bool, (const FnlApp *app, FnlOutputColor *out));
FUNC(fnl_request_size_pt, void, (FnlApp *app, int w_pt, int h_pt));
FUNC(fnl_key_down, bool, (const FnlApp *app, FnlKey k));
FUNC(fnl_set_text_input, void, (FnlApp *app, bool on));
FUNC(fnl_is_text_input, bool, (const FnlApp *app));
FUNC(fnl_compose_text, size_t, (const FnlApp *app, char *buf, size_t size));
FUNC(fnl_set_text_rect_pt, void, (FnlApp *app, int x, int y, int w, int h));
FUNC(fnl_clipboard_set, bool, (FnlApp *app, const char *utf8));
FUNC(fnl_clipboard_request, void, (FnlApp *app));
FUNC(fnl_clipboard_text, size_t, (const FnlApp *app, char *buf, size_t size));

/* fnl_vulkan.h */
#ifdef FNL_CHECK_VULKAN
FIELD(FnlVulkanSetup, size, size_t);
FIELD(FnlVulkanSetup, api_version, uint32_t);
FIELD(FnlVulkanSetup, instance_extension_count, uint32_t);
FIELD(FnlVulkanSetup, instance_extensions, const char *const *);
FIELD(FnlVulkanSetup, instance_pnext, const void *);
FIELD(FnlVulkanSetup, device_extension_count, uint32_t);
FIELD(FnlVulkanSetup, device_extensions, const char *const *);
FIELD(FnlVulkanSetup, features, const VkPhysicalDeviceFeatures *);
FIELD(FnlVulkanSetup, feature_chain, const void *);
FIELD(FnlVulkanGpu, instance, VkInstance);
FIELD(FnlVulkanGpu, physical_device, VkPhysicalDevice);
FIELD(FnlVulkanGpu, device, VkDevice);
FIELD(FnlVulkanGpu, queue, VkQueue);
FIELD(FnlVulkanGpu, queue_family, uint32_t);
FIELD(FnlVulkanGpu, pipeline_cache, VkPipelineCache);
FIELD(FnlVulkanGpu, get_instance_proc_addr, PFN_vkGetInstanceProcAddr);
FIELD(FnlVulkanGpu, api_version, uint32_t);
FIELD(FnlVulkanSceneLayout, render_pass, VkRenderPass);
FIELD(FnlVulkanSceneLayout, subpass, uint32_t);
FIELD(FnlVulkanSceneLayout, samples, VkSampleCountFlagBits);
FIELD(FnlVulkanSceneLayout, color_format, VkFormat);
FIELD(FnlVulkanSceneLayout, depth_format, VkFormat);
FUNC(fnl_vulkan_gpu, bool, (FnlApp *app, FnlVulkanGpu *out));
FUNC(fnl_vulkan_queue_submit, VkResult, (FnlApp *app, uint32_t count, const VkSubmitInfo *submits, VkFence fence));
FUNC(fnl_vulkan_queue_wait_idle, VkResult, (FnlApp *app));
FUNC(fnl_vulkan_scene_layout, bool, (FnlApp *app, FnlSceneTarget target, FnlVulkanSceneLayout *out));
FUNC(fnl_vulkan_scene_cmd, bool, (FnlApp *app, VkCommandBuffer *out));
FUNC(fnl_vulkan_scene_rect, bool, (FnlApp *app, VkRect2D *out));
#endif

/* fnl_metal.h */
FIELD(FnlMetalGpu, device, FnlMtlDevice);
FIELD(FnlMetalGpu, queue, FnlMtlCommandQueue);
FIELD(FnlMetalSceneLayout, samples, int);
FIELD(FnlMetalSceneLayout, color_format, uint32_t);
FIELD(FnlMetalSceneLayout, depth_format, uint32_t);
FIELD(FnlMetalScene, encoder, FnlMtlRenderEncoder);
FIELD(FnlMetalScene, command_buffer, FnlMtlCommandBuffer);
FIELD(FnlMetalScene, x, int);
FIELD(FnlMetalScene, y, int);
FIELD(FnlMetalScene, w, int);
FIELD(FnlMetalScene, h, int);
FUNC(fnl_metal_gpu, bool, (FnlApp *app, FnlMetalGpu *out));
FUNC(fnl_metal_scene_layout, bool, (FnlApp *app, FnlSceneTarget target, FnlMetalSceneLayout *out));
FUNC(fnl_metal_scene, bool, (FnlApp *app, FnlMetalScene *out));

/* fnl_d3d12.h */
#ifdef FNL_CHECK_D3D12
FIELD(FnlD3D12Gpu, device, ID3D12Device *);
FIELD(FnlD3D12Gpu, queue, ID3D12CommandQueue *);
FIELD(FnlD3D12SceneLayout, samples, int);
FIELD(FnlD3D12SceneLayout, color_format, uint32_t);
FIELD(FnlD3D12SceneLayout, depth_format, uint32_t);
FIELD(FnlD3D12Scene, list, ID3D12GraphicsCommandList *);
FIELD(FnlD3D12Scene, rtv, D3D12_CPU_DESCRIPTOR_HANDLE);
FIELD(FnlD3D12Scene, dsv, D3D12_CPU_DESCRIPTOR_HANDLE);
FIELD(FnlD3D12Scene, rect, D3D12_RECT);
FUNC(fnl_d3d12_gpu, bool, (FnlApp *app, FnlD3D12Gpu *out));
FUNC(fnl_d3d12_scene_layout, bool, (FnlApp *app, FnlSceneTarget target, FnlD3D12SceneLayout *out));
FUNC(fnl_d3d12_scene, bool, (FnlApp *app, FnlD3D12Scene *out));
#endif
