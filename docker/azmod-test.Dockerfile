# SPDX-License-Identifier: GPL-3.0-only
# azmod-test:18.04: the unit suite's Linux image (the unit tests' run.sh and run-linux.sh).
# azmod-build:18.04 (docker/azmod-build.Dockerfile, the release toolchain) plus what the suites
# need beyond a compiler:
#   libsqlite3-dev  cpspool (the cross-stick spool, linked with -lsqlite3)
#   python3         png (fixture generator), ep147-pngtable and the address-table --check
#   git             the crash-chain control (git show of an older crash_guards.c)
#   docker.io       the aarch64 hook / altstack / fd suites and exports.sh run their builds in
#                   azmod-build:18.04 through the host's docker socket (-v /var/run/docker.sock)
#   rsync           the addrgen controls copy the tree
#   clang-10        libFuzzer harnesses, pinned
#   clang-tidy-10   static analysis, from the same pinned LLVM 10 as clang-10; cppcheck 1.82-1 beside it
#   clang-format    format-check.sh, pinned at 21.1.8: the binary from the PyPI wheel, checked
#                   against its sha256 (ca-certificates only for the download)
# Build azmod-build:18.04 first, then:
#   docker build --platform=linux/arm64 -t azmod-test:18.04 -f docker/azmod-test.Dockerfile docker
FROM azmod-build:18.04
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libsqlite3-dev libxtst-dev python3 git docker.io rsync \
 && rm -rf /var/lib/apt/lists/* \
 && git config --system --add safe.directory '*'
ARG CLANG_FORMAT_VERSION=21.1.8
ARG CLANG_FORMAT_WHEEL=https://files.pythonhosted.org/packages/ab/5d/6720c895c5cfc01831fac55ecf849f10d03d6222700909af14f6047c933b/clang_format-21.1.8-py2.py3-none-manylinux_2_26_aarch64.manylinux_2_28_aarch64.whl
ARG CLANG_FORMAT_SHA256=c2a044953efab5f7d0261f3a76aabc01358592faac1b73af38d8c83db6e91a69
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && python3 -c "import hashlib, io, sys, urllib.request, zipfile; \
d = urllib.request.urlopen(sys.argv[1]).read(); \
h = hashlib.sha256(d).hexdigest(); \
h == sys.argv[2] or sys.exit('clang-format wheel sha256 ' + h + ' != ' + sys.argv[2]); \
open('/usr/local/bin/clang-format', 'wb').write(zipfile.ZipFile(io.BytesIO(d)).read('clang_format/data/bin/clang-format'))" \
        "$CLANG_FORMAT_WHEEL" "$CLANG_FORMAT_SHA256" \
 && chmod 755 /usr/local/bin/clang-format \
 && /usr/local/bin/clang-format --version | grep -q "clang-format version $CLANG_FORMAT_VERSION\$"
ARG CLANG_VERSION=1:10.0.0-4ubuntu1~18.04.2
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-10=$CLANG_VERSION libclang-common-10-dev=$CLANG_VERSION \
 && rm -rf /var/lib/apt/lists/* \
 && test -f /usr/lib/llvm-10/lib/clang/10.0.0/lib/linux/libclang_rt.fuzzer-aarch64.a \
 && clang-10 --version | grep -q 'clang version 10\.0\.0-4ubuntu1~18\.04\.2'
# Static analysis (tools/static-check.sh): clang-tidy from the same LLVM 10 build as clang-10 above
# (one LLVM in the image), and cppcheck, the distribution's 1.82 (the smallest install, no build).
ARG CPPCHECK_VERSION=1.82-1
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-tidy-10=$CLANG_VERSION cppcheck=$CPPCHECK_VERSION \
 && rm -rf /var/lib/apt/lists/* \
 && clang-tidy-10 --version | grep -q 'LLVM version 10\.0\.0' \
 && cppcheck --version | grep -q '^Cppcheck 1\.82$'
