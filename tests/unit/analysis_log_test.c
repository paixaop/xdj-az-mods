/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"
static char flag_path[1024], log_path[1024];
static int fail_open, debug_calls, g_azlog_on = -1;
#define AZLOG_FLAG flag_path
#define AZLOG_MAX_BYTES 1024
static const char *az_logsfx(void) { return ""; }
static void dbg(const char *format, ...) {
    (void) format;
    debug_calls++;
}
static FILE *fixture_fopen(const char *path, const char *mode) {
    assert(!strcmp(path, "/tmp/azmod-analysis.log"));
    if (fail_open) return NULL;
    return fopen(log_path, mode);
}
#define fopen fixture_fopen
#include ANALYSIS_LOG_INC
#undef fopen
static long read_log(char *text, size_t capacity) {
    FILE *f = fopen(log_path, "r");
    if (!f) return -1;
    size_t n = fread(text, 1, capacity - 1, f);
    text[n] = 0;
    assert(!ferror(f) && !fclose(f));
    return (long) n;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    ut_suite = "analysis-log";
    snprintf(flag_path, sizeof flag_path, "%s/logs.on", argv[1]);
    snprintf(log_path, sizeof log_path, "%s/analysis.log", argv[1]);
    unlink(flag_path);
    unlink(log_path);
    analysis_diagnostic("Deck 1 analysis failed");
    CHECK("disabled normal logs create no file", access(log_path, F_OK) != 0);
    FILE *f = fopen(flag_path, "w");
    assert(f && !fclose(f));
    g_azlog_on = -1;
    analysis_diagnostic("Deck 1 analysis result -1: model unavailable");
    char text[2048];
    CHECK("normal logging writes the complete diagnostic", read_log(text, sizeof text) > 0 &&
                                                               !strcmp(text, "Deck 1 analysis result -1: model unavailable\n"));
    CHECK("normal logging does not invoke debug tracing", debug_calls == 0);
    f = fopen(log_path, "w");
    assert(f);
    for (unsigned i = 0; i <= AZLOG_MAX_BYTES; i++) fputc('x', f);
    assert(!fclose(f));
    analysis_diagnostic("Newest failure survives rotation");
    long size = read_log(text, sizeof text);
    CHECK("existing core cap bounds growth and retains newest error", size > 0 && size < AZLOG_MAX_BYTES &&
                                                                          strstr(text, "log truncated") && strstr(text, "Newest failure survives rotation"));
    fail_open = 1;
    analysis_diagnostic("Unavailable log destination");
    CHECK("unavailable log destination leaves previous diagnostics intact", read_log(text, sizeof text) == size);
    (void) dbg; /* The regression control restores the old debug-only adapter. */
    unlink(flag_path);
    unlink(log_path);
    return ut_summary();
}
