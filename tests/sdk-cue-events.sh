#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-cue-events.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/deck.inc" <<'PY'
from pathlib import Path
import sys
p=Path('src/azmod/sdk_deck.c');t=p.read_text()
prefix=t[:t.index('/* az_core_v1.deck_info */')]
events=t[t.index('/* ---- on_deck_event'):]
Path(sys.argv[1]).write_text(prefix+'\n'+events)
PY
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -Isrc/azmod -Isrc)
case ${SAN:-plain} in plain|'') ;; asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);;tsan)flags+=(-fsanitize=thread);; *)exit 2;;esac
"${HOST_CC:-cc}" "${flags[@]}" "-DSDKCUEEVENTS_INC=\"$work/deck.inc\"" tests/unit/sdkcueevents_test.c -o "$work/test"
status=0
(ulimit -c 0;"$work/test") >"$work/run.log" 2>&1 || status=$?
cat "$work/run.log"
if [ "${EXPECT_CAPABILITY_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q SDK_CUE_EVENT_BASELINE_OK "$work/run.log"
    grep -q 'registered cue observer capability must not require a receipt' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: registered observer lacks capability before its first receipt'
elif [ "${EXPECT_REVISION_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q SDK_CUE_EVENT_BASELINE_OK "$work/run.log"
    grep -q 'missing public cue revision' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: public cue event delivered; public cue revision absent'
elif [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q SDK_CUE_EVENT_BASELINE_OK "$work/run.log"
    grep -q 'missing public native cue-change deck event' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: actual public SDK registration/tick ran; native cue-change event absent'
else exit "$status";fi
