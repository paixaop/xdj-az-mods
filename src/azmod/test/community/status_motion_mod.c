/* SPDX-License-Identifier: GPL-3.0-only */
/* Two independent motion ceilings in a normally loaded, tickless listener. */
#include "azmod-mod.h"
#include <stdio.h>
#define ID "status-motion"
static unsigned rates[]={1,30};
static az_deck_subscription handles[2];
static void changed(const az_deck_status_event_v1 *e,void *u) {
    const az_status_position_v1 *p=e->view->position;
    if (!p || e->deck!=2 || e->view->transport || e->topics!=AZ_STATUS_TOPIC(AZ_STATUS_POSITION)) __builtin_trap();
    double seconds=p->head_info.info.availability==AZ_AVAIL_READY ? p->head.seconds : -1;
    printf("MOTION %u %u %.0f %llu\n",*(unsigned *)u,e->kind,seconds,
           (unsigned long long)(e->occurrence ? e->occurrence->ordinal : 0));
}
static int init(const az_core_v1 *core) {
    if (!AZ_HAS(core,library_service)) return -1;
    const az_library_v1 *lib=core->library_service(AZ_LIBRARY_ABI);
    if (!AZ_LIB_HAS(lib,on_deck_status)) return -1;
    for (unsigned i=0;i<2;i++) {
        const az_deck_watch_v1 watch={sizeof watch,AZ_DATA_VERSION,4,rates[i],AZ_STATUS_TOPIC(AZ_STATUS_POSITION)};
        if (lib->on_deck_status(ID,&watch,changed,&rates[i],&handles[i])!=AZ_OK) return -1;
    }
    return 0;
}
static const char *const tags[]={"any",NULL};
static const az_mod_v1 mod={.abi=AZ_MOD_ABI,.size=sizeof mod,.id=ID,.fw_tags=tags,.init=init};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) { return &mod; }
