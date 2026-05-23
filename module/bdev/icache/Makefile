#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025
#  All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

PORT_DIR := logcache/port
LOGGING_DIR := logcache/logging

C_SRCS += vbdev_icache.c vbdev_icache_rpc.c
CXX_SRCS += \
	logcache/log_cache_wrapper.cpp \
	$(PORT_DIR)/log_cache.cpp \
	$(PORT_DIR)/icache.cpp \
	$(PORT_DIR)/ghost_cache.cpp \
	$(PORT_DIR)/age_ghost_cache.cpp \
	$(PORT_DIR)/histogram.cpp \
	$(PORT_DIR)/emwa.cpp \
	$(PORT_DIR)/evict_policy_fifo.cpp \
	$(PORT_DIR)/evict_policy_fifo_zero.cpp \
	$(PORT_DIR)/evict_policy_greedy.cpp \
	$(PORT_DIR)/evict_policy_cost_benefit.cpp \
	$(PORT_DIR)/evict_policy_lambda.cpp \
	$(PORT_DIR)/evict_policy_selective_fifo.cpp \
	$(PORT_DIR)/evict_policy_k_cost_benefit.cpp \
	$(PORT_DIR)/evict_policy_multiqueue.cpp \
	$(PORT_DIR)/evict_policy_midas.cpp \
	$(PORT_DIR)/hot_cold.cpp \
	$(PORT_DIR)/hot_cold_midas.cpp \
	$(PORT_DIR)/multi_hot_cold.cpp \
	$(PORT_DIR)/sepbit.cpp \
	$(PORT_DIR)/istream.cpp \
	$(LOGGING_DIR)/stats_logger.cpp
LIBNAME = bdev_icache

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

CFLAGS += -I$(SPDK_ROOT_DIR)/module/bdev/icache/logcache -I$(SPDK_ROOT_DIR)/module/bdev/icache/logcache/port
CXXFLAGS += -I$(SPDK_ROOT_DIR)/module/bdev/icache/logcache -I$(SPDK_ROOT_DIR)/module/bdev/icache/logcache/port -I$(SPDK_ROOT_DIR)/module/bdev/icache/logcache/logging -DSPDK_ICACHE_ONLY_LOGCACHE
LOCAL_SYS_LIBS += -lstdc++

ICACHE_EXTRA_SYS_LIBS = -lstdc++

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

CXXFLAGS += -std=c++17
SYS_LIBS += $(ICACHE_EXTRA_SYS_LIBS)
