#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# busread-lint.sh [GUEST_DIR]: the panel-bus interposers (read() and ioctl() in
# guest/azmod/bus_interpose.c and its mixer_input.c stage) take settings only from what the overlay thread
# publishes for them (guest/azmod/bus_cfg.h): the settings word bus_cfg(), the color words
# g_mode_rgbw / g_mode2_rgbw, the pad LED blocks through seqblk_read, and function pointers
# loaded once with __atomic_load_n. Reading the overlay's own state there could see a setting
# half-way through a change (the x-pad's five fields, a color's three bytes).
#
# Fails on, inside those two function bodies (comments ignored):
#   g_state.                      the Mod Menu state itself
#   g_xpad_{pos_off,touch_off,touch_msk,span,zone_off}   the x-pad config fields
#   g_reorder_hooks, g_reorder_forced_off                 (published in the word)
#   g_mode_rgb[, g_mode2_rgb[, g_pad_led[ indexed directly
#   a call through g_drum_*_fn without loading it first
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
G=$(cd "${1:-$here/../../guest}" && pwd)
out=$(awk '
    FNR == 1 && FILENAME ~ /mixer_input\.c$/ { inb = 2; fn = "mixer input" }
    /^int ioctl\(int fd, unsigned long req/ { inb = 1; fn = "ioctl()" }
    /^\/\* ---- read\(\): the panel input buses/ { inr = 1; fn = "read path" }
    /^}/ { if (inb) { inb = 0; next } }
    inr && !inb { inb = 2 }
    inb {
        l = $0
        sub(/\/\/.*$/, "", l); gsub(/\/\*[^*]*\*\//, "", l)
        if (l ~ /^[ \t]*\*/ || l ~ /^[ \t]*\/\*/) next
        why = ""
        if (l ~ /g_state\./) why = "g_state"
        else if (l ~ /g_xpad_(pos_off|touch_off|touch_msk|span|zone_off)/) why = "an x-pad config field"
        else if (l ~ /g_reorder_(hooks|forced_off)/) why = "a reorder flag"
        else if (l ~ /g_mode2?_rgb\[|g_pad_led\[[^]]*\]\[|memcpy\([^,]*, *\(const void \*\) *g_pad_led/) why = "an LED array read directly"
        else if (l ~ /g_drum_[a-z]+_fn\(/) why = "a call through g_drum_*_fn"
        if (why != "") printf "%s:%d: %s reads %s\n", FILENAME, FNR, fn, why
    }' "$G/azmod/bus_interpose.c" "$G/azmod/mixer_input.c") || exit 2
if [ -n "$out" ]; then
    echo "$out" | sed 's/^/FAIL  /'
    exit 1
fi
echo "PASS  busread lint: read() and ioctl() take the Mod Menu settings only from the published words"
