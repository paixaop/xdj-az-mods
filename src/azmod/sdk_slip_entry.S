/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the qualified register/CFI bridge without sharing source-cache slots. */
#define azsdk_cache_entry_ azsdk_slip_entry_
#define azsdk_cache_entry_originals azsdk_slip_entry_originals
#define azsdk_cache_dispatch_call azsdk_slip_dispatch_call
#define azsdk_cache_invoke_original azsdk_slip_invoke_original
#include "sdk_cache_entry.S"
