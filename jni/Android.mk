LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_paradise
LOCAL_SRC_FILES := lib/paradise/libparadise_api.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_kernel_modules
LOCAL_SRC_FILES := lib/kernel/libkernel_modules.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_embree
LOCAL_SRC_FILES := lib/libembree4.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_lexers
LOCAL_SRC_FILES := lib/liblexers.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_math
LOCAL_SRC_FILES := lib/libmath.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_simd
LOCAL_SRC_FILES := lib/libsimd.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_sys
LOCAL_SRC_FILES := lib/libsys.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing_tasking
LOCAL_SRC_FILES := lib/libtasking.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := lengjing

LENGJING_SOURCE_FILES := $(wildcard $(LOCAL_PATH)/src/*.c)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*.cc)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*.cpp)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*/*.c)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*/*.cc)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*/*.cpp)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*/*/*.c)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*/*/*.cc)
LENGJING_SOURCE_FILES += $(wildcard $(LOCAL_PATH)/src/*/*/*.cpp)
LENGJING_SOURCE_FILES += $(LOCAL_PATH)/lib/t3/t3sdk.cpp
LOCAL_SRC_FILES := $(LENGJING_SOURCE_FILES:$(LOCAL_PATH)/%=%)
LOCAL_SRC_FILES := $(filter-out src/ImGui/imgui_demo.cpp,$(LOCAL_SRC_FILES))

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/include \
    $(LOCAL_PATH)/include/Android_draw \
    $(LOCAL_PATH)/include/Android_Graphics \
    $(LOCAL_PATH)/include/Android_my_imgui \
    $(LOCAL_PATH)/include/Android_touch \
    $(LOCAL_PATH)/include/My_Utils \
    $(LOCAL_PATH)/include/ImGui \
    $(LOCAL_PATH)/include/ImGui/backends \
    $(LOCAL_PATH)/include/native_surface \
    $(LOCAL_PATH)/lib \
    $(LOCAL_PATH)/lib/embree4 \
    $(LOCAL_PATH)/lib/paradise

LOCAL_CONLYFLAGS := -std=c11
LOCAL_CPPFLAGS := -std=c++17 -fvisibility-inlines-hidden -fno-rtti -fexceptions -fpermissive -Wno-narrowing
LOCAL_CFLAGS := -fvisibility=hidden -fdata-sections -ffunction-sections -O3 -ffp-contract=fast -funroll-loops -fomit-frame-pointer -flto=thin
LOCAL_LDFLAGS := -flto=thin -Wl,--gc-sections -Wl,--icf=safe -Wl,-O2 -Wl,--strip-all
LOCAL_LDLIBS := -llog -landroid -ldl -lz
LOCAL_STATIC_LIBRARIES := \
    lengjing_paradise \
    lengjing_kernel_modules \
    lengjing_embree \
    lengjing_lexers \
    lengjing_math \
    lengjing_simd \
    lengjing_sys \
    lengjing_tasking

include $(BUILD_EXECUTABLE)
