/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "azloadobserver.h"
#include "azstatus_native.h"
#include "azmod-mod.h"

extern const struct azstatus_native_v1 *azstem_status_service_v1(void);
void azn_test_reserved(unsigned d,unsigned k) { (void)d; (void)k; }
int azn_register_source_capture(azn_source_capture_fn f) { (void)f; return 0; }
static uint64_t source_pin=17;
uint64_t azn_load_source_pin(unsigned d,uint64_t e,uint64_t l) { return d==0 && e==6 && l==4 ? source_pin:0; }
unsigned azn_load_kind(unsigned d,uint64_t e,uint64_t l) {(void)d;(void)e;(void)l;return 0;}
unsigned azn_load_original_bpm(unsigned d,uint64_t u,uint64_t e,uint64_t l,double *out) {(void)d;(void)u;(void)e;(void)l;(void)out;return AZN_BPM_UNKNOWN;}
int azn_load_track_matches(unsigned d,uint64_t e,uint64_t l,const unsigned char id[12]) {(void)d;(void)e;(void)l;(void)id;return 0;}
int azn_empty_seed(unsigned d,struct azstatus_native_receipt *r) {(void)d;(void)r;return 0;}
int azn_metadata_seed(unsigned d,struct azstatus_native_receipt *r) { (void)d;(void)r;return 0; }
int azn_install_terminal(int build,azn_setting_patch_fn fn) { (void)build; (void)fn; return 0; }
int azn_install_settings(int build,azn_setting_patch_fn fn) { (void)build; (void)fn; return 0; }
int azsdk_load_read(int d,struct azsdk_load_sample *out) {
    if (d) return 0;
    *out=(struct azsdk_load_sample){.unit_generation=2,.load_generation=4,.invalidation_epoch=6,.native_request_id=1};
    return 1;
}
static atomic_int paused, resume_probe;
static int probe(const char *path,int *found) {
    if (!strcmp(path,"/a.wav")) {
        atomic_store(&paused,1);
        while (!atomic_load(&resume_probe)) usleep(100);
        *found=1;
        return AZ_AVAIL_READY;
    }
    *found=!strcmp(path,"/c.wav");
    return AZ_AVAIL_READY;
}
static struct azstatus_native_receipt take_stems(const struct azstatus_native_v1 *api,unsigned cut) {
    struct pollfd p={.fd=api->wake_fd(),.events=POLLIN};
    for (int i=0;i<10;i++) {
        struct azstatus_native_receipt r={.size=sizeof r,.version=1};
        while (api->take(0,&r)) {
            assert(r.kind==AZN_STEMS && r.attempt<=cut && !r.lost_topics && r.load_generation==4);
            if (r.attempt==cut && r.valid==AZ_AVAIL_READY) return r;
        }
        assert(poll(&p,1,1000)==1);
        uint64_t wakes;
        assert(read(p.fd,&wakes,sizeof wakes)==sizeof wakes);
    }
    assert(0); return (struct azstatus_native_receipt){0};
}
int main(void) {
    const struct azstatus_native_v1 *api=azstem_status_service_v1();
    api->interest(0,AZ_STATUS_TOPIC(AZ_STATUS_STEMS));
    azn_stems_start(probe);
    azn_stems_control(0,0);
    api->bind_stems(0,1,4,"/a.wav");
    while (!atomic_load(&paused)) usleep(100);
    api->bind_stems(0,2,4,"/b.wav");
    atomic_store(&resume_probe,1);
    struct azstatus_native_receipt r=take_stems(api,2);
    assert(!r.a && !r.b && r.c==AZ_AVAIL_READY);
    api->bind_stems(0,1,4,"/a.wav"); /* delayed binding cannot restore the old source */
    api->bind_stems(0,3,4,"/c.wav");
    r=take_stems(api,3);assert(r.a && !r.b);
    azn_stems_control(0,1);
    azn_stems_decoded(0,azn_stems_job(0,3,"/c.wav"),"/c.wav",1); /* different load */
    r=(struct azstatus_native_receipt){.size=sizeof r,.version=1};
    assert(api->stems_seed(0,&r) && r.c==AZ_AVAIL_LOADING);
    struct azn_stem_job first=azn_stems_job(0,4,"/c.wav");
    azn_stems_decoded(0,first,"/c.wav",1);
    assert(api->stems_seed(0,&r) && r.a && r.b && r.c==AZ_AVAIL_READY);
    azn_stems_control(0,0);
    assert(api->stems_seed(0,&r) && r.a && !r.b && r.c==AZ_AVAIL_READY);
    api->bind_stems(0,4,0,NULL);
    assert(!api->stems_seed(0,&r));
    api->bind_stems(0,5,4,"/c.wav");
    r=take_stems(api,5); assert(r.a && !r.b);
    azn_stems_control(0,1);
    azn_stems_decoded(0,first,"/c.wav",1);
    assert(api->stems_seed(0,&r) && r.c==AZ_AVAIL_LOADING); /* same load and name, retired source */
    api->bind_stems(0,6,4,NULL);
    struct azn_stem_job pending=azn_stems_job(0,4,"/c.wav");
    azn_stems_decoded(0,pending,"/c.wav",1);
    api->bind_stems(0,7,4,"/c.wav");
    r=take_stems(api,7); assert(r.a && r.b && r.c==AZ_AVAIL_READY);
    api->bind_stems(0,8,4,NULL); pending=azn_stems_job(0,4,"/c.wav"); ++source_pin;
    azn_stems_decoded(0,pending,"/c.wav",1);
    api->bind_stems(0,9,4,"/c.wav");
    r=take_stems(api,9); assert(r.a && !r.b && r.c==AZ_AVAIL_LOADING);
    puts("PASS qualified stems, delayed discovery, empty lookup, engine enable/disable and invalidation");
}
