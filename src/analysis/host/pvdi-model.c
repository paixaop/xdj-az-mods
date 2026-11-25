/* SPDX-License-Identifier: GPL-3.0-only */
#include "pvdi-model.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    window_values = 240,
    features = 29,
    double_bytes = 8,
    header_bytes = 32,
    constants_bytes = (window_values + 2 * features) * double_bytes,
    weights_bytes = 1669314 * 4,
    model_bytes = header_bytes + constants_bytes + weights_bytes
};

struct signature {
    const char *sha256;
    uint32_t size;
};

struct file_range {
    uint32_t offset, length;
};

struct graph_layout {
    struct signature file;
    struct file_range tensors[12];
};

struct pvdi_profile {
    const char *bundle_version;
    struct signature arm64;
    const struct graph_layout *graph;
    uint32_t window_offset, normalization_offset;
    uint64_t final_scale, final_offset;
};

/* Exact tensor_content ranges in this pinned GraphDef, in AZPVDI01 order.
 * Versions that ship the identical GraphDef can share this layout. */
static const struct graph_layout graph_7219 = {
    .file = {"4f1009aea543aaebd27a232d26de057780019e710a4c2075dd2e5ca9b01d4ef6", 6758758},
    .tensors = {
        {0x2627e, 1167360},  /* Forward layer 0 kernel */
        {0x1434b6, 4096},    /* Forward layer 0 bias */
        {0x1457fa, 2097152}, /* Forward layer 1 kernel */
        {0x345a32, 4096},    /* Forward layer 1 bias */
        {0x34ec96, 1167360}, /* Backward layer 0 kernel */
        {0x46bece, 4096},    /* Backward layer 0 bias */
        {0x46e212, 2097152}, /* Backward layer 1 kernel */
        {0x66e44a, 4096},    /* Backward layer 1 bias */
        {0x17d, 131072},     /* Dense layer 0 weights */
        {0x20247, 256},      /* Dense layer 0 bias */
        {0x20420, 512},      /* Dense layer 1 weights */
        {0x206e5, 8}         /* Dense layer 1 bias */
    }
};

/* Index by the full CFBundleVersion. Offsets are into the thin Mach-O file.
 * The last normalization pair is recorded as exact IEEE 754 binary64 bits. */
static const struct pvdi_profile profiles[] = {
    {
        .bundle_version = "7.2.19.0342",
        .arm64 = {"2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907", 135191888},
        .graph = &graph_7219,
        .window_offset = 0x378ef70,
        .normalization_offset = 0x378f6f0,
        .final_scale = UINT64_C(0x3ffc0e4d228f3eb5),
        .final_offset = UINT64_C(0xbf63f11a04352fc7)
    }
};

static CFDictionaryRef copy_bundle_info(const char *app)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)app,
                                                          (CFIndex)strlen(app), true);
    if (!url)
        return NULL;
    CFDictionaryRef info = CFBundleCopyInfoDictionaryForURL(url);
    CFRelease(url);
    return info;
}

static bool bundle_version(const char *app, char *version, size_t size)
{
    CFDictionaryRef info = copy_bundle_info(app);
    if (!info)
        return false;
    CFTypeRef value = CFDictionaryGetValue(info, CFSTR("CFBundleVersion"));
    bool ok = value && CFGetTypeID(value) == CFStringGetTypeID() &&
              CFStringGetCString((CFStringRef)value, version, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(info);
    return ok;
}

const struct pvdi_profile *pvdi_profile_for_app(const char *app)
{
    char version[64];
    if (!bundle_version(app, version, sizeof version)) {
        fprintf(stderr, "Cannot read rekordbox bundle version from %s\n", app);
        return NULL;
    }
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
        if (strcmp(version, profiles[i].bundle_version) == 0)
            return &profiles[i];
    fprintf(stderr, "Unsupported rekordbox build %s. Supported builds:", version);
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
        fprintf(stderr, " %s", profiles[i].bundle_version);
    fputc('\n', stderr);
    return NULL;
}

static bool matches_sha256(const uint8_t *data, const struct signature *signature)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    CC_SHA256(data, signature->size, digest);
    for (size_t i = 0; i < sizeof digest; ++i) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[sizeof hex - 1] = '\0';
    return strcmp(hex, signature->sha256) == 0;
}

static uint8_t *read_verified(const char *path, struct signature signature, const char *version)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }
    uint8_t *data = malloc(signature.size);
    bool complete = data && fread(data, 1, signature.size, file) == signature.size &&
                    fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (complete && matches_sha256(data, &signature))
        return data;
    fprintf(stderr, "Input does not match the verified rekordbox %s file: %s\n", version, path);
    free(data);
    return NULL;
}

static bool contains_range(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static void write_le(uint8_t *output, uint64_t value, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        output[i] = (uint8_t)value;
        value >>= 8;
    }
}

static bool copy_constants(uint8_t *output, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    size_t window_bytes = window_values * double_bytes;
    if (!contains_range(profile->arm64.size, profile->window_offset, window_bytes) ||
        !contains_range(profile->arm64.size, profile->normalization_offset,
                        2 * (features - 1) * double_bytes))
        return false;
    memcpy(output, arm64 + profile->window_offset, window_bytes);
    const uint8_t *table = arm64 + profile->normalization_offset;
    uint8_t *scales = output + window_bytes;
    uint8_t *offsets = scales + features * double_bytes;
    for (size_t i = 0; i < features - 1; ++i) {
        size_t slot = (i / 2 * 4 + i % 2) * double_bytes;
        memcpy(scales + i * double_bytes, table + slot, double_bytes);
        memcpy(offsets + i * double_bytes, table + slot + 2 * double_bytes, double_bytes);
    }
    write_le(scales + (features - 1) * double_bytes, profile->final_scale, double_bytes);
    write_le(offsets + (features - 1) * double_bytes, profile->final_offset, double_bytes);
    return true;
}

static bool copy_weights(uint8_t *output, const uint8_t *graph, const struct graph_layout *layout)
{
    size_t copied = 0;
    for (size_t i = 0; i < sizeof layout->tensors / sizeof layout->tensors[0]; ++i) {
        const struct file_range *range = &layout->tensors[i];
        if (!contains_range(layout->file.size, range->offset, range->length) ||
            !contains_range(weights_bytes, copied, range->length))
            return false;
        memcpy(output + copied, graph + range->offset, range->length);
        copied += range->length;
    }
    return copied == weights_bytes;
}

static void write_header(uint8_t *model)
{
    uint32_t hash = 2166136261u;
    for (size_t i = header_bytes; i < model_bytes; ++i)
        hash = (hash ^ model[i]) * 16777619u;
    memcpy(model, "AZPVDI01", 8);
    write_le(model + 8, model_bytes - header_bytes, 4);
    write_le(model + 12, hash, 4);
    memset(model + 16, 0, header_bytes - 16);
}

static bool write_model(const char *path, uint8_t *model)
{
    write_header(model);
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return false;
    }
    bool written = fwrite(model, 1, model_bytes, file) == model_bytes;
    int closed = fclose(file);
    if (!written || closed)
        fprintf(stderr, "Could not write the complete model to %s\n", path);
    return written && !closed;
}

static bool build_model(const struct pvdi_profile *profile, const uint8_t *graph,
                        const uint8_t *arm64, const char *output_path)
{
    uint8_t *model = malloc(model_bytes);
    if (!model) {
        perror("Allocate vocal model");
        return false;
    }
    bool valid = copy_constants(model + header_bytes, arm64, profile) &&
                 copy_weights(model + header_bytes + constants_bytes, graph, profile->graph);
    if (!valid)
        fprintf(stderr, "Invalid model layout for rekordbox %s\n", profile->bundle_version);
    bool ok = valid && write_model(output_path, model);
    free(model);
    return ok;
}

bool pvdi_import(const struct pvdi_profile *profile, const char *graph_path,
                 const char *arm64_path, const char *output_path)
{
    uint8_t *graph = read_verified(graph_path, profile->graph->file, profile->bundle_version);
    if (!graph)
        return false;
    uint8_t *arm64 = read_verified(arm64_path, profile->arm64, profile->bundle_version);
    bool ok = arm64 && build_model(profile, graph, arm64, output_path);
    free(arm64);
    free(graph);
    return ok;
}
