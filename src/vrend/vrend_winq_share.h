/*
 * winq: memory for textures that OpenGL (vrend) and Vulkan (Venus) both use, see vrend_winq_share.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef VREND_WINQ_SHARE_H
#define VREND_WINQ_SHARE_H

#ifdef _WIN32

#include <stdbool.h>
#include <stdint.h>

struct pipe_resource;

/* Allocates Vulkan memory for an optimal tiling 2D image and returns it as an NT handle (to be closed
 * with vrend_winq_share_close) and its size.  luid is the adapter OpenGL renders with (8 bytes). */
bool
vrend_winq_share_alloc(const uint8_t *luid,
                       bool luid_valid,
                       uint32_t width,
                       uint32_t height,
                       uint32_t vk_format,
                       void **out_handle,
                       uint64_t *out_size,
                       uint32_t *out_mem_type);

/* Which resources are OpenGL textures with Vulkan memory, by resource id.  Set when such a resource is
 * attached to a Venus context (cleared when any other resource with that id is), read by the Venus
 * context that imports it, on another thread.  handle is the texture's NT handle; a lookup only finds
 * the entry with a handle to the same memory. */
void
vrend_winq_share_registry_set(uint32_t res_id, bool shared, uint32_t mem_type, void *handle);

bool
vrend_winq_share_registry_get(uint32_t res_id, void *handle, uint32_t *out_mem_type);

/* The resource with this id is gone. */
void
vrend_winq_share_registry_clear(uint32_t res_id);

/* All resources are gone (renderer reset or cleanup). */
void
vrend_winq_share_registry_reset(void);

/* Whether two handles refer to the same kernel object (false when that cannot be told). */
bool
vrend_winq_share_same_object(void *a, void *b);

/* The LUID (8 bytes) of the adapter the shared memory is allocated on; false if nothing is shared. */
bool
vrend_winq_share_luid(uint8_t *out_luid);

void
vrend_winq_share_close(void *handle);

/* The size, memory type and NT handle (owned by the texture) of the Vulkan memory behind a vrend texture,
 * 0 if it has none (vrend_renderer.c). */
uint64_t
vrend_renderer_winq_share_size(struct pipe_resource *pres, uint32_t *mem_type, void **handle);

#endif /* _WIN32 */

#endif /* VREND_WINQ_SHARE_H */
