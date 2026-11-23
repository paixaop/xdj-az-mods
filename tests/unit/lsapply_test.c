/* SPDX-License-Identifier: GPL-3.0-only */
/* The library spool's applier (ls_apply_buf and ls_allowed, extracted from
 * guest/azmod/library_edits.c by run.sh; lp_exec from library_db.c), against a real SQLite
 * library. A spool is a file on a stick anyone can write: <stick>/PIONEER/azspool/<name>.sql.
 * The applier is the only gate between it and the user's rekordbox library, so:
 *   - an allowed spool (INSERT / INSERT OR IGNORE / UPDATE on the library tables, one per line,
 *     each ending in ';', blank and "--" lines skipped, CRLF taken) is applied whole, in one
 *     transaction, then deleted, and the lists are re-read;
 *   - any other line (another statement, another table, a table whose name only starts like
 *     one, a second statement after ';', no final ';') rejects the whole file before anything
 *     is written: renamed .rejected;
 *   - the second pass runs exactly the statements the first pass checked: a byte the second
 *     pass would split a line at, or a NUL that would hide the rest of the file, rejects it,
 *     and a trimmed line does not end it early;
 *   - a statement that fails rolls everything back (.failed); EP147 inside a transaction of its
 *     own leaves the spool for later; no backup of the library, nothing applied;
 *   - a file at or over the 1 MB cap is rejected unread.
 * argv[1] = a scratch directory. */
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

static struct {
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*step)(void *);
    int (*finalize)(void *);
    int (*get_autocommit)(void *);
} g_sq;

static int g_backup_rc, g_backups, g_invalidates, g_pdb_syncs, g_art_fixes;
static int lib_backup(void *conn, FILE *lg) {
    (void) conn, (void) lg;
    g_backups++;
    return g_backup_rc;
}
static void lib_invalidate(uintptr_t cache, void *conn, FILE *lg) {
    (void) cache, (void) conn, (void) lg;
    g_invalidates++;
}
static void ls_pdb_sync(void *conn, const char *spool, const char *hint, FILE *lg) {
    (void) conn, (void) spool, (void) hint, (void) lg;
    g_pdb_syncs++;
}
static int ls_art_fix(void *conn, const char *root, const char *buf, size_t n, FILE *lg) {
    (void) conn, (void) root, (void) buf, (void) n, (void) lg;
    g_art_fixes++;
    return 0;
}
#include LSAPPLY_INC

static sqlite3 *g_db;
static char g_dir[512], g_spool[600];

static const char *SCHEMA =
    "CREATE TABLE content(content_id INTEGER PRIMARY KEY, title, path);"
    "CREATE TABLE playlist(playlist_id INTEGER PRIMARY KEY, name);"
    "CREATE TABLE playlist_content(playlist_id, content_id, sequenceNo);"
    "CREATE TABLE contentx(a);"
    "CREATE TABLE agentRegistry(registry_id INTEGER PRIMARY KEY, str_1);"
    "INSERT INTO content VALUES(1,'kept','/Contents/a.mp3');"
    "INSERT INTO agentRegistry VALUES(1,'kept');";

static long long count(const char *sql) {
    sqlite3_stmt *st = 0;
    long long v = -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}
static void fresh(void) {
    if (g_db)
        sqlite3_close(g_db);
    sqlite3_open(":memory:", &g_db);
    sqlite3_exec(g_db, SCHEMA, 0, 0, 0);
    g_backup_rc = g_backups = g_invalidates = g_pdb_syncs = g_art_fixes = 0;
    char p[700];
    unlink(g_spool);
    snprintf(p, sizeof p, "%s.rejected", g_spool);
    unlink(p);
    snprintf(p, sizeof p, "%s.failed", g_spool);
    unlink(p);
}
static void put(const char *data, size_t n) {
    FILE *f = fopen(g_spool, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}
static int apply(const char *text) {
    put(text, strlen(text));
    char *buf = malloc(LS_SPOOL_CAP);
    int rc = ls_apply_buf(g_db, 0, g_spool, buf, NULL);
    free(buf);
    return rc;
}
static int exists(const char *suffix) {
    char p[700];
    snprintf(p, sizeof p, "%s%s", g_spool, suffix);
    return access(p, F_OK) == 0;
}
/* the library as fresh() made it: nothing of the spool's written */
static int untouched(void) {
    return count("SELECT count(*) FROM content") == 1 && count("SELECT count(*) FROM playlist") == 0 &&
           count("SELECT count(*) FROM agentRegistry WHERE str_1='kept'") == 1 &&
           count("SELECT count(*) FROM contentx") == 0;
}
/* a refused spool: rejected before the backup, nothing written */
static int rejected(int rc) {
    return rc == -3 && exists(".rejected") && !exists("") && g_backups == 0 && untouched();
}

static int reject_commit(void *unused) {
    (void) unused;
    return 1;
}
int main(int argc, char **argv) {
    ut_suite = "spool apply";
    if (argc < 2)
        return 2;
    snprintf(g_dir, sizeof g_dir, "%s/PIONEER/azspool", argv[1]);
    char mk[600];
    snprintf(mk, sizeof mk, "%s/PIONEER", argv[1]);
    mkdir(argv[1], 0755);
    mkdir(mk, 0755);
    mkdir(g_dir, 0755);
    snprintf(g_spool, sizeof g_spool, "%s/x.sql", g_dir);
    *(void **) &g_sq.prepare = (void *) sqlite3_prepare_v2;
    *(void **) &g_sq.step = (void *) sqlite3_step;
    *(void **) &g_sq.finalize = (void *) sqlite3_finalize;
    *(void **) &g_sq.get_autocommit = (void *) sqlite3_get_autocommit;

    /* ---- normal use */
    fresh();
    int rc = apply("-- AZMOD cross-stick copy from /media/usb/STICK\n"
                   "\n"
                   "INSERT INTO content VALUES(2,'two','/Contents/b.mp3');\r\n"
                   "  INSERT OR IGNORE INTO content VALUES(1,'dup','/x');\n"
                   "INSERT INTO playlist(playlist_id, name) VALUES(5,'new list');\n"
                   "INSERT INTO playlist_content VALUES(5,2,1);\n"
                   "UPDATE content SET title='renamed' WHERE content_id=1;\n");
    CHECK("an allowed spool is applied: 0", rc == 0);
    CHECK("its rows are in the library", count("SELECT count(*) FROM content") == 2 &&
                                             count("SELECT count(*) FROM playlist_content") == 1 &&
                                             count("SELECT count(*) FROM content WHERE title='renamed'") == 1);
    CHECK("INSERT OR IGNORE keeps the existing row", count("SELECT count(*) FROM content WHERE title='dup'") == 0);
    CHECK("the applied spool is deleted", !exists("") && !exists(".rejected") && !exists(".failed"));
    CHECK("after the backup, the export.pdb sync and the lists' re-read, once each",
          g_backups == 1 && g_pdb_syncs == 1 && g_invalidates == 1 && g_art_fixes == 1);
    fresh();
    rc = apply("-- only a comment\n\n");
    CHECK("a spool of comments and blank lines applies nothing and is deleted", rc == 0 && !exists("") && untouched());

    /* ---- refused before anything is written */
    fresh();
    CHECK("DELETE: rejected, nothing written",
          rejected(apply("INSERT INTO content VALUES(3,'x','/c');\nDELETE FROM content;\n")));
    fresh();
    CHECK("DROP TABLE: rejected", rejected(apply("DROP TABLE content;\n")));
    fresh();
    CHECK("a table outside the library's list: rejected",
          rejected(apply("UPDATE agentRegistry SET str_1='x';\n")));
    fresh();
    CHECK("a table whose name only starts like an allowed one: rejected",
          rejected(apply("INSERT INTO contentx VALUES(1);\n")));
    fresh();
    CHECK("INSERT OR REPLACE: rejected", rejected(apply("INSERT OR REPLACE INTO content VALUES(1,'x','/a');\n")));
    fresh();
    CHECK("a second statement after ';' on the line: rejected",
          rejected(apply("UPDATE content SET title='a' WHERE 0; DELETE FROM content;\n")));
    fresh();
    CHECK("no final ';': rejected", rejected(apply("INSERT INTO content VALUES(3,'x','/c')\n")));
    fresh();
    CHECK("a literal broken over two lines: rejected (each line is checked alone)",
          rejected(apply("INSERT INTO content VALUES(3,'a\nUPDATE content SET title=1;',1);\n")));
    fresh();
    CHECK("a statement written in lower case: rejected", rejected(apply("delete from content;\n")));

    /* ---- the second pass runs what the first checked */
    fresh();
    rc = apply("INSERT INTO content VALUES(2,'a','/a');  \nINSERT INTO content VALUES(3,'b','/b');\r\n"
               "INSERT INTO content VALUES(4,'c','/c');\n");
    CHECK("a line with trailing spaces or CRLF does not end the spool: every line after it is applied",
          rc == 0 && count("SELECT count(*) FROM content") == 4);
    fresh();
    CHECK("a 0x01 byte inside an allowed line (pass 2's line mark): rejected, the DELETE behind it never runs",
          rejected(apply("UPDATE content SET title=1 WHERE 0 \x01"
                         "DELETE FROM content WHERE 1;\n")));
    fresh();
    {
        static const char nul[] = "INSERT INTO content VALUES(3,'x','/c');\n\0DELETE FROM agentRegistry;\n"
                                  "INSERT INTO content VALUES(4,'y','/d');\n";
        put(nul, sizeof nul - 1);
        char *buf = malloc(LS_SPOOL_CAP);
        rc = ls_apply_buf(g_db, 0, g_spool, buf, NULL);
        free(buf);
        CHECK("a NUL byte (both passes would stop there, the rest unchecked and dropped): rejected",
              rejected(rc));
    }

    /* ---- failures leave the library as it was */
    fresh();
    rc = apply("INSERT INTO content VALUES(3,'x','/c');\nINSERT INTO content VALUES(1,'pk clash','/d');\n");
    CHECK("a statement that fails: everything rolled back, .failed, lists not re-read",
          rc == -4 && exists(".failed") && !exists("") && untouched() && g_invalidates == 0 && g_pdb_syncs == 0);
    fresh();
    sqlite3_exec(g_db, "BEGIN", 0, 0, 0);
    rc = apply("INSERT INTO content VALUES(3,'x','/c');\n");
    sqlite3_exec(g_db, "ROLLBACK", 0, 0, 0);
    CHECK("EP147 inside a transaction: not applied, the spool stays for the next tick",
          rc == -4 && exists("") && !exists(".failed") && g_backups == 0 && untouched());
    fresh();
    g_backup_rc = -1;
    rc = apply("INSERT INTO content VALUES(3,'x','/c');\n");
    CHECK("no backup of the library: nothing applied, .failed", rc == -4 && exists(".failed") && untouched());

    fresh();
    sqlite3_commit_hook(g_db, reject_commit, NULL);
    rc = apply("INSERT INTO content VALUES(3,'x','/c');\n");
    CHECK("a COMMIT failure is not reported applied or deleted",
          rc == -4 && exists(".failed") && !exists("") && untouched() && !g_invalidates && !g_pdb_syncs);
    sqlite3_commit_hook(g_db, NULL, NULL);
    fresh();
    sqlite3_exec(g_db, "ALTER TABLE content ADD COLUMN analysisDataFilePath TEXT; UPDATE content SET path='/a' WHERE content_id=1", NULL, NULL, NULL);
    const char *link = "-- AZMOD analysis-link 1 2f61 2f62\n"
                       "UPDATE content SET analysisDataFilePath='/b' WHERE content_id=1 AND path='/a' AND analysisDataFilePath IS NULL;\n";
    rc = apply(link);
    CHECK("analysis linking verifies the intended locator inside its transaction", rc == 0 && !exists(""));
    sqlite3_exec(g_db, "UPDATE content SET analysisDataFilePath='/later' WHERE content_id=1", NULL, NULL, NULL);
    rc = apply(link);
    CHECK("a later locator edit is preserved and reported as a failed link",
          rc == -4 && exists(".failed") && count("SELECT count(*) FROM content WHERE analysisDataFilePath='/later'") == 1);
    fresh();
    rc = apply("-- AZMOD analysis-link 1 2f61;DELETE 2f62\nUPDATE content SET title='bad' WHERE content_id=1;\n");
    CHECK("malformed analysis verification metadata is rejected before mutation", rc == -3 && untouched() && !g_backups);

    /* ---- the size cap */
    fresh();
    {
        size_t n = LS_SPOOL_CAP;
        char *big = malloc(n);
        memset(big, '-', n);
        for (size_t i = 79; i < n; i += 80)
            big[i] = '\n';
        put(big, n);
        char *buf = malloc(LS_SPOOL_CAP);
        rc = ls_apply_buf(g_db, 0, g_spool, buf, NULL);
        free(buf);
        free(big);
        CHECK("a spool of 1 MB: rejected (-2) unread", rc == -2 && exists(".rejected") && g_backups == 0);
    }
    fresh();
    {
        size_t n = LS_SPOOL_CAP - 2;
        char *big = malloc(n + 1);
        memset(big, '-', n);
        for (size_t i = 79; i < n; i += 80)
            big[i] = '\n';
        big[n - 1] = '\n';
        put(big, n);
        char *buf = malloc(LS_SPOOL_CAP);
        rc = ls_apply_buf(g_db, 0, g_spool, buf, NULL);
        free(buf);
        free(big);
        CHECK("a spool one byte under the cap is read (CONTROL for the cap)", rc == 0 && !exists(""));
    }
    sqlite3_close(g_db);
    return ut_summary();
}
