/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define LOG_TAG "GRALLOC-PIPE"

#include <cutils/log.h>
#include <errno.h>
#include <dlfcn.h>

#include <pipe/p_screen.h>
#include <pipe/p_context.h>
#include <state_tracker/winsys_handle.h>
#include <util/u_inlines.h>
#include <util/u_memory.h>

#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

#if defined(__LP64__)
#define DRI_LIBRARY_PATH "/vendor/lib64/dri"
#else
#define DRI_LIBRARY_PATH "/vendor/lib/dri"
#endif

struct pipe_manager {
	struct gralloc_drm_drv_t base;

	int fd;
	void *gallium;
	pthread_mutex_t mutex;
	struct pipe_loader_device *dev;
	struct pipe_screen *screen;
	struct pipe_context *context;
};

struct pipe_buffer {
	struct gralloc_drm_bo_t base;

	struct pipe_resource *resource;
	struct winsys_handle winsys;

	struct pipe_transfer *transfer;
};

static enum pipe_format get_pipe_format(int format)
{
	enum pipe_format fmt;

	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
		fmt = PIPE_FORMAT_R8G8B8A8_UNORM;
		break;
	case HAL_PIXEL_FORMAT_RGBX_8888:
		fmt = PIPE_FORMAT_R8G8B8X8_UNORM;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		fmt = PIPE_FORMAT_R8G8B8_UNORM;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
		fmt = PIPE_FORMAT_B5G6R5_UNORM;
		break;
	case HAL_PIXEL_FORMAT_BGRA_8888:
		fmt = PIPE_FORMAT_B8G8R8A8_UNORM;
		break;
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_DRM_NV12:
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
	default:
		fmt = PIPE_FORMAT_NONE;
		break;
	}

	return fmt;
}

static unsigned get_pipe_bind(int usage)
{
	unsigned bind = PIPE_BIND_SHARED;

	if (usage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN))
		bind |= PIPE_BIND_LINEAR;
	if (usage & GRALLOC_USAGE_HW_TEXTURE)
		bind |= PIPE_BIND_SAMPLER_VIEW;
	if (usage & GRALLOC_USAGE_HW_RENDER)
		bind |= PIPE_BIND_RENDER_TARGET;
	if (usage & GRALLOC_USAGE_HW_FB) {
		bind |= PIPE_BIND_RENDER_TARGET;
		bind |= PIPE_BIND_SCANOUT;
	}

	return bind;
}

static struct pipe_buffer *get_pipe_buffer_locked(struct pipe_manager *pm,
		const struct gralloc_drm_handle_t *handle)
{
	struct pipe_buffer *buf;
	struct pipe_resource templ;

	memset(&templ, 0, sizeof(templ));
	templ.format = get_pipe_format(handle->format);
	templ.bind = get_pipe_bind(handle->usage);
	templ.target = PIPE_TEXTURE_2D;

	if (templ.format == PIPE_FORMAT_NONE ||
	    !pm->screen->is_format_supported(pm->screen, templ.format,
				templ.target, 0, 0, templ.bind)) {
		ALOGE("unsupported format 0x%x", handle->format);
		return NULL;
	}

	buf = CALLOC(1, sizeof(*buf));
	if (!buf) {
		ALOGE("failed to allocate pipe buffer");
		return NULL;
	}

	templ.width0 = handle->width;
	templ.height0 = handle->height;
	templ.depth0 = 1;
	templ.array_size = 1;

	if (handle->name) {
		buf->winsys.type = WINSYS_HANDLE_TYPE_SHARED;
		buf->winsys.handle = handle->name;
		buf->winsys.stride = handle->stride;

		buf->resource = pm->screen->resource_from_handle(pm->screen,
				&templ, &buf->winsys, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
		if (!buf->resource)
			goto fail;
	}
	else {
		buf->resource =
			pm->screen->resource_create(pm->screen, &templ);
		if (!buf->resource)
			goto fail;

		buf->winsys.type = WINSYS_HANDLE_TYPE_SHARED;
		if (!pm->screen->resource_get_handle(pm->screen, pm->context,
				buf->resource, &buf->winsys, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
			goto fail;
	}

	/* need the gem handle for fb */
	if (handle->usage & GRALLOC_USAGE_HW_FB) {
		struct winsys_handle tmp;

		memset(&tmp, 0, sizeof(tmp));
		tmp.type = WINSYS_HANDLE_TYPE_KMS;
		if (!pm->screen->resource_get_handle(pm->screen, pm->context,
				buf->resource, &tmp, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
			goto fail;

		buf->base.fb_handle = tmp.handle;
	}

	return buf;

fail:
	ALOGE("failed to allocate pipe buffer");
	if (buf->resource)
		pipe_resource_reference(&buf->resource, NULL);
	FREE(buf);

	return NULL;
}

static struct gralloc_drm_bo_t *pipe_alloc(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_handle_t *handle)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;
	struct pipe_buffer *buf;

	pthread_mutex_lock(&pm->mutex);
	buf = get_pipe_buffer_locked(pm, handle);
	pthread_mutex_unlock(&pm->mutex);

	if (buf) {
		handle->name = (int) buf->winsys.handle;
		handle->stride = (int) buf->winsys.stride;

		buf->base.handle = handle;
	}

	return &buf->base;
}

static void pipe_free(struct gralloc_drm_drv_t *drv, struct gralloc_drm_bo_t *bo)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;
	struct pipe_buffer *buf = (struct pipe_buffer *) bo;

	pthread_mutex_lock(&pm->mutex);

	if (buf->transfer)
		pipe_transfer_unmap(pm->context, buf->transfer);
	pipe_resource_reference(&buf->resource, NULL);

	pthread_mutex_unlock(&pm->mutex);

	FREE(buf);
}

static int pipe_map(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo, int x, int y, int w, int h,
		int enable_write, void **addr)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;
	struct pipe_buffer *buf = (struct pipe_buffer *) bo;
	int err = 0;

	pthread_mutex_lock(&pm->mutex);

	/* need a context to get transfer */
	if (!pm->context) {
		pm->context = pm->screen->context_create(pm->screen, NULL, 0);
		if (!pm->context) {
			ALOGE("failed to create pipe context");
			err = -ENOMEM;
		}
	}

	if (!err) {
		enum pipe_transfer_usage usage;

		usage = PIPE_TRANSFER_READ;
		if (enable_write)
			usage |= PIPE_TRANSFER_WRITE;

		assert(!buf->transfer);

		/*
		 * ignore x, y, w and h so that returned addr points at the
		 * start of the buffer
		 */
		*addr = pipe_transfer_map(pm->context, buf->resource,
					  0, 0, usage, 0, 0,
					  buf->resource->width0, buf->resource->height0,
					  &buf->transfer);
		if (*addr == NULL)
			err = -ENOMEM;
	}

	pthread_mutex_unlock(&pm->mutex);

	return err;
}

static void pipe_unmap(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;
	struct pipe_buffer *buf = (struct pipe_buffer *) bo;

	pthread_mutex_lock(&pm->mutex);

	assert(buf && buf->transfer);

	pipe_transfer_unmap(pm->context, buf->transfer);
	buf->transfer = NULL;

	pm->context->flush(pm->context, NULL, 0);

	pthread_mutex_unlock(&pm->mutex);
}

static void pipe_blit(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *dst_bo,
		struct gralloc_drm_bo_t *src_bo,
		uint16_t dst_x1, uint16_t dst_y1,
		uint16_t dst_x2, uint16_t dst_y2,
		uint16_t src_x1, uint16_t src_y1,
		uint16_t src_x2, uint16_t src_y2)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;
	struct pipe_buffer *dst = (struct pipe_buffer *) dst_bo;
	struct pipe_buffer *src = (struct pipe_buffer *) src_bo;
	struct pipe_box src_box;

	if (dst_bo->handle->width != src_bo->handle->width ||
	    dst_bo->handle->height != src_bo->handle->height ||
	    dst_bo->handle->stride != src_bo->handle->stride ||
	    dst_bo->handle->format != src_bo->handle->format) {
		ALOGE("copy between incompatible buffers");
		return;
	}

	if (dst_x2 > dst_bo->handle->width)
		dst_x2 = dst_bo->handle->width;
	if (dst_y2 > dst_bo->handle->height)
		dst_y2 = dst_bo->handle->height;

	if (src_x2 <= src_x1 || src_y2 <= src_y1)
		return;

	u_box_2d(src_x1, src_y1, src_x2 - src_x1, src_y2 - src_y1, &src_box);

	pthread_mutex_lock(&pm->mutex);

	/* need a context for copying */
	if (!pm->context) {
		pm->context = pm->screen->context_create(pm->screen, NULL, 0);
		if (!pm->context) {
			ALOGE("failed to create pipe context");
			pthread_mutex_unlock(&pm->mutex);
			return;
		}
	}

	pm->context->resource_copy_region(pm->context,
			dst->resource, 0, dst_x1, dst_y1, 0,
			src->resource, 0, &src_box);
	pm->context->flush(pm->context, NULL, 0);

	pthread_mutex_unlock(&pm->mutex);
}

static void pipe_init_kms_features(struct gralloc_drm_drv_t *drv, struct gralloc_drm_t *drm)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;

	switch (drm->primary->fb_format) {
	case HAL_PIXEL_FORMAT_BGRA_8888:
	case HAL_PIXEL_FORMAT_RGB_565:
		break;
	default:
		drm->primary->fb_format = HAL_PIXEL_FORMAT_BGRA_8888;
		break;
	}

	drm->mode_quirk_vmwgfx = 0;
	drm->swap_mode = DRM_SWAP_FLIP;
	drm->mode_sync_flip = 1;
	drm->swap_interval = 1;
	drm->vblank_secondary = 0;
}

static void pipe_destroy(struct gralloc_drm_drv_t *drv)
{
	struct pipe_manager *pm = (struct pipe_manager *) drv;

	if (pm->context)
		pm->context->destroy(pm->context);
	pm->screen->destroy(pm->screen);
	dlclose(pm->gallium);
	FREE(pm);
}

struct gralloc_drm_drv_t *gralloc_drm_drv_create_for_pipe(int fd, const char *name)
{
	struct pipe_manager *pm;
	struct pipe_screen *(*load_pipe_screen)(struct pipe_loader_device **dev, int fd);

	pm = CALLOC(1, sizeof(*pm));
	if (!pm) {
		ALOGE("failed to allocate pipe manager for %s", name);
		return NULL;
	}

	pm->fd = fd;
	pthread_mutex_init(&pm->mutex, NULL);

	pm->gallium = dlopen(DRI_LIBRARY_PATH"/gallium_dri.so", RTLD_NOW | RTLD_GLOBAL);
	if (!pm->gallium)
		goto err_open;

	load_pipe_screen = dlsym(pm->gallium, "load_pipe_screen");
	if (!load_pipe_screen)
		goto err_load;

	pm->screen = load_pipe_screen(&pm->dev, fd);
	if (!pm->screen)
		goto err_load;

	pm->base.destroy = pipe_destroy;
	pm->base.init_kms_features = pipe_init_kms_features;
	pm->base.alloc = pipe_alloc;
	pm->base.free = pipe_free;
	pm->base.map = pipe_map;
	pm->base.unmap = pipe_unmap;
	pm->base.blit = pipe_blit;

	return &pm->base;

err_load:
	dlclose(pm->gallium);
err_open:
	FREE(pm);
	return NULL;
}
