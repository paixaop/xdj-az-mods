#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Bounded stored cue/status and waveform preview checks; no firmware or deck needed.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-stored.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
args=()
if [ -n "${ANLZ_SAMPLE_DIR:-}" ]; then
    args=("$ANLZ_SAMPLE_DIR/ANLZ0000.DAT" "$ANLZ_SAMPLE_DIR/ANLZ0000.EXT")
    [ ! -f "$ANLZ_SAMPLE_DIR/ANLZ0000.2EX" ] || args+=("$ANLZ_SAMPLE_DIR/ANLZ0000.2EX")
fi
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod tests/unit/sdkstored_test.c -lm -o "$task_tmp/real"
"$task_tmp/real" "${args[@]}"
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod tests/unit/sdkmonopreview_test.c -lm -o "$task_tmp/mono"
mono_args=()
[ -z "${ANLZ_SAMPLE_DIR:-}" ] || mono_args=("$ANLZ_SAMPLE_DIR/ANLZ0000.DAT")
"$task_tmp/mono" "${mono_args[@]}"
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
source=Path('src/azmod'); root=Path(sys.argv[1])
variants=[
    ('short-cue', 'sdk_analysis.h', 'en < 0x28u', 'en < (co2 ? 0x2cu : 0x28u)'),
    ('cue-header', 'sdk_analysis.h', 'hc_be32(e + 4) != (co2 ? 0x10u : 0x1cu)', '0'),
    ('active-status', 'sdk_analysis.h', 'c.auto_active = status == 4;', 'c.auto_active = 0;'),
    ('active-identity', 'sdk_analysis.h', 'if (matches == 1 && active >= 0)', 'if (matches > 0 && active >= 0)'),
    ('memory-color', 'sdk_analysis.h', 'if (kind && en - 0x2c - nc >= 4)', 'if (en - 0x2c - nc >= 4)'),
    ('preview-clock', 'sdk_waveform.h', 'duration_s / index->sample_count', '1.0 / 150.0'),
    ('preview-stride', 'sdk_waveform.h', '{SDKW_PWV4, 6, 0}', '{SDKW_PWV4, 3, 0}'),
    ('preview-header', 'sdk_waveform.h', 'encoding == SDKW_ENCODING_PWV6 || sww_is_mono_preview(encoding) ? 20u : 24u', 'encoding == SDKW_ENCODING_PWV6 || sww_is_mono_preview(encoding) ? 24u : 24u'),
    ('preview-validity', 'sdk_wave_provider.h', 'if (!(data->view.metadata.valid & AZ_META_DURATION))', 'if (0)'),
    ('preview-capacity', 'sdk_analysis.h', 'detail[AZ_WAVE_WINDOW_MAX * 6]', 'detail[AZ_WAVE_WINDOW_MAX * 3]'),
    ('phrase-version', 'sdk_analysis.h', 'stride = hc_be16(t + 14)', 'stride = hc_be32(t + 12)'),
    ('phrase-unknown', 'sdk_analysis.h', 'if (version > 1)', 'if (version > 1000)'),
    ('preview-request', 'sdk_data_engine.h', 'wave->encoding > AZ_WAVE_PWV7_LAYERED_RAW', 'wave->encoding > AZ_WAVE_PWV7_BANDS'),
    ('mono-count', 'sdk_waveform.h', 'if (count != tag->size - tag->header_size)', 'if (0)'),
    ('mono-word', 'sdk_waveform.h', 'if (sww_be32(p + 16) != format.header_word)', 'if (0)'),
    ('mono-clock', 'sdk_waveform.h', 'encoding == SDKW_ENCODING_PWV4 || encoding == SDKW_ENCODING_PWV6 ||\n           encoding == SDKW_ENCODING_PWV2 || encoding == SDKW_ENCODING_PWAV;', 'encoding == SDKW_ENCODING_PWV4 || encoding == SDKW_ENCODING_PWV6;'),
]
for name,filename,old,new in variants:
    target=root/name; target.mkdir()
    for dependency in ['sdk_analysis.h','sdk_data_engine.h','sdk_waveform.h','sdk_wave_provider.h','azmod-mod.h','hc_anlz.h']:
        (target/dependency).write_bytes((source/dependency).read_bytes())
    path=target/filename; text=path.read_text()
    assert text.count(old)==1, (filename, old)
    path.write_text(text.replace(old,new))
PY
for control in short-cue cue-header active-status active-identity memory-color preview-clock preview-stride preview-header preview-validity preview-capacity phrase-version phrase-unknown preview-request; do
    ${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp/$control" -Isrc/azmod tests/unit/sdkstored_test.c -lm -o "$task_tmp/$control-test"
    if ( ulimit -c 0; "$task_tmp/$control-test" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control control was not caught" >&2; exit 1
    fi
    grep -q 'FAIL stored:' "$task_tmp/$control.log" || { echo "FAIL: $control lacked its required assertion" >&2; exit 1; }
    echo "CONTROL caught: $control"
done
for control in mono-count mono-word mono-clock; do
    ${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp/$control" -Isrc/azmod tests/unit/sdkmonopreview_test.c -lm -o "$task_tmp/$control-test"
    if ( ulimit -c 0; "$task_tmp/$control-test" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control control was not caught" >&2; exit 1
    fi
    grep -qi 'assert' "$task_tmp/$control.log" || { echo "FAIL: $control lacked its assertion" >&2; exit 1; }
    echo "CONTROL caught: $control"
done
