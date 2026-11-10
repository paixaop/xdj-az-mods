#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Supply a checkout of the core that predates the deck-status service.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
legacy=${1:?usage: sdk-status-compat.sh OLD_CORE_CHECKOUT}
old=$(cd "$legacy/src/azmod" && pwd)
new=$root/src/azmod
test -f "$old/test/community/harness.c"
if grep -q 'on_deck_status' "$old/azmod-mod.h"; then
    echo "The legacy header already supplies deck status" >&2
    exit 2
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -O2 -fPIC -shared -fvisibility=hidden -I"$old" "$root/tests/unit/sdkstatus_legacy_mod.c" -o "$work/legacy-status-client.so"
cc -O2 -fPIC -shared -fvisibility=hidden -I"$new" "$new/test/community/status_mod.c" -o "$work/status-watch.so"
for provider in old new; do
    src=$new
    if [[ $provider == old ]]; then src=$old; fi
    headers=(-include stdatomic.h)
    if [[ $provider == old ]] && ! grep -q 'static int bpmstate_current' "$src/test/community/harness.c"; then
        # This baseline's standalone loader harness omits the optional GUI BPM owner.
        printf '%s\n' 'static int bpmstate_current(int d, unsigned long generation) {(void)d;(void)generation;return 0;}' > "$work/legacy_gui.h"
        headers+=(-include "$work/legacy_gui.h")
    fi
    stage="$work/$provider/az-usbload/community"
    trust="$work/$provider/azmod-community"
    mkdir -p "$stage" "$trust"
    touch "$trust/.approved-init"
    for id in legacy-status-client status-watch; do
        mkdir "$stage/$id"; cp "$work/$id.so" "$stage/$id/$id.so"
        printf '{"azmod":1,"id":"%s","name":"%s","version":"1.0.0","api":1,"author":"test","summary":"ABI test","source":"https://example.com/x","firmware":["any"],"hooks":false}\n' "$id" "$id" > "$stage/$id/azmod.json"
        printf '%s\n' "$id" >> "$stage/enabled"
        printf '%s %s\n' "$id" "$(sha256sum < "$work/$id.so" | cut -d' ' -f1)" >> "$trust/approved"
    done
    cc -O2 -no-pie "${headers[@]}" -I"$src" "-DCM_ROOT=\"$work/$provider\"" "-DCM_LOGDIR=\"$work/log-$provider\"" \
        "$src/test/community/harness.c" -pthread -ldl -lm -rdynamic -o "$work/host"
    "$work/host" init > "$work/$provider.trace"
    if ! grep -q '^LEGACY setting round trip$' "$work/$provider.trace"; then
        cat "$work/$provider.trace" >&2
        exit 1
    fi
    grep -Fx 'LEGACY setting round trip' "$work/$provider.trace"
    grep '^MOD legacy-status-client state=1' "$work/$provider.trace"
    if [[ $provider == old ]]; then
        grep '^MOD status-watch ' "$work/$provider.trace" | grep -i 'init'
        if grep -q '^MOD status-watch state=1' "$work/$provider.trace"; then exit 1; fi
    else
        grep '^MOD status-watch state=1' "$work/$provider.trace"
    fi
done
