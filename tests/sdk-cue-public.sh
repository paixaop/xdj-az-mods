#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: GNU Linux native listener fixture required'; exit 77; }
work=$(mktemp -d "${TMPDIR:-/tmp}/az-cue-public.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import sys
out=Path(sys.argv[1]); text=Path('src/azmod/sdk_deck.c').read_text()
prefix=text[:text.index('/* az_core_v1.deck_info */')]
events=text[text.index('/* ---- on_deck_event'):]
(out/'deck.inc').write_text(prefix+'\n'+events)
native=Path('tests/unit/sdkcue_test.cpp').read_text()
native=native[:native.index('int main(int argc, char **argv)')]
(out/'native.inc').write_text(native+'\n#endif\n')
PY
flags=(-O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -Isrc -Isrc/azmod -Isrc/common -pthread)
case ${SAN:-plain} in plain|'') ;; asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;tsan)flags+=(-fsanitize=thread -fno-omit-frame-pointer);;*)exit 2;;esac
"${HOST_CC:-gcc}" -std=c11 "${flags[@]}" "-DSDKCUEEVENTS_INC=\"$work/deck.inc\"" -c tests/unit/sdkcuepublic_test.c -o "$work/public.o"
"${HOST_CXX:-g++}" -std=c++11 -rdynamic "${flags[@]}" "-DSDKCUE_NATIVE_FIXTURE=\"$work/native.inc\"" tests/unit/sdkcuepublic_test.cpp src/azstem/sdk_cue_observer.cpp "$work/public.o" -o "$work/test"
"$work/test" 1
"$work/test" 2
