/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_CACHE_H
#define AZMOD_SOURCE_PROVIDER_CACHE_H

#include "source_provider.h"

#pragma GCC visibility push(hidden)

struct azsp_cache;

/* Worker only. Parent is an absolute directory reserved for AZ-MODS. The
 * returned root and its lock remain owned until this firmware process exits.
 * Opening a new cache reclaims verified roots whose owner has exited. */
int azsp_cache_open(const char *parent, struct azsp_cache **out);
const char *azsp_cache_root(const struct azsp_cache *cache);
/* Discard an unpublished cache. The caller has exclusive ownership and no
 * native mount or reader was allowed to use its root. */
void azsp_cache_discard(struct azsp_cache *cache);

/* Worker only. Retain a prepared stage before publishing its audio path to
 * firmware. The caller can drop its reference after a successful hold. Native
 * asynchronous consumers keep the pathname available for the session. */
int azsp_cache_hold(struct azsp_cache *cache, struct azsp_stage *stage);

#pragma GCC visibility pop
#endif
