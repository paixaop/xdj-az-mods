#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/azmod)
case ${SAN:-plain} in plain) ;; asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);; *) exit 2;; esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdksource_browser_native_test.c -o "$work/test"
"$work/test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdk_source_browser_test.c src/azmod/source_provider.c -o "$work/service-test"
"$work/service-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_test.c src/azmod/source_provider.c -o "$work/provider-test"
"$work/provider-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_import_test.c src/azmod/source_provider.c \
    src/azmod/source_import_files.c src/azmod/source_import_sql.c src/azmod/source_import_publish.c \
    src/azmod/source_provider_catalog.c src/azmod/source_provider_stream.c src/azmod/source_import_analyzer.c \
    src/azmod/track_storage.c -lsqlite3 -lm -o "$work/import-test"
"$work/import-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_import_queue_test.c -o "$work/import-queue-test"
"$work/import-queue-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_import_worker_test.c -o "$work/import-worker-test"
"$work/import-worker-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_import_analyzer_test.c src/azmod/track_storage.c -lm -o "$work/import-analyzer-test"
"$work/import-analyzer-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_dispatch_test.c -o "$work/source-dispatch-test"
"$work/source-dispatch-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_cache_test.c src/azmod/source_provider.c \
    src/azmod/source_provider_cache.c -o "$work/source-cache-test"
"$work/source-cache-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_query_test.c src/azmod/source_provider_query.c \
    src/azmod/source_provider_catalog.c -lsqlite3 -o "$work/source-query-test"
"$work/source-query-test" "$work"
runtime_libs=()
if [ "$(uname -s)" = Linux ]; then runtime_libs+=(-ldl); fi
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_runtime_test.c "${runtime_libs[@]}" -o "$work/source-runtime-test"
"$work/source-runtime-test"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_lifetime_test.c -o "$work/source-lifetime-test"
"$work/source-lifetime-test"
if [[ $(uname -s) == Linux && $(uname -m) == aarch64 ]]; then
    "${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_carrier_test.c -ldl -o "$work/source-carrier-test"
    "$work/source-carrier-test"
fi
