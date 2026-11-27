/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "source_import_format.h"

#if defined(__aarch64__) && defined(__linux__)
#include <pthread.h>
#include <string.h>
#include <strings.h>

/* Exact 2.00 EP147 native audio factory profile. This uses the nonthrowing
 * file-open result; the higher-level cloud property adapter throws when the
 * same factory cannot read an audio file. */
#define FORMAT_FIRMWARE_200 2 /* EPB_V200 */
#define FORMAT_FACTORY_CREATE ((uintptr_t) 0x00e7baf8)
#define FORMAT_FACTORY_VTABLE ((uintptr_t) 0x02ae6778)
#define FORMAT_FACTORY_OPEN ((uintptr_t) 0x00dc4018)
#define FORMAT_OWNER_RELEASE ((uintptr_t) 0x006b5d88)
#define FORMAT_STRING_CTOR ((uintptr_t) 0x024a1180)
#define FORMAT_STRING_DTOR ((uintptr_t) 0x02469aa0)
#define FORMAT_MPEG4_READER ((uintptr_t) 0x02ae7e00)

struct format_file {
    char path[AZ_SOURCE_PATH_BYTES];
    uint64_t size, signature;
};

static pthread_once_t format_factory_once = PTHREAD_ONCE_INIT;
static void *format_factory;

__attribute__((visibility("hidden"))) void azspi_factory_sret(uintptr_t *owner, uintptr_t routine);
__asm__(".text\n.p2align 2\n.globl azspi_factory_sret\n.hidden azspi_factory_sret\nazspi_factory_sret:\n"
        "mov x8, x0\nbr x1\n");

static uintptr_t format_word(const void *object, size_t offset) {
    uintptr_t value;
    memcpy(&value, (const uint8_t *) object + offset, sizeof(value));
    return value;
}

static void format_build_factory(void) {
    uintptr_t owner[2] = {0};
    azspi_factory_sret(owner, FORMAT_FACTORY_CREATE);
    void *factory = (void *) owner[0];
    if (factory && format_word(factory, 0) == FORMAT_FACTORY_VTABLE)
        format_factory = factory;
    /* The native process static retains the primary owner. Release this genuine
     * shared_ptr copy after establishing the process-lifetime borrow. */
    if (owner[1])
        ((void (*)(void *)) FORMAT_OWNER_RELEASE)((void *) owner[1]);
}

static void *format_native_reader(const char *path, uintptr_t *string) {
    void (*string_create)(uintptr_t *, const char *) = (void *) FORMAT_STRING_CTOR;
    void *(*open_reader)(void *, const uintptr_t *, int *, unsigned, const uintptr_t *,
                         const uintptr_t *, unsigned, unsigned) = (void *) FORMAT_FACTORY_OPEN;
    string_create(string, path);
    int error = 0;
    const uintptr_t seek = 0;
    const uintptr_t frames[4] = {0}; /* Native empty SeekTable and FrameInfoList. */
    return open_reader(format_factory, string, &error, 1, &seek, frames, 0, 0);
}

static int format_read_file(const struct st_source *source, const char *audio, struct format_file *file) {
    if (!st_source_path(source->mount, audio, file->path))
        return 0;
    struct stat st;
    if (stat(file->path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return 0;
    file->size = (uint64_t) st.st_size;
    file->signature = st_stat_sig(&st);
    return 1;
}

static unsigned format_mpeg4_type(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension && !strcasecmp(extension, ".m4a") ? 4u : 3u;
}

static unsigned format_export_type(uintptr_t vtable, const char *path) {
    static const struct {
        uintptr_t vtable;
        unsigned file_type;
    } types[] = {
        {0x02ae7730, 1},  /* MP3 */
        {0x02ae6a98, 4},  /* AAC */
        {0x02ae71e8, 5},  /* FLAC */
        {0x02ae6ed0, 6},  /* ALAC */
        {0x02ae8360, 11}, /* WAV */
        {0x02ae6d40, 12}, /* AIFF */
    };
    if (vtable == FORMAT_MPEG4_READER)
        return format_mpeg4_type(path);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (types[i].vtable == vtable)
            return types[i].file_type;
    }
    return 0;
}

static int format_sample_clock(uintptr_t vtable, void *reader, struct azspi_audio *format) {
    const void *methods = (const void *) vtable;
    int64_t (*samples)(void *) = (void *) format_word(methods, 0x40);
    uint32_t (*sample_rate)(void *) = (void *) format_word(methods, 0x48);
    int64_t count = samples(reader);
    uint32_t rate = sample_rate(reader);
    if (count < 1 || !rate)
        return 0;
    __uint128_t duration = ((__uint128_t) count * 1000 + rate / 2) / rate;
    if (duration < 1 || duration > UINT32_MAX)
        return 0;
    format->duration_ms = (uint32_t) duration;
    format->sample_rate = rate;
    return 1;
}

static int format_bitrate(uint64_t bytes, struct azspi_audio *format) {
    __uint128_t bits = (__uint128_t) bytes * 8;
    __uint128_t rate = (bits + format->duration_ms / 2) / format->duration_ms;
    if (rate > UINT32_MAX)
        return 0;
    format->bitrate_kbps = (uint32_t) rate;
    return 1;
}

static int format_measure(void *reader, const struct format_file *file, struct azspi_audio *out) {
    uintptr_t vtable = format_word(reader, 0);
    unsigned type = format_export_type(vtable, file->path);
    if (!type)
        return AZ_UNSUPPORTED;
    struct azspi_audio measured = {.file_type = type};
    if (!format_sample_clock(vtable, reader, &measured) || !format_bitrate(file->size, &measured))
        return AZ_FAILED;
    *out = measured;
    return AZ_OK;
}

static int format_measure_reader(void *reader, const struct format_file *file, struct azspi_audio *out) {
    if (!reader)
        return AZ_INVALID;
    int result = format_measure(reader, file, out);
    const void *methods = (const void *) format_word(reader, 0);
    ((void (*)(void *)) format_word(methods, 8))(reader); /* Native deleting destructor. */
    return result;
}

static int format_probe_file(const struct format_file *file, struct azspi_audio *out) {
    if (pthread_once(&format_factory_once, format_build_factory) || !format_factory)
        return AZ_UNSUPPORTED;
    uintptr_t string;
    void *reader = format_native_reader(file->path, &string);
    struct azspi_audio measured;
    int result = format_measure_reader(reader, file, &measured);
    ((void (*)(uintptr_t *)) FORMAT_STRING_DTOR)(&string);
    if (st_disk_sig(file->path) != file->signature)
        return AZ_STALE;
    if (result == AZ_OK)
        *out = measured;
    return result;
}

int azspi_probe_format(int firmware, const struct st_source *source, const char *audio, struct azspi_audio *out) {
    if (firmware != FORMAT_FIRMWARE_200)
        return AZ_UNSUPPORTED;
    if (!source || !out)
        return AZ_INVALID;
    struct format_file file;
    if (!format_read_file(source, audio, &file))
        return AZ_STALE;
    return format_probe_file(&file, out);
}
#else
int azspi_probe_format(int firmware, const struct st_source *source, const char *audio, struct azspi_audio *out) {
    (void) firmware;
    (void) source;
    (void) audio;
    (void) out;
    return AZ_UNSUPPORTED;
}
#endif
