/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device_memory.h"

#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
static inline int getpagesize(void) {
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   return (int)si.dwPageSize;
}
#endif

#include "venus-protocol/vn_protocol_renderer_transport.h"

#include "util/os_file.h"

#include "vkr_device_memory_gen.h"
#include "vkr_physical_device.h"

#include "vrend/vrend_winq_share.h"

/* What all contexts together have allocated with vkAllocateMemory and the
 * limit for it (0: none), see vkr_device_memory_set_limit.  The renderer
 * library lives in one VM process, so this is per VM. */
static atomic_uint_fast64_t vkr_device_memory_limit = 0;
static atomic_uint_fast64_t vkr_device_memory_total = 0;

void
vkr_device_memory_set_limit(uint64_t limit)
{
   atomic_store(&vkr_device_memory_limit, limit);
}

uint64_t
vkr_device_memory_used(void)
{
   return atomic_load(&vkr_device_memory_total);
}

/* Adds size to the total unless that goes over the limit. */
static bool
vkr_device_memory_reserve(uint64_t size)
{
   const uint64_t limit = atomic_load(&vkr_device_memory_limit);
   uint64_t total = atomic_load(&vkr_device_memory_total);
   do {
      if (limit && (size > limit || total > limit - size))
         return false;
   } while (!atomic_compare_exchange_weak(&vkr_device_memory_total, &total, total + size));
   return true;
}

static void
vkr_device_memory_unreserve(uint64_t size)
{
   atomic_fetch_sub(&vkr_device_memory_total, size);
}

#ifdef _WIN32
/* winq: see vkr_device_memory.h.  Open addressing by resource id, like the OpenGL share registry in
 * vrend_winq_share.c; a full table records nothing more, and an import then keeps the guest's type. */
#define VKR_WINQ_EXPORT_REGISTRY_SIZE 4096 /* a power of two */
#define VKR_WINQ_EXPORT_DELETED UINT32_MAX
static struct {
   uint32_t res_id; /* 0: never used, VKR_WINQ_EXPORT_DELETED: free again */
   uint32_t mem_type;
   uint64_t size;
   uint8_t luid[VK_LUID_SIZE];
   /* the exporting resource's handle (closed after its entry is cleared): an import only matches the
    * entry with a handle to the same memory, as the guest can reuse the resource id meanwhile */
   HANDLE handle;
} vkr_winq_export_registry[VKR_WINQ_EXPORT_REGISTRY_SIZE];
static SRWLOCK vkr_winq_export_registry_lock = SRWLOCK_INIT;

/* Returns the slot of res_id, or -1.  The caller holds the lock. */
static int
vkr_winq_export_registry_find(uint32_t res_id)
{
   uint32_t idx = res_id & (VKR_WINQ_EXPORT_REGISTRY_SIZE - 1);
   for (uint32_t probes = 0;
        vkr_winq_export_registry[idx].res_id && probes < VKR_WINQ_EXPORT_REGISTRY_SIZE; probes++) {
      if (vkr_winq_export_registry[idx].res_id == res_id)
         return (int)idx;
      idx = (idx + 1) & (VKR_WINQ_EXPORT_REGISTRY_SIZE - 1);
   }
   return -1;
}

bool
vkr_winq_export_registry_set(uint32_t res_id, uint32_t mem_type, uint64_t size, const uint8_t *luid, void *handle)
{
   if (!res_id || res_id == VKR_WINQ_EXPORT_DELETED || !luid || !handle || handle == INVALID_HANDLE_VALUE)
      return false;
   AcquireSRWLockExclusive(&vkr_winq_export_registry_lock);
   int slot = vkr_winq_export_registry_find(res_id);
   if (slot < 0) {
      uint32_t idx = res_id & (VKR_WINQ_EXPORT_REGISTRY_SIZE - 1);
      for (uint32_t probes = 0; probes < VKR_WINQ_EXPORT_REGISTRY_SIZE; probes++) {
         if (!vkr_winq_export_registry[idx].res_id ||
             vkr_winq_export_registry[idx].res_id == VKR_WINQ_EXPORT_DELETED) {
            slot = (int)idx;
            break;
         }
         idx = (idx + 1) & (VKR_WINQ_EXPORT_REGISTRY_SIZE - 1);
      }
   }
   if (slot >= 0) {
      vkr_winq_export_registry[slot].res_id = res_id;
      vkr_winq_export_registry[slot].mem_type = mem_type;
      vkr_winq_export_registry[slot].size = size;
      memcpy(vkr_winq_export_registry[slot].luid, luid, VK_LUID_SIZE);
      vkr_winq_export_registry[slot].handle = (HANDLE)handle;
   }
   ReleaseSRWLockExclusive(&vkr_winq_export_registry_lock);
   return slot >= 0;
}

bool
vkr_winq_export_registry_get(uint32_t res_id, void *handle, uint32_t *out_mem_type, uint64_t *out_size,
                             uint8_t *out_luid)
{
   bool found = false;
   if (!res_id || res_id == VKR_WINQ_EXPORT_DELETED)
      return false;
   AcquireSRWLockShared(&vkr_winq_export_registry_lock);
   const int slot = vkr_winq_export_registry_find(res_id);
   if (slot >= 0 && vrend_winq_share_same_object(vkr_winq_export_registry[slot].handle, handle)) {
      *out_mem_type = vkr_winq_export_registry[slot].mem_type;
      *out_size = vkr_winq_export_registry[slot].size;
      memcpy(out_luid, vkr_winq_export_registry[slot].luid, VK_LUID_SIZE);
      found = true;
   }
   ReleaseSRWLockShared(&vkr_winq_export_registry_lock);
   return found;
}

void
vkr_winq_export_registry_clear(uint32_t res_id)
{
   if (!res_id || res_id == VKR_WINQ_EXPORT_DELETED)
      return;
   AcquireSRWLockExclusive(&vkr_winq_export_registry_lock);
   const int slot = vkr_winq_export_registry_find(res_id);
   if (slot >= 0)
      vkr_winq_export_registry[slot].res_id = VKR_WINQ_EXPORT_DELETED;
   ReleaseSRWLockExclusive(&vkr_winq_export_registry_lock);
}
#endif /* _WIN32 */

static bool
vkr_get_fd_info_from_resource_info(struct vkr_context *ctx,
                                   struct vkr_device *dev,
                                   const VkImportMemoryResourceInfoMESA *res_info,
                                   VkImportMemoryFdInfoKHR *out_fd,
#ifdef _WIN32
                                   VkImportMemoryWin32HandleInfoKHR *out_win32,
#endif
                                   const void **out_import_info)
{
   struct vkr_resource *res = vkr_context_get_resource(ctx, res_info->resourceId);
   if (!res) {
      vkr_log("failed to import resource: invalid res_id %u", res_info->resourceId);
      vkr_context_set_fatal(ctx);
      return false;
   }

   VkExternalMemoryHandleTypeFlagBits handle_type;
   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
#ifdef _WIN32
      /*
       * The dma-buf shim stores Win32 HANDLEs wrapped as fds in "dma-buf"
       * resources. Unwrap and import as OPAQUE_WIN32 — the only handle
       * type the Windows ICD understands — while keeping the guest's
       * DMA_BUF viewpoint on its side of the wire.
       */
      if (dev->physical_device->dma_buf_shim_active) {
         HANDLE handle = os_get_win32_handle_from_fd(res->u.fd);
         HANDLE dup_handle = NULL;
         if (handle == INVALID_HANDLE_VALUE ||
             !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup_handle,
                              0, FALSE, DUPLICATE_SAME_ACCESS)) {
            winq_dbg("ImportFail res=%u fd_type=%d handle=%p err=%lu", res_info->resourceId, (int)res->fd_type,
                     (void *)handle, (unsigned long)GetLastError());
            return false;
         }

         *out_win32 = (VkImportMemoryWin32HandleInfoKHR){
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
            .pNext = res_info->pNext,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
            .handle = dup_handle,
            .name = NULL,
         };
         *out_import_info = out_win32;
         return true;
      }
#endif
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
#ifdef _WIN32
      if (dev->physical_device->host_external_memory_win32) {
         HANDLE handle = os_get_win32_handle_from_fd(res->u.fd);
         HANDLE dup_handle = NULL;
         if (handle == INVALID_HANDLE_VALUE ||
             !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup_handle,
                              0, FALSE, DUPLICATE_SAME_ACCESS)) {
            winq_dbg("ImportFail res=%u fd_type=%d handle=%p err=%lu", res_info->resourceId, (int)res->fd_type,
                     (void *)handle, (unsigned long)GetLastError());
            return false;
         }

         *out_win32 = (VkImportMemoryWin32HandleInfoKHR){
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
            .pNext = res_info->pNext,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
            .handle = dup_handle,
            .name = NULL,
         };
         *out_import_info = out_win32;
         return true;
      }
#endif
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      break;
   default:
      return false;
   }

   int fd = os_dupfd_cloexec(res->u.fd);
   if (fd < 0)
      return false;

   *out_fd = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = res_info->pNext,
      .fd = fd,
      .handleType = handle_type,
   };
   *out_import_info = out_fd;
   return true;
}

#if defined(HAVE_LINUX_UDMABUF_H) && defined(HAVE_MEMFD_CREATE)
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                             const VkMemoryAllocateInfo *alloc_info,
                                             int *out_udmabuf_fd,
                                             VkImportMemoryFdInfoKHR *out_fd_info)
{
   int memfd = -1;
   int udmabuf_fd = -1;
   int fd = -1;

   memfd = memfd_create("vkr-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (memfd < 0) {
      vkr_log("memfd_create failed (%s)", strerror(errno));
      goto fail;
   }

   const size_t size = align(alloc_info->allocationSize, getpagesize());
   int ret = ftruncate(memfd, size);
   if (ret) {
      vkr_log("ftruncate failed (%s)", strerror(errno));
      goto fail;
   }

   ret = fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
   if (ret) {
      vkr_log("fcntl F_ADD_SEALS failed (%s)", strerror(errno));
      goto fail;
   }

   const struct udmabuf_create create = {
      .memfd = memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .size = size,
   };
   udmabuf_fd = ioctl(physical_dev->udmabuf_dev_fd, UDMABUF_CREATE, &create);
   if (udmabuf_fd < 0) {
      vkr_log("ioctl UDMABUF_CREATE failed (%s)", strerror(errno));
      goto fail;
   }

   fd = os_dupfd_cloexec(udmabuf_fd);
   if (fd < 0) {
      vkr_log("os_dupfd_cloexec failed (%s)", strerror(errno));
      goto fail;
   }

   os_close_fd(memfd);

   *out_udmabuf_fd = udmabuf_fd;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   return VK_SUCCESS;

fail:
   if (udmabuf_fd >= 0)
      os_close_fd(udmabuf_fd);
   if (memfd >= 0)
      os_close_fd(memfd);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#else  /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

static inline VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(
   UNUSED struct vkr_physical_device *physical_dev,
   UNUSED const VkMemoryAllocateInfo *alloc_info,
   UNUSED int *out_udmabuf_fd,
   UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("udmabuf_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

#ifdef ENABLE_GBM_ALLOCATION
#include <gbm.h>

#define GBM_BO_USE_SW_READ_RARELY (1 << 10)
#define GBM_BO_USE_SW_WRITE_RARELY (1 << 12)

static inline int
vkr_gbm_bo_get_fd(void *gbm_bo)
{
   assert(gbm_bo);

   /* gbm_bo_get_fd returns negative error code on failure */
   return gbm_bo_get_fd(gbm_bo);
}

static inline void
vkr_gbm_bo_destroy(void *gbm_bo)
{
   gbm_bo_destroy(gbm_bo);
}

static VkResult
vkr_gbm_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                         const VkMemoryAllocateInfo *alloc_info,
                                         void **out_gbm_bo,
                                         VkImportMemoryFdInfoKHR *out_fd_info)
{
   const uint32_t flags =
      GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_RARELY | GBM_BO_USE_SW_WRITE_RARELY;
   struct gbm_bo *gbm_bo;
   int fd = -1;

   assert(physical_dev->gbm_device);

   /*
    * Reject here for simplicity. Letting VkPhysicalDeviceVulkan11Properties return
    * min(maxMemoryAllocationSize, UINT32_MAX) will affect unmappable scenarios.
    */
   if (alloc_info->allocationSize > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Page alignment is used on all implementations we support. */
   const uint32_t alloc_size = align(alloc_info->allocationSize, getpagesize());
#ifdef MINIGBM
   const uint32_t format = GBM_FORMAT_R8;
   const uint32_t width = alloc_size;
   const uint32_t height = 1;
#else
   /* Mesa gbm has texture size limitations, so we can't rely on R8 here. Instead, we
    * allocate a large enough linear rgba8 buffer.
    */
   const uint32_t format = GBM_FORMAT_ABGR8888;
   const uint8_t pixel_bytes = 4;
   const uint32_t width =
      (uint32_t)ceil(sqrt((alloc_size + pixel_bytes - 1) / pixel_bytes));
   const uint32_t height = width;
#endif /* MINIGBM */

   gbm_bo = gbm_bo_create(physical_dev->gbm_device, width, height, format, flags);
   if (!gbm_bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   fd = vkr_gbm_bo_get_fd(gbm_bo);
   if (fd < 0) {
      vkr_gbm_bo_destroy(gbm_bo);
      return fd == -EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *out_gbm_bo = (void *)gbm_bo;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   return VK_SUCCESS;
}

#else

static inline int
vkr_gbm_bo_get_fd(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
   return -1;
}

static inline void
vkr_gbm_bo_destroy(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
}

static inline VkResult
vkr_gbm_get_fd_info_from_allocation_info(UNUSED struct vkr_physical_device *physical_dev,
                                         UNUSED const VkMemoryAllocateInfo *alloc_info,
                                         UNUSED void **out_gbm_bo,
                                         UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("minigbm_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* ENABLE_GBM_ALLOCATION */

static void
vkr_dispatch_vkAllocateMemory(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkAllocateMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_physical_device *physical_dev = dev->physical_device;

   VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)args->pAllocateInfo;
   const uint32_t mem_type_index = alloc_info->memoryTypeIndex;
   if (unlikely(mem_type_index >= physical_dev->memory_properties.memoryTypeCount)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }
   /* The host type that really backs the guest type (differs only for a device local + host
    * visible type that is backed by system memory). */
   const uint32_t host_mem_type_index = physical_dev->memory_type_redirect[mem_type_index];
   winq_dbg("AllocReq size=%llu guest_type=%u host_type=%u", (unsigned long long)alloc_info->allocationSize,
            mem_type_index, host_mem_type_index);
   alloc_info->memoryTypeIndex = host_mem_type_index;

   /* translate VkImportMemoryResourceInfoMESA into VkImportMemoryFdInfoKHR in place */
   VkImportMemoryFdInfoKHR local_import_info = { .fd = -1 };
#ifdef _WIN32
   VkImportMemoryWin32HandleInfoKHR local_import_win32_info = {
      .handle = INVALID_HANDLE_VALUE,
   };
#endif
   const void *import_info = NULL;
   VkImportMemoryResourceInfoMESA *res_info = NULL;
   bool winq_gl_import = false;
   VkBaseInStructure *prev_of_res_info = vkr_find_prev_struct(
      alloc_info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
   if (prev_of_res_info) {
      res_info = (VkImportMemoryResourceInfoMESA *)prev_of_res_info->pNext;
#ifdef _WIN32
      {
         /* winq: the memory of an OpenGL texture only means something on the adapter it was allocated on,
          * and its memory type is an index into that adapter's types. */
         const struct vkr_resource *winq_chk = vkr_context_get_resource(ctx, res_info->resourceId);
         uint8_t winq_luid[VK_LUID_SIZE];
         if (winq_chk && winq_chk->winq_gl &&
             (winq_chk->winq_gl_mem_type >= physical_dev->memory_properties.memoryTypeCount ||
              !vrend_winq_share_luid(winq_luid) || !physical_dev->id_properties.deviceLUIDValid ||
              memcmp(winq_luid, physical_dev->id_properties.deviceLUID, VK_LUID_SIZE))) {
            winq_dbg("GLIMPORT res=%u refused: another adapter or a bad memory type", res_info->resourceId);
            args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            return;
         }
      }
#endif
      if (!vkr_get_fd_info_from_resource_info(ctx, dev, res_info, &local_import_info,
#ifdef _WIN32
                                              &local_import_win32_info,
#endif
                                              &import_info)) {
         const struct vkr_resource *refused = vkr_context_get_resource(ctx, res_info->resourceId);
         winq_dbg("ImportRefused res=%u size=%llu found=%d fd_type=%d", res_info->resourceId,
                  (unsigned long long)alloc_info->allocationSize, refused ? 1 : 0, refused ? (int)refused->fd_type : -1);
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }

      prev_of_res_info->pNext = import_info;
#ifdef _WIN32
      /* winq: the memory behind the handle was allocated in whole pages (see below); an import of
       * another size is not the same allocation, so ask for the size the memory really has. */
      alloc_info->allocationSize = align(alloc_info->allocationSize, getpagesize());

      /* winq: memory of an OpenGL texture must be imported with the type and size it was allocated
       * with, whatever the guest worked out from the image it wants to bind to it. */
      struct vkr_resource *winq_res = vkr_context_get_resource(ctx, res_info->resourceId);
      if (winq_res && winq_res->winq_gl) {
         winq_gl_import = true;
         alloc_info->memoryTypeIndex = winq_res->winq_gl_mem_type;
         alloc_info->allocationSize = winq_res->size;
         /* The memory is not a dedicated allocation of the image the guest names (which is replaced when
          * it is bound, see vkr_image.c): the host driver would tie the two together. */
         VkBaseInStructure *prev_of_dedicated =
            vkr_find_prev_struct(alloc_info, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
         if (prev_of_dedicated)
            prev_of_dedicated->pNext = prev_of_dedicated->pNext->pNext;
         winq_dbg("GLIMPORT res=%u size=%llu type=%u dedicated=%d", res_info->resourceId,
                  (unsigned long long)alloc_info->allocationSize, alloc_info->memoryTypeIndex,
                  prev_of_dedicated != NULL);
      } else if (winq_res) {
         /* winq: memory another Venus context exported (a swapchain image that gamescope shows, for
          * example) is imported through its opaque Win32 handle, and such an import must use the
          * memory type and size the memory was allocated with.  The guest picks the type from the
          * image it binds; with another type the host driver may fail the import (seen as
          * VK_ERROR_OUT_OF_DEVICE_MEMORY), and the guest's next use of the missing memory then
          * ends its context. */
         uint32_t exp_type;
         uint64_t exp_size;
         uint8_t exp_luid[VK_LUID_SIZE];
         if ((winq_res->fd_type == VIRGL_RESOURCE_FD_DMABUF || winq_res->fd_type == VIRGL_RESOURCE_FD_OPAQUE) &&
             vkr_winq_export_registry_get(res_info->resourceId, os_get_win32_handle_from_fd(winq_res->u.fd),
                                          &exp_type, &exp_size, exp_luid) &&
             exp_type < physical_dev->memory_properties.memoryTypeCount &&
             physical_dev->id_properties.deviceLUIDValid &&
             !memcmp(exp_luid, physical_dev->id_properties.deviceLUID, VK_LUID_SIZE)) {
            winq_dbg("VKIMPORT res=%u type %u->%u size %llu->%llu", res_info->resourceId,
                     alloc_info->memoryTypeIndex, exp_type,
                     (unsigned long long)alloc_info->allocationSize, (unsigned long long)exp_size);
            alloc_info->memoryTypeIndex = exp_type;
            alloc_info->allocationSize = exp_size;
         }
      }
#endif
   }

   VkExportMemoryAllocateInfo *export_info =
      vkr_find_struct(alloc_info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
#ifdef _WIN32
   /* winq: an allocation that imports a handle must not also ask for a new export; the host driver then
    * gives the guest an allocation of its own that does not alias the imported memory.  So the memory
    * is not exportable either. */
   if (res_info && export_info) {
      VkBaseInStructure *prev_of_export = vkr_find_prev_struct(alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
      if (prev_of_export)
         prev_of_export->pNext = export_info->pNext;
      export_info = NULL;
   }
#endif

   /* track if driver has requested export allocation */
   const bool might_export = export_info && export_info->handleTypes;

   /* XXX Force dma_buf/opaque fd export or gbm bo import until a new extension that
    * supports direct export from host visible memory
    *
    * Most VkImage and VkBuffer are non-external while most VkDeviceMemory are external
    * if allocated with a host visible memory type. We still violate the spec by binding
    * external memory to non-external image or buffer, which needs spec changes with a
    * new extension.
    *
    * Skip forcing external if a valid VkImportMemoryResourceInfoMESA is provided, since
    * the mapping will be directly set up from the existing virgl resource.
    */
   const uint32_t property_flags =
      physical_dev->memory_properties.memoryTypes[mem_type_index].propertyFlags;
   uint32_t valid_fd_types = 0;
   VkExternalMemoryHandleTypeFlags guest_export_handle_types = 0;
   int udmabuf_fd = -1;
   void *gbm_bo = NULL;
   VkExportMemoryAllocateInfo local_export_info;
   if ((property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !res_info) {
      /* An implementation can support dma_buf import along with opaque fd export/import.
       * If the client driver is using external memory and requesting dma_buf, without
       * dma_buf fd export support, we must use gbm bo import path instead of forcing
       * opaque fd export. e.g. the client driver uses external memory for wsi image.
       */
#ifdef _WIN32
      /* winq: the host cannot export dma-buf and there is no gbm; the dma-buf shim stands in for it
       * (DMA_BUF is just the opaque Win32 handle, see below), so a guest request to export host visible
       * memory as dma-buf (gamescope's scanout images) is served by the opaque path instead of the gbm
       * import path, which is not built and fails the allocation.  The memory is then exported to the
       * guest as an opaque resource, which the proxy accepts on Windows. */
      const bool dma_buf_via_opaque = physical_dev->dma_buf_shim_active;
#else
      const bool dma_buf_via_opaque = false;
#endif
      const bool no_dma_buf_export =
         !export_info || dma_buf_via_opaque ||
         !(export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      const bool force_gbm_import = !!physical_dev->gbm_device;
      const bool force_udmabuf_import = physical_dev->udmabuf_dev_fd >= 0;
      if (!(force_gbm_import || force_udmabuf_import) &&
          (physical_dev->is_dma_buf_fd_export_supported ||
           (physical_dev->is_opaque_fd_export_supported && no_dma_buf_export))) {
         const VkExternalMemoryHandleTypeFlagBits guest_handle_type =
            physical_dev->is_dma_buf_fd_export_supported
               ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
               : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         VkExternalMemoryHandleTypeFlagBits host_handle_type = guest_handle_type;
#ifdef _WIN32
         if (guest_handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
             physical_dev->host_external_memory_win32) {
            host_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
         }
#endif
         guest_export_handle_types = guest_handle_type;
         if (export_info) {
            export_info->handleTypes = host_handle_type;
         } else {
            local_export_info = (const VkExportMemoryAllocateInfo){
               .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
               .pNext = alloc_info->pNext,
               .handleTypes = host_handle_type,
            };
            export_info = &local_export_info;
            alloc_info->pNext = &local_export_info;

            if (guest_handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
               /* Guest virtgpu kernel aligns up blob mem size to the page boundary. No
                * matter dma-buf or opaque fd export allocation, the actual allocation
                * in most cases would follow the same padding. For dma-buf, we are able
                * to allocate and validate via lseek below. For opaque fd, Vulkan spec
                * requires to use the same size with allocation time for the mapper,
                * either vkr_allocator or vulkano, to import and populate the mapping.
                * Accessible mapping given by vkMapMemory is normally limited to just the
                * alloc size. When KVM register the mappings to guest pci bar, that
                * involves an invalid chunk towards the end of the page boundary. As a
                * result, any guest side accelerated instructions that rely on the
                * paddings can end up with Illegal instruction error. The most common
                * trigger is via memcpy'ing valid range to the guest side mapped buffer
                * memory, and then, e.g. __memcpy_avx_unaligned_erms  can hit the error.
                */
               alloc_info->allocationSize =
                  align(alloc_info->allocationSize, getpagesize());
            }
         }
      } else if (physical_dev->EXT_external_memory_dma_buf) {
         /* Allocate dma_buf externally and force to import. */
         if (export_info) {
            /* Strip export info since valid_fd_types can only be dma_buf here. */
            VkBaseInStructure *prev_of_export_info = vkr_find_prev_struct(
               alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

            prev_of_export_info->pNext = export_info->pNext;
            export_info = NULL;
         }

         if (force_udmabuf_import) {
            /* To be noted, there are 2 limits for udmabuf:
             * - list_limit: udmabuf_create_list->count limit. Default is 1024.
             * - size_limit_mb: Max size of a dmabuf, in megabytes. Default is 64.
             */
            args->ret = vkr_udmabuf_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &udmabuf_fd, &local_import_info);
         } else {
            args->ret = vkr_gbm_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &gbm_bo, &local_import_info);
         }
         if (args->ret != VK_SUCCESS) {
            winq_dbg("ALLOCEXIT dmabuf-force ret=%d size=%llu", (int)args->ret,
                     (unsigned long long)alloc_info->allocationSize);
            return;
         }

         alloc_info->pNext = &local_import_info;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      }
   }

   /* winq: the guest maps an exportable host visible allocation in whole pages (and the device refuses a
    * mapping that is not), so round the size up like the path above does for a forced export; the
    * guest's own export info (for example its cursor and scanout buffers) takes this path too. */
   if (!res_info && export_info)
      alloc_info->allocationSize = align(alloc_info->allocationSize, getpagesize());

   if (!guest_export_handle_types && export_info)
      guest_export_handle_types = export_info->handleTypes;

   if (export_info) {
      if (guest_export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_OPAQUE;
      if (guest_export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_DMABUF;
   }

#ifdef _WIN32
   /*
    * dma-buf shim: host ICD does not understand DMA_BUF_BIT_EXT. Remap any
    * DMA_BUF entry in the export info to OPAQUE_WIN32 before calling the
    * host. guest_export_handle_types was captured above and retains the
    * DMA_BUF bit, so valid_fd_types correctly reports DMA_BUF back to the
    * guest. OPAQUE_FD is similarly remapped to OPAQUE_WIN32 for consistency.
    */
   if (export_info && physical_dev->dma_buf_shim_active) {
      const VkExternalMemoryHandleTypeFlags fd_bits =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      if (export_info->handleTypes & fd_bits) {
         export_info->handleTypes =
            (export_info->handleTypes & ~fd_bits) |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      }
   }
#endif

   /* What is allocated here counts against the host memory limit.  An import
    * only wraps memory that exists already. */
   const bool counted = !res_info;
   if (counted && !vkr_device_memory_reserve(alloc_info->allocationSize)) {
      vkr_log("vkAllocateMemory of %llu bytes refused: over the host memory limit (%llu bytes in use)",
              (unsigned long long)alloc_info->allocationSize,
              (unsigned long long)vkr_device_memory_used());
      if (local_import_info.fd >= 0)
         os_close_fd(local_import_info.fd);
#ifdef _WIN32
      if (local_import_win32_info.handle &&
          local_import_win32_info.handle != INVALID_HANDLE_VALUE)
         CloseHandle(local_import_win32_info.handle);
#endif
      if (gbm_bo)
         vkr_gbm_bo_destroy(gbm_bo);
      args->ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return;
   }

   struct vkr_device_memory *mem = vkr_device_memory_create_and_add(ctx, args);
   winq_dbg("AllocMem ret=%d size=%llu type=%u(guest %u) props=0x%x export=%d res_info=%d used=%llu", (int)args->ret,
            (unsigned long long)alloc_info->allocationSize, host_mem_type_index, mem_type_index, (unsigned)property_flags,
            export_info ? 1 : 0, res_info ? 1 : 0, (unsigned long long)vkr_device_memory_used());
   if (!mem) {
      if (counted)
         vkr_device_memory_unreserve(alloc_info->allocationSize);
      if (local_import_info.fd >= 0)
         os_close_fd(local_import_info.fd);
#ifdef _WIN32
      if (local_import_win32_info.handle &&
          local_import_win32_info.handle != INVALID_HANDLE_VALUE)
         CloseHandle(local_import_win32_info.handle);
#endif
      if (gbm_bo)
         vkr_gbm_bo_destroy(gbm_bo);
      return;
   }

#ifdef _WIN32
   /* Unlike fd imports, importing an NT handle does not transfer its
    * ownership to the driver; close our duplicate now that the import worked.
    */
   if (local_import_win32_info.handle &&
       local_import_win32_info.handle != INVALID_HANDLE_VALUE)
      CloseHandle(local_import_win32_info.handle);
#endif

   mem->device = dev;
   mem->might_export = might_export;
   mem->property_flags = property_flags;
   mem->valid_fd_types = valid_fd_types;
   mem->udmabuf_fd = udmabuf_fd;
   mem->gbm_bo = gbm_bo;
   mem->allocation_size = alloc_info->allocationSize;
   /* the host type the memory really has (an import can take another type than the guest asked for) */
   mem->memory_type_index = alloc_info->memoryTypeIndex;
   mem->counted = counted;
   mem->winq_gl = winq_gl_import;
   mem->winq_gl_mem_type = winq_gl_import ? alloc_info->memoryTypeIndex : 0;
}

static void
vkr_dispatch_vkFreeMemory(struct vn_dispatch_context *dispatch,
                          struct vn_command_vkFreeMemory *args)
{
   TRACE_FUNC();
   struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);
   if (!mem)
      return;

   vkr_device_memory_release(mem);
   vkr_device_memory_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceMemoryCommitment(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryCommitment *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryCommitment_args_handle(args);
   vk->GetDeviceMemoryCommitment(args->device, args->memory,
                                 args->pCommittedMemoryInBytes);
}

static void
vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryOpaqueCaptureAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryOpaqueCaptureAddress_args_handle(args);
   args->ret = vk->GetDeviceMemoryOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkGetMemoryResourcePropertiesMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetMemoryResourcePropertiesMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_resource *res = vkr_context_get_resource(ctx, args->resourceId);
   if (!res) {
      vkr_log("failed to query resource props: invalid res_id %u", args->resourceId);
      vkr_context_set_fatal(ctx);
      return;
   }

   vn_replace_vkGetMemoryResourcePropertiesMESA_args_handle(args);
   if (res->fd_type == VIRGL_RESOURCE_FD_DMABUF) {
      static const VkExternalMemoryHandleTypeFlagBits handle_type =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      VkMemoryFdPropertiesKHR mem_fd_props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
         .pNext = NULL,
         .memoryTypeBits = 0,
      };
      args->ret =
         vk->GetMemoryFdPropertiesKHR(args->device, handle_type, res->u.fd, &mem_fd_props);
      if (args->ret != VK_SUCCESS)
         return;

      args->pMemoryResourceProperties->memoryTypeBits = mem_fd_props.memoryTypeBits;
#ifdef _WIN32
   } else if (res->fd_type == VIRGL_RESOURCE_FD_OPAQUE &&
              dev->GetMemoryWin32HandlePropertiesKHR) {
      HANDLE handle = os_get_win32_handle_from_fd(res->u.fd);
      VkMemoryWin32HandlePropertiesKHR props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR,
         .pNext = NULL,
         .memoryTypeBits = 0,
      };
      args->ret = dev->GetMemoryWin32HandlePropertiesKHR(
         args->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, handle, &props);
      if (args->ret != VK_SUCCESS)
         return;

      args->pMemoryResourceProperties->memoryTypeBits = props.memoryTypeBits;
#endif
   } else {
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }
   /* winq: the types as the guest sees them, see vkr_physical_device_remap_memory_type_bits */
   args->pMemoryResourceProperties->memoryTypeBits = vkr_physical_device_remap_memory_type_bits(
      dev->physical_device, args->pMemoryResourceProperties->memoryTypeBits, false);

   VkMemoryResourceAllocationSizePropertiesMESA *alloc_size_props =
      vkr_find_struct(args->pMemoryResourceProperties->pNext,
                      VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA);
   if (alloc_size_props)
      alloc_size_props->allocationSize = res->size;
   winq_dbg("MEMRES res=%u size=%llu bits=0x%x ret=%d type=%d", args->resourceId, (unsigned long long)res->size,
            args->pMemoryResourceProperties->memoryTypeBits, (int)args->ret, (int)res->fd_type);
}

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateMemory = vkr_dispatch_vkAllocateMemory;
   dispatch->dispatch_vkFreeMemory = vkr_dispatch_vkFreeMemory;
   dispatch->dispatch_vkMapMemory = NULL;
   dispatch->dispatch_vkUnmapMemory = NULL;
   dispatch->dispatch_vkFlushMappedMemoryRanges = NULL;
   dispatch->dispatch_vkInvalidateMappedMemoryRanges = NULL;
   dispatch->dispatch_vkGetDeviceMemoryCommitment =
      vkr_dispatch_vkGetDeviceMemoryCommitment;
   dispatch->dispatch_vkGetDeviceMemoryOpaqueCaptureAddress =
      vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress;

   dispatch->dispatch_vkGetMemoryResourcePropertiesMESA =
      vkr_dispatch_vkGetMemoryResourcePropertiesMESA;
}

void
vkr_device_memory_release(struct vkr_device_memory *mem)
{
   if (mem->counted) {
      mem->counted = false;
      vkr_device_memory_unreserve(mem->allocation_size);
   }
   if (mem->gbm_bo)
      vkr_gbm_bo_destroy(mem->gbm_bo);
   if (mem->udmabuf_fd >= 0)
      os_close_fd(mem->udmabuf_fd);
}

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob)
{
   TRACE_FUNC();

   /* a memory can only be exported once; we don't want two resources to point
    * to the same storage.
    */
   if (mem->exported) {
      vkr_log("mem has been exported");
      return false;
   }

   uint32_t map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool visible = mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const bool cached = mem->property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      if (!visible) {
         vkr_log("mem cannot support mappable blob");
         return false;
      }

      /* XXX guessed */
      map_info = (coherent && cached) ? VIRGL_RENDERER_MAP_CACHE_CACHED
                                      : VIRGL_RENDERER_MAP_CACHE_WC;
   }

   const bool can_export_dma_buf = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF);
   const bool can_export_opaque = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_OPAQUE);
   enum virgl_resource_fd_type fd_type;
   VkExternalMemoryHandleTypeFlagBits handle_type;
   struct virgl_resource_vulkan_info vulkan_info;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
      if (!can_export_dma_buf) {
         vkr_log("mem cannot export to dma_buf for cross device blob sharing");
         return false;
      }
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_dma_buf) {
      /* prefer dmabuf for easier mapping? */
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_opaque) {
      /* prefer opaque for performance? */
      fd_type = VIRGL_RESOURCE_FD_OPAQUE;
      handle_type =
#ifdef _WIN32
         mem->device->physical_device->host_external_memory_win32
            ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
            :
#endif
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

      STATIC_ASSERT(sizeof(vulkan_info.device_uuid) == VK_UUID_SIZE);
      STATIC_ASSERT(sizeof(vulkan_info.driver_uuid) == VK_UUID_SIZE);

      const VkPhysicalDeviceIDProperties *id_props =
         &mem->device->physical_device->id_properties;
      memcpy(vulkan_info.device_uuid, id_props->deviceUUID, VK_UUID_SIZE);
      memcpy(vulkan_info.driver_uuid, id_props->driverUUID, VK_UUID_SIZE);

      vulkan_info.allocation_size = mem->allocation_size;
      vulkan_info.memory_type_index = mem->memory_type_index;
   } else {
      vkr_log("mem is not exportable");
      return false;
   }

   int fd;
   if (mem->udmabuf_fd >= 0) {
      fd = os_dupfd_cloexec(mem->udmabuf_fd);
      if (fd < 0) {
         vkr_log("mem udmabuf fd dup failed (%s)", strerror(errno));
         return false;
      }
   } else if (mem->gbm_bo) {
      assert(handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      assert(can_export_dma_buf && !can_export_opaque);

      fd = vkr_gbm_bo_get_fd(mem->gbm_bo);
      if (fd < 0) {
         vkr_log("mem gbm bo export failed (ret %d)", fd);
         return false;
      }
   } else {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
#ifdef _WIN32
      /* On Windows, both OPAQUE_WIN32 and shim-synthesized DMA_BUF exports
       * are backed by the same physical Win32 HANDLE. The shim advertises
       * DMA_BUF on the guest side; internally the host ICD knows only about
       * OPAQUE_WIN32. */
      const bool win32_export =
         handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT ||
         (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
          mem->device->physical_device->dma_buf_shim_active);
      if (win32_export && mem->device->GetMemoryWin32HandleKHR) {
         HANDLE handle = INVALID_HANDLE_VALUE;
         const VkMemoryGetWin32HandleInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
            .memory = mem->base.handle.device_memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
         };
         VkResult ret = mem->device->GetMemoryWin32HandleKHR(
            mem->device->base.handle.device, &info, &handle);
         if (ret != VK_SUCCESS) {
            vkr_log("mem win32 handle export failed (vk ret %d)", ret);
            return false;
         }
         fd = os_wrap_win32_handle(handle);
         if (fd < 0) {
            CloseHandle(handle);
            return false;
         }
      } else
#endif
      {
         const VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = mem->base.handle.device_memory,
            .handleType = handle_type,
         };
         VkResult ret = vk->GetMemoryFdKHR(mem->device->base.handle.device, &fd_info, &fd);
         if (ret != VK_SUCCESS) {
            vkr_log("mem fd export failed (vk ret %d)", ret);
            return false;
         }
      }
   }

   if (fd_type == VIRGL_RESOURCE_FD_DMABUF) {
#ifndef _WIN32
      /* Real Linux dma-buf fds are seekable and lseek returns the size. On
       * Windows, the "fd" is a token wrapping an NT HANDLE that isn't a
       * real file descriptor; skip the size validation, we trust the size
       * that was passed to vkAllocateMemory. */
      const off_t dma_buf_size = lseek(fd, 0, SEEK_END);
      if (dma_buf_size < 0 || (uint64_t)dma_buf_size < blob_size) {
         vkr_log("mem dma_buf_size %lld < blob_size %" PRIu64, (long long)dma_buf_size,
                 blob_size);
         os_close_fd(fd);
         return false;
      }
#endif
   }

   mem->exported = true;
   *out_blob = (struct virgl_context_blob){
      .type = fd_type,
      .u.fd = fd,
      .map_info = map_info,
      .vulkan_info = vulkan_info,
   };

   return true;
}
