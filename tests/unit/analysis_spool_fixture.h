/* SPDX-License-Identifier: GPL-3.0-only */
static struct {
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*step)(void *);
    int (*finalize)(void *);
    int (*get_autocommit)(void *);
} g_sq;
static int lib_backup(void *connection, FILE *log) {
    (void) connection;
    (void) log;
    return 0;
}
static void lib_invalidate(uintptr_t cache, void *connection, FILE *log) {
    (void) cache;
    (void) connection;
    (void) log;
}
static void ls_pdb_sync(void *connection, const char *path, const char *hint, FILE *log) {
    (void) connection;
    (void) path;
    (void) hint;
    (void) log;
}
static int ls_art_fix(void *connection, const char *root, const char *buf, size_t size, FILE *log) {
    (void) connection;
    (void) root;
    (void) buf;
    (void) size;
    (void) log;
    return 0;
}
#include LSAPPLY_INC
static int apply_analysis_spool(void *database, const char *path) {
    *(void **) &g_sq.prepare = (void *) sqlite3_prepare_v2;
    *(void **) &g_sq.step = (void *) sqlite3_step;
    *(void **) &g_sq.finalize = (void *) sqlite3_finalize;
    *(void **) &g_sq.get_autocommit = (void *) sqlite3_get_autocommit;
    char *bytes = malloc(LS_SPOOL_CAP);
    assert(bytes);
    int rc = ls_apply_buf(database, 0, path, bytes, NULL);
    free(bytes);
    return rc;
}
static void await_link(analysis_source_input *input, int expected) {
    int rc = AZ_PENDING;
    for (unsigned i = 0; i < 3000; ++i) {
        rc = analysis_source_outcome(input->owner, input);
        if (rc != AZ_PENDING && rc != AZ_BUSY) break;
        usleep(1000);
    }
    assert(rc == expected);
}
