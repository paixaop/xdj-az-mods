/* SPDX-License-Identifier: GPL-3.0-only */
/* Private native map service. A packet owns one counted reference to each
 * nonzero content token. No borrowed firmware address crosses a queue. */
#ifndef AZSTATUS_MAPS_H
#define AZSTATUS_MAPS_H
#include <stdint.h>
enum {
    AZNM_GRID = 1,
    AZNM_PHRASES = 2,
    AZNM_PAIR = 4
};
struct aznm_component {
    uint64_t content, provider, revision;
    int64_t offset_samples;
    uint32_t sample_rate, availability;
};
struct aznm_packet {
    struct aznm_component grid, phrases;
    uint64_t owner, changed;
};
struct azn_maps_api {
    uint32_t size, version;
    /* All arguments are genuine typed borrows in the common GUI owner call.
     * The owner is an identity, never an off-thread dereferenceable pointer. */
    int (*prove_pair)(uintptr_t owner, uintptr_t pack, uintptr_t grid_holder, uintptr_t grid_content,
                      uintptr_t phrase_holder, uintptr_t phrase_content);
    void (*retire_pair)(uintptr_t owner);
    int (*read)(const struct aznm_component *, unsigned component, void *rows, uint32_t capacity, uint32_t *count);
    void (*release)(const struct aznm_packet *);
    void (*seed)(unsigned deck);
    int (*refresh_pair)(uintptr_t owner, uintptr_t grid_holder, uintptr_t grid_content,
                        uintptr_t phrase_holder, uintptr_t phrase_content);
    uint64_t (*begin_pair)(uintptr_t owner, uintptr_t pack);
    void (*end_pair)(uint64_t ticket);
    /* Exact immutable grid coordinates in the holder's declared clock. */
    int (*read_samples)(const struct aznm_component *, int64_t *ticks, uint32_t capacity,
                        uint32_t *count, uint32_t *rate);
    /* Worker-only loaded-analysis support (AZ_* result codes). capture owns
     * both content tokens until release; current validates the exact edition.
     * publish_phrases accepts plain, bounded PSSI for that unchanged grid and
     * an empty native phrase content. It never installs or retimes a grid.
     * Only verified firmware profiles expose successful publication. */
    int (*capture)(unsigned deck, uint64_t load, struct aznm_packet *out);
    int (*current)(unsigned deck, uint64_t load, const struct aznm_packet *expected);
    int (*publish_phrases)(unsigned deck, uint64_t load, const struct aznm_packet *expected,
                           const void *pssi, uint32_t size);
};
#ifdef AZ_STATUS_MAPS_PRODUCER
__attribute__((visibility("hidden"))) const struct azn_maps_api *azn_maps_service(void);
__attribute__((visibility("hidden"))) int azn_maps_configure(int build);
#endif
#endif
