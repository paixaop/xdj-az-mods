# SPDX-License-Identifier: GPL-3.0-only
REKORDBOX_APP ?= /Applications/rekordbox 7/rekordbox.app
OUTPUT_DIR ?= $(HOME)
HOST_TOOL ?= src/out/host/create-pvdi-model
WINDOWS_TOOL ?= src/out/host/create-pvdi-model-windows
WINDOWS_SOURCE_TEST ?= src/out/host/test-windows-pvdi-source
WINDOWS_MOTION_TEST ?= src/out/host/test-windows-pvdi-motion
WINDOWS_MOTION_SRC := src/analysis/host/pvdi-windows-motion.c
WINDOWS_SOURCE_SRC := src/analysis/host/pvdi-windows-source.c src/analysis/host/pvdi-windows-reduction.c $(WINDOWS_MOTION_SRC)
WINDOWS_SOURCE_DEPS := src/analysis/host/pvdi-windows-source.h src/analysis/host/pvdi-windows-motion.h \
                      src/analysis/host/pvdi-windows-reduction.h \
                      src/analysis/host/pvdi-windows-vocal-offsets.inc src/analysis/host/pvdi-windows-phrase-offsets.inc \
                      src/analysis/host/pvdi-windows-network-offsets.inc src/analysis/model-format.h src/common/azsha256.h
WINDOWS_TOOL_SRC := src/analysis/host/create-pvdi-model-windows.c src/analysis/host/pvdi-model.c $(WINDOWS_SOURCE_SRC)
HOST_SRC := src/analysis/host/create-pvdi-model.c $(WINDOWS_TOOL_SRC)
PKG_CONFIG ?= pkg-config
WINDOWS_SOURCE_CPPFLAGS ?= $(shell $(PKG_CONFIG) --cflags libcrypto 2>/dev/null)
WINDOWS_SOURCE_LIBS ?= $(shell $(PKG_CONFIG) --libs libcrypto 2>/dev/null || printf '%s' -lcrypto) -lm

ifeq ($(shell uname -s),Darwin)
WINDOWS_IMPORT_TOOL := $(HOST_TOOL)
else
WINDOWS_IMPORT_TOOL := $(WINDOWS_TOOL)
endif

export REKORDBOX_APP OUTPUT_DIR WINDOWS_REKORDBOX_EXE WINDOWS_VD_PARAM_PB WINDOWS_REKORDBOX_VERSION

.PHONY: model
model: $(HOST_TOOL)
	@mkdir -p "$$OUTPUT_DIR"
	@"$(HOST_TOOL)" "$$REKORDBOX_APP" "$$OUTPUT_DIR"

$(HOST_TOOL): $(HOST_SRC) $(WINDOWS_SOURCE_DEPS) src/analysis/host/pvdi-model.h \
              src/analysis/host/pvdi-macos-offsets.inc tools/analysis/create-pvdi-model.mk
	@mkdir -p "$(@D)"
	$(CC) -std=c11 -Wall -Wextra -Werror -ffp-contract=off $(CPPFLAGS) $(WINDOWS_SOURCE_CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
		-DPVDI_WINDOWS_IMPORT $(HOST_SRC) -framework CoreFoundation $(WINDOWS_SOURCE_LIBS) -o "$@"

.PHONY: windows-model
windows-model: $(WINDOWS_IMPORT_TOOL)
	@mkdir -p "$$OUTPUT_DIR"
	@"$(WINDOWS_IMPORT_TOOL)" "$$WINDOWS_REKORDBOX_EXE" "$$WINDOWS_VD_PARAM_PB" "$$OUTPUT_DIR"

$(WINDOWS_TOOL): $(WINDOWS_TOOL_SRC) $(WINDOWS_SOURCE_DEPS) src/analysis/host/pvdi-model.h \
                 src/analysis/host/pvdi-macos-offsets.inc tools/analysis/create-pvdi-model.mk
	@mkdir -p "$(@D)"
	$(CC) -std=c11 -Wall -Wextra -Werror -ffp-contract=off $(CPPFLAGS) $(WINDOWS_SOURCE_CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
		-DPVDI_WINDOWS_IMPORT -DPVDI_WINDOWS_STANDALONE $(WINDOWS_TOOL_SRC) $(WINDOWS_SOURCE_LIBS) -o "$@"

$(WINDOWS_SOURCE_TEST): $(WINDOWS_SOURCE_SRC) $(WINDOWS_SOURCE_DEPS) tests/analysis/test_windows_pvdi_source.c
	@mkdir -p "$(@D)"
	$(CC) -std=c11 -Wall -Wextra -Werror -ffp-contract=off $(CPPFLAGS) $(WINDOWS_SOURCE_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(WINDOWS_SOURCE_SRC) \
		tests/analysis/test_windows_pvdi_source.c $(WINDOWS_SOURCE_LIBS) -o "$@"

$(WINDOWS_MOTION_TEST): $(WINDOWS_MOTION_SRC) src/analysis/host/pvdi-windows-motion.h \
                        tests/analysis/test_windows_pvdi_motion.c src/analysis/model-format.h src/common/azsha256.h
	@mkdir -p "$(@D)"
	$(CC) -std=c11 -Wall -Wextra -Werror $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(WINDOWS_MOTION_SRC) \
		tests/analysis/test_windows_pvdi_motion.c -o "$@"

# Full-file reference checked against native operands and the shipped GraphDef.
AZPVDI60_SHA256 := a400986de5a2258329b1ee22f326e3d1247a190814e0481fe21f77da871f7226
AZPVDIW1_SHA256 := 2fb54bd4666e964f40476cd537d14cc28317fc4e67d911173f67b8eccebddae2
SHA256SUM ?= shasum -a 256

ifeq ($(shell uname -s),Darwin)
.PHONY: test-unified
test-unified: test test-windows
endif

.PHONY: test-windows
test-windows: $(WINDOWS_IMPORT_TOOL) $(WINDOWS_SOURCE_TEST)
	@set -eu; tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/az-model-windows-test.XXXXXX"); \
	  trap 'rm -rf "$$tmp"' EXIT; \
	  "$(WINDOWS_SOURCE_TEST)" "$$WINDOWS_REKORDBOX_EXE" "$$WINDOWS_VD_PARAM_PB" "$$WINDOWS_REKORDBOX_VERSION"; \
	  "$(WINDOWS_IMPORT_TOOL)" "$$WINDOWS_REKORDBOX_EXE" "$$WINDOWS_VD_PARAM_PB" "$$tmp"; \
	  (cd "$$tmp" && printf '%s  pvdi-model.bin\n' '$(AZPVDIW1_SHA256)' | $(SHA256SUM) -c); \
	  printf 'unsupported' > "$$tmp/incorrect.exe"; \
	  if "$(WINDOWS_IMPORT_TOOL)" "$$tmp/incorrect.exe" "$$WINDOWS_VD_PARAM_PB" "$$tmp" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q 'Unsupported Windows rekordbox executable' "$$tmp/rejected.log"; \
	  cp "$$WINDOWS_VD_PARAM_PB" "$$tmp/incorrect.pb"; \
	  printf '\000' | dd of="$$tmp/incorrect.pb" conv=notrunc 2>/dev/null; \
	  if "$(WINDOWS_IMPORT_TOOL)" "$$WINDOWS_REKORDBOX_EXE" "$$tmp/incorrect.pb" "$$tmp" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q 'Input does not match the verified Windows vd_param.pb' "$$tmp/rejected.log"; \
	  (cd "$$tmp" && printf '%s  pvdi-model.bin\n' '$(AZPVDIW1_SHA256)' | $(SHA256SUM) -c)

.PHONY: test
test: $(HOST_TOOL)
	@set -eu; tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/az-model-test.XXXXXX"); \
	  trap 'rm -rf "$$tmp"' EXIT; \
	  "$(HOST_TOOL)" "$$REKORDBOX_APP" "$$tmp"; \
	  (cd "$$tmp" && printf '%s  pvdi-model.bin\n' '$(AZPVDI60_SHA256)' | shasum -a 256 -c); \
	  if "$(HOST_TOOL)" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q '^Usage:' "$$tmp/rejected.log"; \
	  if "$(HOST_TOOL)" "" "$$tmp" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q '^Usage:' "$$tmp/rejected.log"; \
	  if "$(HOST_TOOL)" "$$REKORDBOX_APP" "$$(printf '%2048s' '')" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q 'Model import path is too long' "$$tmp/rejected.log"; \
	  app="$$tmp/Test rekordbox.app"; \
	  mkdir -p "$$app/Contents/MacOS" "$$app/Contents/Resources"; \
	  cp "$$REKORDBOX_APP/Contents/Info.plist" "$$app/Contents/Info.plist"; \
	  /usr/libexec/PlistBuddy -c 'Set :CFBundleVersion 0.0.0' "$$app/Contents/Info.plist"; \
	  if "$(HOST_TOOL)" "$$app" "$$tmp" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q 'Unsupported rekordbox build' "$$tmp/rejected.log"; \
	  cp "$$REKORDBOX_APP/Contents/Info.plist" "$$app/Contents/Info.plist"; \
	  ln -s "$$(cd "$$REKORDBOX_APP/Contents/MacOS" && pwd)/rekordbox" "$$app/Contents/MacOS/rekordbox"; \
	  graph="$$app/Contents/Resources/vd_param_raw.pb"; \
	  cp "$$REKORDBOX_APP/Contents/Resources/vd_param_raw.pb" "$$graph"; \
	  printf '\000' | dd of="$$graph" conv=notrunc 2>/dev/null; \
	  if "$(HOST_TOOL)" "$$app" "$$tmp" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q 'Input does not match the verified rekordbox .*vd_param_raw.pb' "$$tmp/rejected.log"; \
	  cp "$$REKORDBOX_APP/Contents/Resources/vd_param_raw.pb" "$$graph"; \
	  binary="$$app/Contents/MacOS/rekordbox"; \
	  lipo "$$binary" -thin arm64 -output "$$tmp/arm64"; \
	  offset=$$(($$(stat -f '%z' "$$tmp/arm64") - 1)); \
	  byte=$$(od -An -tu1 -j "$$offset" -N 1 "$$tmp/arm64"); \
	  printf '%b' "$$(printf '\\0%03o' "$$((byte ^ 1))")" | \
	    dd of="$$tmp/arm64" bs=1 seek="$$offset" conv=notrunc 2>/dev/null; \
	  rm "$$binary"; \
	  lipo -create "$$tmp/arm64" -output "$$binary"; \
	  if "$(HOST_TOOL)" "$$app" "$$tmp" > "$$tmp/rejected.log" 2>&1; then exit 1; fi; \
	  grep -q 'Input does not match the verified rekordbox .*arm64' "$$tmp/rejected.log"; \
	  (cd "$$tmp" && printf '%s  pvdi-model.bin\n' '$(AZPVDI60_SHA256)' | shasum -a 256 -c)
