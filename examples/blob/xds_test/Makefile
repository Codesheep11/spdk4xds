#  SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
XDS_ROOT_DIR := $(abspath $(SPDK_ROOT_DIR)/..)
CUDA_HOME ?= /usr/local/cuda-13.0

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = xds_blob_bench

C_SRCS := xds_blob_bench.c
C_SRCS += $(XDS_ROOT_DIR)/src/dma/xds_dma_registry.c

CFLAGS += -I$(CUDA_HOME)/include
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/nvme
CFLAGS += -I$(XDS_ROOT_DIR)/include
CFLAGS += -I$(XDS_ROOT_DIR)/kernels/include
CFLAGS += -I$(XDS_ROOT_DIR)/kernels/user/include

SYS_LIBS += -L$(CUDA_HOME)/lib64
SYS_LIBS += -lcudart -lstdc++
SYS_LIBS += $(XDS_ROOT_DIR)/kernels/user/libxds_p2p_user.a

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev blob bdev nvme

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
