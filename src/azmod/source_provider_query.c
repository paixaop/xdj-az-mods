/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "source_provider_query.h"

#include "azmod-mod.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum {
    QUERY_SEARCH_COMMAND = 0x1500,
    QUERY_SEARCH_LIST = 0x12,
    QUERY_MAX_SQL = 2048,
    QUERY_SQLITE_ERROR = 1,
};

static _Thread_local struct azsp_query_scope *query_scope;
static _Atomic unsigned query_sequence;

static size_t query_provider_size(const char *provider) {
    size_t length = provider ? strnlen(provider, 33) : 0;
    return length < 33 ? length : 0;
}

static void query_provider_hex(char *out, const char *provider) {
    static const char digits[] = "0123456789ABCDEF";
    size_t length = query_provider_size(provider);
    for (size_t i = 0; i < length; ++i) {
        unsigned byte = (unsigned char) provider[i];
        out[2 * i] = digits[byte >> 4];
        out[2 * i + 1] = digits[byte & 15];
    }
    out[2 * length] = '\0';
}

static void query_search_token(char token[8]) {
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned sequence = atomic_fetch_add_explicit(&query_sequence, 1, memory_order_relaxed);
    token[0] = 'A';
    token[1] = 'Z';
    for (size_t i = 7; i > 2; --i) {
        token[i - 1] = digits[sequence % 36];
        sequence /= 36;
    }
    token[7] = '\0';
}

static int query_is_search(const void *message) {
    uint16_t command = 0;
    if (message) memcpy(&command, message, sizeof command);
    return command == QUERY_SEARCH_COMMAND;
}

void azsp_query_begin(void *server, const void *message, void *db, const char *provider,
                      struct azsp_query_scope *scope) {
    *scope = (struct azsp_query_scope) {.previous = query_scope};
    query_scope = scope;
    if (!azsp_carrier_library_media(server)) return;
    scope->database = db;
    query_provider_hex(scope->provider_hex, provider);
    if (query_is_search(message)) query_search_token(scope->search_token);
}

void azsp_query_end(struct azsp_query_scope *scope) {
    query_scope = scope->previous;
}

enum query_table { QUERY_CONTENT, QUERY_PLAYLIST, QUERY_PLAYLIST_LINK, QUERY_HISTORY, QUERY_HISTORY_LINK };

struct query_enumeration {
    const char *prefix;
    const char *table_name;
    enum query_table table;
};

static const struct query_enumeration query_enumerations[] = {
    {"SELECT content_id, title, subtitle, bpmx100, length, trackNo, artist_id_artist, artist_id_remixer, "
     "artist_id_originalArtist, artist_id_composer, artist_id_lyricist, album_id, genre_id, label_id, key_id, "
     "color_id, image_id, djComment, rating, releaseYear, dateAdded, path, fileName, fileType, bitrate, isrc, "
     "djPlayCount, isHotCueAutoLoadOn, isKuvoDeliverStatusOn, kuvoDeliveryComment, masterDbId, masterContentId, "
     "analysisDataFilePath FROM content", "content", QUERY_CONTENT},
    {"SELECT artist.artist_id, artist.name FROM artist INNER JOIN ( SELECT DISTINCT artist_id_artist FROM content",
     "content", QUERY_CONTENT},
    {"SELECT album.album_id, album.name, album.image_id FROM album INNER JOIN ( SELECT DISTINCT album_id FROM content",
     "content", QUERY_CONTENT},
    {"SELECT album_id FROM content", "content", QUERY_CONTENT},
    {"SELECT playlist_id, sequenceNo, name, image_id, attribute, playlist_id_parent FROM playlist",
     "playlist", QUERY_PLAYLIST},
    {"SELECT playlist_id, content_id, sequenceNo FROM playlist_content", "playlist_content", QUERY_PLAYLIST_LINK},
    {"SELECT history_id, sequenceNo, name, attribute, history_id_parent FROM history", "history", QUERY_HISTORY},
    {"SELECT history_id, content_id, sequenceNo FROM history_content", "history_content", QUERY_HISTORY_LINK},
};

static int query_boundary(const char *sql, size_t length, size_t prefix) {
    return prefix == length || sql[prefix] == ' ';
}

static int query_enumeration_matches(const struct query_enumeration *rule, const char *sql, size_t length) {
    size_t prefix = strlen(rule->prefix);
    return prefix <= length && !memcmp(rule->prefix, sql, prefix) && query_boundary(sql, length, prefix);
}

static const struct query_enumeration *query_enumeration(const char *sql, size_t length) {
    for (size_t i = 0; i < sizeof query_enumerations / sizeof *query_enumerations; ++i) {
        if (query_enumeration_matches(query_enumerations + i, sql, length)) return query_enumerations + i;
    }
    return NULL;
}

static int query_content_identity(enum query_table table, const char *suffix) {
    return table == QUERY_CONTENT && !strncmp(suffix, " WHERE content_id = ", sizeof " WHERE content_id = " - 1);
}

static const char *query_table_scope(enum query_table table) {
    static const char *const constraints[] = {
        [QUERY_CONTENT] = "azProviderTrack owner WHERE owner.id = rows.content_id AND owner.provider = ",
        [QUERY_PLAYLIST] = "azProviderPlaylist owner WHERE owner.id = rows.playlist_id AND owner.provider = ",
        [QUERY_PLAYLIST_LINK] = "azProviderPlaylist owner WHERE owner.id = rows.playlist_id AND owner.provider = ",
        [QUERY_HISTORY] = ("history_content item JOIN main.azProviderTrack owner ON owner.id = item.content_id "
                           "WHERE item.history_id = rows.history_id AND owner.provider = "),
        [QUERY_HISTORY_LINK] = "azProviderTrack owner WHERE owner.id = rows.content_id AND owner.provider = ",
    };
    return constraints[table];
}

static int query_catalog_sql(char *out, const char *source, const char *suffix,
                            const struct query_enumeration *rule, const char *provider) {
    int head = (int) (suffix - source - strlen(rule->table_name));
    return snprintf(out, QUERY_MAX_SQL,
        "%.*s(SELECT rows.* FROM main.%s rows WHERE EXISTS (SELECT 1 FROM main.%s"
        "CAST(X'%s' AS TEXT))) AS %s%s",
        head, source, rule->table_name, query_table_scope(rule->table), provider, rule->table_name, suffix);
}

static int query_search_sql(char *out, const char *source, const char *suffix, const struct azsp_query_scope *scope) {
    int head = (int) (suffix - source - strlen("content"));
    return snprintf(out, QUERY_MAX_SQL,
        "%.*s(SELECT '%s' AS title, rows.* FROM main.content rows "
        "JOIN main.azProviderSearch result ON result.content_id = rows.content_id "
        "JOIN main.azProviderTrack owner ON owner.id = rows.content_id AND owner.provider = CAST(X'%s' AS TEXT) "
        "ORDER BY result.sequenceNo) AS content%s",
        head, source, scope->search_token, scope->provider_hex, suffix);
}

static int query_make_sql(char *out, const char *sql, const char *suffix,
                          const struct query_enumeration *rule, const struct azsp_query_scope *scope) {
    if (query_content_identity(rule->table, suffix)) return 0;
    if (rule->table == QUERY_CONTENT && scope->search_token[0])
        return query_search_sql(out, sql, suffix, scope);
    return query_catalog_sql(out, sql, suffix, rule, scope->provider_hex);
}

static size_t query_length(const char *sql, int size) {
    size_t limit = size >= 0 && size < QUERY_MAX_SQL ? (size_t) size : QUERY_MAX_SQL;
    return strnlen(sql, limit);
}

static int query_rewrite(char *out, const struct azsp_query_scope *scope, const char *sql, size_t bytes) {
    const struct query_enumeration *rule = query_enumeration(sql, bytes);
    if (!rule) return 0;
    if (bytes == QUERY_MAX_SQL || memchr(sql, ';', bytes)) return -1;
    char copy[QUERY_MAX_SQL];
    memcpy(copy, sql, bytes);
    copy[bytes] = '\0';
    return query_make_sql(out, copy, copy + strlen(rule->prefix), rule, scope);
}

static int query_owns_database(const struct azsp_query_scope *scope, void *db) {
    return scope && db && scope->database == db;
}

static int query_prepare_rewritten(void *db, const char *original_sql, size_t bytes, const char *sql,
                                    void **statement, const char **tail, azsp_query_prepare_fn original) {
    if (tail) *tail = original_sql + bytes;
    return original(db, sql, -1, statement, NULL);
}

static int query_prepare_error(void **statement, const char **tail, const char *sql) {
    if (statement) *statement = NULL;
    if (tail) *tail = sql;
    return QUERY_SQLITE_ERROR;
}

int azsp_query_prepare(void *db, const char *sql, int len, void **statement, const char **tail,
                      azsp_query_prepare_fn original) {
    const struct azsp_query_scope *scope = query_scope;
    if (!sql || !query_owns_database(scope, db)) return original(db, sql, len, statement, tail);
    char rewritten[QUERY_MAX_SQL];
    size_t bytes = query_length(sql, len);
    int size = query_rewrite(rewritten, scope, sql, bytes);
    if (size < 0 || size >= QUERY_MAX_SQL) return query_prepare_error(statement, tail, sql);
    if (size) return query_prepare_rewritten(db, sql, bytes, rewritten, statement, tail, original);
    return original(db, sql, len, statement, tail);
}

#if defined(__aarch64__) && defined(__linux__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../common/ep147build.h"
#pragma GCC diagnostic pop

#define SEARCH_LIST_CREATE 0x13076e0u

struct query_native_word {
    uint16_t *data;
    size_t length;
    uint16_t text[8];
};
_Static_assert(sizeof(struct query_native_word) == 0x20, "EP147 search keyword size");

static unsigned (*search_list_create)(void *, unsigned, unsigned, const uint16_t *, unsigned, const void *);

static void query_native_word(struct query_native_word *word, const char token[8]) {
    word->data = word->text;
    word->length = 7;
    for (size_t i = 0; i < 8; ++i) word->text[i] = (unsigned char) token[i];
}

static int query_uses_native_token(const struct azsp_query_scope *scope, const uint16_t *kind) {
    return scope && scope->search_token[0] && *kind == QUERY_SEARCH_LIST;
}

static unsigned query_search_list_create(void *server, unsigned mode, unsigned media, const uint16_t *kind,
                                         unsigned sort, const void *keywords) {
    const struct azsp_query_scope *scope = query_scope;
    struct query_native_word token;
    const struct query_native_word *vector[3] = {&token, &token + 1, &token + 1};
    if (query_uses_native_token(scope, kind)) {
        query_native_word(&token, scope->search_token);
        keywords = vector;
    }
    return search_list_create(server, mode, media, kind, sort, keywords);
}

static int query_search_entry_matches(void) {
    static const uint32_t expected[] = {0xa9b77bfd, 0x910003fd, 0xa9025bf5, 0x2a0203f6};
    return !memcmp((const void *) SEARCH_LIST_CREATE, expected, sizeof expected);
}
#endif

int azsp_query_install(int build, azsp_carrier_patch_fn patch) {
#if defined(__aarch64__) && defined(__linux__)
    if (!patch || build != EPB_V200) return AZ_UNSUPPORTED;
    if (!query_search_entry_matches()) return AZ_UNSUPPORTED;
    return patch(SEARCH_LIST_CREATE, query_search_list_create, (void **) &search_list_create) == 1 ? AZ_OK : AZ_UNSUPPORTED;
#else
    (void) build;
    (void) patch;
    return AZ_UNSUPPORTED;
#endif
}
