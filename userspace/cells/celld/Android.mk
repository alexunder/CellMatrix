# celld Makefile
#
# Copyright (C) 2011-2013 Columbia University
# Author: Jeremy C. Andrus <jeremya@cs.columbia.edu>
#

LOCAL_PATH := $(call my-dir)

#
# celld (container control daemon)
#
include $(CLEAR_VARS)

LOCAL_CFLAGS :=

LOCAL_SRC_FILES:= \
	ext/glibc_openpty.c \
	cell_console.c \
	nsexec.c \
	shared_ops.c \
	util.c \
	celld.c \
	wifi_host.c \
	cell_config.c

LOCAL_MODULE := celld
LOCAL_MODULE_TAGS := optional
LOCAL_C_INCLUDES := \
	$(call include-path-for, libhardware_legacy)/hardware_legacy
LOCAL_SHARED_LIBRARIES := libm libcutils libc libhardware_legacy
include $(BUILD_EXECUTABLE)


#
# cell (container control front-end)
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	ext/glibc_openpty.c \
	cell_console.c \
	shared_ops.c \
	util.c \
	cell.c

LOCAL_MODULE:= cell
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libm libcutils libc
include $(BUILD_EXECUTABLE)
