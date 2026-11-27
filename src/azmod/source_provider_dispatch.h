/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_DISPATCH_H
#define AZMOD_SOURCE_PROVIDER_DISPATCH_H

#include "azmod-mod.h"

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(hidden)

/* Native EP147 identity. Only the first twelve bytes are significant. */
struct azspd_track_id {
    uint32_t player;
    uint16_t media;
    uint8_t kind, reserved;
    uint32_t track;
    uint32_t padding;
};

struct azspd_search {
    uint64_t ticket, media;
    char query[AZ_SOURCE_QUERY_BYTES];
};

struct azspd_load {
    uint64_t ticket;
    unsigned deck; /* SDK logical deck 1–4. */
    struct azspd_track_id id;
};

struct azspd_preview {
    uint64_t ticket;
    struct azspd_track_id id;
};

/* A committed physical library track can replace the carrier row when the
 * provider chooses import. Specify a native category belonging to that library. */
struct azspd_target {
    struct azspd_track_id id;
    uint16_t category;
    uint8_t order;
    uint32_t sort;
};

struct azspd_host {
    int (*on_message_thread)(void);
    bool (*owns_browse)(uint64_t media);
    bool (*owns_track)(const struct azspd_track_id *id);
    bool (*search)(const struct azspd_search *event);
    bool (*prepare)(const struct azspd_load *event);
    bool (*preview)(const struct azspd_preview *event); /* Optional. */
    void *(*search_view)(void); /* Visible stock SearchView on the message thread. */
};

/* The patch function checks the expected EP147 address before replacing the
 * slot. It returns true only on success. Install before native use. The
 * dispatch callbacks are process-lifetime and run on the message thread.
 * Copy callback arguments before returning. Search and prepare return true
 * when queued. A rejected search runs immediately through the stock path. */
typedef bool (*azspd_patch)(uintptr_t slot, uintptr_t expected, uintptr_t replacement);
int azspd_install(int firmware, const struct azspd_host *host, azspd_patch patch);

/* Message thread only. A newer native query invalidates the ticket. Resume
 * after publishing results, while the same provider's native Search is active. */
bool azspd_search_current(uint64_t ticket);
void azspd_cancel_search(uint64_t ticket);
int azspd_resume_search(uint64_t ticket);

/* Message thread only. A newer load through the same native deck loader
 * invalidates the ticket. Cancel outstanding work on source teardown. */
bool azspd_load_current(uint64_t ticket);
void azspd_cancel_load(uint64_t ticket);
int azspd_resume_load(uint64_t ticket, const struct azspd_target *replacement);

/* Message thread only. A newer Preview or a native Stop, Unload, or Cancel
 * invalidates the ticket. Resume once the cached track is visible to firmware.
 * Preview uses the temporary carrier even for an import provider. */
bool azspd_preview_current(uint64_t ticket);
void azspd_cancel_preview(uint64_t ticket);
int azspd_resume_preview(uint64_t ticket);

struct azstatus_native_v1;
struct azstatus_native_receipt;

enum azspd_deck_use {
    AZSPD_DECK_PENDING,
    AZSPD_DECK_ACCEPTED,
    AZSPD_DECK_RETIRED,
};

/* Initialize deck, id and the pre-replay unit/epoch (zero when unavailable).
 * The same lifecycle observer thread owns subsequent calls. Observation
 * covers this deck only. Preview and analysis need separate release proof. */
struct azspd_deck_watch {
    struct azspd_track_id id;
    unsigned deck; /* SDK deck 1–4, native receipts use 0–3. */
    uint64_t unit, epoch;
    enum azspd_deck_use use;
};

enum azspd_deck_use azspd_deck_observe(struct azspd_deck_watch *watch,
                                    const struct azstatus_native_v1 *api,
                                    const struct azstatus_native_receipt *receipt);

#pragma GCC visibility pop
#endif
