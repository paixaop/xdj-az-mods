/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "azmod-mod.h"
#include "stem_source_rule.h"

static const char *fixture_root, *denied_path;
static int fail_read, source_stats, change_source;
static const char source[] = "/media/usb/A/Music/Song.mp3";
static char g_stems_base[1024];
static pthread_mutex_t g_stems_base_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *physical_path(const char *path, char out[4096]) {
    if (!strncmp(path, "/media", 6) || !strncmp(path, "/run/media", 10) || !strncmp(path, "/mnt", 4)) {
        int n = snprintf(out, 4096, "%s%s", fixture_root, path);
        assert(n > 0 && n < 4096);
        return out;
    }
    return path;
}
static DIR *fixture_opendir(const char *path) {
    if (denied_path && !strcmp(path, denied_path)) {
        errno = EACCES;
        return NULL;
    }
    char mapped[4096];
    return opendir(physical_path(path, mapped));
}
static struct dirent *fixture_readdir(DIR *directory) {
    if (fail_read) {
        fail_read = 0;
        errno = EIO;
        return NULL;
    }
    return readdir(directory);
}
static int fixture_stat(const char *path, struct stat *out) {
    char mapped[4096];
    int result = stat(physical_path(path, mapped), out);
    if (!result && !strcmp(path, source)) {
        ++source_stats;
        if (change_source && source_stats >= 3) ++out->st_size;
    }
    return result;
}
static FILE *fixture_fopen(const char *path, const char *mode) {
    char mapped[4096];
    return fopen(physical_path(path, mapped), mode);
}
static int fixture_access(const char *path, int mode) {
    char mapped[4096];
    return access(physical_path(path, mapped), mode);
}
static void logln(const char *format, ...) { (void) format; }

#define opendir fixture_opendir
#define readdir fixture_readdir
#define stat fixture_stat
#define fopen fixture_fopen
#define access fixture_access
/* Preserve the struct stat tag while replacing calls. */
#undef stat
#define stat(path, out) fixture_stat(path, out)
#define AZ_STATUS_NATIVE_PRODUCER 1
#include FOLDER_INC
#undef opendir
#undef readdir
#undef stat
#undef fopen
#undef access

static void make_directory(const char *path) {
    char mapped[4096], copy[4096];
    snprintf(copy, sizeof copy, "%s", physical_path(path, mapped));
    for (char *at = copy + 1; *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        assert(mkdir(copy, 0700) == 0 || errno == EEXIST);
        *at = '/';
    }
    assert(mkdir(copy, 0700) == 0 || errno == EEXIST);
}
static void write_file(const char *path, const char *text) {
    char mapped[4096];
    FILE *f = fopen(physical_path(path, mapped), "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(!fclose(f));
}
static void expect_folder(const char *expected, int identity, int cached) {
    char folder[1024];
    int ident = -1, complete = 1;
    assert(find_track_folder_any_c("Song", source, folder, sizeof folder, &ident, cached, &complete));
    assert(!strcmp(folder, expected) && ident == identity && complete);
    assert(!pthread_mutex_trylock(&g_nl_mu));
    pthread_mutex_unlock(&g_nl_mu);
}
static void discovery_cases(void) {
    char folder[1024];
    int ident = -1, complete = 1;
    assert(!find_track_folder_any_c(NULL, source, folder, sizeof folder, &ident, 0, &complete));
    assert(!folder[0] && ident == 0);
    make_directory("/media/usb/A/Music");
    write_file(source, "song");
    make_directory("/media/usb/A/StEmS/Song");
    make_directory("/media/usb/A/StEmS/Song (Extended)");
    write_file("/media/usb/A/StEmS/Song/vocals.ogg", "sample");
    expect_folder("/media/usb/A/StEmS/Song", 0, 0);
    int available = -1;
    assert(status_stem_probe(source, &available) == AZ_AVAIL_LOADING && available == -1);
    write_file("/media/usb/A/StEmS/Song/source.json", "{\"file\":\"Music/Song.mp3\",\"size\":4}");
    expect_folder("/media/usb/A/StEmS/Song", 1, 0);
    expect_folder("/media/usb/A/StEmS/Song", 1, 1);
    expect_folder("/media/usb/A/StEmS/Song", 1, 1);
    assert(azstem_has_stems(source) == 1);
    assert(azstem_has_stems(NULL) == -1);
    assert(azstem_has_stems("/missing.mp3") == -1);
    assert(status_stem_probe(source, &available) == AZ_AVAIL_READY && available == 1);
    make_directory("/media/usb/B/STEMS/Song");
    write_file("/media/usb/B/STEMS/Song/source.json", "{\"file\":\"Other.mp3\",\"size\":1000}");
    expect_folder("/media/usb/A/StEmS/Song", 1, 0);
    /* A qualified other-stick candidate outranks a same-stick name-only match. */
    write_file("/media/usb/A/StEmS/Song/source.json", "{}");
    write_file("/media/usb/B/STEMS/Song/source.json", "{\"file\":\"Music/Song.mp3\",\"size\":4}");
    expect_folder("/media/usb/B/STEMS/Song", 1, 0);
    available = -1;
    assert(status_stem_probe(source, &available) == AZ_AVAIL_READY && available == 0);
    char extra[1200];
    snprintf(extra, sizeof extra, "%s/fallback", fixture_root);
    make_directory(extra);
    snprintf(g_stems_base, sizeof g_stems_base, "%s/fallback", fixture_root);
    snprintf(extra, sizeof extra, "%s/fallback/OnlyHere", fixture_root);
    make_directory(extra);
    assert(find_track_folder_any_c("OnlyHere", NULL, folder, sizeof folder, &ident, 0, NULL));
    assert(!strcmp(folder, extra) && ident == 0);
    assert(!find_track_folder_any_c("Absent", source, folder, sizeof folder, &ident, 1, NULL));
}
static void incomplete_cases(void) {
    int available = -1;
    denied_path = "/run/media";
    assert(status_stem_probe(source, &available) == AZ_AVAIL_STALE && available == -1);
    denied_path = NULL;
    fail_read = 1;
    assert(status_stem_probe(source, &available) == AZ_AVAIL_STALE && available == -1);
    source_stats = 0;
    change_source = 1;
    assert(status_stem_probe(source, &available) == AZ_AVAIL_STALE && available == -1);
    change_source = 0;
    assert(status_stem_probe("/missing.mp3", &available) == AZ_AVAIL_STALE);
    assert(status_stem_probe("bad", &available) == AZ_AVAIL_STALE);
    assert(status_stem_probe("/media/usb/A/Music/", &available) == AZ_AVAIL_STALE);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    fixture_root = argv[1];
    discovery_cases();
    incomplete_cases();
    for (unsigned i = 0; i < NL_BASES; ++i) {
        for (int j = 0; j < g_nl[i].n; ++j) free(g_nl[i].names[j]);
        free(g_nl[i].names);
    }
    puts("PASS stem folders: identity/ranking, cached and direct discovery, fallback, incomplete scans and source changes");
    return 0;
}
