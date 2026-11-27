/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/analysis/host/pvdi-windows-source.h"
#include "../../src/analysis/host/pvdi-windows-reduction.h"
#include "../../src/common/azsha256.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static enum pvdi_windows_status expected_status(const char *argument)
{
    if (strcmp(argument, "invalid-executable") == 0)
        return PVDI_WINDOWS_UNSUPPORTED_EXECUTABLE;
    if (strcmp(argument, "invalid-graph") == 0)
        return PVDI_WINDOWS_GRAPH_MISMATCH;
    return PVDI_WINDOWS_OK;
}

static void verify_vocal(const struct pvdi_windows_vocal *vocal)
{
    char hash[65];
    size_t sourced = 0;
    for (size_t i = 0; i < TA_VOCAL_PARAMETER_COUNT; ++i)
        sourced += vocal->present[i];
    assert(sourced == 87);
    assert(!vocal->present[TA_VOCAL_MOTION_ZERO_LAG]);
    for (size_t i = TA_VOCAL_MOTION_REDUCE_PAIR_0; i <= TA_VOCAL_MOTION_ENERGY_EPSILON_LANE; ++i)
        assert(!vocal->present[i]);
    azsha256_hex(vocal->value, sizeof vocal->value, hash);
    assert(strcmp(hash, "67bdfc6578d59eb3b659e870989fbb2a1b577db7bfb54ca10147a9fe44505282") == 0);
    azsha256_hex(vocal->motion, sizeof vocal->motion, hash);
    assert(strcmp(hash, "79f10e58d1ba2544c9e6a31e1f629f90774fef20b2289273acce823791abd047") == 0);
}

static void verify_phrase(const struct pvdi_windows_phrase *phrase)
{
    char hash[65];
    uint8_t mapped[TA_PHRASE_PARAMETER_COUNT];
    size_t sourced = 0;
    for (size_t i = 0; i < TA_PHRASE_PARAMETER_COUNT; ++i) {
        mapped[i] = phrase->present[i];
        sourced += mapped[i];
    }
    assert(sourced == 1685);
    azsha256_hex(mapped, sizeof mapped, hash);
    assert(strcmp(hash, "3805ed65643d22f7cddb37f012a6f95595fd3221956981c4251627bd67a00b0a") == 0);
    azsha256_hex(phrase->value, sizeof phrase->value, hash);
    assert(strcmp(hash, "5ca47f0cfa08bf6750c772b497622d1cf2423421ba4506023f30180ab370466d") == 0);
    azsha256_hex(phrase->reduction, sizeof phrase->reduction, hash);
    assert(strcmp(hash, "244ce044835f5f80a386b3499cbcfd4506ce4b92ab481668960067beb5986112") == 0);
}

static void reject_reduction_mutations(struct pvdi_windows_source *source)
{
    static const uint32_t sites[] = {
        0x013522ad, 0x013522b0, 0x013522c0, 0x013522d6, 0x013522da,
        0x013522e4, 0x01354056, 0x01354095, 0x013540d1, 0x01354113
    };
    uint8_t output[TA_MODEL_WINDOWS_REDUCTION_BYTES];
    assert(!pvdi_windows_reduction_extract(NULL, source->executable_bytes, 3, output));
    assert(!pvdi_windows_reduction_extract(source->executable, source->executable_bytes, 4, output));
    assert(!pvdi_windows_reduction_extract(source->executable, 0, 3, output));
    assert(!pvdi_windows_reduction_extract(source->executable, source->executable_bytes, 3, NULL));
    for (size_t i = 0; i < sizeof sites / sizeof *sites; ++i) {
        source->executable[sites[i]] ^= 1;
        memset(output, 0xa5, sizeof output);
        assert(!pvdi_windows_reduction_extract(source->executable, source->executable_bytes, 3, output));
        for (size_t j = 0; j < sizeof output; j++)
            assert(output[j] == 0xa5);
        source->executable[sites[i]] ^= 1;
    }
}

static void verify_rhythm_ordinal_mask(const struct pvdi_windows_phrase *phrase)
{
    const double *parameters = phrase->value;
    unsigned mask = (unsigned)parameters[TA_RHYTHM_PATTERN_OFFSET_MASK];
    unsigned match = (unsigned)parameters[TA_RHYTHM_PATTERN_OFFSET_MATCH];
    assert(parameters[TA_RHYTHM_PATTERN_GROUP_BARS] == 8);
    assert(parameters[TA_RHYTHM_PATTERN_OFFSET_BIAS] == 1);
    const bool selected[] = {false, true, true, false, false, true, true, false};
    for (unsigned i = 0; i < sizeof selected / sizeof *selected; ++i)
        assert((((i + 1) & mask) == match) == selected[i]);
}

static void verify_network(const struct pvdi_windows_network *network)
{
    azsha256_ctx context;
    char hash[65];
    azsha256_init(&context);
    azsha256_update(&context, network->window, sizeof network->window);
    azsha256_update(&context, network->scale, sizeof network->scale);
    azsha256_update(&context, network->offset, sizeof network->offset);
    azsha256_update(&context, network->thresholds, sizeof network->thresholds);
    azsha256_update(&context, &network->output_class, sizeof network->output_class);
    azsha256_final_hex(&context, hash);
    assert(strcmp(hash, "224b92a5ed692a51568621059531dba8536ee00e00eb645fc2267985fc34c9e3") == 0);
}

static void verify_result(const struct pvdi_windows_source *source, const char *version,
                          enum pvdi_windows_status status)
{
    if (status == PVDI_WINDOWS_OK) {
        assert(source->version && strcmp(source->version, version) == 0);
        assert(source->executable && source->executable_bytes);
        assert(source->graph);
        verify_vocal(&source->vocal);
        verify_phrase(&source->phrase);
        verify_rhythm_ordinal_mask(&source->phrase);
        verify_network(&source->network);
        return;
    }
    assert(!source->version && !source->executable && !source->executable_bytes && !source->graph);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s REKORDBOX_EXE VD_PARAM_PB EXPECTED_VERSION_OR_ERROR\n", argv[0]);
        return 2;
    }
    FILE *executable = fopen(argv[1], "rb");
    FILE *graph = fopen(argv[2], "rb");
    assert(executable && graph);
    struct pvdi_windows_source source;
    enum pvdi_windows_status status = pvdi_windows_load(executable, graph, &source);
    assert(status == expected_status(argv[3]));
    verify_result(&source, argv[3], status);
    if (status == PVDI_WINDOWS_OK && strcmp(argv[3], "7.2.20.0") == 0)
        reject_reduction_mutations(&source);
    pvdi_windows_release(&source);
    verify_result(&source, NULL, PVDI_WINDOWS_UNSUPPORTED_EXECUTABLE);
    fclose(graph);
    fclose(executable);
    printf("Windows source %s passed\n", argv[3]);
    return 0;
}
