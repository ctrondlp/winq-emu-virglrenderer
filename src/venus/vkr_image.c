/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include "vkr_image.h"

#include "vkr_image_gen.h"
#include "vkr_device.h"
#include "vkr_physical_device.h"
#include "vkr_device_memory.h"
#ifdef _WIN32
#include "vkr_win32_shim.h"
#endif

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

static void
vkr_dispatch_vkCreateImage(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateImage *args)
{
   /* XXX If VkExternalMemoryImageCreateInfo is chained by the app, all is
    * good.  If it is not chained, we might still bind an external memory to
    * the image, because vkr_dispatch_vkAllocateMemory makes any HOST_VISIBLE
    * memory external.  That is a spec violation.
    *
    * The discussions in vkr_dispatch_vkCreateBuffer are applicable to both
    * buffers and images.  Additionally, drivers usually use
    * VkExternalMemoryImageCreateInfo to pick a well-defined image layout for
    * interoperability with foreign queues.  However, a well-defined layout
    * might not exist for some images.  When it does, it might still require a
    * dedicated allocation or might have a degraded performance.
    *
    * On the other hand, binding an external memory to an image created
    * without VkExternalMemoryImageCreateInfo usually works.  Yes, it will
    * explode if the external memory is accessed by foreign queues due to the
    * lack of a well-defined image layout.  But we never end up in that
    * situation because the app does not consider the memory external.
    */

#ifdef _WIN32
   /*
    * Strip VK_EXT_image_drm_format_modifier pNext structs before the host
    * ICD sees them: the Windows ICD doesn't understand modifiers. Coerce
    * tiling=LINEAR so the layout matches what we'll report back through
    * vkGetImageDrmFormatModifierPropertiesEXT. LINEAR is the only modifier
    * we synthesize.
    */
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   bool shim_linear = false;
   if (dev->physical_device->dma_buf_shim_active) {
      VkImageCreateInfo *info = (VkImageCreateInfo *)args->pCreateInfo;
      VkBaseInStructure *prev = (VkBaseInStructure *)info;
      while (prev->pNext) {
         VkStructureType t = prev->pNext->sType;
         if (t == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT ||
             t == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT) {
            prev->pNext = prev->pNext->pNext;
            shim_linear = true;
         } else {
            prev = (VkBaseInStructure *)prev->pNext;
         }
      }
      if (shim_linear) {
         /* Guest set tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT along
          * with the stripped modifier pNext. Without the pNext that tiling
          * value is invalid. We honor DRM_FORMAT_MOD_LINEAR by coercing to
          * TILING_LINEAR so the host ICD allocates genuinely row-major
          * memory. Cross-process dma-buf consumers (Wayland compositors,
          * Chromium's VaapiVideoDecoder GL import) need the backing pixels
          * to match what they'll interpret as LINEAR — any other tiling
          * produces garbage output even if vkCreateImage itself succeeds. */
         info->tiling = VK_IMAGE_TILING_LINEAR;
      }
   }
#endif

   const bool linear_tiling = args->pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR;
   if (linear_tiling) {
      VkImageCreateInfo *info = (VkImageCreateInfo *)args->pCreateInfo;
      info->usage = vkr_linear_image_usage(info->imageType, info->format, info->usage);
   }
   struct vkr_image *img = vkr_image_create_and_add(dispatch->data, args);
   if (img)
      img->linear_tiling = linear_tiling;
#ifdef _WIN32
   {
      const VkImageCreateInfo *ci = args->pCreateInfo;
      winq_dbg("CreateImage ret=%d fmt=%d type=%d %ux%ux%u mip=%u layers=%u samples=%d tiling=%d usage=0x%x flags=0x%x "
               "sharing=%d img=%p",
               (int)args->ret, (int)ci->format, (int)ci->imageType, ci->extent.width, ci->extent.height, ci->extent.depth,
               ci->mipLevels, ci->arrayLayers, (int)ci->samples, (int)ci->tiling, ci->usage, ci->flags,
               (int)ci->sharingMode, (void *)img);
   }
   if (img && shim_linear)
      img->dma_buf_shim_linear = true;
   if (img && linear_tiling && args->pCreateInfo->imageType == VK_IMAGE_TYPE_2D &&
       args->pCreateInfo->mipLevels == 1 && args->pCreateInfo->arrayLayers == 1 &&
       args->pCreateInfo->samples == VK_SAMPLE_COUNT_1_BIT) {
      img->winq_create_info = *args->pCreateInfo;
      img->winq_create_info.pNext = NULL;
      img->winq_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      img->winq_create_info.queueFamilyIndexCount = 0;
      img->winq_create_info.pQueueFamilyIndices = NULL;
      img->winq_create_info_valid = true;
   }
#endif
}

#ifdef _WIN32
/*
 * winq: an image the guest binds to the memory of an OpenGL (vrend) texture.
 *
 * The guest sees such a texture as a dma-buf with the linear modifier, because that is all its OpenGL
 * driver can say, and creates a linear image for it.  The host OpenGL driver however lays the texture
 * out its own way (optimal tiling) and stores B8G8R8A8 textures as R8G8B8A8.  So the host image is
 * replaced, before it is bound and can be used, by one that matches the texture.  The caller does
 * this before it translates the guest's handles, which then gives the new image.
 *
 * This makes the replacement and returns it (and its format, in out_gl_format), or VK_NULL_HANDLE if the
 * image cannot be bound to that memory; the bind must fail then.  Nothing is changed until
 * winq_gl_image_commit puts it in place.  The caller holds winq_gl_bind_lock.
 */
static VkImage
winq_gl_image_create(struct vkr_device *dev, const struct vkr_image *img, const struct vkr_device_memory *mem,
                     VkDeviceSize offset, VkFormat *out_gl_format)
{
   struct vn_device_proc_table *vk = &dev->proc_table;
   const VkDevice device = dev->base.handle.device;

   /* Only what the texture's memory can hold: an unbound, single level, linear 2D image at the start of
    * the memory, with a format and usage the replacement has.  Everything else fails the bind, the host
    * driver is never shown an image that does not fit the memory. */
   if (!img || !mem || !mem->winq_gl || !img->winq_create_info_valid || !img->linear_tiling ||
       img->winq_bound || offset != 0)
      return VK_NULL_HANDLE;

   const VkExternalMemoryImageCreateInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };
   VkImageCreateInfo info = img->winq_create_info;
   info.pNext = &external_info;
   info.tiling = VK_IMAGE_TILING_OPTIMAL;
   info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   VkFormat gl_format = VK_FORMAT_UNDEFINED;
   switch (info.format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
      gl_format = VK_FORMAT_R8G8B8A8_UNORM;
      break;
   case VK_FORMAT_B8G8R8A8_SRGB:
      gl_format = VK_FORMAT_R8G8B8A8_SRGB;
      break;
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      break;
   default:
      return VK_NULL_HANDLE;
   }
   if (gl_format != VK_FORMAT_UNDEFINED)
      info.format = gl_format;
   /* The memory was allocated for an image with exactly these settings (vrend_winq_share.c), and an
    * opaque handle may only be bound to an image created the same way. */
   info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (img->winq_create_info.usage & ~info.usage)
      return VK_NULL_HANDLE;

   VkImage image = VK_NULL_HANDLE;
   vkr_win32_shim_keep_tiling = true;
   const VkResult result = vk->CreateImage(device, &info, NULL, &image);
   vkr_win32_shim_keep_tiling = false;
   VkMemoryRequirements reqs = { 0 };
   if (result == VK_SUCCESS)
      vk->GetImageMemoryRequirements(device, image, &reqs);
   winq_dbg("GLIMAGE %p %ux%u format %d -> %d usage=0x%x (guest 0x%x) ret=%d needs %llu of %llu bytes, types 0x%x (have %u)",
            (void *)img, info.extent.width, info.extent.height, (int)img->winq_create_info.format, (int)info.format,
            info.usage, img->winq_create_info.usage, (int)result, (unsigned long long)reqs.size,
            (unsigned long long)mem->allocation_size, reqs.memoryTypeBits, mem->winq_gl_mem_type);
   if (result != VK_SUCCESS)
      return VK_NULL_HANDLE;
   /* The guest picks the size of its image; the texture's memory has the size of the texture. */
   if (reqs.size > mem->allocation_size || mem->winq_gl_mem_type >= 32 ||
       !(reqs.memoryTypeBits & (1u << mem->winq_gl_mem_type))) {
      vk->DestroyImage(device, image, NULL);
      return VK_NULL_HANDLE;
   }

   *out_gl_format = gl_format;
   return image;
}

/* Puts a replacement made by winq_gl_image_create in place of the image's host image. */
static void
winq_gl_image_commit(struct vkr_device *dev, struct vkr_image *img, VkImage image, VkFormat gl_format)
{
   dev->proc_table.DestroyImage(dev->base.handle.device, img->base.handle.image, NULL);
   img->base.handle.image = image;
   img->linear_tiling = false;
   img->dma_buf_shim_linear = false;
   img->winq_create_info_valid = false;
   img->winq_gl_format = gl_format;
   img->winq_gl_bound = true;
   img->winq_bound = true;
}

/* Held while images are replaced and bound to the memory of OpenGL textures: two rings that bind the
 * same image at once (which the guest must not do) cannot both replace and destroy it then. */
static SRWLOCK winq_gl_bind_lock = SRWLOCK_INIT;
#endif /* _WIN32 */

static void
vkr_dispatch_vkDestroyImage(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyImage *args)
{
   vkr_image_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   const struct vkr_image *linear_img = vkr_image_from_handle(args->image);
   const bool linear = linear_img && linear_img->linear_tiling;

   vn_replace_vkGetImageMemoryRequirements_args_handle(args);
   vk->GetImageMemoryRequirements(args->device, args->image, args->pMemoryRequirements);
   if (args->pMemoryRequirements)
      args->pMemoryRequirements->memoryTypeBits = vkr_physical_device_remap_memory_type_bits(dev->physical_device,
                                                      args->pMemoryRequirements->memoryTypeBits,
                                                      linear);
   /* winq: a linear image is shared as a dma-buf, and the guest's import of it refuses a size that is
    * not the whole pages the buffer behind it has, so report whole pages. */
   if (args->pMemoryRequirements && linear)
      args->pMemoryRequirements->size = (args->pMemoryRequirements->size + 4095) & ~(VkDeviceSize)4095;
}

static void
vkr_dispatch_vkGetImageMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   const struct vkr_image *linear_img =
      args->pInfo ? vkr_image_from_handle(args->pInfo->image) : NULL;
   const bool linear = linear_img && linear_img->linear_tiling;

   vn_replace_vkGetImageMemoryRequirements2_args_handle(args);
   vk->GetImageMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
   if (args->pMemoryRequirements)
      args->pMemoryRequirements->memoryRequirements.memoryTypeBits = vkr_physical_device_remap_memory_type_bits(dev->physical_device,
                                                      args->pMemoryRequirements->memoryRequirements.memoryTypeBits,
                                                      linear);
   /* winq: a linear image is shared as a dma-buf, and the guest's import of it refuses a size that is
    * not the whole pages the buffer behind it has, so report whole pages. */
   if (args->pMemoryRequirements && linear)
      args->pMemoryRequirements->memoryRequirements.size =
         (args->pMemoryRequirements->memoryRequirements.size + 4095) & ~(VkDeviceSize)4095;
   if (args->pMemoryRequirements)
      winq_dbg("IMGREQ size=%llu bits=0x%x linear=%d",
               (unsigned long long)args->pMemoryRequirements->memoryRequirements.size,
               args->pMemoryRequirements->memoryRequirements.memoryTypeBits, (int)linear);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements_args_handle(args);
   vk->GetImageSparseMemoryRequirements(args->device, args->image,
                                        args->pSparseMemoryRequirementCount,
                                        args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements2_args_handle(args);
   vk->GetImageSparseMemoryRequirements2(args->device, args->pInfo,
                                         args->pSparseMemoryRequirementCount,
                                         args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkBindImageMemory(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkBindImageMemory *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
#ifdef _WIN32
   /* winq: see winq_gl_image_create */
   struct vkr_image *winq_img = vkr_image_from_handle(args->image);
   const struct vkr_device_memory *winq_mem = vkr_device_memory_from_handle(args->memory);
   const bool winq_gl = winq_mem && winq_mem->winq_gl;
   if (winq_gl) {
      AcquireSRWLockExclusive(&winq_gl_bind_lock);
      VkFormat gl_format = VK_FORMAT_UNDEFINED;
      const VkImage image = winq_gl_image_create(dev, winq_img, winq_mem, args->memoryOffset, &gl_format);
      if (image == VK_NULL_HANDLE) {
         ReleaseSRWLockExclusive(&winq_gl_bind_lock);
         args->ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         return;
      }
      winq_gl_image_commit(dev, winq_img, image, gl_format);
   }
#endif

   vn_replace_vkBindImageMemory_args_handle(args);
   args->ret =
      vk->BindImageMemory(args->device, args->image, args->memory, args->memoryOffset);
#ifdef _WIN32
   if (winq_img && args->ret == VK_SUCCESS)
      winq_img->winq_bound = true;
   if (winq_gl)
      ReleaseSRWLockExclusive(&winq_gl_bind_lock);
#endif
}

static void
vkr_dispatch_vkBindImageMemory2(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindImageMemory2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
#ifdef _WIN32
   /* winq: see winq_gl_image_create.  Every replacement is made before any is put in place, so that an
    * entry that cannot be bound leaves no image changed. */
   const uint32_t winq_count = args->bindInfoCount;
   struct winq_bind {
      struct vkr_image *img; /* for after the handles are translated */
      VkImage replacement;
      VkFormat gl_format;
   } *winq_binds = winq_count ? calloc(winq_count, sizeof(*winq_binds)) : NULL;
   if (winq_count && !winq_binds) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }
   bool winq_gl = false;
   for (uint32_t i = 0; i < winq_count; i++) {
      const struct vkr_device_memory *winq_mem = vkr_device_memory_from_handle(args->pBindInfos[i].memory);
      winq_binds[i].img = vkr_image_from_handle(args->pBindInfos[i].image);
      winq_gl = winq_gl || (winq_mem && winq_mem->winq_gl);
   }
   if (winq_gl) {
      AcquireSRWLockExclusive(&winq_gl_bind_lock);
      bool ok = true;
      for (uint32_t i = 0; i < winq_count && ok; i++) {
         const struct vkr_device_memory *winq_mem = vkr_device_memory_from_handle(args->pBindInfos[i].memory);
         if (!winq_mem || !winq_mem->winq_gl)
            continue;
         winq_binds[i].replacement = winq_gl_image_create(dev, winq_binds[i].img, winq_mem,
                                                          args->pBindInfos[i].memoryOffset,
                                                          &winq_binds[i].gl_format);
         ok = winq_binds[i].replacement != VK_NULL_HANDLE;
         /* marked at once, so that an image named twice is refused the second time */
         if (ok)
            winq_binds[i].img->winq_bound = true;
      }
      for (uint32_t i = 0; i < winq_count; i++) {
         if (winq_binds[i].replacement == VK_NULL_HANDLE)
            continue;
         if (ok) {
            winq_gl_image_commit(dev, winq_binds[i].img, winq_binds[i].replacement, winq_binds[i].gl_format);
         } else {
            vk->DestroyImage(dev->base.handle.device, winq_binds[i].replacement, NULL);
            winq_binds[i].img->winq_bound = false;
         }
      }
      if (!ok) {
         ReleaseSRWLockExclusive(&winq_gl_bind_lock);
         free(winq_binds);
         args->ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         return;
      }
   }
#endif

   vn_replace_vkBindImageMemory2_args_handle(args);
   args->ret = vk->BindImageMemory2(args->device, args->bindInfoCount, args->pBindInfos);
#ifdef _WIN32
   for (uint32_t i = 0; args->ret == VK_SUCCESS && i < winq_count; i++) {
      if (winq_binds[i].img)
         winq_binds[i].img->winq_bound = true;
   }
   if (winq_gl)
      ReleaseSRWLockExclusive(&winq_gl_bind_lock);
   free(winq_binds);
#endif
}

static void
vkr_dispatch_vkGetImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   /* For shim images (host-side tiling=LINEAR, guest thinks tiling=MODIFIER),
    * translate the guest's MEMORY_PLANE_0_BIT_EXT aspect (valid only for
    * MODIFIER tiling per spec) to COLOR_BIT (what the host's LINEAR image
    * understands). Forward the call to the host so the returned rowPitch is
    * what the ICD actually laid out — any synthesized row pitch we invented
    * would mismatch the real memory and produce garbled pixels in external
    * consumers (Wayland compositors, ANGLE dma-buf import).
    */
   struct vkr_image *img = vkr_image_from_handle(args->image);
   VkImageSubresource shim_sub;
   if (img && img->dma_buf_shim_linear && args->pSubresource) {
      shim_sub = *args->pSubresource;
      if (shim_sub.aspectMask & VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
         shim_sub.aspectMask =
            (shim_sub.aspectMask & ~VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT) |
            VK_IMAGE_ASPECT_COLOR_BIT;
      args->pSubresource = &shim_sub;
   }
#endif

   vn_replace_vkGetImageSubresourceLayout_args_handle(args);
   vk->GetImageSubresourceLayout(args->device, args->image, args->pSubresource,
                                 args->pLayout);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   /* Mirror of the non-2 path: translate MEMORY_PLANE_0_BIT_EXT → COLOR_BIT
    * for shim-LINEAR images so the host ICD can service the query on its
    * actually-LINEAR backing. */
   struct vkr_image *img = vkr_image_from_handle(args->image);
   VkImageSubresource2 shim_sub;
   if (img && img->dma_buf_shim_linear && args->pSubresource) {
      shim_sub = *args->pSubresource;
      if (shim_sub.imageSubresource.aspectMask &
          VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
         shim_sub.imageSubresource.aspectMask =
            (shim_sub.imageSubresource.aspectMask &
             ~VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT) |
            VK_IMAGE_ASPECT_COLOR_BIT;
      args->pSubresource = &shim_sub;
   }
#endif

   vn_replace_vkGetImageSubresourceLayout2_args_handle(args);
   vk->GetImageSubresourceLayout2(args->device, args->image, args->pSubresource,
                                  args->pLayout);
}

static void
vkr_dispatch_vkGetDeviceImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageSubresourceLayout_args_handle(args);
   vk->GetDeviceImageSubresourceLayout(args->device, args->pInfo, args->pLayout);
}

static void
vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageDrmFormatModifierPropertiesEXT *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef _WIN32
   /*
    * The Windows ICD doesn't implement this entry point (extension is
    * synthesized by vkr). Every image created through the shim was forced
    * to LINEAR, so answer LINEAR without calling the host.
    */
   if (dev->physical_device->dma_buf_shim_active) {
      args->pProperties->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
      args->ret = VK_SUCCESS;
      return;
   }
#endif

   vn_replace_vkGetImageDrmFormatModifierPropertiesEXT_args_handle(args);
   args->ret = vk->GetImageDrmFormatModifierPropertiesEXT(args->device, args->image,
                                                          args->pProperties);
}

static void
vkr_dispatch_vkCreateImageView(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateImageView *args)
{
#ifdef _WIN32
   /* winq: the host image may have another channel order than the guest thinks, see
    * winq_gl_image_create; a view with the guest's format would swap red and blue. */
   {
      const struct vkr_image *winq_img = vkr_image_from_handle(args->pCreateInfo->image);
      if (winq_img && winq_img->winq_gl_format != VK_FORMAT_UNDEFINED) {
         VkImageViewCreateInfo *info = (VkImageViewCreateInfo *)args->pCreateInfo;
         if (info->format == VK_FORMAT_B8G8R8A8_UNORM)
            info->format = VK_FORMAT_R8G8B8A8_UNORM;
         else if (info->format == VK_FORMAT_B8G8R8A8_SRGB)
            info->format = VK_FORMAT_R8G8B8A8_SRGB;
      }
   }
   if (winq_dbg_cmds()) {
      const VkImageViewCreateInfo *vi = args->pCreateInfo;
      const struct vkr_image *vimg = vkr_image_from_handle(vi->image);
      winq_dbg("  VIEW image=%p (gl format %d, linear %d) type %d format %d comps %d%d%d%d aspect 0x%x mips %u+%u layers %u+%u flags 0x%x pnext %d",
               (void *)(uintptr_t)vi->image, vimg ? (int)vimg->winq_gl_format : -1, vimg ? (int)vimg->linear_tiling : -1,
               (int)vi->viewType, (int)vi->format, (int)vi->components.r, (int)vi->components.g, (int)vi->components.b,
               (int)vi->components.a, vi->subresourceRange.aspectMask, vi->subresourceRange.baseMipLevel,
               vi->subresourceRange.levelCount, vi->subresourceRange.baseArrayLayer, vi->subresourceRange.layerCount,
               (unsigned)vi->flags, vi->pNext ? (int)((const VkBaseInStructure *)vi->pNext)->sType : 0);
   }
#endif
   vkr_image_view_create_and_add(dispatch->data, args);
   if (winq_dbg_cmds())
      winq_dbg("  VIEW -> %p ret %d", args->pView ? (void *)(uintptr_t)*args->pView : NULL, (int)args->ret);
   if (args->ret != VK_SUCCESS)
      winq_dbg("FAIL CreateImageView ret=%d", (int)args->ret);
}

static void
vkr_dispatch_vkDestroyImageView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyImageView *args)
{
   vkr_image_view_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSampler(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCreateSampler *args)
{
   vkr_sampler_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySampler(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkDestroySampler *args)
{
   vkr_sampler_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateSamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroySamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   const bool linear = args->pInfo && args->pInfo->pCreateInfo &&
                       (args->pInfo->pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR ||
                        args->pInfo->pCreateInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
   /* winq: ask about the image vkCreateImage would create, see vkr_linear_image_usage */
   if (args->pInfo && args->pInfo->pCreateInfo && args->pInfo->pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR) {
      VkImageCreateInfo *info = (VkImageCreateInfo *)args->pInfo->pCreateInfo;
      info->usage = vkr_linear_image_usage(info->imageType, info->format, info->usage);
   }

   vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(args);
   vk->GetDeviceImageMemoryRequirements(args->device, args->pInfo,
                                        args->pMemoryRequirements);
   if (args->pMemoryRequirements)
      args->pMemoryRequirements->memoryRequirements.memoryTypeBits = vkr_physical_device_remap_memory_type_bits(dev->physical_device,
                                                      args->pMemoryRequirements->memoryRequirements.memoryTypeBits,
                                                      linear);
   /* winq: a linear image is shared as a dma-buf, and the guest's import of it refuses a size that is
    * not the whole pages the buffer behind it has, so report whole pages. */
   if (args->pMemoryRequirements && linear)
      args->pMemoryRequirements->memoryRequirements.size =
         (args->pMemoryRequirements->memoryRequirements.size + 4095) & ~(VkDeviceSize)4095;
}

static void
vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageSparseMemoryRequirements_args_handle(args);
   vk->GetDeviceImageSparseMemoryRequirements(args->device, args->pInfo,
                                              args->pSparseMemoryRequirementCount,
                                              args->pSparseMemoryRequirements);
}

void
vkr_context_init_image_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImage = vkr_dispatch_vkCreateImage;
   dispatch->dispatch_vkDestroyImage = vkr_dispatch_vkDestroyImage;
   dispatch->dispatch_vkGetImageMemoryRequirements =
      vkr_dispatch_vkGetImageMemoryRequirements;
   dispatch->dispatch_vkGetImageMemoryRequirements2 =
      vkr_dispatch_vkGetImageMemoryRequirements2;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements =
      vkr_dispatch_vkGetImageSparseMemoryRequirements;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements2 =
      vkr_dispatch_vkGetImageSparseMemoryRequirements2;
   dispatch->dispatch_vkBindImageMemory = vkr_dispatch_vkBindImageMemory;
   dispatch->dispatch_vkBindImageMemory2 = vkr_dispatch_vkBindImageMemory2;
   dispatch->dispatch_vkGetImageSubresourceLayout =
      vkr_dispatch_vkGetImageSubresourceLayout;
   dispatch->dispatch_vkGetImageSubresourceLayout2 =
      vkr_dispatch_vkGetImageSubresourceLayout2;
   dispatch->dispatch_vkGetDeviceImageSubresourceLayout =
      vkr_dispatch_vkGetDeviceImageSubresourceLayout;

   dispatch->dispatch_vkGetImageDrmFormatModifierPropertiesEXT =
      vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT;
   dispatch->dispatch_vkGetDeviceImageMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageMemoryRequirements;
   dispatch->dispatch_vkGetDeviceImageSparseMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements;
}

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImageView = vkr_dispatch_vkCreateImageView;
   dispatch->dispatch_vkDestroyImageView = vkr_dispatch_vkDestroyImageView;
}

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSampler = vkr_dispatch_vkCreateSampler;
   dispatch->dispatch_vkDestroySampler = vkr_dispatch_vkDestroySampler;
}

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSamplerYcbcrConversion =
      vkr_dispatch_vkCreateSamplerYcbcrConversion;
   dispatch->dispatch_vkDestroySamplerYcbcrConversion =
      vkr_dispatch_vkDestroySamplerYcbcrConversion;
}
