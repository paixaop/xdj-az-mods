/* SPDX-License-Identifier: GPL-3.0-only */
/* Run the production filesystem adapter and planner against real directories.
 * Inject only readdir failure to model a USB I/O error after partial progress. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "folder_copy.h"
#include "check.h"

static int fail_after = -1, read_entries, closed_dirs, visited, stop_early;

static struct dirent *test_readdir(DIR *dir) {
    if (fail_after >= 0 && read_entries >= fail_after) {
        errno = EIO;
        return NULL;
    }
    struct dirent *entry = readdir(dir);
    if (entry && strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
        read_entries++;
    return entry;
}

static int test_closedir(DIR *dir) {
    int result = closedir(dir);
    closed_dirs++;
    errno = EBUSY; /* Cleanup must not replace the enumeration result. */
    return result;
}

#define readdir test_readdir
#define closedir test_closedir
#include FOLDERLIST_INC
#undef closedir
#undef readdir

static void reset_reader(int after) {
    fail_after = after;
    read_entries = closed_dirs = visited = stop_early = 0;
    errno = EACCES;
}

static int visit(void *unused, const char *name) {
    (void) unused;
    assert(strcmp(name, ".") && strcmp(name, ".."));
    visited++;
    errno = EACCES;
    return stop_early;
}

static void make_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

int main(int argc, char **argv) {
    ut_suite = "folder enumeration";
    assert(argc == 2);
    char root[FC_PATH_MAX], folder[FC_PATH_MAX], empty[FC_PATH_MAX];
    char first[FC_PATH_MAX], second[FC_PATH_MAX];
    assert(snprintf(root, sizeof root, "%s/source.XXXXXX", argv[1]) < (int) sizeof root);
    assert(mkdtemp(root));
    assert(!fc_join(folder, sizeof folder, root, "Set"));
    assert(!fc_join(empty, sizeof empty, root, "Empty"));
    assert(!fc_join(first, sizeof first, folder, "a.mp3"));
    assert(!fc_join(second, sizeof second, folder, "b.mp3"));
    assert(!mkdir(folder, 0700) && !mkdir(empty, 0700));
    make_file(first, "a");
    make_file(second, "bb");
    struct fc_fs fs = {root, fcr_stat, fcr_list};
    struct fc_item items[8];
    struct fc_plan plan = {.items = items, .cap = 8};

    reset_reader(-1);
    CHECK("complete directory produces both tracks",
          fc_plan_make(&fs, &fs, "Set", "copy", &plan) == 0 &&
              !plan.err && plan.files == 2 && plan.bytes == 3 && closed_dirs == 1);
    reset_reader(-1);
    CHECK("empty directory is successful EOF",
          fcr_list(root, "Empty", visit, NULL) == 0 && !visited && closed_dirs == 1);
    reset_reader(1);
    CHECK("I/O error after one track rejects the incomplete plan",
          fc_plan_make(&fs, &fs, "Set", "copy", &plan) == -1 &&
              plan.err == FC_E_LIST && plan.files == 1 && closed_dirs == 1);
    reset_reader(0);
    CHECK("I/O error before any entries also rejects the plan",
          fc_plan_make(&fs, &fs, "Set", "copy", &plan) == -1 &&
              plan.err == FC_E_LIST && !plan.files && closed_dirs == 1);
    reset_reader(-1);
    CHECK("callback errno does not turn EOF into a read error",
          fcr_list(root, "Set", visit, NULL) == 0 && visited == 2 && closed_dirs == 1);
    reset_reader(-1);
    stop_early = 1;
    CHECK("callback stop succeeds despite callback errno",
          fcr_list(root, "Set", visit, NULL) == 0 && visited == 1 && closed_dirs == 1);
    reset_reader(-1);
    CHECK("missing directory fails without a callback",
          fcr_list(root, "Missing", visit, NULL) == -1 && !visited && !closed_dirs);
    char long_mount[FC_PATH_MAX + 180];
    memset(long_mount, 'x', sizeof long_mount - 1);
    long_mount[sizeof long_mount - 1] = 0;
    CHECK("unrepresentable absolute path fails before opening",
          fcr_list(long_mount, "Set", visit, NULL) == -1 && !visited && !closed_dirs);

    assert(!unlink(first) && !unlink(second));
    assert(!rmdir(folder) && !rmdir(empty) && !rmdir(root));
    return ut_summary();
}
