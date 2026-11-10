/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTATUS_IDENTITY_H
#define AZSTATUS_IDENTITY_H
#include <stdint.h>

/* Process-local provider namespaces. Native unit IDs occupy the low range.
 * Independent writer incarnations use a 56-bit sequence within their prefix.
 * A shared owner may supply several fields under one provider. FILE providers
 * use the high half and remain distinct across source/component incarnations. */
#define AZSP_UNIT_MAX UINT64_C(0x0fffffffffffffff)
#define AZSP_SERIAL_MAX UINT64_C(0x00ffffffffffffff)
#define AZSP_RAW UINT64_C(0x1000000000000000)
#define AZSP_MEMORY UINT64_C(0x2000000000000000)
#define AZSP_MASTER UINT64_C(0x2100000000000001)
#define AZSP_TEMPO UINT64_C(0x2100000100000000)
#define AZSP_TEMPO_MAX UINT64_C(64)
#define AZSP_SETTINGS UINT64_C(0x2200000000000000)
#define AZSP_KEY UINT64_C(0x2400000000000000)
#define AZSP_GUI_MODEL UINT64_C(0x2500000000000000)
#define AZSP_SLIP UINT64_C(0x2600000000000000)
#define AZSP_PAD UINT64_C(0x2700000000000000)
#define AZSP_MAP UINT64_C(0x3100000000000000)
#define AZSP_DERIVED UINT64_C(0x3200000000000000)
#define AZSP_HOT UINT64_C(0x4000000000000000)
#define AZSP_FILE UINT64_C(0x8000000000000000)
#endif
