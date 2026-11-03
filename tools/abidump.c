/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The public ABI as text: every struct a caller and the library share, by
 * size and field offset, and every enum constant by value, with the version
 * the header declares. Run in every native build and compared against
 * tools/abi/fenestal-0.abi, so that a change to either header that moves
 * something a compiled caller depends on fails the build instead of a
 * program built against an older header.
 *
 *   abidump > fenestal.abi
 *
 * An intended change is made by copying the build's output over the
 * baseline in the same commit, which is what makes it a decision and not an
 * accident. Before 1.0 any change is allowed that way and wants a line in
 * CHANGELOG.md; from 1.0 only an addition is, and a change to anything
 * already in the baseline wants FNL_VERSION_MAJOR to go up with it.
 *
 * The lists below are written out rather than derived, so a new field or
 * constant is a line here too; a struct that grows without one still fails,
 * on its size.
 */

#include <fenestal/fenestal.h>
#include <fenestal/fnl_gl.h>
/* Only where Vulkan's headers are: each renderer's tree compares against a
 * baseline of its own (tools/abi/fenestal-0.* for Vulkan, -gl for GL). */
#ifdef FNL_ABI_VULKAN
#include <fenestal/fnl_vulkan.h>
#endif

#include <stddef.h>
#include <stdio.h>

#define S(t)    printf("sizeof %s %zu\n", #t, sizeof(t))
#define F(t, f) printf("offset %s.%s %zu\n", #t, #f, offsetof(t, f))
#define E(c)    printf("enum %s %lld\n", #c, (long long)(c))

int main(void)
{
	/* The major alone: minor and patch move with every release, and the
	 * baseline is one per major (and before 1.0 simply updated). */
	printf("major %d\n", FNL_VERSION_MAJOR);
	S(FnlInitDesc);
	F(FnlInitDesc, size);
	F(FnlInitDesc, header_version);
	F(FnlInitDesc, log);
	F(FnlInitDesc, log_ud);
	F(FnlInitDesc, title);
	F(FnlInitDesc, width_pt);
	F(FnlInitDesc, height_pt);
	F(FnlInitDesc, install_signal_handlers);
	F(FnlInitDesc, renderer);
	F(FnlInitDesc, no_late_latch);
	F(FnlInitDesc, late_latch_fixed);
	F(FnlInitDesc, scale_override120);
	F(FnlInitDesc, gpu_index);
	F(FnlInitDesc, gpu_name);
	F(FnlInitDesc, frame_times);
	F(FnlInitDesc, echo_events);
	F(FnlInitDesc, renderers);
	S(FnlFrame);
	F(FnlFrame, index);
	F(FnlFrame, time);
	F(FnlFrame, area_w_px);
	F(FnlFrame, area_h_px);
	F(FnlFrame, scene_w_px);
	F(FnlFrame, scene_h_px);
	F(FnlFrame, target);
	F(FnlFrame, samples);
	F(FnlFrame, scene_time);
	S(FnlCallbacks);
	F(FnlCallbacks, size);
	F(FnlCallbacks, ud);
	F(FnlCallbacks, on_gpu_created);
	F(FnlCallbacks, on_gpu_lost);
	F(FnlCallbacks, on_record);
	F(FnlCallbacks, on_overlay);
	S(FnlMode);
	F(FnlMode, w_px);
	F(FnlMode, h_px);
	F(FnlMode, refresh_mhz);
	S(FnlFrameStats);
	F(FnlFrameStats, size);
	F(FnlFrameStats, frames);
	F(FnlFrameStats, presents);
	F(FnlFrameStats, pace_us);
	F(FnlFrameStats, fence_us);
	F(FnlFrameStats, acquire_us);
	F(FnlFrameStats, record_us);
	F(FnlFrameStats, submit_us);
	F(FnlFrameStats, present_us);
	F(FnlFrameStats, rebuild_us);
	F(FnlFrameStats, draw_us);
	F(FnlFrameStats, gpu_us);
	F(FnlFrameStats, gpu_samples);
	F(FnlFrameStats, gpu_idle);
	F(FnlFrameStats, cycles);
	F(FnlFrameStats, missed);
	F(FnlFrameStats, rebuilds);
	F(FnlFrameStats, period_ms);
	F(FnlFrameStats, hold_ms);
	S(FnlFrameSample);
	F(FnlFrameSample, index);
	F(FnlFrameSample, time);
	F(FnlFrameSample, pace_ms);
	F(FnlFrameSample, fence_ms);
	F(FnlFrameSample, acquire_ms);
	F(FnlFrameSample, record_ms);
	F(FnlFrameSample, submit_ms);
	F(FnlFrameSample, present_ms);
	F(FnlFrameSample, rebuild_ms);
	F(FnlFrameSample, draw_ms);
	F(FnlFrameSample, presented);
	F(FnlFrameSample, gpu_ms);
	S(FnlFrameShown);
	F(FnlFrameShown, index);
	F(FnlFrameShown, time);
	F(FnlFrameShown, vblank);
	S(FnlColor);
	F(FnlColor, r);
	F(FnlColor, g);
	F(FnlColor, b);
	F(FnlColor, a);
	S(FnlEvent);
	F(FnlEvent, type);
	F(FnlEvent, time);
	F(FnlEvent, input_age);
	F(FnlEvent, key.key);
	F(FnlEvent, key.pressed);
	F(FnlEvent, key.repeat);
	F(FnlEvent, button.x);
	F(FnlEvent, button.y);
	F(FnlEvent, button.button);
	F(FnlEvent, button.pressed);
	F(FnlEvent, motion.x);
	F(FnlEvent, motion.y);
	F(FnlEvent, relative.dx);
	F(FnlEvent, relative.dy);
	F(FnlEvent, scroll.x);
	F(FnlEvent, scroll.y);
	F(FnlEvent, scroll.dx);
	F(FnlEvent, scroll.dy);
	F(FnlEvent, focus.focused);
	F(FnlEvent, resize.w_pt);
	F(FnlEvent, resize.h_pt);
	F(FnlEvent, resize.w_px);
	F(FnlEvent, resize.h_px);
	F(FnlEvent, scale.scale120);
	F(FnlEvent, window.fullscreen);
	F(FnlEvent, window.maximized);
	F(FnlEvent, antialias.aa);
	F(FnlEvent, scene.w_px);
	F(FnlEvent, scene.h_px);
	F(FnlEvent, capture.captured);
	F(FnlEvent, lost.count);
	F(FnlEvent, text.codepoint);
	F(FnlEvent, text.utf8);
	F(FnlEvent, compose.length);
	F(FnlEvent, clipboard.length);
	F(FnlEvent, output.w_pt);
	F(FnlEvent, output.h_pt);
	F(FnlEvent, gpu.index);
	F(FnlEvent, gpu.renderer);
#ifdef FNL_ABI_VULKAN
	S(FnlVulkanGpu);
	F(FnlVulkanGpu, instance);
	F(FnlVulkanGpu, physical_device);
	F(FnlVulkanGpu, device);
	F(FnlVulkanGpu, queue);
	F(FnlVulkanGpu, queue_family);
	F(FnlVulkanGpu, pipeline_cache);
	F(FnlVulkanGpu, get_instance_proc_addr);
	S(FnlVulkanSceneLayout);
	F(FnlVulkanSceneLayout, render_pass);
	F(FnlVulkanSceneLayout, subpass);
	F(FnlVulkanSceneLayout, samples);
	F(FnlVulkanSceneLayout, color_format);
	F(FnlVulkanSceneLayout, depth_format);
#endif
	S(FnlGlSceneLayout);
	F(FnlGlSceneLayout, samples);
	F(FnlGlSceneLayout, color_format);
	F(FnlGlSceneLayout, depth_format);
	E(FNL_OK);
	E(FNL_SKIPPED);
	E(FNL_ERR_UNSUPPORTED);
	E(FNL_ERR_NO_DISPLAY);
	E(FNL_ERR_NO_DEVICE);
	E(FNL_ERR_OUT_OF_MEMORY);
	E(FNL_ERR_DEVICE_LOST);
	E(FNL_ERR_INVALID_ARGUMENT);
	E(FNL_ERR_VERSION_MISMATCH);
	E(FNL_ERR_SYSTEM);
	E(FNL_LOG_ERROR);
	E(FNL_LOG_WARN);
	E(FNL_LOG_INFO);
	E(FNL_LOG_DEBUG);
	E(FNL_RENDERER_DEFAULT);
	E(FNL_RENDERER_VULKAN);
	E(FNL_RENDERER_OPENGL);
	E(FNL_RENDERER_D3D12);
	E(FNL_RENDERER_METAL);
	/* A macro and not an enum, but a value a compiled caller passes. */
	E(FNL_RENDERERS_ANY);
	E(FNL_GPU_KIND_UNKNOWN);
	E(FNL_GPU_KIND_INTEGRATED);
	E(FNL_GPU_KIND_DISCRETE);
	E(FNL_GPU_KIND_VIRTUAL);
	E(FNL_GPU_KIND_SOFTWARE);
	E(FNL_TARGET_DIRECT);
	E(FNL_TARGET_OFFSCREEN);
	E(FNL_TARGET_OFFSCREEN_MS);
	E(FNL_TARGET_COUNT);
	E(FNL_AA_NONE);
	E(FNL_AA_MSAA);
	E(FNL_AA_MLAA);
	E(FNL_KEY_UNKNOWN);
	E(FNL_KEY_W);
	E(FNL_KEY_A);
	E(FNL_KEY_S);
	E(FNL_KEY_D);
	E(FNL_KEY_UP);
	E(FNL_KEY_DOWN);
	E(FNL_KEY_LEFT);
	E(FNL_KEY_RIGHT);
	E(FNL_KEY_LSHIFT);
	E(FNL_KEY_RSHIFT);
	E(FNL_KEY_ESCAPE);
	E(FNL_KEY_F1);
	E(FNL_KEY_F2);
	E(FNL_KEY_F9);
	E(FNL_KEY_F10);
	E(FNL_KEY_F11);
	E(FNL_KEY_B);
	E(FNL_KEY_C);
	E(FNL_KEY_E);
	E(FNL_KEY_F);
	E(FNL_KEY_G);
	E(FNL_KEY_H);
	E(FNL_KEY_I);
	E(FNL_KEY_J);
	E(FNL_KEY_K);
	E(FNL_KEY_L);
	E(FNL_KEY_M);
	E(FNL_KEY_N);
	E(FNL_KEY_O);
	E(FNL_KEY_P);
	E(FNL_KEY_Q);
	E(FNL_KEY_R);
	E(FNL_KEY_T);
	E(FNL_KEY_U);
	E(FNL_KEY_V);
	E(FNL_KEY_X);
	E(FNL_KEY_Y);
	E(FNL_KEY_Z);
	E(FNL_KEY_0);
	E(FNL_KEY_1);
	E(FNL_KEY_2);
	E(FNL_KEY_3);
	E(FNL_KEY_4);
	E(FNL_KEY_5);
	E(FNL_KEY_6);
	E(FNL_KEY_7);
	E(FNL_KEY_8);
	E(FNL_KEY_9);
	E(FNL_KEY_F3);
	E(FNL_KEY_F4);
	E(FNL_KEY_F5);
	E(FNL_KEY_F6);
	E(FNL_KEY_F7);
	E(FNL_KEY_F8);
	E(FNL_KEY_F12);
	E(FNL_KEY_SPACE);
	E(FNL_KEY_ENTER);
	E(FNL_KEY_TAB);
	E(FNL_KEY_BACKSPACE);
	E(FNL_KEY_MINUS);
	E(FNL_KEY_EQUAL);
	E(FNL_KEY_LBRACKET);
	E(FNL_KEY_RBRACKET);
	E(FNL_KEY_BACKSLASH);
	E(FNL_KEY_SEMICOLON);
	E(FNL_KEY_APOSTROPHE);
	E(FNL_KEY_GRAVE);
	E(FNL_KEY_COMMA);
	E(FNL_KEY_PERIOD);
	E(FNL_KEY_SLASH);
	E(FNL_KEY_INTL_BACKSLASH);
	E(FNL_KEY_LCTRL);
	E(FNL_KEY_RCTRL);
	E(FNL_KEY_LALT);
	E(FNL_KEY_RALT);
	E(FNL_KEY_LSUPER);
	E(FNL_KEY_RSUPER);
	E(FNL_KEY_MENU);
	E(FNL_KEY_INSERT);
	E(FNL_KEY_DELETE);
	E(FNL_KEY_HOME);
	E(FNL_KEY_END);
	E(FNL_KEY_PAGE_UP);
	E(FNL_KEY_PAGE_DOWN);
	E(FNL_KEY_CAPS_LOCK);
	E(FNL_KEY_NUM_LOCK);
	E(FNL_KEY_SCROLL_LOCK);
	E(FNL_KEY_PRINT_SCREEN);
	E(FNL_KEY_PAUSE);
	E(FNL_KEY_KP_0);
	E(FNL_KEY_KP_1);
	E(FNL_KEY_KP_2);
	E(FNL_KEY_KP_3);
	E(FNL_KEY_KP_4);
	E(FNL_KEY_KP_5);
	E(FNL_KEY_KP_6);
	E(FNL_KEY_KP_7);
	E(FNL_KEY_KP_8);
	E(FNL_KEY_KP_9);
	E(FNL_KEY_KP_DECIMAL);
	E(FNL_KEY_KP_DIVIDE);
	E(FNL_KEY_KP_MULTIPLY);
	E(FNL_KEY_KP_SUBTRACT);
	E(FNL_KEY_KP_ADD);
	E(FNL_KEY_KP_ENTER);
	E(FNL_BUTTON_LEFT);
	E(FNL_BUTTON_MIDDLE);
	E(FNL_BUTTON_RIGHT);
	E(FNL_EVENT_KEY);
	E(FNL_EVENT_BUTTON);
	E(FNL_EVENT_MOTION);
	E(FNL_EVENT_RELATIVE);
	E(FNL_EVENT_SCROLL);
	E(FNL_EVENT_FOCUS);
	E(FNL_EVENT_CLOSE);
	E(FNL_EVENT_RESIZE);
	E(FNL_EVENT_SCALE);
	E(FNL_EVENT_WINDOW);
	E(FNL_EVENT_ANTIALIAS);
	E(FNL_EVENT_SCENE_SIZE);
	E(FNL_EVENT_CAPTURE);
	E(FNL_EVENT_LOST);
	E(FNL_EVENT_TEXT);
	E(FNL_EVENT_COMPOSE);
	E(FNL_EVENT_CLIPBOARD);
	E(FNL_EVENT_OUTPUT);
	E(FNL_EVENT_GPU);
	E(FNL_TEXT_BODY);
	E(FNL_TEXT_LARGE);
	E(FNL_TEXT_UI);
	E(FNL_TEXT_SIZE_COUNT);
	E(FNL_EDGE_NONE);
	E(FNL_EDGE_TOP);
	E(FNL_EDGE_BOTTOM);
	E(FNL_EDGE_LEFT);
	E(FNL_EDGE_RIGHT);
	E(FNL_EDGE_TOP_LEFT);
	E(FNL_EDGE_TOP_RIGHT);
	E(FNL_EDGE_BOTTOM_LEFT);
	E(FNL_EDGE_BOTTOM_RIGHT);
	E(FNL_CURSOR_DEFAULT);
	E(FNL_CURSOR_TEXT);
	E(FNL_CURSOR_POINTER);
	E(FNL_CURSOR_CROSSHAIR);
	E(FNL_CURSOR_MOVE);
	E(FNL_CURSOR_EW_RESIZE);
	E(FNL_CURSOR_NS_RESIZE);
	E(FNL_CURSOR_NOT_ALLOWED);
	E(FNL_CURSOR_WAIT);
	return 0;
}
