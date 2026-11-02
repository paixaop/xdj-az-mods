/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azcueexport.h"
#include "../common/azcueobserver.h"
#include <string.h>

unsigned azstem_cue_observer_mask_v1(void) {
    return azsdk_cue_capabilities() & 15u;
}

static int cue_export_request(int deck, const struct azstem_cue_observation_v1 *out,
                              uint32_t *size) {
    if (!out || deck < 0 || deck >= 4) return 0;
    memcpy(size, out, sizeof *size);
    if (*size < sizeof *out) return 0;
    uint32_t version;
    memcpy(&version, (const unsigned char *)out + sizeof *size, sizeof version);
    return version == 1;
}

static int cue_export_sample_valid(const struct azsdk_cue_sample *sample) {
    return sample->unit_generation && sample->revision && sample->observed_ns &&
           sample->changed_index_mask && !(sample->changed_index_mask & ~0x1ffu) &&
           !sample->reserved;
}

static void cue_export_copy(struct azstem_cue_observation_v1 *out, uint32_t size,
                            const struct azsdk_cue_sample *sample) {
    const struct azstem_cue_observation_v1 value = {
        size, 1, sample->unit_generation, sample->revision, sample->observed_ns,
        sample->changed_index_mask, 0
    };
    memcpy(out, &value, sizeof value);
}

int azstem_cue_take_v1(int deck, struct azstem_cue_observation_v1 *out) {
    uint32_t size;
    if (!cue_export_request(deck, out, &size)) return -1;
    struct azsdk_cue_sample sample;
    if (!azsdk_cue_take(deck, &sample)) {
        /* Distinguish an idle current registration from retirement. No pending
         * change is consumed until the request's prefix has been validated. */
        return azsdk_cue_read(deck, &sample) && cue_export_sample_valid(&sample) ? 1 : -1;
    }
    if (!cue_export_sample_valid(&sample)) return -1;
    cue_export_copy(out, size, &sample);
    return 0;
}

int azstem_cue_observation_v1(int deck, struct azstem_cue_observation_v1 *out) {
    uint32_t size;
    if (!cue_export_request(deck, out, &size)) return -1;
    struct azsdk_cue_sample sample;
    if (!azsdk_cue_read(deck, &sample) || !cue_export_sample_valid(&sample)) return -1;
    cue_export_copy(out, size, &sample);
    return 0;
}
