/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/aznative_calls.h"
#include "../../src/common/aznative_listener.h"
#include "../../src/common/azconnectionhooks.h"
#include "../../src/common/azsources.h"
struct sqlite3_stmt;
extern az_native_string_allocate_fn allocate_string;
extern az_native_editor_text_fn read_text;
extern az_native_string_destroy_fn destroy_string;
extern az_native_listener_register_fn register_listener;
extern struct az_native_player *player;
extern struct az_native_editor *editor;
extern struct az_native_notification_server *server;
extern struct az_native_owner owner;
extern struct az_native_string string;
extern struct az_native_text text;
extern struct az_connection_installer installer;
extern struct az_native_listener_vtable listener_methods;
extern az_sql_version_number_fn version;
extern az_crypto_digest_fn digest;
extern struct engine_st *engine;
extern struct sqlite3 *database;
extern struct sqlite3_stmt *statement;

void check_types(void) {
#if TYPE_ERROR == 1
    (void) az_native_string_allocate(read_text, 16);
#elif TYPE_ERROR == 2
    (void) az_native_editor_text(read_text, player);
#elif TYPE_ERROR == 3
    installer.open = installer.close;
#elif TYPE_ERROR == 4
    az_listener_register(register_listener, player, &owner);
#elif TYPE_ERROR == 5
    listener_methods.notify_route = destroy_string;
#elif TYPE_ERROR == 6
    destroy_string(&text);
#elif TYPE_ERROR == 7
    (void) version(1);
#elif TYPE_ERROR == 8
    (void) digest(NULL, 0, NULL, NULL, engine, engine);
#elif TYPE_ERROR == 9
    (void) azsdk_sources_reader_fd(statement, 1, 1, 1);
#else
    string = az_native_string_allocate(allocate_string, 16);
    text = az_native_editor_text(read_text, editor);
    az_listener_register(register_listener, server, &owner);
    destroy_string(&string);
    (void) version();
    (void) digest(NULL, 0, NULL, NULL, NULL, engine);
    (void) azsdk_sources_reader_fd(database, 1, 1, 1);
#endif
}
