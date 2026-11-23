/* SPDX-License-Identifier: GPL-3.0-only */
/* The same register and unwind bridge, with private diagnostic original slots. */
#define azsdk_cache_entry_ azdiag_slip_entry_
#define azsdk_cache_entry_originals azdiag_slip_entry_originals
#define azsdk_cache_dispatch_call azdiag_slip_dispatch_call
#define azsdk_cache_invoke_original azdiag_slip_invoke_original
#include "../sdk_cache_entry.S"
