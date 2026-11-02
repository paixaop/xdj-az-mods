#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-deck-listeners.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/deck.inc" <<'PY'
from pathlib import Path
import sys
text=Path('src/azmod/sdk_deck.c').read_text()
prefix=text[:text.index('/* az_core_v1.deck_info */')]
events=text[text.index('/* ---- on_deck_event'):text.index('static int sd_native_load_read')]
Path(sys.argv[1]).write_text(prefix+'\n'+events)
PY
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -Isrc -Isrc/azmod)
case ${SAN:-plain} in plain|'') ;; asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);;tsan)flags+=(-fsanitize=thread);;*)exit 2;;esac
"${HOST_CC:-cc}" "${flags[@]}" "-DSDKLISTENERS_INC=\"$work/deck.inc\"" tests/unit/sdkdecklisteners_test.c -o "$work/test"
status=0
(ulimit -c 0;"$work/test" failed) >"$work/run.log" 2>&1 || status=$?
cat "$work/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q FAILED_INIT_EVENT_BASELINE_OK "$work/run.log"
    grep -q 'failed init event subscriptions must release later owner capacity' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: failed init owners exhaust actual public listener capacity'
else
    [ "$status" = 0 ]
    "$work/test" loaded
    "$work/test" mixed
fi
