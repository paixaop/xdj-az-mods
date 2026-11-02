/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSLIPOBSERVER_H
#define AZSLIPOBSERVER_H
#include "azcacheentry.h"
#define AZSLIP_TARGET_COUNT 4
struct azsdk_slip_native_profile {
    uintptr_t targets[AZSLIP_TARGET_COUNT];
    unsigned char expected[AZSLIP_TARGET_COUNT][16];
    size_t code_bytes[AZSLIP_TARGET_COUNT];
    char code_sha256[AZSLIP_TARGET_COUNT][65];
    uintptr_t owner_scope;
    size_t owner_scope_bytes;
    char owner_scope_sha256[65];
    uintptr_t construct_return,parent_returns[4];
    uintptr_t source_control_vtable,parent_control_vtable;
    uintptr_t registration;
    size_t registration_bytes;
    char registration_sha256[65];
    uintptr_t producer,getter_sites[3];
    size_t producer_bytes;
    char producer_sha256[65];
};
typedef int (*azsdk_slip_installer)(uintptr_t,void *,void **,void *);
#ifdef __cplusplus
extern "C" {
#endif
int azsdk_slip_install(const struct azsdk_slip_native_profile *,azsdk_slip_installer,void *);
int azsdk_slip_ready(void);
int azsdk_slip_getters_install(const struct azsdk_slip_native_profile *,azsdk_slip_installer,void *);
extern uint32_t azsdk_slip_getter_hooks_v1;
/* Internal read-only debug ABI, not public Slip support. Collected on the GUI
 * tick, with no pointers/calls needed by a bound emulator reader. raw is only
 * usable when valid=1; registered alone supplies no initial value/semantics. */
struct azsdk_slip_debug_v1 {
    uint64_t sequence,generation,revision,observed_ns,callbacks;
    uint32_t registered,valid,raw,reserved;
};
extern struct azsdk_slip_debug_v1 azsdk_slip_observation_v1[4];
/* Diagnostic code identities observed at the three genuine native call sites.
 * No pointees are read by the recorder. Bits0/1/2 identify slot4/13/14. */
struct azsdk_slip_getter_debug_v1 {
    uint64_t sequence,generation,observed_ns,target_slot4,target_slot13,target_slot14;
    uint32_t seen_mask,conflict_mask;
};
extern struct azsdk_slip_getter_debug_v1 azsdk_slip_getter_observation_v1[4];
void azsdk_slip_capture_getter(unsigned site,uintptr_t source,uintptr_t target);
void azsdk_slip_collect_v1(void);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
#include <cstdint>
#include <memory>
namespace azsdk_slip {
enum Kind { Construct, PublishParent, DisposeSource, DisposeParent };
class Listener {
public:
    virtual ~Listener() {}
    virtual void changed(unsigned raw) = 0;
};
using Register = void (*)(void *,const std::weak_ptr<Listener> &);
struct Profile {
    std::uintptr_t construct_return, parent_returns[4];
    std::uintptr_t source_control_vtable, parent_control_vtable;
    Register registration;
};
struct Receipt {
    std::uint64_t generation, revision, observed_ns;
    unsigned raw, reserved;
};
using Original = void (*)(azce_call *);
/* Internal raw notification only. configure is startup-only after exact code
 * binding. Public enabled semantics and support remain independently gated. */
bool configure(const Profile &) noexcept;
void forward(azce_call *, Original);
bool read(unsigned deck, Receipt *) noexcept;
}
#endif
#endif
