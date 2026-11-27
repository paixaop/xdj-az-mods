/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared by gesture and SDK writes. Called on the owning library thread. */
static void *playlist_pdb_connection(void *server) {
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0;
    unsigned media = 0;
    if (mlserver_chain(server, &svc, &lib, &db, &media) ||
        !own_q(db + SQLDB_CONN, &conn))
        return NULL;
    return (void *) conn;
}

static int mv_pdb_sync_status(void *server) {
    void *conn = playlist_pdb_connection(server);
    char root[300];
    if (!conn || !sq_resolve() || !cp_mount_of(conn, root, sizeof root))
        return AZ_PDB_FAILED;
    FILE *log = azlog_open("/tmp/azmod-libspool.log", "a");
    if (log)
        setvbuf(log, NULL, _IONBF, 0);
    char why[32];
    snprintf(why, sizeof why, "edit op %d", g_mv.op);
    int rc = ls_pdb_sync_root(conn, root, NULL, why, log);
    if (log)
        fclose(log);
    return rc < 0 ? AZ_PDB_FAILED : AZ_PDB_CURRENT;
}
