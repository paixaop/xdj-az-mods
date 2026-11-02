/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azdeckinfo.h"
#include "../../src/azstem/ep147rate.h"
#include <stdio.h>
#include <string.h>
#ifdef POSITION_MOCK_OBJECT
int azstem_deck_snapshot;
#else
static unsigned calls;
int azstem_deck_snapshot(int deck,struct azstem_deck_snap *out) {
    if(deck<0 || deck>=4 || !out || out->size!=sizeof *out) return -1;
    int size=out->size; memset(out,0,sizeof *out); out->size=size;
    out->loaded=1; out->content_id=(unsigned)deck+1;
    snprintf(out->title,sizeof out->title,"Mock deck %d",deck+1);
    out->playhead=(long long)(deck+1)*AZ_EP147_RATE+(calls++/4)*(AZ_EP147_RATE/10);
    out->pos_s=(double)out->playhead/AZ_EP147_RATE;
    return 0;
}
#endif
