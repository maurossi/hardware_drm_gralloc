# Copyright (C) 2010 Chia-I Wu <olvaffe@gmail.com>
# Copyright (C) 2010-2011 LunarG Inc.
# 
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

# Android.mk for drm_gralloc

DRM_GPU_DRIVERS := $(strip $(filter-out swrast, $(BOARD_GPU_DRIVERS)))

freedreno_drivers := freedreno
intel_drivers := i915 i965 i915g ilo
radeon_drivers := r300g r600g
nouveau_drivers := nouveau

valid_drivers := \
	$(freedreno_drivers) \
	$(intel_drivers) \
	$(radeon_drivers) \
	$(nouveau_drivers)

# Assume other driver names are pipe drivers
ifneq ($(filter-out $(valid_drivers), $(DRM_GPU_DRIVERS)),)
DRM_GPU_DRIVERS += pipe
endif

ifneq ($(strip $(DRM_GPU_DRIVERS)),)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libgralloc_drm
LOCAL_MODULE_TAGS := optional
LOCAL_VENDOR_MODULE := true

LOCAL_CFLAGS := -Wno-unused-parameter

LOCAL_SRC_FILES := \
	gralloc_drm.cpp \
	gralloc_drm_kms.c

LOCAL_EXPORT_C_INCLUDE_DIRS := \
	$(LOCAL_PATH)

LOCAL_C_INCLUDES := \
	external/libdrm \
	external/libdrm/include/drm

LOCAL_SHARED_LIBRARIES := \
	libdrm \
	liblog \
	libcutils \
	libhardware_legacy \

ifneq ($(filter $(freedreno_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_freedreno.c
LOCAL_CFLAGS += -DENABLE_FREEDRENO
LOCAL_SHARED_LIBRARIES += libdrm_freedreno
endif

ifneq ($(filter $(intel_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_intel.c
LOCAL_CFLAGS += -DENABLE_INTEL
LOCAL_SHARED_LIBRARIES += libdrm_intel
endif

ifneq ($(filter $(radeon_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_radeon.c
LOCAL_CFLAGS += -DENABLE_RADEON
LOCAL_SHARED_LIBRARIES += libdrm_radeon
endif

ifneq ($(filter $(nouveau_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_nouveau.c
LOCAL_CFLAGS += -DENABLE_NOUVEAU
LOCAL_SHARED_LIBRARIES += libdrm_nouveau
endif

ifneq ($(filter pipe, $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_pipe.c
LOCAL_CFLAGS += -DENABLE_PIPE
LOCAL_C_INCLUDES += \
	external/mesa/include \
	external/mesa/src \
	external/mesa/src/gallium/include \
	external/mesa/src/gallium/auxiliary


LOCAL_SHARED_LIBRARIES += libdl
endif # pipe_drivers

include $(BUILD_SHARED_LIBRARY)


include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
	gralloc.c \

LOCAL_SHARED_LIBRARIES := \
	libgralloc_drm \
	libdrm \
	liblog \

# for glFlush/glFinish
LOCAL_SHARED_LIBRARIES += \
	libGLESv1_CM

LOCAL_MODULE := gralloc.drm
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_VENDOR_MODULE := true
LOCAL_CFLAGS := -Wno-unused-parameter
include $(BUILD_SHARED_LIBRARY)

endif # DRM_GPU_DRIVERS
