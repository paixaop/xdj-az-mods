#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# redirect-lint.sh [GUEST_DIR]: every write into EP147's code or vtables goes through the
# installers in guest/azmod/inline_hooks.c (azm_install_hook, azm_patch_vtable), which store
# the saved original before the redirect and make the redirect one release-ordered store.
# A hand-written "unlock, *slot = hook, *orig = old, relock" elsewhere is how a call lands in
# a hook whose original is still empty (a branch to address 0).
#
# Fails on any azprot_unlock( in guest/azmod/*.c or guest/azstem/*.c outside these functions:
#   guest/azmod/inline_hooks.c   every function (the installers themselves)
#   guest/azstem/azstem.c        install_hook_at (stores *orig before the jump; hooktest checks it)
#   guest/azmod/azmod.c          install_msgtick_hook (original stored with release, then the slot)
#                                usbmod_apply, pngpatch_apply (art bytes in .rodata, no redirect)
#   guest/azmod/waveform.c       apply_ui60 (one instruction word, release store, no original)
# A new exception must be added here, with its reason, after review.
#
# Control (tools/unit-tests/run.sh): the same scan over a copy with a planted hand-written slot
# swap must fail.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
G=$(cd "${1:-$here/../../guest}" && pwd)
bad=0
for f in "$G"/azmod/*.{c,h,cpp} "$G"/azstem/*.{c,h,cpp}; do
    [ -f "$f" ] || continue
    rel=${f#"$G"/}
    out=$(REL="$rel" awk '
        BEGIN {
            allow["azmod/azmod.c:install_msgtick_hook"] = 1
            allow["azmod/azmod.c:usbmod_apply"] = 1
            allow["azmod/azmod.c:pngpatch_apply"] = 1
            allow["azmod/waveform.c:apply_ui60"] = 1
            allow["azstem/azstem.c:install_hook_at"] = 1
            # Reviewed expected-entry check and ordered single-slot store; refuses on
            # protection failure and preserves originals for forward-only rollback.
            allow["azstem/sdk_vtable_patch.h:sdk_patch_slot"] = 1
        }
        /^[a-zA-Z].*[ *]([a-z_][a-z0-9_]*)\(.*\)[^;]*\{[ \t]*$/ {
            fn = $0; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn)
        }
        /^}/ { fn = "" }
        {
            l = $0
            sub(/\/\/.*$/, "", l); gsub(/\/\*[^*]*\*\//, "", l)
            if (l ~ /azprot_unlock[ \t]*\(/ && l !~ /^[ \t]*static int azprot_unlock/) {
                if (ENVIRON["REL"] == "azmod/inline_hooks.c") next
                if ((ENVIRON["REL"] ":" fn) in allow) next
                printf "%s:%d: azprot_unlock in %s(): use azm_patch_vtable / azm_install_hook\n", ENVIRON["REL"], NR, (fn == "" ? "?" : fn)
            }
        }' "$f")
    if [ -n "$out" ]; then
        echo "$out" | sed 's/^/FAIL  /'
        bad=1
    fi
done
if [ "$bad" = 0 ]; then
    echo "PASS  redirect lint: code and vtable redirects only through the ordered installers"
    exit 0
fi
exit 1
