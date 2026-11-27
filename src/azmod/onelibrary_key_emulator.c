/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file onelibrary_key_emulator.c
 * @brief Emulator-only: supplies the OneLibrary key so the stick's exportLibrary.db opens.
 *
 * EP147 opens PIONEER/rekordbox/exportLibrary.db through libsqlcipher with a passphrase
 * derived from provisioned state. The emulator lacks that state, so sqlite3_key arrives
 * empty, every decrypt fails and the stick mounts as folders only. The sqlite3_key hook
 * substitutes the OneLibrary key when the supplied key is empty and otherwise forwards
 * unchanged; the open/exec/prepare hooks only log paths and PRAGMAs. The key material
 * (onelibkey.h) is never logged.
 *
 * Every hook is a plain pass-through while azmod is inert or the flag
 * /home/root/settings/azmod-onelibrarykey.on is absent (read once, lazily). That flag
 * belongs on the emulator image only: never create it on a real deck, where it would
 * force a wrong key into EP147's DB connections. sqlite3_prepare_v2 also carries an
 * independent opt-in trace (azmod-sqltrace.on) of playlist SQL to /tmp/azmod-sql.log.
 */
#include "onelibkey.h" /* static const char ONELIBRARY_KEY[]: kept out of the public source */
static volatile int g_onelib_fix = -1; /* -1 until the flag file is read */
static int onelib_fix_enabled(void) {
    if (g_onelib_fix < 0)
        g_onelib_fix = (access("/home/root/settings/azmod-onelibrarykey.on", F_OK) == 0);
    return g_onelib_fix;
}
static int (*next_sqlite3_key)(void *, const void *, int);
static int (*next_sqlite3_exec)(void *, const char *, void *, void *, void **);
int sqlite3_key(void *db, const void *pkey, int nkey) {
    NEXT(next_sqlite3_key, "sqlite3_key");
    if (g_azm_inert)
        return next_sqlite3_key(db, pkey, nkey);
    if (!onelib_fix_enabled())
        return next_sqlite3_key(db, pkey, nkey);
    NEXT(next_sqlite3_exec, "sqlite3_exec");
    if (nkey <= 0 || !pkey) {
        dbg("[onelibrary] sqlite3_key: empty (len=%d) -> substituting OneLibrary key\n", nkey);
        pkey = ONELIBRARY_KEY;
        nkey = (int) sizeof(ONELIBRARY_KEY) - 1;
    } else {
        dbg("[onelibrary] sqlite3_key: len=%d (material not logged)\n", nkey);
    }
    return next_sqlite3_key(db, pkey, nkey);
}
static int (*next_sqlite3_open_v2)(const char *, void **, int, const char *);
int sqlite3_open_v2(const char *path, void **db, int flags, const char *vfs) {
    NEXT(next_sqlite3_open_v2, "sqlite3_open_v2");
    if (g_azm_inert)
        return next_sqlite3_open_v2(path, db, flags, vfs);
    if (!onelib_fix_enabled())
        return next_sqlite3_open_v2(path, db, flags, vfs);
    if (path)
        dbg("[onelibrary] sqlite3_open_v2: '%s' flags=%d\n", path, flags);
    return next_sqlite3_open_v2(path, db, flags, vfs);
}
static int (*next_sqlite3_exec)(void *, const char *, void *, void *, void **);
int sqlite3_exec(void *db, const char *sql, void *cb, void *arg, void **errmsg) {
    NEXT(next_sqlite3_exec, "sqlite3_exec");
    if (g_azm_inert)
        return next_sqlite3_exec(db, sql, cb, arg, errmsg);
    if (!onelib_fix_enabled())
        return next_sqlite3_exec(db, sql, cb, arg, errmsg);
    if (sql && (sql[0] == 'P' || sql[0] == 'p')) /* PRAGMAs only: keep the log lean */
        dbg("[onelibrary] sqlite3_exec: '%s'\n", sql);
    return next_sqlite3_exec(db, sql, cb, arg, errmsg);
}
static void onelib_log_sql(void *db, const char *sql, int n) {
    FILE *f = azlog_open("/tmp/azmod-sql.log", "a");
    if (!f)
        return;
    int length = n >= 0 && n < 4000 ? n : 4000;
    fprintf(f, "[%ld] db=%lx %.*s\n", (long) syscall(SYS_gettid), (unsigned long) (uintptr_t) db, length, sql);
    fclose(f);
}

static void onelib_trace_sql(void *db, const char *sql, int n) {
    static int on = -1;
    static _Atomic int budget = 300;
    if (on < 0)
        on = AZ_PROBE_ON("/home/root/settings/azmod-sqltrace.on");
    if (on && sql && strstr(sql, "laylist") && __atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED) > 0)
        onelib_log_sql(db, sql, n);
}

static azsp_query_prepare_fn next_sqlite3_prepare_v2;
int sqlite3_prepare_v2(void *db, const char *sql, int n, void **stmt, const char **tail) {
    NEXT(next_sqlite3_prepare_v2, "sqlite3_prepare_v2");
    if (g_azm_inert)
        return next_sqlite3_prepare_v2(db, sql, n, stmt, tail);
    onelib_trace_sql(db, sql, n);
    if (onelib_fix_enabled() && sql && sql[0] == 'P')
        dbg("[onelibrary] sqlite3_prepare_v2: '%.*s'\n", n < 0 ? (int) strlen(sql) : n, sql);
    return azsp_query_prepare(db, sql, n, stmt, tail, next_sqlite3_prepare_v2);
}

extern int g_hidinj_peer, g_hidinj_reads; /* azmod-hidinject.on, block at EOF */
void hidinj_read_note(ssize_t r, size_t asked, const unsigned char *b, uintptr_t ra);



