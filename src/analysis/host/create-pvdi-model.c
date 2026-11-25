/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "pvdi-model.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct import_paths {
    char executable[PATH_MAX], graph[PATH_MAX], output[PATH_MAX], temporary[PATH_MAX];
    char arm64[PATH_MAX], staged_model[PATH_MAX];
};

static bool append_path(char path[PATH_MAX], const char *base, const char *suffix)
{
    int length = snprintf(path, PATH_MAX, "%s%s", base, suffix);
    return length >= 0 && length < PATH_MAX;
}

static bool prepare_paths(struct import_paths *paths, const char *app, const char *output_dir)
{
    return append_path(paths->executable, app, "/Contents/MacOS/rekordbox") &&
           append_path(paths->graph, app, "/Contents/Resources/vd_param_raw.pb") &&
           append_path(paths->output, output_dir, "/pvdi-model.bin") &&
           append_path(paths->temporary, output_dir, "/.pvdi-import.XXXXXX");
}

static bool run_tool(const char *tool, char *const args[])
{
    pid_t pid;
    int error = posix_spawnp(&pid, tool, NULL, NULL, args, environ);
    if (error) {
        fprintf(stderr, "%s: %s\n", tool, strerror(error));
        return false;
    }
    int status;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        perror("waitpid");
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool extract_model(struct import_paths *paths, const struct pvdi_profile *profile)
{
    char *thin[] = {"lipo", paths->executable, "-thin", "arm64", "-output", paths->arm64, NULL};
    return run_tool("lipo", thin) && pvdi_import(profile, paths->graph, paths->arm64, paths->staged_model);
}

static bool publish_model(struct import_paths *paths, const struct pvdi_profile *profile)
{
    if (!extract_model(paths, profile))
        return false;
    if (rename(paths->staged_model, paths->output)) {
        perror(paths->output);
        return false;
    }
    printf("Created %s\n", paths->output);
    return true;
}

static bool create_model(struct import_paths *paths, const struct pvdi_profile *profile)
{
    umask(0077);
    if (!mkdtemp(paths->temporary)) {
        perror(paths->temporary);
        return false;
    }
    append_path(paths->arm64, paths->temporary, "/arm64");
    append_path(paths->staged_model, paths->temporary, "/pvdi-model.bin");
    bool success = publish_model(paths, profile);
    unlink(paths->arm64);
    unlink(paths->staged_model);
    rmdir(paths->temporary);
    return success;
}

int main(int argc, char **argv)
{
    if (argc != 3 || !argv[1][0] || !argv[2][0]) {
        fprintf(stderr, "Usage: %s REKORDBOX_APP OUTPUT_DIRECTORY\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct import_paths paths;
    if (!prepare_paths(&paths, argv[1], argv[2]) ||
        strlen(paths.temporary) + sizeof "/pvdi-model.bin" > PATH_MAX) {
        fputs("Model import path is too long\n", stderr);
        return EXIT_FAILURE;
    }
    const struct pvdi_profile *profile = pvdi_profile_for_app(argv[1]);
    return profile && create_model(&paths, profile) ? EXIT_SUCCESS : EXIT_FAILURE;
}
