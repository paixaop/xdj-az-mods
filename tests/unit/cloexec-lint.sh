#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# cloexec-lint.sh [GUEST_DIR]: every descriptor azmod/azstem open inside EP147 must be
# close-on-exec. EP147 forks children all the time (sh -c ping ...), and a child inherits
# every fd without FD_CLOEXEC: a held file on the stick keeps its FAT volume busy during an
# eject, and the eject poller only counts EP147's own fds, so it never sees the child's.
#
# Fails on, in guest/azmod/*.c, guest/azmod/dev/*.c, guest/azstem/*.c, guest/common/*.h:
#   fopen(..., "<mode>")        whose mode literal has no 'e'
#   fopen(..., <non-literal>)   (cannot be checked)
#   open/open64/openat/next_open(...) without O_CLOEXEC
#   socket/socketpair(...)      without SOCK_CLOEXEC
#   accept/pipe/dup/dup2/popen  (use accept4/pipe2/F_DUPFD_CLOEXEC/dup3 with the flag)
# A justified exception carries the marker "cloexec-ok: <why>" on the same line.
#
# Control: the same scan over a copy with a planted fopen "rb", a planted open() without
# O_CLOEXEC and a planted socket() without SOCK_CLOEXEC must report exactly those three.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
G=$(cd "${1:-$here/../../guest}" && pwd)

scan() { # dir -> prints violations, exit 1 if any
    local d=$1
    local files=()
    for f in "$d"/azmod/*.c "$d"/azmod/dev/*.c "$d"/azstem/*.c "$d"/common/*.h; do
        [ -f "$f" ] && files+=("$f")
    done
    perl -ne '
        BEGIN { $bad = 0 }
        $ok = /cloexec-ok:/;
        $l = $_;
        if ($inc) { if ($l =~ s{^.*?\*/}{}) { $inc = 0 } else { $l = "" } }
        $l =~ s{/\*.*?\*/}{}g;                             # one-line block comments
        if ($l =~ s{/\*.*$}{}) { $inc = 1 }                # block comment opens here
        $l =~ s{//.*$}{} unless $l =~ m{"[^"]*//};         # line comment (not inside a string)
        $l = "" if $ok;
        my @why;
        (my $c = $l) =~ s/"(?:[^"\\]|\\.)*"/""/g;       # strings blanked for the call checks
        while ($l =~ /\bfopen\s*\(((?:[^;"]|"[^"]*")*?),\s*("([^"]*)"|[^,)]+)\s*\)/g) {
            if (!defined $3) { push @why, "fopen with a non-literal mode" }
            elsif ($3 !~ /e/) { push @why, "fopen mode \"$3\" lacks e" }
        }
        if ($c =~ /(?<![\w.>])(?:next_)?open(?:64|at)?\s*\(/ && $c !~ /^\s*(?:bool|int|static|extern)\b[^=]*\bopen(?:64)?\s*\(\s*const/
            && $c !~ /O_CLOEXEC/) { push @why, "open without O_CLOEXEC" }
        if ($c =~ /\bsocket(?:pair)?\s*\(/ && $c !~ /SOCK_CLOEXEC/) { push @why, "socket without SOCK_CLOEXEC" }
        if ($c =~ /(?<![\w.>])(accept|pipe|dup|dup2|popen)\s*\(/) { push @why, "$1() creates an inheritable fd" }
        if (@why) { $bad++; chomp; printf "  %s:%d: %s\n      %s\n", $ARGV, $., join("; ", @why), $_ }
        if (eof) { close ARGV; $inc = 0 }                  # $. and comment state per file
        END { exit($bad ? 1 : 0) }
    ' "${files[@]}"
}

fail=0
if out=$(scan "$G"); then
    echo "PASS  cloexec: every fopen/open/socket in azmod + azstem is close-on-exec"
else
    echo "FAIL  cloexec: descriptors EP147's children would inherit:"; echo "$out"; fail=1
fi

# control
tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cloexec.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/azmod/dev" "$tmp/azstem" "$tmp/common"
cp "$G"/azmod/*.c "$tmp/azmod/"; cp "$G"/azmod/dev/*.c "$tmp/azmod/dev/" 2>/dev/null
cp "$G"/azstem/*.c "$tmp/azstem/"; cp "$G"/common/*.h "$tmp/common/"
cat >> "$tmp/azstem/drumroll.c" <<'EOF'
static void planted_a(void) { FILE *f = fopen("/tmp/x", "rb"); if (f) fclose(f); }
static void planted_b(void) { int fd = open("/tmp/x", O_RDONLY); if (fd >= 0) close(fd); }
static void planted_c(void) { int s = socket(AF_INET, SOCK_DGRAM, 0); if (s >= 0) close(s); }
EOF
cout=$(scan "$tmp"); crc=$?
n=$(printf '%s\n' "$cout" | grep -c 'planted_')
if [ $crc -ne 0 ] && [ "$n" -eq 3 ]; then
    echo "CONTROL FAILED AS REQUIRED  cloexec: planted fopen \"rb\", open() and socket() all reported"
else
    echo "CONTROL DID NOT FAIL  cloexec: planted violations reported $n/3 (instrument broken)"; fail=1
fi
[ $fail = 0 ] && echo "CLOEXEC LINT: OK" || echo "CLOEXEC LINT: FAILED"
exit $fail
