#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -pthread -I"$root/src/azmod" \
  "$root/tests/unit/sdk_status_test.c" -lm -o "$work/status"
"$work/status"
"$work/status" memory-budget
"$work/status" stem-source
"$work/status" payload-identity
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -O1 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -pthread -I"$root/src/azmod" \
  "-DDS_METRICS_PATH=\"$work/proof.stats\"" "$root/tests/unit/sdk_status_proof_test.c" -lm -o "$work/proof"
"$work/proof"
if [ "$(uname -s)" = Linux ]; then
  "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -pthread -I"$root/src/azmod" \
    "-DCM_ROOT=\"$work\"" "-DDS_METRICS_PATH=\"$work/worker.stats\"" \
    "$root/tests/unit/sdk_status_metrics_worker_test.c" -Wl,--wrap=pthread_create,--wrap=nanosleep -lm -o "$work/metrics"
  "$work/metrics"
fi
