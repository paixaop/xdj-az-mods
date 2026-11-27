/* SPDX-License-Identifier: GPL-3.0-only */
#include "internal.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

static void load_asset(const char *directory, const char *expected_error)
{
    const ta_request request = {.assets_path = directory};
    ta_context *context = calloc(1, sizeof *context);
    assert(context);
    context->request = &request;
    double parameters[TA_PHRASE_PARAMETER_COUNT];
    if (!setjmp(context->failure)) {
        ta_model_phrase_parameters(context, parameters);
        assert(!expected_error);
        assert(context->windows_model);
        assert(parameters[TA_PHRASE_SUM_PAIR_3] < 0);
        assert(parameters[TA_RECURRENCE_DISTANCE_PAIR_3] < 0);
    } else {
        assert(expected_error && context->status == TA_FAILED);
        assert(strcmp(context->error, expected_error) == 0);
    }
    ta_cleanup(context);
    free(context);
}

static void write_checksum(uint8_t *model)
{
    uint32_t hash = 2166136261u;
    for (size_t i = TA_MODEL_HEADER_BYTES; i < TA_MODEL_WINDOWS_BYTES; ++i)
        hash = (hash ^ model[i]) * 16777619u;
    for (unsigned i = 0; i < 4; ++i)
        model[12 + i] = (uint8_t)(hash >> (8 * i));
}

static void check_mutation(const char *directory, const char *path, const uint8_t *model,
                           size_t length, const char *error)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(model, 1, length, file) == length);
    assert(fclose(file) == 0);
    load_asset(directory, error);
}

static void check_reduction(const char *directory, const char *path, uint8_t *model)
{
    uint8_t *recipe = model + TA_MODEL_WINDOWS_REDUCTION_OFFSET;
    recipe[0] ^= 1;
    check_mutation(directory, path, model, TA_MODEL_WINDOWS_BYTES, "analysis model checksum");
    write_checksum(model);
    check_mutation(directory, path, model, TA_MODEL_WINDOWS_BYTES, "invalid Windows phrase reduction recipe");
    recipe[0] ^= 1;
    uint8_t *first = recipe + TA_WINDOWS_REDUCTION_HEADER_BYTES;
    first[1] ^= 1;
    write_checksum(model);
    check_mutation(directory, path, model, TA_MODEL_WINDOWS_BYTES, "invalid Windows phrase reduction recipe");
    first[1] ^= 1;
    uint8_t *pair = first + TA_WINDOWS_REDUCTION_MODES;
    uint8_t original = pair[0];
    pair[0] = pair[1];
    write_checksum(model);
    check_mutation(directory, path, model, TA_MODEL_WINDOWS_BYTES, "invalid Windows phrase reduction recipe");
    pair[0] = original;
    write_checksum(model);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    load_asset(argv[1], NULL);
    char path[1200];
    int length = snprintf(path, sizeof path, "%s/pvdi-model.bin", argv[1]);
    assert(length > 0 && (size_t)length < sizeof path);
    FILE *file = fopen(path, "rb");
    uint8_t *model = calloc(TA_MODEL_WINDOWS_BYTES + 1, 1);
    assert(file && model);
    assert(fread(model, 1, TA_MODEL_WINDOWS_BYTES, file) == TA_MODEL_WINDOWS_BYTES);
    assert(fclose(file) == 0);

    char temporary[] = "/tmp/az-windows-model-XXXXXX";
    assert(mkdtemp(temporary));
    snprintf(path, sizeof path, "%s/pvdi-model.bin", temporary);
    const char *size_error = "invalid model size; regenerate pvdi-model.bin";
    check_mutation(temporary, path, model, TA_MODEL_WINDOWS_BYTES - 1, size_error);
    check_mutation(temporary, path, model, TA_MODEL_WINDOWS_BYTES + 1, size_error);
    check_reduction(temporary, path, model);
    check_mutation(temporary, path, model, TA_MODEL_WINDOWS_BYTES, NULL);
    assert(unlink(path) == 0 && rmdir(temporary) == 0);
    free(model);
    puts("Windows model asset, exact file extent, checksum, and phrase recipe checks passed");
    return 0;
}
