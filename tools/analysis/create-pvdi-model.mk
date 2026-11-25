# SPDX-License-Identifier: GPL-3.0-only
REKORDBOX_APP ?= /Applications/rekordbox 7/rekordbox.app
OUTPUT_DIR ?= $(HOME)
HOST_TOOL ?= src/out/host/create-pvdi-model
HOST_SRC := src/analysis/host/create-pvdi-model.c src/analysis/host/pvdi-model.c
export REKORDBOX_APP OUTPUT_DIR

.PHONY: model
model: $(HOST_TOOL)
	@"$(HOST_TOOL)" "$$REKORDBOX_APP" "$$OUTPUT_DIR"

$(HOST_TOOL): $(HOST_SRC) src/analysis/host/pvdi-model.h
	@mkdir -p "$(@D)"
	$(CC) -std=c11 -Wall -Wextra -Werror $(CFLAGS) $(LDFLAGS) $(HOST_SRC) -framework CoreFoundation -o "$@"
