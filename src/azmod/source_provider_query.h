/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_QUERY_H
#define AZMOD_SOURCE_PROVIDER_QUERY_H

#include "source_provider_carrier.h"

typedef int (*azsp_query_prepare_fn)(void *, const char *, int, void **, const char **);

struct azsp_query_scope {
    struct azsp_query_scope *previous;
    void *database;
    char provider_hex[65];
    char search_token[8];
};

/* Install when starting the EP147 2.00 native provider profile. */
int azsp_query_install(int build, azsp_carrier_patch_fn patch);

/* On a library worker, bracket the original native onMessage callback. The
 * core passes the owning SQLite connection and the selected mod's stable ID.
 * End the scope before running any core catalog or import operation. */
void azsp_query_begin(void *server, const void *message, void *db, const char *provider,
                      struct azsp_query_scope *scope);
void azsp_query_end(struct azsp_query_scope *scope);

int azsp_query_prepare(void *db, const char *sql, int len, void **statement, const char **tail,
                       azsp_query_prepare_fn original);

#endif
