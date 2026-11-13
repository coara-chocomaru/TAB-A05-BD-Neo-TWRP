LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := ubuntu-loader
LOCAL_SRC_FILES := ubuntu_loader.cpp
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/sbin
LOCAL_MULTILIB := 64
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_CXX_STL := libc++
LOCAL_CPPFLAGS := -D_GNU_SOURCE -std=gnu++11 -O2 -pipe -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wall -Wextra
LOCAL_CFLAGS := -D_GNU_SOURCE -O2 -Wall -Wextra
LOCAL_LDFLAGS += -Wl,--gc-sections

include $(BUILD_EXECUTABLE)
