/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_SOURCE_KIND_H
#define AZSTATUS_SOURCE_KIND_H
#include "../azmod/azmod-mod.h"
/* trackid::TrackID begins with the typed trackid::MediaID passed to the load
 * facade. Only call on the copied identity of the matched successful load.
 * Member kind is byte 0. Storage interface and slot are separate bitfields
 * in byte 4. Neither identifies a library, file, row or source generation. */
static inline int azn_local_source_kind(int build, unsigned storage, unsigned slot) {
    if (storage == 2 && slot <= 1) return slot ? AZ_STATUS_SOURCE_USB2 : AZ_STATUS_SOURCE_USB1;
    if (storage == 4) return AZ_STATUS_SOURCE_CLOUD; /* kRekordboxCloud */
    if (storage >= 6 && storage <= 8) return AZ_STATUS_SOURCE_STREAMING; /* Beatport, Tidal, Apple Music */
    return build == 2 && storage == 11 ? AZ_STATUS_SOURCE_STREAMING : 0; /* Spotify */
}
static inline int azn_remote_source_kind(unsigned member, unsigned storage) {
    if (member >= 2 && member <= 5 && storage >= 1 && storage <= 5 && storage != 4)
        return AZ_STATUS_SOURCE_LINKED;
    return 0;
}
static inline int azn_source_kind(int build, const unsigned char track_id[12]) {
    if (!track_id || (build != 1 && build != 2)) return 0;
    unsigned member = track_id[0], storage = track_id[4] & 31u, slot = track_id[4] >> 5;
    if (member == 1) return azn_local_source_kind(build, storage, slot); /* LinkMember::Local */
    /* Positive remote single player, all-in-one player, wired rekordbox PC
     * or network device on a native disk/library interface. A mixed external
     * cloud/streaming route needs its own source association. */
    return azn_remote_source_kind(member, storage);
}
#endif
