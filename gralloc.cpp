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

#define LOG_TAG "GRALLOC-MOD"

#include <cutils/log.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>

#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

static pthread_mutex_t gralloc_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Initialize the DRM device object
 */
static int drm_init(struct drm_module_t *dmod)
{
	int err = 0;

	pthread_mutex_lock(&dmod->mutex);
	if (!dmod->drm) {
		dmod->drm = gralloc_drm_create();
		if (!dmod->drm)
			err = -EINVAL;
	}
	pthread_mutex_unlock(&dmod->mutex);

	return err;
}

static int drm_mod_perform(const struct gralloc_module_t *mod, int op, ...)
{
	struct drm_module_t *dmod = (struct drm_module_t *) mod;
	va_list args;
	int err;

	err = drm_init(dmod);
	if (err)
		return err;

	va_start(args, op);
	switch (op) {
	case static_cast<int>(GRALLOC_MODULE_PERFORM_GET_DRM_FD):
		{
			int *fd = va_arg(args, int *);
			*fd = gralloc_drm_get_fd(dmod->drm);
			err = 0;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	va_end(args);

	return err;
}

static int drm_mod_register_buffer(const gralloc_module_t *mod,
		buffer_handle_t handle)
{
	struct drm_module_t *dmod = (struct drm_module_t *) mod;
	int err;

	err = drm_init(dmod);
	if (err)
		return err;

	pthread_mutex_lock(&gralloc_lock);
	err = gralloc_drm_handle_register(handle, dmod->drm);
	pthread_mutex_unlock(&gralloc_lock);

	return err;
}

static int drm_mod_unregister_buffer(const gralloc_module_t *mod,
		buffer_handle_t handle)
{
	int err;

	pthread_mutex_lock(&gralloc_lock);
	err = gralloc_drm_handle_unregister(handle);
	pthread_mutex_unlock(&gralloc_lock);

	return err;
}

/*
 * Lock a bo.  XXX thread-safety?
 */
int gralloc_drm_bo_lock(struct gralloc_drm_bo_t *bo,
		uint32_t usage, int x, int y, int w, int h,
		void **addr)
{
	if ((bo->handle->usage & usage) != usage) {
		/* make FB special for testing software renderer with */
		if (!(bo->handle->usage & (
				GRALLOC_USAGE_SW_READ_OFTEN |
				GRALLOC_USAGE_HW_FB |
				GRALLOC_USAGE_HW_TEXTURE |
				GRALLOC_USAGE_HW_VIDEO_ENCODER))) {
			ALOGE("bo.usage:x%X/usage:x%X is not GRALLOC_USAGE_HW_{FB,TEXTURE,VIDEO_ENCODER}",
					bo->handle->usage, usage);
			return -EINVAL;
		}
	}

	/* allow multiple locks with compatible usages */
	if (bo->lock_count && (bo->locked_for & usage) != usage)
		return -EINVAL;

	usage |= bo->locked_for;

	if (usage & (GRALLOC_USAGE_SW_WRITE_MASK |
		     GRALLOC_USAGE_SW_READ_MASK)) {
		/* the driver is supposed to wait for the bo */
		int write = !!(usage & GRALLOC_USAGE_SW_WRITE_MASK);
		int err = bo->drm->drv->map(bo->drm->drv, bo,
				x, y, w, h, write, addr);
		if (err)
			return err;
	}
	else {
		/* kernel handles the synchronization here */
	}

	bo->lock_count++;
	bo->locked_for |= usage;

	return 0;
}

/*
 * Unlock a bo.
 */
void gralloc_drm_bo_unlock(struct gralloc_drm_bo_t *bo)
{
	int mapped = bo->locked_for &
		(GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK);

	if (!bo->lock_count)
		return;

	if (mapped)
		bo->drm->drv->unmap(bo->drm->drv, bo);

	bo->lock_count--;
	if (!bo->lock_count)
		bo->locked_for = 0;
}

static int drm_mod_lock(const gralloc_module_t *mod, buffer_handle_t handle,
		int usage, int x, int y, int w, int h, void **ptr)
{
	struct gralloc_drm_bo_t *bo;
	int err;

	pthread_mutex_lock(&gralloc_lock);

	bo = gralloc_drm_bo_from_handle(handle);
	if (!bo) {
		err = -EINVAL;
		goto unlock;
	}

	err = gralloc_drm_bo_lock(bo, usage, x, y, w, h, ptr);

unlock:
	pthread_mutex_unlock(&gralloc_lock);
	return err;
}

static int drm_mod_lock_ycbcr(const gralloc_module_t *mod, buffer_handle_t bhandle,
		int usage, int x, int y, int w, int h, struct android_ycbcr *ycbcr)
{
	struct gralloc_drm_handle_t *handle;
	struct gralloc_drm_bo_t *bo;
	void *ptr;
	int err;

	bo = gralloc_drm_bo_from_handle(bhandle);
	if (!bo)
		return -EINVAL;
	handle = bo->handle;

	switch(handle->format) {
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
		break;
	default:
		return -EINVAL;
	}

	err = gralloc_drm_bo_lock(bo, usage, x, y, w, h, &ptr);
	if (err)
		return err;

	switch(handle->format) {
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
		ycbcr->y = ptr;
		ycbcr->cb = (uint8_t *)ptr + handle->stride * handle->height;
		ycbcr->cr = (uint8_t *)ycbcr->cb + 1;
		ycbcr->ystride = handle->stride;
		ycbcr->cstride = handle->stride;
		ycbcr->chroma_step = 2;
		break;
	default:
		break;
	}

	return 0;
}

static int drm_mod_unlock(const gralloc_module_t *mod, buffer_handle_t handle)
{
	struct drm_module_t *dmod = (struct drm_module_t *) mod;
	struct gralloc_drm_bo_t *bo;
	int err = 0;

	pthread_mutex_lock(&gralloc_lock);

	bo = gralloc_drm_bo_from_handle(handle);
	if (!bo) {
		err = -EINVAL;
		goto unlock;
	}

	gralloc_drm_bo_unlock(bo);

unlock:
	pthread_mutex_unlock(&gralloc_lock);
	return err;
}

static int drm_mod_close_gpu0(struct hw_device_t *dev)
{
	struct drm_module_t *dmod = (struct drm_module_t *)dev->module;
	struct alloc_device_t *alloc = (struct alloc_device_t *) dev;

	gralloc_drm_destroy(dmod->drm);
	delete alloc;

	return 0;
}

static int drm_mod_free_gpu0(alloc_device_t *dev, buffer_handle_t handle)
{
	struct drm_module_t *dmod = (struct drm_module_t *) dev->common.module;
	struct gralloc_drm_bo_t *bo;
	int err = 0;

	pthread_mutex_lock(&gralloc_lock);

	bo = gralloc_drm_bo_from_handle(handle);
	if (!bo) {
		err = -EINVAL;
		goto unlock;
	}

	gralloc_drm_bo_decref(bo);

unlock:
	pthread_mutex_unlock(&gralloc_lock);
	return err;
}

static int drm_mod_alloc_gpu0(alloc_device_t *dev,
		int w, int h, int format, int usage,
		buffer_handle_t *handle, int *stride)
{
	struct drm_module_t *dmod = (struct drm_module_t *) dev->common.module;
	struct gralloc_drm_bo_t *bo;
	int size, bpp, err = 0;

	bpp = gralloc_drm_get_bpp(format);
	if (!bpp)
		return -EINVAL;

	pthread_mutex_lock(&gralloc_lock);

	bo = gralloc_drm_bo_create(dmod->drm, w, h, format, usage);
	if (!bo) {
		err = -ENOMEM;
		goto unlock;
	}

	*handle = gralloc_drm_bo_get_handle(bo, stride);
	/* in pixels */
	*stride /= bpp;

unlock:
	pthread_mutex_unlock(&gralloc_lock);
	return err;
}

static int drm_mod_open_gpu0(struct drm_module_t *dmod, hw_device_t **dev)
{
	struct alloc_device_t *alloc;
	int err;

	err = drm_init(dmod);
	if (err)
		return err;

	alloc = new alloc_device_t;
	if (!alloc)
		return -EINVAL;

	alloc->common.tag = HARDWARE_DEVICE_TAG;
	alloc->common.version = 0;
	alloc->common.module = &dmod->base.common;
	alloc->common.close = drm_mod_close_gpu0;

	alloc->alloc = drm_mod_alloc_gpu0;
	alloc->free = drm_mod_free_gpu0;

	*dev = &alloc->common;

	return 0;
}

static int drm_mod_open(const struct hw_module_t *mod,
		const char *name, struct hw_device_t **dev)
{
	struct drm_module_t *dmod = (struct drm_module_t *) mod;
	int err;

	if (strcmp(name, GRALLOC_HARDWARE_GPU0) == 0)
		err = drm_mod_open_gpu0(dmod, dev);
	else
		err = -EINVAL;

	return err;
}

static struct hw_module_methods_t drm_mod_methods = {
	.open = drm_mod_open
};

struct drm_module_t HAL_MODULE_INFO_SYM = {
	.base = {
		.common = {
			.tag = HARDWARE_MODULE_TAG,
			.version_major = 1,
			.version_minor = 0,
			.id = GRALLOC_HARDWARE_MODULE_ID,
			.name = "DRM Memory Allocator",
			.author = "Chia-I Wu",
			.methods = &drm_mod_methods
		},
		.registerBuffer = drm_mod_register_buffer,
		.unregisterBuffer = drm_mod_unregister_buffer,
		.lock = drm_mod_lock,
		.unlock = drm_mod_unlock,
		.perform = drm_mod_perform,
		.lock_ycbcr = drm_mod_lock_ycbcr,
	},

	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.drm = NULL
};
