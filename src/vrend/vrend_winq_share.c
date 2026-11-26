/*
 * winq: memory for textures that OpenGL (vrend) and Vulkan (Venus) both use.
 *
 * On Linux a texture the guest shares between its OpenGL and Vulkan programs is a dma-buf both host
 * drivers can open.  Windows has nothing like that, so such a texture gets its storage from Vulkan:
 * the memory is allocated here for a Vulkan image with the same size and format, exported as an NT
 * handle, and imported by OpenGL (GL_EXT_memory_object_win32) and later by the Venus context that
 * imports the resource.  The image uses optimal tiling: that is the layout the OpenGL driver assumes
 * for memory object textures (it has no other on NVIDIA).
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "vrend_winq_share.h"
#include "virgl_util.h"

/*
 * The Vulkan instance and device the memory is allocated with, set up at the first shared texture and kept
 * for the life of the process (a renderer that is initialized again reuses them).  Only the vrend thread
 * allocates (it needs vrend's current OpenGL context), so the setup needs no lock.  Venus threads only read
 * the LUID, after they found a resource in the registry below: its lock orders that read after the setup.
 */
static struct {
   int state; /* 0: not tried, 1: ready, -1: failed (sharing stays off) */
   HMODULE lib;
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   VkPhysicalDeviceMemoryProperties mem_props;
   uint8_t luid[VK_LUID_SIZE];

   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
   PFN_vkDestroyInstance DestroyInstance;
   PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
   PFN_vkCreateDevice CreateDevice;
   PFN_vkDestroyDevice DestroyDevice;
   PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32HandleKHR;
} share;

/* Every shared texture holds an NT handle and a Vulkan allocation the Venus memory limit does not see. */
#define WINQ_SHARE_MAX_LIVE 4096
static volatile LONG share_live;

static bool
vrend_winq_share_setup(const uint8_t *luid, bool luid_valid)
{
   share.lib = LoadLibraryExA("vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!share.lib)
      return false;
   share.GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(share.lib, "vkGetInstanceProcAddr");
   if (!share.GetInstanceProcAddr)
      return false;
   PFN_vkCreateInstance CreateInstance =
      (PFN_vkCreateInstance)share.GetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!CreateInstance)
      return false;

   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "virglrenderer",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };
   if (CreateInstance(&instance_info, NULL, &share.instance) != VK_SUCCESS)
      return false;

#define GET_I(name)                                                                              \
   share.name = (PFN_vk##name)share.GetInstanceProcAddr(share.instance, "vk" #name);             \
   if (!share.name)                                                                              \
      return false
   GET_I(DestroyInstance);
   GET_I(EnumeratePhysicalDevices);
   GET_I(GetPhysicalDeviceProperties2);
   GET_I(GetPhysicalDeviceMemoryProperties);
   GET_I(CreateDevice);
   GET_I(DestroyDevice);
   GET_I(GetDeviceProcAddr);
#undef GET_I

   /* The device OpenGL renders with: the handles only work on the same adapter. */
   VkPhysicalDevice devs[16];
   uint32_t dev_count = ARRAY_SIZE(devs);
   VkResult result = share.EnumeratePhysicalDevices(share.instance, &dev_count, devs);
   if (result != VK_SUCCESS && result != VK_INCOMPLETE)
      return false;
   for (uint32_t i = 0; i < dev_count && !share.physical_device; i++) {
      VkPhysicalDeviceIDProperties id_props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
      };
      VkPhysicalDeviceProperties2 props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &id_props,
      };
      share.GetPhysicalDeviceProperties2(devs[i], &props);
      if (luid_valid && id_props.deviceLUIDValid && !memcmp(id_props.deviceLUID, luid, VK_LUID_SIZE)) {
         share.physical_device = devs[i];
         memcpy(share.luid, luid, VK_LUID_SIZE);
      }
   }
   if (!share.physical_device) {
      virgl_warn("winq share: no Vulkan device matches the OpenGL adapter, textures are not shared\n");
      return false;
   }
   share.GetPhysicalDeviceMemoryProperties(share.physical_device, &share.mem_props);

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *const exts[] = { "VK_KHR_external_memory_win32" };
   const VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = ARRAY_SIZE(exts),
      .ppEnabledExtensionNames = exts,
   };
   if (share.CreateDevice(share.physical_device, &dev_info, NULL, &share.device) != VK_SUCCESS)
      return false;

#define GET_D(name)                                                                              \
   share.name = (PFN_vk##name)share.GetDeviceProcAddr(share.device, "vk" #name);                 \
   if (!share.name)                                                                              \
      return false
   GET_D(CreateImage);
   GET_D(DestroyImage);
   GET_D(GetImageMemoryRequirements);
   GET_D(AllocateMemory);
   GET_D(FreeMemory);
   GET_D(GetMemoryWin32HandleKHR);
#undef GET_D

   return true;
}

static bool
vrend_winq_share_init(const uint8_t *luid, bool luid_valid)
{
   if (share.state)
      return share.state > 0;

   if (vrend_winq_share_setup(luid, luid_valid)) {
      share.state = 1;
      return true;
   }

   /* Release what a setup that failed half way created; it is not tried again. */
   if (share.device && share.DestroyDevice)
      share.DestroyDevice(share.device, NULL);
   if (share.instance && share.DestroyInstance)
      share.DestroyInstance(share.instance, NULL);
   if (share.lib)
      FreeLibrary(share.lib);
   memset(&share, 0, sizeof(share));
   share.state = -1;
   return false;
}

/* Allocates the memory and returns its NT handle, NULL on failure. */
static HANDLE
vrend_winq_share_alloc_memory(uint32_t width,
                              uint32_t height,
                              uint32_t vk_format,
                              uint64_t *out_size,
                              uint32_t *out_mem_type)
{
   const VkExternalMemoryImageCreateInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external_info,
      .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, /* the guest also looks at it as sRGB */
      .imageType = VK_IMAGE_TYPE_2D,
      .format = (VkFormat)vk_format,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   if (share.CreateImage(share.device, &image_info, NULL, &image) != VK_SUCCESS)
      return NULL;

   VkMemoryRequirements reqs;
   share.GetImageMemoryRequirements(share.device, image, &reqs);
   share.DestroyImage(share.device, image, NULL);

   uint32_t type_index = UINT32_MAX;
   for (uint32_t i = 0; i < share.mem_props.memoryTypeCount; i++) {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (share.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
         type_index = i;
         break;
      }
   }
   if (type_index == UINT32_MAX)
      return NULL;

   /* Whole pages: the importing Venus context asks for a page aligned size. */
   const uint64_t size = (reqs.size + 4095) & ~(uint64_t)4095;
   const VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_info,
      .allocationSize = size,
      .memoryTypeIndex = type_index,
   };
   VkDeviceMemory mem;
   if (share.AllocateMemory(share.device, &alloc_info, NULL, &mem) != VK_SUCCESS)
      return NULL;

   const VkMemoryGetWin32HandleInfoKHR handle_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
      .memory = mem,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };
   HANDLE handle = NULL;
   const VkResult result = share.GetMemoryWin32HandleKHR(share.device, &handle_info, &handle);
   /* The handle keeps the memory alive from here on. */
   share.FreeMemory(share.device, mem, NULL);
   if (result != VK_SUCCESS || !handle)
      return NULL;

   *out_size = size;
   *out_mem_type = type_index;
   return handle;
}

bool
vrend_winq_share_alloc(const uint8_t *luid,
                       bool luid_valid,
                       uint32_t width,
                       uint32_t height,
                       uint32_t vk_format,
                       void **out_handle,
                       uint64_t *out_size,
                       uint32_t *out_mem_type)
{
   if (!vrend_winq_share_init(luid, luid_valid))
      return false;

   /* Counted before the memory is allocated, so that the cap also holds back the allocations. */
   if (InterlockedIncrement(&share_live) > WINQ_SHARE_MAX_LIVE) {
      InterlockedDecrement(&share_live);
      return false;
   }
   HANDLE handle = vrend_winq_share_alloc_memory(width, height, vk_format, out_size, out_mem_type);
   if (!handle) {
      InterlockedDecrement(&share_live);
      return false;
   }
   *out_handle = handle;
   return true;
}

static BOOL CALLBACK
vrend_winq_share_resolve_compare(PINIT_ONCE once, PVOID param, PVOID *context)
{
   (void)once;
   (void)context;
   HMODULE module = GetModuleHandleW(L"kernelbase.dll");
   FARPROC proc = module ? GetProcAddress(module, "CompareObjectHandles") : NULL;
   *(FARPROC *)param = proc;
   return TRUE;
}

/* Whether two handles refer to the same kernel object.  Needs CompareObjectHandles (Windows 10 1607 and
 * later); false where it is missing, so that a lookup that depends on it finds nothing. */
bool
vrend_winq_share_same_object(void *a, void *b)
{
   typedef BOOL(WINAPI * PFN_CompareObjectHandles)(HANDLE, HANDLE);
   static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
   static FARPROC proc;
   InitOnceExecuteOnce(&once, vrend_winq_share_resolve_compare, &proc, NULL);
   if (!proc || !a || !b || a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE)
      return false;
   return ((PFN_CompareObjectHandles)(void (*)(void))proc)((HANDLE)a, (HANDLE)b) != FALSE;
}

/* The adapter the shared memory lives on, for the Venus side to compare with its own device. */
bool
vrend_winq_share_luid(uint8_t *out_luid)
{
   if (share.state != 1)
      return false;
   memcpy(out_luid, share.luid, VK_LUID_SIZE);
   return true;
}

#define WINQ_REGISTRY_SIZE 8192 /* a power of two, more than WINQ_SHARE_MAX_LIVE */
#define WINQ_REGISTRY_DELETED UINT32_MAX
static struct {
   uint32_t res_id; /* 0: never used, WINQ_REGISTRY_DELETED: free again */
   uint32_t mem_type;
   /* The texture's NT handle, owned by the vrend resource, which clears its entry before closing it.  An
    * import only matches the entry when its handle refers to the same memory: the guest picks the
    * resource ids and can give another resource the same id while an import is under way. */
   HANDLE handle;
} registry[WINQ_REGISTRY_SIZE];
static SRWLOCK registry_lock = SRWLOCK_INIT;

/* Returns the slot of res_id, or -1.  The caller holds the lock. */
static int
registry_find(uint32_t res_id)
{
   uint32_t idx = res_id & (WINQ_REGISTRY_SIZE - 1);
   for (uint32_t probes = 0; registry[idx].res_id && probes < WINQ_REGISTRY_SIZE; probes++) {
      if (registry[idx].res_id == res_id)
         return (int)idx;
      idx = (idx + 1) & (WINQ_REGISTRY_SIZE - 1);
   }
   return -1;
}

void
vrend_winq_share_registry_set(uint32_t res_id, bool shared, uint32_t mem_type, void *handle)
{
   if (!res_id || res_id == WINQ_REGISTRY_DELETED)
      return;
   AcquireSRWLockExclusive(&registry_lock);
   int slot = registry_find(res_id);
   if (!shared) {
      if (slot >= 0)
         registry[slot].res_id = WINQ_REGISTRY_DELETED;
   } else {
      if (slot < 0) {
         uint32_t idx = res_id & (WINQ_REGISTRY_SIZE - 1);
         for (uint32_t probes = 0; probes < WINQ_REGISTRY_SIZE; probes++) {
            if (!registry[idx].res_id || registry[idx].res_id == WINQ_REGISTRY_DELETED) {
               slot = (int)idx;
               break;
            }
            idx = (idx + 1) & (WINQ_REGISTRY_SIZE - 1);
         }
      }
      /* A full table (cannot happen while the live count is capped below its size) shares nothing more. */
      if (slot >= 0) {
         registry[slot].res_id = res_id;
         registry[slot].mem_type = mem_type;
         registry[slot].handle = (HANDLE)handle;
      }
   }
   ReleaseSRWLockExclusive(&registry_lock);
}

void
vrend_winq_share_registry_clear(uint32_t res_id)
{
   vrend_winq_share_registry_set(res_id, false, 0, NULL);
}

void
vrend_winq_share_registry_reset(void)
{
   AcquireSRWLockExclusive(&registry_lock);
   memset(registry, 0, sizeof(registry));
   ReleaseSRWLockExclusive(&registry_lock);
}

bool
vrend_winq_share_registry_get(uint32_t res_id, void *handle, uint32_t *out_mem_type)
{
   bool found = false;
   if (!res_id || res_id == WINQ_REGISTRY_DELETED)
      return false;
   AcquireSRWLockShared(&registry_lock);
   const int slot = registry_find(res_id);
   if (slot >= 0 && vrend_winq_share_same_object(registry[slot].handle, handle)) {
      found = true;
      *out_mem_type = registry[slot].mem_type;
   }
   ReleaseSRWLockShared(&registry_lock);
   return found;
}

void
vrend_winq_share_close(void *handle)
{
   if (handle) {
      CloseHandle((HANDLE)handle);
      InterlockedDecrement(&share_live);
   }
}

#endif /* _WIN32 */
