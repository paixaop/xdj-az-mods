/* SPDX-License-Identifier: GPL-3.0-only */
/* Unsanitized parent: preserve the child's sanitizer and validate its precise fault. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_MAX (1024u * 1024u)
#define TIMEOUT_MS 30000u
enum proof {
    NO_PROOF,
    SIGNAL_PROOF,
    ASAN_PROOF
};
struct expected {
    uint64_t address, begin, end;
    unsigned line;
};
static int consume(const char **p, const char *text) {
    size_t n = strlen(text);
    if (strncmp(*p, text, n)) return 0;
    *p += n;
    return 1;
}
static int hex_value(const char **p, uint64_t *out) {
    if (!consume(p, "0x")) return 0;
    uint64_t value = 0;
    unsigned n = 0;
    while (isxdigit((unsigned char) **p)) {
        if (++n > 16) return 0;
        unsigned char c = (unsigned char) *(*p)++;
        unsigned digit = isdigit(c) ? (unsigned) (c - '0') : (unsigned) tolower(c) - 'a' + 10u;
        value = value * 16u + digit;
    }
    if (!n) return 0;
    *out = value;
    return 1;
}
static int decimal(const char **p, unsigned *out) {
    unsigned value = 0, n = 0;
    while (isdigit((unsigned char) **p)) {
        unsigned digit = (unsigned) (*(*p)++ - '0');
        if (value > (UINT_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        ++n;
    }
    if (!n) return 0;
    *out = value;
    return 1;
}
static int expected_line(const char *p, struct expected *out) {
    if (!consume(&p, "CONTROL_EXPECT wave report: address=") || !hex_value(&p, &out->address) ||
        !consume(&p, " pc_begin=") || !hex_value(&p, &out->begin) ||
        !consume(&p, " pc_end=") || !hex_value(&p, &out->end) ||
        !consume(&p, " source=")) return 0;
    const char *name = strrchr(p, '/');
    p = name ? name + 1 : p;
    if (!consume(&p, "old-probe.inc:") || !decimal(&p, &out->line) || *p) return 0;
    return out->address && out->begin && out->end > out->begin &&
           out->end - out->begin <= 4096 && out->line;
}
static int in_probe(uint64_t pc, const struct expected *out) {
    return pc >= out->begin && pc < out->end;
}
static int top_frame(const char *line, unsigned expected) {
    while (isspace((unsigned char) *line)) ++line;
    if (strncmp(line, "#0 ", 3) || !strstr(line, " in wave_prov_report ")) return 0;
    const char *p = strstr(line, "old-probe.inc:");
    if (!p) return 0;
    p += strlen("old-probe.inc:");
    unsigned value;
    return decimal(&p, &value) && value == expected &&
           (!*p || *p == ':' || isspace((unsigned char) *p));
}
static enum proof validate(const char *text, const char **why) {
    char *copy = strdup(text);
    if (!copy) {
        *why = "allocation failure";
        return NO_PROOF;
    }
    struct expected expected = {0};
    unsigned metadata = 0, signals = 0, errors = 0, frames = 0;
    int read_access = 0, malformed = 0;
    uint64_t signal_address = 0, signal_pc = 0, asan_address = 0, asan_pc = 0;
    char *state = NULL;
    for (char *line = strtok_r(copy, "\n", &state); line; line = strtok_r(NULL, "\n", &state)) {
        const char *p = line;
        if (strstr(line, "runtime error:") || strstr(line, "FAIL wave report:")) malformed = 1;
        if (!strncmp(p, "CONTROL_EXPECT ", 15)) {
            ++metadata;
            if (!expected_line(p, &expected)) malformed = 1;
        }
        if (consume(&p, "CONTROL_FAULT wave report: address=")) {
            ++signals;
            if (!hex_value(&p, &signal_address) || !consume(&p, " pc=") ||
                !hex_value(&p, &signal_pc) || *p) malformed = 1;
        }
        p = strstr(line, "ERROR: AddressSanitizer: ");
        if (p) {
            ++errors;
            p += strlen("ERROR: AddressSanitizer: ");
            if (!consume(&p, "SEGV on unknown address ") || !hex_value(&p, &asan_address) ||
                !consume(&p, " (pc ") || !hex_value(&p, &asan_pc)) malformed = 1;
        }
        if (strstr(line, "The signal is caused by a READ memory access.")) read_access = 1;
        if (top_frame(line, expected.line)) ++frames;
    }
    enum proof result = NO_PROOF;
    if (!malformed && metadata == 1 && signals == 1 && !errors &&
        signal_address == expected.address && in_probe(signal_pc, &expected))
        result = SIGNAL_PROOF;
    if (!malformed && metadata == 1 && !signals && errors == 1 && frames == 1 && read_access &&
        asan_address == expected.address && in_probe(asan_pc, &expected)) result = ASAN_PROOF;
    free(copy);
    *why = result ? "precise probe fault" : "missing or mismatched probe fault evidence";
    return result;
}
static int qualified(enum proof proof, int status) {
    if (proof == SIGNAL_PROOF) return WIFEXITED(status) && WEXITSTATUS(status) == 97;
    if (proof == ASAN_PROOF) return (WIFEXITED(status) && WEXITSTATUS(status) != 0) ||
                                    (WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT || WTERMSIG(status) == SIGSEGV ||
                                                             WTERMSIG(status) == SIGBUS));
    return 0;
}
static int self_test(void) {
    static const char expected[] = "CONTROL_EXPECT wave report: address=0x100080 pc_begin=0x4000 pc_end=0x4020 source=/out/old-probe.inc:17\n";
    static const char signal[] = "CONTROL_FAULT wave report: address=0x100080 pc=0x4010\n";
    static const char asan[] = "==1==ERROR: AddressSanitizer: SEGV on unknown address 0x100080 (pc 0x4010 bp 0x5000 sp 0x5000 T0)\nThe signal is caused by a READ memory access.\n    #0 0x400f in wave_prov_report /out/old-probe.inc:17\n";
    const struct {
        const char *part, *replacement;
    } wrong[] = {
        {"address 0x100080", "address 0x100084"}, {"pc 0x4010", "pc 0x4030"}, {"READ memory", "WRITE memory"}, {"in wave_prov_report ", "in unrelated_fault "}, {"old-probe.inc:17", "old-probe.inc:18"}, {"old-probe.inc:17", "other-probe.inc:17"}};
    char text[4096];
    const char *why;
    snprintf(text, sizeof text, "%s%s", expected, signal);
    if (validate(text, &why) != SIGNAL_PROOF) return 1;
    snprintf(text, sizeof text, "%sCONTROL_FAULT wave report: address=0x100084 pc=0x4010\n", expected);
    if (validate(text, &why) != NO_PROOF) return 1;
    snprintf(text, sizeof text, "%sCONTROL_FAULT wave report: address=0x100080 pc=0x4030\n", expected);
    if (validate(text, &why) != NO_PROOF) return 1;
    snprintf(text, sizeof text, "%s%s", expected, asan);
    if (validate(text, &why) != ASAN_PROOF) return 1;
    for (unsigned i = 0; i < sizeof wrong / sizeof *wrong; ++i) {
        const char *at = strstr(asan, wrong[i].part);
        if (!at) return 1;
        size_t prefix = (size_t) (at - asan);
        snprintf(text, sizeof text, "%s%.*s%s%s", expected, (int) prefix, asan,
                 wrong[i].replacement, at + strlen(wrong[i].part));
        if (validate(text, &why) != NO_PROOF) return 1;
    }
    snprintf(text, sizeof text, "%s%s%s", expected, asan, asan);
    if (validate(text, &why) != NO_PROOF) return 1;
    snprintf(text, sizeof text, "%s%s%s", expected, expected, signal);
    if (validate(text, &why) != NO_PROOF) return 1;
    snprintf(text, sizeof text, "%s%sFAIL wave report: unrelated memory fault\n", expected, signal);
    if (validate(text, &why) != NO_PROOF) return 1;
    if (validate(asan, &why) != NO_PROOF || qualified(ASAN_PROOF, 0) ||
        qualified(SIGNAL_PROOF, 0) || qualified(SIGNAL_PROOF, 1 << 8)) return 1;
    puts("wave report control validator: precise signal/ASan proofs accepted; unrelated faults rejected");
    return 0;
}
static uint64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
    return (uint64_t) value.tv_sec * 1000u + (unsigned long) value.tv_nsec / 1000000u;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test")) return self_test();
    if (argc < 3 || argc > 4) {
        fputs("usage: check-control program log [argument]\n", stderr);
        return 2;
    }
    struct rlimit limit = {0, 0};
    if (setrlimit(RLIMIT_CORE, &limit)) return 2;
    int descriptors[2];
    if (pipe(descriptors)) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0 || dup2(descriptors[1], STDERR_FILENO) < 0) _exit(127);
        if (descriptors[1] != STDOUT_FILENO && descriptors[1] != STDERR_FILENO) close(descriptors[1]);
        char *arguments[] = {argv[1], argc == 4 ? argv[3] : NULL, NULL};
        execv(argv[1], arguments);
        _exit(127);
    }
    close(descriptors[1]);
    char *text = calloc(CAPTURE_MAX + 1u, 1);
    uint64_t start = now_ms();
    size_t count = 0;
    int failed = !text || !start, status = 0, reaped = 0;
    while (!failed) {
        uint64_t now = now_ms();
        if (!now || now - start >= TIMEOUT_MS) {
            failed = 1;
            break;
        }
        struct pollfd descriptor = {descriptors[0], POLLIN, 0};
        int ready = poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (!ready) continue;
        if (count == CAPTURE_MAX) {
            failed = 1;
            break;
        }
        ssize_t got = read(descriptors[0], text + count, CAPTURE_MAX - count);
        if (got < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (!got) break;
        count += (size_t) got;
    }
    close(descriptors[0]);
    while (!failed) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            reaped = 1;
            break;
        }
        if (result < 0 && errno != EINTR) {
            failed = 1;
            break;
        }
        if (now_ms() - start >= TIMEOUT_MS) {
            failed = 1;
            break;
        }
        poll(NULL, 0, 10);
    }
    if (!reaped) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    FILE *log = fopen(argv[2], "wb");
    if (!log || (text && fwrite(text, 1, count, log) != count)) failed = 1;
    if (log && fclose(log)) failed = 1;
    const char *why = "capture, timeout or log failure";
    enum proof proof = !failed ? validate(text, &why) : NO_PROOF;
    int ok = !failed && qualified(proof, status);
    if (!ok) fprintf(stderr, "FAIL wave report control: %s (status=%d)\n%s", why, status, text ? text : "");
    else printf("CONTROL caught: restored widget probe; exact %s address/PC/context\n",
                proof == ASAN_PROOF ? "ASan" : "signal");
    free(text);
    return ok ? 0 : 1;
}
