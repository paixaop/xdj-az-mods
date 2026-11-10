/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <stdio.h>
#include "azmod-mod.h"
#include "../common/azstatus_source.h"
#include "sdk_status_map_source.c"
static uintptr_t model[120],pack[40],phrase[9],grid[9],phrases[7],beats[7];
static unsigned grants, releases, proof, retired, forwarded;
static void *fail;
static unsigned char take(uintptr_t object) {
    if (object==(uintptr_t)fail) return 0;
    ++grants;return 1;
}
static unsigned char put(uintptr_t object) { assert(object);++releases;return 0; }
static int prove(uintptr_t o,uintptr_t p,uintptr_t gh,uintptr_t gc,uintptr_t ph,uintptr_t pc) {
    assert(o==(uintptr_t)model+0x268 && p==(uintptr_t)pack);
    assert(gh==(uintptr_t)grid && gc==(uintptr_t)beats && ph==(uintptr_t)phrase && pc==(uintptr_t)phrases);
    assert(grants==releases+3 || grants==releases+4);++proof;return 1;
}
static void retire(uintptr_t o) { assert(o==(uintptr_t)model+0x268);++retired; }
static uint64_t native_set(uintptr_t m,const uintptr_t *ph,const uintptr_t *gh) {
    assert(m==(uintptr_t)model);++forwarded;
    model[0x2c8/8]=*ph;model[0x2d0/8]=*gh;return 0xabcde;
}
static uint64_t native_clear(uintptr_t m) {
    assert(m==(uintptr_t)model);++forwarded;
    model[0x2c8/8]=model[0x2d0/8]=0;return 0xbabce;
}
static uint64_t native_update(uintptr_t task) { assert(task);return 471; }
int main(void) {
    struct sm_profile profile={.set_return=100,.clear_return=120,.owner_vptr=901,.pack_vptr=902};
    profile.functions[2].address=(uintptr_t)take;profile.functions[3].address=(uintptr_t)put;
    sm_profile=&profile;
    struct azn_maps_api api={.size=sizeof api,.version=1,.prove_pair=prove,.retire_pair=retire};
    sm_original[0]=native_set;sm_original[1]=native_clear;atomic_store(&sm_maps,&api);atomic_store(&sm_ready,1);
    model[0x268/8]=profile.owner_vptr;pack[0]=profile.pack_vptr;
    pack[0x120/8]=(uintptr_t)phrase;pack[0xd8/8]=(uintptr_t)grid;
    phrase[0x28/8]=(uintptr_t)phrases;grid[0x28/8]=(uintptr_t)beats;
    const uintptr_t *ph=pack+0x120/8,*gh=pack+0xd8/8;
    assert(sm_set_forward((uintptr_t)model,ph,gh,100)==0xabcde);
    assert(proof==1 && retired==1 && forwarded==1 && grants==releases+1);
    assert(sm_set_forward((uintptr_t)model,ph,gh,99)==0xabcde && proof==1 && forwarded==2);
    fail=phrases;
    assert(sm_set_forward((uintptr_t)model,ph,gh,100)==0xabcde && proof==1 && grants==releases+1);
    fail=NULL;uintptr_t impostor=*ph;
    assert(sm_set_forward((uintptr_t)model,&impostor,gh,100)==0xabcde && proof==1 && forwarded==4);
    uintptr_t task[5]={0};task[3]=(uintptr_t)model+0x268;task[4]=(uintptr_t)phrase;
    sm_original[2]=native_update;
    assert(sm_update((uintptr_t)task,0)==471 && proof==2);
    task[4]=(uintptr_t)grid;
    assert(sm_update((uintptr_t)task,0)==471 && proof==2);
    assert(sm_clear_forward((uintptr_t)model,120)==0xbabce && retired==2 && forwarded==5 && grants==releases);
    puts("GUI native map: paired pack members, content leases, replacement, clear and original return passed");
}
