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
#include <map>

#include "grallocbufferhandler.h"
#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

std::map<gralloc_drm_bo_t *, buffer_handle_t *> all_records;

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

static int drm_mod_create_buffer(struct drm_module_t *dmod,
		int w, int h, int format, int usage,
		buffer_handle_t *handle, int *stride)
{
	struct gralloc_drm_bo_t *bo;
	int size, bpp, err;

	if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
		ALOGV("Convert implementation defined format to ARGB8888 w:%d, h:%d, usage:0x%x",
			w, h, usage);
		format = HAL_PIXEL_FORMAT_RGBA_8888;
	}

	bpp = gralloc_drm_get_bpp(format);
	if (!bpp)
		return -EINVAL;

	bo = gralloc_drm_bo_create(dmod->drm, w, h, format, usage);
	if (!bo)
		return -ENOMEM;

	*handle = gralloc_drm_bo_get_handle(bo, stride);
	/* in pixels */
	*stride /= bpp;

	all_records.insert(std::make_pair(bo, handle));

	return 0;
}

static int drm_mod_destroy_buffer(buffer_handle_t handle)
{
	struct gralloc_drm_bo_t *bo;

	bo = gralloc_drm_bo_from_handle(handle);
	if (!bo)
		return -EINVAL;

	gralloc_drm_bo_decref(bo);

	std::map<gralloc_drm_bo_t *,
		buffer_handle_t *>::const_iterator it = all_records.find(bo);
	if (it == all_records.end())
		return -EINVAL;
	all_records.erase(it);

	return 0;
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
	case static_cast<int>(GRALLOC_MODULE_PERFORM_DRM_IMPORT):
		{
			int fd = va_arg(args, int);
			buffer_handle_t handle = va_arg(args, buffer_handle_t);
			struct HwcBuffer *hwc_bo = va_arg(args, struct HwcBuffer *);

			gralloc_drm_handle_t *gr_handle = gralloc_drm_handle(handle);

			if (!gr_handle) {
				ALOGE("could not find gralloc drm handle");
				err = -EINVAL;
				break;
			}

			/* call driver to resolve HwcBuffer */
			if (dmod->drm->drv->resolve_buffer)
				err = dmod->drm->drv->resolve_buffer(dmod->drm->drv, fd, gr_handle, hwc_bo);
			else
				err = -EINVAL;
		}
		break;
	case static_cast<int>(GRALLOC_MODULE_PERFORM_CREATE_BUFFER):
		{
			uint32_t width = va_arg(args, uint32_t);
			uint32_t height = va_arg(args, uint32_t);
			int format = va_arg(args, int);
			int usage = va_arg(args, int);
			buffer_handle_t* handle = va_arg(args, buffer_handle_t*);
			int stride;
			err = drm_mod_create_buffer(dmod, width, height, format, usage, handle, &stride);
		}
		break;
	case static_cast<int>(GRALLOC_MODULE_PERFORM_DESTROY_BUFFER):
		{
			buffer_handle_t handle = va_arg(args, buffer_handle_t);
			err = drm_mod_destroy_buffer(handle);
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

	return gralloc_drm_handle_register(handle, dmod->drm);
}

static int drm_mod_unregister_buffer(const gralloc_module_t *mod,
		buffer_handle_t handle)
{
	return gralloc_drm_handle_unregister(handle);
}

static int drm_mod_lock(const gralloc_module_t *mod, buffer_handle_t handle,
		int usage, int x, int y, int w, int h, void **ptr)
{
	struct gralloc_drm_bo_t *bo;
	int err;

	bo = gralloc_drm_bo_from_handle(handle);
	if (!bo)
		return -EINVAL;

	return gralloc_drm_bo_lock(bo, usage, x, y, w, h, ptr);
}

static int drm_mod_lock_ycbcr(const gralloc_module_t *mod, buffer_handle_t bhandle,
		int usage, int x, int y, int w, int h, struct android_ycbcr *ycbcr)
{
	struct gralloc_drm_handle_t *handle;
	struct gralloc_drm_bo_t *bo;
	void *ptr = NULL;
	int err;

	bo = gralloc_drm_bo_from_handle(bhandle);
	if (!bo)
		return -EINVAL;
	handle = bo->handle;

	switch(handle->format) {
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
		break;
	default:
		return -EINVAL;
	}

	if (usage != 0) {
		err = gralloc_drm_bo_lock(bo, usage, x, y, w, h, &ptr);
		if (err)
			return err;
	}

	uint32_t pitches[4];
	uint32_t offsets[4];
	uint32_t handles[4];
	gralloc_drm_resolve_format(bhandle, pitches, offsets, handles);

	switch(handle->format) {
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
		ycbcr->y = ptr;
		ycbcr->cb = (uint8_t *)ptr + (offsets[1] - offsets[2]);
		ycbcr->cr = (uint8_t *)ptr + (offsets[2] - offsets[0]);
		ycbcr->ystride = pitches[0];
		ycbcr->cstride = pitches[1];
		ycbcr->chroma_step = 1;
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

	bo = gralloc_drm_bo_from_handle(handle);
	if (!bo)
		return -EINVAL;

	gralloc_drm_bo_unlock(bo);

	return 0;
}

static int drm_mod_close_gpu0(struct hw_device_t *dev)
{
	struct drm_module_t *dmod = (struct drm_module_t *)dev->module;
	struct alloc_device_t *alloc = (struct alloc_device_t *) dev;

	gralloc_drm_destroy(dmod->drm);
	delete alloc;

	return 0;
}

static int drm_mod_free_gpu0(alloc_device_t */*dev*/, buffer_handle_t handle)
{
	return drm_mod_destroy_buffer(handle);
}

static int drm_mod_alloc_gpu0(alloc_device_t *dev,
		int w, int h, int format, int usage,
		buffer_handle_t *handle, int *stride)
{
	struct drm_module_t *dmod = (struct drm_module_t *) dev->common.module;
	return drm_mod_create_buffer(dmod, w, h, format, usage, handle, stride);
}

static void drm_mod_dump_gpu0(struct alloc_device_t * /*dev*/, char *buff, int buff_len)
{
	int used = 0;

	used += snprintf(buff+used, buff_len-used, "dump all buffer objects info:\n");

	for(std::map<gralloc_drm_bo_t *, buffer_handle_t *>::iterator it=all_records.begin();
			it!=all_records.end(); ++it) {
		used += snprintf(buff+used, buff_len-used, "bo: %p, handle: %p, width: %d,"
			" height: %d, format: %x, usage: %x\n", (*it).first, (*it).second,
			(*it).first->handle->width, (*it).first->handle->height,
			(*it).first->handle->format, (*it).first->handle->usage);
		if (used >= buff_len)
			return;
	}

	return;
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
	alloc->dump = drm_mod_dump_gpu0;

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
