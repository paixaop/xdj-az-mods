#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Asset-independent sanitizer readiness. Compile-only never claims a runtime pass.
set -euo pipefail
cd "$(dirname "$0")/.."
output=; compile_only=0; static_runtimes=0; modes=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) [ "$#" -ge 2 ] || exit 2; output=$2; shift 2 ;;
        --compile-only) compile_only=1; shift ;;
        --static-runtimes) static_runtimes=1; shift ;;
        plain|asan|tsan)
            case " ${modes[*]-} " in *" $1 "*) echo "Duplicate preflight mode: $1" >&2; exit 2 ;; esac
            modes+=("$1"); shift ;;
        *) echo 'Usage: sanitizer-preflight.sh [--output NEW_DIRECTORY] [--compile-only] [--static-runtimes] [plain asan tsan]' >&2; exit 2 ;;
    esac
done
[ "${#modes[@]}" -gt 0 ] || modes=(plain asan tsan)
if [ -n "$output" ]; then
    [ ! -e "$output" ] || { echo 'Preflight output directory must be new' >&2; exit 2; }
    mkdir "$output"
else
    output=$(mktemp -d "${TMPDIR:-/tmp}/az-sanitizer-preflight.XXXXXX")
fi
output=$(cd "$output" && pwd)
printf 'Sanitizer preflight artifacts: %s\n' "$output"
compiler=${HOST_CC:-cc}
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
mkdir "$output/source"
cp tests/unit/sanitizer_preflight.c tests/unit/sanitizer_preflight_timeout.c tests/unit/sanitizer_preflight_run.sh "$output/source/"
"$compiler" "${flags[@]}" "$output/source/sanitizer_preflight_timeout.c" -o "$output/limit"
for mode in "${modes[@]}"; do
    san=()
    case "$mode" in
        asan)
            san=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
            [ "$static_runtimes" -eq 0 ] || san+=(-static-libasan -static-libubsan)
            ;;
        tsan)
            san=(-fsanitize=thread -fno-omit-frame-pointer)
            [ "$static_runtimes" -eq 0 ] || san+=(-static-libtsan)
            ;;
    esac
    "$compiler" "${flags[@]}" "${san[@]}" "$output/source/sanitizer_preflight.c" -o "$output/$mode"
done
cp "$output/source/sanitizer_preflight_run.sh" "$output/run.sh"
python3 - "$output" "$compiler" "$static_runtimes" "${modes[@]}" <<'PY'
from pathlib import Path
import hashlib, json, subprocess, sys
root=Path(sys.argv[1])
files=list((root/'source').iterdir())+[root/'limit']+[root/name for name in sys.argv[4:]]
hashes={str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
version=subprocess.check_output([sys.argv[2],'--version'],universal_newlines=True).splitlines()[0]
(root/'manifest.json').write_text(json.dumps({'scope':'sanitizer runtime preflight, not the full unit suite',
    'compiler':version,'modes':sys.argv[4:],'static_runtimes':sys.argv[3]=='1','sha256':hashes},indent=2)+'\n')
PY
if [ "$compile_only" -eq 1 ]; then
    echo 'PREFLIGHT COMPILED: runtime NOT RUN'
    exit 0
fi
exec sh "$output/run.sh" "$output" "${modes[@]}"
