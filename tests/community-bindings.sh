#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-community-bindings.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
python3 - "$task_tmp/community_buttons.inc" <<'EXTRACT'
from pathlib import Path
import sys
source = Path('src/azmod/community.c').resolve()
text = source.read_text()
start = text.index('static int cm_button_press(')
end = text.index('\n#endif', start)
Path(sys.argv[1]).write_text('#line %d "%s"\n' % (text.count('\n', 0, start) + 1, source) + text[start:end] + '\n')
EXTRACT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -fno-optimize-sibling-calls -Isrc/azmod -I"$task_tmp" -pthread -rdynamic)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" -DCM_ROOT="\"$task_tmp/root\"" -DCM_LOGDIR="\"$task_tmp/log\"" -DCM_USB_ROOT="\"$task_tmp/usb\"" \
 tests/unit/community_bindings_test.c src/azmod/sdk_status_engine.c -ldl -lm -o "$task_tmp/test"
"$task_tmp/test"
