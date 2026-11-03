/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* What the KMS copy path would cost with the frame drawn on another GPU
 * than the one driving the display, with nothing of fenestal in it: the
 * question of FNL_GPU=nvidia on the KMS backend (TODO), where the display
 * belongs to the Intel GPU and the frames would be read back from the
 * NVIDIA one and written into the Intel side's dumb buffer, as
 * kms_present_pixels() writes lavapipe's. For each host-visible memory
 * type of the named device, `rounds` times:
 *
 *   gpu   an OPTIMAL w x h B8G8R8A8 image cleared and copied into a buffer
 *         of that type, submit to fence
 *   both  the buffer straight into a dumb buffer on the display's DRM node
 *         (its mapping, write-combined on i915) by memcpy, row by row, as
 *         kms_present_pixels() does; first, so it reads the frame cold
 *   read  then the buffer into malloc'd memory alone
 *   scan  and that memory into the dumb buffer alone
 *
 * By glibc's memcpy throughout, as the copy path copies: from an uncached
 * mapping streaming loads would read faster.
 *
 *   kmscopy [device-name-part [card [w h [rounds]]]]
 *           defaults: NVK, /dev/dri/card1, 1920 1080, 20
 *
 * Medians in ms. Needs no window server and no display: a dumb buffer is
 * made and mapped without being master.
 *
 *   gcc -std=c11 -O2 -Wall -Wextra -o build/kmscopy tools/probe/kmscopy.c \
 *       -lvulkan */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                              \
	do {                                                                      \
		VkResult r_ = (x);                                                    \
		if (r_ != VK_SUCCESS) {                                               \
			fprintf(stderr, "kmscopy: %s: %d\n", #x, r_);                     \
			exit(2);                                                          \
		}                                                                     \
	} while (0)

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static double
median(double *v, int n)
{
	qsort(v, n, sizeof *v, cmp_double);
	return v[n / 2];
}

static const char *
flags_str(VkMemoryPropertyFlags f)
{
	static char s[64];
	snprintf(s, sizeof s, "%s%s%s%s",
			 f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEVICE_LOCAL " : "",
			 f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "VISIBLE " : "",
			 f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "COHERENT " : "",
			 f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "CACHED" : "");
	return s;
}

int
main(int argc, char **argv)
{
	const char *want = argc > 1 ? argv[1] : "NVK";
	const char *card = argc > 2 ? argv[2] : "/dev/dri/card1";
	uint32_t w = argc > 4 ? (uint32_t)atoi(argv[3]) : 1920;
	uint32_t h = argc > 4 ? (uint32_t)atoi(argv[4]) : 1080;
	int rounds = argc > 5 ? atoi(argv[5]) : 20;
	if (rounds < 1 || rounds > 1000)
		rounds = 20;
	VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

	/* The display side: a dumb buffer on the card, mapped as the copy path
	 * maps its scanout buffers. */
	int fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(card);
		return 2;
	}
	char drv[32] = "";
	struct drm_version ver = { .name = drv, .name_len = sizeof drv - 1 };
	ioctl(fd, DRM_IOCTL_VERSION, &ver);
	struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return 2;
	}
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return 2;
	}
	unsigned char *scan = mmap(NULL, cd.size, PROT_READ | PROT_WRITE,
							   MAP_SHARED, fd, (off_t)md.offset);
	if (scan == MAP_FAILED) {
		perror("mmap dumb");
		return 2;
	}
	printf("display: %s (%s), dumb buffer %ux%u pitch %u\n", card, drv, w, h,
		   cd.pitch);

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							  .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties props;
	for (uint32_t i = 0; i < n && !pd; i++) {
		vkGetPhysicalDeviceProperties(pds[i], &props);
		if (strstr(props.deviceName, want))
			pd = pds[i];
	}
	if (!pd) {
		fprintf(stderr, "kmscopy: no device named %s\n", want);
		return 2;
	}
	printf("render: %s, %ux%u B8G8R8A8, %.1f MB a frame, %d rounds\n",
		   props.deviceName, w, h, bytes / 1e6, rounds);

	uint32_t nq = 16, qf = UINT32_MAX;
	VkQueueFamilyProperties qp[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
	for (uint32_t q = 0; q < nq && qf == UINT32_MAX; q++)
		if (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			qf = q;
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qf,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
	};
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);

	/* The frame: an OPTIMAL image in the device's own memory, as a
	 * swapchain image is. */
	VkImageCreateInfo imci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	};
	VkImage img;
	CHECK(vkCreateImage(dev, &imci, NULL, &img));
	VkMemoryRequirements ir;
	vkGetImageMemoryRequirements(dev, img, &ir);
	uint32_t it = UINT32_MAX;
	for (uint32_t t = 0; t < mp.memoryTypeCount && it == UINT32_MAX; t++)
		if ((ir.memoryTypeBits & (1u << t)) &&
			(mp.memoryTypes[t].propertyFlags &
			 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			it = t;
	for (uint32_t t = 0; t < mp.memoryTypeCount && it == UINT32_MAX; t++)
		if (ir.memoryTypeBits & (1u << t))
			it = t;
	VkMemoryAllocateInfo iai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = ir.size,
		.memoryTypeIndex = it,
	};
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &iai, NULL, &imem));
	CHECK(vkBindImageMemory(dev, img, imem, 0));

	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = qf,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	unsigned char *host = malloc(bytes);
	if (!host)
		return 2;

	printf("%-4s %-36s %8s %8s %8s %8s\n", "type", "flags", "gpu", "read",
		   "scan", "both");
	for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
		if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			continue;
		VkBufferCreateInfo bci = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = bytes,
			.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		};
		VkBuffer buf;
		CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
		VkMemoryRequirements br;
		vkGetBufferMemoryRequirements(dev, buf, &br);
		if (!(br.memoryTypeBits & (1u << t))) {
			printf("%-4u %-36s  (not for a buffer)\n", t, flags_str(f));
			vkDestroyBuffer(dev, buf, NULL);
			continue;
		}
		VkMemoryAllocateInfo bai = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = br.size,
			.memoryTypeIndex = t,
		};
		VkDeviceMemory bmem;
		if (vkAllocateMemory(dev, &bai, NULL, &bmem) != VK_SUCCESS) {
			printf("%-4u %-36s  (allocation failed)\n", t, flags_str(f));
			vkDestroyBuffer(dev, buf, NULL);
			continue;
		}
		CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
		unsigned char *map;
		CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map));

		double gpu[1000], rd[1000], sc[1000], both[1000];
		for (int r = 0; r < rounds; r++) {
			CHECK(vkResetCommandBuffer(cmd, 0));
			VkCommandBufferBeginInfo cbi = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			CHECK(vkBeginCommandBuffer(cmd, &cbi));
			VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
											  0, 1 };
			VkImageMemoryBarrier b1 = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = img,
				.subresourceRange = range,
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
								 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
								 NULL, 1, &b1);
			VkClearColorValue c = { .float32 = { r & 1, 0.5f, 0.25f, 1 } };
			vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								 &c, 1, &range);
			VkImageMemoryBarrier b2 = b1;
			b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
								 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
								 NULL, 1, &b2);
			VkBufferImageCopy region = {
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.imageExtent = { w, h, 1 },
			};
			vkCmdCopyImageToBuffer(cmd, img,
								   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
								   &region);
			VkBufferMemoryBarrier hb = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = buf,
				.size = VK_WHOLE_SIZE,
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
								 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb,
								 0, NULL);
			CHECK(vkEndCommandBuffer(cmd));
			VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
								.commandBufferCount = 1,
								.pCommandBuffers = &cmd };
			CHECK(vkResetFences(dev, 1, &fence));
			double t0 = now_ms();
			CHECK(vkQueueSubmit(queue, 1, &si, fence));
			CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
			gpu[r] = now_ms() - t0;
			if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
				VkMappedMemoryRange mr = {
					.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
					.memory = bmem,
					.size = VK_WHOLE_SIZE,
				};
				CHECK(vkInvalidateMappedMemoryRanges(dev, 1, &mr));
			}
			t0 = now_ms();
			for (uint32_t y = 0; y < h; y++)
				memcpy(scan + (size_t)y * cd.pitch, map + (size_t)y * w * 4,
					   (size_t)w * 4);
			both[r] = now_ms() - t0;
			t0 = now_ms();
			memcpy(host, map, bytes);
			rd[r] = now_ms() - t0;
			t0 = now_ms();
			for (uint32_t y = 0; y < h; y++)
				memcpy(scan + (size_t)y * cd.pitch, host + (size_t)y * w * 4,
					   (size_t)w * 4);
			sc[r] = now_ms() - t0;
		}
		/* The last round's clear (0xffff8040 or 0xffff7f40 by rounding)
		 * reached the dumb buffer: a wrong copy would make every figure
		 * meaningless. One pixel, so a check, not a proof. */
		uint32_t px;
		memcpy(&px, scan, 4);
		printf("%-4u %-36s %8.2f %8.2f %8.2f %8.2f   first pixel %08x\n", t,
			   flags_str(f), median(gpu, rounds), median(rd, rounds),
			   median(sc, rounds), median(both, rounds), px);
		vkUnmapMemory(dev, bmem);
		vkFreeMemory(dev, bmem, NULL);
		vkDestroyBuffer(dev, buf, NULL);
	}
	return 0;
}
