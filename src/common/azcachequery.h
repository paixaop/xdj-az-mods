/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEQUERY_H
#define AZCACHEQUERY_H
#include "azcachebirth.h"

/* Private copied evidence for the synchronous native reply path. A raw tag23
 * value is not a TrackID or committed load. No native result/row is retained.
 * The reader succeeds only within the same owned MusicLibraryServer message,
 * after its supported selected-cache/adapter query, with unchanged source,
 * selected storage and cache provenance. No source/command capability. */
namespace azsdk_cache_query {
struct Evidence {
    azsdk_cache_birth::Observation cache;
    std::uint32_t sequence = 0, client = 0, selector = 0, start = 0, count = 0;
};
__attribute__((visibility("hidden"))) bool current_owned(Evidence &) noexcept;
}
#endif
