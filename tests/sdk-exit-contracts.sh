#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-exit-contracts.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
python3 - "$task_tmp/exit_contracts.inc" <<'PY'
from pathlib import Path
import sys
path = Path('src/azmod/sdk_command_service.c').resolve()
source = path.read_text()
parts = []
for begin, end in [('static int sdk_exit_delivery_receipts(', 'static uint64_t sdk_exit_supported_operations('),
                   ('static int sdk_exit_loop_metadata(', 'static void sdk_exit_cue_revision(')]:
    a, b = source.index(begin), source.index(end)
    parts.append('#line %d "%s"\n' % (source.count('\n', 0, a) + 1, path) + source[a:b])
Path(sys.argv[1]).write_text('\n'.join(parts))
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Isrc/azmod -Isrc/common -I"$task_tmp")
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkexit_contracts_test.c -lm -o "$task_tmp/test"
"$task_tmp/test"
