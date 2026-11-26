/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;

struct vkr_device_memory {
   struct vkr_object base;

   struct vkr_device *device;

   bool might_export;

   uint32_t property_flags;
   uint32_t valid_fd_types;

   /* gbm bo backing non-external mappable memory */
   struct gbm_bo *gbm_bo;

   /* udmabuf backing non-external mappable memory */
   int udmabuf_fd;

   uint64_t allocation_size;
   uint32_t memory_type_index;

   bool exported;

   /* allocation_size is counted against the host memory limit */
   bool counted;

   /* winq: imported from an OpenGL (vrend) texture, see vkr_resource::winq_gl; winq_gl_mem_type is the
    * host memory type the texture's memory was allocated with */
   bool winq_gl;
   uint32_t winq_gl_mem_type;
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

void
vkr_device_memory_release(struct vkr_device_memory *mem);

/* Limit for the memory all guests together can have allocated through
 * vkAllocateMemory (not counting imports of memory that already exists), 0 for
 * no limit.  An allocation that would go over it fails with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY, which is what an application expects when the
 * card is full.  vkr_device_memory_used() is what is counted now. */
void
vkr_device_memory_set_limit(uint64_t limit);

uint64_t
vkr_device_memory_used(void);

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob);

#ifdef _WIN32
/* winq: memory a Venus context exported for sharing (for example a swapchain image that gamescope
 * shows), by resource id: the host memory type and allocation size it was allocated with and the LUID
 * of the adapter it is on.  Another context imports it through its opaque Win32 handle, and such an
 * import must use the same type and size.  Set by the exporting context, removed when its resource is
 * freed, read by any context (all run in this process, on their own threads).  handle is the exporting
 * resource's handle; a lookup only finds the entry with a handle to the same memory. */
bool
vkr_winq_export_registry_set(uint32_t res_id, uint32_t mem_type, uint64_t size, const uint8_t *luid, void *handle);

bool
vkr_winq_export_registry_get(uint32_t res_id, void *handle, uint32_t *out_mem_type, uint64_t *out_size,
                             uint8_t *out_luid);

void
vkr_winq_export_registry_clear(uint32_t res_id);
#endif

#endif /* VKR_DEVICE_MEMORY_H */
