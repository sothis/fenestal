/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Vulkan's frames presented through a DXGI swapchain on Win32: the one file
 * of this renderer that names DXGI and Direct3D 11 (tools/check-layering.py
 * excuses it), as the bridges are the ones that name a window server.
 *
 * Vulkan on Win32 has no resize in place: a surface's extent is its
 * window's, so every size is a new swapchain, and Intel's driver takes 25
 * to 30 ms over one -- 33 to 44 fps through a drag at three quarters of a
 * core (DESIGN *Measured on Windows itself*). DXGI resizes in place:
 * buffers larger than the window, shown from their corner and clipped by
 * it, as on Direct3D 12 (d3d_window_images()). And Windows shows a
 * DXGI swapchain sooner: on all four laptop GPUs it is flipped onto a
 * plane, a refresh after its present, where NVIDIA's own swapchains are
 * composed or kept a refresh more (DESIGN *Vulkan through DXGI*). So
 * wherever the device can, the renderer draws into images of its own
 * (RendererState::surfaceless) that are Direct3D 11 textures shared with
 * it, and each frame is copied into the swapchain's back buffer by a
 * Direct3D 11 device on the same adapter and presented there. Direct3D 11
 * and not 12: Intel's driver imports no Direct3D 12 resource and no
 * Direct3D 12 fence, and does import a Direct3D 11 texture with its keyed
 * mutex, which orders the two devices' work on it. d3d11.dll is loaded at
 * run time, the link line being the system's own libraries (README).
 * DESIGN *Vulkan through DXGI*.
 */

#define VK_USE_PLATFORM_WIN32_KHR

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "backend/win32/win32_state.h"

#include <vulkan/vulkan_win32.h>

#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_5.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
	HMODULE              d3d11;
	ID3D11Device        *dev;
	ID3D11DeviceContext *ctx;
	IDXGISwapChain2     *sc;
	HANDLE               wait;
	ID3D11Texture2D     *tex[OVERLAY_MAX_IMAGES];
	IDXGIKeyedMutex     *km[OVERLAY_MAX_IMAGES];
	uint32_t             n;
	int                  buf_w, buf_h;
	bool                 acquire_failed_said;
	/* Vsync off where the factory offers tearing: the swapchain made for
	 * it, its flags kept for each resize, and every present tearing. */
	bool                 tear;
	UINT                 sc_flags;
	PFN_vkGetMemoryWin32HandlePropertiesKHR handle_props;
	/* What a frame's submit carries: the image's keyed mutex taken at key
	 * 0, which Direct3D 11 left it at, and given back at key 1, which the
	 * copy takes. */
	VkWin32KeyedMutexAcquireReleaseInfoKHR km_info;
	VkDeviceMemory       km_mem;
	uint64_t             key_acquire, key_release;
	uint32_t             km_timeout;
} VkDxgi;

#define DX(a) ((VkDxgi *)VK(a)->dxgi_state)

/* The swapchain's buffers. Three: with two, a window DWM composes -- one
 * being resized, its buffers larger than it -- has one buffer DWM holds
 * and one the copy waits for, and the frame handed over waits on that
 * copy, two refreshes a frame on aspire's WDDM 2.1 display: 30 fps through
 * a drag on both its GPUs, where three hold 60, and flat out 715 and 1378
 * fps on the HD 530 and the 960M against 459 and 871. From latch to screen
 * at rest it is 20.2 ms either way. */
#define DXGI_BUFFERS 3

static void dxgi_textures_release(VkDxgi *d)
{
	for (uint32_t i = 0; i < d->n; i++) {
		if (d->km[i])
			IDXGIKeyedMutex_Release(d->km[i]);
		if (d->tex[i])
			ID3D11Texture2D_Release(d->tex[i]);
		d->km[i] = NULL;
		d->tex[i] = NULL;
	}
	d->n = 0;
}

void vk_dxgi_end(App *a)
{
	VkDxgi *d = DX(a);
	if (!d)
		return;
	dxgi_textures_release(d);
	if (d->sc)
		IDXGISwapChain2_Release(d->sc);
	if (d->ctx) {
		ID3D11DeviceContext_ClearState(d->ctx);
		ID3D11DeviceContext_Flush(d->ctx);
		ID3D11DeviceContext_Release(d->ctx);
	}
	if (d->dev)
		ID3D11Device_Release(d->dev);
	if (d->d3d11)
		FreeLibrary(d->d3d11);
	free(d);
	VK(a)->dxgi_state = NULL;
}

/* The Direct3D 11 device on the adapter the Vulkan device is, by LUID, and
 * the swapchain on the window, at `e`. */
static FnlResult dxgi_open(App *a, VkExtent2D e)
{
	VkDxgi *d = DX(a);
	PFN_vkGetPhysicalDeviceProperties2KHR props2 =
		(PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(
			VK(a)->instance, "vkGetPhysicalDeviceProperties2KHR");
	if (!props2)
		return fnl_fail(FNL_ERR_NO_DEVICE, "DXGI present: no "
						"vkGetPhysicalDeviceProperties2KHR");
	VkPhysicalDeviceIDPropertiesKHR id = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR };
	VkPhysicalDeviceProperties2KHR p = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
		.pNext = &id };
	props2(VK(a)->phys, &p);
	if (!id.deviceLUIDValid)
		return fnl_fail(FNL_ERR_NO_DEVICE, "DXGI present: the device has "
						"no LUID");

	IDXGIFactory2 *f = NULL;
	if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&f)))
		return fnl_fail(FNL_ERR_NO_DEVICE, "DXGI present: no factory");
	IDXGIAdapter1 *ad = NULL;
	for (UINT i = 0;
		 IDXGIFactory2_EnumAdapters1(f, i, &ad) != DXGI_ERROR_NOT_FOUND;
		 i++) {
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &desc)) &&
			!memcmp(&desc.AdapterLuid, id.deviceLUID, sizeof desc.AdapterLuid))
			break;
		IDXGIAdapter1_Release(ad);
		ad = NULL;
	}
	if (!ad) {
		IDXGIFactory2_Release(f);
		return fnl_fail(FNL_ERR_NO_DEVICE, "DXGI present: no adapter has "
						"the Vulkan device's LUID");
	}
	d->d3d11 = LoadLibraryW(L"d3d11.dll");
	PFN_D3D11_CREATE_DEVICE make = d->d3d11
		? (PFN_D3D11_CREATE_DEVICE)(void (*)(void))GetProcAddress(
			  d->d3d11, "D3D11CreateDevice")
		: NULL;
	HRESULT hr = make ? make((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN,
							 NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
							 D3D11_SDK_VERSION, &d->dev, NULL, &d->ctx)
					  : E_NOINTERFACE;
	IDXGIAdapter1_Release(ad);
	if (FAILED(hr)) {
		IDXGIFactory2_Release(f);
		return fnl_fail(FNL_ERR_NO_DEVICE, "DXGI present: D3D11CreateDevice "
						"0x%08lx", (unsigned long)hr);
	}
	/* Tearing only with vsync off, as on Direct3D 12 (surface_win32.c):
	 * with vsync on the flag cost a refresh there. */
	d->tear = false;
	IDXGIFactory5 *f5 = NULL;
	if (!fnl_atomic_load_bool(&a->vsync) &&
		SUCCEEDED(IDXGIFactory2_QueryInterface(f, &IID_IDXGIFactory5,
											   (void **)&f5))) {
		BOOL allow = FALSE;
		if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(
				f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
				sizeof allow)))
			d->tear = allow != FALSE;
		IDXGIFactory5_Release(f5);
	}
	d->sc_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
				  (d->tear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	DXGI_SWAP_CHAIN_DESC1 sd = {
		.Width = e.width, .Height = e.height,
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { 1, 0 },
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = DXGI_BUFFERS,
		.Scaling = DXGI_SCALING_NONE,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
		.Flags = d->sc_flags,
	};
	IDXGISwapChain1 *sc1 = NULL;
	hr = IDXGIFactory2_CreateSwapChainForHwnd(
		f, (IUnknown *)d->dev, win32_present_hwnd(a->backend_state), &sd,
		NULL, NULL, &sc1);
	if (SUCCEEDED(hr))
		IDXGIFactory2_MakeWindowAssociation(f, a->backend_state->hwnd,
											DXGI_MWA_NO_ALT_ENTER);
	IDXGIFactory2_Release(f);
	if (FAILED(hr))
		return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: "
						"CreateSwapChainForHwnd 0x%08lx", (unsigned long)hr);
	hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain2,
										(void **)&d->sc);
	IDXGISwapChain1_Release(sc1);
	if (FAILED(hr))
		return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: no IDXGISwapChain2");
	IDXGISwapChain2_SetMaximumFrameLatency(d->sc, 1);
	d->wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(d->sc);
	d->buf_w = (int)e.width;
	d->buf_h = (int)e.height;
	d->handle_props = (PFN_vkGetMemoryWin32HandlePropertiesKHR)
		vkGetDeviceProcAddr(VK(a)->device,
							"vkGetMemoryWin32HandlePropertiesKHR");
	fnl_log(FNL_LOG_INFO, "the frames go through a DXGI swapchain, "
			"copied by Direct3D 11");
	return FNL_OK;
}

/* One shared texture, imported as the window's image `i`. */
static FnlResult dxgi_image(App *a, VkExtent2D e, uint32_t i)
{
	VkDxgi *d = DX(a);
	const D3D11_TEXTURE2D_DESC td = {
		.Width = e.width, .Height = e.height, .MipLevels = 1,
		.ArraySize = 1, .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
		.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
					 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
	};
	HRESULT hr = ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->tex[i]);
	if (FAILED(hr))
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "DXGI present: texture "
						"0x%08lx", (unsigned long)hr);
	d->n = i + 1;
	ID3D11Texture2D_QueryInterface(d->tex[i], &IID_IDXGIKeyedMutex,
								   (void **)&d->km[i]);
	IDXGIResource1 *r = NULL;
	HANDLE h = NULL;
	hr = ID3D11Texture2D_QueryInterface(d->tex[i], &IID_IDXGIResource1,
										(void **)&r);
	if (SUCCEEDED(hr)) {
		hr = IDXGIResource1_CreateSharedHandle(
			r, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			NULL, &h);
		IDXGIResource1_Release(r);
	}
	if (FAILED(hr) || !d->km[i]) {
		if (SUCCEEDED(hr))
			CloseHandle(h);
		return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: no shared handle "
						"0x%08lx", (unsigned long)hr);
	}

	TargetImage *t = &VK(a)->own[i];
	const VkExternalMemoryImageCreateInfoKHR ext = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR,
	};
	const VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK(a)->surf_format.format,
		.extent = { e.width, e.height, 1 },
		.mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	/* Each handle is nulled on a failure, which leaves it undefined: the
	 * teardown destroys every own[] entry (retired_destroy()). */
	VkResult vr = vkCreateImage(VK(a)->device, &ici, NULL, &t->image);
	if (vr != VK_SUCCESS) {
		t->image = VK_NULL_HANDLE;
		CloseHandle(h);
		return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: vkCreateImage %d",
						(int)vr);
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(VK(a)->device, t->image, &req);
	VkMemoryWin32HandlePropertiesKHR hp = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
	if (d->handle_props)
		d->handle_props(VK(a)->device,
						VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR,
						h, &hp);
	uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
	if (!bits)
		bits = hp.memoryTypeBits ? hp.memoryTypeBits : req.memoryTypeBits;
	uint32_t type = 0;
	while (type < 31 && !(bits & (1u << type)))
		type++;
	const VkMemoryDedicatedAllocateInfoKHR ded = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
		.image = t->image,
	};
	const VkImportMemoryWin32HandleInfoKHR imp = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
		.pNext = &ded,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR,
		.handle = h,
	};
	const VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &imp,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	vr = vkAllocateMemory(VK(a)->device, &mai, NULL, &t->mem);
	CloseHandle(h);
	if (vr != VK_SUCCESS) {
		t->mem = VK_NULL_HANDLE;
		return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: the import %d",
						(int)vr);
	}
	vr = vkBindImageMemory(VK(a)->device, t->image, t->mem, 0);
	if (vr == VK_SUCCESS)
		vr = make_image_view(a, t->image, VK(a)->surf_format.format,
							 VK_IMAGE_ASPECT_COLOR_BIT, &VK(a)->views[i]);
	if (vr != VK_SUCCESS)
		return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: bind or view %d",
						(int)vr);
	VK(a)->image_count = i + 1;
	return FNL_OK;
}

/* The window's images at `e`: the swapchain's buffers made that size as
 * well. The textures before them go now: Vulkan's imports hold the memory
 * they share for as long as the retired images need it. */
FnlResult vk_dxgi_own_images(App *a, VkExtent2D e, uint32_t n)
{
	VkDxgi *d = DX(a);
	VK(a)->swapchain = VK_NULL_HANDLE;
	VK(a)->own_next = 0;
	VK(a)->snap_usage = true;
	if (!d->dev)
		FNL_TRY(dxgi_open(a, e));
	dxgi_textures_release(d);
	if ((int)e.width != d->buf_w || (int)e.height != d->buf_h) {
		ID3D11DeviceContext_ClearState(d->ctx);
		ID3D11DeviceContext_Flush(d->ctx);
		const HRESULT hr = IDXGISwapChain2_ResizeBuffers(
			d->sc, DXGI_BUFFERS, e.width, e.height, DXGI_FORMAT_B8G8R8A8_UNORM,
			d->sc_flags);
		if (FAILED(hr))
			return fnl_fail(FNL_ERR_SYSTEM, "DXGI present: ResizeBuffers "
							"0x%08lx", (unsigned long)hr);
		d->buf_w = (int)e.width;
		d->buf_h = (int)e.height;
	}
	for (uint32_t i = 0; i < n; i++)
		FNL_TRY(dxgi_image(a, e, i));
	VK(a)->swap_fresh  = true;
	VK(a)->swap_agreed = true;
	return FNL_OK;
}

const void *vk_dxgi_submit_chain(App *a, uint32_t img)
{
	VkDxgi *d = DX(a);
	d->km_mem = VK(a)->own[img].mem;
	d->key_acquire = 0;
	d->key_release = 1;
	d->km_timeout = 2000;
	d->km_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR){
		.sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR,
		.acquireCount = 1,
		.pAcquireSyncs = &d->km_mem,
		.pAcquireKeys = &d->key_acquire,
		.pAcquireTimeouts = &d->km_timeout,
		.releaseCount = 1,
		.pReleaseSyncs = &d->km_mem,
		.pReleaseKeys = &d->key_release,
	};
	return &d->km_info;
}

/* For fnl_frame_shown(), as Direct3D 12's frame.c does it: the number DXGI
 * gave the present just made, as the frame being drawn (App::presents,
 * counted once this returns), and the number it last showed with the
 * vertical blank's QPC time, now_seconds()'s clock: a time a frame was
 * shown at, unless it tore (`tear`), when it is only the blank the
 * statistics name. Never waits. */
static void shown_stamp(App *a, VkDxgi *d, double at)
{
	UINT number;
	if (FAILED(IDXGISwapChain2_GetLastPresentCount(d->sc, &number)))
		return;
	app_present_numbered(a, a->presents, number, at);
	DXGI_FRAME_STATISTICS st;
	LARGE_INTEGER freq;
	if (FAILED(IDXGISwapChain2_GetFrameStatistics(d->sc, &st)) ||
		!QueryPerformanceFrequency(&freq) || !freq.QuadPart)
		return;
	app_present_number_shown(a, st.PresentCount,
							 (double)st.SyncQPCTime.QuadPart /
								 (double)freq.QuadPart, !d->tear);
}

/* The frame's image, once its submit has given it back at key 1: the
 * window's part copied into the back buffer and presented, and the image
 * given back at key 0 for the frame that draws into it next. */
VkResult vk_dxgi_present(App *a, uint32_t img)
{
	VkDxgi *d = DX(a);
	if (img >= d->n)
		return VK_ERROR_OUT_OF_DATE_KHR;
	/* The latency object, but not when the frames tear. It is released
	 * once a refresh as the display takes a frame, so with vsync off it
	 * held the rate to the display's: 60 fps on the GTX 960M, where its
	 * own swapchain's mailbox made 774. Nor is vsync off without it,
	 * where the factory offers no tearing: a present then queues behind
	 * the last, flipped at a refresh too, and with no wait the queue
	 * fills -- 60 fps still, and 132 ms from latch to screen. Tearing,
	 * the keyed mutex paces the frames. The swapchain is made for vsync
	 * once (fnl_init() fixes it), so no skipped count is owed. */
	const bool vsync = fnl_atomic_load_bool(&a->vsync);
	if (d->wait && !d->tear)
		WaitForSingleObjectEx(d->wait, 100, TRUE);
	HRESULT hr = IDXGIKeyedMutex_AcquireSync(d->km[img], 1, 2000);
	if (hr != S_OK) {
		if (!d->acquire_failed_said)
			fnl_log(FNL_LOG_WARN, "DXGI present: the image was not handed "
					"over (0x%08lx)", (unsigned long)hr);
		d->acquire_failed_said = true;
		return VK_ERROR_OUT_OF_DATE_KHR;
	}
	ID3D11Texture2D *back = NULL;
	hr = IDXGISwapChain2_GetBuffer(d->sc, 0, &IID_ID3D11Texture2D,
								   (void **)&back);
	if (SUCCEEDED(hr)) {
		const D3D11_BOX box = { 0, 0, 0, (UINT)a->extent.w,
								(UINT)a->extent.h, 1 };
		ID3D11DeviceContext_CopySubresourceRegion(
			d->ctx, (ID3D11Resource *)back, 0, 0, 0, 0,
			(ID3D11Resource *)d->tex[img], 0, &box);
		ID3D11Texture2D_Release(back);
	}
	IDXGIKeyedMutex_ReleaseSync(d->km[img], 0);
	const double at = now_seconds();
	hr = IDXGISwapChain2_Present(d->sc, vsync ? 1 : 0,
								 d->tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		return VK_ERROR_DEVICE_LOST;
	if (SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED && a->counting)
		shown_stamp(a, d, at);
	return FAILED(hr) ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
}

/* ---- the choice ---------------------------------------------------------- */

/* What sharing an image with Direct3D 11 takes of a device, each enabled
 * where offered (VkBridge::device_extensions). */
static const char *const SHARE_EXTS[] = {
	"VK_KHR_external_memory", "VK_KHR_external_memory_win32",
	"VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
	"VK_KHR_win32_keyed_mutex",
};

const char *const *vk_dxgi_device_extensions(uint32_t *count)
{
	*count = (uint32_t)(sizeof SHARE_EXTS / sizeof SHARE_EXTS[0]);
	return SHARE_EXTS;
}

/* Whether `dev` can share an image with Direct3D 11: every extension that
 * takes, and an image of the window's format importable from a Direct3D 11
 * texture. Of `inst` alone (VkBridge::shares). */
bool vk_dxgi_shares(VkInstance inst, VkPhysicalDevice dev)
{
	/* Never under wine, whose winevulkan offers every extension below and
	 * whose DXGI then has no adapter by the device's LUID and no shared
	 * handle (E_NOTIMPL), or, where DXVK's dxgi.dll is loaded for
	 * vkd3d-proton, aborts the program in D3D11CreateDevice. Wine is told
	 * by ntdll exporting wine_get_version, as Direct3D 12 tells it
	 * (d3d_bridge_swapchain()). */
	const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	if (nt && GetProcAddress(nt, "wine_get_version"))
		return false;
	uint32_t n = 0;
	if (vkEnumerateDeviceExtensionProperties(dev, NULL, &n, NULL) !=
			VK_SUCCESS || n == 0)
		return false;
	VkExtensionProperties *ext = calloc(n, sizeof *ext);
	if (!ext)
		return false;
	int have = 0;
	if (vkEnumerateDeviceExtensionProperties(dev, NULL, &n, ext) ==
			VK_SUCCESS)
		for (size_t w = 0; w < sizeof SHARE_EXTS / sizeof SHARE_EXTS[0]; w++)
			for (uint32_t i = 0; i < n; i++)
				if (!strcmp(ext[i].extensionName, SHARE_EXTS[w])) {
					have++;
					break;
				}
	free(ext);
	if (have != (int)(sizeof SHARE_EXTS / sizeof SHARE_EXTS[0]))
		return false;

	PFN_vkGetPhysicalDeviceImageFormatProperties2KHR fmt2 =
		(PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)
			vkGetInstanceProcAddr(
				inst, "vkGetPhysicalDeviceImageFormatProperties2KHR");
	if (!fmt2)
		return false;
	VkPhysicalDeviceExternalImageFormatInfoKHR info_ext = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR,
	};
	VkPhysicalDeviceImageFormatInfo2KHR info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR,
		.pNext = &info_ext,
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	};
	VkExternalImageFormatPropertiesKHR out = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR };
	VkImageFormatProperties2KHR props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR,
		.pNext = &out };
	return fmt2(dev, &info, &props) == VK_SUCCESS &&
		   (out.externalMemoryProperties.externalMemoryFeatures &
			VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR);
}

/* The same of the device picked, which create_device() made with those
 * extensions where offered, on an instance that has the one telling what
 * an image imports. */
static bool dxgi_offered(App *a)
{
	return VK(a)->dxgi_exts && (VK(a)->backend_ext_on & 1u) &&
		   vk_dxgi_shares(VK(a)->instance, VK(a)->phys);
}

/* After the device is made and before anything is built for the window:
 * the way the GPU list's entry chosen by index says (VkBridge::shares),
 * else as FNL_VK_DXGI says (1 or 0), else DXGI wherever the device can
 * share images with Direct3D 11. The device's own swapchain was the
 * default where its rebuild was fast, and is the slower way to the screen
 * on every GPU measured: 36 ms from latch to screen on the GTX 1050 Ti and
 * 73 on the GTX 960M, against 20 through DXGI, and flat out 774 fps on the
 * 960M against 871. */
FnlResult vk_dxgi_choose(App *a)
{
	const int env = vk_dxgi_env();
	bool forced = env >= 0;
	bool want = env == 1;
	if (VK(a)->pick_via >= 0) {
		forced = true;
		want = VK(a)->pick_via == 1;
	}
	VK(a)->dxgi = VK(a)->surfaceless = false;
	if (forced && !want)
		return FNL_OK;
	if (!dxgi_offered(a)) {
		if (forced)
			fnl_log(FNL_LOG_WARN, "FNL_VK_DXGI: this device shares no image "
					"with Direct3D 11; its own swapchain it is");
		return FNL_OK;
	}
	if (!VK(a)->dxgi_state) {
		VK(a)->dxgi_state = calloc(1, sizeof(VkDxgi));
		if (!VK(a)->dxgi_state)
			return FNL_OK;      /* the swapchain, then */
	}
	VK(a)->dxgi = VK(a)->surfaceless = true;
	return FNL_OK;
}
