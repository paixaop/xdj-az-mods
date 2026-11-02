/* SPDX-License-Identifier: GPL-3.0-only */
#include "fuzz_common.h"
#include "sdk_analysis.h"
int LLVMFuzzerTestOneInput(const uint8_t *b, size_t n) {
    struct st_data *d = malloc(sizeof *d);
    if (!d) return 0;
    st_data_init(d);
    const uint8_t *buffers[2] = {b, NULL};
    size_t sizes[2] = {n, 0};
    /* A corpus input can be a single file or two concatenated PMAI containers. */
    if (n >= 12 && !memcmp(b, "PMAI", 4)) {
        size_t first = hc_be32(b + 8);
        if (first >= 12 && first < n) {
            sizes[0] = first;
            buffers[1] = b + first;
            sizes[1] = n - first;
        }
    }
    int rc = st_parse_pair(d, buffers, sizes, 31);
    if (rc == AZ_OK) {
        FZ_ASSERT(d->view.beat_count <= ST_BEATS && d->view.cue_count <= ST_CUES && d->view.phrase_count <= ST_PHRASES);
        for (uint32_t i = 0; i < d->view.cue_count; i++) {
            FZ_ASSERT(d->cues[i].valid & AZ_CUE_START);
            FZ_ASSERT(d->cues[i].type != AZ_CUE_LOOP || d->cues[i].end_s > d->cues[i].start_s);
        }
        for (uint32_t i = 1; i < d->view.beat_count; i++) FZ_ASSERT(d->beats[i].time_s > d->beats[i - 1].time_s);
    }
    free(d);
    return 0;
}
