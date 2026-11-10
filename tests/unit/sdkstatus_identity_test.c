/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azstatus_identity.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

struct range {
    uint64_t first, last;
};

int main(void) {
    const struct range producers[] = {
        {1, AZSP_UNIT_MAX},
        {AZSP_RAW + 1, AZSP_RAW + AZSP_UNIT_MAX},
        {AZSP_MEMORY + 1, AZSP_MEMORY + AZSP_SERIAL_MAX},
        {AZSP_MASTER, AZSP_MASTER},
        {AZSP_TEMPO + 1, AZSP_TEMPO + AZSP_TEMPO_MAX},
        {AZSP_SETTINGS + 1, AZSP_SETTINGS + AZSP_SERIAL_MAX},
        {AZSP_KEY + 1, AZSP_KEY + AZSP_SERIAL_MAX},
        {AZSP_GUI_MODEL + 1, AZSP_GUI_MODEL + AZSP_SERIAL_MAX},
        {AZSP_SLIP + 1, AZSP_SLIP + AZSP_SERIAL_MAX},
        {AZSP_PAD + 1, AZSP_PAD + AZSP_SERIAL_MAX},
        {AZSP_MAP + 1, AZSP_MAP + AZSP_SERIAL_MAX},
        {AZSP_DERIVED + 1, AZSP_DERIVED + 4},
        {AZSP_HOT + 1, AZSP_HOT + AZSP_SERIAL_MAX},
        {AZSP_FILE + 1, UINT64_MAX}};
    for (size_t i = 0; i < sizeof producers / sizeof producers[0]; ++i) {
        assert(producers[i].first && producers[i].last >= producers[i].first);
        for (size_t j = 0; j < i; ++j)
            assert(producers[i].first > producers[j].last || producers[j].first > producers[i].last);
    }
    puts("PASS provider first identities and reserved ranges are disjoint");
}
