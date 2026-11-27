/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "pvdi-model.h"

#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { path_capacity = 1024 };

static FILE *open_input(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        perror(path);
    return file;
}

#ifdef _WIN32
static FILE *open_reserved_file(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        remove(path);
    }
    return file;
}

static FILE *open_staged_model(const char *directory, char staged[path_capacity])
{
    if (!GetTempFileNameA(directory, "pvd", 0, staged)) {
        fprintf(stderr, "Could not reserve a model file in %s (Windows error %lu)\n",
                directory, GetLastError());
        return NULL;
    }
    return open_reserved_file(staged);
}

static bool replace_model(const char *staged, const char *destination)
{
    if (MoveFileExA(staged, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    fprintf(stderr, "Could not replace %s (Windows error %lu)\n", destination, GetLastError());
    return false;
}
#else
static FILE *open_staged_model(const char *directory, char staged[path_capacity])
{
    int length = snprintf(staged, path_capacity, "%s/.pvdi-import.XXXXXX", directory);
    if (length < 0 || length >= path_capacity) {
        fputs("Model import path is too long\n", stderr);
        return NULL;
    }
    int fd = mkstemp(staged);
    if (fd < 0) {
        perror(staged);
        return NULL;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        perror(staged);
        close(fd);
        remove(staged);
    }
    return file;
}

static bool replace_model(const char *staged, const char *destination)
{
    if (rename(staged, destination) == 0)
        return true;
    perror(destination);
    return false;
}
#endif

static bool output_paths(const char *directory, char destination[path_capacity])
{
    int length = snprintf(destination, path_capacity, "%s/pvdi-model.bin", directory);
    if (length < 0 || length >= path_capacity) {
        fputs("Model import path is too long\n", stderr);
        return false;
    }
    return true;
}

static bool extract_model(FILE *executable, FILE *graph, const char *directory)
{
    char destination[path_capacity], staged[path_capacity];
    if (!output_paths(directory, destination))
        return false;
    FILE *output = open_staged_model(directory, staged);
    if (!output)
        return false;
    bool imported = pvdi_import_windows(executable, graph, output);
    bool closed = fclose(output) == 0;
    if (!closed)
        perror(staged);
    bool ok = imported && closed && replace_model(staged, destination);
    if (ok)
        printf("Created %s\n", destination);
    else
        remove(staged);
    return ok;
}

bool pvdi_create_windows_model(const char *executable_path, const char *graph_path, const char *directory)
{
    FILE *executable = open_input(executable_path);
    if (!executable)
        return false;
    FILE *graph = open_input(graph_path);
    bool ok = graph && extract_model(executable, graph, directory);
    if (graph)
        fclose(graph);
    fclose(executable);
    return ok;
}

#ifdef PVDI_WINDOWS_STANDALONE
int main(int argc, char **argv)
{
    if (argc != 4 || !argv[1][0] || !argv[2][0] || !argv[3][0]) {
        fprintf(stderr, "Usage: %s REKORDBOX_EXE VD_PARAM_PB OUTPUT_DIRECTORY\n", argv[0]);
        return EXIT_FAILURE;
    }
    return pvdi_create_windows_model(argv[1], argv[2], argv[3]) ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
