/* SPDX-License-Identifier: GPL-3.0-only */
/* The real private live export: bounded writes, input prefix, versions and stream copies. */
#include "../common/azdeckinfo.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NDECK 4
static int g_sdk_live_on = 1, ready = 1;
static unsigned native_mask = 5;
static struct azsdk_live_sample current = {
    .unit_generation = 11, .observed_fields = 7, .valid_fields = 7, .mode_is_2 = 1, .play_pause = 0, .play_aux = 1, .quantize = 0, .key_lock = 1, .revisions = {12, 23, 34}, .observed_ns = {123, 234, 345}};
int azsdk_live_read(int deck, struct azsdk_live_sample *out) {
    assert(deck >= 0 && deck < NDECK);
    if (!ready) return 0;
    *out = current;
    return 1;
}
unsigned azsdk_live_capabilities(void) { return native_mask; }
#include LIVE_EXPORT_INC
#ifdef LIVE_CONFIG_INC
#define DJPUCTOR_DEFAULT ((uintptr_t) 0x10000)
#define F_OK 0
static uintptr_t g_djpuctor_addr;
static int sdk_loop_build = 2, track_flag, live_flag, configure_calls, cue_configure_calls;
void azsdk_live_configure(int on, int build) {
    assert(on == 1 && build == 2);
    configure_calls++;
}
void azsdk_cue_configure(int on, int build) { assert(on == 1 && build == 2); ++cue_configure_calls; }
static int flag_access(const char *path, int mode) {
    assert(mode == F_OK);
    if (!strcmp(path, "/home/root/settings/azmod.track-data.on")) return track_flag ? 0 : -1;
    assert(!strcmp(path, "/home/root/settings/azmod.native-live.on"));
    return live_flag ? 0 : -1;
}
#define access flag_access
#include LIVE_CONFIG_INC
#undef access
static void configuration(void) {
    for (int td = 0; td < 2; ++td)
        for (int native = 0; native < 2; ++native)
            for (int track = 0; track < 2; ++track)
                for (int original = 0; original < 2; ++original) {
                    g_djpuctor_addr = original ? DJPUCTOR_DEFAULT : 0;
                    track_flag = track;
                    live_flag = native;
                    g_sdk_live_on = configure_calls = cue_configure_calls = 0;
                    live_configuration(td);
                    int expected = td && native && track && original;
                    assert(configure_calls == expected && cue_configure_calls == expected && g_sdk_live_on == expected);
                }
    g_sdk_live_on = 1;
}
#endif
static void buffers(int version, int fill) {
    _Alignas(struct azstem_live_observation_v1) unsigned char bytes[128], before[128];
    for (unsigned n = 0; n < sizeof bytes; ++n) {
        memset(bytes, 0xa5, sizeof bytes);
        const uint32_t header[2] = {n, (uint32_t) version};
        memcpy(bytes, header, sizeof header);
        memcpy(before, bytes, sizeof bytes);
        int rc = azstem_live_observation_v1(2, (void *) bytes);
        if (n < sizeof(struct azstem_live_observation_v1) || !fill) {
            assert(rc == -1 && !memcmp(bytes, before, sizeof bytes));
        } else {
            const struct azstem_live_observation_v1 *out = (const void *) bytes;
            assert(!rc && out->size == n && out->version == 1);
            assert(!memcmp(&out->sample, &current, sizeof current));
            assert(!memcmp(bytes + sizeof *out, before + sizeof *out, sizeof bytes - sizeof *out));
        }
    }
}
int main(void) {
#ifdef LIVE_CONFIG_INC
    configuration();
#endif
    _Static_assert(sizeof(struct azsdk_live_sample) == 88, "private live sample");
    _Static_assert(sizeof(struct azstem_live_observation_v1) == 96, "private versioned wrapper");
    uint32_t *prefix = malloc(sizeof *prefix);
    assert(prefix);
    *prefix = sizeof *prefix;
    assert(azstem_live_observation_v1(0, (void *) prefix) == -1 && *prefix == sizeof *prefix);
    free(prefix);
    assert(azstem_live_observer_mask_v1() == 5);
    buffers(1, 1);
    buffers(0, 0);
    buffers(2, 0);
    current.valid_fields = 0;
    current.mode_is_2 = current.play_pause = current.play_aux = current.quantize = current.key_lock = -1;
    buffers(1, 1); /* Invalid receipts still carry their independent clocks. */
    ready = 0;
    buffers(1, 0);
    ready = 1;
    g_sdk_live_on = 0;
    assert(!azstem_live_observer_mask_v1());
    buffers(1, 0);
    g_sdk_live_on = 1;
    struct azstem_live_observation_v1 out = {.size = sizeof out, .version = 1}, before = out;
    assert(azstem_live_observation_v1(-1, &out) == -1 && !memcmp(&out, &before, sizeof out));
    assert(azstem_live_observation_v1(4, &out) == -1 && !memcmp(&out, &before, sizeof out));
    assert(azstem_live_observation_v1(0, NULL) == -1);
    puts("sdk live export: versions, raw prefix, guarded writes, independent streams and absent observations passed");
}
