/* SPDX-License-Identifier: GPL-3.0-only */
/* One TLS context per thread, shared by all private cache observer modules. */
struct azcm_frame;
struct azcr_scope;
__attribute__((visibility("hidden"))) _Thread_local struct azcm_frame *azcm_head;
__attribute__((visibility("hidden"))) _Thread_local struct azcr_scope *azcr_current;
