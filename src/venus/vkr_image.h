/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_IMAGE_H
#define VKR_IMAGE_H

#include "vkr_common.h"

struct vkr_image {
   struct vkr_object base;

   /* winq: the host image has VK_IMAGE_TILING_LINEAR (after the modifier shim, if any ran).  The
    * guest creates its scanout and export images like this; vkr_physical_device_remap_memory_type_bits
    * steers them to host visible memory so the device can read their pixels. */
   bool linear_tiling;

#ifdef _WIN32
   /* Set when the image was created through the VK_EXT_image_drm_format_modifier
    * shim: the guest asked for tiling=DRM_FORMAT_MODIFIER_EXT with a
    * modifier pNext chain; the shim rewrote that to tiling=LINEAR before
    * forwarding to the host ICD. Used later to translate aspect-mask
    * values in vkGetImageSubresourceLayout(2) — the guest queries
    * VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT (valid only for MODIFIER
    * tiling), but on the host we need VK_IMAGE_ASPECT_COLOR_BIT (the
    * aspect valid for LINEAR). */
   bool dma_buf_shim_linear;

   /* winq: what a linear image was created with, kept in case it is bound to the memory of an OpenGL
    * texture: the host image is then replaced by one with optimal tiling (see winq_gl_image_create in vkr_image.c).
    * winq_gl_format is the format of the replacement if it differs from what the guest asked for,
    * else VK_FORMAT_UNDEFINED. */
   VkImageCreateInfo winq_create_info;
   bool winq_create_info_valid;
   VkFormat winq_gl_format;
   /* winq: the image is bound to the memory of an OpenGL texture */
   bool winq_gl_bound;
   /* winq: the image is bound to memory (of any kind), it can no longer be replaced */
   bool winq_bound;
#endif
};
VKR_DEFINE_OBJECT_CAST(image, VK_OBJECT_TYPE_IMAGE, VkImage)

struct vkr_image_view {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(image_view, VK_OBJECT_TYPE_IMAGE_VIEW, VkImageView)

struct vkr_sampler {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(sampler, VK_OBJECT_TYPE_SAMPLER, VkSampler)

struct vkr_sampler_ycbcr_conversion {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(sampler_ycbcr_conversion,
                       VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
                       VkSamplerYcbcrConversion)

void
vkr_context_init_image_dispatch(struct vkr_context *ctx);

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx);

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx);

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx);

#endif /* VKR_IMAGE_H */
