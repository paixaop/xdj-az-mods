/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual SourceState process storage and genuine GNU weak-control lifetime.
 * The late callback reads a separate allocator flag before touching state, so
 * the old-state runtime RED never accesses an already destroyed mutex/object. */
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#define REQUIRE(x,why) do{if(!(x)){std::fprintf(stderr,"FAIL source-storage: %s\n",why);std::abort();}}while(0)
namespace music_library { class MountPoint { public: virtual ~MountPoint(){} }; }
#ifndef SOURCES_INC
#define SOURCES_INC "../../src/azmod/sdk_native_sources.cpp"
#endif
#include SOURCES_INC
namespace {
bool control_freed;
unsigned build;
template<class T>struct NativeControlAllocator {
    using value_type=T;
    NativeControlAllocator()noexcept{}
    template<class U>NativeControlAllocator(const NativeControlAllocator<U>&)noexcept{}
    T*allocate(std::size_t n){return static_cast<T*>(::operator new(n*sizeof(T)));}
    void deallocate(T*p,std::size_t)noexcept{control_freed=true;::operator delete(p);}
    template<class U>struct rebind{using other=NativeControlAllocator<U>;};
};
template<class T,class U>bool operator==(const NativeControlAllocator<T>&,const NativeControlAllocator<U>&){return true;}
template<class T,class U>bool operator!=(const NativeControlAllocator<T>&,const NativeControlAllocator<U>&){return false;}
void late_native_shutdown(){
    REQUIRE(!control_freed,"SourceState destroyed before later native shutdown hook");
    auto &state=source_state();std::lock_guard<std::mutex>lock(state.registry_mutex);
    REQUIRE(state.entries[0].mount.expired(),"late hook sees actual expired native owner without a revived allocation");
    state.next_epoch=1;
    std::printf("PASS source-storage build=%u: same state and mutex survive native shutdown interval\n",build);
}
}
int main(int argc,char**argv){
    REQUIRE(argc==2&&(argv[1][0]=='1'||argv[1][0]=='2'),"independent profile process");build=unsigned(argv[1][0]-'0');
    // Registration BEFORE SourceState construction makes this callback run
    // AFTER an ordinary function-local object's registered C++ destructor.
    REQUIRE(std::atexit(late_native_shutdown)==0,"late native callback registration");
    auto owner=std::allocate_shared<music_library::MountPoint>(NativeControlAllocator<music_library::MountPoint>{});
    source_state().entries[0].mount=owner;owner.reset();
    REQUIRE(!control_freed&&source_state().entries[0].mount.expired(),"actual native strong lifetime ended and genuine weak control remains");
    std::printf("NATIVE_STATE_STORAGE_OK build=%u: real GNU weak control retained before shutdown\n",build);std::fflush(stdout);
}
