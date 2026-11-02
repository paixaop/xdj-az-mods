/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azloadexport.h"
#include <string.h>

int azstem_load_observation_v1(int deck, struct azstem_load_observation_v1 *out) {
    if (!out) return -1;
    uint32_t size;
    memcpy(&size, out, sizeof size);
    if (size < sizeof(struct azstem_load_observation_v1)) return -1;
    uint32_t version;
    memcpy(&version, (const unsigned char *) out + sizeof size, sizeof version);
    if (version != 1 || deck < 0 || deck >= 4) return -1;
    struct azsdk_load_sample sample;
    if (!azsdk_load_read(deck, &sample)) return -1;
    struct azstem_load_observation_v1 value = {size, 1, sample};
    memcpy(out, &value, sizeof value);
    return 0;
}
