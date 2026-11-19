/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_command_target.h"
#include "../common/aztimestamp.h"
#include <string.h>

static int target_receipt(uint64_t observed, uint64_t now)
{
    if (!observed || !now) return 0;
    int order = compare_timestamps(observed, now, UINT64_C(1000000000), UINT64_C(1000000000), 1);
    return order != AZ_TIME_INVALID && order != AZ_TIME_AFTER;
}
static int target_loaded(const az_deck_snapshot *s, int deck)
{
    return s->size >= sizeof *s && s->version == AZ_DATA_VERSION &&
           s->deck == (uint32_t)deck && (s->valid & AZ_LIVE_LOADED) && s->loaded == 1;
}
static int target_load(const az_deck_snapshot *s, uint64_t expected, uint64_t now)
{
    return (s->valid & AZ_LIVE_NATIVE_LOAD) && s->native_load_unit_generation &&
           s->native_load_generation == expected && target_receipt(s->native_load_observed_ns, now);
}
static int target_cues(const az_native_cue_snapshot *c, int deck, uint64_t load)
{
    return c->size >= sizeof *c && c->version == AZ_DATA_VERSION && c->deck == (uint32_t)deck &&
           c->revision && c->count <= AZ_NATIVE_CUE_LIMIT && c->native_load_generation == load &&
           c->source_rate_hz && c->source_rate_hz < 0x7fffffffu;
}
static int target_capabilities(const az_data_capabilities *c)
{
    return c->size >= sizeof *c && c->version == AZ_DATA_VERSION;
}
static int target_source_valid(const struct az_command_target_source *source)
{
    return source && source->deck_snapshot && source->native_cues && source->capabilities;
}
static int target_capture(const struct az_command_target_source *source, int deck,
                          uint64_t load, uint64_t now, az_command_target_v1 *v)
{
    int rc = source->deck_snapshot(deck, &v->deck);
    if (rc != AZ_OK) return rc;
    if (!target_loaded(&v->deck, deck)) return AZ_PENDING;
    if (!target_load(&v->deck, load, now)) return AZ_STALE;
    rc = source->native_cues(deck, &v->cues);
    if (rc != AZ_OK) return rc;
    return target_cues(&v->cues, deck, load) ? AZ_OK : AZ_STALE;
}
static int target_same(const az_command_target_v1 *a, const az_command_target_v1 *b)
{
    return a->deck.native_load_unit_generation == b->deck.native_load_unit_generation &&
           a->deck.native_load_invalidation_epoch == b->deck.native_load_invalidation_epoch &&
           a->cues.revision == b->cues.revision && a->cues.source_rate_hz == b->cues.source_rate_hz;
}
static int target_caps_read(const struct az_command_target_source *source, uint64_t *commands)
{
    az_data_capabilities caps = { .size = sizeof caps };
    int rc = source->capabilities(&caps);
    if (rc != AZ_OK) return rc;
    if (!target_capabilities(&caps)) return AZ_FAILED;
    *commands = caps.commands;
    return AZ_OK;
}
static az_command_target_v1 target_empty(void)
{
    az_command_target_v1 v = { .size = sizeof v, .version = AZ_DATA_VERSION };
    v.deck.size = sizeof v.deck;
    v.cues.size = sizeof v.cues;
    return v;
}
static int target_collect(const struct az_command_target_source *source, int deck,
                          uint64_t load, uint64_t now, az_command_target_v1 *v)
{
    int rc = target_caps_read(source, &v->commands);
    if (rc != AZ_OK) return rc;
    if (!v->commands) return AZ_UNSUPPORTED;
    rc = target_capture(source, deck, load, now, v);
    if (rc != AZ_OK) return rc;
    az_command_target_v1 after = target_empty();
    rc = target_capture(source, deck, load, now, &after);
    if (rc != AZ_OK) return rc;
    if (!target_same(v, &after)) return AZ_STALE;
    rc = target_caps_read(source, &after.commands);
    if (rc != AZ_OK) return rc;
    return v->commands == after.commands ? AZ_OK : AZ_STALE;
}
int az_command_target_read(const struct az_command_target_source *source, int deck,
                           uint64_t load, uint64_t now, az_command_target_v1 *out)
{
    if (!out || out->size < sizeof *out || deck < 0 || deck >= 4 || !load || !now) return AZ_INVALID;
    if (!target_source_valid(source)) return AZ_INVALID;
    az_command_target_v1 value = target_empty();
    int rc = target_collect(source, deck, load, now, &value);
    if (rc != AZ_OK) return rc;
    value.size = out->size;
    memcpy(out, &value, sizeof value);
    return AZ_OK;
}
