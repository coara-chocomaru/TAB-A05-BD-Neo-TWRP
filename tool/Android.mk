LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE               := coara
LOCAL_MODULE_TAGS          := optional
LOCAL_SRC_FILES            := coara.cpp
LOCAL_CPPFLAGS             := -std=c++11 -Wall -Wextra
LOCAL_STATIC_LIBRARIES     :=
LOCAL_SHARED_LIBRARIES     :=
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_MODULE_CLASS         := EXECUTABLES
LOCAL_MODULE_PATH          := $(TARGET_RECOVERY_ROOT_OUT)/sbin

include $(BUILD_EXECUTABLE)
