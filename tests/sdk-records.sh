#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-records.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/deck.inc" <<'PY'
from pathlib import Path
import os, sys
p=Path(os.environ.get('AZ_CORE_TEST_ROOT', '.'))/'src/azmod/sdk_deck.c';t=p.read_text()
prefix=t[:t.index('/* az_core_v1.deck_info */')]
events=t[t.index('/* ---- on_deck_event'):]
Path(sys.argv[1]).write_text(prefix+'\n'+events)
PY
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -Isrc/azmod -Isrc)
case ${SAN:-plain} in plain|'') ;; asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);;tsan)flags+=(-fsanitize=thread);; *)exit 2;;esac
"${HOST_CC:-cc}" "${flags[@]}" "-DSDKCUEEVENTS_INC=\"$work/deck.inc\"" tests/unit/sdkrecord_test.c -o "$work/test"
"$work/test"
