/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "drv_helpers.h"
#include "drv_priv.h"
#include "util.h"

#define INIT_DUMB_DRIVER_WITH_NAME(driver, _name)                                                  \
	const struct backend backend_##driver = {                                                  \
		.name = _name,                                                                     \
		.init = dumb_driver_init,                                                          \
		.bo_create = drv_dumb_bo_create,                                                   \
		.bo_create_with_modifiers = dumb_bo_create_with_modifiers,                         \
		.bo_destroy = drv_dumb_bo_destroy,                                                 \
		.bo_import = drv_prime_bo_import,                                                  \
		.bo_map = drv_dumb_bo_map,                                                         \
		.bo_unmap = drv_bo_munmap,                                                         \
		.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,           \
	};

#define INIT_DUMB_DRIVER(driver) INIT_DUMB_DRIVER_WITH_NAME(driver, #driver)

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
						   DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
						   DRM_FORMAT_BGR888,	DRM_FORMAT_RGB565 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_R8, DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };

static int dumb_driver_init(struct driver *drv)
{
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &LINEAR_METADATA, BO_USE_TEXTURE_MASK);

	drv_modify_combination(drv, DRM_FORMAT_R8, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
				   BO_USE_GPU_DATA_BUFFER | BO_USE_SENSOR_DIRECT_DATA);
	drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	drv_modify_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA, BO_USE_HW_VIDEO_ENCODER);

	return drv_modify_linear_combinations(drv);
}

static int dumb_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					 uint32_t format, const uint64_t *modifiers, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (modifiers[i] == DRM_FORMAT_MOD_LINEAR) {
			return drv_dumb_bo_create(bo, width, height, format, 0);
		}
	}

	return -EINVAL;
}

INIT_DUMB_DRIVER(evdi)
INIT_DUMB_DRIVER(komeda)
INIT_DUMB_DRIVER(marvell)
INIT_DUMB_DRIVER(meson)
INIT_DUMB_DRIVER(nouveau)
INIT_DUMB_DRIVER(radeon)
INIT_DUMB_DRIVER_WITH_NAME(sun4i_drm, "sun4i-drm")
INIT_DUMB_DRIVER(synaptics)
INIT_DUMB_DRIVER(udl)
INIT_DUMB_DRIVER(vkms)

#ifndef DRV_VC4
/*
 * BCM2711/BCM2712 (Raspberry Pi 4/5): vc4 is KMS-only and rejects VC4 BO ioctls
 * (-ENODEV on gen > VC4_GEN_4), so buffers are allocated as dumb buffers on the vc4
 * primary node.
 *
 * Buffers used only by the GPU (texture and/or render target, without CPU, scanout,
 * composer or video use) are laid out in the Broadcom UIF tiled format. The v3d GPU
 * cannot sample raster (linear) textures, so Mesa copies a linear buffer into a tiled
 * shadow before every draw that samples it; for imported buffers such as hardware
 * bitmaps that copy is repeated on every draw. The display (HVS) cannot scan out UIF,
 * so every other buffer stays linear.
 */
static const uint32_t vc4_uif_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888 };

/*
 * UIF layout for 32-bit formats, matching Mesa's v3d driver (v3d_setup_slices() and
 * v3d_get_ub_pad() in src/gallium/drivers/v3d/v3d_resource.c): a UIF block is 2x2
 * utiles of 4x4 pixels, the width is aligned to a column of 4 UIF blocks, and the
 * height is padded so that pages of the same bank stay apart across columns.
 */
#define VC4_UIF_BLOCK_SIZE_PX 8
#define VC4_UIF_COLUMN_WIDTH_PX (4 * VC4_UIF_BLOCK_SIZE_PX)
/* 1.5 pages of 4096 bytes, in 1024-byte rows of UIF blocks. */
#define VC4_UIF_PAGE_UB_ROWS_TIMES_1_5 6
/* Page cache of 8 banks x 4096 bytes, in 1024-byte rows of UIF blocks. */
#define VC4_UIF_PAGE_CACHE_UB_ROWS 32

static uint32_t vc4_uif_ub_pad(uint32_t height_ub)
{
	uint32_t offset_in_pc = height_ub % VC4_UIF_PAGE_CACHE_UB_ROWS;

	if (offset_in_pc == 0)
		return 0;
	if (offset_in_pc < VC4_UIF_PAGE_UB_ROWS_TIMES_1_5) {
		if (height_ub < VC4_UIF_PAGE_CACHE_UB_ROWS)
			return 0;
		return VC4_UIF_PAGE_UB_ROWS_TIMES_1_5 - offset_in_pc;
	}
	if (offset_in_pc > VC4_UIF_PAGE_CACHE_UB_ROWS - VC4_UIF_PAGE_UB_ROWS_TIMES_1_5)
		return VC4_UIF_PAGE_CACHE_UB_ROWS - offset_in_pc;
	return 0;
}

static int vc4_dumb_init(struct driver *drv)
{
	struct format_metadata uif_metadata = { 2, 0, DRM_FORMAT_MOD_BROADCOM_UIF };
	int ret = dumb_driver_init(drv);

	if (ret)
		return ret;

	drv_add_combinations(drv, vc4_uif_formats, ARRAY_SIZE(vc4_uif_formats), &uif_metadata,
			     BO_USE_TEXTURE | BO_USE_RENDERING);
	return 0;
}

static int vc4_dumb_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			      uint64_t use_flags)
{
	struct combination *combo = drv_get_combination(bo->drv, format, use_flags);
	struct drm_mode_create_dumb create_dumb = { 0 };
	struct drm_mode_destroy_dumb destroy_dumb = { 0 };
	uint32_t aligned_width, height_ub, padded_height, stride;

	if (!combo || combo->metadata.modifier != DRM_FORMAT_MOD_BROADCOM_UIF)
		return drv_dumb_bo_create(bo, width, height, format, use_flags);

	aligned_width = ALIGN(width, VC4_UIF_COLUMN_WIDTH_PX);
	height_ub = DIV_ROUND_UP(height, VC4_UIF_BLOCK_SIZE_PX);
	padded_height = (height_ub + vc4_uif_ub_pad(height_ub)) * VC4_UIF_BLOCK_SIZE_PX;
	stride = aligned_width * 4;

	create_dumb.width = aligned_width;
	create_dumb.height = padded_height;
	create_dumb.bpp = 32;
	if (drmIoctl(bo->drv->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb)) {
		drv_loge("DRM_IOCTL_MODE_CREATE_DUMB failed for UIF buffer (%d)\n", errno);
		return -errno;
	}

	if (create_dumb.pitch != stride) {
		drv_loge("unexpected dumb buffer pitch %u for UIF stride %u\n", create_dumb.pitch,
			 stride);
		destroy_dumb.handle = create_dumb.handle;
		drmIoctl(bo->drv->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
		return -EINVAL;
	}

	drv_bo_from_format(bo, stride, 1, padded_height, format);
	bo->handle.u32 = create_dumb.handle;
	bo->meta.total_size = create_dumb.size;
	bo->meta.format_modifier = DRM_FORMAT_MOD_BROADCOM_UIF;
	return 0;
}

static void *vc4_dumb_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	/* UIF buffers are allocated only without CPU use flags; their memory is tiled. */
	if (bo->meta.format_modifier == DRM_FORMAT_MOD_BROADCOM_UIF) {
		drv_loge("CPU mapping of a UIF tiled buffer is not supported\n");
		return MAP_FAILED;
	}
	return drv_dumb_bo_map(bo, vma, map_flags);
}

const struct backend backend_vc4_dumb = {
	.name = "vc4",
	.init = vc4_dumb_init,
	.bo_create = vc4_dumb_bo_create,
	.bo_create_with_modifiers = dumb_bo_create_with_modifiers,
	.bo_destroy = drv_dumb_bo_destroy,
	.bo_import = drv_prime_bo_import,
	.bo_map = vc4_dumb_bo_map,
	.bo_unmap = drv_bo_munmap,
	.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
};
#endif

#ifndef DRV_ROCKCHIP
INIT_DUMB_DRIVER(rockchip)
#endif
#ifndef DRV_MEDIATEK
INIT_DUMB_DRIVER(mediatek)
#endif
