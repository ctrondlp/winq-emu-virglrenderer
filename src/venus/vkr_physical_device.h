/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_PHYSICAL_DEVICE_H
#define VKR_PHYSICAL_DEVICE_H

#include "vkr_common.h"

#include "venus-protocol/vn_protocol_renderer_util.h"

/* winq: the row pitch the host driver gives a LINEAR image depends on the usage (the color attachment
 * bit asks for a bigger alignment), but a guest that imports a buffer another process exported creates
 * its image with its own usage and expects the exporter's pitch (the host cannot take an explicit
 * layout).  Colour attachment is therefore always included for the usual colour formats, so every
 * linear image of one size has the same pitch. */
static inline bool
vkr_linear_pitch_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return true;
   default:
      return false;
   }
}

/* winq: the usage the host gets for a LINEAR image the guest creates or asks about.  The host cannot
 * create LINEAR images with the attachment feedback loop, input attachment or host transfer usage, and a
 * guest that asks for one of them (Zink adds the feedback loop bit to every render target) would then be
 * refused a linear image it can share, so these bits are dropped; colour attachment is added as
 * vkr_linear_pitch_format says.  vkCreateImage, vkGetDeviceImageMemoryRequirements and
 * vkGetPhysicalDeviceImageFormatProperties2 must all use it, or the guest is told about another image
 * than the one that is created. */
static inline VkImageUsageFlags
vkr_linear_image_usage(VkImageType type, VkFormat format, VkImageUsageFlags usage)
{
   usage &= ~(VkImageUsageFlags)(VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
                                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT);
   if (type == VK_IMAGE_TYPE_2D && vkr_linear_pitch_format(format))
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   return usage;
}

struct vkr_physical_device {
   struct vkr_object base;

   struct vn_physical_device_proc_table proc_table;

   VkPhysicalDeviceProperties properties;
   uint32_t api_version;

   VkExtensionProperties *extensions;
   uint32_t extension_count;

   bool KHR_external_memory_fd;
   bool EXT_external_memory_dma_buf;

   bool KHR_external_fence_fd;
   bool KHR_external_semaphore_fd;

   bool host_external_memory_win32;
   /* winq: the host has VK_NV_linear_color_attachment, so LINEAR images can be color attachments */
   bool host_linear_color_attachment;
   bool host_external_fence_win32;
   bool host_external_semaphore_win32;

   /* Windows-host synthesis of Linux dma-buf + modifier extensions.
    * When true, vkr advertises VK_EXT_external_memory_dma_buf and
    * VK_EXT_image_drm_format_modifier to the guest and internally
    * translates them to OPAQUE_WIN32 + LINEAR tiling on the host ICD.
    *
    * This unblocks guest stacks that require these extensions to
    * function: ANGLE's Vulkan backend for Chromium's VaapiVideoDecoder,
    * Zink's dma-buf export for Wayland presentation, and any Mesa
    * consumer that checks for dma-buf Vulkan interop before taking
    * the accelerated path.
    */
   bool dma_buf_shim_active;
   /* True when the extension was advertised by vkr but not by the host
    * ICD. CreateDevice must NOT pass these names to the host ICD.
    */
   bool EXT_external_memory_dma_buf_synthesized;
   bool EXT_image_drm_format_modifier_synthesized;

   VkPhysicalDeviceMemoryProperties memory_properties;
   /* winq: host memory type index that backs each guest visible memory type (identity unless a
    * device local + host visible type is backed by system memory, see init_memory_properties). */
   uint32_t memory_type_redirect[VK_MAX_MEMORY_TYPES];
   VkPhysicalDeviceIDProperties id_properties;
   bool is_dma_buf_fd_export_supported;
   bool is_opaque_fd_export_supported;
   void *gbm_device;
   int udmabuf_dev_fd;

   VkQueueFamilyProperties *queue_family_properties;
   uint32_t queue_family_property_count;

   struct list_head devices;
};
VKR_DEFINE_OBJECT_CAST(physical_device, VK_OBJECT_TYPE_PHYSICAL_DEVICE, VkPhysicalDevice)

void
vkr_context_init_physical_device_dispatch(struct vkr_context *ctx);

void
vkr_physical_device_destroy(struct vkr_context *ctx,
                            struct vkr_physical_device *physical_dev);

/* winq: a guest memory type whose allocations are backed by another host memory type (see
 * vkr_physical_device_init_memory_properties) is allowed for a resource exactly when the host
 * allows the backing type for it.  Other types are left alone.
 *
 * With linear set (a VK_IMAGE_TILING_LINEAR image: the guest's scanout and export images) the
 * device local types that are not host visible are dropped when such a backed type is allowed, so
 * the guest picks the backed type: its memory is host visible system memory, which the device can
 * map and read to show the image on the display.  Device local memory cannot be read by the host. */
static inline uint32_t
vkr_physical_device_remap_memory_type_bits(const struct vkr_physical_device *physical_dev,
                                           uint32_t bits, bool linear)
{
   bool backed = false;
   for (uint32_t i = 0; i < physical_dev->memory_properties.memoryTypeCount; i++) {
      const uint32_t host = physical_dev->memory_type_redirect[i];
      if (host == i)
         continue;
      if (bits & (1u << host)) {
         bits |= 1u << i;
         backed = true;
      } else {
         bits &= ~(1u << i);
      }
   }
   if (linear && backed) {
      for (uint32_t i = 0; i < physical_dev->memory_properties.memoryTypeCount; i++) {
         const VkMemoryPropertyFlags f = physical_dev->memory_properties.memoryTypes[i].propertyFlags;
         if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            bits &= ~(1u << i);
      }
   }
   return bits;
}

#endif /* VKR_PHYSICAL_DEVICE_H */
