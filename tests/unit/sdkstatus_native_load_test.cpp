/* SPDX-License-Identifier: GPL-3.0-only */
/* The same checked GNU virtual entries as the legacy lifecycle fixture, with an
 * independently registered passive source witness. */
#define main legacy_load_fixture_main
#include "sdkloadobserver_test.cpp"
#undef main
#include "azstatus_native.h"
#include "azmod-mod.h"
#include <limits>
extern "C" const azstatus_native_v1 *azstem_status_service_v1();
extern "C" int azn_install_terminal(int,azn_setting_patch_fn) {return 0;}
extern "C" int azn_install_settings(int,azn_setting_patch_fn) {return 0;}
extern "C" int azn_metadata_seed(unsigned,azstatus_native_receipt *) {return 0;}
extern "C" void azn_test_reserved(unsigned,unsigned) {}
static unsigned captures;
static bool qualify_file = true;
alignas(8) static unsigned char owner[16], other[16];
alignas(8) static unsigned char identity[0x38]={1,0,0,0,2,1,1,0,36,0,0,0};
static uint64_t capture(unsigned deck,uint64_t unit,uint64_t epoch,uintptr_t where,const unsigned char track_id[12]) {
    CHECK(deck==0 && unit && epoch && where==reinterpret_cast<uintptr_t>(owner) && !memcmp(track_id,identity,12));
    ++captures;
    return qualify_file ? UINT64_C(10000) + captures : 0;
}
static uint64_t foreign_capture(unsigned,uint64_t,uint64_t,uintptr_t,const unsigned char *) {return UINT64_MAX;}
int main(int argc,char **argv) {
    CHECK(argc==2);
    int build=atoi(argv[1]);
    Fixture f;
    native_profile(build);
    azsdk_load_configure(1,build,installer);
    CHECK(azsdk_load_register(0,&f.unit));
    auto api=azstem_status_service_v1();
    CHECK(api->register_source_capture(capture) && api->register_source_capture(capture));
    CHECK(!api->register_source_capture(foreign_capture));
    uintptr_t where=reinterpret_cast<uintptr_t>(owner);
    f.facade.track_expected=identity;
    f.facade.where_expected=&where;
    f.controller.where_expected=&where;
    CHECK(api->capabilities(0)&AZN_CAP_ORIGINAL_BPM);
    for (unsigned n=1;n<=6;n++) {
        TrackLoadOption request(n);
        where=reinterpret_cast<uintptr_t>(owner);
        double bpm=n==3 ? std::numeric_limits<double>::max() : n==4 ? std::numeric_limits<double>::quiet_NaN() : 128.25+n;
        memcpy(identity+0x30,&bpm,sizeof bpm);
        load(&f.facade,request,identity,&where,&f.speed,&f.controller);
        CHECK(captures==n);
        const uint64_t pending_epoch=azn_lifecycle_fence(0);
        CHECK(azn_load_pending(0,pending_epoch,nullptr)==n+1);
        CHECK(azn_load_pending(0,pending_epoch,identity)==n+1);
        unsigned char pending_wrong[12];memcpy(pending_wrong,identity,12);pending_wrong[8]^=1;
        CHECK(!azn_load_pending(0,pending_epoch,pending_wrong));
        CHECK(!azn_load_pending(0,pending_epoch+1,identity));
        azsdk_load_test_hold(0,1);
        CHECK(azn_load_pending(0,pending_epoch,identity)==n+1); // never competes with completion's writer gate
        azsdk_load_test_hold(0,0);
        /* Neither caller mutation nor a completion for an old request may
         * provide the value for this new matched load. */
        double replaced=200;memcpy(identity+0x30,&replaced,sizeof replaced);
        azstatus_native_receipt receipt {};
        receipt.size=sizeof receipt; receipt.version=1;
        CHECK(!api->load_seed(0,&receipt));
        if (n>1) emit(&f.controller,n-1,0,&where);
        if (n==2) where=reinterpret_cast<uintptr_t>(other);
        emit(&f.controller,n,n==5 ? 1:0,&where);
        CHECK(!azn_load_pending(0,pending_epoch,nullptr));
        CHECK(!azn_load_pending(0,pending_epoch,identity));
        CHECK(api->take(0,&receipt) && receipt.a==AZN_LOADING && !receipt.source_pin && receipt.attempt==n);
        if (n==5) {
            CHECK(api->take(0,&receipt) && receipt.a==AZN_FAILED && !receipt.original_bpm_state);
            CHECK(!api->load_seed(0,&receipt));
            continue;
        }
        CHECK(api->take(0,&receipt) && receipt.a==AZN_LOADED && receipt.load_generation);
        unsigned state=n==2 || n==4 ? AZN_BPM_UNAVAILABLE : n==3 ? AZN_BPM_EMPTY : AZN_BPM_READY;
        CHECK(receipt.original_bpm_state==state);
        if (state==AZN_BPM_READY) CHECK(receipt.original_bpm==bpm);
        double untouched=-123;
        CHECK(azn_load_original_bpm(0,receipt.unit+1,receipt.epoch,receipt.load_generation,&untouched)==AZN_BPM_UNKNOWN && untouched==-123);
        CHECK(azn_load_track_matches(0,receipt.epoch,receipt.load_generation,identity));
        unsigned char wrong[12];memcpy(wrong,identity,12);wrong[8]^=1;
        CHECK(!azn_load_track_matches(0,receipt.epoch,receipt.load_generation,wrong));
        CHECK(receipt.source_pin==(n==2 ? UINT64_C(0):UINT64_C(10000)+n));
        CHECK(receipt.source_kind==(n==2 ? 0u:unsigned(AZ_STATUS_SOURCE_USB1)));
        uint64_t pin=receipt.source_pin;
        CHECK(api->load_seed(0,&receipt) && receipt.source_pin==pin &&
              receipt.source_kind==(n==2 ? 0u:unsigned(AZ_STATUS_SOURCE_USB1)));
        CHECK(receipt.original_bpm_state==state);
        if (state==AZN_BPM_READY) CHECK(receipt.original_bpm==bpm);
        emit(&f.controller,n,0,&where);
        CHECK(!api->take(0,&receipt));
    }
    /* Native source admission survives the absence of a file provider. These
     * routes still need an exact borrowed TrackID and matching completion. */
    const struct { unsigned char member, storage; unsigned source; } routes[] = {
        {1, 2u | (1u << 5), AZ_STATUS_SOURCE_USB2},
        {1, 4, AZ_STATUS_SOURCE_CLOUD},
        {1, 6, AZ_STATUS_SOURCE_STREAMING},
        {1, 11, build == 2 ? unsigned(AZ_STATUS_SOURCE_STREAMING) : 0},
        {2, 2, AZ_STATUS_SOURCE_LINKED},
        {1, 31, 0},
    };
    unsigned next = 7;
    qualify_file = false;
    for (const auto &route : routes) {
        identity[0] = route.member;
        identity[4] = route.storage;
        TrackLoadOption request(next);
        load(&f.facade, request, identity, &where, &f.speed, &f.controller);
        emit(&f.controller, next, 0, &where);
        azstatus_native_receipt receipt {};
        receipt.size = sizeof receipt;
        receipt.version = 1;
        CHECK(api->take(0, &receipt) && receipt.a == AZN_LOADING && receipt.attempt == next);
        CHECK(api->take(0, &receipt) && receipt.a == AZN_LOADED && receipt.load_generation &&
              receipt.source_kind == route.source && !receipt.source_pin);
        CHECK(azn_load_track_matches(0, receipt.epoch, receipt.load_generation, identity));
        CHECK(api->load_seed(0, &receipt) && receipt.source_kind == route.source && !receipt.source_pin);
        CHECK(!api->take(0, &receipt));
        ++next;
    }
    f.eject(next);
    azstatus_native_receipt seed{}; seed.size=sizeof seed; seed.version=1;
    CHECK(!api->load_seed(0,&seed));
    CHECK(api->take(0,&seed) && seed.a==AZN_UNKNOWN);
    uint64_t empty=azn_empty_fence(0);CHECK(empty);
    azn_empty_commit(0,empty);azn_empty_commit(0,empty);
    CHECK(api->take(0,&seed) && seed.a==AZN_EMPTY && !api->take(0,&seed));
    CHECK(api->load_seed(0,&seed) && seed.a==AZN_EMPTY && !seed.load_generation && !seed.original_bpm_state);
    azsdk_load_retire(0);
    CHECK(!(api->capabilities(0)&AZN_CAP_ORIGINAL_BPM));
    printf("PASS native source-pin and original BPM lifecycle, copied arguments, stale/failure/missing values, profile %d\n",build);
}
