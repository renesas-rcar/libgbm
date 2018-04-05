/*
 * Copyright (c) 2013 Renesas Solutions Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Takanari Hayama <taki@igel.co.jp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

#include <xf86drm.h>
#include <libkms.h>
#include <errno.h>

#include <wayland-kms.h>

#include "gbmint.h"
#include "common_drm.h"
#include "gbm_kmsint.h"

#if defined(DEBUG)
#  define GBM_DEBUG(s, x...)	{ printf(s, ## x); }
#else
#  define GBM_DEBUG(s, x...)	{ }
#endif

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL<<56) - 1)
#endif

/*
 * Destroy gbm backend
 */
static void gbm_kms_destroy(struct gbm_device *gbm)
{
	free(gbm);
}

/*
 * Check if the given format is supported
 *
 * Weston requires GBM_FORMAT_XRGB8888 only for now.
 */
static int gbm_kms_is_format_supported(struct gbm_device *gbm,
				       uint32_t format, uint32_t usage)
{
	switch (format) {
		// 32bpp
	case GBM_FORMAT_ARGB8888:
	case GBM_BO_FORMAT_ARGB8888:
	case GBM_FORMAT_XRGB8888:
	case GBM_BO_FORMAT_XRGB8888:
		return 1;
	default:
		return 0;
	}
}

static int
gbm_kms_get_format_modifier_plane_count(struct gbm_device *gbm,
                                        uint32_t format,
                                        uint64_t modifier)
{
	return -1;
}

static void gbm_kms_bo_destroy(struct gbm_bo *_bo)
{
	struct gbm_kms_bo *bo = (struct gbm_kms_bo*)_bo;

	if (!bo)
		return;

	if (bo->allocated) {
		if (bo->addr)
			kms_bo_unmap(bo->bo);

		if (bo->fd)
			close(bo->fd);

		if (bo->bo)
			kms_bo_destroy(&bo->bo);
	}

	free(bo);

	return;
}

static struct gbm_bo *
gbm_kms_bo_create(struct gbm_device *gbm,
		  uint32_t width, uint32_t height,
		  uint32_t format, uint32_t usage,
		  const uint64_t *modifiers,
		  const unsigned int count)
{
	struct gbm_kms_device *dev = (struct gbm_kms_device*)gbm;
	struct gbm_kms_bo *bo;
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_TERMINATE_PROP_LIST
	};

	GBM_DEBUG("%s: %s: %d\n", __FILE__, __func__, __LINE__);

	if (!(bo = calloc(1, sizeof(struct gbm_kms_bo))))
		return NULL;

	switch (format) {
		// 32bpp
	case GBM_FORMAT_ARGB8888:
	case GBM_BO_FORMAT_ARGB8888:
	case GBM_FORMAT_XRGB8888:
	case GBM_BO_FORMAT_XRGB8888:
		break;
	default:
		// unsupported...
		goto error;
	}

	if (usage & (uint32_t)GBM_BO_USE_CURSOR_64X64)
		attr[1] = KMS_BO_TYPE_CURSOR_64X64_A8R8G8B8;
	attr[3] = width;
	attr[5] = height;

	// Create BO
	if (kms_bo_create(dev->kms, attr, &bo->bo))
		goto error;

	bo->base.gbm = gbm;
	bo->base.width = width;
	bo->base.height = height;
	bo->base.format = format;

	kms_bo_get_prop(bo->bo, KMS_HANDLE, &bo->base.handle.u32);
	kms_bo_get_prop(bo->bo, KMS_PITCH, &bo->base.stride);

	bo->size = bo->base.stride * bo->base.height;
	bo->allocated = true;

	if (drmPrimeHandleToFD(dev->base.base.fd, bo->base.handle.u32, DRM_CLOEXEC, &bo->fd)) {
		GBM_DEBUG("%s: %s: drmPrimeHandleToFD() failed. %s\n", __FILE__, __func__, strerror(errno));
		goto error;
	}

	// Map to the user space for bo_write
	if (usage & (uint32_t)GBM_BO_USE_WRITE) {
		if (kms_bo_map(bo->bo, &bo->addr))
			goto error;
	}

	return (struct gbm_bo*)bo;

 error:
	GBM_DEBUG("%s: %s: %d: ERROR!!!!\n", __FILE__, __func__, __LINE__);
	gbm_kms_bo_destroy((struct gbm_bo*)bo);
	return NULL;
}

static int gbm_kms_bo_write(struct gbm_bo *_bo, const void *buf, size_t count)
{
	struct gbm_kms_bo *bo = (struct gbm_kms_bo*)_bo;

	if (!bo->addr)
		return -1;

	memcpy(bo->addr, buf, count);

	return 0;
}

static struct gbm_kms_bo* gbm_kms_import_wl_buffer(struct gbm_device *gbm,
						   void *_buffer)
{
	struct wl_kms_buffer *buffer;
	struct gbm_kms_bo *bo;

	buffer = wayland_kms_buffer_get((struct wl_resource*)_buffer);
	if (!buffer)
		return NULL;

	// XXX: BO handle is imported in wayland-kms.
	if (!(bo = calloc(1, sizeof(struct gbm_kms_bo))))
		return NULL;

	bo->base.gbm = gbm;

	bo->base.width = buffer->width;
	bo->base.height = buffer->height;
	bo->base.format = buffer->format;
	bo->base.stride = buffer->stride;
	bo->base.handle.u32 = buffer->handle;

	if (buffer->num_planes > 0 && buffer->num_planes <= MAX_PLANES) {
		int i;
		bo->num_planes = buffer->num_planes;
		for (i = 0; i < buffer->num_planes; i++)  {
			bo->planes[i].handle = buffer->planes[i].handle;
			bo->planes[i].stride = buffer->planes[i].stride;
		}
	} else {
		bo->num_planes = 1;
	}

	return bo;
}

static struct gbm_kms_bo* gbm_kms_import_fd(struct gbm_device *gbm,
					    void *_buffer)
{
	struct gbm_import_fd_data *fd_data = _buffer;
	struct gbm_kms_device *dev = (struct gbm_kms_device*)gbm;
	struct gbm_kms_bo *bo;
	uint32_t handle;

	if (drmPrimeFDToHandle(dev->base.base.fd, fd_data->fd, &handle)) {
		GBM_DEBUG("%s: %s: drmPrimeFDToHandle() failed. %s\n",
			  __FILE__, __func__, strerror(errno));
		return NULL;
	}

	// XXX: BO handle is imported in wayland-kms.
	if (!(bo = calloc(1, sizeof(struct gbm_kms_bo))))
		return NULL;

	bo->base.gbm = gbm;
	bo->base.width = fd_data->width;
	bo->base.height = fd_data->height;
	bo->base.format = fd_data->format;
	bo->base.stride = fd_data->stride;
	bo->base.handle.u32 = handle;
	bo->num_planes = 1;

	return bo;
}

static struct gbm_bo *gbm_kms_bo_import(struct gbm_device *gbm,
					uint32_t type, void *_buffer,
					uint32_t usage)
{
	struct gbm_kms_bo *bo = NULL;

	/*
	 * We need to  import wl_buffer/gbm_import_fd_data for Weston,
	 * i.e. client's rendering buffer.
	 *
	 * To make gbm_kms backend to be agnostic to none-DRM/KMS as much as
	 * possible, wl_buffer that we receive must be DRM BO, nothing else.
	 * We are already kind of wayland dependent which is enough.
	 */
	switch (type) {
	case GBM_BO_IMPORT_WL_BUFFER:
		bo = gbm_kms_import_wl_buffer(gbm, _buffer);
		break;
	case GBM_BO_IMPORT_FD:
		bo = gbm_kms_import_fd(gbm, _buffer);
		break;
	default:
		GBM_DEBUG("%s: invalid type = %d\n", __func__, type);
		break;
	}

	return (struct gbm_bo*)bo;
}

static int
gbm_kms_bo_get_planes(struct gbm_bo *_bo)
{
	return 1;
}

static union gbm_bo_handle
gbm_kms_bo_get_handle_for_plane(struct gbm_bo *_bo, int plane)
{
	return _bo->handle;
}

static uint32_t
gbm_kms_bo_get_stride(struct gbm_bo *_bo, int plane)
{
	return _bo->stride;
}

static uint32_t
gbm_kms_bo_get_offset(struct gbm_bo *_bo, int plane)
{
	return 0;
}

static uint64_t
gbm_kms_bo_get_modifier(struct gbm_bo *_bo)
{
	errno = ENOSYS;
	return DRM_FORMAT_MOD_INVALID;
}

static int gbm_kms_surface_set_bo(struct gbm_kms_surface *surface, int n, void *addr, int fd, uint32_t stride)
{
	struct gbm_kms_bo *bo;

	if (n < 0 || n > 1)
		return -1;

	if (surface->bo[n])
		free(surface->bo[n]);

	if (addr == NULL && stride == 0)
		return 0;

	if (!(bo = calloc(1, sizeof(struct gbm_kms_bo))))
		return -1;

	bo->base.gbm = surface->base.gbm;
	bo->base.width = surface->base.width;
	bo->base.height = surface->base.height;
	bo->base.format = surface->base.format;
	bo->base.stride = stride;
	bo->size = stride * surface->base.height;
	bo->addr = addr;
	bo->fd = fd;
	bo->allocated = false;

	surface->bo[n] = bo;

	return 0;
}

static void gbm_kms_surface_destroy(struct gbm_surface *_surface);

static struct gbm_surface *
gbm_kms_surface_create(struct gbm_device *gbm,
		       uint32_t width, uint32_t height,
		       uint32_t format, uint32_t flags,
		       const uint64_t *modifiers, const unsigned count)
{
	struct gbm_kms_surface *surface;
	GBM_DEBUG("%s: %s: %d\n", __FILE__, __func__, __LINE__);

	if (!(surface = calloc(1, sizeof(struct gbm_kms_surface))))
		return NULL;

	surface->base.gbm = gbm;
	surface->base.width = width;
	surface->base.height = height;
	surface->base.format = format;
	surface->base.flags = flags;

	GBM_DEBUG("%s: %s: %d: created surface %dx%d\n", __FILE__, __func__, __LINE__, width, height);
	surface->front = -1;
	surface->set_bo = gbm_kms_surface_set_bo;

	return (struct gbm_surface*)surface;
}

static void gbm_kms_surface_destroy(struct gbm_surface *_surface)
{
	struct gbm_kms_surface *surface = (struct gbm_kms_surface*)_surface;

	if (!surface)
		return;

	free(surface);
}

static struct gbm_bo *gbm_kms_surface_lock_front_buffer(struct gbm_surface *_surface)
{
	struct gbm_kms_surface *surface = (struct gbm_kms_surface*)_surface;

	/*
	 * Drm-compositor in the weston server relies on this API. After
	 * composing clients' surfaces with gl-renderer, drm-compositor locks
	 * the gbm_surface and queries BO attached to it with this API.
	 * Drm-compositor then sets it to DRM/KMS with drmModeAddFB() and
	 * drmModeAddFB2().
	 */

	if (surface->front >= 0) {
		struct gbm_kms_bo *front = surface->bo[surface->front];
		front->locked = 1;
		return (struct gbm_bo*)front;
	}

	return NULL;
}

static void gbm_kms_surface_release_buffer(struct gbm_surface *_surface, struct gbm_bo *_bo)
{
	struct gbm_kms_bo *bo = (struct gbm_kms_bo*)_bo;
	bo->locked = 0;
	return;
}

static int gbm_kms_surface_has_free_buffers(struct gbm_surface *_surface)
{
	struct gbm_kms_surface *surface = (struct gbm_kms_surface*)_surface;
	return ((!surface->bo[0]->locked) || (!surface->bo[1]->locked));
}

struct gbm_device kms_gbm_device = {
	.name = "kms",

	.destroy = gbm_kms_destroy,
	.is_format_supported = gbm_kms_is_format_supported,
	.get_format_modifier_plane_count = gbm_kms_get_format_modifier_plane_count,

	.bo_create = gbm_kms_bo_create,
	.bo_import = gbm_kms_bo_import,
	.bo_write = gbm_kms_bo_write,
	.bo_destroy = gbm_kms_bo_destroy,
	.bo_get_planes = gbm_kms_bo_get_planes,
	.bo_get_handle = gbm_kms_bo_get_handle_for_plane,
	.bo_get_stride = gbm_kms_bo_get_stride,
	.bo_get_offset = gbm_kms_bo_get_offset,
	.bo_get_modifier = gbm_kms_bo_get_modifier,

	.surface_create = gbm_kms_surface_create,
	.surface_lock_front_buffer = gbm_kms_surface_lock_front_buffer,
	.surface_release_buffer = gbm_kms_surface_release_buffer,
	.surface_has_free_buffers = gbm_kms_surface_has_free_buffers,
	.surface_destroy = gbm_kms_surface_destroy,
};

static struct gbm_device *kms_device_create(int fd)
{
	struct gbm_kms_device *dev;

	GBM_DEBUG("%s: %d\n", __func__, __LINE__);

	if (!(dev = calloc(1, sizeof(struct gbm_kms_device))))
		return NULL;

	dev->base.type = GBM_DRM_DRIVER_TYPE_CUSTOM;

	dev->base.base = kms_gbm_device;
	dev->base.base.fd = fd;

	if (kms_create(fd, &dev->kms)) {
		free(dev);
		return NULL;
	}

	return &dev->base.base;
}

/* backend loader looks for symbol "gbm_backend" */
struct gbm_backend gbm_backend = {
	.backend_name = "kms",
	.create_device = kms_device_create,
};
