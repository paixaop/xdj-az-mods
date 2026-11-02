/* SPDX-License-Identifier: GPL-3.0-only */
/* Private versioned C export. This does not assert a public SDK capability. */
#ifndef AZLOADEXPORT_H
#define AZLOADEXPORT_H
#include "azloadobserver.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct azstem_load_observation_v1 {
    uint32_t size, version;
    struct azsdk_load_sample sample;
};
typedef int (*azstem_load_observation_v1_fn)(int, struct azstem_load_observation_v1 *);
/* 0: copied a current receipt; -1: bad prefix/version/deck or no receipt.
 * A non-NULL output provides at least its uint32_t size prefix. Short callers
 * are rejected before version/sample reads. Failure changes no output byte;
 * success preserves caller size and any unknown tail beyond this version. */
__attribute__((visibility("default"))) int azstem_load_observation_v1(int deck, struct azstem_load_observation_v1 *out);
#ifdef __cplusplus
}
static_assert(sizeof(struct azstem_load_observation_v1) == 48, "private load export ABI");
#else
_Static_assert(sizeof(struct azstem_load_observation_v1) == 48, "private load export ABI");
#endif
#endif
