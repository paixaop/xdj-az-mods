/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEREPLYNATIVE_H
#define AZCACHEREPLYNATIVE_H
#include "azcachebirth.h"
#include "azcachereply.h"
#include "azcacheentry.h"
namespace azsdk_cache_reply {
/* Valid only during the same genuinely owned Reception original invocation.
 * borrowed_stocker is copied opaque context, never an owner or permission to
 * dereference/reacquire that object. pack_id and RequestID remain distinct
 * native types; neither is a source/track/player load generation. */
struct CallbackReceipt {
    Receipt selected;
    std::uint32_t pack_id = 0;
    std::uintptr_t borrowed_stocker = 0;
};
__attribute__((visibility("hidden"))) void forward_ticket_owned(azce_call *, azsdk_cache_birth::Forward);
__attribute__((visibility("hidden"))) void forward_reception_owned(azce_call *, azsdk_cache_birth::Forward);
__attribute__((visibility("hidden"))) bool current_callback_owned(CallbackReceipt &) noexcept;
}
#endif
