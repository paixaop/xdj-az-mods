/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_OWNED_H
#define AZSTATUS_OWNED_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal to native azstem producers. A successful acquire owns one token
 * and keeps its nominal PlayMusicPack alive until azn_pin_drop(token). The
 * immutable native object address may be used only under that lease. Current
 * deck membership is separate from object lifetime; revalidate after copying. */
struct azn_pack_view { uint64_t token,unit,fence,load; uintptr_t object; };
typedef void (*azn_pack_observer)(unsigned deck,uint64_t unit,uint64_t fence,uint64_t load);
__attribute__((visibility("hidden"))) int azn_pack_acquire(unsigned deck,struct azn_pack_view *out);
__attribute__((visibility("hidden"))) int azn_pack_current(unsigned deck,const struct azn_pack_view *view);
/* Conservative retention check for a pack already held by the caller.
 * A deck registration still holds the pack while its load retires. */
__attribute__((visibility("hidden"))) int azn_pack_retains(uintptr_t pack);
/* Registers a process-lifetime bounded notification callback. A withdrawal
 * carries load zero. Concurrent callbacks can arrive out of order. */
__attribute__((visibility("hidden"))) int azn_pack_observe(azn_pack_observer observer);
/* Only a genuine *owned* native argument may be pinned. Weak pointer values
 * from another object or telemetry cannot be promoted through this interface. */
__attribute__((visibility("hidden"))) uint64_t azn_owned_pin(uintptr_t owned);
__attribute__((visibility("hidden"))) int azn_pin_hold(uint64_t token,uintptr_t *object);
__attribute__((visibility("hidden"))) void azn_pin_drop(uint64_t token);
#ifdef __cplusplus
}
#endif
#endif
