# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userapp

MODULE_SRCS += \
    $(LOCAL_DIR)/taskgrinder.cpp

MODULE_NAME := taskgrinder

MODULE_LIBS := \
    system/ulib/mxio \
    system/ulib/magenta \
    system/ulib/c

MODULE_STATIC_LIBS := \
    system/ulib/mx \
    system/ulib/mxtl \
    system/ulib/mxcpp

include make/module.mk
