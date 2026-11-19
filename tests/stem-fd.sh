#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Exercise actual procfs descriptor ownership during library close/eject.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Linux ]]; then
    echo 'SKIP: descriptor ownership requires Linux procfs'; exit 77
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work/fds.inc" <<'PY'
from pathlib import Path
import json,sys
path=Path('src/azmod/stem_marks.c');source=path.read_text()
start=source.index('static int sm_fds_on(');end=source.index('\n}',start)+2
Path(sys.argv[1]).write_text('#line {} {}\n'.format(source[:start].count('\n')+1,json.dumps(str(path.resolve())))+source[start:end]+'\n')
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-gcc}" "${flags[@]}" -DSTEM_FD_IMPL_INC="\"$work/fds.inc\"" tests/unit/stem_fd_test.c -Wl,--wrap=opendir,--wrap=readlink -ldl -o "$work/fds"
"$work/fds"
