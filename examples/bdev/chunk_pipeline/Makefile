#  SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
CUDA_HOME ?= /usr/local/cuda-13.0

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = chunk_pipeline

C_SRCS := chunk_pipeline.c

CFLAGS += -I$(CUDA_HOME)/include
SYS_LIBS += -L$(CUDA_HOME)/lib64
SYS_LIBS += -lcudart -lstdc++

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
