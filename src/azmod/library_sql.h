/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_LIBRARY_SQL_H
#define AZ_LIBRARY_SQL_H
#pragma GCC visibility push(hidden)
struct az_library_sql {
    int (*open_v2)(const char *, void **, int, const char *);
    int (*key)(void *, const void *, int);
    int (*close)(void *);
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*bind_int64)(void *, int, long long);
    int (*step)(void *);
    long long (*col_int64)(void *, int);
    const unsigned char *(*col_text)(void *, int);
    int (*finalize)(void *);
    int ok;
};
const struct az_library_sql *az_storage_sql(void);
int az_storage_sql_ready(void);
void *sdk_db_open(const char *database);
#pragma GCC visibility pop
#endif
