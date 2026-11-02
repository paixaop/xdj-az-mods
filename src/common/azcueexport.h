/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUEEXPORT_H
#define AZCUEEXPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private DLL boundary. Times describe GUI collection, not native edit time.
 * These receipts identify a player-unit registration, never source/load,
 * persisted data, or the semantic identity of a native cue index. */
struct azstem_cue_observation_v1 {
    uint32_t size, version;
    uint64_t unit_generation, revision, collected_ns;
    uint32_t changed_index_mask, reserved;
};
typedef int (*azstem_cue_observation_v1_fn)(int, struct azstem_cue_observation_v1 *);
typedef unsigned (*azstem_cue_observer_mask_v1_fn)(void);
/* Acknowledged active registration support, low four logical-deck bits.
 * Independent of whether a cue notification has produced a receipt. */
__attribute__((visibility("default"))) unsigned azstem_cue_observer_mask_v1(void);
/* take: GUI thread only;0 copies new changes,1 means no new pending changes,
 * -1 unavailable/invalid. read:0 copies last current collection,-1 unavailable.
 * Both preserve caller size and unknown tail. Failure never changes output. */
__attribute__((visibility("default"))) int azstem_cue_take_v1(int, struct azstem_cue_observation_v1 *);
__attribute__((visibility("default"))) int azstem_cue_observation_v1(int, struct azstem_cue_observation_v1 *);
#ifdef __cplusplus
}
static_assert(sizeof(struct azstem_cue_observation_v1) == 40, "private cue-change export ABI");
#else
_Static_assert(sizeof(struct azstem_cue_observation_v1) == 40, "private cue-change export ABI");
#endif
#endif
