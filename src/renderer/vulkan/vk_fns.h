/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Vulkan renderer's every entry point, called through a pointer the loader
 * hands back rather than linked: the loader is opened at run time
 * (vk_load.c), so that a machine without one still loads the program and
 * fnl_init() steps past Vulkan to the next renderer (DESIGN *One build per
 * operating system*, *The link line*).
 *
 * The prototypes stay, so every call site reads as Vulkan's own and the
 * pointer each is redirected to takes its type from the function it stands
 * for. **A function the renderer calls and these lists do not name still
 * compiles**, and links against the system's loader, which is the thing
 * this file removes: a new call wants a row in its list and a line below.
 *
 * What a window server's header declares -- vkCreateXcbSurfaceKHR and the
 * rest -- is not here, since this header sees no window server; each
 * surface_*.c fetches its own by vkGetInstanceProcAddr at the point of use.
 * So is every entry of an instance extension the instance may be made
 * without, which the loader hands back as NULL there: VK_KHR_display's, in
 * surface_kms.c. VK_KHR_surface is always enabled and VK_KHR_swapchain's
 * entries are the loader's whatever is enabled, so theirs are below.
 *
 * Included by vk_internal.h, ahead of every call.
 */

#ifndef FNL_VK_FNS_H
#define FNL_VK_FNS_H

#include <vulkan/vulkan.h>

/* Inside an X-macro the argument is used only as #f or fnl_##f, neither of
 * which expands it; a bare f would turn into the redirect below wherever
 * the macro is used after it. */

/* The loader's own, taken by vkGetInstanceProcAddr(NULL, ...). */
#define VK_GLOBAL_FNS(X)                                                      \
	X(vkCreateInstance)                                                       \
	X(vkEnumerateInstanceExtensionProperties)

/* Taken by vkGetInstanceProcAddr(instance, ...) once the instance is made.
 * One App at a time, so one set, loaded again for each instance. */
#define VK_INSTANCE_FNS(X)                                                    \
	X(vkDestroyInstance)                                                      \
	X(vkEnumeratePhysicalDevices)                                             \
	X(vkGetPhysicalDeviceProperties)                                          \
	X(vkGetPhysicalDeviceFeatures)                                            \
	X(vkGetPhysicalDeviceFormatProperties)                                    \
	X(vkGetPhysicalDeviceMemoryProperties)                                    \
	X(vkGetPhysicalDeviceQueueFamilyProperties)                               \
	X(vkEnumerateDeviceExtensionProperties)                                   \
	X(vkCreateDevice)                                                         \
	X(vkGetDeviceProcAddr)                                                    \
	X(vkDestroySurfaceKHR)                                                    \
	X(vkGetPhysicalDeviceSurfaceSupportKHR)                                   \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                              \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                   \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR)                              \
	/* and the device's, through the loader's trampolines */                  \
	X(vkAcquireNextImageKHR)                                                  \
	X(vkAllocateCommandBuffers)                                               \
	X(vkAllocateDescriptorSets)                                               \
	X(vkAllocateMemory)                                                       \
	X(vkBeginCommandBuffer)                                                   \
	X(vkBindBufferMemory)                                                     \
	X(vkBindImageMemory)                                                      \
	X(vkCmdBeginRenderPass)                                                   \
	X(vkCmdBindDescriptorSets)                                                \
	X(vkCmdBindPipeline)                                                      \
	X(vkCmdBindVertexBuffers)                                                 \
	X(vkCmdClearAttachments)                                                  \
	X(vkCmdCopyBufferToImage)                                                 \
	X(vkCmdCopyImageToBuffer)                                                 \
	X(vkCmdDraw)                                                              \
	X(vkCmdEndRenderPass)                                                     \
	X(vkCmdPipelineBarrier)                                                   \
	X(vkCmdPushConstants)                                                     \
	X(vkCmdResetQueryPool)                                                    \
	X(vkCmdSetBlendConstants)                                                 \
	X(vkCmdSetScissor)                                                        \
	X(vkCmdSetViewport)                                                       \
	X(vkCmdWriteTimestamp)                                                    \
	X(vkCreateBuffer)                                                         \
	X(vkCreateCommandPool)                                                    \
	X(vkCreateDescriptorPool)                                                 \
	X(vkCreateDescriptorSetLayout)                                            \
	X(vkCreateFence)                                                          \
	X(vkCreateFramebuffer)                                                    \
	X(vkCreateGraphicsPipelines)                                              \
	X(vkCreateImage)                                                          \
	X(vkCreateImageView)                                                      \
	X(vkCreatePipelineCache)                                                  \
	X(vkCreatePipelineLayout)                                                 \
	X(vkCreateQueryPool)                                                      \
	X(vkCreateRenderPass)                                                     \
	X(vkCreateSampler)                                                        \
	X(vkCreateSemaphore)                                                      \
	X(vkCreateShaderModule)                                                   \
	X(vkCreateSwapchainKHR)                                                   \
	X(vkDestroyBuffer)                                                        \
	X(vkDestroyCommandPool)                                                   \
	X(vkDestroyDescriptorPool)                                                \
	X(vkDestroyDescriptorSetLayout)                                           \
	X(vkDestroyDevice)                                                        \
	X(vkDestroyFence)                                                         \
	X(vkDestroyFramebuffer)                                                   \
	X(vkDestroyImage)                                                         \
	X(vkDestroyImageView)                                                     \
	X(vkDestroyPipeline)                                                      \
	X(vkDestroyPipelineCache)                                                 \
	X(vkDestroyPipelineLayout)                                                \
	X(vkDestroyQueryPool)                                                     \
	X(vkDestroyRenderPass)                                                    \
	X(vkDestroySampler)                                                       \
	X(vkDestroySemaphore)                                                     \
	X(vkDestroyShaderModule)                                                  \
	X(vkDestroySwapchainKHR)                                                  \
	X(vkDeviceWaitIdle)                                                       \
	X(vkEndCommandBuffer)                                                     \
	X(vkFreeCommandBuffers)                                                   \
	X(vkFreeMemory)                                                           \
	X(vkGetBufferMemoryRequirements)                                          \
	X(vkGetDeviceQueue)                                                       \
	X(vkGetFenceStatus)                                                       \
	X(vkGetImageMemoryRequirements)                                           \
	X(vkGetPipelineCacheData)                                                 \
	X(vkGetQueryPoolResults)                                                  \
	X(vkGetSwapchainImagesKHR)                                                \
	X(vkMapMemory)                                                            \
	X(vkQueuePresentKHR)                                                      \
	X(vkQueueSubmit)                                                          \
	X(vkQueueWaitIdle)                                                        \
	X(vkResetCommandBuffer)                                                   \
	X(vkResetFences)                                                          \
	X(vkUnmapMemory)                                                          \
	X(vkUpdateDescriptorSets)                                                 \
	X(vkWaitForFences)

/* Before the redirects: __typeof__(f) must see the function, not them. */
#define X_DECLARE(f) extern __typeof__(f) *fnl_##f;
extern __typeof__(vkGetInstanceProcAddr) *fnl_vkGetInstanceProcAddr;
VK_GLOBAL_FNS(X_DECLARE)
VK_INSTANCE_FNS(X_DECLARE)
#undef X_DECLARE

/* The redirects: vkGetInstanceProcAddr, then both lists in their order. */
#define vkGetInstanceProcAddr (*fnl_vkGetInstanceProcAddr)
#define vkCreateInstance (*fnl_vkCreateInstance)
#define vkEnumerateInstanceExtensionProperties \
	(*fnl_vkEnumerateInstanceExtensionProperties)
#define vkDestroyInstance (*fnl_vkDestroyInstance)
#define vkEnumeratePhysicalDevices (*fnl_vkEnumeratePhysicalDevices)
#define vkGetPhysicalDeviceProperties (*fnl_vkGetPhysicalDeviceProperties)
#define vkGetPhysicalDeviceFeatures (*fnl_vkGetPhysicalDeviceFeatures)
#define vkGetPhysicalDeviceFormatProperties \
	(*fnl_vkGetPhysicalDeviceFormatProperties)
#define vkGetPhysicalDeviceMemoryProperties \
	(*fnl_vkGetPhysicalDeviceMemoryProperties)
#define vkGetPhysicalDeviceQueueFamilyProperties \
	(*fnl_vkGetPhysicalDeviceQueueFamilyProperties)
#define vkEnumerateDeviceExtensionProperties \
	(*fnl_vkEnumerateDeviceExtensionProperties)
#define vkCreateDevice (*fnl_vkCreateDevice)
#define vkGetDeviceProcAddr (*fnl_vkGetDeviceProcAddr)
#define vkDestroySurfaceKHR (*fnl_vkDestroySurfaceKHR)
#define vkGetPhysicalDeviceSurfaceSupportKHR \
	(*fnl_vkGetPhysicalDeviceSurfaceSupportKHR)
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR \
	(*fnl_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
#define vkGetPhysicalDeviceSurfaceFormatsKHR \
	(*fnl_vkGetPhysicalDeviceSurfaceFormatsKHR)
#define vkGetPhysicalDeviceSurfacePresentModesKHR \
	(*fnl_vkGetPhysicalDeviceSurfacePresentModesKHR)
#define vkAcquireNextImageKHR (*fnl_vkAcquireNextImageKHR)
#define vkAllocateCommandBuffers (*fnl_vkAllocateCommandBuffers)
#define vkAllocateDescriptorSets (*fnl_vkAllocateDescriptorSets)
#define vkAllocateMemory (*fnl_vkAllocateMemory)
#define vkBeginCommandBuffer (*fnl_vkBeginCommandBuffer)
#define vkBindBufferMemory (*fnl_vkBindBufferMemory)
#define vkBindImageMemory (*fnl_vkBindImageMemory)
#define vkCmdBeginRenderPass (*fnl_vkCmdBeginRenderPass)
#define vkCmdBindDescriptorSets (*fnl_vkCmdBindDescriptorSets)
#define vkCmdBindPipeline (*fnl_vkCmdBindPipeline)
#define vkCmdBindVertexBuffers (*fnl_vkCmdBindVertexBuffers)
#define vkCmdClearAttachments (*fnl_vkCmdClearAttachments)
#define vkCmdCopyBufferToImage (*fnl_vkCmdCopyBufferToImage)
#define vkCmdCopyImageToBuffer (*fnl_vkCmdCopyImageToBuffer)
#define vkCmdDraw (*fnl_vkCmdDraw)
#define vkCmdEndRenderPass (*fnl_vkCmdEndRenderPass)
#define vkCmdPipelineBarrier (*fnl_vkCmdPipelineBarrier)
#define vkCmdPushConstants (*fnl_vkCmdPushConstants)
#define vkCmdResetQueryPool (*fnl_vkCmdResetQueryPool)
#define vkCmdSetBlendConstants (*fnl_vkCmdSetBlendConstants)
#define vkCmdSetScissor (*fnl_vkCmdSetScissor)
#define vkCmdSetViewport (*fnl_vkCmdSetViewport)
#define vkCmdWriteTimestamp (*fnl_vkCmdWriteTimestamp)
#define vkCreateBuffer (*fnl_vkCreateBuffer)
#define vkCreateCommandPool (*fnl_vkCreateCommandPool)
#define vkCreateDescriptorPool (*fnl_vkCreateDescriptorPool)
#define vkCreateDescriptorSetLayout (*fnl_vkCreateDescriptorSetLayout)
#define vkCreateFence (*fnl_vkCreateFence)
#define vkCreateFramebuffer (*fnl_vkCreateFramebuffer)
#define vkCreateGraphicsPipelines (*fnl_vkCreateGraphicsPipelines)
#define vkCreateImage (*fnl_vkCreateImage)
#define vkCreateImageView (*fnl_vkCreateImageView)
#define vkCreatePipelineCache (*fnl_vkCreatePipelineCache)
#define vkCreatePipelineLayout (*fnl_vkCreatePipelineLayout)
#define vkCreateQueryPool (*fnl_vkCreateQueryPool)
#define vkCreateRenderPass (*fnl_vkCreateRenderPass)
#define vkCreateSampler (*fnl_vkCreateSampler)
#define vkCreateSemaphore (*fnl_vkCreateSemaphore)
#define vkCreateShaderModule (*fnl_vkCreateShaderModule)
#define vkCreateSwapchainKHR (*fnl_vkCreateSwapchainKHR)
#define vkDestroyBuffer (*fnl_vkDestroyBuffer)
#define vkDestroyCommandPool (*fnl_vkDestroyCommandPool)
#define vkDestroyDescriptorPool (*fnl_vkDestroyDescriptorPool)
#define vkDestroyDescriptorSetLayout (*fnl_vkDestroyDescriptorSetLayout)
#define vkDestroyDevice (*fnl_vkDestroyDevice)
#define vkDestroyFence (*fnl_vkDestroyFence)
#define vkDestroyFramebuffer (*fnl_vkDestroyFramebuffer)
#define vkDestroyImage (*fnl_vkDestroyImage)
#define vkDestroyImageView (*fnl_vkDestroyImageView)
#define vkDestroyPipeline (*fnl_vkDestroyPipeline)
#define vkDestroyPipelineCache (*fnl_vkDestroyPipelineCache)
#define vkDestroyPipelineLayout (*fnl_vkDestroyPipelineLayout)
#define vkDestroyQueryPool (*fnl_vkDestroyQueryPool)
#define vkDestroyRenderPass (*fnl_vkDestroyRenderPass)
#define vkDestroySampler (*fnl_vkDestroySampler)
#define vkDestroySemaphore (*fnl_vkDestroySemaphore)
#define vkDestroyShaderModule (*fnl_vkDestroyShaderModule)
#define vkDestroySwapchainKHR (*fnl_vkDestroySwapchainKHR)
#define vkDeviceWaitIdle (*fnl_vkDeviceWaitIdle)
#define vkEndCommandBuffer (*fnl_vkEndCommandBuffer)
#define vkFreeCommandBuffers (*fnl_vkFreeCommandBuffers)
#define vkFreeMemory (*fnl_vkFreeMemory)
#define vkGetBufferMemoryRequirements (*fnl_vkGetBufferMemoryRequirements)
#define vkGetDeviceQueue (*fnl_vkGetDeviceQueue)
#define vkGetFenceStatus (*fnl_vkGetFenceStatus)
#define vkGetImageMemoryRequirements (*fnl_vkGetImageMemoryRequirements)
#define vkGetPipelineCacheData (*fnl_vkGetPipelineCacheData)
#define vkGetQueryPoolResults (*fnl_vkGetQueryPoolResults)
#define vkGetSwapchainImagesKHR (*fnl_vkGetSwapchainImagesKHR)
#define vkMapMemory (*fnl_vkMapMemory)
#define vkQueuePresentKHR (*fnl_vkQueuePresentKHR)
#define vkQueueSubmit (*fnl_vkQueueSubmit)
#define vkQueueWaitIdle (*fnl_vkQueueWaitIdle)
#define vkResetCommandBuffer (*fnl_vkResetCommandBuffer)
#define vkResetFences (*fnl_vkResetFences)
#define vkUnmapMemory (*fnl_vkUnmapMemory)
#define vkUpdateDescriptorSets (*fnl_vkUpdateDescriptorSets)
#define vkWaitForFences (*fnl_vkWaitForFences)

#endif /* FNL_VK_FNS_H */
