/* SPDX-License-Identifier: GPL-3.0-only */
/* Internal receipt joining the local MusicLibrary request to a verified PlayMusicPack.
 * All address-like words are comparison keys. They never grant native object ownership. */
#ifndef AZSTATUS_SOURCE_H
#define AZSTATUS_SOURCE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct azjs_origin {
    uint64_t owner;
    uint32_t track_id;
};
typedef int (*azjs_installer)(uintptr_t target, uintptr_t expected, void *hook, void **original, int slot);
__attribute__((visibility("hidden"))) int azjs_install(int build, azjs_installer install);
/* Genuine MusicLibraryServer / Ticket / Reception scopes only. */
__attribute__((visibility("hidden"))) void azjs_server(uintptr_t message, uint32_t peer, uint64_t owner, int entering);
__attribute__((visibility("hidden"))) void azjs_ticket(uintptr_t ticket, uintptr_t packet, int accepted);
__attribute__((visibility("hidden"))) void azjs_ticket_return(uintptr_t ticket, uintptr_t returned);
__attribute__((visibility("hidden"))) void azjs_reception(uintptr_t receiver, uint64_t pack, const void *load_info);
__attribute__((visibility("hidden"))) uint64_t azjs_capture(unsigned deck, uint64_t unit, uint64_t epoch,
                                                            uintptr_t pack, const unsigned char track_id[12]);
__attribute__((visibility("hidden"))) int azjs_lookup(uint64_t token, struct azjs_origin *out);
#ifdef __cplusplus
}
#endif
#endif
