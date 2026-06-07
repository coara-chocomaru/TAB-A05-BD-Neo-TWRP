#
# Copyright (C) 2024 The Android Open Source Project
# Copyright (C) 2024 SebaUbuntu's TWRP device tree generator
#
# SPDX-License-Identifier: Apache-2.0
#

LOCAL_PATH := device/sts/a05bd
PRODUCT_CHARACTERISTICS := tablet
PRODUCT_PACKAGES += coara
PRODUCT_PACKAGES += \
    android.hardware.health@2.1-impl \
    android.hardware.health@2.1-impl.recovery \
    android.hardware.health@2.1-service

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.build.product=TAB-A05-BD \
    ro.product.board=TAB-A05-BD \
    ro.product.device=TAB-A05-BD \
    ro.product.name=TAB-A05-BD \
    ro.product.locale=ja-JP \
    ro.build.system_root_image=true \
    persist.sys.locale=ja-JP \
    ro.treble.enabled=true \
    ro.build.characteristics=tablet \
    ro.product.first_api_level=28

PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)
