/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile against the previous public header. Both loaders must run this same
 * ELF and honor a write and read through its original service table. */
#include "azmod-mod.h"
#include <stdio.h>
#define ID "legacy-status-client"
static int init(const az_core_v1 *core) {
    if (!AZ_HAS(core,on_deck_event) || !core->cfg_set || !core->cfg_get) return -1;
    core->cfg_set(ID,"compat",137);
    if (core->cfg_get(ID,"compat",0)!=137) return -1;
    puts("LEGACY setting round trip");
    return 0;
}
static const char *const tags[]={"any",NULL};
static const az_mod_v1 mod={.abi=AZ_MOD_ABI,.size=sizeof mod,.id=ID,.fw_tags=tags,.init=init};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) {return &mod;}
