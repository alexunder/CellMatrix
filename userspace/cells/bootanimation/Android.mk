LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := cells_bootanimation.zip
LOCAL_MODULE := cells_bootanimation

LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT)/media
LOCAL_MODULE_STEM := bootanimation.zip

include $(BUILD_PREBUILT)
