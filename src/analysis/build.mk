# SPDX-License-Identifier: GPL-3.0-only
.DEFAULT_GOAL := all
AZ_SDK_INCLUDE ?= src/azmod
OUT ?= out/analysis
ANALYSIS_ASSETS ?= assets/analysis
CC = cc
AR ?= ar
PYTHON ?= python3
SANITIZE ?=
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -I$(AZ_SDK_INCLUDE) \
            -Isrc/analysis -Isrc/analysis/dsp/include -Ivendor/analysis/dsp -Ivendor/analysis/faad/include -Ivendor/analysis/faad/libfaad -Ivendor/analysis/mimalloc/include
CFLAGS += -std=c11 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden \
          -ffp-contract=off -fno-omit-frame-pointer -fno-optimize-sibling-calls -fexceptions -fasynchronous-unwind-tables -pthread
LDLIBS += -lm -pthread
ifneq ($(SANITIZE),)
CFLAGS += -g -fsanitize=$(SANITIZE) -fno-sanitize-recover=all
LDFLAGS += -fsanitize=$(SANITIZE)
endif
FAAD_FLAGS := -DLC_ONLY_DECODER -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 \
              -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 \
              -DPACKAGE_VERSION=\"2.11.4\"
ENGINE_SRC := $(wildcard src/analysis/*.c) src/analysis/dsp/decode.c src/analysis/dsp/aac.c \
              src/analysis/dsp/dsp.c src/analysis/dsp/phrase_filters.c $(wildcard vendor/analysis/faad/libfaad/*.c) \
              vendor/analysis/mimalloc/src/static.c $(wildcard vendor/analysis/musl-math/*.c)
ENGINE_OBJ := $(patsubst %.c,$(OUT)/%.o,$(ENGINE_SRC))
# Retain the original worker's release optimization for its DSP/codec kernels.
$(OUT)/src/analysis/vocals.o $(filter $(OUT)/src/analysis/dsp/% $(OUT)/vendor/analysis/faad/%,$(ENGINE_OBJ)): CFLAGS += -O3
ifeq ($(shell $(CC) -dumpmachine | cut -d- -f1),aarch64)
CFLAGS += -march=armv8-a -mtune=cortex-a53
endif
.PHONY: all test clean
all: $(OUT)/libanalysis.a
$(OUT)/libanalysis.a: $(ENGINE_OBJ)
	$(AR) rcs $@ $^
$(OUT)/vendor/analysis/faad/%.o: vendor/analysis/faad/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(FAAD_FLAGS) $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -MMD -MP -c $< -o $@
$(OUT)/vendor/analysis/mimalloc/src/static.o: vendor/analysis/mimalloc/src/static.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -DNDEBUG -DMI_STATIC_LIB \
	  $(if $(findstring address,$(SANITIZE)),-DMI_TRACK_ASAN=1) -ftls-model=local-dynamic -MMD -MP -c $< -o $@
$(OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(OUT)/src/analysis/dsp/decode.o $(OUT)/src/analysis/dsp/aac.o: CFLAGS += -Wno-error

-include $(ENGINE_OBJ:.o=.d)
