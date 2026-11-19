/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_AAC_H
#define TA_AAC_H
#include "PortableDSP.h"
typedef struct ta_aac ta_aac;
int ta_aac_candidate(const char *path);
ta_aac *ta_aac_open(const char *path, unsigned *rate, uint64_t *frames);
int64_t ta_aac_read(ta_aac *, int16_t *, uint64_t);
int ta_aac_seek(ta_aac *, uint64_t);
void ta_aac_timeline(ta_aac *, ta_decode_clock *);
void ta_aac_close(ta_aac *);
#endif
