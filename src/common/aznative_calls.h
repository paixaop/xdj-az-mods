/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZNATIVE_CALLS_H
#define AZNATIVE_CALLS_H
#include "aznative_c.h"

/* Only address qualification converts raw firmware addresses to these types.
 * The carrier preserves the native hidden-result ABI; wrappers copy exactly
 * the initialized prefix into the operation's result. Returned owners retain
 * the native ownership contract and must be released by their native service. */
struct az_native_player_interface;
struct az_native_player;
struct az_native_location;
struct az_native_track;
struct az_native_editor;
struct az_native_phrase_atom_ref;
struct az_native_phrase_content;
struct az_native_string {
    char *data;
};
struct az_native_text {
    const char *text;
};

typedef struct native_result_storage (*az_native_string_allocate_fn)(size_t);
typedef void (*az_native_string_destroy_fn)(struct az_native_string *);
typedef void (*az_native_string_assign_fn)(struct az_native_string *, const struct az_native_string *);
typedef struct native_result_storage (*az_native_player_acquire_fn)(struct az_native_player_interface *);
typedef struct native_result_storage (*az_native_location_acquire_fn)(const struct az_native_player *);
typedef struct native_result_storage (*az_native_track_state_fn)(const struct az_native_track *);
typedef struct native_result_storage (*az_native_editor_text_fn)(struct az_native_editor *);
typedef struct native_result_storage (*az_native_phrase_make_fn)(const struct az_native_phrase_atom_ref *);

static inline struct az_native_string az_native_string_allocate(az_native_string_allocate_fn function, size_t bytes) {
    struct native_result_storage value = function(bytes);
    struct az_native_string result;
    __builtin_memcpy(&result, &value, sizeof result);
    return result;
}
static inline struct az_native_player *az_native_player_acquire(az_native_player_acquire_fn function,
                                                                struct az_native_player_interface *interface) {
    struct native_result_storage value = function(interface);
    struct az_native_player *result;
    __builtin_memcpy(&result, &value, sizeof result);
    return result;
}
static inline struct az_native_location *az_native_location_acquire(az_native_location_acquire_fn function,
                                                                    const struct az_native_player *player) {
    struct native_result_storage value = function(player);
    struct az_native_location *result;
    __builtin_memcpy(&result, &value, sizeof result);
    return result;
}
static inline struct az_native_owner az_native_track_state_acquire(az_native_track_state_fn function,
                                                                   const struct az_native_track *track) {
    struct native_result_storage value = function(track);
    struct az_native_owner result;
    __builtin_memcpy(&result, &value, sizeof result);
    return result;
}
static inline struct az_native_text az_native_editor_text(az_native_editor_text_fn function,
                                                          struct az_native_editor *editor) {
    struct native_result_storage value = function(editor);
    struct az_native_text result;
    __builtin_memcpy(&result, &value, sizeof result);
    return result;
}
static inline struct az_native_phrase_content *az_native_phrase_make(az_native_phrase_make_fn function,
                                                                     const struct az_native_phrase_atom_ref *atom) {
    struct native_result_storage value = function(atom);
    struct az_native_phrase_content *result;
    __builtin_memcpy(&result, &value, sizeof result);
    return result;
}
#endif
