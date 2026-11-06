/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUEACTOR_H
#define AZCUEACTOR_H
#include "azexit.h"
#ifdef __cplusplus
extern "C" {
#define AZCUE_NOEXCEPT noexcept
#else
#define AZCUE_NOEXCEPT
#endif
/* Exact, relocated entries/tables from the verified stock image. No native task
 * is manufactured: memory_producer constructs and owns every queue envelope. */
struct azcue_actor_profile {
    uintptr_t manager_vptr, box_vptr, owner_vptr, task_vptr;
    uintptr_t memory_producer, post, execute, complete_dtor;
};
struct azcue_actor_callbacks {
    /* Runs with the registered manager alive on its native task owner. Never
     * retain manager. AZ_PENDING permits a later complete() from persistence. */
    int (*execute)(const struct azexit_cue_request *, void *manager,
                   struct azexit_result *, void *context);
    int (*current)(unsigned deck, uint64_t load_generation, void *context);
    void *context;
};
int azsdk_cue_actor_configure(const struct azcue_actor_profile *, azexit_install_fn,
                              const struct azcue_actor_callbacks *) AZCUE_NOEXCEPT;
/* Lifecycle caller pins manager through register and routes teardown through
 * retire BEFORE native destruction. Registration and drive are management work. */
int azsdk_cue_actor_register(unsigned, void *, uint64_t) AZCUE_NOEXCEPT;
unsigned azsdk_cue_actor_mask(void) AZCUE_NOEXCEPT;
void azsdk_cue_actor_retire(unsigned, void *manager) AZCUE_NOEXCEPT;
int azsdk_cue_actor_enqueue(const struct azexit_cue_request *) AZCUE_NOEXCEPT;
void azsdk_cue_actor_drive(unsigned) AZCUE_NOEXCEPT;
int azsdk_cue_actor_cancel(unsigned, uint64_t) AZCUE_NOEXCEPT;
int azsdk_cue_actor_peek(unsigned, struct azexit_result *) AZCUE_NOEXCEPT;
int azsdk_cue_actor_take(unsigned, struct azexit_result *) AZCUE_NOEXCEPT;
int azsdk_cue_actor_poll(unsigned, struct azexit_result *) AZCUE_NOEXCEPT;
int azsdk_cue_actor_release(unsigned, uint64_t) AZCUE_NOEXCEPT;
/* Queue another owned native envelope for the same pending request. A cleanup
 * continuation may run after deadline/load invalidation; execute must drain
 * existing receipts first and validate before dispatching additional work.
 * AZ_BUSY leaves state unchanged: retain the notification and retry from the
 * management tick. Native reply threads must not wait on an executing frame. */
int azsdk_cue_actor_resume(unsigned, uint64_t) AZCUE_NOEXCEPT;
int azsdk_cue_actor_complete(unsigned, uint64_t, const struct azexit_result *) AZCUE_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef AZCUE_NOEXCEPT
#endif
