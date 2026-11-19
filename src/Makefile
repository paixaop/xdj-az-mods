# SPDX-License-Identifier: GPL-3.0-only
# Builds azmod.so and azstem.so for the XDJ-AZ (aarch64, glibc). Run it inside the
# azmod-build:18.04 container (see ../BUILDING.md); the official releases are built
# with exactly these commands, so the same source gives the same bytes.
#
#   make CC=gcc azmod azstem           -> out/azmod.so, out/azstem.so
#   make CC=gcc azhandoff              -> out/az-handoff-guard (rides the stick in MODS/deck/lib)
#
# The base flags follow upstream. Additional SDK services are private C objects.
# Use the shared GCC unwinder across EP147 and both preload libraries: a private
# static unwinder can abort when another runtime enters a C cleanup landing pad.

CC ?= gcc

# `-mno-outline-atomics` is a GCC 10+ flag. Older GCCs (e.g. Ubuntu 18.04's
# 7.5) error out on it. Probe and add it only if the compiler accepts it.
NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' \
    | $(CC) -x c -mno-outline-atomics -c -o /dev/null - 2>/dev/null \
    && echo -mno-outline-atomics)

OUT := out
AZMOD := $(OUT)/azmod.so
AZSTEM := $(OUT)/azstem.so
AZHANDOFF := $(OUT)/az-handoff-guard

.PHONY: all azmod azstem azhandoff clean
all: azmod azstem azhandoff
azmod: $(OUT) $(AZMOD)
azstem: $(OUT) $(AZSTEM)
azhandoff: $(OUT) $(AZHANDOFF)

$(OUT):
	mkdir -p $(OUT)

# --- azmod.so (the Mod Menu and every on-screen mod; LD_PRELOAD into EP147) ---
# azmod/text.c renders overlay labels with FreeType (already on the deck).
AZMOD_FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null || echo -I/usr/include/freetype2)
# Implicit function declarations must be errors: an LD_PRELOAD object links fine against a
# function that does not exist and only fails when the call runs, inside EP147.
# AZ_UNWIND: EP147 throws C++ exceptions through our hooks on normal error paths;
# every C frame needs unwind info or a handled error becomes std::terminate.
AZ_UNWIND := -fexceptions -fasynchronous-unwind-tables
# Warnings are errors: the source builds with none under -Wall -Wextra (implicit function
# declarations included, the reason given above).
AZ_WARN := -Wall -Wextra -Werror
AZMOD_CFLAGS := $(AZ_UNWIND) -O2 $(AZ_WARN) -fPIC -shared -shared-libgcc -Wl,--no-undefined $(NO_OUTLINE_ATOMICS) -Iazmod $(AZMOD_FT_CFLAGS)
# Release mode (the default) hides rows that are still in testing.
AZMOD_RELEASE ?= 1
ifeq ($(AZMOD_RELEASE),1)
AZMOD_CFLAGS += -DAZMOD_RELEASE
endif

# Release builds ship without the full symbol table: each target links to <target>.full, the
# shipped file is that link with --strip-unneeded (the dynamic exports stay). The .full file is
# the same build with its symbols, for the unit tests' binary lints (procmem-lint.sh scans it and
# proves the shipped file is exactly it, stripped) and for reading a crash address. Development
# builds (AZMOD_RELEASE=0) ship unstripped.
STRIP ?= strip
OBJCOPY ?= objcopy
# azmod.so keeps exactly one group of symbols: the arrays of the asset headers below (art made
# from the deck's firmware, and the library key), which the public source tree does not ship and
# its tools/extract-assets.py copies out of an official azmod.so by name. The list is those
# headers' identifiers that are data symbols of this build, so it follows the headers.
AZMOD_ASSET_HDRS := $(addprefix azmod/,caption_strips.h fxart.h fxhdr.h fxbars.h holdtile.h sfxtile.h menulabels.h collabels.h stems4art.h srcref.h wavref.h onelibkey.h onelibkey_sm.h)
ifeq ($(AZMOD_RELEASE),1)
az_strip = $(STRIP) --strip-unneeded -o $(1) $(1).full
az_strip_keep_assets = nm --defined-only $(1).full | awk '$$2 ~ /^[rRdD]$$/ {print $$3}' | sort -u > $(1).objs && \
	grep -ohE '[A-Za-z_][A-Za-z0-9_]*' $(AZMOD_ASSET_HDRS) | sort -u | comm -12 - $(1).objs > $(1).keep && \
	$(OBJCOPY) --strip-unneeded --keep-symbols=$(1).keep $(1).full $(1) && rm -f $(1).objs
else
az_strip_keep_assets = cp $(1).full $(1)
az_strip = cp $(1).full $(1)
endif
# Test triggers and diagnostic probes: emulator test builds only, never released.
ifeq ($(AZMOD_PROBES),1)
AZMOD_CFLAGS += -DAZMOD_PROBES
endif
# menu.c #includes the Mod Menu's parts (ui_draw.c ... menu_composite.c, listed there): they are
# not translation units of their own, and the azmod/*.c wildcard below makes them prerequisites.
AZMOD_SRCS := azmod/azmod.c azmod/menu.c azmod/text.c azmod/azconf.c
AZMOD_HDRS := $(wildcard azmod/slip_diagnostic/*) $(wildcard azmod/*.h azmod/*.c azmod/*.inc azmod/*.S azmod/*.def) $(wildcard common/*.h common/*.c common/*.S) azstem/sdk_native.map
AZSDK_NATIVE_CFLAGS := $(AZ_UNWIND) -std=gnu11 -O2 $(AZ_WARN) -fPIC $(NO_OUTLINE_ATOMICS) -fvisibility=hidden
AZANALYSIS_ARCHIVE := $(abspath $(OUT))/analysis/libanalysis.a
AZMOD_HDRS += $(shell find analysis ../vendor/analysis -type f) azmod/analysis_runtime.h azmod/analysis_source.h
$(AZMOD): $(AZMOD_SRCS) $(AZMOD_HDRS) | $(OUT)
	$(MAKE) -C .. -f src/analysis/build.mk CC="$(CC)" AR="$(AR)" OUT="$(abspath $(OUT))/analysis" all
	$(CC) $(AZSDK_NATIVE_CFLAGS) -Iazmod -c -o $(OUT)/analysis_runtime.o azmod/analysis_runtime.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -Iazmod -c -o $(OUT)/analysis_owner.o azmod/analysis_owner.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -Iazmod -c -o $(OUT)/analysis_source.o azmod/analysis_source.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -Iazmod -c -o $(OUT)/analysis_encoding.o azmod/analysis_encoding.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -Iazmod -c -o $(OUT)/analysis_host.o azmod/analysis_host.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -Iazmod -c -o $(OUT)/analysis_settings.o azmod/analysis_settings.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/beatport_service.o azmod/beatport_service.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_status_store.o azmod/sdk_status_store.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_status_engine.o azmod/sdk_status_engine.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_status_metrics.o azmod/sdk_status_metrics.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_command_service.o azmod/sdk_command_service.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_command_engine.o azmod/sdk_command_engine.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/track_storage.o azmod/track_storage.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/track_service.o azmod/track_service.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/azcache_scopes.o common/azcache_scopes.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_command_target.o azmod/sdk_command_target.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/slip_diagnostic.o azmod/slip_diagnostic/diagnostic.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/slip_capture.o azmod/slip_diagnostic/capture.c
	$(CC) $(AZ_UNWIND) -fPIC -c -o $(OUT)/slip_diagnostic_entry.o azmod/slip_diagnostic/entry.S
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_native_sources.o azmod/sdk_native_sources.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_connection_hooks.o azmod/sdk_connection_hooks.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_cache_message.o azmod/sdk_cache_message.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_native_slip.o azmod/sdk_native_slip.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_slip_hooks.o azmod/sdk_slip_hooks.c
	$(CC) $(AZSDK_NATIVE_CFLAGS) -c -o $(OUT)/sdk_slip_getters.o azmod/sdk_slip_getters.c
	$(CC) $(AZ_UNWIND) -O2 $(AZ_WARN) -fPIC $(NO_OUTLINE_ATOMICS) -fvisibility=hidden -c -o $(OUT)/sdk_cache_entry.o azmod/sdk_cache_entry.c
	$(CC) $(AZ_UNWIND) -fPIC -c -o $(OUT)/sdk_cache_entry_asm.o azmod/sdk_cache_entry.S
	$(CC) $(AZ_UNWIND) -fPIC -c -o $(OUT)/sdk_slip_entry_asm.o azmod/sdk_slip_entry.S
	$(CC) $(AZ_UNWIND) -fPIC -c -o $(OUT)/sdk_slip_getter_entry.o azmod/sdk_slip_getter_entry.S
	$(CC) $(AZMOD_CFLAGS) -Wl,--version-script=azstem/sdk_native.map -o $@.full $(AZMOD_SRCS) $(OUT)/beatport_service.o $(OUT)/analysis_runtime.o $(OUT)/analysis_owner.o $(OUT)/analysis_source.o $(OUT)/analysis_encoding.o $(OUT)/analysis_host.o $(OUT)/analysis_settings.o $(AZANALYSIS_ARCHIVE) -Wl,--exclude-libs,libanalysis.a $(OUT)/track_service.o $(OUT)/track_storage.o $(OUT)/sdk_command_engine.o $(OUT)/sdk_command_service.o $(OUT)/sdk_status_engine.o $(OUT)/sdk_status_store.o $(OUT)/sdk_status_metrics.o $(OUT)/slip_diagnostic.o $(OUT)/slip_capture.o $(OUT)/slip_diagnostic_entry.o $(OUT)/sdk_command_target.o $(OUT)/azcache_scopes.o $(OUT)/sdk_native_sources.o $(OUT)/sdk_connection_hooks.o $(OUT)/sdk_cache_message.o $(OUT)/sdk_native_slip.o $(OUT)/sdk_slip_hooks.o $(OUT)/sdk_cache_entry.o $(OUT)/sdk_cache_entry_asm.o $(OUT)/sdk_slip_entry_asm.o $(OUT)/sdk_slip_getters.o $(OUT)/sdk_slip_getter_entry.o -ldl -lpthread -Wl,-Bstatic -lXtst -Wl,-Bdynamic -Wl,--exclude-libs,libXtst.a -lX11 -lXext -lfreetype -lm
	$(call az_strip_keep_assets,$@)
	@echo "Built: $@"

# --- azstem.so (stems audio, drum rolls; LD_PRELOAD into EP147) ---
# stb_vorbis.c (public domain / MIT, vendored) is its own translation unit.
AZSTEM_CFLAGS := -DAZ_STATUS_MAPS_PRODUCER -DAZ_STATUS_MEMORY_PRODUCER -DAZ_STATUS_NATIVE_PRODUCER $(AZ_UNWIND) -O2 $(AZ_WARN) -fPIC -shared -shared-libgcc $(NO_OUTLINE_ATOMICS) -Iazstem -DAZMOD_RELEASE
AZSTEM_STB_CFLAGS := $(AZ_UNWIND) -O2 -w -fPIC $(NO_OUTLINE_ATOMICS) -Iazstem -fvisibility=hidden
AZSTEM_OBSERVER_CFLAGS := -DAZ_STATUS_MAPS_PRODUCER -DAZ_STATUS_MEMORY_PRODUCER -DAZ_STATUS_NATIVE_PRODUCER $(AZSDK_NATIVE_CFLAGS)
$(AZSTEM): $(wildcard azstem/*.c azstem/*.h azstem/*.map azstem/*.inc azstem/*.S) $(wildcard common/*.h common/*.c common/*.S) | $(OUT)
	$(CC) $(AZSTEM_STB_CFLAGS) -c -o $(OUT)/stb_vorbis.o azstem/stb_vorbis.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_loop_observer.o azstem/sdk_loop_observer.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_status_memory.o azstem/sdk_status_memory.c
	$(CC) $(AZ_UNWIND) -fPIC -c -o $(OUT)/sdk_status_memory_entry.o azstem/sdk_status_memory_entry.S
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_status_maps.o azstem/sdk_status_maps.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_live_observer.o azstem/sdk_live_observer.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_native_commands.o azstem/sdk_native_commands.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_exit.o azstem/sdk_exit.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_actor.o azstem/sdk_cue_actor.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_store.o azstem/sdk_cue_store.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_persist.o azstem/sdk_cue_persist.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_lifecycle.o azstem/sdk_cue_lifecycle.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_commands.o azstem/sdk_cue_commands.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_recall.o azstem/sdk_cue_recall.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_load_observer.o azstem/sdk_load_observer.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_analysis_source.o azstem/sdk_analysis_source.c
	$(CC) $(AZSTEM_OBSERVER_CFLAGS) -c -o $(OUT)/sdk_cue_observer.o azstem/sdk_cue_observer.c
	$(CC) $(AZSTEM_CFLAGS) -Wl,--version-script=azstem/sdk_native.map -o $@.full azstem/azstem.c azstem/overview_lease.c azstem/sdk_status_capture.c $(OUT)/stb_vorbis.o $(OUT)/sdk_loop_observer.o $(OUT)/sdk_status_memory.o $(OUT)/sdk_status_memory_entry.o $(OUT)/sdk_status_maps.o $(OUT)/sdk_live_observer.o $(OUT)/sdk_native_commands.o $(OUT)/sdk_exit.o $(OUT)/sdk_cue_actor.o $(OUT)/sdk_cue_store.o $(OUT)/sdk_cue_persist.o $(OUT)/sdk_cue_lifecycle.o $(OUT)/sdk_cue_commands.o $(OUT)/sdk_cue_recall.o $(OUT)/sdk_load_observer.o $(OUT)/sdk_analysis_source.o $(OUT)/sdk_cue_observer.o -lm -ldl -lpthread
	$(call az_strip,$@)
	@echo "Built: $@"

# --- az-handoff-guard (the way back to the deck during a mod's handoff; runs while EP147 is stopped) ---
# static: it must not depend on anything else on the deck
$(AZHANDOFF): azhandoff/az-handoff-guard.c azhandoff/guard_logic.h | $(OUT)
	$(CC) -O2 -Wall -Wextra -static -Werror $(NO_OUTLINE_ATOMICS) -o $@.full azhandoff/az-handoff-guard.c
	$(call az_strip,$@)

clean:
	rm -rf $(OUT)
